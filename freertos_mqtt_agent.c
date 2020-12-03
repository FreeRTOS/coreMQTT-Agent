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
    char pSubscriptionFilterString[ MQTT_AGENT_SUBSCRIPTION_BUFFER_SIZE ];
    uint16_t filterStringLength;
    PublishCallback_t pIncomingPublishCallback;
    void * pIncomingPublishCallbackContext;
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
 * @param[in] topicFilter Topic filter string of subscription.
 * @param[in] topicFilterLength Length of topic filter string.
 * @param[in] pIncomingPublishCallback Callback function for the subscription.
 * @param[in] pIncomingPublishCallbackContext Context for the subscription callback.
 *
 * @return `true` if subscription added or exists, `false` if insufficient memory.
 */
static bool addSubscription( MQTTAgentContext_t * pAgentContext,
                             const char * topicFilter,
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
 * @param[in] topicFilter Topic filter of subscription.
 * @param[in] topicFilterLength Length of topic filter.
 */
static void prvRemoveSubscription( MQTTAgentContext_t * pAgentContext,
                                   const char * topicFilter,
                                   uint16_t topicFilterLength );

/**
 * @brief Populate the parameters of a #Command_t
 *
 * @param[in] commandType Type of command.
 * @param[in] pMqttContext Pointer to MQTT context to use for command.
 * @param[in] pMqttInfoParam Pointer to MQTTPublishInfo_t or MQTTSubscribeInfo_t.
 * @param[in] publishCallback Subscription callback function for incomingin publishes.
 * @param[in] pIncomingPublishCallbackContext Subscription callback context.
 * @param[in] pxContext Context and necessary structs for command.
 * @param[in] xCallback Callback for when command completes.
 * @param[out] pCommand Pointer to initialized command.
 *
 * @return `true` if all necessary structs for the command exist in pxContext,
 * else `false`
 */
static bool prvCreateCommand( CommandType_t commandType,
                              MQTTContext_t * pMqttContext,
                              void * pMqttInfoParam,
                              PublishCallback_t publishCallback,
                              void * pIncomingPublishCallbackContext,
                              CommandContext_t * pxContext,
                              CommandCallback_t xCallback,
                              Command_t * pCommand );

/**
 * @brief Add a command to the global command queue.
 *
 * @param[in] pCommand Pointer to command to copy to queue.
 *
 * @return true if the command was added to the queue, else false.
 */
static bool prvAddCommandToQueue( Command_t * pCommand );

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
static MQTTStatus_t prvProcessCommand( Command_t * pCommand );

/**
 * @brief Dispatch an incoming publish to the appropriate publish callback.
 *
 * @param[in] pAgentContext Agent context for the MQTT connection.
 * @param[in] pxPublishInfo Incoming publish information.
 */
static void prvHandleIncomingPublish( MQTTAgentContext_t * pAgentContext,
                                      MQTTPublishInfo_t * pxPublishInfo );

/**
 * @brief Add or delete subscription information from a SUBACK or UNSUBACK.
 *
 * @param[in] pAgentContext Agent context for the MQTT connection.
 * @param[in] pxPacketInfo Pointer to incoming packet.
 * @param[in] pxDeserializedInfo Pointer to deserialized information from
 * the incoming packet.
 * @param[in] pxAckInfo Pointer to stored information for the original subscribe
 * or unsubscribe operation resulting in the received packet.
 * @param[in] ucPacketType The type of the incoming packet, either SUBACK or UNSUBACK.
 */
static void prvHandleSubscriptionAcks( MQTTAgentContext_t * pAgentContext,
                                       MQTTPacketInfo_t * pxPacketInfo,
                                       MQTTDeserializedInfo_t * pxDeserializedInfo,
                                       AckInfo_t * pxAckInfo,
                                       uint8_t ucPacketType );

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
 * @param[in] xMqttContextHandle Handle of the MQTT connection to use.
 * @param[in] pCommandContext Context and necessary structs for command.
 * @param[in] cmdCallback Callback for when command completes.
 * @param[in] pMqttInfoParam Pointer to MQTTPublishInfo_t or MQTTSubscribeInfo_t.
 * @param[in] publishCallback Subscription callback function for incoming publishes.
 * @param[in] pIncomingPublishCallbackContext Subscription callback context.
 *
 * @return `true` if the command was added to the queue, `false` if not.
 */
static bool createAndAddCommand( CommandType_t commandType,
                                 MQTTContextHandle_t xMqttContextHandle,
                                 void * pMqttInfoParam,
                                 CommandCallback_t cmdCallback,
                                 CommandContext_t * pCommandContext,
                                 PublishCallback_t publishCallback,
                                 void * pIncomingPublishCallbackContext );

/*-----------------------------------------------------------*/

/**
 * @brief Queue for main task to handle MQTT operations.
 *
 * This is a private variable initialized when the agent is initialised.
 */
static QueueHandle_t xCommandQueue = NULL;

/**
 * @brief Array of contexts, one for each potential MQTT connection.
 */
static MQTTAgentContext_t xAgentContexts[ MAX_CONNECTIONS ] = { 0 };
static MQTTContext_t xMQTTContexts[ MAX_CONNECTIONS ] = { 0 };

/**
 * @brief The network buffer must remain valid for the lifetime of the MQTT context.
 */
static uint8_t pcNetworkBuffer[ MAX_CONNECTIONS ][ mqttexampleNETWORK_BUFFER_SIZE ];/*_RB_ Need to move and rename constant. Also this requires both buffers to be the same size. */

/*-----------------------------------------------------------*/

//_RB_ Document the below functions and data.
#define MAX_COMMAND_CONTEXTS 10
static Command_t xCommandStructurePool[ MAX_COMMAND_CONTEXTS ];
static SemaphoreHandle_t xFreeCommandStructureMutex = NULL;
static void pxReleaseCommandStructure( Command_t *pxCommandToRelease )
{
    BaseType_t x;

    for( x = 0; x < MAX_COMMAND_CONTEXTS; x++ )
    {
        if( pxCommandToRelease == &( xCommandStructurePool[ x ] ) )
        {
            memset( ( void * ) pxCommandToRelease, 0x00, sizeof( Command_t ) );
            xSemaphoreGive( xFreeCommandStructureMutex );
            LogInfo( ( "Returned Command Context %d to pool", ( int ) x ) );
            break;
        }
    }
}

static Command_t *pxGetCommandStructure( TickType_t xMaxBlockTime )
{
    Command_t *pxReturn = NULL;
    BaseType_t x;

    /* Check counting semaphore has been created. */
    if( xFreeCommandStructureMutex != NULL )
    {
        /* If the semaphore count is not zero then a command context is available. */
        if( xSemaphoreTake( xFreeCommandStructureMutex, xMaxBlockTime ) == pdPASS )
        {
            for( x = 0; x < MAX_COMMAND_CONTEXTS; x++ )
            {
                taskENTER_CRITICAL();
                {
                    if( xCommandStructurePool[ x ].commandType == NONE )
                    {
                        LogInfo( ( "Removed Command Context %d from pool", ( int ) x ) );
                        pxReturn = &( xCommandStructurePool[ x ] );
                        memset( ( void * ) pxReturn, 0x00, sizeof( Command_t ) );
                        pxReturn->commandType = !NONE;
                        taskEXIT_CRITICAL();
                        break;
                    }
                }
                taskEXIT_CRITICAL();
            }
        }
    }

    return pxReturn;
}


static bool addAwaitingOperation( MQTTAgentContext_t * pAgentContext,
                                     uint16_t packetId,
                                     Command_t * pCommand )
{
    size_t i = 0;
    bool xAckAdded = false;
    AckInfo_t * pxPendingAcks = pAgentContext->pPendingAcks;

    for( i = 0; i < PENDING_ACKS_MAX_SIZE; i++ )
    {
        if( pxPendingAcks[ i ].packetId == MQTT_PACKET_ID_INVALID )
        {
            pxPendingAcks[ i ].packetId = packetId;
            pxPendingAcks[ i ].pOriginalCommand = pCommand;
            xAckAdded = true;
            break;
        }
    }

    return xAckAdded;
}

/*-----------------------------------------------------------*/

static AckInfo_t getAwaitingOperation( MQTTAgentContext_t * pAgentContext,
                                          uint16_t incomingPacketId,
                                          bool remove )
{
    size_t i = 0;
    AckInfo_t xFoundAck = { 0 };
    AckInfo_t * pxPendingAcks = pAgentContext->pPendingAcks;

    for( i = 0; i < PENDING_ACKS_MAX_SIZE; i++ )
    {
        if( pxPendingAcks[ i ].packetId == incomingPacketId )
        {
            xFoundAck = pxPendingAcks[ i ];

            if( remove )
            {
                pxPendingAcks[ i ].packetId = MQTT_PACKET_ID_INVALID;
//_RB_ What to do here to return the command.                memset( pxPendingAcks[ i ].pOriginalCommand, 0x00, sizeof( Command_t ) );
            }

            break;
        }
    }

    if( xFoundAck.packetId == MQTT_PACKET_ID_INVALID )
    {
        LogError( ( "No ack found for packet id %u.\n", incomingPacketId ) );
    }

    return xFoundAck;
}

/*-----------------------------------------------------------*/

static bool addSubscription( MQTTAgentContext_t * pAgentContext,
                                const char * topicFilter,
                                uint16_t topicFilterLength,
                                PublishCallback_t pIncomingPublishCallback,
                                void * pIncomingPublishCallbackContext )
{
    int32_t i = 0;
    size_t ulAvailableIndex = SUBSCRIPTIONS_MAX_COUNT;
    SubscriptionElement_t * pxSubscriptions = pAgentContext->pSubscriptionList;
    bool ret = false;

    /* Start at end of array, so that we will insert at the first available index. */
    for( i = ( int32_t ) SUBSCRIPTIONS_MAX_COUNT - 1; i >= 0; i-- )//_RB_ Why is this scanning backward to find the first slot rather than just forwards and breaking when it finds a slot?
    {
        if( pxSubscriptions[ i ].filterStringLength == 0 )
        {
            ulAvailableIndex = i;
        }
        else if( ( pxSubscriptions[ i ].filterStringLength == topicFilterLength ) &&
                 ( strncmp( topicFilter, pxSubscriptions[ i ].pSubscriptionFilterString, topicFilterLength ) == 0 ) )
        {
            /* If a subscription already exists, don't do anything. */
            if( ( pxSubscriptions[ i ].pIncomingPublishCallback == pIncomingPublishCallback ) &&
                ( pxSubscriptions[ i ].pIncomingPublishCallbackContext == pIncomingPublishCallbackContext ) )
            {
                LogWarn( ( "Subscription already exists.\n" ) );
                ulAvailableIndex = SUBSCRIPTIONS_MAX_COUNT;
                ret = true;
                break;
            }
        }
    }

    if( ( ulAvailableIndex < SUBSCRIPTIONS_MAX_COUNT ) && ( pIncomingPublishCallback != NULL ) )
    {
        pxSubscriptions[ ulAvailableIndex ].filterStringLength = topicFilterLength;
        pxSubscriptions[ ulAvailableIndex ].pIncomingPublishCallback = pIncomingPublishCallback;
        pxSubscriptions[ ulAvailableIndex ].pIncomingPublishCallbackContext = pIncomingPublishCallbackContext;
        configASSERT( topicFilterLength < MQTT_AGENT_SUBSCRIPTION_BUFFER_SIZE ); /*_RB_ Too late to catch this here. */
        memcpy( pxSubscriptions[ ulAvailableIndex ].pSubscriptionFilterString, topicFilter, topicFilterLength );
        ret = true;
    }

    return ret;
}

/*-----------------------------------------------------------*/

static void prvRemoveSubscription( MQTTAgentContext_t * pAgentContext,
                                   const char * topicFilter,
                                   uint16_t topicFilterLength )
{
    size_t i = 0;
    SubscriptionElement_t * pxSubscriptions = pAgentContext->pSubscriptionList;

    for( i = 0; i < SUBSCRIPTIONS_MAX_COUNT; i++ )
    {
        if( pxSubscriptions[ i ].filterStringLength == topicFilterLength )
        {
            if( strncmp( pxSubscriptions[ i ].pSubscriptionFilterString, topicFilter, topicFilterLength ) == 0 )
            {
                pxSubscriptions[ i ].filterStringLength = 0;
                pxSubscriptions[ i ].pIncomingPublishCallback = NULL;
                pxSubscriptions[ i ].pIncomingPublishCallbackContext = NULL;
                memset( pxSubscriptions[ i ].pSubscriptionFilterString, 0x00, MQTT_AGENT_SUBSCRIPTION_BUFFER_SIZE );
            }
        }
    }
}

/*-----------------------------------------------------------*/

static bool prvCreateCommand( CommandType_t commandType,
                              MQTTContext_t * pMqttContext,
                              void * pMqttInfoParam,
                              PublishCallback_t publishCallback,
                              void * pIncomingPublishCallbackContext,
                              CommandContext_t * pxContext,
                              CommandCallback_t xCallback,
                              Command_t * pCommand )
{
    bool xIsValid = true;

    memset( pCommand, 0x00, sizeof( Command_t ) );

    /* Determine if required parameters are present in context. */
    switch( commandType )
    {
        case SUBSCRIBE:
            xIsValid = ( pMqttContext != NULL ) && ( pMqttInfoParam != NULL ) && ( publishCallback != NULL );
            break;

        case UNSUBSCRIBE:
            xIsValid = ( pMqttContext != NULL ) && ( pMqttInfoParam != NULL );
            break;

        case PUBLISH:
            xIsValid = ( pMqttContext != NULL ) && ( pMqttInfoParam != NULL );
            break;

        case PROCESSLOOP:
        case PING:
        case DISCONNECT:
        case FREE:
            xIsValid = ( pMqttContext != NULL );
            break;

        default:
            /* Other operations don't need the MQTT context. */
            break;
    }

    if( xIsValid )
    {
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
        pCommand->pIncomingPublishCallback = publishCallback;
        pCommand->pIncomingPublishCallbackContext = pIncomingPublishCallbackContext;
        pCommand->pxCmdContext = pxContext;
        pCommand->pCommandCompleteCallback = xCallback;
    }

    return xIsValid;
}

/*-----------------------------------------------------------*/

static bool prvAddCommandToQueue( Command_t * pCommand )
{
    return xQueueSendToBack( xCommandQueue, &pCommand, MQTT_AGENT_QUEUE_WAIT_TIME );
}

/*-----------------------------------------------------------*/

static MQTTStatus_t prvProcessCommand( Command_t * pCommand )
{
    MQTTStatus_t xStatus = MQTTSuccess;
    uint16_t packetId = MQTT_PACKET_ID_INVALID;
    bool xAddAckToList = false, xAckAdded = false;
    MQTTPublishInfo_t * pxPublishInfo;
    MQTTSubscribeInfo_t * pxSubscribeInfo;
    MQTTContext_t * pMQTTContext = pCommand->pMqttContext;
    MQTTAgentContext_t * pAgentContext = NULL;
    uint32_t i;
    uint32_t processLoopTimeoutMs = MQTT_AGENT_PROCESS_LOOP_TIMEOUT_MS;
    const size_t xMaxCount = ( size_t ) 1; /* The agent interface only allows one subscription command at a time. */

    switch( pCommand->commandType )
    {
        case PUBLISH:
            pxPublishInfo = ( MQTTPublishInfo_t * )  &( pCommand->mqttOperationInfo.publishInfo );

            if( pxPublishInfo->qos != MQTTQoS0 )
            {
                packetId = MQTT_GetPacketId( pMQTTContext );
            }

            LogDebug( ( "Publishing message to %.*s.\n", ( int ) pxPublishInfo->topicNameLength, pxPublishInfo->pTopicName ) );
            xStatus = MQTT_Publish( pMQTTContext, pxPublishInfo, packetId );

            /* Add to pending ack list, or call callback if QoS 0. */
            xAddAckToList = ( pxPublishInfo->qos != MQTTQoS0 ) && ( xStatus == MQTTSuccess );
            break;

        case SUBSCRIBE:
        case UNSUBSCRIBE:
            pxSubscribeInfo = ( MQTTSubscribeInfo_t * ) &( pCommand->mqttOperationInfo.subscribeInfo );
            configASSERT( pxSubscribeInfo->pTopicFilter != NULL );
            packetId = MQTT_GetPacketId( pMQTTContext );

            if( pCommand->commandType == SUBSCRIBE )
            {
                /* Even if some subscriptions already exist in the subscription list,
                 * it is fine to send another subscription request. A valid use case
                 * for this is changing the maximum QoS of the subscription. */
                xStatus = MQTT_Subscribe( pMQTTContext,
                                          pxSubscribeInfo,
                                          xMaxCount,
                                          packetId );
            }
            else
            {
                xStatus = MQTT_Unsubscribe( pMQTTContext,
                                            pxSubscribeInfo,
                                            xMaxCount,
                                            packetId );
            }

            xAddAckToList = ( xStatus == MQTTSuccess );
            break;

        case PING:
            xStatus = MQTT_Ping( pMQTTContext );

            break;

        case DISCONNECT:
            xStatus = MQTT_Disconnect( pMQTTContext );

            break;

        case FREE:

            for( i = 0; i < MAX_CONNECTIONS; i++ )
            {
                if( xAgentContexts[ i ].pMQTTContext == pMQTTContext )
                {
                    memset( &xAgentContexts[ i ], 0x00, sizeof( MQTTAgentContext_t ) );
                    break;
                }
            }

            break;

        case TERMINATE:
            LogInfo( ( "Terminating command loop.\n" ) );

        default:
            break;
    }

    if( xAddAckToList )
    {
        pAgentContext = getAgentFromContext( pCommand->pMqttContext );
        xAckAdded = addAwaitingOperation( pAgentContext, packetId, pCommand );

        /* Set the return status if no memory was available to store the operation
         * information. */
        if( !xAckAdded )
        {
            LogError( ( "No memory to wait for acknowledgment for packet %u\n", packetId ) );

            /* All operations that can wait for acks (publish, subscribe, unsubscribe)
             * require a context. */
            xStatus = MQTTNoMemory;//_RB_ Should the command structure be returned here?
        }
    }
    else
    {
        //_RB_ Should the command structure be freed here?
    }

    if( !xAckAdded )
    {
        /* The command is complete, call the callback. */
        if( pCommand->pCommandCompleteCallback != NULL )
        {
            pCommand->pCommandCompleteCallback( pCommand->pxCmdContext, xStatus );
        }

        pxReleaseCommandStructure( pCommand );
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
    if( ( xStatus == MQTTSuccess ) && ( pMQTTContext != NULL ) && ( pMQTTContext->connectStatus == MQTTConnected ) )
    {
        xStatus = MQTT_ProcessLoop( pMQTTContext, processLoopTimeoutMs );
    }

    return xStatus;
}

/*-----------------------------------------------------------*/

static void prvHandleIncomingPublish( MQTTAgentContext_t * pAgentContext,
                                      MQTTPublishInfo_t * pxPublishInfo )
{
    bool xIsMatched = false, xRelayedPublish = false;
    MQTTStatus_t xStatus;
    size_t i;
    SubscriptionElement_t * pxSubscriptions = pAgentContext->pSubscriptionList;

    configASSERT( pxPublishInfo != NULL );

    for( i = 0; i < SUBSCRIPTIONS_MAX_COUNT; i++ )
    {
        if( pxSubscriptions[ i ].filterStringLength > 0 )
        {
            xStatus = MQTT_MatchTopic( pxPublishInfo->pTopicName,
                                       pxPublishInfo->topicNameLength,
                                       pxSubscriptions[ i ].pSubscriptionFilterString,
                                       pxSubscriptions[ i ].filterStringLength,
                                       &xIsMatched );
            /* The call can't fail if the topic name and filter is valid. */
            configASSERT( xStatus == MQTTSuccess );

            if( xIsMatched )
            {
                LogDebug( ( "Adding publish to response queue for %.*s\n",
                            pxSubscriptions[ i ].filterStringLength,
                            pxSubscriptions[ i ].pSubscriptionFilterString ) );
                pxSubscriptions[ i ].pIncomingPublishCallback( pxPublishInfo, pxSubscriptions[ i ].pIncomingPublishCallbackContext );
                xRelayedPublish = true;
            }
        }
    }

    /* It is possible a publish was sent on an unsubscribed topic. This is
     * possible on topics reserved by the broker, e.g. those beginning with
     * '$'. In this case, we copy the publish to a queue we configured to
     * receive these publishes. */
    if( !xRelayedPublish )
    {
        LogWarn( ( "Publish received on topic %.*s with no subscription.\n",
                   pxPublishInfo->topicNameLength,
                   pxPublishInfo->pTopicName ) );

        if( pAgentContext->pUnsolicitedPublishCallback != NULL )
        {
            pAgentContext->pUnsolicitedPublishCallback( pxPublishInfo, pAgentContext->pUnsolicitedPublishCallbackContext );
        }
    }
}

/*-----------------------------------------------------------*/

static void prvHandleSubscriptionAcks( MQTTAgentContext_t * pAgentContext,
                                       MQTTPacketInfo_t * pxPacketInfo,
                                       MQTTDeserializedInfo_t * pxDeserializedInfo,
                                       AckInfo_t * pxAckInfo,
                                       uint8_t ucPacketType )
{
    CommandContext_t * pxAckContext = NULL;
    CommandCallback_t vAckCallback = NULL;
    uint8_t * pcSubackCodes = NULL;
    MQTTSubscribeInfo_t * pxSubscribeInfo = NULL;

    configASSERT( pxAckInfo != NULL );

    pxAckContext = pxAckInfo->pOriginalCommand->pxCmdContext;
    vAckCallback = pxAckInfo->pOriginalCommand->pCommandCompleteCallback;
    pxSubscribeInfo = &( pxAckInfo->pOriginalCommand->mqttOperationInfo.subscribeInfo );
    pcSubackCodes = pxPacketInfo->pRemainingData + 2U;

    if( ucPacketType == MQTT_PACKET_TYPE_SUBACK )
    {
        if( *pcSubackCodes != MQTTSubAckFailure )
        {
            LogInfo( ( "Adding subscription to %.*s\n",//_RB_ This format specifier is not portable..
                        pxSubscribeInfo->topicFilterLength,
                        pxSubscribeInfo->pTopicFilter ) );
            addSubscription( pAgentContext,
                                pxSubscribeInfo->pTopicFilter,
                                pxSubscribeInfo->topicFilterLength,
                                pxAckInfo->pOriginalCommand->pIncomingPublishCallback,
                                pxAckInfo->pOriginalCommand->pIncomingPublishCallbackContext );
        }
        else
        {
            LogError( ( "Subscription to %.*s failed.\n",
                        pxSubscribeInfo->topicFilterLength,
                        pxSubscribeInfo->pTopicFilter ) );
        }
    }
    else
    {
        LogInfo( ( "Removing subscription to %.*s\n",
                    pxSubscribeInfo->topicFilterLength,
                    pxSubscribeInfo->pTopicFilter ) );
        prvRemoveSubscription( pAgentContext,
                                pxSubscribeInfo->pTopicFilter,
                                pxSubscribeInfo->topicFilterLength );
    }

    if( vAckCallback != NULL )
    {
        vAckCallback( pxAckContext, pxDeserializedInfo->deserializationResult );
    }

    pxReleaseCommandStructure( pxAckInfo->pOriginalCommand ); //_RB_ Is this always the right place for this?
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
        ret = xAgentContexts[ contextIndex ].pMQTTContext;

        if( ++contextIndex >= MAX_CONNECTIONS )
        {
            contextIndex = 0U;
        }
    } while( ( ret == NULL ) && ( oldIndex != contextIndex ) );

    return ret;
}

/*-----------------------------------------------------------*/

static MQTTAgentContext_t * getAgentFromContext( MQTTContext_t * pMQTTContext ) //_RB_ Doesn't work unless there is only one MQTTContext_t object per connection.  Will need to do this by handle somehow.
{
    MQTTAgentContext_t * ret = NULL;
    int i = 0;

    configASSERT( pMQTTContext );

    for( i = 0; i < MAX_CONNECTIONS; i++ )
    {
        if( xAgentContexts[ i ].pMQTTContext == pMQTTContext )
        {
            ret = &xAgentContexts[ i ];
            break;
        }
    }

    return ret;
}

/*-----------------------------------------------------------*/

void MQTTAgent_EventCallback( MQTTContext_t * pMqttContext,
                              MQTTPacketInfo_t * pPacketInfo,
                              MQTTDeserializedInfo_t * pDeserializedInfo )
{
    configASSERT( pMqttContext != NULL );
    configASSERT( pPacketInfo != NULL );
    AckInfo_t xAckInfo;
    uint16_t packetIdentifier = pDeserializedInfo->packetIdentifier;
    CommandCallback_t vAckCallback = NULL;
    MQTTAgentContext_t * pAgentContext = getAgentFromContext( pMqttContext );

    /* Handle incoming publish. The lower 4 bits of the publish packet
     * type is used for the dup, QoS, and retain flags. Hence masking
     * out the lower bits to check if the packet is publish. */
    if( ( pPacketInfo->type & 0xF0U ) == MQTT_PACKET_TYPE_PUBLISH )
    {
        prvHandleIncomingPublish( pAgentContext, pDeserializedInfo->pPublishInfo );
    }
    else
    {
        /* Handle other packets. */
        switch( pPacketInfo->type )
        {
            case MQTT_PACKET_TYPE_PUBACK:
            case MQTT_PACKET_TYPE_PUBCOMP:
                xAckInfo = getAwaitingOperation( pAgentContext, packetIdentifier, true );

                if( xAckInfo.packetId == packetIdentifier )
                {
                    vAckCallback = xAckInfo.pOriginalCommand->pCommandCompleteCallback;

                    if( vAckCallback != NULL )
                    {
                        vAckCallback( xAckInfo.pOriginalCommand->pxCmdContext, pDeserializedInfo->deserializationResult );
                    }
                }
                pxReleaseCommandStructure( xAckInfo.pOriginalCommand ); //_RB_ Is this always the right place for this?
                break;

            case MQTT_PACKET_TYPE_SUBACK:
            case MQTT_PACKET_TYPE_UNSUBACK:
                xAckInfo = getAwaitingOperation( pAgentContext, packetIdentifier, true );

                if( xAckInfo.packetId == packetIdentifier )
                {
                    prvHandleSubscriptionAcks( pAgentContext, pPacketInfo, pDeserializedInfo, &xAckInfo, pPacketInfo->type );
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
/*_RB_ Temporary until all APIs use the handle rather than the pointer. */
MQTTContext_t * MQTTAgent_GetMQTTContext( MQTTContextHandle_t xMQTTContextHandle )
{
    MQTTContext_t *pxReturn;

    if( xMQTTContextHandle < MAX_CONNECTIONS )
    {
        pxReturn = &( xMQTTContexts[ xMQTTContextHandle ] );
    }
    else
    {
        pxReturn = NULL;
    }

    return pxReturn;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_Init( MQTTContextHandle_t xMQTTContextHandle,
                             TransportInterface_t *pxTransportInterface,
                             MQTTGetCurrentTimeFunc_t prvGetTimeMs,
                             PublishCallback_t vUnkownIncomingPublishCallback,
                             void * pDefaultPublishContext )
{
    MQTTStatus_t xReturn;
    MQTTFixedBuffer_t xNetworkBuffer;
    static uint8_t ucQueueStorageArea[ MQTT_AGENT_COMMAND_QUEUE_LENGTH * sizeof( Command_t * ) ];
    static StaticQueue_t xStaticQueue;

    /* The command queue should not have been created yet. */
    configASSERT( xCommandQueue == NULL );
    xCommandQueue = xQueueCreateStatic(  MQTT_AGENT_COMMAND_QUEUE_LENGTH,
                                         sizeof( Command_t * ),
                                         ucQueueStorageArea,
                                         &xStaticQueue );

    /*_RB_ Need to make singleton. */

    if( ( xMQTTContextHandle >= MAX_CONNECTIONS ) ||
        ( pxTransportInterface == NULL ) ||
        ( prvGetTimeMs == NULL ) )
    {
        xReturn = MQTTBadParameter;
    }
    else
    {
        /* Fill the values for network buffer. */
        xNetworkBuffer.pBuffer = &( pcNetworkBuffer[ xMQTTContextHandle ][ 0 ] );
        xNetworkBuffer.size = mqttexampleNETWORK_BUFFER_SIZE;

        xReturn = MQTT_Init( &( xMQTTContexts[ xMQTTContextHandle ] ),
                             pxTransportInterface,
                             prvGetTimeMs,
                             MQTTAgent_EventCallback,
                             &xNetworkBuffer );

        if( xReturn == MQTTSuccess )
        {
            /* Also initialise the agent context.  Assert if already initialised. */
            configASSERT( xAgentContexts[ xMQTTContextHandle ].pMQTTContext == NULL );
            xAgentContexts[ xMQTTContextHandle ].pMQTTContext = &( xMQTTContexts[ xMQTTContextHandle ] );
            xAgentContexts[ xMQTTContextHandle ].pUnsolicitedPublishCallback = vUnkownIncomingPublishCallback;
            xAgentContexts[ xMQTTContextHandle ].pUnsolicitedPublishCallbackContext = pDefaultPublishContext;

            memset( ( void * ) xCommandStructurePool, 0x00, sizeof( xCommandStructurePool ) );
            xFreeCommandStructureMutex = xSemaphoreCreateCounting( MAX_COMMAND_CONTEXTS, MAX_COMMAND_CONTEXTS );
            configASSERT( xFreeCommandStructureMutex ); /*_RB_ Create all objects here statically. */
        }
    }

    return xReturn;
}


/*-----------------------------------------------------------*/

MQTTContext_t * MQTTAgent_CommandLoop( void )
{
    Command_t *pCommand;
    MQTTStatus_t xStatus = MQTTSuccess;
    static int lNumProcessed = 0;
    MQTTContext_t * ret = NULL;

    /* The command queue should have been created before this task gets created. */
    configASSERT( xCommandQueue );

    /* Loop until we receive a terminate command. */
    for( ; ; )
    {
        /* If there is no command in the queue, try again. */
        if( xQueueReceive( xCommandQueue, &( pCommand ), MQTT_AGENT_QUEUE_WAIT_TIME ) != pdFALSE )
        {
            /* Keep a count of processed operations, for debug logs. */
            lNumProcessed++;
        }

        xStatus = prvProcessCommand( pCommand );

        /* Return the current MQTT context if status was not successful. */
        if( xStatus != MQTTSuccess )
        {
            LogError( ( "MQTT operation failed with status %s\n",
                        MQTT_Status_strerror( xStatus ) ) );
            ret = pCommand->pMqttContext;
            break;
        }

        /* Terminate the loop if we receive the termination command. */
        if( pCommand->commandType == TERMINATE )
        {
            ret = NULL;
            break;
        }

        LogDebug( ( "Processed %d operations.", lNumProcessed ) );
    }

    return ret;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_ResumeSession( MQTTContext_t * pMqttContext,
                                      bool xSessionPresent )
{
    MQTTStatus_t xResult = MQTTSuccess;
    MQTTAgentContext_t * pAgentContext = getAgentFromContext( pMqttContext );
    AckInfo_t * pxPendingAcks = pAgentContext->pPendingAcks;
    SubscriptionElement_t * pxSubscriptions = pAgentContext->pSubscriptionList;
    MQTTSubscribeInfo_t * pxResendSubscriptions = pAgentContext->pResendSubscriptions;
    MQTTPublishInfo_t * pxOriginalPublish = NULL;

    /* Resend publishes if session is present. NOTE: It's possible that some
     * of the operations that were in progress during the network interruption
     * were subscribes. In that case, we would want to mark those operations
     * as completing with error and remove them from the list of operations, so
     * that the calling task can try subscribing again. We do not handle that
     * case in this demo for simplicity, since only one subscription packet is
     * sent per iteration of this demo. */
    if( xSessionPresent )
    {
        MQTTStateCursor_t cursor = MQTT_STATE_CURSOR_INITIALIZER;
        uint16_t packetId = MQTT_PACKET_ID_INVALID;
        AckInfo_t xFoundAck;

        packetId = MQTT_PublishToResend( pAgentContext->pMQTTContext, &cursor );

        while( packetId != MQTT_PACKET_ID_INVALID )
        {
            /* Retrieve the operation but do not remove it from the list. */
            xFoundAck = getAwaitingOperation( pAgentContext, packetId, false );

            if( xFoundAck.packetId == packetId )
            {
                /* Set the DUP flag. */
                pxOriginalPublish = &( xFoundAck.pOriginalCommand->mqttOperationInfo.publishInfo );
                pxOriginalPublish->dup = true;
                xResult = MQTT_Publish( pAgentContext->pMQTTContext, pxOriginalPublish, packetId );

                if( xResult != MQTTSuccess )
                {
                    LogError( ( "Error in resending publishes. Error code=%s\n", MQTT_Status_strerror( xResult ) ) );
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
            if( pxPendingAcks[ i ].packetId != MQTT_PACKET_ID_INVALID )
            {
                if( pxPendingAcks[ i ].pOriginalCommand->pCommandCompleteCallback != NULL )
                {
                    /* Bad response to indicate network error. */
                    pxPendingAcks[ i ].pOriginalCommand->pCommandCompleteCallback( pxPendingAcks[ i ].pOriginalCommand->pxCmdContext, MQTTBadResponse );
                }

                /* Now remove it from the list. */
                getAwaitingOperation( pAgentContext, pxPendingAcks[ i ].packetId, true );
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
//_RB_ removed j below           xCommandCreated = prvCreateCommand( SUBSCRIBE, pMqttContext, pxResendSubscriptions, j, NULL, NULL, NULL, NULL, &xNewCommand );
            xCommandCreated = prvCreateCommand( SUBSCRIBE, pMqttContext, pxResendSubscriptions, NULL, NULL, NULL, NULL, &xNewCommand );
            configASSERT( xCommandCreated == true );
//_RB_            xNewCommand.uintParam = j;
            xNewCommand.pIncomingPublishCallbackContext = NULL;
            /* Send to the front of the queue so we will resubscribe as soon as possible. */
            xCommandAdded = xQueueSendToFront( xCommandQueue, &xNewCommand, MQTT_AGENT_QUEUE_WAIT_TIME );
            configASSERT( xCommandAdded == pdTRUE );
        }
    }

    return xResult;
}

/*-----------------------------------------------------------*/
//_RB_ Should return an MQTTStatus_t.
static bool createAndAddCommand( CommandType_t commandType,
                                 MQTTContextHandle_t xMqttContextHandle,
                                 void * pMqttInfoParam,
                                 CommandCallback_t cmdCallback,
                                 CommandContext_t * pCommandContext,
                                 PublishCallback_t publishCallback,
                                 void * pIncomingPublishCallbackContext )
{
    bool ret = false;
    const TickType_t xBlockTimeMS = ( TickType_t ) 500;/*_RB_ Could make a parameter. */
    Command_t *pCommand = NULL;

    /* If the packet ID is zero then the MQTT context has not been initialised as 0
     * is the initial value but not a valid packet ID. */
    if( ( xMqttContextHandle < MAX_CONNECTIONS ) && ( xMQTTContexts[ xMqttContextHandle ].nextPacketId != 0 ) )
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
            pCommand = pxGetCommandStructure( pdMS_TO_TICKS( xBlockTimeMS ) );
        }

        if( pCommand != NULL )
        {
            ret = prvCreateCommand( commandType,
                                    &( xMQTTContexts[ xMqttContextHandle ] ),
                                    pMqttInfoParam,
                                    publishCallback,
                                    pIncomingPublishCallbackContext,
                                    pCommandContext,
                                    cmdCallback,
                                    pCommand );

            if( ret )
            {
                ret = prvAddCommandToQueue( pCommand );
            }

            if( ret == false )
            {
                /* Could not send the command to the queue so release the command
                 * structure again. */
                pxReleaseCommandStructure( pCommand );
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
/*_RB_ Should return MQTTStatus_t. */
bool MQTTAgent_Subscribe( MQTTContextHandle_t mqttContextHandle,
                          MQTTSubscribeInfo_t * pSubscriptionInfo,
                          PublishCallback_t incomingPublishCallback,
                          void * incomingPublishCallbackContext,
                          CommandCallback_t commandCompleteCallback,
                          void * commandCompleteCallbackContext )
{
    return createAndAddCommand( SUBSCRIBE,
                                mqttContextHandle,
                                pSubscriptionInfo,
                                commandCompleteCallback,
                                commandCompleteCallbackContext,
                                incomingPublishCallback,
                                incomingPublishCallbackContext );
}

/*-----------------------------------------------------------*/

bool MQTTAgent_Unsubscribe( MQTTContextHandle_t mqttContextHandle,
                            MQTTSubscribeInfo_t * pSubscriptionList,
                            CommandContext_t * pCommandContext,
                            CommandCallback_t cmdCallback )
{
    return createAndAddCommand( UNSUBSCRIBE, mqttContextHandle, pSubscriptionList, cmdCallback, pCommandContext, NULL, NULL );
}

/*-----------------------------------------------------------*/

bool MQTTAgent_Publish( MQTTContextHandle_t mqttContextHandle,
                        MQTTPublishInfo_t * pPublishInfo,
                        CommandCallback_t commandCompleteCallback,
                        CommandContext_t * commandCompleteCallbackContext )
{
    return createAndAddCommand( PUBLISH,
                                mqttContextHandle,
                                pPublishInfo,
                                commandCompleteCallback,
                                commandCompleteCallbackContext,
                                NULL,
                                NULL );
}

/*-----------------------------------------------------------*/

bool MQTTAgent_ProcessLoop( MQTTContextHandle_t mqttContextHandle,
                            CommandContext_t * pCommandContext,
                            CommandCallback_t cmdCallback )
{
    return createAndAddCommand( PROCESSLOOP, mqttContextHandle, NULL, cmdCallback, pCommandContext, NULL, NULL );
}


/*-----------------------------------------------------------*/

bool MQTTAgent_Ping( MQTTContextHandle_t mqttContextHandle,
                     CommandContext_t * pCommandContext,
                     CommandCallback_t cmdCallback )
{
    return createAndAddCommand( PING, mqttContextHandle, NULL, cmdCallback, pCommandContext, NULL, NULL );
}

/*-----------------------------------------------------------*/

bool MQTTAgent_Disconnect( MQTTContextHandle_t mqttContextHandle,
                           CommandContext_t * pCommandContext,
                           CommandCallback_t cmdCallback )
{
    return createAndAddCommand( DISCONNECT, mqttContextHandle, NULL, cmdCallback, pCommandContext, NULL, NULL );
}

/*-----------------------------------------------------------*/

bool MQTTAgent_Free( MQTTContextHandle_t mqttContextHandle,
                     CommandContext_t * pCommandContext,
                     CommandCallback_t cmdCallback )
{
    return createAndAddCommand( FREE, mqttContextHandle, NULL, cmdCallback, pCommandContext, NULL, NULL );
}

/*-----------------------------------------------------------*/

bool MQTTAgent_Terminate( void )
{
    return createAndAddCommand( TERMINATE, 0, NULL, NULL, NULL, NULL, NULL );
}

/*-----------------------------------------------------------*/

uint32_t MQTTAgent_GetNumWaiting( void )
{
    return uxQueueMessagesWaiting( xCommandQueue );
}

/*-----------------------------------------------------------*/

