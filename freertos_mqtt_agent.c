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
 * so doing, serializes all access to coreMQTT even when multiple tasks are using the
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
#include <assert.h>

/* MQTT agent include. */
#include "freertos_mqtt_agent.h"
#include "agent_command_pool.h"

/*-----------------------------------------------------------*/

#if 0

/**
 * @brief An element in the list of subscriptions maintained by the agent.
 *
 * @note The agent allows multiple tasks to subscribe to the same topic.
 * In this case, another element is added to the subscription list, differing
 * in the intended publish callback.
 */
typedef struct subscriptionElement
{
    IncomingPublishCallback_t pIncomingPublishCallback;
    void * pIncomingPublishCallbackContext;
    uint16_t filterStringLength;
    char pSubscriptionFilterString[ MQTT_AGENT_MAX_SUBSCRIPTION_FILTER_LENGTH ];
} SubscriptionElement_t;
#endif

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

#if 0

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
                             IncomingPublishCallback_t pIncomingPublishCallback,
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

#endif

/**
 * @brief Populate the parameters of a #Command_t
 *
 * @param[in] commandType Type of command.  For example, publish or subscribe.
 * @param[in] pMqttAgentContext Pointer to MQTT context to use for command.
 * @param[in] pMqttInfoParam Pointer to MQTTPublishInfo_t or MQTTSubscribeInfo_t.
 * @param[in] commandCompleteCallback Callback for when command completes.
 * @param[in] pCommandCompleteCallbackContext Context and necessary structs for command.
 * @param[out] pCommand Pointer to initialized command.
 *
 * @return `MQTTSuccess` if all necessary fields for the command are passed,
 * else an enumerated error code.
 */
static MQTTStatus_t createCommand( CommandType_t commandType,
                                   MQTTAgentContext_t * pMqttAgentContext,
                                   void * pMqttInfoParam,
                                   CommandCallback_t commandCompleteCallback,
                                   CommandContext_t * pCommandCompleteCallbackContext,
                                   Command_t * pCommand );

/**
 * @brief Add a command to the global command queue.
 *
 * @param[in] pQueue Queue to which to add command.
 * @param[in] pCommand Pointer to command to copy to queue.
 * @param[in] blockTimeMs The maximum amount of time to milliseconds to wait in the
 * Blocked state (so not consuming any CPU time) for the command to be posted to the
 * queue should the queue already be full.
 *
 * @return MQTTSuccess if the command was added to the queue, else an enumerated
 * error code.
 */
static MQTTStatus_t addCommandToQueue( AgentQueue_t * pQueue,
                                       Command_t * pCommand,
                                       uint32_t blockTimeMs );

/**
 * @brief Process a #Command_t.
 *
 * @note This agent does not check existing subscriptions before sending a
 * SUBSCRIBE or UNSUBSCRIBE packet. If a subscription already exists, then
 * a SUBSCRIBE packet will be sent anyway, and if multiple tasks are subscribed
 * to a topic filter, then they will all be unsubscribed after an UNSUBSCRIBE.
 *
 * @param[in] pMqttAgentContext Agent context for MQTT connection.
 * @param[in] pCommand Pointer to command to process.
 *
 * @return status of MQTT library API call.
 */
static MQTTStatus_t processCommand( MQTTAgentContext_t * pMqttAgentContext,
                                    Command_t * pCommand );

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
 * @brief Retrieve a pointer to an agent context given an MQTT context.
 *
 * @param[in] pMQTTContext MQTT Context to search for.
 *
 * @return Pointer to agent context, or NULL.
 */
static MQTTAgentContext_t * getAgentFromMQTTContext( MQTTContext_t * pMQTTContext );

