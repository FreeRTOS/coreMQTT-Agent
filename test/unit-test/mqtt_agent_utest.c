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
#include "mqtt_agent.h"
#include "mock_core_mqtt.h"
#include "mock_core_mqtt_state.h"

/**
 * @brief Time at the beginning of each test. Note that this is not updated with
 * a real clock. Instead, we simply increment this variable.
 */
static uint32_t globalEntryTime;

/**
 * @brief Mock Counter variable to check callback is called on command completion.
 */
static uint32_t commandCompleteCallbackCount;

/**
 * @brief Mock Command variable.
 */
static Command_t command;

/**
 * @brief Message context to use for tests.
 */
static AgentMessageContext_t globalMessageContext;
/* ========================================================================== */

/**
 * @brief A mocked timer query function that increments on every call.
 */
static uint32_t getTime( void )
{
    return globalEntryTime++;
}
/* ========================================================================== */

/**
 * @brief A mocked send function to send command to agent.
 */
static bool mockSend( AgentMessageContext_t * pMsgCtx,
                      const void * pData,
                      uint32_t blockTimeMs )
{
    return true;
}

/**
 * @brief A mocked receive function for the agent to receive command.
 */
static bool mockReceive( AgentMessageContext_t * pMsgCtx,
                         void * pBuffer,
                         uint32_t blockTimeMs )
{
    return true;
}

/**
 * @brief A mocked function to release an allocated command.
 */
static bool mockReleaseCommand( Command_t * pCommandToRelease )
{
    return true;
}

/**
 * @brief A mocked function to obtain a pointer to an allocated command.
 */
static Command_t * mockGetCommand( uint32_t blockTimeMs )
{
    return &command;
}

/**
 * @brief A mocked Callback to invoke upon command completion.
 */
static void mockCommandCallback( void * pCmdCallbackContext,
                                 MQTTAgentReturnInfo_t * pReturnInfo )
{
    commandCompleteCallbackCount++;
}

/**
 * @brief Context Definition with which tasks may deliver messages to the agent.
 */
struct AgentMessageContext
{
    Command_t * pSentCommand;
};

/**
 * @brief A mock publish callback function.
 */
static void mockPublishCallback( MQTTAgentContext_t * pMqttAgentContext,
                                 uint16_t packetId,
                                 MQTTPublishInfo_t * pPublishInfo )
{
    ( void ) pMqttAgentContext;
    ( void ) packetId;
    ( void ) pPublishInfo;
}

/**
 * @brief Function to initialize MQTT Agent Context to valid parameters.
 */
