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
 * @file mqtt_agent.c
 * @brief Implements an MQTT agent (or daemon task) to enable multithreaded access to 
 * coreMQTT.
 * 
 * @note Implements an MQTT agent (or daemon task) on top of the coreMQTT MQTT client
 * library.  The agent makes coreMQTT usage thread safe by being the only task (or
 * thread) in the system that is allowed to access the native coreMQTT API - and in
 * so doing, serialises all access to coreMQTT even when multiple tasks are using the
 * same MQTT connection.
 * 
 * The agent provides an equivalent API for each coreMQTT API.  Whereas coreMQTT
 * APIs are prefixed "MQTT_", the agent APIs are prefixed "MQTTAgent_".  For example,
 * that agent's MQTTAgent_Publish() API is the thread safe equivalent to coreMQTT's
 * MQTT_Publish() API.
 * 
 * See https://_RB_ for examples and usage information.
 */

/* Standard includes. */
#include <string.h>
#include <stdio.h>

/* Kernel includes. */
#include "FreeRTOS.h"
#include "semphr.h"

/* MQTT agent include. */
#include "freertos_mqtt_agent.h"

/*-----------------------------------------------------------*/

/**
 * @brief A type of command for interacting with the MQTT API.
 */
typedef enum CommandType
{
    NONE = 0,    /**< @brief No command received.  Must be zero (its memset() value). */
    PROCESSLOOP, /**< @brief Call MQTT_ProcessLoop(). */
    PUBLISH,     /**< @brief Call MQTT_Publish(). */
    SUBSCRIBE,   /**< @brief Call MQTT_Subscribe(). */
    UNSUBSCRIBE, /**< @brief Call MQTT_Unsubscribe(). */
    PING,        /**< @brief Call MQTT_Ping(). */
    DISCONNECT,  /**< @brief Call MQTT_Disconnect(). */
    FREE,        /**< @brief Remove a mapping from an MQTT Context to the agent. */
    TERMINATE    /**< @brief Exit the command loop and stop processing commands. */
} CommandType_t;

/**
 * Commands sent to the MQTT agent include subscribe commands and publish commands,
 * but never both at the same time.  Therefore the structures that describe a publish
 * or subscribe operation can be in a union in order to save memory. */
typedef union MqttOperation
{
    MQTTPublishInfo_t publishInfo;
    MQTTSubscribeInfo_t subscribeInfo;
} MqttOperationInfo_t;

/**
 * @brief The commands sent from the publish API to the MQTT agent.
 * 
 * @note The structure used to pass information from the public facing API into the 
 * agent task. */
typedef struct Command
{
    CommandType_t commandType;
    CommandContext_t * pxCmdContext;
    CommandCallback_t pCommandCompleteCallback;
    MQTTContext_t * pMqttContext;
    PublishCallback_t pIncomingPublishCallback;
    void * pIncomingPublishCallbackContext;
    MqttOperationInfo_t mqttOperationInfo;
} Command_t;

/**
 * @brief Information for a pending MQTT ack packet expected by the demo.
 */
typedef struct ackInfo
{
    uint16_t packetId;
    Command_t *pOriginalCommand;
} AckInfo_t;

/**
 * @brief An element in the list of subscriptions maintained in the demo.
 *
 * @note This demo allows multiple tasks to subscribe to the same topic.
 * In this case, another element is added to the subscription list, differing
 * in the destination response queue.
 */
typedef struct subscriptionElement
{
    PublishCallback_t pIncomingPublishCallback;
    void * pIncomingPublishCallbackContext;
    uint16_t filterStringLength;
    char pSubscriptionFilterString[ MQTT_AGENT_SUBSCRIPTION_BUFFER_SIZE ];
} SubscriptionElement_t;

/**
 * @brief Associated information for a single MQTT connection.
 */
typedef struct MQTTAgentContext
{
    MQTTContext_t * pMQTTContext;
    AckInfo_t pPendingAcks[ PENDING_ACKS_MAX_SIZE ];
    SubscriptionElement_t pSubscriptionList[ SUBSCRIPTIONS_MAX_COUNT ];
    MQTTSubscribeInfo_t pResendSubscriptions[ SUBSCRIPTIONS_MAX_COUNT ];
    PublishCallback_t pUnsolicitedPublishCallback;
    void * pUnsolicitedPublishCallbackContext;
} MQTTAgentContext_t;

/*-----------------------------------------------------------*/

/**
 * @brief Track an operation by adding it to a list, indicating it is anticipating
 * an acknowledgment.
 *
 * @param[in] pAgentContext Agent context for the MQTT connection.
 * @param[in] packetId Packet ID of pending ack.
 * @param[in] pCommand Pointer to command that is expecting an ack.
 *
 * @return `true` if the operation was added; else `false`
 */
static bool addAwaitingOperation( MQTTAgentContext_t * pAgentContext,
                                  uint16_t packetId,
                                  Command_t * pCommand );

/**
 * @brief Retrieve an operation from the list of pending acks, and optionally
 * remove it from the list.
 *
 * @param[in] pAgentContext Agent context for the MQTT connection.
 * @param[in] packetId Packet ID of incoming ack.
 * @param[in] remove Flag indicating if the operation should be removed from the list.
 *
 * @return Stored information about the operation awaiting the ack.
 */
static AckInfo_t getAwaitingOperation( MQTTAgentContext_t * pAgentContext,
                                       uint16_t incomingPacketId,
                                       bool remove );

/**
 * @brief Add a subscription to the subscription list.
 *
 * @note Multiple tasks can be subscribed to the same topic with different
 * context-callback pairs. However, a single context-callback pair may only be
 * associated to the same topic filter once.
 *
 * @param[in] pAgentContext Agent context for the MQTT connection.
 * @param[in] topicFilterString Topic filter string of subscription.
 * @param[in] topicFilterLength Length of topic filter string.
 * @param[in] pIncomingPublishCallback Callback function for the subscription.
 * @param[in] pIncomingPublishCallbackContext Context for the subscription callback.
 *
 * @return `true` if subscription added or exists, `false` if insufficient memory.
 */
static bool addSubscription( MQTTAgentContext_t * pAgentContext,
                             const char * topicFilterString,
                             uint16_t topicFilterLength,
                             PublishCallback_t pIncomingPublishCallback,
                             void * pIncomingPublishCallbackContext );

/**
 * @brief Remove a subscription from the subscription list.
 *
 * @note If the topic filter exists multiple times in the subscription list,
 * then every instance of the subscription will be removed.
 *
 * @param[in] pAgentContext Agent context for the MQTT connection.
 * @param[in] topicFilterString Topic filter of subscription.
 * @param[in] topicFilterLength Length of topic filter.
 */
static void removeSubscription( MQTTAgentContext_t * pAgentContext,
                                const char * topicFilterString,
                                uint16_t topicFilterLength );

/**
 * @brief Populate the parameters of a #Command_t
 *
 * @param[in] commandType Type of command.  For example, publish or subscribe.
 * @param[in] pMqttContext Pointer to MQTT context to use for command.
 * @param[in] pMqttInfoParam Pointer to MQTTPublishInfo_t or MQTTSubscribeInfo_t.
 * @param[in] incomingPublishCallback Subscription callback function for incoming publishes.
 * @param[in] pIncomingPublishCallbackContext Subscription callback context.
 * @param[in] commandCompleteCallback Callback for when command completes.
 * @param[in] pCommandCompleteCallbackContext Context and necessary structs for command.
 * @param[out] pCommand Pointer to initialized command.
 *
 * @return `true` if all necessary structs for the command exist in pxContext,
 * else `false`
 */