/**
 * @brief Helper function for creating a command and adding it to the command
 * queue.
 *
 * @param[in] commandType Type of command.
 * @param[in] pMqttAgentContext Handle of the MQTT connection to use.
 * @param[in] pCommandCompleteCallbackContext Context and necessary structs for command.
 * @param[in] cmdCompleteCallback Callback for when command completes.
 * @param[in] pMqttInfoParam Pointer to MQTTPublishInfo_t or MQTTSubscribeInfo_t.
 * @param[in] blockTimeMs Maximum amount of time in milliseconds to wait (in the
 * Blocked state, so not consuming any CPU time) for the command to be posted to the
 * MQTT agent should the MQTT agent's event queue be full.
 *
 * @return MQTTSuccess if the command was posted to the MQTT agent's event queue.
 * Otherwise an enumerated error code.
 */
static MQTTStatus_t createAndAddCommand( CommandType_t commandType,
                                         MQTTAgentContext_t * pMqttAgentContext,
                                         void * pMqttInfoParam,
                                         CommandCallback_t cmdCompleteCallback,
                                         CommandContext_t * pCommandCompleteCallbackContext,
                                         uint32_t blockTimeMs );

/**
 * @brief Called before accepting any PUBLISH or SUBSCRIBE messages to check
 * there is space in the pending ACK list for the outgoing PUBLISH or SUBSCRIBE.
 *
 * @note Because the MQTT agent is inherently multi threaded, and this function
 * is called from the context of the application task and not the MQTT agent
 * task, this function can only return a best effort result.  It can definitely
 * say if there is space for a new pending ACK when the function is called, but
 * the case of space being exhausted when the agent executes a command that
 * results in an ACK must still be handled.
 *
 * @param[in] pAgentContext Pointer to the context for the MQTT connection to
 * which the PUBLISH or SUBSCRIBE message is to be sent.
 *
 * @return true if there is space in that MQTT connection's ACK list, otherwise
 * false;
 */
static bool isSpaceInPendingAckList( MQTTAgentContext_t * pAgentContext );
/*-----------------------------------------------------------*/

/**
 * @brief Flag that is set to true in the application callback to let the agent know
 * that calling MQTT_ProcessLoop() resulted in events on the connected socket.  If
 * the flag gets set to true then MQTT_ProcessLoop() is called again as there may be
 * more received data waiting to be processed.
 */
static bool packetProcessedDuringLoop = false;

/*-----------------------------------------------------------*/

