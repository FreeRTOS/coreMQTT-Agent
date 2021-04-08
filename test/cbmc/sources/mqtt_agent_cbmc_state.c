/*
 * coreMQTT-Agent v1.0.0
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
 */

/**
 * @file mqtt_agent_cbmc_state.c
 * @brief Implements the functions defined in mqtt_agent_cbmc_state.h.
 */
#include <stdint.h>
#include <stdlib.h>
#include "mqtt_agent.h"
#include "mqtt_agent_cbmc_state.h"
#include "network_interface_stubs.h"
#include "get_time_stub.h"
#include "incoming_publish_callback_stub.h"
#include "agent_message_stubs.h"
#include "agent_command_pool_stubs.h"


MQTTFixedBuffer_t * allocateMqttFixedBuffer( MQTTFixedBuffer_t * pFixedBuffer )
{
    if( pFixedBuffer == NULL )
    {
        pFixedBuffer = malloc( sizeof( MQTTFixedBuffer_t ) );
    }

    if( pFixedBuffer != NULL )
    {
        __CPROVER_assume( pFixedBuffer->size < CBMC_MAX_OBJECT_SIZE );
        pFixedBuffer->pBuffer = malloc( pFixedBuffer->size );
    }

    return pFixedBuffer;
}

bool isValidMqttFixedBuffer( const MQTTFixedBuffer_t * pFixedBuffer )
{
    bool isValid = true;

    if( pFixedBuffer != NULL )
    {
        isValid = pFixedBuffer->size < CBMC_MAX_OBJECT_SIZE;
    }

    return isValid;
}

MQTTAgentContext_t * allocateMqttAgentContext( MQTTAgentContext_t * pContext )
{
    TransportInterface_t * pTransportInterface;
    MQTTFixedBuffer_t * pNetworkBuffer;
    AgentMessageInterface_t * pMessageInterface;
    MQTTStatus_t status = MQTTSuccess;
    void * pIncomingCallbackContext;

    if( pContext == NULL )
    {
        pContext = malloc( sizeof( MQTTAgentContext_t ) );
    }

    pTransportInterface = malloc( sizeof( TransportInterface_t ) );

    if( pTransportInterface != NULL )
    {
        /* The possibility that recv and send callbacks are NULL is tested in the
         * MQTT_Init proof. MQTT_Init is required to be called before any other
         * function in core_mqtt.h. */
        pTransportInterface->recv = NetworkInterfaceReceiveStub;
        pTransportInterface->send = NetworkInterfaceSendStub;
    }

    pNetworkBuffer = allocateMqttFixedBuffer( NULL );
    __CPROVER_assume( isValidMqttFixedBuffer( pNetworkBuffer ) );

    pMessageInterface = malloc( sizeof( AgentMessageInterface_t ) );

    if( pMessageInterface != NULL )
    {
        pMessageInterface->send = AgentMessageSendStub;
        pMessageInterface->recv = AgentMessageRecvStub;
        pMessageInterface->getCommand = AgentGetCommandStub;
        pMessageInterface->releaseCommand = Agent_ReleaseCommand;
    }

    /* It is part of the API contract to call MQTT_Init() with the MQTTContext_t
     * before any other function in core_mqtt.h. */
    if( pContext != NULL )
    {
        status = MQTTAgent_Init( pContext,
                                 pMessageInterface,
                                 pNetworkBuffer,
                                 pTransportInterface,
                                 GetCurrentTimeStub,
                                 IncomingPublishCallbackStub,
                                 pIncomingCallbackContext );
    }

    /* If the MQTTAgentContext_t initialization failed, then set the context to NULL
     * so that function under harness will return immediately upon a NULL
     * parameter check. */
    if( status != MQTTSuccess )
    {
        pContext = NULL;
    }

    return pContext;
}

bool isValidMqttAgentContext( const MQTTAgentContext_t * pContext )
{
    bool isValid = false;

    if( pContext != NULL )
    {
        isValid = isValidMqttFixedBuffer( &( pContext->mqttContext.networkBuffer ) );
    }

    return isValid;
}

bool isAgentSendCommandFunctionStatus( MQTTStatus_t mqttStatus )
{
    return( ( mqttStatus == MQTTSuccess ) ||
            ( mqttStatus == MQTTBadParameter ) ||
            ( mqttStatus == MQTTNoMemory ) ||
            ( mqttStatus == MQTTSendFailed ) );
}

MQTTAgentConnectArgs_t * allocateConnectArgs( MQTTAgentConnectArgs_t * pConnectArgs )
{
    if(pConnectArgs==NULL){
        pConnectArgs= malloc(sizeof(MQTTAgentConnectArgs_t));
        __CPROVER_assume( pConnectArgs != NULL );
    }

    pConnectArgs->pConnectInfo=malloc(sizeof(MQTTConnectInfo_t));
    __CPROVER_assume( pConnectArgs->pConnectInfo != NULL );
    pConnectArgs->pWillInfo=malloc(sizeof(MQTTPublishInfo_t));

    return pConnectArgs;
}