static bool createCommand( CommandType_t commandType,
                           MQTTContext_t * pMqttContext,
                           void * pMqttInfoParam,
                           PublishCallback_t incomingPublishCallback,
                           void * pIncomingPublishCallbackContext,
                           CommandCallback_t commandCompleteCallback,
                           CommandContext_t * pCommandCompleteCallbackContext,
                           Command_t * pCommand );

/**
 * @brief Add a command to the global command queue.
 *
 * @param[in] pCommand Pointer to command to copy to queue.
 *
 * @return true if the command was added to the queue, else false.
 */
static bool addCommandToQueue( Command_t * pCommand );

/**
 * @brief Process a #Command_t.
 *
 * @note This agent does not check existing subscriptions before sending a
 * SUBSCRIBE or UNSUBSCRIBE packet. If a subscription already exists, then
 * a SUBSCRIBE packet will be sent anyway, and if multiple tasks are subscribed
 * to a topic filter, then they will all be unsubscribed after an UNSUBSCRIBE.
 *
 * @param[in] pCommand Pointer to command to process.
 *
 * @return status of MQTT library API call.
 */
static MQTTStatus_t processCommand( Command_t * pCommand );

/**
 * @brief Dispatch an incoming publish to the appropriate publish callback.
 *
 * @param[in] pAgentContext Agent context for the MQTT connection.
 * @param[in] pPublishInfo Incoming publish information.
 */
static void handleIncomingPublish( MQTTAgentContext_t * pAgentContext,
                                   MQTTPublishInfo_t * pPublishInfo );

/**
 * @brief Dispatch incoming publishes and acks to their various handler functions.
 *
 * @param[in] pMqttContext MQTT Context
 * @param[in] pPacketInfo Pointer to incoming packet.
 * @param[in] pDeserializedInfo Pointer to deserialized information from
 * the incoming packet.
 */
static void mqttEventCallback( MQTTContext_t * pMqttContext,
                               MQTTPacketInfo_t * pPacketInfo,
                               MQTTDeserializedInfo_t * pDeserializedInfo );

/**
 * @brief Add or delete subscription information from a SUBACK or UNSUBACK.
 *
 * @param[in] pAgentContext Agent context for the MQTT connection.
 * @param[in] pPacketInfo Pointer to incoming packet.
 * @param[in] pDeserializedInfo Pointer to deserialized information from
 * the incoming packet.
 * @param[in] pAckInfo Pointer to stored information for the original subscribe
 * or unsubscribe operation resulting in the received packet.
 * @param[in] packetType The type of the incoming packet, either SUBACK or UNSUBACK.
 */
static void handleSubscriptionAcks( MQTTAgentContext_t * pAgentContext,
                                    MQTTPacketInfo_t * pPacketInfo,
                                    MQTTDeserializedInfo_t * pDeserializedInfo,
                                    AckInfo_t * pAckInfo,
                                    uint8_t packetType );

/**
 * @brief Retrieve an MQTT context for an empty command's process loop.
 *
 * @note Successive calls to this function will loop through the contexts stored
 * from MQTTAgent_Register(), ensuring that connections will not remain idle too
 * long when the queue is empty.
 *
 * @return Pointer to MQTT context, or NULL.
 */
static MQTTContext_t * getContextForProcessLoop( void );

/**
 * @brief Retrieve a pointer to an agent context given an MQTT context.
 *
 * @param[in] pMQTTContext MQTT Context to search for.
 *
 * @return Pointer to agent context, or NULL.
 */
static MQTTAgentContext_t * getAgentFromContext( MQTTContext_t * pMQTTContext );

/**
 * @brief Helper function for creating a command and adding it to the command
 * queue.
 *
 * @param[in] commandType Type of command.
 * @param[in] mqttContextHandle Handle of the MQTT connection to use.
 * @param[in] pCommandCompleteCallbackContext Context and necessary structs for command.
 * @param[in] cmdCompleteCallback Callback for when command completes.
 * @param[in] pMqttInfoParam Pointer to MQTTPublishInfo_t or MQTTSubscribeInfo_t.
 * @param[in] incomingPublishCallback Subscription callback function for incoming 
 *            publishes.
 * @param[in] pIncomingPublishCallbackContext Subscription callback context.
 *
 * @return `true` if the command was added to the queue, `false` if not.
 */
static bool createAndAddCommand( CommandType_t commandType,
                                 MQTTContextHandle_t mqttContextHandle,
                                 void * pMqttInfoParam,
                                 CommandCallback_t cmdCompleteCallback,
                                 CommandContext_t * pCommandCompleteCallbackContext,
                                 PublishCallback_t incomingPublishCallback,
                                 void * pIncomingPublishCallbackContext );


/**
 * @brief Obtain a Command_t structure from the pool of structures managed by the agent.
 * 
 * @note Command_t structures hold everything the MQTT agent needs to process a 
 * command that originates from application.  Examples of commands are PUBLISH and
 * SUBSCRIBE.  The Command_t structure must persist for the duration of the command's
 * operation so are obtained from a pool of statically allocated structures when a
 * new command is created, and returned to the pool when the command is complete.
 * The MQTT_COMMAND_CONTEXTS_POOL_SIZE configuration file constant defines how many
 * structures the pool contains.
 *
 * @param[in] blockTimeMs The length of time the calling task should remain in the
 * Blocked state (so not consuming any CPU time) to wait for a Command_t structure to
 * become available should one not be immediately at the time of the call.
 *
 * @return A pointer to a Command_t structure if one because available before 
 * blockTimeMs time expired, otherwise NULL.
 */
static Command_t *getCommandStructureFromPool( TickType_t blockTimeMs );

/**
 * @brief Give a Command_t structure back to the the pool of structures managed by 
 * the agent.
 * 
 * @note Command_t structures hold everything the MQTT agent needs to process a 
 * command that originates from application.  Examples of commands are PUBLISH and
 * SUBSCRIBE.  The Command_t structure must persist for the duration of the command's
 * operation so are obtained from a pool of statically allocated structures when a
 * new command is created, and returned to the pool when the command is complete.
 * The MQTT_COMMAND_CONTEXTS_POOL_SIZE configuration file constant defines how many
 * structures the pool contains.
 *
 * @param[in] pxCommandToRelease A pointer to the Command_t structure to return to
 * the pool.  The structure must first have been obtained by calling
 * getCommandStructureFromPool(), otherwise releaseCommandStructureToPool() will
 * have no effect.
 *
 * @return true if the Command_t structure was returned to the pool, otherwise false.
 */
static bool releaseCommandStructureToPool( Command_t *pxCommandToRelease );

/*-----------------------------------------------------------*/

/**
 * @brief Queue used to pass commands from the application to the MQTT agent..
 *
 * This is a private variable initialized when the agent is initialised.
 */
static QueueHandle_t commandQueue = NULL;

/**
 * @brief Arrays of agent and MQTT contexts, one for each potential MQTT connection.
 */
static MQTTAgentContext_t agentContexts[ MAX_CONNECTIONS ] = { 0 }; /*_RB_ Change name of constant. */
static MQTTContext_t mqttContexts[ MAX_CONNECTIONS ] = { 0 };

/**
 * @brief The network buffer must remain valid for the lifetime of the MQTT context.
 */