static bool isSpaceInPendingAckList( MQTTAgentContext_t * pAgentContext )
{
    AckInfo_t * pendingAcks;
    bool spaceFound = false;
    size_t i;

    if( pAgentContext != NULL )
    {
        pendingAcks = pAgentContext->pPendingAcks;

        /* Are there any open slots? */
        for( i = 0; i < MQTT_AGENT_MAX_OUTSTANDING_ACKS; i++ )
        {
            /* If the packetId is MQTT_PACKET_ID_INVALID then the array space is
             * not in use. */
            if( pendingAcks[ i ].packetId == MQTT_PACKET_ID_INVALID )
            {
                spaceFound = true;
                break;
            }
        }
    }

    return spaceFound;
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
     * find one with incomingPacketId. */
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

#if 0

static bool addSubscription( MQTTAgentContext_t * pAgentContext,
                             const char * topicFilterString,
                             uint16_t topicFilterLength,
                             IncomingPublishCallback_t pIncomingPublishCallback,
                             void * pIncomingPublishCallbackContext )
{
    int32_t i = 0;
    size_t availableIndex = MQTT_AGENT_MAX_SIMULTANEOUS_SUBSCRIPTIONS;
    SubscriptionElement_t * pSubscriptions = pAgentContext->pSubscriptionList;
    bool ret = false;

    /* The topic filter length was checked when the SUBSCRIBE command was created. */
    assert( topicFilterLength < MQTT_AGENT_MAX_SUBSCRIPTION_FILTER_LENGTH );

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

#endif

/*-----------------------------------------------------------*/
/*_RB_ Requires refactoring to reduce complexity. */
static MQTTStatus_t createCommand( CommandType_t commandType,
                                   MQTTAgentContext_t * pMqttAgentContext,
                                   void * pMqttInfoParam,
                                   CommandCallback_t commandCompleteCallback,
                                   CommandContext_t * pCommandCompleteCallbackContext,
                                   Command_t * pCommand )
{
    bool isValid, isSpace = true;
    MQTTStatus_t statusReturn;
    MQTTSubscribeInfo_t * pSubscribeInfo;
    MQTTPublishInfo_t * pPublishInfo;
    size_t uxHeaderBytes;
    const size_t uxControlAndLengthBytes = ( size_t ) 4; /* Control, remaining length and length bytes. */

    memset( pCommand, 0x00, sizeof( Command_t ) );

    /* Determine if required parameters are present in context. */
    switch( commandType )
    {
        case SUBSCRIBE:
            /* This message type results in the broker returning an ACK.  The
             * agent maintains an array of outstanding ACK messages.  See if
             * the array contains space for another outstanding ack. */
            isSpace = isSpaceInPendingAckList( pMqttAgentContext );

            pSubscribeInfo = ( MQTTSubscribeInfo_t * ) pMqttInfoParam;
            isValid = ( pSubscribeInfo != NULL ) &&
                      ( pMqttAgentContext != NULL ) &&
                      ( isSpace == true );

            break;

        case UNSUBSCRIBE:
            /* This message type results in the broker returning an ACK.  The
             * agent maintains an array of outstanding ACK messages.  See if
             * the array contains space for another outstanding ack. */
            isSpace = isSpaceInPendingAckList( pMqttAgentContext );

            isValid = ( pMqttAgentContext != NULL ) &&
                      ( pMqttInfoParam != NULL ) &&
                      ( isSpace == true );
            break;

        case PUBLISH:
            pPublishInfo = ( MQTTPublishInfo_t * ) pMqttInfoParam;

            /* Calculate the space consumed by everything other than the
             * payload. */
            if( pPublishInfo == NULL )
            {
                isValid = false;
            }
            else
            {
                uxHeaderBytes = uxControlAndLengthBytes;
                uxHeaderBytes += pPublishInfo->topicNameLength;

                /* This message type results in the broker returning an ACK. The
                 * agent maintains an array of outstanding ACK messages.  See if
                 * the array contains space for another outstanding ack.  QoS0
                 * publish does not result in an ack so it doesn't matter if
                 * there is no space in the ACK array. */
                if( pPublishInfo->qos != MQTTQoS0 )
                {
                    isSpace = isSpaceInPendingAckList( pMqttAgentContext );
                }

                isValid = ( pMqttAgentContext != NULL ) &&
                          /* Will the message fit in the defined buffer? */
                          ( ( pPublishInfo->payloadLength + uxHeaderBytes ) < pMqttAgentContext->mqttContext.networkBuffer.size ) &&
                          ( isSpace == true );
            }
            break;

        case PROCESSLOOP:
        case PING:
        case CONNECT:
        case DISCONNECT:
            isValid = ( pMqttAgentContext != NULL );
            break;

        default:
            /* Other operations don't need the MQTT context. */
            isValid = true;
            break;
    }

    if( isValid )
    {

        pCommand->commandType = commandType;
        pCommand->pArgs = pMqttInfoParam;
        pCommand->pCmdContext = pCommandCompleteCallbackContext;
        pCommand->pCommandCompleteCallback = commandCompleteCallback;
    }

    /* Calculate here in case of a SUBSCRIBE. */
    statusReturn = ( isValid ) ? MQTTSuccess : MQTTBadParameter;

    if( ( statusReturn == MQTTBadParameter ) && ( isSpace == false ) )
    {
        /* The error was caused not by a bad parameter, but because there was
         * no room in the pending Ack list for the Ack response to an outgoing
         * PUBLISH or SUBSCRIBE message. */
        statusReturn = MQTTNoMemory;
    }

    return statusReturn;
}

/*-----------------------------------------------------------*/

static MQTTStatus_t addCommandToQueue( AgentQueue_t * pQueue,
                                       Command_t * pCommand,
                                       uint32_t blockTimeMs )
{
    MQTTStatus_t statusReturn = MQTTIllegalState;
    bool queueStatus;

    /* The application called an API function.  The API function was validated and
     * packed into a Command_t structure.  Now post a reference to the Command_t
     * structure to the MQTT agent for processing. */
    if( pQueue != NULL )
    {
        queueStatus = Agent_QueueSend( pQueue, &pCommand, blockTimeMs );

        statusReturn = ( queueStatus ) ? MQTTSuccess : MQTTSendFailed;
    }

    return statusReturn;
}

/*-----------------------------------------------------------*/

static MQTTStatus_t processCommand( MQTTAgentContext_t * pMqttAgentContext,
                                    Command_t * pCommand ) //_RB_ Break up into sub-functions.
{
    MQTTStatus_t operationStatus = MQTTSuccess;
    uint16_t packetId = MQTT_PACKET_ID_INVALID;
    bool addAckToList = false, ackAdded = false;
    MQTTPublishInfo_t * pPublishInfo;
    MQTTSubscribeInfo_t * pSubscribeInfo;
    MQTTContext_t * pMQTTContext;
    bool runProcessLoops = true;
    const uint32_t processLoopTimeoutMs = 0;
    const size_t maxNewSubscriptionsInOneGo = ( size_t ) 1; /* The agent interface only allows one subscription command at a time. */
    MQTTAgentReturnInfo_t returnInfo = { 0 };

    assert( pMqttAgentContext != NULL );

    pMQTTContext = &( pMqttAgentContext->mqttContext );

    if( pCommand != NULL )
    {
        switch( pCommand->commandType )
        {
            case PUBLISH:
                pPublishInfo = ( MQTTPublishInfo_t * ) ( pCommand->pArgs );

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
                pSubscribeInfo = ( MQTTSubscribeInfo_t * ) ( pCommand->pArgs );
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

            case CONNECT:
                MQTTAgentConnectArgs_t * pConnectArgs = ( MQTTAgentConnectArgs_t * ) ( pCommand->pArgs );
                operationStatus = MQTT_Connect( pMQTTContext,
                                                pConnectArgs->pConnectInfo,
                                                pConnectArgs->pWillInfo,
                                                pConnectArgs->timeoutMs,
                                                &( pConnectArgs->sessionPresent ) );
                break;

            case DISCONNECT:
                operationStatus = MQTT_Disconnect( pMQTTContext );
                runProcessLoops = false;

                break;

            case TERMINATE:
                LogInfo( ( "Terminating command loop.\n" ) );
                runProcessLoops = false;

            default:
                break;
        }

        if( addAckToList )
        {
            ackAdded = addAwaitingOperation( pMqttAgentContext, packetId, pCommand );

            /* Set the return status if no memory was available to store the operation
             * information. */
            if( !ackAdded )
            {
                LogError( ( "No memory to wait for acknowledgment for packet %u\n", packetId ) );

                /* All operations that can wait for acks (publish, subscribe,
                 * unsubscribe) require a context. */
                operationStatus = MQTTNoMemory;
            }
        }

        if( !ackAdded )
        {
            /* The command is complete, call the callback. */
            if( pCommand->pCommandCompleteCallback != NULL )
            {
                returnInfo.returnCode = operationStatus;
                pCommand->pCommandCompleteCallback( pCommand->pCmdContext, &returnInfo );
            }

            Agent_ReleaseCommand( pCommand );
        }
    }

    /* Don't run process loops if there was an error or disconnect. */
    runProcessLoops = ( operationStatus != MQTTSuccess ) ? false : runProcessLoops;

    /* Run the process loop if there were no errors and the MQTT connection
     * still exists. */
    if( runProcessLoops )
    {
        do
        {
            packetProcessedDuringLoop = false;

            if( ( operationStatus == MQTTSuccess ) &&
                ( pMQTTContext->connectStatus == MQTTConnected ) )
            {
                operationStatus = MQTT_ProcessLoop( pMQTTContext, processLoopTimeoutMs );
            }
        } while( packetProcessedDuringLoop );
    }

    return operationStatus;
}

/*-----------------------------------------------------------*/
#if 0
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
                   pPublishInfo->pTopicName ) );/*_RB_ Remove these non portable format specifiers. */

        if( pAgentContext->pUnsolicitedPublishCallback != NULL )
        {
            pAgentContext->pUnsolicitedPublishCallback( pPublishInfo,
                                                        pAgentContext->pUnsolicitedPublishCallbackContext );
        }
    }
}
#endif
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
    MQTTStatus_t subscriptionAddStatus = MQTTSuccess;
    MQTTAgentReturnInfo_t returnInfo = { 0 };

    assert( pAckInfo != NULL );

    pAckContext = pAckInfo->pOriginalCommand->pCmdContext;
    ackCallback = pAckInfo->pOriginalCommand->pCommandCompleteCallback;
    pSubackCodes = pPacketInfo->pRemainingData + 2U; /*_RB_ Where does 2 come from? */
    subscriptionAddStatus = pDeserializedInfo->deserializationResult;

