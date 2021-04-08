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
 * @file mqtt_agent_command_functions.c
 * @brief Implements functions to process MQTT agent commands.
 */

/* Standard includes. */
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* MQTT agent include. */
#include "mqtt_agent.h"

/* Header include. */
#include "mqtt_agent_command_functions.h"

/*-----------------------------------------------------------*/

/**
 * @brief Conclude a command and mark as an error. Copied from mqtt_agent.c
 *
 * @param[in] pAgentContext Agent context for the MQTT connection.
 * @param[in] pCommand Command to complete.
 */
static void concludeCommandAsError( const MQTTAgentContext_t * pAgentContext,
                                    Command_t * pCommand );

static void concludeCommandAsError( const MQTTAgentContext_t * pAgentContext,
                                    Command_t * pCommand )
{
    bool commandReleased = false;
    MQTTAgentReturnInfo_t returnInfo;

    ( void ) memset( &returnInfo, 0x00, sizeof( MQTTAgentReturnInfo_t ) );
    assert( pAgentContext != NULL );
    assert( pAgentContext->agentInterface.releaseCommand != NULL );
    assert( pCommand != NULL );

    returnInfo.returnCode = MQTTBadResponse;

    if( pCommand->pCommandCompleteCallback != NULL )
    {
        pCommand->pCommandCompleteCallback( pCommand->pCmdContext, &returnInfo );
    }

    commandReleased = pAgentContext->agentInterface.releaseCommand( pCommand );

    if( !commandReleased )
    {
        LogError( ( "Failed to release command %p of type %d.",
                    ( void * ) pCommand,
                    pCommand->commandType ) );
    }
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgentCommand_ProcessLoop( MQTTAgentContext_t * pMqttAgentContext,
                                           void * pUnusedArg,
                                           MQTTAgentCommandFuncReturns_t * pReturnFlags )
{
    ( void ) pUnusedArg;
    ( void ) pMqttAgentContext;
    assert( pReturnFlags != NULL );

    ( void ) memset( pReturnFlags, 0x00, sizeof( MQTTAgentCommandFuncReturns_t ) );
    pReturnFlags->runProcessLoop = true;

    return MQTTSuccess;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgentCommand_Publish( MQTTAgentContext_t * pMqttAgentContext,
                                       void * pPublishArg,
                                       MQTTAgentCommandFuncReturns_t * pReturnFlags )
{
    MQTTPublishInfo_t * pPublishInfo;
    MQTTStatus_t ret;

    assert( pMqttAgentContext != NULL );
    assert( pPublishArg != NULL );
    assert( pReturnFlags != NULL );

    ( void ) memset( pReturnFlags, 0x00, sizeof( MQTTAgentCommandFuncReturns_t ) );
    pPublishInfo = ( MQTTPublishInfo_t * ) ( pPublishArg );

    if( pPublishInfo->qos != MQTTQoS0 )
    {
        pReturnFlags->packetId = MQTT_GetPacketId( &( pMqttAgentContext->mqttContext ) );
    }

    LogInfo( ( "Publishing message to %.*s.\n", ( int ) pPublishInfo->topicNameLength, pPublishInfo->pTopicName ) );
    ret = MQTT_Publish( &( pMqttAgentContext->mqttContext ), pPublishInfo, pReturnFlags->packetId );

    /* Add to pending ack list, or call callback if QoS 0. */
    pReturnFlags->addAcknowledgment = ( pPublishInfo->qos != MQTTQoS0 ) && ( ret == MQTTSuccess );
    pReturnFlags->runProcessLoop = true;

    return ret;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgentCommand_Subscribe( MQTTAgentContext_t * pMqttAgentContext,
                                         void * pVoidSubscribeArgs,
                                         MQTTAgentCommandFuncReturns_t * pReturnFlags )
{
    MQTTAgentSubscribeArgs_t * pSubscribeArgs;
    MQTTStatus_t ret;

    assert( pMqttAgentContext != NULL );
    assert( pVoidSubscribeArgs != NULL );
    assert( pReturnFlags != NULL );

    ( void ) memset( pReturnFlags, 0x00, sizeof( MQTTAgentCommandFuncReturns_t ) );
    pSubscribeArgs = ( MQTTAgentSubscribeArgs_t * ) ( pVoidSubscribeArgs );
    pReturnFlags->packetId = MQTT_GetPacketId( &( pMqttAgentContext->mqttContext ) );

    ret = MQTT_Subscribe( &( pMqttAgentContext->mqttContext ),
                          pSubscribeArgs->pSubscribeInfo,
                          pSubscribeArgs->numSubscriptions,
                          pReturnFlags->packetId );

    pReturnFlags->addAcknowledgment = ( ret == MQTTSuccess );
    pReturnFlags->runProcessLoop = true;

    return ret;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgentCommand_Unsubscribe( MQTTAgentContext_t * pMqttAgentContext,
                                           void * pVoidSubscribeArgs,
                                           MQTTAgentCommandFuncReturns_t * pReturnFlags )
{
    MQTTAgentSubscribeArgs_t * pSubscribeArgs;
    MQTTStatus_t ret;

    assert( pMqttAgentContext != NULL );
    assert( pVoidSubscribeArgs != NULL );
    assert( pReturnFlags != NULL );

    ( void ) memset( pReturnFlags, 0x00, sizeof( MQTTAgentCommandFuncReturns_t ) );
    pSubscribeArgs = ( MQTTAgentSubscribeArgs_t * ) ( pVoidSubscribeArgs );
    pReturnFlags->packetId = MQTT_GetPacketId( &( pMqttAgentContext->mqttContext ) );

    ret = MQTT_Unsubscribe( &( pMqttAgentContext->mqttContext ),
                            pSubscribeArgs->pSubscribeInfo,
                            pSubscribeArgs->numSubscriptions,
                            pReturnFlags->packetId );

    pReturnFlags->addAcknowledgment = ( ret == MQTTSuccess );
    pReturnFlags->runProcessLoop = true;

    return ret;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgentCommand_Connect( MQTTAgentContext_t * pMqttAgentContext,
                                       void * pConnectArgs,
                                       MQTTAgentCommandFuncReturns_t * pReturnFlags )
{
    MQTTStatus_t ret;
    MQTTAgentConnectArgs_t * pConnectInfo;

    assert( pMqttAgentContext != NULL );
    assert( pConnectArgs != NULL );
    assert( pReturnFlags != NULL );

    pConnectInfo = ( MQTTAgentConnectArgs_t * ) ( pConnectArgs );

    ret = MQTT_Connect( &( pMqttAgentContext->mqttContext ),
                        pConnectInfo->pConnectInfo,
                        pConnectInfo->pWillInfo,
                        pConnectInfo->timeoutMs,
                        &( pConnectInfo->sessionPresent ) );

    /* Resume a session if one existed, else clear the list of acknowledgments. */
    if( ret == MQTTSuccess )
    {
        LogInfo( ( "Session present flag: %d", pConnectInfo->sessionPresent ) );
        ret = MQTTAgent_ResumeSession( pMqttAgentContext,
                                       pConnectInfo->sessionPresent );
    }

    ( void ) memset( pReturnFlags, 0x00, sizeof( MQTTAgentCommandFuncReturns_t ) );

    return ret;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgentCommand_Disconnect( MQTTAgentContext_t * pMqttAgentContext,
                                          void * pUnusedArg,
                                          MQTTAgentCommandFuncReturns_t * pReturnFlags )
{
    MQTTStatus_t ret;

    ( void ) pUnusedArg;

    assert( pMqttAgentContext != NULL );
    assert( pReturnFlags != NULL );

    ret = MQTT_Disconnect( &( pMqttAgentContext->mqttContext ) );

    ( void ) memset( pReturnFlags, 0x00, sizeof( MQTTAgentCommandFuncReturns_t ) );
    pReturnFlags->endLoop = true;

    return ret;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgentCommand_Ping( MQTTAgentContext_t * pMqttAgentContext,
                                    void * pUnusedArg,
                                    MQTTAgentCommandFuncReturns_t * pReturnFlags )
{
    MQTTStatus_t ret;

    ( void ) pUnusedArg;

    assert( pMqttAgentContext != NULL );
    assert( pReturnFlags != NULL );

    ret = MQTT_Ping( &( pMqttAgentContext->mqttContext ) );

    ( void ) memset( pReturnFlags, 0x00, sizeof( MQTTAgentCommandFuncReturns_t ) );

    pReturnFlags->runProcessLoop = true;

    return ret;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTTAgentCommand_Terminate( MQTTAgentContext_t * pMqttAgentContext,
                                         void * pUnusedArg,
                                         MQTTAgentCommandFuncReturns_t * pReturnFlags )
{
    Command_t * pReceivedCommand = NULL;
    bool commandWasReceived = false;
    AckInfo_t * pendingAcks;
    size_t i;

    ( void ) pUnusedArg;

    assert( pMqttAgentContext != NULL );
    assert( pMqttAgentContext->agentInterface.releaseCommand != NULL );
    assert( pReturnFlags != NULL );

    pendingAcks = pMqttAgentContext->pPendingAcks;

    LogInfo( ( "Terminating command loop.\n" ) );
    ( void ) memset( pReturnFlags, 0x00, sizeof( MQTTAgentCommandFuncReturns_t ) );
    pReturnFlags->endLoop = true;

    /* Cancel all operations waiting in the queue. */
    do
    {
        pReceivedCommand = NULL;
        commandWasReceived = pMqttAgentContext->agentInterface.recv(
            pMqttAgentContext->agentInterface.pMsgCtx,
            &( pReceivedCommand ),
            0U );

        if( pReceivedCommand != NULL )
        {
            concludeCommandAsError( pMqttAgentContext, pReceivedCommand );
        }
    } while( commandWasReceived );

    /* Cancel any operations awaiting an acknowledgment. */
    for( i = 0; i < MQTT_AGENT_MAX_OUTSTANDING_ACKS; i++ )
    {
        if( pendingAcks[ i ].packetId != MQTT_PACKET_ID_INVALID )
        {
            concludeCommandAsError( pMqttAgentContext, pendingAcks[ i ].pOriginalCommand );

            /* Now remove it from the list. */
            ( void ) memset( &( pendingAcks[ i ] ), 0x00, sizeof( AckInfo_t ) );
        }
    }

    return MQTTSuccess;
}

/*-----------------------------------------------------------*/