static uint8_t networkBuffers[ MAX_CONNECTIONS ][ mqttexampleNETWORK_BUFFER_SIZE ];/*_RB_ Need to move and rename constant. Also this requires both buffers to be the same size. */

/**
 * @brief The pool of command structures used to hold information on commands (such
 * as PUBLISH or SUBSCRIBE) between the command being created by an API call and 
 * by either an error or the execution of the commands callback.
 */
static Command_t commandStructurePool[ MQTT_COMMAND_CONTEXTS_POOL_SIZE ];

/**
 * @brief A counting semaphore used to guard the pool of Command_t structures.  To
 * obtain a structure first decrement the semaphore count.  To return a structure
 * increment the semaphore count after the structure is back in the pool.
 */
static SemaphoreHandle_t freeCommandStructMutex = NULL;

/**
 * @brief Flag that is set to true in the application callback to let the agent know
 * that calling MQTT_ProcessLoop() resulted in events on the connected socket.  If
 * the flag gets set to true then MQTT_ProcessLoop() is called again as there may be
 * more received data waiting to be processed.
 */
static bool packetProcessedDuringLoop = false;

/*-----------------------------------------------------------*/

static bool releaseCommandStructureToPool( Command_t *pxCommandToRelease )
{
    size_t i;
    bool structReturned = false;

    /* See if the structure being returned is actually from the pool. */
    for( i = 0; i < MQTT_COMMAND_CONTEXTS_POOL_SIZE; i++ )
    {
        if( pxCommandToRelease == &( commandStructurePool[ i ] ) )
        {
            /* Yes its from the pool.  Clearing it to zero not only removes the old
             * data it also sets the structure's commandType parameter to NONE to
             * mark the structure as free again. */
            memset( ( void * ) pxCommandToRelease, 0x00, sizeof( Command_t ) );

            /* Give back the counting semaphore after returning the structure so the
             * semaphore count equals the number of available structures. */
            xSemaphoreGive( freeCommandStructMutex );
            structReturned = true;

            LogDebug( ( "Returned Command Context %d to pool", ( int ) i ) );

            break;
        }
    }

    return structReturned;
}

/*-----------------------------------------------------------*/

static Command_t *getCommandStructureFromPool( TickType_t blockTimeMs )
{
    Command_t *structToUse = NULL;
    size_t i;

    /* Check counting semaphore has been created. */
    if( freeCommandStructMutex != NULL )
    {
        /* If the semaphore count is not zero then a command context is available. */
        if( xSemaphoreTake( freeCommandStructMutex, pdMS_TO_TICKS( blockTimeMs ) ) == pdPASS )
        {
            for( i = 0; i < MQTT_COMMAND_CONTEXTS_POOL_SIZE; i++ )
            {
                taskENTER_CRITICAL();
                {
                    /* If the commandType is NONE then the structure is not in use. */
                    if( commandStructurePool[ i ].commandType == NONE )
                    {
                        LogDebug( ( "Removed Command Context %d from pool", ( int ) i ) );
                        structToUse = &( commandStructurePool[ i ] );

                        /* To show the struct is no longer available to be returned
                         * by calls to releaseCommandStructureToPool(). */
                        structToUse->commandType = !NONE;
                        taskEXIT_CRITICAL();
                        break;
                    }
                }
                taskEXIT_CRITICAL();
            }
        }
    }

    return structToUse;
}

/*-----------------------------------------------------------*/

static bool addAwaitingOperation( MQTTAgentContext_t * pAgentContext,
                                  uint16_t packetId,
                                  Command_t * pCommand )
{
    size_t i = 0;
    bool ackAdded = false;
    AckInfo_t * pendingAcks = pAgentContext->pPendingAcks;

    /* Look for an unused space in the array of message IDs that are still waiting to
     * be acknowledged. */
    for( i = 0; i < PENDING_ACKS_MAX_SIZE; i++ )
    {
        /* If the packetId is MQTT_PACKET_ID_INVALID then the array space is not in
         * use. */
        if( pendingAcks[ i ].packetId == MQTT_PACKET_ID_INVALID )
        {
            pendingAcks[ i ].packetId = packetId;
            pendingAcks[ i ].pOriginalCommand = pCommand;
            ackAdded = true;
            break;
        }
    }

    return ackAdded;
}

/*-----------------------------------------------------------*/

static AckInfo_t getAwaitingOperation( MQTTAgentContext_t * pAgentContext,
                                       uint16_t incomingPacketId,
                                       bool remove )
{
    size_t i = 0;
    AckInfo_t foundAck = { 0 };
    AckInfo_t * pendingAcks = pAgentContext->pPendingAcks;

    /* Look through the array of packet IDs that are still waiting to be acked to
     * find one with incomginPacketId. */
    for( i = 0; i < PENDING_ACKS_MAX_SIZE; i++ )
    {
        if( pendingAcks[ i ].packetId == incomingPacketId )
        {
            foundAck = pendingAcks[ i ];

            if( remove )
            {
                pendingAcks[ i ].packetId = MQTT_PACKET_ID_INVALID;
            }

            break;
        }
    }

    if( foundAck.packetId == MQTT_PACKET_ID_INVALID )
    {
        LogError( ( "No ack found for packet id %u.\n", incomingPacketId ) );
    }

    return foundAck;
}

/*-----------------------------------------------------------*/

static bool addSubscription( MQTTAgentContext_t * pAgentContext,
                             const char * topicFilterString,
                             uint16_t topicFilterLength,
                             PublishCallback_t pIncomingPublishCallback,
                             void * pIncomingPublishCallbackContext )
{
    int32_t i = 0;
    size_t availableIndex = SUBSCRIPTIONS_MAX_COUNT;
    SubscriptionElement_t * pxSubscriptions = pAgentContext->pSubscriptionList;
    bool ret = false;

    /* The topic filter length was checked when the SUBSCRIBE command was created. */
    configASSERT( topicFilterLength < MQTT_AGENT_SUBSCRIPTION_BUFFER_SIZE );

    if( topicFilterLength < MQTT_AGENT_SUBSCRIPTION_BUFFER_SIZE )
    {
        /* Start at end of array, so that we will insert at the first available index.
         * Scans backwards to find duplicates. */
        for( i = ( int32_t ) SUBSCRIPTIONS_MAX_COUNT - 1; i >= 0; i-- )
        {
            if( pxSubscriptions[ i ].filterStringLength == 0 )
            {
                availableIndex = i;
            }
            else if( ( pxSubscriptions[ i ].filterStringLength == topicFilterLength ) &&
                     ( strncmp( topicFilterString, pxSubscriptions[ i ].pSubscriptionFilterString, topicFilterLength ) == 0 ) )
            {
                /* If a subscription already exists, don't do anything. */
                if( ( pxSubscriptions[ i ].pIncomingPublishCallback == pIncomingPublishCallback ) &&
                    ( pxSubscriptions[ i ].pIncomingPublishCallbackContext == pIncomingPublishCallbackContext ) )
                {
                    LogWarn( ( "Subscription already exists.\n" ) );
                    availableIndex = SUBSCRIPTIONS_MAX_COUNT;
                    ret = true;
                    break;
                }
            }
        }

        if( ( availableIndex < SUBSCRIPTIONS_MAX_COUNT ) && ( pIncomingPublishCallback != NULL ) )
        {
            pxSubscriptions[ availableIndex ].filterStringLength = topicFilterLength;
            pxSubscriptions[ availableIndex ].pIncomingPublishCallback = pIncomingPublishCallback;
            pxSubscriptions[ availableIndex ].pIncomingPublishCallbackContext = pIncomingPublishCallbackContext;
            memcpy( pxSubscriptions[ availableIndex ].pSubscriptionFilterString, topicFilterString, topicFilterLength );
            ret = true;
        }
    }

    return ret;
}