#if 0

    bool subscriptionAdded = false;
    MQTTSubscribeInfo_t * pSubscribeInfo = ( MQTTSubscribeInfo_t * ) ( pAckInfo->pOriginalCommand->pArgs );

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

#endif

    if( ackCallback != NULL )
    {
        returnInfo.returnCode = subscriptionAddStatus;
        returnInfo.pSubackCodes = pSubackCodes;
        ackCallback( pAckContext, &returnInfo );
    }

    Agent_ReleaseCommand( pAckInfo->pOriginalCommand ); //_RB_ Is this always the right place for this?
}

/*-----------------------------------------------------------*/

static MQTTAgentContext_t * getAgentFromMQTTContext( MQTTContext_t * pMQTTContext )
{
    MQTTAgentContext_t * ret = ( MQTTAgentContext_t * ) pMQTTContext;

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
    MQTTAgentReturnInfo_t returnInfo = { 0 };
    const uint8_t upperNibble = ( uint8_t ) 0xF0;

    assert( pMqttContext != NULL );
    assert( pPacketInfo != NULL );

    pAgentContext = getAgentFromMQTTContext( pMqttContext );

    /* This callback executes from within MQTT_ProcessLoop().  Setting this flag
     * indicates that the callback executed so the caller of MQTT_ProcessLoop() knows
     * it should call it again as there may be more data to process. */
    packetProcessedDuringLoop = true;

    /* Handle incoming publish. The lower 4 bits of the publish packet type is used
     * for the dup, QoS, and retain flags. Hence masking out the lower bits to check
     * if the packet is publish. */
    if( ( pPacketInfo->type & upperNibble ) == MQTT_PACKET_TYPE_PUBLISH )
    {
        pAgentContext->pIncomingCallback( pAgentContext, packetIdentifier, pDeserializedInfo->pPublishInfo );
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
                        returnInfo.returnCode = pDeserializedInfo->deserializationResult;
                        ackCallback( ackInfo.pOriginalCommand->pCmdContext,
                                     &returnInfo );
                    }
                }

                Agent_ReleaseCommand( ackInfo.pOriginalCommand ); //_RB_ Is this always the right place for this?
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
                                         MQTTAgentContext_t * pMqttAgentContext,
                                         void * pMqttInfoParam,
                                         CommandCallback_t commandCompleteCallback,
                                         CommandContext_t * pCommandCompleteCallbackContext,
                                         uint32_t blockTimeMs )
{
    MQTTStatus_t statusReturn = MQTTSuccess;
    Command_t * pCommand;

    /* If the packet ID is zero then the MQTT context has not been initialised as 0
     * is the initial value but not a valid packet ID. */
    if( pMqttAgentContext->mqttContext.nextPacketId != 0 )
    {
        pCommand = Agent_GetCommand( blockTimeMs );

        if( pCommand != NULL )
        {
            statusReturn = createCommand( commandType,
                                          pMqttAgentContext,
                                          pMqttInfoParam,
                                          commandCompleteCallback,
                                          pCommandCompleteCallbackContext,
                                          pCommand );

            if( statusReturn == MQTTSuccess )
            {
                statusReturn = addCommandToQueue( pMqttAgentContext->commandQueue, pCommand, blockTimeMs );
            }

            if( statusReturn != MQTTSuccess )
            {
                /* Could not send the command to the queue so release the command
                 * structure again. */
                Agent_ReleaseCommand( pCommand );
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

MQTTStatus_t MQTTAgent_Init( MQTTAgentContext_t * pMqttAgentContext,
                             AgentQueue_t * pAgentQueue,
                             MQTTFixedBuffer_t * pNetworkBuffer,
                             TransportInterface_t * pTransportInterface,
                             MQTTGetCurrentTimeFunc_t getCurrentTimeMs,
                             IncomingPublishCallback_t incomingCallback,
                             void * pIncomingPacketContext )
{
    MQTTStatus_t returnStatus;

    if( ( pMqttAgentContext == NULL ) ||
        ( pAgentQueue == NULL ) ||
        ( pTransportInterface == NULL ) ||
        ( getCurrentTimeMs == NULL ) )
    {
        returnStatus = MQTTBadParameter;
    }
    else
    {
        memset( pMqttAgentContext, 0x00, sizeof( MQTTAgentContext_t ) );

        returnStatus = MQTT_Init( &( pMqttAgentContext->mqttContext ),
                                  pTransportInterface,
                                  getCurrentTimeMs,
                                  mqttEventCallback,
                                  pNetworkBuffer );

        if( returnStatus == MQTTSuccess )
        {
            pMqttAgentContext->pIncomingCallback = incomingCallback;
            pMqttAgentContext->pIncomingCallbackContext = pIncomingPacketContext;
            pMqttAgentContext->commandQueue = pAgentQueue;
        }
    }

    return returnStatus;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_CommandLoop( MQTTAgentContext_t * pMqttAgentContext )
{
    Command_t * pCommand;
    MQTTStatus_t operationStatus = MQTTSuccess;
    CommandType_t currentCommandType = NONE;

    /* The command queue should have been created before this task gets created. */
    assert( pMqttAgentContext->commandQueue );

    if( pMqttAgentContext == NULL )
    {
        operationStatus = MQTTBadParameter;
    }

    /* Loop until an error or we receive a terminate command. */
    while( operationStatus == MQTTSuccess )
    {
        /* Wait for the next command, if any. */
        pCommand = NULL;
        ( void ) Agent_QueueReceive( pMqttAgentContext->commandQueue, &( pCommand ), MQTT_AGENT_MAX_EVENT_QUEUE_WAIT_TIME );
        /* Set the command type in case the command is released while processing. */
        currentCommandType = ( pCommand ) ? pCommand->commandType : NONE;
        operationStatus = processCommand( pMqttAgentContext, pCommand );

        /* Return the current MQTT context on disconnect or error. */
        if( ( currentCommandType == DISCONNECT ) || ( operationStatus != MQTTSuccess ) )
        {
            if( operationStatus != MQTTSuccess )
            {
                LogError( ( "MQTT operation failed with status %s\n",
                            MQTT_Status_strerror( operationStatus ) ) );
            }
            break;
        }

        /* Terminate the loop if we receive the termination command. */
        if( currentCommandType == TERMINATE )
        {
            break;
        }
    }

    return operationStatus;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_ResumeSession( MQTTAgentContext_t * pMqttAgentContext,
                                      bool sessionPresent ) /*_RB_ Need to test this function. */
{
    MQTTStatus_t statusResult = MQTTSuccess;
    MQTTContext_t * pMqttContext;
    MQTTAgentContext_t * pAgentContext;
    AckInfo_t * pendingAcks;
    MQTTPublishInfo_t * originalPublish = NULL;

    /* If the packet ID is zero then the MQTT context has not been initialised as 0
     * is the initial value but not a valid packet ID. */
    if( ( pMqttAgentContext != NULL ) && ( pMqttAgentContext->mqttContext.nextPacketId != 0 ) )
    {
        pMqttContext = &( pMqttAgentContext->mqttContext );
        pAgentContext = pMqttAgentContext;
        pendingAcks = pAgentContext->pPendingAcks;

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
                foundAck = getAwaitingOperation( pMqttAgentContext, packetId, false );

                if( foundAck.packetId == packetId )
                {
                    /* Set the DUP flag. */
                    originalPublish = ( MQTTPublishInfo_t * ) ( foundAck.pOriginalCommand->pArgs );
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
            size_t i = 0;
            MQTTAgentReturnInfo_t returnInfo = { 0 };
            returnInfo.returnCode = MQTTBadResponse;

            /* We have a clean session, so clear all operations pending acknowledgments. */
            for( i = 0; i < MQTT_AGENT_MAX_OUTSTANDING_ACKS; i++ )
            {
                if( pendingAcks[ i ].packetId != MQTT_PACKET_ID_INVALID )
                {
                    if( pendingAcks[ i ].pOriginalCommand->pCommandCompleteCallback != NULL )
                    {
                        /* Bad response to indicate network error. */
                        pendingAcks[ i ].pOriginalCommand->pCommandCompleteCallback( pendingAcks[ i ].pOriginalCommand->pCmdContext, &returnInfo );
                    }

                    /* Now remove it from the list. */
                    getAwaitingOperation( pMqttAgentContext, pendingAcks[ i ].packetId, true );
                }
            }

#if 0
            size_t j = 0;
            SubscriptionElement_t * pSubscriptions;

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

                    /* We cannot add the command to the queue since the command loop
                     * should not be running during a reconnect. */
                    statusResult = createCommand( SUBSCRIBE, pMqttAgentContext, &pResendSubscription, resubscribeCallback, NULL, &xResubscribeCommand );

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
                     *    is invoked. 
                     */
                    statusResult = processCommand( pMqttAgentContext, &xResubscribeCommand );
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
#endif
        }
    }
    else
    {
        statusResult = MQTTIllegalState;
    }

    return statusResult;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_Subscribe( MQTTAgentContext_t * pMqttAgentContext,
                                  MQTTAgentSubscribeArgs_t * pSubscriptionArgs,
                                  CommandInfo_t * pCommandInfo )
{
    MQTTStatus_t statusReturn;

    statusReturn = createAndAddCommand( SUBSCRIBE,                      /* commandType */
                                        pMqttAgentContext,              /* mqttContextHandle */
                                        pSubscriptionArgs,              /* pMqttInfoParam */
                                        pCommandInfo->cmdCompleteCallback,        /* commandCompleteCallback */
                                        pCommandInfo->pCmdCompleteCallbackContext, /* pCommandCompleteCallbackContext */
                                        pCommandInfo->blockTimeMs );
    return statusReturn;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_Unsubscribe( MQTTAgentContext_t * pMqttAgentContext,
                                    MQTTAgentSubscribeArgs_t * pSubscriptionArgs,
                                    CommandInfo_t * pCommandInfo )
{
    MQTTStatus_t statusReturn;

    statusReturn = createAndAddCommand( UNSUBSCRIBE,                     /* commandType */
                                        pMqttAgentContext,               /* mqttContextHandle */
                                        pSubscriptionArgs,               /* pMqttInfoParam */
                                        pCommandInfo->cmdCompleteCallback,             /* commandCompleteCallback */
                                        pCommandInfo->pCmdCompleteCallbackContext, /* pCommandCompleteCallbackContext */
                                        pCommandInfo->blockTimeMs );

    return statusReturn;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_Publish( MQTTAgentContext_t * pMqttAgentContext,
                                MQTTPublishInfo_t * pPublishInfo,
                                CommandInfo_t * pCommandInfo )
{
    MQTTStatus_t statusReturn;

    statusReturn = createAndAddCommand( PUBLISH,                        /* commandType */
                                        pMqttAgentContext,              /* mqttContextHandle */
                                        pPublishInfo,                   /* pMqttInfoParam */
                                        pCommandInfo->cmdCompleteCallback,        /* commandCompleteCallback */
                                        pCommandInfo->pCmdCompleteCallbackContext, /* pCommandCompleteCallbackContext */
                                        pCommandInfo->blockTimeMs );

    return statusReturn;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_TriggerProcessLoop( MQTTAgentContext_t * pMqttAgentContext,
                                           uint32_t blockTimeMs )
{
    MQTTStatus_t statusReturn;

    statusReturn = createAndAddCommand( PROCESSLOOP,       /* commandType */
                                        pMqttAgentContext, /* mqttContextHandle */
                                        NULL,              /* pMqttInfoParam */
                                        NULL,              /* commandCompleteCallback */
                                        NULL,              /* pCommandCompleteCallbackContext */
                                        blockTimeMs );

    return statusReturn;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_Connect( MQTTAgentContext_t * pMqttAgentContext,
                                MQTTAgentConnectArgs_t * pConnectArgs,
                                CommandInfo_t * pCommandInfo )
{
    MQTTStatus_t statusReturn;
    statusReturn = createAndAddCommand( CONNECT,
                                        pMqttAgentContext,
                                        pConnectArgs,
                                        pCommandInfo->cmdCompleteCallback,
                                        pCommandInfo->pCmdCompleteCallbackContext,
                                        pCommandInfo->blockTimeMs );

    return statusReturn;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_Disconnect( MQTTAgentContext_t * pMqttAgentContext,
                                   CommandInfo_t * pCommandInfo )
{
    MQTTStatus_t statusReturn;

    statusReturn = createAndAddCommand( DISCONNECT,                        /* commandType */
                                        pMqttAgentContext,              /* mqttContextHandle */
                                        NULL,                   /* pMqttInfoParam */
                                        pCommandInfo->cmdCompleteCallback,        /* commandCompleteCallback */
                                        pCommandInfo->pCmdCompleteCallbackContext, /* pCommandCompleteCallbackContext */
                                        pCommandInfo->blockTimeMs );

    return statusReturn;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_Ping( MQTTAgentContext_t * pMqttAgentContext,
                             CommandInfo_t * pCommandInfo )
{
    MQTTStatus_t statusReturn;

    statusReturn = createAndAddCommand( PING,                        /* commandType */
                                        pMqttAgentContext,              /* mqttContextHandle */
                                        NULL,                   /* pMqttInfoParam */
                                        pCommandInfo->cmdCompleteCallback,        /* commandCompleteCallback */
                                        pCommandInfo->pCmdCompleteCallbackContext, /* pCommandCompleteCallbackContext */
                                        pCommandInfo->blockTimeMs );

    return statusReturn;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_Terminate( MQTTAgentContext_t * pMqttAgentContext,
                                  CommandInfo_t * pCommandInfo )
{
    MQTTStatus_t statusReturn = MQTTSuccess;

    statusReturn = createAndAddCommand( TERMINATE,
                                        pMqttAgentContext,
                                        NULL,
                                        pCommandInfo->cmdCompleteCallback,
                                        pCommandInfo->pCmdCompleteCallbackContext,
                                        pCommandInfo->blockTimeMs );

    return statusReturn;
}

/*-----------------------------------------------------------*/
