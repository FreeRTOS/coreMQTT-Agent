/*
 * MQTTAgent v1.1.0
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
 * @file mqtt_agent_utest.c
 * @brief Unit tests for functions in mqtt_agent_utest.h
 */
#include <string.h>
#include <stdbool.h>

#include "unity.h"

/* Include paths for public enums, structures, and macros. */

#include "mock_core_mqtt.h"

/**
 * @brief MQTT client identifier.
 */
#define MQTT_CLIENT_IDENTIFIER                 "testclient"

/**
 * @brief A sample network context that we set to NULL.
 */
#define MQTT_SAMPLE_NETWORK_CONTEXT            ( NULL )

/**
 * @brief Time at the beginning of each test. Note that this is not updated with
 * a real clock. Instead, we simply increment this variable.
 */
static uint32_t globalEntryTime = 0;


/* ============================   UNITY FIXTURES ============================ */

/* Called before each test method. */
void setUp()
{
    memset( mqttBuffer, 0x0, sizeof( mqttBuffer ) );
    MQTT_State_strerror_IgnoreAndReturn( "DUMMY_MQTT_STATE" );

    globalEntryTime = 0;
}

/* Called after each test method. */
void tearDown()
{
}

/* Called at the beginning of the whole suite. */
void suiteSetUp()
{
}

/* Called at the end of the whole suite. */
int suiteTearDown( int numFailures )
{
    return numFailures;
}

/* ========================================================================== */

/**
 * @brief Mocked successful transport send.
 *
 * @param[in] tcpSocket TCP socket.
 * @param[in] pMessage Data to send.
 * @param[in] bytesToWrite Length of data to send.
 *
 * @return Number of bytes sent; negative value on error;
 * 0 for timeout or 0 bytes sent.
 */
static int32_t transportSendSuccess( NetworkContext_t * pNetworkContext,
                                     const void * pBuffer,
                                     size_t bytesToWrite )
{
    TEST_ASSERT_EQUAL( MQTT_SAMPLE_NETWORK_CONTEXT, pNetworkContext );
    ( void ) pBuffer;
    return bytesToWrite;
}

/**
 * @brief Mocked successful transport read.
 *
 * @param[in] tcpSocket TCP socket.
 * @param[out] pBuffer Buffer for receiving data.
 * @param[in] bytesToRead Size of pBuffer.
 *
 * @return Number of bytes received; negative value on error.
 */
static int32_t transportRecvSuccess( NetworkContext_t * pNetworkContext,
                                     void * pBuffer,
                                     size_t bytesToRead )
{
    TEST_ASSERT_EQUAL( MQTT_SAMPLE_NETWORK_CONTEXT, pNetworkContext );
    ( void ) pBuffer;
    return bytesToRead;
}


/**
 * @brief Initialize the transport interface with the mocked functions for
 * send and receive.
 *
 * @brief param[in] pTransport The transport interface to use with the context.
 */
static void setupTransportInterface( TransportInterface_t * pTransport )
{
    pTransport->pNetworkContext = MQTT_SAMPLE_NETWORK_CONTEXT;
    pTransport->send = transportSendSuccess;
    pTransport->recv = transportRecvSuccess;
}

/**
 * @brief A mocked timer query function that increments on every call.
 */
static uint32_t getTime( void )
{
    return globalEntryTime++;
}
/* ========================================================================== */

/**
 * @brief Test that MQTTAgent_Init is able to update the context object correctly.
 */

void test_MQTTAgent_Init_Happy_Path( void )
{

    MQTTAgentContext_t mqttAgentContext;
    AgentMessageContext_t msgCtx;
    MQTTFixedBuffer_t networkBuffer;
    TransportInterface_t transportInterface;
    IncomingPublishCallback_t incomingCallback;
    void incomingPacketContext;
    MQTTStatus_t mqttStatus;

    //Mock MQtt_init
    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgCtx, &networkBuffer , &transportInterface, getTime, incomingCallback, incomingPacketContext);
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    TEST_ASSERT_FALSE(NULL,mqttAgentContext);
    TEST_ASSERT_EQUAL_PTR( &incomingCallback, mqttAgentContext.pIncomingCallback );
    TEST_ASSERT_EQUAL_PTR( &incomingPacketContext, mqttAgentContext.pIncomingCallbackContext );
    TEST_ASSERT_EQUAL_PTR( &msgCtx,  mqttAgentContext.pMessageCtx );

}

/**
 * @brief Test that any NULL parameter causes MQTTAgent_Init to return MQTTBadParameter.
 */
void test_MQTT_Init_Invalid_Params( void )
{
    MQTTAgentContext_t mqttAgentContext;
    AgentMessageContext_t msgCtx;
    MQTTFixedBuffer_t networkBuffer;
    TransportInterface_t transportInterface;
    IncomingPublishCallback_t incomingCallback;
    void incomingPacketContext;
    MQTTStatus_t mqttStatus;

    /* Check that MQTTBadParameter is returned if any NULL parameters are passed. */
    //Mock MQtt_init
    mqttStatus = MQTTAgent_Init( NULL, &msgCtx, &networkBuffer , &transportInterface, getTime, incomingCallback, incomingPacketContext);
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, NULL, &networkBuffer , &transportInterface, getTime, incomingCallback, incomingPacketContext);
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgCtx, &networkBuffer , NULL, getTime, incomingCallback, incomingPacketContext);
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgCtx, NULL , &transportInterface, getTime, incomingCallback, incomingPacketContext);
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgCtx, &networkBuffer , &transportInterface, getTime, incomingCallback, NULL);
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    /* Test if NULL is passed for any of the function pointers. */
    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgCtx, &networkBuffer , &transportInterface, NULL, incomingCallback, incomingPacketContext);
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgCtx, &networkBuffer , &transportInterface, getTime, NULL, incomingPacketContext);
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

}