/*-----------------------------------------------------------*/

static void removeSubscription( MQTTAgentContext_t * pAgentContext,
                                const char * topicFilterString,
                                uint16_t topicFilterLength )
{
    size_t i = 0;
    SubscriptionElement_t * pxSubscriptions = pAgentContext->pSubscriptionList;

    for( i = 0; i < SUBSCRIPTIONS_MAX_COUNT; i++ )
    {
        if( pxSubscriptions[ i ].filterStringLength == topicFilterLength )
        {
            if( strncmp( pxSubscriptions[ i ].pSubscriptionFilterString, topicFilterString, topicFilterLength ) == 0 )
            {
                memset( &( pxSubscriptions[ i ] ), 0x00, sizeof( SubscriptionElement_t ) );
            }
        }
    }
}

/*-----------------------------------------------------------*/

static bool createCommand( CommandType_t commandType,
                           MQTTContext_t * pMqttContext,
                           void * pMqttInfoParam,
                           PublishCallback_t incomingPublishCallback,
                           void * pIncomingPublishCallbackContext,
                           CommandCallback_t commandCompleteCallback,
                           CommandContext_t * pCommandCompleteCallbackContext,
                           Command_t * pCommand )
{
    bool isValid;
    MQTTSubscribeInfo_t *pSubscribeInfo;

    memset( pCommand, 0x00, sizeof( Command_t ) );

    /* Determine if required parameters are present in context. */
    switch( commandType )
    {
        case SUBSCRIBE:
            pSubscribeInfo = ( MQTTSubscribeInfo_t * ) pMqttInfoParam;
            isValid = ( pMqttContext != NULL ) && 
                      ( pMqttInfoParam != NULL ) && 
                      ( incomingPublishCallback != NULL ) &&
                      ( pSubscribeInfo->topicFilterLength < MQTT_AGENT_SUBSCRIPTION_BUFFER_SIZE );
            break;

        case UNSUBSCRIBE:
            isValid = ( pMqttContext != NULL ) && ( pMqttInfoParam != NULL );
            break;

        case PUBLISH:
            isValid = ( pMqttContext != NULL ) && ( pMqttInfoParam != NULL );
            break;

        case PROCESSLOOP:
        case PING:
        case DISCONNECT:
        case FREE:
            isValid = ( pMqttContext != NULL );
            break;

        default:
            /* Other operations don't need the MQTT context. */
            isValid = true;
            break;
    }

    if( isValid )
    {
        /* Copy the publish or subscribe info into the command so the application
         * writer does not need to keep it in context until the command completes. */
        if( commandType == SUBSCRIBE )
        {
            pCommand->mqttOperationInfo.subscribeInfo = *( ( MQTTSubscribeInfo_t * ) pMqttInfoParam );
        }

        if( commandType == PUBLISH )
        {
            pCommand->mqttOperationInfo.publishInfo = *( ( MQTTPublishInfo_t * ) pMqttInfoParam );
        }

        pCommand->commandType = commandType;
        pCommand->pMqttContext = pMqttContext;
        pCommand->pIncomingPublishCallback = incomingPublishCallback;
        pCommand->pIncomingPublishCallbackContext = pIncomingPublishCallbackContext;
        pCommand->pxCmdContext = pCommandCompleteCallbackContext;
        pCommand->pCommandCompleteCallback = commandCompleteCallback;
    }

    return isValid;
}

/*-----------------------------------------------------------*/

static bool addCommandToQueue( Command_t * pCommand )
{
bool ret;

    /* The application called an API function.  The API function was validated and
     * packed into a Command_t structure.  Now post a reference to the Command_t
     * structure to the MQTT agent for processing. */
    if( commandQueue == NULL )
    {
        ret = false;
    }
    else
    {
        ret = xQueueSendToBack( commandQueue, &pCommand, MQTT_AGENT_QUEUE_WAIT_TIME );
    }

    return ret;
}

/*-----------------------------------------------------------*/

static MQTTStatus_t processCommand( Command_t * pCommand ) //_RB_ Break up into sub-functions.
{
    MQTTStatus_t operationStatus = MQTTSuccess;
    uint16_t packetId = MQTT_PACKET_ID_INVALID;
    bool addAckToList = false, ackAdded = false;
    MQTTPublishInfo_t * pPublishInfo;
    MQTTSubscribeInfo_t * pSubscribeInfo;    
    MQTTAgentContext_t * pAgentContext;
    MQTTContext_t * pMQTTContext;
    size_t i;
    uint32_t processLoopTimeoutMs = MQTT_AGENT_PROCESS_LOOP_TIMEOUT_MS;
    const size_t maxNewSubscriptionsInOneGo = ( size_t ) 1; /* The agent interface only allows one subscription command at a time. */

    if( pCommand != NULL )
    {
        pMQTTContext = pCommand->pMqttContext;
        switch( pCommand->commandType )
        {
            case PUBLISH:
                pPublishInfo = ( MQTTPublishInfo_t * )  &( pCommand->mqttOperationInfo.publishInfo );

                if( pPublishInfo->qos != MQTTQoS0 )
                {
                    packetId = MQTT_GetPacketId( pMQTTContext );
                }

                LogInfo( ( "Publishing message to %.*s.\n", ( int ) pPublishInfo->topicNameLength, pPublishInfo->pTopicName ) );
                operationStatus = MQTT_Publish( pMQTTContext, pPublishInfo, packetId );

                /* Add to pending ack list, or call callback if QoS 0. */
                addAckToList = ( pPublishInfo->qos != MQTTQoS0 ) && ( operationStatus == MQTTSuccess );
                break;

            case SUBSCRIBE:
            case UNSUBSCRIBE:
                pSubscribeInfo = ( MQTTSubscribeInfo_t * ) &( pCommand->mqttOperationInfo.subscribeInfo );
                packetId = MQTT_GetPacketId( pMQTTContext );

                if( pCommand->commandType == SUBSCRIBE )
                {
                    /* Even if some subscriptions already exist in the subscription list,
                     * it is fine to send another subscription request. A valid use case
                     * for this is changing the maximum QoS of the subscription. */
                    operationStatus = MQTT_Subscribe( pMQTTContext,
                                              pSubscribeInfo,
                                              maxNewSubscriptionsInOneGo,
                                              packetId );
                }
                else
                {
                    operationStatus = MQTT_Unsubscribe( pMQTTContext,
                                                pSubscribeInfo,
                                                maxNewSubscriptionsInOneGo,
                                                packetId );
                }

                addAckToList = ( operationStatus == MQTTSuccess );
                break;

            case PING:
                operationStatus = MQTT_Ping( pMQTTContext );

                break;

            case DISCONNECT:
                operationStatus = MQTT_Disconnect( pMQTTContext );

                break;

            case FREE:
                for( i = 0; i < MAX_CONNECTIONS; i++ )
                {
                    if( agentContexts[ i ].pMQTTContext == pMQTTContext )
                    {
                        memset( &agentContexts[ i ], 0x00, sizeof( MQTTAgentContext_t ) );
                        break;
                    }
                }

                break;

            case TERMINATE:
                LogInfo( ( "Terminating command loop.\n" ) );

            default:
                break;
        }

        if( addAckToList )
        {
            pAgentContext = getAgentFromContext( pCommand->pMqttContext );
            ackAdded = addAwaitingOperation( pAgentContext, packetId, pCommand );

            /* Set the return status if no memory was available to store the operation
             * information. */
            if( !ackAdded )
            {
                LogError( ( "No memory to wait for acknowledgment for packet %u\n", packetId ) );

                /* All operations that can wait for acks (publish, subscribe, unsubscribe)
                 * require a context. */
                operationStatus = MQTTNoMemory;//_RB_ Should the command structure be returned here?
            }
        }
        else
        {
            //_RB_ Should the command structure be freed here?
        }

        if( !ackAdded )
        {
            /* The command is complete, call the callback. */
            if( pCommand->pCommandCompleteCallback != NULL )
            {
                pCommand->pCommandCompleteCallback( pCommand->pxCmdContext, operationStatus );
            }

            releaseCommandStructureToPool( pCommand );
        }
    }

    /* If empty command, iterate through stored contexts so that all MQTT
     * connections are used equally across the empty commands. */
//_RB_ Command structure has already been released    if( pCommand->commandType == NONE )
//    {
//        pMQTTContext = getContextForProcessLoop();
//        /* Set context for original command in case this results in a network error. */
//        pCommand->pMqttContext = pMQTTContext;
//    }

    /* Run a single iteration of the process loop if there were no errors and
     * the MQTT connection still exists. */
    for( i = 0; i < MAX_CONNECTIONS; i++ )
    {
        pAgentContext = &( agentContexts[ i ] );
        if( pAgentContext->pMQTTContext != NULL )
        {
            do
            {
                packetProcessedDuringLoop = false;

                if( ( operationStatus == MQTTSuccess ) && 
                    ( pAgentContext->pMQTTContext->connectStatus == MQTTConnected ) )
                {
                    operationStatus = MQTT_ProcessLoop( pAgentContext->pMQTTContext, 
                                                        processLoopTimeoutMs );
                }
            } while( packetProcessedDuringLoop == true );
        }
    }

    return operationStatus;/*_RB_ Need to determine which agent the status came from. */
}

