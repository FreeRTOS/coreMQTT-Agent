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
#include "mock_mqtt_agent_command_functions.h"


/**
 * @brief The agent messaging context.
 */
struct AgentMessageContext
{
    Command_t * pSentCommand;
};

/**
 * @brief Command callback context.
 */
struct CommandContext
{
    MQTTStatus_t returnStatus;
};

/**
 * @brief Time at the beginning of each test. Note that this is not updated with
 * a real clock. Instead, we simply increment this variable.
 */
static uint32_t globalEntryTime = 0;

/**
 * @brief The number of times the release command function is called.
 */
static uint32_t commandReleaseCallCount = 0;

/**
 * @brief Message context to use for tests.
 */
static AgentMessageContext_t globalMessageContext;

/**
 * @brief Command struct pointer to return from mocked getCommand.
 */
static Command_t * pCommandToReturn;

/**
 * @brief Mock Counter variable to check callback is called on command completion.
 */
static uint32_t commandCompleteCallbackCount;

/**
 * @brief Mock Counter variable to check callback is called when incoming publish is received.
 */
static uint32_t publishCallbackCount;

/**
 * @brief Mock packet type to be used for testing all the received packet types via mqttEventcallback.
 */
static uint8_t packetType;

/**
 * @brief Mock packet identifier to be used for acknowledging received packets via mqttEventcallback.
 */
static uint16_t packetIdentifier;

/* ============================   UNITY FIXTURES ============================ */

