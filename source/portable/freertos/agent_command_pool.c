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

/*-----------------------------------------------------------*/

#define SEMAPHORES_NOT_INITIALIZED    ( 0U )
#define SEMAPHORES_INIT_PENDING       ( 1U )
#define SEMAPHORES_INITIALIZED        ( 2U )

/**
 * @brief The pool of command structures used to hold information on commands (such
 * as PUBLISH or SUBSCRIBE) between the command being created by an API call and
 * completion of the command by the execution of the command's callback.
 */
static Command_t commandStructurePool[ MQTT_COMMAND_CONTEXTS_POOL_SIZE ];

/**
 * @brief An array of semaphores to guard each Command_t structure in the pool.
 * Structures must be obtained by first obtaining its associated semaphore.
 */
static SemaphoreHandle_t commandSems[ MQTT_COMMAND_CONTEXTS_POOL_SIZE ];

/**
 * @brief Initialization status of the command semaphore array.
 */
static volatile uint8_t initStatus = SEMAPHORES_NOT_INITIALIZED;

/*-----------------------------------------------------------*/

/**
 * @brief Initialize the pool of command structures.
 */
static void initializePool( void );

/**
 * @brief Iterate through the array of command structures until one is
 * available or the end is reached.
 *
 * @return Pointer to available command structure, or NULL.
 */
static Command_t * getCommand( void );

/*-----------------------------------------------------------*/

static void initializePool( void )
{
    bool owner = false;
    size_t i;
    static StaticSemaphore_t commandSemsStorage[ MQTT_COMMAND_CONTEXTS_POOL_SIZE ];
    BaseType_t semaphoreCreated = pdFALSE;

    taskENTER_CRITICAL();
    {
        if( initStatus == SEMAPHORES_NOT_INITIALIZED )
        {
            owner = true;
            initStatus = SEMAPHORES_INIT_PENDING;
        }
    }
    taskEXIT_CRITICAL();

    if( owner )
    {
        memset( ( void * ) commandStructurePool, 0x00, sizeof( commandStructurePool ) );

        /* Create a binary semaphore for each command. */
        for( i = 0; i < MQTT_COMMAND_CONTEXTS_POOL_SIZE; i++ )
        {
            commandSems[ i ] = xSemaphoreCreateBinaryStatic( &commandSemsStorage[ i ] );
            semaphoreCreated = xSemaphoreGive( commandSems[ i ] );
            configASSERT( semaphoreCreated == pdTRUE );
        }

        initStatus = SEMAPHORES_INITIALIZED;
    }
}

/*-----------------------------------------------------------*/

static Command_t * getCommand( void )
{
    Command_t * structToUse = NULL;
    size_t i;

    for( i = 0; i < MQTT_COMMAND_CONTEXTS_POOL_SIZE; i++ )
    {
        if( xSemaphoreTake( commandSems[ i ], 0 ) == pdTRUE )
        {
            break;
        }
    }

    if( i < MQTT_COMMAND_CONTEXTS_POOL_SIZE )
    {
        structToUse = &( commandStructurePool[ i ] );
    }

    return structToUse;
}

/*-----------------------------------------------------------*/

Command_t * Agent_GetCommand( uint32_t blockTimeMs )
{
    Command_t * structToUse = NULL;
    uint32_t cumulativeDelayMs = 0U;

    /* Check here so we do not enter a critical section every time. */
    if( initStatus == SEMAPHORES_NOT_INITIALIZED )
    {
        initializePool();
    }

    /* Check semaphores have been created. */
    if( initStatus == SEMAPHORES_INITIALIZED )
    {
        do
        {
            structToUse = getCommand();

            if( structToUse == NULL )
            {
                vTaskDelay( pdMS_TO_TICKS( 1 ) );
                cumulativeDelayMs++;
            }
        } while( ( structToUse == NULL ) && ( cumulativeDelayMs < blockTimeMs ) );
    }

    if( structToUse == NULL )
    {
        LogError( ( "No command structure available." ) );
    }

    return structToUse;
}

/*-----------------------------------------------------------*/

bool Agent_ReleaseCommand( Command_t * pCommandToRelease )
{
    size_t commandIndex;
    bool structReturned = false;

    /* Calculate the index of the struct. */
    commandIndex = pCommandToRelease - commandStructurePool;

    /* Ensure the structure being returned is actually from the pool. */
    if( commandIndex < MQTT_COMMAND_CONTEXTS_POOL_SIZE )
    {
        structReturned = ( bool ) xSemaphoreGive( commandSems[ commandIndex ] );
    }

    if( structReturned )
    {
        LogDebug( ( "Returned Command Context %d to pool",
                    ( int ) commandIndex ) );
    }

    return structReturned;
}