/*-----------------------------------------------------------*/

static void handleIncomingPublish( MQTTAgentContext_t * pAgentContext,
                                   MQTTPublishInfo_t * pPublishInfo )
{
    bool isMatched = false, relayedPublish = false;
    MQTTStatus_t operationStatus;
    size_t i;
    SubscriptionElement_t * pxSubscriptions = pAgentContext->pSubscriptionList;

    for( i = 0; i < SUBSCRIPTIONS_MAX_COUNT; i++ )
    {
        if( pxSubscriptions[ i ].filterStringLength > 0 )
        {
            operationStatus = MQTT_MatchTopic( pPublishInfo->pTopicName,
                                               pPublishInfo->topicNameLength,
                                               pxSubscriptions[ i ].pSubscriptionFilterString,
                                               pxSubscriptions[ i ].filterStringLength,
                                               &isMatched );
            if( isMatched )
            {
                LogDebug( ( "Adding publish to response queue for %.*s\n",
                            pxSubscriptions[ i ].filterStringLength,
                            pxSubscriptions[ i ].pSubscriptionFilterString ) );
                pxSubscriptions[ i ].pIncomingPublishCallback( pPublishInfo, pxSubscriptions[ i ].pIncomingPublishCallbackContext );
                relayedPublish = true;
            }
            else
            {
                /* Terminate the string for logging. */
                LogWarn( ( "Received unmatched incoming publish." ) );
            }
        }
    }

    /* It is possible a publish was sent on an unsubscribed topic. This is
     * possible on topics reserved by the broker, e.g. those beginning with
     * '$'. In this case, we copy the publish to a queue we configured to
     * receive these publishes. */
    if( !relayedPublish )
    {
        LogWarn( ( "Publish received on topic %.*s with no subscription.\n",
                   pPublishInfo->topicNameLength,
                   pPublishInfo->pTopicName ) );

        if( pAgentContext->pUnsolicitedPublishCallback != NULL )
        {
            pAgentContext->pUnsolicitedPublishCallback( pPublishInfo, 
                                                        pAgentContext->pUnsolicitedPublishCallbackContext );
        }
    }
}

/*-----------------------------------------------------------*/

static void handleSubscriptionAcks( MQTTAgentContext_t * pAgentContext,
                                    MQTTPacketInfo_t * pPacketInfo,
                                    MQTTDeserializedInfo_t * pDeserializedInfo,
                                    AckInfo_t * pAckInfo,
                                    uint8_t packetType )
{
    CommandContext_t * pAckContext = NULL;
    CommandCallback_t ackCallback = NULL;
    uint8_t * pSubackCodes = NULL;
    MQTTSubscribeInfo_t * pSubscribeInfo = NULL;

    configASSERT( pAckInfo != NULL );

    pAckContext = pAckInfo->pOriginalCommand->pxCmdContext;
    ackCallback = pAckInfo->pOriginalCommand->pCommandCompleteCallback;
    pSubscribeInfo = &( pAckInfo->pOriginalCommand->mqttOperationInfo.subscribeInfo );
    pSubackCodes = pPacketInfo->pRemainingData + 2U; /*_RB_ Where does 2 come from? */

    if( packetType == MQTT_PACKET_TYPE_SUBACK )
    {
        if( *pSubackCodes != MQTTSubAckFailure )
        {
            LogInfo( ( "Adding subscription to %.*s\n",//_RB_ This format specifier is not portable.  Need to do something so topic filters are always terminated.
                        pSubscribeInfo->topicFilterLength,
                        pSubscribeInfo->pTopicFilter ) );
            addSubscription( pAgentContext,
                             pSubscribeInfo->pTopicFilter,
                             pSubscribeInfo->topicFilterLength,
                             pAckInfo->pOriginalCommand->pIncomingPublishCallback,
                             pAckInfo->pOriginalCommand->pIncomingPublishCallbackContext );
        }
        else
        {
            LogError( ( "Subscription to %.*s failed.\n",
                        pSubscribeInfo->topicFilterLength,
                        pSubscribeInfo->pTopicFilter ) );
        }
    }
    else
    {
        LogInfo( ( "Removing subscription to %.*s\n",
                    pSubscribeInfo->topicFilterLength,
                    pSubscribeInfo->pTopicFilter ) );
        removeSubscription( pAgentContext,
                            pSubscribeInfo->pTopicFilter,
                            pSubscribeInfo->topicFilterLength );
    }

    if( ackCallback != NULL )
    {
        ackCallback( pAckContext, pDeserializedInfo->deserializationResult );
    }

    releaseCommandStructureToPool( pAckInfo->pOriginalCommand ); //_RB_ Is this always the right place for this?
}

/*-----------------------------------------------------------*/

static MQTTContext_t * getContextForProcessLoop( void )
{
    static uint32_t contextIndex = 0U;
    uint32_t oldIndex = 0U;
    MQTTContext_t * ret = NULL;

    oldIndex = contextIndex;

    do
    {
        ret = agentContexts[ contextIndex ].pMQTTContext;

        if( ++contextIndex >= MAX_CONNECTIONS )
        {
            contextIndex = 0U;
        }
    } while( ( ret == NULL ) && ( oldIndex != contextIndex ) );

    return ret;
}

/*-----------------------------------------------------------*/

