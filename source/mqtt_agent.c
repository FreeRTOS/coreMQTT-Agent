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
 */

/* Standard includes. */
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* MQTT agent include. */
#include "mqtt_agent.h"
#include "mqtt_agent_command_functions.h"

/*-----------------------------------------------------------*/

const MQTTAgentCommandFunc_t pCommandFunctionTable[ NUM_COMMANDS ] = MQTT_AGENT_FUNCTION_TABLE;

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
 * @param[in] incomingPacketId Packet ID of incoming ack.
 *
 * @return Pointer to stored information about the operation awaiting the ack.
 * Returns NULL if the packet ID is zero or original command does not exist.
 */
static AckInfo_t * getAwaitingOperation( MQTTAgentContext_t * pAgentContext,
                                         uint16_t incomingPacketId );

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
 * @param[in] pAgentContext Agent context for the MQTT connection.
 * @param[in] pCommand Pointer to command to copy to queue.
 * @param[in] blockTimeMs The maximum amount of time to milliseconds to wait in the
 * Blocked state (so not consuming any CPU time) for the command to be posted to the
 * queue should the queue already be full.
 *
 * @return MQTTSuccess if the command was added to the queue, else an enumerated
 * error code.
 */
static MQTTStatus_t addCommandToQueue( MQTTAgentContext_t * pAgentContext,
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
 * @param[out] pEndLoop Whether the command loop should terminate.
 *
 * @return status of MQTT library API call.
 */
static MQTTStatus_t processCommand( MQTTAgentContext_t * pMqttAgentContext,
                                    Command_t * pCommand,
                                    bool * pEndLoop );

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
 * @brief Mark a command as complete after receiving an acknowledgment packet.
 *
 * @param[in] pAgentContext Agent context for the MQTT connection.
 * @param[in] pPacketInfo Pointer to incoming packet.
 * @param[in] pDeserializedInfo Pointer to deserialized information from
 * the incoming packet.
 * @param[in] pAckInfo Pointer to stored information for the original operation
 * resulting in the received packet.
 * @param[in] packetType The type of the incoming packet, either SUBACK, UNSUBACK,
 * PUBACK, or PUBCOMP.
 */
static void handleAcks( MQTTAgentContext_t * pAgentContext,
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
 * @param[in] pMqttInfoParam Pointer to command argument.
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
 * @brief Resend QoS 1 and 2 publishes after resuming a session.
 *
 * @param[in] pMqttAgentContext Agent context for the MQTT connection.
 *
 * @return MQTTSuccess if all publishes resent successfully, else error code
 * from #MQTT_Publish.
 */
static MQTTStatus_t resendPublishes( MQTTAgentContext_t * pMqttAgentContext );

/**
 * @brief Clear the list of pending acknowledgments by invoking each callback
 * with #MQTTBadParameter.
 *
 * @param[in] pMqttAgentContext Agent context of the MQTT connection.
 */
static void clearPendingAcknowledgments( MQTTAgentContext_t * pMqttAgentContext );

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

static AckInfo_t * getAwaitingOperation( MQTTAgentContext_t * pAgentContext,
                                         uint16_t incomingPacketId )
{
    size_t i = 0;
    AckInfo_t * pFoundAck = NULL;

    assert( pAgentContext != NULL );

    /* Look through the array of packet IDs that are still waiting to be acked to
     * find one with incomingPacketId. */
    for( i = 0; i < MQTT_AGENT_MAX_OUTSTANDING_ACKS; i++ )
    {
        if( pAgentContext->pPendingAcks[ i ].packetId == incomingPacketId )
        {
            pFoundAck = &( pAgentContext->pPendingAcks[ i ] );
            break;
        }
    }

    if( pFoundAck == NULL )
    {
        LogError( ( "No ack found for packet id %u.\n", incomingPacketId ) );
    }
    else if( ( pFoundAck->pOriginalCommand == NULL ) || ( pFoundAck->packetId == 0U ) )
    {
        LogError( ( "Found ack had empty fields. PacketId=%hu, Original Command=%p",
                    ( unsigned short ) pFoundAck->packetId,
                    ( void * ) pFoundAck->pOriginalCommand ) );
        pFoundAck = NULL;
    }

    return pFoundAck;
}

/*-----------------------------------------------------------*/

static MQTTStatus_t createCommand( CommandType_t commandType,
                                   MQTTAgentContext_t * pMqttAgentContext,
                                   void * pMqttInfoParam,
                                   CommandCallback_t commandCompleteCallback,
                                   CommandContext_t * pCommandCompleteCallbackContext,
                                   Command_t * pCommand )
{
    bool isValid, isSpace = true;
    MQTTStatus_t statusReturn;
    MQTTAgentSubscribeArgs_t * pSubscribeArgs;
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

            pSubscribeArgs = ( MQTTAgentSubscribeArgs_t * ) pMqttInfoParam;
            isValid = ( pSubscribeArgs != NULL ) &&
                      ( pSubscribeArgs->pSubscribeInfo != NULL ) &&
                      ( pMqttAgentContext != NULL ) &&
                      ( isSpace == true );

            break;

        case UNSUBSCRIBE:

            /* This message type results in the broker returning an ACK.  The
             * agent maintains an array of outstanding ACK messages.  See if
             * the array contains space for another outstanding ack. */
            isSpace = isSpaceInPendingAckList( pMqttAgentContext );

            pSubscribeArgs = ( MQTTAgentSubscribeArgs_t * ) pMqttInfoParam;
            isValid = ( pMqttAgentContext != NULL ) &&
                      ( pSubscribeArgs != NULL ) &&
                      ( pSubscribeArgs->pSubscribeInfo != NULL ) &&
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

static MQTTStatus_t addCommandToQueue( MQTTAgentContext_t * pAgentContext,
                                       Command_t * pCommand,
                                       uint32_t blockTimeMs )
{
    MQTTStatus_t statusReturn = MQTTIllegalState;
    bool queueStatus;

    /* The application called an API function.  The API function was validated and
     * packed into a Command_t structure.  Now post a reference to the Command_t
     * structure to the MQTT agent for processing. */
    if( pAgentContext != NULL )
    {
        queueStatus = pAgentContext->agentInterface.send(
            pAgentContext->agentInterface.pMsgCtx,
            &pCommand,
            blockTimeMs
            );

        statusReturn = ( queueStatus ) ? MQTTSuccess : MQTTSendFailed;
    }

    return statusReturn;
}

/*-----------------------------------------------------------*/

static MQTTStatus_t processCommand( MQTTAgentContext_t * pMqttAgentContext,
                                    Command_t * pCommand,
                                    bool * pEndLoop )
{
    MQTTStatus_t operationStatus = MQTTSuccess;
    bool ackAdded = false;
    MQTTAgentReturnInfo_t returnInfo = { 0 };
    MQTTAgentCommandFunc_t commandFunction = NULL;
    void * pCommandArgs = NULL;
    const uint32_t processLoopTimeoutMs = 0U;
    uint8_t commandOutFlags = 0U;
    MQTTAgentCommandFuncReturns_t commandOutParams = { 0 };

    assert( pMqttAgentContext != NULL );
    assert( pEndLoop != NULL );

    if( pCommand != NULL )
    {
        assert( pCommand->commandType < NUM_COMMANDS );
        commandFunction = pCommandFunctionTable[ pCommand->commandType ];
        pCommandArgs = pCommand->pArgs;
    }
    else
    {
        commandFunction = pCommandFunctionTable[ NONE ];
    }

    operationStatus = commandFunction( pMqttAgentContext, pCommandArgs, &commandOutParams );

    if( commandOutParams.addAcknowledgment )
    {
        ackAdded = addAwaitingOperation( pMqttAgentContext, commandOutParams.packetId, pCommand );

        /* Set the return status if no memory was available to store the operation
         * information. */
        if( !ackAdded )
        {
            LogError( ( "No memory to wait for acknowledgment for packet %u\n", commandOutParams.packetId ) );

            /* All operations that can wait for acks (publish, subscribe,
             * unsubscribe) require a context. */
            operationStatus = MQTTNoMemory;
        }
    }

    if( ( pCommand != NULL ) && ( ackAdded != true ) )
    {
        /* The command is complete, call the callback. */
        if( pCommand->pCommandCompleteCallback != NULL )
        {
            returnInfo.returnCode = operationStatus;
            pCommand->pCommandCompleteCallback( pCommand->pCmdContext, &returnInfo );
        }

        pMqttAgentContext->agentInterface.releaseCommand( pCommand );
    }

    /* Run the process loop if there were no errors and the MQTT connection
     * still exists. */
    if( ( operationStatus == MQTTSuccess ) && commandOutParams.runProcessLoop )
    {
        do
        {
            pMqttAgentContext->packetReceivedInLoop = false;

            if( ( operationStatus == MQTTSuccess ) &&
                ( pMqttAgentContext->mqttContext.connectStatus == MQTTConnected ) )
            {
                operationStatus = MQTT_ProcessLoop( &( pMqttAgentContext->mqttContext ), processLoopTimeoutMs );
            }
        } while( pMqttAgentContext->packetReceivedInLoop );
    }

    /* Set the flag to break from the command loop. */
    *pEndLoop = ( commandOutParams.endLoop || ( operationStatus != MQTTSuccess ) );

    return operationStatus;
}

/*-----------------------------------------------------------*/

static void handleAcks( MQTTAgentContext_t * pAgentContext,
                        MQTTPacketInfo_t * pPacketInfo,
                        MQTTDeserializedInfo_t * pDeserializedInfo,
                        AckInfo_t * pAckInfo,
                        uint8_t packetType )
{
    CommandContext_t * pAckContext = NULL;
    CommandCallback_t ackCallback = NULL;
    uint8_t * pSubackCodes = NULL;
    MQTTAgentReturnInfo_t returnInfo = { 0 };

    assert( pAckInfo != NULL );
    assert( pAckInfo->pOriginalCommand != NULL );

    pAckContext = pAckInfo->pOriginalCommand->pCmdContext;
    ackCallback = pAckInfo->pOriginalCommand->pCommandCompleteCallback;
    /* A SUBACK's status codes start 2 bytes after the variable header. */
    pSubackCodes = ( packetType == MQTT_PACKET_TYPE_SUBACK ) ? pPacketInfo->pRemainingData + 2U : NULL;

    if( ackCallback != NULL )
    {
        returnInfo.returnCode = pDeserializedInfo->deserializationResult;
        returnInfo.pSubackCodes = pSubackCodes;
        ackCallback( pAckContext, &returnInfo );
    }

    pAgentContext->agentInterface.releaseCommand( pAckInfo->pOriginalCommand );
    /* Clear the entry from the list. */
    memset( pAckInfo, 0x00, sizeof( AckInfo_t ) );
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
    AckInfo_t * pAckInfo;
    uint16_t packetIdentifier = pDeserializedInfo->packetIdentifier;
    MQTTAgentContext_t * pAgentContext;
    const uint8_t upperNibble = ( uint8_t ) 0xF0;

    assert( pMqttContext != NULL );
    assert( pPacketInfo != NULL );

    pAgentContext = getAgentFromMQTTContext( pMqttContext );

    /* This callback executes from within MQTT_ProcessLoop().  Setting this flag
     * indicates that the callback executed so the caller of MQTT_ProcessLoop() knows
     * it should call it again as there may be more data to process. */
    pAgentContext->packetReceivedInLoop = true;

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
            case MQTT_PACKET_TYPE_SUBACK:
            case MQTT_PACKET_TYPE_UNSUBACK:
                pAckInfo = getAwaitingOperation( pAgentContext, packetIdentifier );

                if( pAckInfo != NULL )
                {
                    /* This function will also clear the memory associated with
                     * the ack list entry. */
                    handleAcks( pAgentContext,
                                pPacketInfo,
                                pDeserializedInfo,
                                pAckInfo,
                                pPacketInfo->type );
                }
                else
                {
                    LogError( ( "No operation found matching packet id %u.\n", packetIdentifier ) );
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

    /* If the packet ID is zero then the MQTT context has not been initialized as 0
     * is the initial value but not a valid packet ID. */
    if( pMqttAgentContext->mqttContext.nextPacketId != 0 )
    {
        pCommand = pMqttAgentContext->agentInterface.getCommand( blockTimeMs );

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
                statusReturn = addCommandToQueue( pMqttAgentContext, pCommand, blockTimeMs );
            }

            if( statusReturn != MQTTSuccess )
            {
                /* Could not send the command to the queue so release the command
                 * structure again. */
                pMqttAgentContext->agentInterface.releaseCommand( pCommand );
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

static MQTTStatus_t resendPublishes( MQTTAgentContext_t * pMqttAgentContext )
{
    MQTTStatus_t statusResult = MQTTSuccess;
    MQTTStateCursor_t cursor = MQTT_STATE_CURSOR_INITIALIZER;
    uint16_t packetId = MQTT_PACKET_ID_INVALID;
    AckInfo_t * pFoundAck = NULL;
    MQTTPublishInfo_t * pOriginalPublish = NULL;
    MQTTContext_t * pMqttContext;

    assert( pMqttAgentContext != NULL );
    pMqttContext = &( pMqttAgentContext->mqttContext );

    packetId = MQTT_PublishToResend( pMqttContext, &cursor );

    while( packetId != MQTT_PACKET_ID_INVALID )
    {
        /* Retrieve the operation but do not remove it from the list. */
        pFoundAck = getAwaitingOperation( pMqttAgentContext, packetId );

        if( pFoundAck != NULL )
        {
            /* Set the DUP flag. */
            pOriginalPublish = ( MQTTPublishInfo_t * ) ( pFoundAck->pOriginalCommand->pArgs );
            pOriginalPublish->dup = true;
            statusResult = MQTT_Publish( pMqttContext, pOriginalPublish, packetId );

            if( statusResult != MQTTSuccess )
            {
                LogError( ( "Error in resending publishes. Error code=%s\n", MQTT_Status_strerror( statusResult ) ) );
                break;
            }
        }

        packetId = MQTT_PublishToResend( pMqttContext, &cursor );
    }

    return statusResult;
}

/*-----------------------------------------------------------*/

static void clearPendingAcknowledgments( MQTTAgentContext_t * pMqttAgentContext )
{
    size_t i = 0;
    MQTTAgentReturnInfo_t returnInfo = { 0 };
    AckInfo_t * pendingAcks;

    returnInfo.returnCode = MQTTBadResponse;

    assert( pMqttAgentContext != NULL );

    pendingAcks = pMqttAgentContext->pPendingAcks;

    /* Clear all operations pending acknowledgments. */
    for( i = 0; i < MQTT_AGENT_MAX_OUTSTANDING_ACKS; i++ )
    {
        if( pendingAcks[ i ].packetId != MQTT_PACKET_ID_INVALID )
        {
            if( pendingAcks[ i ].pOriginalCommand->pCommandCompleteCallback != NULL )
            {
                /* Bad response to indicate network error. */
                pendingAcks[ i ].pOriginalCommand->pCommandCompleteCallback(
                    pendingAcks[ i ].pOriginalCommand->pCmdContext,
                    &returnInfo );
            }

            /* Now remove it from the list. */
            memset( &( pendingAcks[ i ] ), 0x00, sizeof( AckInfo_t ) );
        }
    }
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_Init( MQTTAgentContext_t * pMqttAgentContext,
                             AgentMessageInterface_t * pMsgInterface,
                             MQTTFixedBuffer_t * pNetworkBuffer,
                             TransportInterface_t * pTransportInterface,
                             MQTTGetCurrentTimeFunc_t getCurrentTimeMs,
                             IncomingPublishCallback_t incomingCallback,
                             void * pIncomingPacketContext )
{
    MQTTStatus_t returnStatus;

    if( ( pMqttAgentContext == NULL ) ||
        ( pMsgInterface == NULL ) ||
        ( pTransportInterface == NULL ) ||
        ( getCurrentTimeMs == NULL ) )
    {
        returnStatus = MQTTBadParameter;
    }
    else if( ( pMsgInterface->pMsgCtx == NULL ) ||
             ( pMsgInterface->send == NULL ) ||
             ( pMsgInterface->recv == NULL ) ||
             ( pMsgInterface->getCommand == NULL ) ||
             ( pMsgInterface->receiveCommand == NULL ) )
    {
        LogError( ( "Invalid parameter: pMsgInterface must set all members." ) );
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
            pMqttAgentContext->agentInterface = *pMsgInterface;
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
    bool endLoop = false;

    /* The command queue should have been created before this task gets created. */
    assert( pMqttAgentContext->agentInterface.pMsgCtx );

    if( pMqttAgentContext == NULL )
    {
        operationStatus = MQTTBadParameter;
    }

    /* Loop until an error or we receive a terminate command. */
    while( operationStatus == MQTTSuccess )
    {
        /* Wait for the next command, if any. */
        pCommand = NULL;
        ( void ) pMqttAgentContext->agentInterface.recv(
            pMqttAgentContext->agentInterface.pMsgCtx,
            &( pCommand ),
            MQTT_AGENT_MAX_EVENT_QUEUE_WAIT_TIME
            );
        /* Set the command type in case the command is released while processing. */
        currentCommandType = ( pCommand ) ? pCommand->commandType : NONE;
        operationStatus = processCommand( pMqttAgentContext, pCommand, &endLoop );

        if( operationStatus != MQTTSuccess )
        {
            LogError( ( "MQTT operation failed with status %s\n",
                        MQTT_Status_strerror( operationStatus ) ) );
        }

        /* Terminate the loop on disconnects, errors, or the termination command. */
        if( endLoop )
        {
            break;
        }
    }

    return operationStatus;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_ResumeSession( MQTTAgentContext_t * pMqttAgentContext,
                                      bool sessionPresent )
{
    MQTTStatus_t statusResult = MQTTSuccess;

    /* If the packet ID is zero then the MQTT context has not been initialized as 0
     * is the initial value but not a valid packet ID. */
    if( ( pMqttAgentContext != NULL ) && ( pMqttAgentContext->mqttContext.nextPacketId != 0 ) )
    {
        /* Resend publishes if session is present. NOTE: It's possible that some
         * of the operations that were in progress during the network interruption
         * were subscribes. In that case, we would want to mark those operations
         * as completing with error and remove them from the list of operations, so
         * that the calling task can try subscribing again. */
        if( sessionPresent )
        {
            statusResult = resendPublishes( pMqttAgentContext );
        }

        /* If we wanted to resume a session but none existed with the broker, we
         * should mark all in progress operations as errors so that the tasks that
         * created them can try again. */
        else
        {
            /* We have a clean session, so clear all operations pending acknowledgments. */
            clearPendingAcknowledgments( pMqttAgentContext );
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

    statusReturn = createAndAddCommand( SUBSCRIBE,                                 /* commandType */
                                        pMqttAgentContext,                         /* mqttContextHandle */
                                        pSubscriptionArgs,                         /* pMqttInfoParam */
                                        pCommandInfo->cmdCompleteCallback,         /* commandCompleteCallback */
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

    statusReturn = createAndAddCommand( UNSUBSCRIBE,                               /* commandType */
                                        pMqttAgentContext,                         /* mqttContextHandle */
                                        pSubscriptionArgs,                         /* pMqttInfoParam */
                                        pCommandInfo->cmdCompleteCallback,         /* commandCompleteCallback */
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

    statusReturn = createAndAddCommand( PUBLISH,                                   /* commandType */
                                        pMqttAgentContext,                         /* mqttContextHandle */
                                        pPublishInfo,                              /* pMqttInfoParam */
                                        pCommandInfo->cmdCompleteCallback,         /* commandCompleteCallback */
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

    statusReturn = createAndAddCommand( DISCONNECT,                                /* commandType */
                                        pMqttAgentContext,                         /* mqttContextHandle */
                                        NULL,                                      /* pMqttInfoParam */
                                        pCommandInfo->cmdCompleteCallback,         /* commandCompleteCallback */
                                        pCommandInfo->pCmdCompleteCallbackContext, /* pCommandCompleteCallbackContext */
                                        pCommandInfo->blockTimeMs );

    return statusReturn;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgent_Ping( MQTTAgentContext_t * pMqttAgentContext,
                             CommandInfo_t * pCommandInfo )
{
    MQTTStatus_t statusReturn;

    statusReturn = createAndAddCommand( PING,                                      /* commandType */
                                        pMqttAgentContext,                         /* mqttContextHandle */
                                        NULL,                                      /* pMqttInfoParam */
                                        pCommandInfo->cmdCompleteCallback,         /* commandCompleteCallback */
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
