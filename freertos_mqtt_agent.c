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
 * See https://www.FreeRTOS.org/mqtt_agent for examples and usage information.
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
    MQTTContext_t * pMqttContext;
    CommandCallback_t pCommandCompleteCallback;
    CommandContext_t * pxCmdContext;
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
    Command_t * pOriginalCommand;
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
    char pSubscriptionFilterString[ MQTT_AGENT_MAX_SUBSCRIPTION_FILTER_LENGTH ];
} SubscriptionElement_t;

/**
 * @brief Associated information for a single MQTT connection.
 */
typedef struct MQTTAgentContext
{
    MQTTContext_t * pMQTTContext;
    AckInfo_t pPendingAcks[ MQTT_AGENT_MAX_OUTSTANDING_ACKS ];
    SubscriptionElement_t pSubscriptionList[ MQTT_AGENT_MAX_SIMULTANEOUS_SUBSCRIPTIONS ];
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
 * @return `MQTTSuccess` if all necessary fields for the command are passed,
 * else an enumerated error code.
 */
static MQTTStatus_t createCommand( CommandType_t commandType,
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
 * @param[in] blockTimeMS The maximum amount of time to milliseconds to wait in the
 * Blocked state (so not consuming any CPU time) for the command to be posted to the
 * queue should the queue already be full.
 *
 * @return MQTTSuccess if the command was added to the queue, else an enumerated
 * error code.
 */
static MQTTStatus_t addCommandToQueue( Command_t * pCommand,
                                       uint32_t blockTimeMS );

/**
 * @brief Process a #Command_t.
 *
 * @note This agent does not check existing subscriptions before sending a
 * SUBSCRIBE or UNSUBSCRIBE packet. If a subscription already exists, then
 * a SUBSCRIBE packet will be sent anyway, and if multiple tasks are subscribed
 * to a topic filter, then they will all be unsubscribed after an UNSUBSCRIBE.
 *
 * @param[in] pCommand Pointer to command to process.
 * @param[out] pOutMqttContext Double pointer to MQTT context that returned the status.
 *
 * @return status of MQTT library API call.
 */
static MQTTStatus_t processCommand( Command_t * pCommand,
                                    MQTTContext_t ** pOutMqttContext );

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
 * @param[in] blockTimeMS Maximum amount of time in milliseconds to wait (in the
 * Blocked state, so not consuming any CPU time) for the command to be posted to the
 * MQTT agent should the MQTT agent's event queue be full.
 *
 * @return MQTTSuccess if the command was posted to the MQTT agent's event queue.
 * Otherwise an enumerated error code.
 */
static MQTTStatus_t createAndAddCommand( CommandType_t commandType,
                                         MQTTContextHandle_t mqttContextHandle,
                                         void * pMqttInfoParam,
                                         CommandCallback_t cmdCompleteCallback,
                                         CommandContext_t * pCommandCompleteCallbackContext,
                                         PublishCallback_t incomingPublishCallback,
                                         void * pIncomingPublishCallbackContext,
                                         uint32_t blockTimeMS );


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
 * @return A pointer to a Command_t structure if one becomes available before
 * blockTimeMs time expired, otherwise NULL.
 */
static Command_t * getCommandStructureFromPool( TickType_t blockTimeMs );

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
static bool releaseCommandStructureToPool( Command_t * pxCommandToRelease );

/**
 * @brief A #CommandCallback_t used for a resubscribe operation during a reconnect. It
 * sets a variable so that the result of the subscribe can be accessed.
 *
 * @param[in] pResubscribeContext Currently unused.
 * @param[in] subscribeStatus Deserialized result of SUBACK.
 */
static void resubscribeCallback( void * pResubscribeContext,
                                 MQTTStatus_t subscribeStatus );

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
static MQTTAgentContext_t agentContexts[ MQTT_AGENT_MAX_SIMULTANEOUS_CONNECTIONS ] = { 0 }; /*_RB_ Change name of constant. */
static MQTTContext_t mqttContexts[ MQTT_AGENT_MAX_SIMULTANEOUS_CONNECTIONS ] = { 0 };

/**
 * @brief MQTTSubscribeInfo_t to use when re-establishing subscriptions, if a broker starts a clean session.
 */
static MQTTSubscribeInfo_t pResendSubscription;
static MQTTStatus_t resubscribeStatuses[ MQTT_AGENT_MAX_SIMULTANEOUS_SUBSCRIPTIONS ];

/**
 * @brief The network buffer must remain valid for the lifetime of the MQTT context.
 */
static uint8_t networkBuffers[ MQTT_AGENT_MAX_SIMULTANEOUS_CONNECTIONS ][ MQTT_AGENT_NETWORK_BUFFER_SIZE ];

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

static bool releaseCommandStructureToPool( Command_t * pxCommandToRelease )
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

static Command_t * getCommandStructureFromPool( TickType_t blockTimeMs )
{
    Command_t * structToUse = NULL;
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
    for( i = 0; i < MQTT_AGENT_MAX_OUTSTANDING_ACKS; i++ )
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
    for( i = 0; i < MQTT_AGENT_MAX_OUTSTANDING_ACKS; i++ )
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
    size_t availableIndex = MQTT_AGENT_MAX_SIMULTANEOUS_SUBSCRIPTIONS;
    SubscriptionElement_t * pSubscriptions = pAgentContext->pSubscriptionList;
    bool ret = false;

    /* The topic filter length was checked when the SUBSCRIBE command was created. */
    configASSERT( topicFilterLength < MQTT_AGENT_MAX_SUBSCRIPTION_FILTER_LENGTH );

    if( topicFilterLength < MQTT_AGENT_MAX_SUBSCRIPTION_FILTER_LENGTH )
    {
        /* Start at end of array, so that we will insert at the first available index.
         * Scans backwards to find duplicates. */
        for( i = ( int32_t ) MQTT_AGENT_MAX_SIMULTANEOUS_SUBSCRIPTIONS - 1; i >= 0; i-- )
        {
            if( pSubscriptions[ i ].filterStringLength == 0 )
            {
                availableIndex = i;
            }
            else if( ( pSubscriptions[ i ].filterStringLength == topicFilterLength ) &&
                     ( strncmp( topicFilterString, pSubscriptions[ i ].pSubscriptionFilterString, topicFilterLength ) == 0 ) )
            {
                /* If a subscription already exists, don't do anything. */
                if( ( pSubscriptions[ i ].pIncomingPublishCallback == pIncomingPublishCallback ) &&
                    ( pSubscriptions[ i ].pIncomingPublishCallbackContext == pIncomingPublishCallbackContext ) )
                {
                    LogWarn( ( "Subscription already exists.\n" ) );
                    availableIndex = MQTT_AGENT_MAX_SIMULTANEOUS_SUBSCRIPTIONS;
                    ret = true;
                    break;
                }
            }
        }

        if( ( availableIndex < MQTT_AGENT_MAX_SIMULTANEOUS_SUBSCRIPTIONS ) && ( pIncomingPublishCallback != NULL ) )
        {
            pSubscriptions[ availableIndex ].filterStringLength = topicFilterLength;
            pSubscriptions[ availableIndex ].pIncomingPublishCallback = pIncomingPublishCallback;
            pSubscriptions[ availableIndex ].pIncomingPublishCallbackContext = pIncomingPublishCallbackContext;
            memcpy( pSubscriptions[ availableIndex ].pSubscriptionFilterString, topicFilterString, topicFilterLength );
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
    SubscriptionElement_t * pSubscriptions = pAgentContext->pSubscriptionList;

    for( i = 0; i < MQTT_AGENT_MAX_SIMULTANEOUS_SUBSCRIPTIONS; i++ )
    {
        if( pSubscriptions[ i ].filterStringLength == topicFilterLength )
        {
            if( strncmp( pSubscriptions[ i ].pSubscriptionFilterString, topicFilterString, topicFilterLength ) == 0 )
            {
                memset( &( pSubscriptions[ i ] ), 0x00, sizeof( SubscriptionElement_t ) );
            }
        }
    }
}

/*-----------------------------------------------------------*/

static MQTTStatus_t createCommand( CommandType_t commandType,
                                   MQTTContext_t * pMqttContext,
                                   void * pMqttInfoParam,
                                   PublishCallback_t incomingPublishCallback,
                                   void * pIncomingPublishCallbackContext,
                                   CommandCallback_t commandCompleteCallback,
                                   CommandContext_t * pCommandCompleteCallbackContext,
                                   Command_t * pCommand )
{
    bool isValid;
    MQTTStatus_t statusReturn;
    MQTTSubscribeInfo_t * pSubscribeInfo;

    memset( pCommand, 0x00, sizeof( Command_t ) );

    /* Determine if required parameters are present in context. */
    switch( commandType )
    {
        case SUBSCRIBE: /*_RB_ Should not be accepted if MQTT_AGENT_MAX_OUTSTANDING_ACKS has been reached. */
            pSubscribeInfo = ( MQTTSubscribeInfo_t * ) pMqttInfoParam;
            isValid = ( pMqttContext != NULL ) &&
                      ( pMqttInfoParam != NULL ) &&
                      ( pSubscribeInfo->topicFilterLength < MQTT_AGENT_MAX_SUBSCRIPTION_FILTER_LENGTH );
            break;

        case UNSUBSCRIBE:
            isValid = ( pMqttContext != NULL ) && ( pMqttInfoParam != NULL );
            break;

        case PUBLISH: /*_RB_ Should not be accepted if MQTT_AGENT_MAX_OUTSTANDING_ACKS has been reached or if the payload length is greater than MQTT_AGENT_NETWORK_BUFFER_SIZE. */
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

            /* A subscription without a callback is not valid, but create the command
             * anyway in case of a resubscribe. */
            isValid = ( incomingPublishCallback != NULL );
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

    /* Calculate here in case of a SUBSCRIBE. */
    statusReturn = ( isValid ) ? MQTTSuccess : MQTTBadParameter;

    return statusReturn;
}

/*-----------------------------------------------------------*/

static MQTTStatus_t addCommandToQueue( Command_t * pCommand,
                                       uint32_t blockTimeMS )
{
    MQTTStatus_t statusReturn;
    BaseType_t queueStatus;

    /* The application called an API function.  The API function was validated and
     * packed into a Command_t structure.  Now post a reference to the Command_t
     * structure to the MQTT agent for processing. */
    if( commandQueue == NULL )
    {
        statusReturn = MQTTIllegalState;
    }
    else
    {
        queueStatus = xQueueSendToBack( commandQueue, &pCommand, pdMS_TO_TICKS( ( TickType_t ) blockTimeMS ) );

        if( queueStatus != pdFAIL )
        {
            statusReturn = MQTTSuccess;
        }
        else
        {
            statusReturn = MQTTSendFailed;
        }
    }

    return statusReturn;
}

/*-----------------------------------------------------------*/

static MQTTStatus_t processCommand( Command_t * pCommand,
                                    MQTTContext_t ** pOutMqttContext ) //_RB_ Break up into sub-functions.
{
    MQTTStatus_t operationStatus = MQTTSuccess;
    uint16_t packetId = MQTT_PACKET_ID_INVALID;
    bool addAckToList = false, ackAdded = false;
    MQTTPublishInfo_t * pPublishInfo;
    MQTTSubscribeInfo_t * pSubscribeInfo;
    MQTTAgentContext_t * pAgentContext;
    MQTTContext_t * pMQTTContext;
    size_t i;
    const uint32_t processLoopTimeoutMs = 0;
    const size_t maxNewSubscriptionsInOneGo = ( size_t ) 1; /* The agent interface only allows one subscription command at a time. */

    if( pCommand != NULL )
    {
        pMQTTContext = pCommand->pMqttContext;
        *pOutMqttContext = pMQTTContext;

        switch( pCommand->commandType )
        {
            case PUBLISH:
                pPublishInfo = ( MQTTPublishInfo_t * ) &( pCommand->mqttOperationInfo.publishInfo );

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

                for( i = 0; i < MQTT_AGENT_MAX_SIMULTANEOUS_CONNECTIONS; i++ )
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
    for( i = 0; i < MQTT_AGENT_MAX_SIMULTANEOUS_CONNECTIONS; i++ )
    {
        pAgentContext = &( agentContexts[ i ] );

        if( pAgentContext->pMQTTContext != NULL )
        {
            *pOutMqttContext = pAgentContext->pMQTTContext;

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

    return operationStatus; /*_RB_ Need to determine which agent the status came from. */
}

/*-----------------------------------------------------------*/

static void handleIncomingPublish( MQTTAgentContext_t * pAgentContext,
                                   MQTTPublishInfo_t * pPublishInfo )
{
    bool isMatched = false, relayedPublish = false;
    size_t i;
    SubscriptionElement_t * pSubscriptions = pAgentContext->pSubscriptionList;

    for( i = 0; i < MQTT_AGENT_MAX_SIMULTANEOUS_SUBSCRIPTIONS; i++ )
    {
        if( pSubscriptions[ i ].filterStringLength > 0 )
        {
            MQTT_MatchTopic( pPublishInfo->pTopicName,
                             pPublishInfo->topicNameLength,
                             pSubscriptions[ i ].pSubscriptionFilterString,
                             pSubscriptions[ i ].filterStringLength,
                             &isMatched );

            if( isMatched )
            {
                LogDebug( ( "Adding incoming publish callback %.*s\n",
                            pSubscriptions[ i ].filterStringLength,
                            pSubscriptions[ i ].pSubscriptionFilterString ) );
                pSubscriptions[ i ].pIncomingPublishCallback( pPublishInfo,
                                                              pSubscriptions[ i ].pIncomingPublishCallbackContext );
                relayedPublish = true;
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
    bool subscriptionAdded = false;
    MQTTStatus_t subscriptionAddStatus = MQTTSuccess;

    configASSERT( pAckInfo != NULL );

    pAckContext = pAckInfo->pOriginalCommand->pxCmdContext;
    ackCallback = pAckInfo->pOriginalCommand->pCommandCompleteCallback;
    pSubscribeInfo = &( pAckInfo->pOriginalCommand->mqttOperationInfo.subscribeInfo );
    pSubackCodes = pPacketInfo->pRemainingData + 2U; /*_RB_ Where does 2 come from? */
    subscriptionAddStatus = pDeserializedInfo->deserializationResult;

    if( packetType == MQTT_PACKET_TYPE_SUBACK )
    {
        if( *pSubackCodes != MQTTSubAckFailure )
        {
            LogInfo( ( "Adding subscription to %.*s\n",//_RB_ This format specifier is not portable.  Need to do something so topic filters are always terminated.
                       pSubscribeInfo->topicFilterLength,
                       pSubscribeInfo->pTopicFilter ) );
            subscriptionAdded = addSubscription( pAgentContext,
                                                 pSubscribeInfo->pTopicFilter,
                                                 pSubscribeInfo->topicFilterLength,
                                                 pAckInfo->pOriginalCommand->pIncomingPublishCallback,
                                                 pAckInfo->pOriginalCommand->pIncomingPublishCallbackContext );

            /* Not an error if we never asked to add a subscription. */
            if( ( !subscriptionAdded ) && ( pAckInfo->pOriginalCommand->pIncomingPublishCallback != NULL ) )
            {
                /* Set to no memory if there is no space to store the subscription.
                 * We do not break here since there may be existing subscriptions
                 * that can have their QoS updated. */
                subscriptionAddStatus = MQTTNoMemory;
            }
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
        ackCallback( pAckContext, subscriptionAddStatus );
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

        if( ++contextIndex >= MQTT_AGENT_MAX_SIMULTANEOUS_CONNECTIONS )
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

    for( i = 0; i < MQTT_AGENT_MAX_SIMULTANEOUS_CONNECTIONS; i++ )
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

static MQTTStatus_t createAndAddCommand( CommandType_t commandType,
                                         MQTTContextHandle_t mqttContextHandle,
                                         void * pMqttInfoParam,
                                         CommandCallback_t commandCompleteCallback,
                                         CommandContext_t * pCommandCompleteCallbackContext,
                                         PublishCallback_t incomingPublishCallback,
                                         void * pIncomingPublishCallbackContext,
                                         uint32_t blockTimeMS )
{
    MQTTStatus_t statusReturn = MQTTSuccess;
    Command_t * pCommand;

    /* If the packet ID is zero then the MQTT context has not been initialised as 0
     * is the initial value but not a valid packet ID. */
    if( ( mqttContextHandle < MQTT_AGENT_MAX_SIMULTANEOUS_CONNECTIONS ) && ( mqttContexts[ mqttContextHandle ].nextPacketId != 0 ) )
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
            pCommand = getCommandStructureFromPool( blockTimeMS );
        }

        if( pCommand != NULL )
        {
            statusReturn = createCommand( commandType,
                                          &( mqttContexts[ mqttContextHandle ] ),
                                          pMqttInfoParam,
                                          incomingPublishCallback,
                                          pIncomingPublishCallbackContext,
                                          commandCompleteCallback,
                                          pCommandCompleteCallbackContext,
                                          pCommand );

            if( statusReturn == MQTTSuccess )
            {
                statusReturn = addCommandToQueue( pCommand, blockTimeMS );
            }

            if( statusReturn != MQTTSuccess )
            {
                /* Could not send the command to the queue so release the command
                 * structure again. */
                releaseCommandStructureToPool( pCommand );
            }
        }
        else
        {
            /* Ran out of Command_t structures - pool is empty. */
            statusReturn = MQTTNoMemory;
        }
    }

    return statusReturn;
}

/*-----------------------------------------------------------*/

static void resubscribeCallback( void * pResubscribeContext,
                                 MQTTStatus_t subscribeStatus )
{
    // MQTTStatus_t * pResult = ( MQTTStatus_t * ) pResubscribeContext;
    // *pResult = subscribeStatus;
    int32_t i = 0;

    ( void ) pResubscribeContext;

    /* Find first available element and set status. */
    for( i = 0; i < MQTT_AGENT_MAX_SIMULTANEOUS_SUBSCRIPTIONS; i++ )
    {
        /* subscribeStatus can be either MQTTSuccess or MQTTServerRefused.
         * MQTTIllegalState means that it was initialized. */
        if( resubscribeStatuses[ i ] == MQTTIllegalState )
        {
            resubscribeStatuses[ i ] = subscribeStatus;
            break;
        }
    }
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_Init( MQTTContextHandle_t mqttContextHandle,
                             TransportInterface_t * pTransportInterface,
                             MQTTGetCurrentTimeFunc_t getCurrentTimeMs,
                             PublishCallback_t unknownIncomingPublishCallback,
                             void * pDefaultPublishContext )
{
    MQTTStatus_t returnStatus;
    MQTTFixedBuffer_t networkBuffer;
    static uint8_t staticQueueStorageArea[ MQTT_AGENT_COMMAND_QUEUE_LENGTH * sizeof( Command_t * ) ];
    static StaticQueue_t staticQueueStructure;

    /* The command queue should not have been created yet. */
    configASSERT( commandQueue == NULL ); /*_RB_ Needs to change so two contexts can be used in the same agent. */
    commandQueue = xQueueCreateStatic( MQTT_AGENT_COMMAND_QUEUE_LENGTH,
                                       sizeof( Command_t * ),
                                       staticQueueStorageArea,
                                       &staticQueueStructure );

    /*_RB_ Need to make singleton. */

    if( ( mqttContextHandle >= MQTT_AGENT_MAX_SIMULTANEOUS_CONNECTIONS ) ||
        ( pTransportInterface == NULL ) ||
        ( getCurrentTimeMs == NULL ) )
    {
        returnStatus = MQTTBadParameter;
    }
    else
    {
        /* Fill the values for network buffer. */
        networkBuffer.pBuffer = &( networkBuffers[ mqttContextHandle ][ 0 ] );
        networkBuffer.size = MQTT_AGENT_NETWORK_BUFFER_SIZE;

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
            agentContexts[ mqttContextHandle ].pUnsolicitedPublishCallback = unknownIncomingPublishCallback;
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
    Command_t * pCommand;
    MQTTStatus_t operationStatus = MQTTSuccess;
    MQTTContext_t * ret = NULL;
    CommandType_t currentCommandType = NONE;

    /* The command queue should have been created before this task gets created. */
    configASSERT( commandQueue );

    /* Loop until we receive a terminate command. */
    for( ; ; )
    {
        /* Wait for the next command, if any. */
        pCommand = NULL;
        xQueueReceive( commandQueue, &( pCommand ), pdMS_TO_TICKS( MQTT_AGENT_MAX_EVENT_QUEUE_WAIT_TIME ) );
        /* Set the command type in case the command is released while processing. */
        currentCommandType = ( pCommand ) ? pCommand->commandType : NONE;
        operationStatus = processCommand( pCommand, &ret );

        /* Return the current MQTT context if status was not successful. */
        if( operationStatus != MQTTSuccess )
        {
            LogError( ( "MQTT operation failed with status %s\n",
                        MQTT_Status_strerror( operationStatus ) ) );
            break;
        }

        /* Terminate the loop if we receive the termination command. */
        if( currentCommandType == TERMINATE )
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
    MQTTContext_t * pMqttContext;
    MQTTAgentContext_t * pAgentContext;
    AckInfo_t * pendingAcks;
    SubscriptionElement_t * pSubscriptions;
    MQTTPublishInfo_t * originalPublish = NULL;

    /* If the packet ID is zero then the MQTT context has not been initialised as 0
     * is the initial value but not a valid packet ID. */
    if( ( mqttContextHandle < MQTT_AGENT_MAX_SIMULTANEOUS_CONNECTIONS ) && ( mqttContexts[ mqttContextHandle ].nextPacketId != 0 ) )
    {
        pMqttContext = &( mqttContexts[ mqttContextHandle ] );
        pAgentContext = getAgentFromContext( pMqttContext );
        pendingAcks = pAgentContext->pPendingAcks;
        pSubscriptions = pAgentContext->pSubscriptionList;

        /* Resend publishes if session is present. NOTE: It's possible that some
         * of the operations that were in progress during the network interruption
         * were subscribes. In that case, we would want to mark those operations
         * as completing with error and remove them from the list of operations, so
         * that the calling task can try subscribing again. */
        if( sessionPresent )
        {
            MQTTStateCursor_t cursor = MQTT_STATE_CURSOR_INITIALIZER;
            uint16_t packetId = MQTT_PACKET_ID_INVALID;
            AckInfo_t foundAck;

            packetId = MQTT_PublishToResend( pMqttContext, &cursor );

            while( packetId != MQTT_PACKET_ID_INVALID )
            {
                /* Retrieve the operation but do not remove it from the list. */
                foundAck = getAwaitingOperation( pAgentContext, packetId, false );

                if( foundAck.packetId == packetId )
                {
                    /* Set the DUP flag. */
                    originalPublish = &( foundAck.pOriginalCommand->mqttOperationInfo.publishInfo );
                    originalPublish->dup = true;
                    statusResult = MQTT_Publish( pMqttContext, originalPublish, packetId );

                    if( statusResult != MQTTSuccess )
                    {
                        LogError( ( "Error in resending publishes. Error code=%s\n", MQTT_Status_strerror( statusResult ) ) );
                        break;
                    }
                }

                packetId = MQTT_PublishToResend( pMqttContext, &cursor );
            }
        }

        /* If we wanted to resume a session but none existed with the broker, we
         * should mark all in progress operations as errors so that the tasks that
         * created them can try again. Also, we will resubscribe to the filters in
         * the subscription list, so tasks do not unexpectedly lose their subscriptions. */
        else
        {
            size_t i = 0, j = 0;
            static Command_t xNewCommand;
            MQTTContext_t * pErrMqttContext = NULL;

            /* We have a clean session, so clear all operations pending acknowledgments. */
            for( i = 0; i < MQTT_AGENT_MAX_OUTSTANDING_ACKS; i++ )
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

            /* Resubscribe by sending each subscription in a new packet. It's
             * possible there may be repeated subscriptions in the list. This is
             * fine, since clients are able to subscribe to a topic with an
             * existing subscription. */
            for( i = 0; i < MQTT_AGENT_MAX_SIMULTANEOUS_SUBSCRIPTIONS; i++ )
            {
                /* Set the resubscribe status to an error. It should be updated in the callback. */
                resubscribeStatuses[ i ] = MQTTIllegalState;

                if( pSubscriptions[ i ].filterStringLength != 0 )
                {
                    pResendSubscription.pTopicFilter = pSubscriptions[ i ].pSubscriptionFilterString;
                    pResendSubscription.topicFilterLength = pSubscriptions[ i ].filterStringLength;

                    /* We don't know what the original QoS was, so we use QoS 1 since that is the
                    * maximum allowed by AWS IoT, and any QoS 0 publishes will still be QoS 0. */
                    pResendSubscription.qos = MQTTQoS1;

                    /* We cannot add the commmand to the queue since the command loop
                     * should not be running during a reconnect. */
                    statusResult = createCommand( SUBSCRIBE, pMqttContext, &pResendSubscription, NULL, NULL, resubscribeCallback, NULL, &xNewCommand );

                    /* NOTE!! Since every iteration of the loop uses the same command struct,
                     * the subscribe info WILL be overwritten, and the call to addSubscription()
                     * may use a different topic filter than the one corresponding to the SUBACK.
                     * However, this is acceptable for our use case, as
                     *
                     * 1. The broker will have received the correct topic filter in the SUBSCRIBE packet.
                     *
                     * 2. addSubscription() will not change the subscription list, since
                     *    pIncomingPublishCallback is set to NULL. The only functionality
                     *    we require for the resubscription is that resubscribeCallback()
                     *    is invoked. */
                    statusResult = processCommand( &xNewCommand, &pErrMqttContext );
                    j++; /* Keep count of how many subscribes we have sent. */
                }
            }

            /* Resubscribe if needed. */
            if( ( j > 0 ) && ( statusResult == MQTTSuccess ) )
            {
                /* We expect to receive 'j' number of SUBACKs. */
                for( i = 0; i < j; i++ )
                {
                    /* If subscribe was processed but suback was not received, run the process loop. */
                    if( resubscribeStatuses[ i ] != MQTTSuccess )
                    {
                        statusResult = MQTT_ProcessLoop( pMqttContext, MQTT_AGENT_MAX_EVENT_QUEUE_WAIT_TIME );
                    }

                    statusResult = ( statusResult == MQTTSuccess ) ? resubscribeStatuses[ i ] : statusResult;

                    if( statusResult != MQTTSuccess )
                    {
                        LogError( ( "Resubscribe failed with result %s", MQTT_Status_strerror( statusResult ) ) );
                        break;
                    }
                }
            }

            LogInfo( ( "Resubscribe complete with result %s.", MQTT_Status_strerror( statusResult ) ) );
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
    MQTTStatus_t statusReturn;

    if( ( mqttContextHandle < MQTT_AGENT_MAX_SIMULTANEOUS_CONNECTIONS ) &&
        ( mqttContexts[ mqttContextHandle ].connectStatus == MQTTNotConnected ) )
    {
        statusReturn = MQTT_Connect( &( mqttContexts[ mqttContextHandle ] ),
                                     pConnectInfo,
                                     pWillInfo,
                                     timeoutMs,
                                     pSessionPresent );
    }
    else
    {
        statusReturn = MQTTIllegalState;
    }

    return statusReturn;
}

/*-----------------------------------------------------------*/
/*_RB_ Should return MQTTStatus_t. */
MQTTStatus_t MQTTAgent_Subscribe( MQTTContextHandle_t mqttContextHandle,
                                  MQTTSubscribeInfo_t * pSubscriptionInfo,
                                  PublishCallback_t incomingPublishCallback,
                                  void * incomingPublishCallbackContext,
                                  CommandCallback_t commandCompleteCallback,
                                  void * commandCompleteCallbackContext,
                                  uint32_t blockTimeMS )
{
    MQTTStatus_t statusReturn;

    statusReturn = createAndAddCommand( SUBSCRIBE,                      /* commandType */
                                        mqttContextHandle,              /* mqttContextHandle */
                                        pSubscriptionInfo,              /* pMqttInfoParam */
                                        commandCompleteCallback,        /* commandCompleteCallback */
                                        commandCompleteCallbackContext, /* pCommandCompleteCallbackContext */
                                        incomingPublishCallback,        /* incomingPublishCallback */
                                        incomingPublishCallbackContext, /* pIncomingPublishCallbackContext */
                                        blockTimeMS );
    return statusReturn;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_Unsubscribe( MQTTContextHandle_t mqttContextHandle,
                                    MQTTSubscribeInfo_t * pSubscriptionList,
                                    CommandCallback_t cmdCompleteCallback,
                                    CommandContext_t * pCommandCompleteCallbackContext,
                                    uint32_t blockTimeMS )
{
    MQTTStatus_t statusReturn;

    statusReturn = createAndAddCommand( UNSUBSCRIBE,                     /* commandType */
                                        mqttContextHandle,               /* mqttContextHandle */
                                        pSubscriptionList,               /* pMqttInfoParam */
                                        cmdCompleteCallback,             /* commandCompleteCallback */
                                        pCommandCompleteCallbackContext, /* pCommandCompleteCallbackContext */
                                        NULL,                            /* incomingPublishCallback */
                                        NULL,                            /* pIncomingPublishCallbackContext */
                                        blockTimeMS );

    return statusReturn;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_Publish( MQTTContextHandle_t mqttContextHandle,
                                MQTTPublishInfo_t * pPublishInfo,
                                CommandCallback_t commandCompleteCallback,
                                CommandContext_t * commandCompleteCallbackContext,
                                uint32_t blockTimeMS )
{
    MQTTStatus_t statusReturn;

    statusReturn = createAndAddCommand( PUBLISH,                        /* commandType */
                                        mqttContextHandle,              /* mqttContextHandle */
                                        pPublishInfo,                   /* pMqttInfoParam */
                                        commandCompleteCallback,        /* commandCompleteCallback */
                                        commandCompleteCallbackContext, /* pCommandCompleteCallbackContext */
                                        NULL,                           /* incomingPublishCallback */
                                        NULL,                           /* pIncomingPublishCallbackContext */
                                        blockTimeMS );

    return statusReturn;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_TriggerProcessLoop( MQTTContextHandle_t mqttContextHandle,
                                           uint32_t blockTimeMS )
{
    MQTTStatus_t statusReturn;

    statusReturn = createAndAddCommand( PROCESSLOOP,       /* commandType */
                                        mqttContextHandle, /* mqttContextHandle */
                                        NULL,              /* pMqttInfoParam */
                                        NULL,              /* commandCompleteCallback */
                                        NULL,              /* pCommandCompleteCallbackContext */
                                        NULL,              /* incomingPublishCallback */
                                        NULL,              /* pIncomingPublishCallbackContext */
                                        blockTimeMS );

    return statusReturn;
}


/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_Ping( MQTTContextHandle_t mqttContextHandle,
                             CommandCallback_t cmdCompleteCallback,
                             CommandContext_t * pCommandCompleteCallbackContext,
                             uint32_t blockTimeMS )
{
    MQTTStatus_t statusReturn;

    statusReturn = createAndAddCommand( PING,                            /* commandType */
                                        mqttContextHandle,               /* mqttContextHandle */
                                        NULL,                            /* pMqttInfoParam */
                                        cmdCompleteCallback,             /* commandCompleteCallback */
                                        pCommandCompleteCallbackContext, /* pCommandCompleteCallbackContext */
                                        NULL,                            /* incomingPublishCallback */
                                        NULL,                            /* pIncomingPublishCallbackContext */
                                        blockTimeMS );

    return statusReturn;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_Disconnect( MQTTContextHandle_t mqttContextHandle,
                                   CommandCallback_t cmdCompleteCallback,
                                   CommandContext_t * pCommandCompleteCallbackContext,
                                   uint32_t blockTimeMS )
{
    MQTTStatus_t statusReturn;

    statusReturn = createAndAddCommand( DISCONNECT,                      /* commandType */
                                        mqttContextHandle,               /* mqttContextHandle */
                                        NULL,                            /* pMqttInfoParam */
                                        cmdCompleteCallback,             /* commandCompleteCallback */
                                        pCommandCompleteCallbackContext, /* pCommandCompleteCallbackContext */
                                        NULL,                            /* incomingPublishCallback */
                                        NULL,                            /* pIncomingPublishCallbackContext */
                                        blockTimeMS );

    return statusReturn;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_Free( MQTTContextHandle_t mqttContextHandle,
                             CommandCallback_t cmdCompleteCallback,
                             CommandContext_t * pCommandCompleteCallbackContext,
                             uint32_t blockTimeMS )
{
    MQTTStatus_t statusReturn = MQTTSuccess;

    statusReturn = createAndAddCommand( FREE,                            /* commandType */
                                        mqttContextHandle,               /* mqttContextHandle */
                                        NULL,                            /* pMqttInfoParam */
                                        cmdCompleteCallback,             /* commandCompleteCallback */
                                        pCommandCompleteCallbackContext, /* pCommandCompleteCallbackContext */
                                        NULL,                            /* incomingPublishCallback */
                                        NULL,                            /* pIncomingPublishCallbackContext */
                                        blockTimeMS );
    return statusReturn;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_Terminate( uint32_t blockTimeMS )
{
    MQTTStatus_t statusReturn = MQTTSuccess;

    statusReturn = createAndAddCommand( TERMINATE, 0, NULL, NULL, NULL, NULL, NULL, blockTimeMS );

    return statusReturn;
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

/*-----------------------------------------------------------*/