static MQTTAgentContext_t * getAgentFromContext( MQTTContext_t * pMQTTContext )
{
    MQTTAgentContext_t * ret = NULL;
    int i = 0;

    configASSERT( pMQTTContext );

    for( i = 0; i < MAX_CONNECTIONS; i++ )
    {
        if( agentContexts[ i ].pMQTTContext == pMQTTContext )
        {
            ret = &agentContexts[ i ];
            break;
        }
    }

    return ret;
}

/*-----------------------------------------------------------*/

static void mqttEventCallback( MQTTContext_t * pMqttContext,
                               MQTTPacketInfo_t * pPacketInfo,
                               MQTTDeserializedInfo_t * pDeserializedInfo )
{
    AckInfo_t ackInfo;
    uint16_t packetIdentifier = pDeserializedInfo->packetIdentifier;
    CommandCallback_t ackCallback = NULL;
    MQTTAgentContext_t * pAgentContext;
    const uint8_t uppderNibble = ( uint8_t ) 0xF0;

    configASSERT( pMqttContext != NULL );
    configASSERT( pPacketInfo != NULL );

    pAgentContext = getAgentFromContext( pMqttContext );
    configASSERT( pAgentContext != NULL );

    /* This callback executes from within MQTT_ProcessLoop().  Setting this flag
     * indicates that the callback executed so the caller of MQTT_ProcessLoop() knows
     * it should call it again as there may be more data to process. */
    packetProcessedDuringLoop = true;

    /* Handle incoming publish. The lower 4 bits of the publish packet type is used 
     * for the dup, QoS, and retain flags. Hence masking out the lower bits to check 
     * if the packet is publish. */
    if( ( pPacketInfo->type & uppderNibble ) == MQTT_PACKET_TYPE_PUBLISH )
    {
        handleIncomingPublish( pAgentContext, pDeserializedInfo->pPublishInfo );
    }
    else
    {
        /* Handle other packets. */
        switch( pPacketInfo->type )
        {
            case MQTT_PACKET_TYPE_PUBACK:
            case MQTT_PACKET_TYPE_PUBCOMP:
                ackInfo = getAwaitingOperation( pAgentContext, packetIdentifier, true );

                if( ackInfo.packetId == packetIdentifier )
                {
                    ackCallback = ackInfo.pOriginalCommand->pCommandCompleteCallback;

                    if( ackCallback != NULL )
                    {
                        ackCallback( ackInfo.pOriginalCommand->pxCmdContext, 
                                     pDeserializedInfo->deserializationResult );
                    }
                }
                releaseCommandStructureToPool( ackInfo.pOriginalCommand ); //_RB_ Is this always the right place for this?
                break;

            case MQTT_PACKET_TYPE_SUBACK:
            case MQTT_PACKET_TYPE_UNSUBACK:
                ackInfo = getAwaitingOperation( pAgentContext, packetIdentifier, true );

                if( ackInfo.packetId == packetIdentifier )
                {
                    handleSubscriptionAcks( pAgentContext, 
                                            pPacketInfo, 
                                            pDeserializedInfo, 
                                            &ackInfo, 
                                            pPacketInfo->type );
                }
                else
                {
                    LogError( ( "No subscription or unsubscribe operation found matching packet id %u.\n", packetIdentifier ) );
                }

                break;

            /* Nothing to do for these packets since they don't indicate command completion. */
            case MQTT_PACKET_TYPE_PUBREC:
            case MQTT_PACKET_TYPE_PUBREL:
                break;

            case MQTT_PACKET_TYPE_PINGRESP:

                /* Nothing to be done from application as library handles
                 * PINGRESP with the use of MQTT_ProcessLoop API function. */
                LogWarn( ( "PINGRESP should not be handled by the application "
                           "callback when using MQTT_ProcessLoop.\n" ) );
                break;

            /* Any other packet type is invalid. */
            default:
                LogError( ( "Unknown packet type received:(%02x).\n",
                            pPacketInfo->type ) );
        }
    }
}