/* Called before each test method. */
void setUp()
{
    globalEntryTime = 0;
    commandReleaseCallCount = 0;
    publishCallbackCount = 0;
    globalMessageContext.pSentCommand = NULL;
    pCommandToReturn = NULL;
    commandCompleteCallbackCount = 0;
    packetIdentifier = 1U;
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
 * @brief A mocked send function to send commands to the agent.
 */
static bool stubSend( AgentMessageContext_t * pMsgCtx,
                      const void * pData,
                      uint32_t blockTimeMs )
{
    Command_t ** pCommandToSend = ( Command_t ** ) pData;

    pMsgCtx->pSentCommand = *pCommandToSend;
    return true;
}

/**
 * @brief A mocked send function to send commands to the agent.
 * Returns failure.
 */
static bool stubSendFail( AgentMessageContext_t * pMsgCtx,
                          const void * pData,
                          uint32_t blockTimeMs )
{
    ( void ) pMsgCtx;
    ( void ) pData;
    ( void ) blockTimeMs;
    return false;
}

/**
 * @brief A mocked receive function for the agent to receive commands.
 */
static bool stubReceive( AgentMessageContext_t * pMsgCtx,
                         void * pBuffer,
                         uint32_t blockTimeMs )
{
    Command_t ** pCommandToReceive = ( Command_t ** ) pBuffer;

    *pCommandToReceive = pMsgCtx->pSentCommand;
    return true;
}

/**
 * @brief A mocked function to obtain an allocated command.
 */
static Command_t * stubGetCommand( uint32_t blockTimeMs )
{
    return pCommandToReturn;
}

/**
 * @brief A mocked function to release an allocated command.
 */
static bool stubReleaseCommand( Command_t * pCommandToRelease )
{
    ( void ) pCommandToRelease;
    commandReleaseCallCount++;
    return true;
}

/**
 * @brief A mock publish callback function.
 */
static void stubPublishCallback( MQTTAgentContext_t * pMqttAgentContext,
                                 uint16_t packetId,
                                 MQTTPublishInfo_t * pPublishInfo )
{
    ( void ) pMqttAgentContext;
    ( void ) packetId;
    ( void ) pPublishInfo;

    publishCallbackCount++;
}

static void stubCompletionCallback( void * pCommandCompletionContext,
                                    MQTTAgentReturnInfo_t * pReturnInfo )
{
    CommandContext_t * pCastContext;

    pCastContext = ( CommandContext_t * ) pCommandCompletionContext;

    if( pCastContext != NULL )
    {
        pCastContext->returnStatus = pReturnInfo->returnCode;
    }

    commandCompleteCallbackCount++;
}

/**
 * @brief A mocked timer query function that increments on every call.
 */
static uint32_t stubGetTime( void )
{
    return globalEntryTime++;
}

/**
 * @brief A stub for MQTT_Init function to be used to initialize the event callback.
 */
static MQTTStatus_t MQTT_Init_stub( MQTTContext_t * pContext,
                                    const TransportInterface_t * pTransport,
                                    MQTTGetCurrentTimeFunc_t getTimeFunc,
                                    MQTTEventCallback_t userCallback,
                                    const MQTTFixedBuffer_t * pNetworkBuffer )
{
    pContext->connectStatus = MQTTNotConnected;
    pContext->transportInterface = *pTransport;
    pContext->getTime = getTimeFunc;
    pContext->appCallback = userCallback;
    pContext->networkBuffer = *pNetworkBuffer;
    pContext->nextPacketId = 1;
    return MQTTSuccess;
}

/**
 * @brief A stub for MQTT_ProcessLoop function to be used to test the event callback.
 */
MQTTStatus_t MQTT_ProcessLoop_stub( MQTTContext_t * pContext,
                                    uint32_t timeoutMs )
{
    MQTTPacketInfo_t packetInfo;
    MQTTDeserializedInfo_t deserializedInfo;
    MQTTAgentContext_t * pMqttAgentContext;

    packetInfo.type = packetType;
    deserializedInfo.packetIdentifier = packetIdentifier;

    pContext->appCallback( pContext, &packetInfo, &deserializedInfo );
    pMqttAgentContext = ( MQTTAgentContext_t * ) pContext;
    pMqttAgentContext->packetReceivedInLoop = false;

    return MQTTSuccess;
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
    messageInterface.send = stubSend;
    messageInterface.recv = stubReceive;
    messageInterface.releaseCommand = stubReleaseCommand;
    messageInterface.getCommand = stubGetCommand;

    MQTT_Init_StubWithCallback( MQTT_Init_stub );
    mqttStatus = MQTTAgent_Init( pAgentContext,
                                 &messageInterface,
                                 &networkBuffer,
                                 &transportInterface,
                                 stubGetTime,
                                 stubPublishCallback,
                                 incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    /* Set packet ID nonzero to indicate initialization. */
    pAgentContext->mqttContext.nextPacketId = 1U;
}

/**
 * @brief Helper function to test API functions of the form
 * MQTTStatus_t func( MQTTAgentContext_t *, CommandInfo_t * )
 * for invalid parameters.
 *
 * @param[in] FuncToTest Pointer to function to test.
 * @param[in] pFuncName String of function name to print for error messages.
 */
static void invalidParamsTestFunc( MQTTStatus_t ( * FuncToTest )( MQTTAgentContext_t *, CommandInfo_t * ),
                                   const char * pFuncName )
{
    MQTTAgentContext_t agentContext = { 0 };
    MQTTStatus_t mqttStatus;
    CommandInfo_t commandInfo = { 0 };

    setupAgentContext( &agentContext );

    /* Test for NULL parameters. */
    mqttStatus = ( FuncToTest ) ( &agentContext, NULL );
    TEST_ASSERT_EQUAL_MESSAGE( MQTTBadParameter, mqttStatus, pFuncName );

    mqttStatus = ( FuncToTest ) ( NULL, &commandInfo );
    TEST_ASSERT_EQUAL_MESSAGE( MQTTBadParameter, mqttStatus, pFuncName );

    /* Various NULL parameters for the agent interface. */
    agentContext.agentInterface.send = NULL;
    mqttStatus = ( FuncToTest ) ( &agentContext, &commandInfo );
    TEST_ASSERT_EQUAL_MESSAGE( MQTTBadParameter, mqttStatus, pFuncName );

    agentContext.agentInterface.send = stubSend;
    agentContext.agentInterface.recv = NULL;
    mqttStatus = ( FuncToTest ) ( &agentContext, &commandInfo );
    TEST_ASSERT_EQUAL_MESSAGE( MQTTBadParameter, mqttStatus, pFuncName );

    agentContext.agentInterface.recv = stubReceive;
    agentContext.agentInterface.getCommand = NULL;
    mqttStatus = ( FuncToTest ) ( &agentContext, &commandInfo );
    TEST_ASSERT_EQUAL_MESSAGE( MQTTBadParameter, mqttStatus, pFuncName );

    agentContext.agentInterface.getCommand = stubGetCommand;
    agentContext.agentInterface.releaseCommand = NULL;
    mqttStatus = ( FuncToTest ) ( &agentContext, &commandInfo );
    TEST_ASSERT_EQUAL_MESSAGE( MQTTBadParameter, mqttStatus, pFuncName );

    agentContext.agentInterface.releaseCommand = stubReleaseCommand;
    agentContext.agentInterface.pMsgCtx = NULL;
    mqttStatus = ( FuncToTest ) ( &agentContext, &commandInfo );
    TEST_ASSERT_EQUAL_MESSAGE( MQTTBadParameter, mqttStatus, pFuncName );

    agentContext.agentInterface.pMsgCtx = &globalMessageContext;
    /* Invalid packet ID to indicate uninitialized context. */
    agentContext.mqttContext.nextPacketId = MQTT_PACKET_ID_INVALID;
    mqttStatus = ( FuncToTest ) ( &agentContext, &commandInfo );
    TEST_ASSERT_EQUAL_MESSAGE( MQTTBadParameter, mqttStatus, pFuncName );
}

/* ========================================================================== */

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
    msgInterface.send = stubSend;
    msgInterface.recv = stubReceive;
    msgInterface.releaseCommand = stubReleaseCommand;
    msgInterface.getCommand = stubGetCommand;

    MQTT_Init_ExpectAnyArgsAndReturn( MQTTSuccess );
    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgInterface, &networkBuffer, &transportInterface, stubGetTime, stubPublishCallback, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    TEST_ASSERT_EQUAL_PTR( stubPublishCallback, mqttAgentContext.pIncomingCallback );
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
    IncomingPublishCallback_t incomingCallback = stubPublishCallback;
    void * incomingPacketContext;
    AgentMessageContext_t msg;
    MQTTStatus_t mqttStatus;

    msgInterface.pMsgCtx = &msg;
    msgInterface.send = stubSend;
    msgInterface.recv = stubReceive;
    msgInterface.getCommand = stubGetCommand;
    msgInterface.releaseCommand = stubReleaseCommand;

    /* Check that MQTTBadParameter is returned if any NULL parameters are passed. */
    mqttStatus = MQTTAgent_Init( NULL, &msgInterface, &networkBuffer, &transportInterface, stubGetTime, incomingCallback, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, NULL, &networkBuffer, &transportInterface, stubGetTime, incomingCallback, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgInterface, &networkBuffer, NULL, stubGetTime, incomingCallback, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    /* Test if NULL is passed for any of the function pointers. */
    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgInterface, &networkBuffer, &transportInterface, stubGetTime, NULL, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgInterface, &networkBuffer, &transportInterface, NULL, incomingCallback, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    msgInterface.pMsgCtx = NULL;
    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgInterface, &networkBuffer, &transportInterface, stubGetTime, incomingCallback, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    msgInterface.pMsgCtx = &msg;
    msgInterface.send = NULL;
    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgInterface, &networkBuffer, &transportInterface, stubGetTime, incomingCallback, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    msgInterface.send = stubSend;
    msgInterface.recv = NULL;
    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgInterface, &networkBuffer, &transportInterface, stubGetTime, incomingCallback, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    msgInterface.recv = stubReceive;
    msgInterface.releaseCommand = NULL;
    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgInterface, &networkBuffer, &transportInterface, stubGetTime, incomingCallback, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    msgInterface.releaseCommand = stubReleaseCommand;
    msgInterface.getCommand = NULL;
    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgInterface, &networkBuffer, &transportInterface, stubGetTime, incomingCallback, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    msgInterface.getCommand = stubGetCommand;

    /* MQTT_Init() should check the network buffer. */
    MQTT_Init_ExpectAnyArgsAndReturn( MQTTBadParameter );
    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgInterface, NULL, &transportInterface, stubGetTime, incomingCallback, incomingPacketContext );
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

void test_MQTTAgent_ResumeSession_session_present_no_publish_found( void )
{
    bool sessionPresent = true;
    MQTTStatus_t mqttStatus;
    MQTTAgentContext_t mqttAgentContext;

    setupAgentContext( &mqttAgentContext );

    MQTT_PublishToResend_ExpectAnyArgsAndReturn( 2 );
    mqttAgentContext.pPendingAcks[ 0 ].packetId = 1U;
    MQTT_PublishToResend_ExpectAnyArgsAndReturn( MQTT_PACKET_ID_INVALID );
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
    Command_t command = { 0 };

    setupAgentContext( &mqttAgentContext );

    command.pCommandCompleteCallback = NULL;
    mqttAgentContext.pPendingAcks[ 1 ].packetId = 1U;
    mqttAgentContext.pPendingAcks[ 1 ].pOriginalCommand = &command;
    /* Check that only acknowledgments with valid packet IDs are cleared. */
    mqttStatus = MQTTAgent_ResumeSession( &mqttAgentContext, false );
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    TEST_ASSERT_EQUAL( 0, mqttAgentContext.pPendingAcks[ 1 ].packetId );
    TEST_ASSERT_EQUAL( NULL, mqttAgentContext.pPendingAcks[ 1 ].pOriginalCommand );

    command.pCommandCompleteCallback = stubCompletionCallback;
    mqttAgentContext.pPendingAcks[ 1 ].packetId = 1U;
    mqttAgentContext.pPendingAcks[ 1 ].pOriginalCommand = &command;
    /* Check that command callback is called if it is specified to indicate network error. */
    mqttStatus = MQTTAgent_ResumeSession( &mqttAgentContext, false );
    TEST_ASSERT_EQUAL_INT( 1, commandCompleteCallbackCount );
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
}

/* ========================================================================== */

/**
 * @brief Test error cases when a command cannot be obtained.
 */
void test_MQTTAgent_Ping_Command_Allocation_Failure( void )
{
    MQTTAgentContext_t agentContext = { 0 };
    MQTTStatus_t mqttStatus;
    CommandInfo_t commandInfo = { 0 };
    Command_t command = { 0 };

    setupAgentContext( &agentContext );

    pCommandToReturn = NULL;
    mqttStatus = MQTTAgent_Ping( &agentContext, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTNoMemory, mqttStatus );
}

/**
 * @brief Test error case when a command cannot be enqueued.
 */
void test_MQTTAgent_Ping_Command_Send_Failure( void )
{
    MQTTAgentContext_t agentContext = { 0 };
    MQTTStatus_t mqttStatus;
    CommandInfo_t commandInfo = { 0 };
    Command_t command = { 0 };

    setupAgentContext( &agentContext );

    pCommandToReturn = &command;
    agentContext.agentInterface.send = stubSendFail;
    mqttStatus = MQTTAgent_Ping( &agentContext, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTSendFailed, mqttStatus );
    /* Test that the command was released. */
    TEST_ASSERT_EQUAL_INT( 1, commandReleaseCallCount );
    /* Also test that the command was set. */
    TEST_ASSERT_EQUAL( PING, command.commandType );
}

/**
 * @brief Test that an MQTTNoMemory error is returned when there
 * is no more space to store a pending acknowledgment for
 * a command that expects one.
 */
void test_MQTTAgent_Subscribe_No_Ack_Space( void )
{
    MQTTAgentContext_t agentContext;
    MQTTStatus_t mqttStatus;
    CommandInfo_t commandInfo = { 0 };
    Command_t command = { 0 };
    MQTTSubscribeInfo_t subscribeInfo = { 0 };
    MQTTAgentSubscribeArgs_t subscribeArgs = { 0 };
    int i;

    setupAgentContext( &agentContext );
    pCommandToReturn = &command;

    /* No space in pending ack list. */
    for( i = 0; i < MQTT_AGENT_MAX_OUTSTANDING_ACKS; i++ )
    {
        agentContext.pPendingAcks[ i ].packetId = ( i + 1 );
    }

    subscribeArgs.pSubscribeInfo = &subscribeInfo;
    subscribeArgs.numSubscriptions = 1U;
    mqttStatus = MQTTAgent_Subscribe( &agentContext, &subscribeArgs, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTNoMemory, mqttStatus );
}

/**
 * @brief Test MQTTAgent_Subscribe() with invalid parameters.
 */
void test_MQTTAgent_Subscribe_Invalid_Parameters( void )
{
    MQTTAgentContext_t agentContext;
    MQTTStatus_t mqttStatus;
    CommandInfo_t commandInfo = { 0 };
    Command_t command = { 0 };
    MQTTSubscribeInfo_t subscribeInfo = { 0 };
    MQTTAgentSubscribeArgs_t subscribeArgs = { 0 };

    setupAgentContext( &agentContext );
    pCommandToReturn = &command;

    /* NULL parameters. */
    mqttStatus = MQTTAgent_Subscribe( NULL, &subscribeArgs, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Subscribe( &agentContext, NULL, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Subscribe( &agentContext, &subscribeArgs, NULL );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    /* Incorrect subscribe args. */
    mqttStatus = MQTTAgent_Subscribe( &agentContext, &subscribeArgs, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    subscribeArgs.pSubscribeInfo = &subscribeInfo;
    subscribeArgs.numSubscriptions = 0U;
    mqttStatus = MQTTAgent_Subscribe( &agentContext, &subscribeArgs, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );
}

/**
 * @brief Test that MQTTAgent_Subscribe() works as intended.
 */
void test_MQTTAgent_Subscribe_success( void )
{
    MQTTAgentContext_t agentContext;
    MQTTStatus_t mqttStatus;
    CommandInfo_t commandInfo = { 0 };
    Command_t command = { 0 };
    MQTTSubscribeInfo_t subscribeInfo = { 0 };
    MQTTAgentSubscribeArgs_t subscribeArgs = { 0 };

    setupAgentContext( &agentContext );
    pCommandToReturn = &command;

    /* Success case. */
    subscribeArgs.pSubscribeInfo = &subscribeInfo;
    subscribeArgs.numSubscriptions = 1U;
    mqttStatus = MQTTAgent_Subscribe( &agentContext, &subscribeArgs, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    TEST_ASSERT_EQUAL_PTR( &command, globalMessageContext.pSentCommand );
    TEST_ASSERT_EQUAL( SUBSCRIBE, command.commandType );
    TEST_ASSERT_EQUAL_PTR( &subscribeArgs, command.pArgs );
    TEST_ASSERT_NULL( command.pCommandCompleteCallback );
}

/**
 * @brief Test MQTTAgent_Unsubscribe() with invalid parameters.
 */
void test_MQTTAgent_Unsubscribe_Invalid_Parameters( void )
{
    MQTTAgentContext_t agentContext;
    MQTTStatus_t mqttStatus;
    CommandInfo_t commandInfo = { 0 };
    Command_t command = { 0 };
    MQTTSubscribeInfo_t subscribeInfo = { 0 };
    MQTTAgentSubscribeArgs_t subscribeArgs = { 0 };

    setupAgentContext( &agentContext );
    pCommandToReturn = &command;

    /* NULL parameters. */
    mqttStatus = MQTTAgent_Unsubscribe( NULL, &subscribeArgs, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Unsubscribe( &agentContext, NULL, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Unsubscribe( &agentContext, &subscribeArgs, NULL );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    /* Incorrect subscribe args. */
    mqttStatus = MQTTAgent_Unsubscribe( &agentContext, &subscribeArgs, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    subscribeArgs.pSubscribeInfo = &subscribeInfo;
    subscribeArgs.numSubscriptions = 0U;
    mqttStatus = MQTTAgent_Unsubscribe( &agentContext, &subscribeArgs, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );
}

/**
 * @brief Test that MQTTAgent_Unsubscribe() works as intended.
 */
void test_MQTTAgent_Unsubscribe_success( void )
{
    MQTTAgentContext_t agentContext;
    MQTTStatus_t mqttStatus;
    CommandInfo_t commandInfo = { 0 };
    Command_t command = { 0 };
    MQTTSubscribeInfo_t subscribeInfo = { 0 };
    MQTTAgentSubscribeArgs_t subscribeArgs = { 0 };

    setupAgentContext( &agentContext );
    pCommandToReturn = &command;
    commandInfo.cmdCompleteCallback = stubCompletionCallback;

    /* Success case. */
    subscribeArgs.pSubscribeInfo = &subscribeInfo;
    subscribeArgs.numSubscriptions = 1U;
    mqttStatus = MQTTAgent_Unsubscribe( &agentContext, &subscribeArgs, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    TEST_ASSERT_EQUAL_PTR( &command, globalMessageContext.pSentCommand );
    TEST_ASSERT_EQUAL( UNSUBSCRIBE, command.commandType );
    TEST_ASSERT_EQUAL_PTR( &subscribeArgs, command.pArgs );
    TEST_ASSERT_EQUAL_PTR( stubCompletionCallback, command.pCommandCompleteCallback );
}

/* ========================================================================== */

/**
 * @brief Test MQTTAgent_Publish() with invalid parameters.
 */
void test_MQTTAgent_Publish_Invalid_Parameters( void )
{
    MQTTAgentContext_t agentContext = { 0 };
    MQTTStatus_t mqttStatus;
    CommandInfo_t commandInfo = { 0 };
    Command_t command = { 0 };
    MQTTPublishInfo_t publishInfo = { 0 };

    setupAgentContext( &agentContext );
    pCommandToReturn = &command;
    commandInfo.cmdCompleteCallback = stubCompletionCallback;
    /* Test topic name. */
    publishInfo.pTopicName = "test";
    publishInfo.topicNameLength = 4;

    /* NULL parameters. */
    mqttStatus = MQTTAgent_Publish( NULL, &publishInfo, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Publish( &agentContext, NULL, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Publish( &agentContext, &publishInfo, NULL );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    /* This needs to be large enough to hold the PUBLISH:
     * 1 byte: control header
     * 1 byte: remaining length
     * 2 bytes: topic name length
     * 1+ bytes: topic name.
     * For this test case, the buffer must have size at least
     * 1+1+2+4=8. */
    agentContext.mqttContext.networkBuffer.size = 6;
    mqttStatus = MQTTAgent_Publish( &agentContext, &publishInfo, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );
}

/**
 * @brief Test that an MQTTNoMemory error is returned when there
 * is no more space to store a pending acknowledgment for
 * a command that expects one.
 */
void test_MQTTAgent_Publish_No_Ack_Space( void )
{
    MQTTAgentContext_t agentContext = { 0 };
    MQTTStatus_t mqttStatus;
    CommandInfo_t commandInfo = { 0 };
    Command_t command = { 0 };
    MQTTPublishInfo_t publishInfo = { 0 };
    int i;

    setupAgentContext( &agentContext );
    pCommandToReturn = &command;
    commandInfo.cmdCompleteCallback = stubCompletionCallback;
    /* Test topic name. */
    publishInfo.pTopicName = "test";
    publishInfo.topicNameLength = 4;
    /* Ack space is only necessary for QoS > 0. */
    publishInfo.qos = MQTTQoS1;

    /* No space in pending ack list. */
    for( i = 0; i < MQTT_AGENT_MAX_OUTSTANDING_ACKS; i++ )
    {
        agentContext.pPendingAcks[ i ].packetId = ( i + 1 );
    }

    /* This needs to be large enough to hold the PUBLISH:
     * 1 byte: control header
     * 1 byte: remaining length
     * 2 bytes: topic name length
     * 1+ bytes: topic name.
     * For this test case, the buffer must have size at least
     * 1+1+2+4=8. */
    agentContext.mqttContext.networkBuffer.size = 10;
    mqttStatus = MQTTAgent_Publish( &agentContext, &publishInfo, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTNoMemory, mqttStatus );
}

/**
 * @brief Test that MQTTAgent_Publish() works as intended.
 */
void test_MQTTAgent_Publish_success( void )
{
    MQTTAgentContext_t agentContext = { 0 };
    MQTTStatus_t mqttStatus;
    CommandInfo_t commandInfo = { 0 };
    Command_t command = { 0 };
    MQTTPublishInfo_t publishInfo = { 0 };

    setupAgentContext( &agentContext );
    pCommandToReturn = &command;
    commandInfo.cmdCompleteCallback = stubCompletionCallback;
    /* Test topic name. */
    publishInfo.pTopicName = "test";
    publishInfo.topicNameLength = 4;

    /* This needs to be large enough to hold the PUBLISH:
     * 1 byte: control header
     * 1 byte: remaining length
     * 2 bytes: topic name length
     * 1+ bytes: topic name.
     * For this test case, the buffer must have size at least
     * 1+1+2+4=8. */
    agentContext.mqttContext.networkBuffer.size = 10;
    mqttStatus = MQTTAgent_Publish( &agentContext, &publishInfo, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    TEST_ASSERT_EQUAL_PTR( &command, globalMessageContext.pSentCommand );
    TEST_ASSERT_EQUAL( PUBLISH, command.commandType );
    TEST_ASSERT_EQUAL_PTR( &publishInfo, command.pArgs );
    TEST_ASSERT_EQUAL_PTR( stubCompletionCallback, command.pCommandCompleteCallback );
}

/* ========================================================================== */

/**
 * @brief Test MQTTAgent_Connect() with invalid parameters.
 */
void test_MQTTAgent_Connect_Invalid_Parameters( void )
{
    MQTTAgentContext_t agentContext = { 0 };
    MQTTStatus_t mqttStatus;
    CommandInfo_t commandInfo = { 0 };
    Command_t command = { 0 };
    MQTTAgentConnectArgs_t connectArgs = { 0 };
    MQTTConnectInfo_t connectInfo = { 0 };

    setupAgentContext( &agentContext );
    pCommandToReturn = &command;
    commandInfo.cmdCompleteCallback = stubCompletionCallback;
    connectArgs.pConnectInfo = &connectInfo;

    /* NULL parameters. */
    mqttStatus = MQTTAgent_Connect( NULL, &connectArgs, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Connect( &agentContext, NULL, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Connect( &agentContext, &connectArgs, NULL );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    /* Invalid argument. */
    connectArgs.pConnectInfo = NULL;
    mqttStatus = MQTTAgent_Connect( &agentContext, &connectArgs, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );
}

/**
 * @brief Test that MQTTAgent_Connect() works as intended.
 */
void test_MQTTAgent_Connect_success( void )
{
    MQTTAgentContext_t agentContext;
    MQTTStatus_t mqttStatus;
    CommandInfo_t commandInfo = { 0 };
    Command_t command = { 0 };
    MQTTAgentConnectArgs_t connectArgs = { 0 };
    MQTTConnectInfo_t connectInfo = { 0 };

    setupAgentContext( &agentContext );
    pCommandToReturn = &command;
    commandInfo.cmdCompleteCallback = stubCompletionCallback;

    /* Success case. */
    connectArgs.pConnectInfo = &connectInfo;
    mqttStatus = MQTTAgent_Connect( &agentContext, &connectArgs, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    TEST_ASSERT_EQUAL_PTR( &command, globalMessageContext.pSentCommand );
    TEST_ASSERT_EQUAL( CONNECT, command.commandType );
    TEST_ASSERT_EQUAL_PTR( &connectArgs, command.pArgs );
    TEST_ASSERT_EQUAL_PTR( stubCompletionCallback, command.pCommandCompleteCallback );
}

/* ========================================================================== */

/**
 * @brief Test MQTTAgent_ProcessLoop() with invalid parameters.
 */
void test_MQTTAgent_ProcessLoop_Invalid_Params( void )
{
    /* Call the common helper function with the function pointer and name. */
    invalidParamsTestFunc( MQTTAgent_ProcessLoop, "Function=MQTTAgent_ProcessLoop" );
}

/**
 * @brief Test that MQTTAgent_ProcessLoop() works as intended.
 */
void test_MQTTAgent_ProcessLoop_success( void )
{
    MQTTAgentContext_t agentContext;
    MQTTStatus_t mqttStatus;
    CommandInfo_t commandInfo = { 0 };
    Command_t command = { 0 };

    setupAgentContext( &agentContext );
    pCommandToReturn = &command;

    /* Success case. */
    mqttStatus = MQTTAgent_ProcessLoop( &agentContext, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    TEST_ASSERT_EQUAL_PTR( &command, globalMessageContext.pSentCommand );
    TEST_ASSERT_EQUAL( PROCESSLOOP, command.commandType );
    TEST_ASSERT_NULL( command.pArgs );
    TEST_ASSERT_NULL( command.pCommandCompleteCallback );
}

/* ========================================================================== */

/**
 * @brief Test MQTTAgent_Disconnect() with invalid parameters.
 */
void test_MQTTAgent_Disconnect_Invalid_Params( void )
{
    /* Call the common helper function with the function pointer and name. */
    invalidParamsTestFunc( MQTTAgent_Disconnect, "Function=MQTTAgent_Disconnect" );
}

/**
 * @brief Test that MQTTAgent_Disconnect() works as intended.
 */
void test_MQTTAgent_Disconnect_success( void )
{
    MQTTAgentContext_t agentContext;
    MQTTStatus_t mqttStatus;
    CommandInfo_t commandInfo = { 0 };
    Command_t command = { 0 };

    setupAgentContext( &agentContext );
    pCommandToReturn = &command;
    commandInfo.cmdCompleteCallback = stubCompletionCallback;

    /* Success case. */
    mqttStatus = MQTTAgent_Disconnect( &agentContext, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    TEST_ASSERT_EQUAL_PTR( &command, globalMessageContext.pSentCommand );
    TEST_ASSERT_EQUAL( DISCONNECT, command.commandType );
    TEST_ASSERT_NULL( command.pArgs );
    TEST_ASSERT_EQUAL_PTR( stubCompletionCallback, command.pCommandCompleteCallback );
}

/* ========================================================================== */

/**
 * @brief Test MQTTAgent_Disconnect() with invalid parameters.
 */
void test_MQTTAgent_Ping_Invalid_Params( void )
{
    /* Call the common helper function with the function pointer and name. */
    invalidParamsTestFunc( MQTTAgent_Ping, "Function=MQTTAgent_Ping" );
}

/**
 * @brief Test that MQTTAgent_Ping() works as intended.
 */
void test_MQTTAgent_Ping_success( void )
{
    MQTTAgentContext_t agentContext;
    MQTTStatus_t mqttStatus;
    CommandInfo_t commandInfo = { 0 };
    Command_t command = { 0 };

    setupAgentContext( &agentContext );
    pCommandToReturn = &command;
    commandInfo.cmdCompleteCallback = stubCompletionCallback;

    /* Success case. */
    mqttStatus = MQTTAgent_Ping( &agentContext, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    TEST_ASSERT_EQUAL_PTR( &command, globalMessageContext.pSentCommand );
    TEST_ASSERT_EQUAL( PING, command.commandType );
    TEST_ASSERT_NULL( command.pArgs );
    TEST_ASSERT_EQUAL_PTR( stubCompletionCallback, command.pCommandCompleteCallback );
}

/* ========================================================================== */

/**
 * @brief Test MQTTAgent_Terminate() with invalid parameters.
 */
void test_MQTTAgent_Terminate_Invalid_Params( void )
{
    /* Call the common helper function with the function pointer and name. */
    invalidParamsTestFunc( MQTTAgent_Terminate, "Function=MQTTAgent_Terminate" );
}

/**
 * @brief Test that MQTTAgent_Terminate() works as intended.
 */
void test_MQTTAgent_Terminate_success( void )
{
    MQTTAgentContext_t agentContext;
    MQTTStatus_t mqttStatus;
    CommandInfo_t commandInfo = { 0 };
    Command_t command = { 0 };

    setupAgentContext( &agentContext );
    pCommandToReturn = &command;
    commandInfo.cmdCompleteCallback = stubCompletionCallback;

    /* Success case. */
    mqttStatus = MQTTAgent_Terminate( &agentContext, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    TEST_ASSERT_EQUAL_PTR( &command, globalMessageContext.pSentCommand );
    TEST_ASSERT_EQUAL( TERMINATE, command.commandType );
    TEST_ASSERT_NULL( command.pArgs );
    TEST_ASSERT_EQUAL_PTR( stubCompletionCallback, command.pCommandCompleteCallback );
}

/**
 * @brief Test MQTTAgent_CommandLoop behavior with invalid params.
 */
void test_MQTTAgent_CommandLoop_invalid_params( void )
{
    MQTTStatus_t mqttStatus;
    MQTTAgentContext_t mqttAgentContext;

    mqttStatus = MQTTAgent_CommandLoop( NULL );

    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    setupAgentContext( &mqttAgentContext );

    mqttAgentContext.agentInterface.pMsgCtx = NULL;

    mqttStatus = MQTTAgent_CommandLoop( &mqttAgentContext );

    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );
}

/**
 * @brief Test MQTTAgent_CommandLoop behavior when there is no command in the command Queue.
 */
void test_MQTTAgent_CommandLoop_with_empty_command_queue( void )
{
    MQTTStatus_t mqttStatus;
    MQTTAgentContext_t mqttAgentContext;
    MQTTAgentCommandFuncReturns_t pReturnFlags;

    setupAgentContext( &mqttAgentContext );

    mqttAgentContext.mqttContext.connectStatus = MQTTConnected;
    pReturnFlags.addAcknowledgment = false;
    pReturnFlags.runProcessLoop = true;
    pReturnFlags.endLoop = true;

    MQTTAgentCommand_ProcessLoop_ExpectAnyArgsAndReturn( MQTTSuccess );
    MQTTAgentCommand_ProcessLoop_ReturnThruPtr_pReturnFlags( &pReturnFlags );
    MQTT_ProcessLoop_IgnoreAndReturn( MQTTSuccess );

    mqttStatus = MQTTAgent_CommandLoop( &mqttAgentContext );

    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
}

/**
 * @brief Test MQTTAgent_CommandLoop behavior when there is command to be processed in the command queue.
 */
void test_MQTTAgent_CommandLoop_process_commands_in_command_queue( void )
{
    MQTTStatus_t mqttStatus;
    MQTTAgentContext_t mqttAgentContext;
    MQTTAgentCommandFuncReturns_t pReturnFlags;
    Command_t commandToSend;

    setupAgentContext( &mqttAgentContext );
    mqttAgentContext.mqttContext.connectStatus = MQTTConnected;
    pReturnFlags.addAcknowledgment = false;
    pReturnFlags.runProcessLoop = true;
    pReturnFlags.endLoop = true;

    MQTTAgentCommand_Publish_ExpectAnyArgsAndReturn( MQTTSuccess );
    MQTTAgentCommand_Publish_ReturnThruPtr_pReturnFlags( &pReturnFlags );
    MQTT_ProcessLoop_IgnoreAndReturn( MQTTSuccess );

    /* Initializing command to be sent to the commandLoop. */
    commandToSend.commandType = PUBLISH;
    commandToSend.pCommandCompleteCallback = stubCompletionCallback;
    commandToSend.pCmdContext = NULL;
    commandToSend.pArgs = NULL;
    mqttAgentContext.agentInterface.pMsgCtx->pSentCommand = &commandToSend;

    mqttStatus = MQTTAgent_CommandLoop( &mqttAgentContext );

    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    /* Ensure that callback is invoked. */
    TEST_ASSERT_EQUAL( 1, commandCompleteCallbackCount );

    /* Test case when processing a particular command fails. */
    MQTTAgentCommand_Publish_ExpectAnyArgsAndReturn( MQTTBadParameter );
    MQTTAgentCommand_Publish_ReturnThruPtr_pReturnFlags( &pReturnFlags );

    mqttStatus = MQTTAgent_CommandLoop( &mqttAgentContext );

    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );
    TEST_ASSERT_EQUAL( 2, commandCompleteCallbackCount );
}

/**
 * @brief Test that MQTTAgent_CommandLoop adds acknowledgment in pendingAcks for the commands requiring
 * acknowledgments.
 */
void test_MQTTAgent_CommandLoop_add_acknowledgment_success( void )
{
    MQTTStatus_t mqttStatus;
    MQTTAgentContext_t mqttAgentContext;
    MQTTAgentCommandFuncReturns_t pReturnFlags;
    Command_t commandToSend;

    setupAgentContext( &mqttAgentContext );
    mqttAgentContext.mqttContext.connectStatus = MQTTConnected;
    pReturnFlags.addAcknowledgment = true;
    pReturnFlags.runProcessLoop = true;
    pReturnFlags.endLoop = true;
    pReturnFlags.packetId = 1U;

    MQTTAgentCommand_Publish_ExpectAnyArgsAndReturn( MQTTSuccess );
    MQTTAgentCommand_Publish_ReturnThruPtr_pReturnFlags( &pReturnFlags );
    MQTT_ProcessLoop_IgnoreAndReturn( MQTTSuccess );

    /* Initializing command to be sent to the commandLoop. */
    commandToSend.commandType = PUBLISH;
    commandToSend.pCommandCompleteCallback = stubCompletionCallback;
    commandToSend.pCmdContext = NULL;
    commandToSend.pArgs = NULL;

    mqttAgentContext.agentInterface.pMsgCtx->pSentCommand = &commandToSend;
    mqttStatus = MQTTAgent_CommandLoop( &mqttAgentContext );
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );

    /* Ensure that acknowledgment is added. */
    TEST_ASSERT_EQUAL( 1, mqttAgentContext.pPendingAcks[ 0 ].packetId );
    TEST_ASSERT_EQUAL_PTR( &commandToSend, mqttAgentContext.pPendingAcks[ 0 ].pOriginalCommand );
    /* Ensure that callback is not invoked. */
    TEST_ASSERT_EQUAL( 0, commandCompleteCallbackCount );
}

/**
 * @brief Test that MQTTAgent_CommandLoop returns MQTTNoMemory if
 * pPendingAcks array is full.
 */
void test_MQTTAgent_CommandLoop_add_acknowledgment_failure( void )
{
    MQTTStatus_t mqttStatus;
    MQTTAgentContext_t mqttAgentContext;
    MQTTAgentCommandFuncReturns_t pReturnFlags;
    AckInfo_t * pendingAcks = mqttAgentContext.pPendingAcks;
    size_t i = 0;
    Command_t commandToSend;

    setupAgentContext( &mqttAgentContext );
    mqttAgentContext.mqttContext.connectStatus = MQTTConnected;
    pReturnFlags.addAcknowledgment = true;
    pReturnFlags.runProcessLoop = false;
    pReturnFlags.endLoop = false;
    pReturnFlags.packetId = 1U;


    MQTTAgentCommand_Publish_ExpectAnyArgsAndReturn( MQTTSuccess );
    MQTTAgentCommand_Publish_ReturnThruPtr_pReturnFlags( &pReturnFlags );
    MQTT_ProcessLoop_IgnoreAndReturn( MQTTSuccess );

    /* Initializing command to be sent to the commandLoop. */
    commandToSend.commandType = PUBLISH;
    commandToSend.pCommandCompleteCallback = stubCompletionCallback;
    commandToSend.pCmdContext = NULL;
    commandToSend.pArgs = NULL;

    for( i = 0; i < MQTT_AGENT_MAX_OUTSTANDING_ACKS; i++ )
    {
        /* Assigning valid packet ID to all array spaces to make no space for incoming acknowledgment. */
        pendingAcks[ i ].packetId = i + 1;
    }

    mqttAgentContext.agentInterface.pMsgCtx->pSentCommand = &commandToSend;

    mqttStatus = MQTTAgent_CommandLoop( &mqttAgentContext );

    TEST_ASSERT_EQUAL( MQTTNoMemory, mqttStatus );

    /* Ensure that callback is invoked. */
    TEST_ASSERT_EQUAL( 1, commandCompleteCallbackCount );
}

/**
 * @brief Test mqttEventCallback invocation via MQTT_ProcessLoop.
 */
void test_MQTTAgent_CommandLoop_with_eventCallback( void )
{
    MQTTStatus_t mqttStatus;
    MQTTAgentContext_t mqttAgentContext;
    MQTTAgentCommandFuncReturns_t pReturnFlags;
    Command_t command, commandToSend;

    /* Setting up MQTT Agent Context. */
    setupAgentContext( &mqttAgentContext );

    mqttAgentContext.mqttContext.connectStatus = MQTTConnected;
    pReturnFlags.addAcknowledgment = false;
    pReturnFlags.runProcessLoop = true;
    pReturnFlags.endLoop = true;

    /* Initializing command to be sent to the commandLoop. */
    commandToSend.commandType = PUBLISH;
    commandToSend.pCommandCompleteCallback = stubCompletionCallback;
    commandToSend.pCmdContext = NULL;
    commandToSend.pArgs = NULL;

    mqttAgentContext.agentInterface.pMsgCtx->pSentCommand = &commandToSend;

    /* Invoking mqttEventCallback with MQTT_PACKET_TYPE_PUBREL packet type.
     * MQTT_PACKET_TYPE_PUBREC packet type code path will also be covered
     * by this test case. */
    packetType = MQTT_PACKET_TYPE_PUBREL;

    MQTTAgentCommand_Publish_ExpectAnyArgsAndReturn( MQTTSuccess );
    MQTTAgentCommand_Publish_ReturnThruPtr_pReturnFlags( &pReturnFlags );
    MQTT_ProcessLoop_StubWithCallback( MQTT_ProcessLoop_stub );

    mqttStatus = MQTTAgent_CommandLoop( &mqttAgentContext );

    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    /* Ensure that callback is invoked. */
    TEST_ASSERT_EQUAL( 1, commandCompleteCallbackCount );

    /* Invoking mqttEventCallback with MQTT_PACKET_TYPE_PINGRESP packet type. */
    packetType = MQTT_PACKET_TYPE_PINGRESP;

    MQTTAgentCommand_Publish_ExpectAnyArgsAndReturn( MQTTSuccess );
    MQTTAgentCommand_Publish_ReturnThruPtr_pReturnFlags( &pReturnFlags );
    MQTT_ProcessLoop_StubWithCallback( MQTT_ProcessLoop_stub );

    mqttStatus = MQTTAgent_CommandLoop( &mqttAgentContext );

    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    /* Ensure that callback is invoked. */
    TEST_ASSERT_EQUAL( 2, commandCompleteCallbackCount );


    /* Invoking mqttEventCallback with MQTT_PACKET_TYPE_PUBACK packet type.
     * MQTT_PACKET_TYPE_PUBCOMP, MQTT_PACKET_TYPE_SUBACK, MQTT_PACKET_TYPE_UNSUBACK
     * packet types code path will also be covered by this test case. */
    packetType = MQTT_PACKET_TYPE_PUBACK;

    mqttAgentContext.pPendingAcks[ 0 ].packetId = 1U;
    command.pCommandCompleteCallback = stubCompletionCallback;
    mqttAgentContext.pPendingAcks[ 0 ].pOriginalCommand = &command;

    MQTTAgentCommand_Publish_ExpectAnyArgsAndReturn( MQTTSuccess );
    MQTTAgentCommand_Publish_ReturnThruPtr_pReturnFlags( &pReturnFlags );
    MQTT_ProcessLoop_StubWithCallback( MQTT_ProcessLoop_stub );

    mqttStatus = MQTTAgent_CommandLoop( &mqttAgentContext );

    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    /* Ensure that callback is invoked. */
    TEST_ASSERT_EQUAL( 4, commandCompleteCallbackCount );
    /* Ensure that acknowledgment is cleared. */
    TEST_ASSERT_EQUAL( 0, mqttAgentContext.pPendingAcks[ 0 ].packetId );
    TEST_ASSERT_EQUAL( NULL, mqttAgentContext.pPendingAcks[ 0 ].pOriginalCommand );

    /* mqttEventcallback behavior when the command for the pending ack is NULL for the received PUBACK. */
    mqttAgentContext.pPendingAcks[ 0 ].packetId = 1U;
    mqttAgentContext.pPendingAcks[ 0 ].pOriginalCommand = NULL;
    MQTTAgentCommand_Publish_ExpectAnyArgsAndReturn( MQTTSuccess );
    MQTTAgentCommand_Publish_ReturnThruPtr_pReturnFlags( &pReturnFlags );
    MQTT_ProcessLoop_StubWithCallback( MQTT_ProcessLoop_stub );

    mqttStatus = MQTTAgent_CommandLoop( &mqttAgentContext );

    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    /* Ensure that callback is invoked. */
    TEST_ASSERT_EQUAL( 5, commandCompleteCallbackCount );

    /* mqttEventcallback behavior when the packet Id 0 is received in the PUBACK. */
    packetIdentifier = 0U;
    mqttAgentContext.pPendingAcks[ 0 ].packetId = 0U;
    command.pCommandCompleteCallback = stubCompletionCallback;
    mqttAgentContext.pPendingAcks[ 0 ].pOriginalCommand = &command;

    MQTTAgentCommand_Publish_ExpectAnyArgsAndReturn( MQTTSuccess );
    MQTTAgentCommand_Publish_ReturnThruPtr_pReturnFlags( &pReturnFlags );
    MQTT_ProcessLoop_StubWithCallback( MQTT_ProcessLoop_stub );

    mqttStatus = MQTTAgent_CommandLoop( &mqttAgentContext );

    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    /* Ensure that callback is invoked. */
    TEST_ASSERT_EQUAL( 6, commandCompleteCallbackCount );

    /* Test case checking no corresponding ack exists for the PUBACK received in the pPendingAcks array. */
    packetIdentifier = 1U;

    MQTTAgentCommand_Publish_ExpectAnyArgsAndReturn( MQTTSuccess );
    MQTTAgentCommand_Publish_ReturnThruPtr_pReturnFlags( &pReturnFlags );
    MQTT_ProcessLoop_StubWithCallback( MQTT_ProcessLoop_stub );

    mqttStatus = MQTTAgent_CommandLoop( &mqttAgentContext );

    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    /* Ensure that callback is invoked. */
    TEST_ASSERT_EQUAL( 7, commandCompleteCallbackCount );

    /* Invoking mqttEventCallback with MQTT_PACKET_TYPE_SUBACK packet type. */
    packetType = MQTT_PACKET_TYPE_SUBACK;

    mqttAgentContext.pPendingAcks[ 0 ].packetId = 1U;
    command.pCommandCompleteCallback = NULL;
    mqttAgentContext.pPendingAcks[ 0 ].pOriginalCommand = &command;


    MQTTAgentCommand_Publish_ExpectAnyArgsAndReturn( MQTTSuccess );
    MQTTAgentCommand_Publish_ReturnThruPtr_pReturnFlags( &pReturnFlags );
    MQTT_ProcessLoop_StubWithCallback( MQTT_ProcessLoop_stub );

    mqttStatus = MQTTAgent_CommandLoop( &mqttAgentContext );

    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    /* Ensure that callback is invoked. */
    TEST_ASSERT_EQUAL( 8, commandCompleteCallbackCount );
    /* Ensure that acknowledgment is cleared. */
    TEST_ASSERT_EQUAL( 0, mqttAgentContext.pPendingAcks[ 0 ].packetId );
    TEST_ASSERT_EQUAL( NULL, mqttAgentContext.pPendingAcks[ 0 ].pOriginalCommand );

    /* Invoking mqttEventCallback with MQTT_PACKET_TYPE_PUBLISH packet type. */
    packetType = MQTT_PACKET_TYPE_PUBLISH;

    MQTTAgentCommand_Publish_ExpectAnyArgsAndReturn( MQTTSuccess );
    MQTTAgentCommand_Publish_ReturnThruPtr_pReturnFlags( &pReturnFlags );
    MQTT_ProcessLoop_StubWithCallback( MQTT_ProcessLoop_stub );

    mqttStatus = MQTTAgent_CommandLoop( &mqttAgentContext );

    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    /* Ensure that callback is invoked. */
    TEST_ASSERT_EQUAL( 9, commandCompleteCallbackCount );
    TEST_ASSERT_EQUAL( 1, publishCallbackCount );

    /* Test case when completion callback is NULL. */
    commandToSend.pCommandCompleteCallback = NULL;

    MQTTAgentCommand_Publish_ExpectAnyArgsAndReturn( MQTTSuccess );
    MQTTAgentCommand_Publish_ReturnThruPtr_pReturnFlags( &pReturnFlags );
    MQTT_ProcessLoop_StubWithCallback( MQTT_ProcessLoop_stub );

    mqttStatus = MQTTAgent_CommandLoop( &mqttAgentContext );

    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    /* Ensure that callback is not invoked. */
    TEST_ASSERT_EQUAL( 9, commandCompleteCallbackCount );
}
