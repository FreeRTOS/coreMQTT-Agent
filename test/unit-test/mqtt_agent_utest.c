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


/* ============================   UNITY FIXTURES ============================ */

/* Called before each test method. */
void setUp()
{
    globalEntryTime = 0;
    commandReleaseCallCount = 0;
    globalMessageContext.pSentCommand = NULL;
    pCommandToReturn = NULL;
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

    MQTT_Init_ExpectAnyArgsAndReturn( MQTTSuccess );
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
static void invalidParamsTestFunc( MQTTStatus_t ( *FuncToTest )( MQTTAgentContext_t *, CommandInfo_t * ),
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

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgInterface, &networkBuffer, &transportInterface, stubGetTime, incomingCallback, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    msgInterface.pMsgCtx = &msg;

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgInterface, &networkBuffer, &transportInterface, stubGetTime, incomingCallback, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    msgInterface.send = stubSend;

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgInterface, &networkBuffer, &transportInterface, stubGetTime, incomingCallback, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    msgInterface.recv = stubReceive;

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgInterface, &networkBuffer, &transportInterface, stubGetTime, incomingCallback, incomingPacketContext );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    msgInterface.releaseCommand = stubReleaseCommand;

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
 * @brief Test that a bad parameter is returned when there
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
 * @brief Test that a bad parameter is returned when there
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