/*-----------------------------------------------------------*/
//_RB_ Should return an MQTTStatus_t.
static bool createAndAddCommand( CommandType_t commandType,
                                 MQTTContextHandle_t mqttContextHandle,
                                 void * pMqttInfoParam,
                                 CommandCallback_t commandCompleteCallback,
                                 CommandContext_t * pCommandCompleteCallbackContext,
                                 PublishCallback_t incomingPublishCallback,
                                 void * pIncomingPublishCallbackContext )
{
    bool ret = false;
    const TickType_t blockTimeMs = ( TickType_t ) pdMS_TO_TICKS( 1500 );/*_RB_ Could make a parameter. */
    Command_t *pCommand;

    /* If the packet ID is zero then the MQTT context has not been initialised as 0
     * is the initial value but not a valid packet ID. */
    if( ( mqttContextHandle < MAX_CONNECTIONS ) && ( mqttContexts[ mqttContextHandle ].nextPacketId != 0 ) )
    {
        if( commandType == PROCESSLOOP ) /*_RB_ What if another task calls this? */
        {
            static Command_t xStaticCommand = { 0 };

            /* This is called from the MQTT agent context so cannot wait for a command
             * structure.  The command structure is only used to unblock the task rather
             * than carry data so can just be a single static here. */
            pCommand = &xStaticCommand;
        }
        else
        {
            pCommand = getCommandStructureFromPool( blockTimeMs );
        }

        if( pCommand != NULL )
        {
            ret = createCommand( commandType,
                                 &( mqttContexts[ mqttContextHandle ] ),
                                 pMqttInfoParam,
                                 incomingPublishCallback,
                                 pIncomingPublishCallbackContext,
                                 commandCompleteCallback,
                                 pCommandCompleteCallbackContext,
                                 pCommand );

            if( ret )
            {
                ret = addCommandToQueue( pCommand );
            }

            if( ret == false )
            {
                /* Could not send the command to the queue so release the command
                 * structure again. */
                releaseCommandStructureToPool( pCommand );
            }
        }
        else
        {
            ret = false;
        }

        return ret;

    }

    return ret;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_Init( MQTTContextHandle_t mqttContextHandle,
                             TransportInterface_t *pTransportInterface,
                             MQTTGetCurrentTimeFunc_t getCurrentTimeMs,
                             PublishCallback_t unkownIncomingPublishCallback,
                             void * pDefaultPublishContext )
{
    MQTTStatus_t returnStatus;
    MQTTFixedBuffer_t networkBuffer;
    static uint8_t staticQueueStorageArea[ MQTT_AGENT_COMMAND_QUEUE_LENGTH * sizeof( Command_t * ) ];
    static StaticQueue_t staticQueueStructure;

    /* The command queue should not have been created yet. */
    configASSERT( commandQueue == NULL ); /*_RB_ Needs to change so two contexts can be used in the same agent. */
    commandQueue = xQueueCreateStatic(  MQTT_AGENT_COMMAND_QUEUE_LENGTH,
                                        sizeof( Command_t * ),
                                        staticQueueStorageArea,
                                        &staticQueueStructure );

    /*_RB_ Need to make singleton. */

    if( ( mqttContextHandle >= MAX_CONNECTIONS ) ||
        ( pTransportInterface == NULL ) ||
        ( getCurrentTimeMs == NULL ) )
    {
        returnStatus = MQTTBadParameter;
    }
    else
    {
        /* Fill the values for network buffer. */
        networkBuffer.pBuffer = &( networkBuffers[ mqttContextHandle ][ 0 ] );
        networkBuffer.size = mqttexampleNETWORK_BUFFER_SIZE;

        returnStatus = MQTT_Init( &( mqttContexts[ mqttContextHandle ] ),
                                  pTransportInterface,
                                  getCurrentTimeMs,
                                  mqttEventCallback,
                                  &networkBuffer );

        if( returnStatus == MQTTSuccess )
        {
            /* Also initialise the agent context.  Assert if already initialised. */
            configASSERT( agentContexts[ mqttContextHandle ].pMQTTContext == NULL );
            agentContexts[ mqttContextHandle ].pMQTTContext = &( mqttContexts[ mqttContextHandle ] );
            agentContexts[ mqttContextHandle ].pUnsolicitedPublishCallback = unkownIncomingPublishCallback;
            agentContexts[ mqttContextHandle ].pUnsolicitedPublishCallbackContext = pDefaultPublishContext;

            memset( ( void * ) commandStructurePool, 0x00, sizeof( commandStructurePool ) );
            freeCommandStructMutex = xSemaphoreCreateCounting( MQTT_COMMAND_CONTEXTS_POOL_SIZE, MQTT_COMMAND_CONTEXTS_POOL_SIZE );
            configASSERT( freeCommandStructMutex ); /*_RB_ Create all objects here statically. */
        }
    }

    return returnStatus;
}

/*-----------------------------------------------------------*/

MQTTContext_t * MQTTAgent_CommandLoop( void )
{
    Command_t *pCommand;
    MQTTStatus_t operationStatus = MQTTSuccess;
    MQTTContext_t * ret = NULL;

    /* The command queue should have been created before this task gets created. */
    configASSERT( commandQueue );

    /* Loop until we receive a terminate command. */
    for( ; ; )
    {
        /* Wait for the next command, if any. */
        pCommand = NULL;
        xQueueReceive( commandQueue, &( pCommand ), MQTT_AGENT_QUEUE_WAIT_TIME );
        operationStatus = processCommand( pCommand );

        /* Return the current MQTT context if status was not successful. */
        if( operationStatus != MQTTSuccess )
        {
            LogError( ( "MQTT operation failed with status %s\n",
                        MQTT_Status_strerror( operationStatus ) ) );
            ret = pCommand->pMqttContext;
            break;
        }

        /* Terminate the loop if we receive the termination command. */
        if( pCommand->commandType == TERMINATE )
        {
            ret = NULL;
            break;
        }
    }

    return ret;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_ResumeSession( MQTTContextHandle_t mqttContextHandle,
                                      bool sessionPresent ) /*_RB_ Need to test this function. */
{
    MQTTStatus_t statusResult = MQTTSuccess;
    MQTTContext_t *pMqttContext;
    MQTTAgentContext_t * pAgentContext;
    AckInfo_t * pendingAcks;
    SubscriptionElement_t * pxSubscriptions;
    MQTTSubscribeInfo_t * pxResendSubscriptions;
    MQTTPublishInfo_t * originalPublish = NULL;

    /* If the packet ID is zero then the MQTT context has not been initialised as 0
     * is the initial value but not a valid packet ID. */
    if( ( mqttContextHandle < MAX_CONNECTIONS ) && ( mqttContexts[ mqttContextHandle ].nextPacketId != 0 ) )
    {
        pMqttContext = &( mqttContexts[ mqttContextHandle ] );
        pAgentContext = getAgentFromContext( pMqttContext );
        pendingAcks = pAgentContext->pPendingAcks;
        pxSubscriptions = pAgentContext->pSubscriptionList;
        pxResendSubscriptions = pAgentContext->pResendSubscriptions;

        /* Resend publishes if session is present. NOTE: It's possible that some
         * of the operations that were in progress during the network interruption
         * were subscribes. In that case, we would want to mark those operations
         * as completing with error and remove them from the list of operations, so
         * that the calling task can try subscribing again. We do not handle that
         * case in this demo for simplicity, since only one subscription packet is
         * sent per iteration of this demo. */
        if( sessionPresent )
        {
            MQTTStateCursor_t cursor = MQTT_STATE_CURSOR_INITIALIZER;
            uint16_t packetId = MQTT_PACKET_ID_INVALID;
            AckInfo_t foundAck;

            packetId = MQTT_PublishToResend( pAgentContext->pMQTTContext, &cursor );

            while( packetId != MQTT_PACKET_ID_INVALID )
            {
                /* Retrieve the operation but do not remove it from the list. */
                foundAck = getAwaitingOperation( pAgentContext, packetId, false );

                if( foundAck.packetId == packetId )
                {
                    /* Set the DUP flag. */
                    originalPublish = &( foundAck.pOriginalCommand->mqttOperationInfo.publishInfo );
                    originalPublish->dup = true;
                    statusResult = MQTT_Publish( pAgentContext->pMQTTContext, originalPublish, packetId );

                    if( statusResult != MQTTSuccess )
                    {
                        LogError( ( "Error in resending publishes. Error code=%s\n", MQTT_Status_strerror( statusResult ) ) );
                        break;
                    }
                }

                packetId = MQTT_PublishToResend( pAgentContext->pMQTTContext, &cursor );
            }
        }

        /* If we wanted to resume a session but none existed with the broker, we
         * should mark all in progress operations as errors so that the tasks that
         * created them can try again. Also, we will resubscribe to the filters in
         * the subscription list, so tasks do not unexpectedly lose their subscriptions. */
        else
        {
            size_t i = 0, j = 0;
            Command_t xNewCommand;
            bool xCommandCreated = false;
            BaseType_t xCommandAdded;

            /* We have a clean session, so clear all operations pending acknowledgments. */
            for( i = 0; i < PENDING_ACKS_MAX_SIZE; i++ )
            {
                if( pendingAcks[ i ].packetId != MQTT_PACKET_ID_INVALID )
                {
                    if( pendingAcks[ i ].pOriginalCommand->pCommandCompleteCallback != NULL )
                    {
                        /* Bad response to indicate network error. */
                        pendingAcks[ i ].pOriginalCommand->pCommandCompleteCallback( pendingAcks[ i ].pOriginalCommand->pxCmdContext, MQTTBadResponse );
                    }

                    /* Now remove it from the list. */
                    getAwaitingOperation( pAgentContext, pendingAcks[ i ].packetId, true );
                }
            }

            /* Populate the array of MQTTSubscribeInfo_t. It's possible there may be
             * repeated subscriptions in the list. This is fine, since clients
             * are able to subscribe to a topic with an existing subscription. */
            for( i = 0; i < SUBSCRIPTIONS_MAX_COUNT; i++ )
            {
                if( pxSubscriptions[ i ].filterStringLength != 0 )
                {
                    pxResendSubscriptions[ j ].pTopicFilter = pxSubscriptions[ i ].pSubscriptionFilterString;
                    pxResendSubscriptions[ j ].topicFilterLength = pxSubscriptions[ i ].filterStringLength;
                    pxResendSubscriptions[ j ].qos = MQTTQoS1;
                    j++;
                }
            }

            /* Resubscribe if needed. */
            if( j > 0 )
            {
    //_RB_ removed j below           xCommandCreated = createCommand( SUBSCRIBE, pMqttContext, pxResendSubscriptions, j, NULL, NULL, NULL, NULL, &xNewCommand );
                xCommandCreated = createCommand( SUBSCRIBE, pMqttContext, pxResendSubscriptions, NULL, NULL, NULL, NULL, &xNewCommand );
                configASSERT( xCommandCreated == true );
    //_RB_            xNewCommand.uintParam = j;
                xNewCommand.pIncomingPublishCallbackContext = NULL;
                /* Send to the front of the queue so we will resubscribe as soon as possible. */
                xCommandAdded = xQueueSendToFront( commandQueue, &xNewCommand, MQTT_AGENT_QUEUE_WAIT_TIME );
                configASSERT( xCommandAdded == pdTRUE );
            }
        }
    }
    else
    {
        statusResult = MQTTIllegalState;
    }

    return statusResult;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_Connect( MQTTContextHandle_t mqttContextHandle,
                                const MQTTConnectInfo_t * pConnectInfo,
                                const MQTTPublishInfo_t * pWillInfo,
                                uint32_t timeoutMs,
                                bool * pSessionPresent )
{
    MQTTStatus_t ret;
    
    if( ( mqttContextHandle < MAX_CONNECTIONS ) && 
        ( mqttContexts[ mqttContextHandle ].connectStatus == MQTTNotConnected ) )
    {
        ret = MQTT_Connect( &( mqttContexts[ mqttContextHandle ] ),
                            pConnectInfo,
                            pWillInfo,
                            timeoutMs,
                            pSessionPresent );
    }
    else
    {
        ret = MQTTIllegalState;
    }

    return ret;
} 

/*-----------------------------------------------------------*/
/*_RB_ Should return MQTTStatus_t. */
bool MQTTAgent_Subscribe( MQTTContextHandle_t mqttContextHandle,
                          MQTTSubscribeInfo_t * pSubscriptionInfo,
                          PublishCallback_t incomingPublishCallback,
                          void * incomingPublishCallbackContext,
                          CommandCallback_t commandCompleteCallback,
                          void * commandCompleteCallbackContext )
{
bool ret;

    ret = createAndAddCommand( SUBSCRIBE,                       /* commandType */
                               mqttContextHandle,               /* mqttContextHandle */
                               pSubscriptionInfo,               /* pMqttInfoParam */
                               commandCompleteCallback,         /* commandCompleteCallback */
                               commandCompleteCallbackContext,  /* pCommandCompleteCallbackContext */
                               incomingPublishCallback,         /* incomingPublishCallback */
                               incomingPublishCallbackContext );/* pIncomingPublishCallbackContext */
    return ret;
}

/*-----------------------------------------------------------*/

bool MQTTAgent_Unsubscribe( MQTTContextHandle_t mqttContextHandle,
                            MQTTSubscribeInfo_t * pSubscriptionList,
                            CommandContext_t * pCommandCompleteCallbackContext,
                            CommandCallback_t cmdCompleteCallback )/*_RB_ Swap these parameters around and check the other command APIs.  Also document what happens to the contexts in the header file - i.e. it gets passed into the callback. */
{
    return createAndAddCommand( UNSUBSCRIBE,                    /* commandType */
                                mqttContextHandle,              /* mqttContextHandle */
                                pSubscriptionList,              /* pMqttInfoParam */
                                cmdCompleteCallback,            /* commandCompleteCallback */
                                pCommandCompleteCallbackContext,/* pCommandCompleteCallbackContext */
                                NULL,                           /* incomingPublishCallback */
                                NULL );                         /* pIncomingPublishCallbackContext */
}

/*-----------------------------------------------------------*/

bool MQTTAgent_Publish( MQTTContextHandle_t mqttContextHandle,
                        MQTTPublishInfo_t * pPublishInfo,
                        CommandCallback_t commandCompleteCallback,
                        CommandContext_t * commandCompleteCallbackContext )
{
    return createAndAddCommand( PUBLISH,                        /* commandType */
                                mqttContextHandle,              /* mqttContextHandle */
                                pPublishInfo,                   /* pMqttInfoParam */
                                commandCompleteCallback,        /* commandCompleteCallback */
                                commandCompleteCallbackContext, /* pCommandCompleteCallbackContext */
                                NULL,                           /* incomingPublishCallback */
                                NULL );                         /* pIncomingPublishCallbackContext */
}

/*-----------------------------------------------------------*/

bool MQTTAgent_ProcessLoop( MQTTContextHandle_t mqttContextHandle,
                            CommandContext_t * pCommandCompleteCallbackContext,
                            CommandCallback_t cmdCompleteCallback )
{
    return createAndAddCommand( PROCESSLOOP,                    /* commandType */
                                mqttContextHandle,              /* mqttContextHandle */
                                NULL,                           /* pMqttInfoParam */
                                cmdCompleteCallback,            /* commandCompleteCallback */
                                pCommandCompleteCallbackContext,/* pCommandCompleteCallbackContext */
                                NULL,                           /* incomingPublishCallback */
                                NULL );                         /* pIncomingPublishCallbackContext */
}


/*-----------------------------------------------------------*/

bool MQTTAgent_Ping( MQTTContextHandle_t mqttContextHandle, /*_RB_ Use this in the demo. */
                     CommandContext_t * pCommandCompleteCallbackContext,
                     CommandCallback_t cmdCompleteCallback )
{
    return createAndAddCommand( PING,                           /* commandType */
                                mqttContextHandle,              /* mqttContextHandle */
                                NULL,                           /* pMqttInfoParam */
                                cmdCompleteCallback,            /* commandCompleteCallback */
                                pCommandCompleteCallbackContext,/* pCommandCompleteCallbackContext */
                                NULL,                           /* incomingPublishCallback */
                                NULL );                         /* pIncomingPublishCallbackContext */
}

/*-----------------------------------------------------------*/

bool MQTTAgent_Disconnect( MQTTContextHandle_t mqttContextHandle,
                           CommandContext_t * pCommandCompleteCallbackContext,
                           CommandCallback_t cmdCompleteCallback )
{
    return createAndAddCommand( DISCONNECT,                     /* commandType */
                                mqttContextHandle,              /* mqttContextHandle */
                                NULL,                           /* pMqttInfoParam */
                                cmdCompleteCallback,            /* commandCompleteCallback */
                                pCommandCompleteCallbackContext,/* pCommandCompleteCallbackContext */
                                NULL,                           /* incomingPublishCallback */
                                NULL );                         /* pIncomingPublishCallbackContext */
}

/*-----------------------------------------------------------*/

bool MQTTAgent_Free( MQTTContextHandle_t mqttContextHandle,
                     CommandContext_t * pCommandCompleteCallbackContext,
                     CommandCallback_t cmdCompleteCallback )
{
    return createAndAddCommand( FREE,                           /* commandType */
                                mqttContextHandle,              /* mqttContextHandle */
                                NULL,                           /* pMqttInfoParam */
                                cmdCompleteCallback,            /* commandCompleteCallback */
                                pCommandCompleteCallbackContext,/* pCommandCompleteCallbackContext */
                                NULL,                           /* incomingPublishCallback */
                                NULL );                         /* pIncomingPublishCallbackContext */
}

/*-----------------------------------------------------------*/

bool MQTTAgent_Terminate( void )
{
    return createAndAddCommand( TERMINATE, 0, NULL, NULL, NULL, NULL, NULL );
}

/*-----------------------------------------------------------*/

uint32_t MQTTAgent_GetNumWaiting( void )
{
uint32_t ret;

    if( commandQueue != NULL )
    {
        ret = uxQueueMessagesWaiting( commandQueue );
    }
    else
    {
        ret = 0U;
    }

    return ret;
}

//_RB_ Hack to integrate OTA.
MQTTContext_t * MQTTAgent_GetMQTTContext( int );
MQTTContext_t * MQTTAgent_GetMQTTContext( int x )
{
    return &mqttContexts[ x ];
}
/*-----------------------------------------------------------*/