static void setupAgentContext( MQTTAgentContext_t * pAgentContext )
{
    AgentMessageInterface_t messageInterface = { 0 };
    MQTTFixedBuffer_t networkBuffer = { 0 };
    TransportInterface_t transportInterface = { 0 };
    void * incomingPacketContext = NULL;
    MQTTStatus_t mqttStatus;

    messageInterface.pMsgCtx = &globalMessageContext;
    messageInterface.send = mockSend;
    messageInterface.recv = mockReceive;
    messageInterface.releaseCommand = mockReleaseCommand;
    messageInterface.getCommand = mockGetCommand;

    MQTT_Init_ExpectAnyArgsAndReturn( MQTTSuccess );
    mqttStatus = MQTTAgent_Init( pAgentContext,
                                 &messageInterface,
                                 &networkBuffer,
                                 &transportInterface,
                                 getTime,
                                 mockPublishCallback,
                                 incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    /* Set packet ID nonzero to indicate initialization. */
    pAgentContext->mqttContext.nextPacketId = 1U;
}

/* ============================   UNITY FIXTURES ============================ */

/* Called before each test method. */
void setUp()
{
    globalEntryTime = 0;
    commandCompleteCallbackCount = 0;
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


/**
 * @brief Test that MQTTAgent_Init is able to update the context object correctly.
 */

void test_MQTTAgent_Init_Happy_Path( void )
{
    MQTTAgentContext_t mqttAgentContext;
    AgentMessageInterface_t msgInterface = { 0 };
    MQTTFixedBuffer_t networkBuffer = { 0 };
    TransportInterface_t transportInterface = { 0 };
    void * incomingPacketContext;
    AgentMessageContext_t msg;
    MQTTStatus_t mqttStatus;

    msgInterface.pMsgCtx = &msg;
    msgInterface.send = mockSend;
    msgInterface.recv = mockReceive;
    msgInterface.releaseCommand = mockReleaseCommand;
    msgInterface.getCommand = mockGetCommand;

    MQTT_Init_ExpectAnyArgsAndReturn( MQTTSuccess );
    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgInterface, &networkBuffer, &transportInterface, getTime, mockPublishCallback, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    TEST_ASSERT_EQUAL_PTR( mockPublishCallback, mqttAgentContext.pIncomingCallback );
    TEST_ASSERT_EQUAL_PTR( incomingPacketContext, mqttAgentContext.pIncomingCallbackContext );
    TEST_ASSERT_EQUAL_MEMORY( &msgInterface, &mqttAgentContext.agentInterface, sizeof( msgInterface ) );
}

/**
 * @brief Test that any NULL parameter causes MQTTAgent_Init to return MQTTBadParameter.
 */
void test_MQTTAgent_Init_Invalid_Params( void )
{
    MQTTAgentContext_t mqttAgentContext;
    AgentMessageInterface_t msgInterface = { 0 };
    MQTTFixedBuffer_t networkBuffer = { 0 };
    TransportInterface_t transportInterface = { 0 };
    IncomingPublishCallback_t incomingCallback = mockPublishCallback;
    void * incomingPacketContext;
    AgentMessageContext_t msg;
    MQTTStatus_t mqttStatus;

    /* Check that MQTTBadParameter is returned if any NULL parameters are passed. */
    mqttStatus = MQTTAgent_Init( NULL, &msgInterface, &networkBuffer, &transportInterface, getTime, incomingCallback, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, NULL, &networkBuffer, &transportInterface, getTime, incomingCallback, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgInterface, &networkBuffer, NULL, getTime, incomingCallback, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    /* Test if NULL is passed for any of the function pointers. */
    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgInterface, &networkBuffer, &transportInterface, getTime, NULL, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgInterface, &networkBuffer, &transportInterface, NULL, incomingCallback, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgInterface, &networkBuffer, &transportInterface, getTime, incomingCallback, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    msgInterface.pMsgCtx = &msg;

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgInterface, &networkBuffer, &transportInterface, getTime, incomingCallback, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    msgInterface.send = mockSend;

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgInterface, &networkBuffer, &transportInterface, getTime, incomingCallback, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    msgInterface.recv = mockReceive;

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgInterface, &networkBuffer, &transportInterface, getTime, incomingCallback, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    msgInterface.releaseCommand = mockReleaseCommand;

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgInterface, &networkBuffer, &transportInterface, getTime, incomingCallback, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    msgInterface.getCommand = mockGetCommand;

    /* MQTT_Init() should check the network buffer. */
    MQTT_Init_ExpectAnyArgsAndReturn( MQTTBadParameter );
    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgInterface, NULL, &transportInterface, getTime, incomingCallback, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

}

/**
 * @brief Test that any NULL parameter causes MQTTAgent_ResumeSession to return MQTTBadParameter.
 */
void test_MQTTAgent_ResumeSession_Invalid_Params( void )
{
    bool sessionPresent = true;
    MQTTStatus_t mqttStatus;

    MQTTAgentContext_t mqttAgentContext;

    setupAgentContext( &mqttAgentContext );

    /* Check that MQTTBadParameter is returned if any NULL mqttAgentContext is passed. */
    mqttStatus = MQTTAgent_ResumeSession( NULL, sessionPresent );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    /* Check that MQTTBadParameter is returned if the MQTT context has not been initialized. */
    mqttAgentContext.mqttContext.nextPacketId = 0;
    mqttStatus = MQTTAgent_ResumeSession( &mqttAgentContext, sessionPresent );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );
}


void test_MQTTAgent_ResumeSession_session_present_no_resent_publishes( void )
{
    bool sessionPresent = true;
    MQTTStatus_t mqttStatus;
    MQTTAgentContext_t mqttAgentContext;

    setupAgentContext( &mqttAgentContext );

    MQTT_PublishToResend_IgnoreAndReturn( MQTT_PACKET_ID_INVALID );
    mqttStatus = MQTTAgent_ResumeSession( &mqttAgentContext, sessionPresent );
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
}


void test_MQTTAgent_ResumeSession_failed_publish( void )
{
    bool sessionPresent = true;
    MQTTStatus_t mqttStatus;
    MQTTAgentContext_t mqttAgentContext;
    Command_t command;
    MQTTPublishInfo_t args;
    AckInfo_t ackInfo;

    setupAgentContext( &mqttAgentContext );

    command.pArgs = &args;
    ackInfo.packetId = 1U;
    ackInfo.pOriginalCommand = &command;
    mqttAgentContext.pPendingAcks[ 0 ] = ackInfo;
    /* Check that failed resending publish return MQTTSendFailed. */
    MQTT_PublishToResend_IgnoreAndReturn( 1 );
    MQTT_Publish_IgnoreAndReturn( MQTTSendFailed );
    mqttStatus = MQTTAgent_ResumeSession( &mqttAgentContext, sessionPresent );
    TEST_ASSERT_EQUAL( MQTTSendFailed, mqttStatus );
}

void test_MQTTAgent_ResumeSession_publish_resend_success( void )
{
    bool sessionPresent = true;
    MQTTStatus_t mqttStatus;
    MQTTAgentContext_t mqttAgentContext;
    Command_t command;
    MQTTPublishInfo_t args;
    AckInfo_t ackInfo;

    setupAgentContext( &mqttAgentContext );

    command.pArgs = &args;

    ackInfo.packetId = 1U;
    ackInfo.pOriginalCommand = &command;
    mqttAgentContext.pPendingAcks[ 0 ] = ackInfo;

    /* Check that publish ack is resent successfully when session resumes. */
    MQTT_PublishToResend_ExpectAnyArgsAndReturn( 1 );
    MQTT_Publish_IgnoreAndReturn( MQTTSuccess );
    MQTT_PublishToResend_ExpectAnyArgsAndReturn( MQTT_PACKET_ID_INVALID );
    mqttStatus = MQTTAgent_ResumeSession( &mqttAgentContext, sessionPresent );
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
}


void test_MQTTAgent_ResumeSession_no_session_present( void )
{
    bool sessionPresent = true;
    MQTTStatus_t mqttStatus;
    MQTTAgentContext_t mqttAgentContext;
    Command_t command;
    AckInfo_t ackInfo, ackInfoCallback;

    setupAgentContext( &mqttAgentContext );

    mqttAgentContext.pPendingAcks[ 1 ].packetId = 1U;
    mqttAgentContext.pPendingAcks[ 1 ].pOriginalCommand = &command;
    /* Check that only acknowledgments with valid packet IDs are cleared. */
    mqttStatus = MQTTAgent_ResumeSession( &mqttAgentContext, false );
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    TEST_ASSERT_EQUAL( 0, mqttAgentContext.pPendingAcks[ 1 ].packetId );
    TEST_ASSERT_EQUAL( NULL, mqttAgentContext.pPendingAcks[ 1 ].pOriginalCommand );

    command.pCommandCompleteCallback = mockCommandCallback;
    ackInfoCallback.pOriginalCommand = &command;
    mqttAgentContext.pPendingAcks[ 1 ].packetId = 1U;
    mqttAgentContext.pPendingAcks[ 1 ].pOriginalCommand = &command;
    /* Check that command callback is called if it is specified to indicate network error. */
    mqttStatus = MQTTAgent_ResumeSession( &mqttAgentContext, false );
    TEST_ASSERT_EQUAL_INT( 1, commandCompleteCallbackCount );
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
}
