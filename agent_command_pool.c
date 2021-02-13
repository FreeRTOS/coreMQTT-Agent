/*
 * FreeRTOS V202011.00
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * https://www.FreeRTOS.org
 * https://aws.amazon.com/freertos
 *
 */

/**
 * @file agent_command_pool.c
 * @brief Implements functions to obtain and release commands.
 */

/* Standard includes. */
#include <string.h>
#include <stdio.h>

/* Kernel includes. */
#include "FreeRTOS.h"
#include "semphr.h"

/* Header include. */
#include "agent_command_pool.h"
#include "agent_message.h"

/*-----------------------------------------------------------*/

struct AgentMessageContext
{
    QueueHandle_t queue;
};

/*-----------------------------------------------------------*/

#define QUEUE_NOT_INITIALIZED    ( 0U )
#define QUEUE_INIT_PENDING       ( 1U )
#define QUEUE_INITIALIZED        ( 2U )

/**
 * @brief The pool of command structures used to hold information on commands (such
 * as PUBLISH or SUBSCRIBE) between the command being created by an API call and
 * completion of the command by the execution of the command's callback.
 */
static Command_t commandStructurePool[ MQTT_COMMAND_CONTEXTS_POOL_SIZE ];

/**
 * @brief A queue used to guard the pool of Command_t structures. Structures
 * may be obtained by receiving a pointer from the queue, and returned by
 * sending the pointer back into it.
 */
static AgentMessageContext_t commandStructQueue;

/**
 * @brief Initialization status of the queue.
 */
static volatile uint8_t initStatus = QUEUE_NOT_INITIALIZED;

/*-----------------------------------------------------------*/

static void initializePool()
{
    bool owner = false;
    size_t i;
    Command_t * pCommand;
    static uint8_t staticQueueStorageArea[ MQTT_COMMAND_CONTEXTS_POOL_SIZE * sizeof( Command_t * ) ];
    static StaticQueue_t staticQueueStructure;

    taskENTER_CRITICAL();
    {
        if( initStatus == QUEUE_NOT_INITIALIZED )
        {
            owner = true;
            initStatus = QUEUE_INIT_PENDING;
        }
    }
    taskEXIT_CRITICAL();

    if( owner )
    {
        memset( ( void * ) commandStructurePool, 0x00, sizeof( commandStructurePool ) );
        commandStructQueue.queue = xQueueCreateStatic( MQTT_COMMAND_CONTEXTS_POOL_SIZE,
                                                       sizeof( Command_t * ),
                                                       staticQueueStorageArea,
                                                       &staticQueueStructure );
        configASSERT( commandStructQueue.queue );

        /* Populate the queue. */
        for( i = 0; i < MQTT_COMMAND_CONTEXTS_POOL_SIZE; i++ )
        {
            /* Store the address as a variable. */
            pCommand = &commandStructurePool[ i ];
            /* Send the pointer to the queue. */
            Agent_MessageSend( &commandStructQueue, &pCommand, 0U );
        }

        initStatus = QUEUE_INITIALIZED;
    }
}

/*-----------------------------------------------------------*/

Command_t * Agent_GetCommand( uint32_t blockTimeMs )
{
    Command_t * structToUse = NULL;
    size_t i;
    bool structRetrieved = false;

    /* Check here so we do not enter a critical section every time. */
    if( initStatus == QUEUE_NOT_INITIALIZED )
    {
        initializePool();
    }

    /* Check queue has been created. */
    if( initStatus == QUEUE_INITIALIZED )
    {
        /* Retrieve a struct from the queue. */
        structRetrieved = Agent_MessageReceive( &commandStructQueue, &( structToUse ), blockTimeMs );

        if( !structRetrieved )
        {
            LogError( ( "No command structure available." ) );
        }
    }

    return structToUse;
}

/*-----------------------------------------------------------*/

bool Agent_ReleaseCommand( Command_t * pCommandToRelease )
{
    size_t i;
    bool structReturned = false;

    /* See if the structure being returned is actually from the pool. */
    if( ( pCommandToRelease >= commandStructurePool ) &&
        ( pCommandToRelease < ( commandStructurePool + MQTT_COMMAND_CONTEXTS_POOL_SIZE ) ) )
    {
        structReturned = Agent_MessageSend( &commandStructQueue, &pCommandToRelease, 0U );
    }

    if( structReturned )
    {
        LogDebug( ( "Returned Command Context %d to pool",
                    ( int ) ( pCommandToRelease - commandStructurePool ) ) );
    }

    return structReturned;
}
