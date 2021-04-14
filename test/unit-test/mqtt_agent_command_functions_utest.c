/*
 * coreMQTT Agent v1.0.0
 * Copyright (C) 2021 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
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
 * @file mqtt_agent_command_functions_utest.c
 * @brief Unit tests for functions in mqtt_agent_command_functions_utest.h
 */
#include <string.h>
#include <stdbool.h>

#include "unity.h"

/* Include paths for public enums, structures, and macros. */

#include "mock_core_mqtt.h"
#include "mock_core_mqtt_state.h"
#include "mock_mqtt_agent.h"
#include "mqtt_agent_command_functions.h"

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
 * @brief Mock Counter variable for calling stubReceive multiple times.
 */
static uint32_t receiveCounter;

/**
 * @brief The number of times the release command function is called.
 */
static uint32_t commandReleaseCallCount = 0;


/* ========================================================================== */

/**
 * @brief A mocked send function to send commands to the agent.
 */
static bool stubSend( AgentMessageContext_t * pMsgCtx,
                      Command_t * const * pCommandToSend,
                      uint32_t blockTimeMs )
{
    ( void ) blockTimeMs;
    pMsgCtx->pSentCommand = *pCommandToSend;
    return true;
}

/**
 * @brief A mocked receive function for the agent to receive commands.
 */
static bool stubReceive( AgentMessageContext_t * pMsgCtx,
                         Command_t ** pReceivedCommand,
                         uint32_t blockTimeMs )
{
    bool ret = false;

    ( void ) blockTimeMs;

    if( receiveCounter++ == 0 )
    {
        *pReceivedCommand = pMsgCtx->pSentCommand;
        ret = true;
    }

    return ret;
}

/**
 * @brief A mocked function to obtain an allocated command.
 */
static Command_t * stubGetCommand( uint32_t blockTimeMs )
{
    ( void ) blockTimeMs;
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
 * @brief A mocked callback function used for testing.
 */
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

/* ============================   UNITY FIXTURES ============================ */

/* Called before each test method. */
void setUp()
{
    globalMessageContext.pSentCommand = NULL;
    pCommandToReturn = NULL;
    commandCompleteCallbackCount = 0;
    commandReleaseCallCount = 0;
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
 * @brief Test that MQTTAgentCommand_ProcessLoop() works as intended.
 */
void test_MQTTAgentCommand_ProcessLoop( void )
{
    MQTTAgentContext_t mqttAgentContext = { 0 };
    MQTTAgentCommandFuncReturns_t returnFlags = { 0 };
    MQTTStatus_t mqttStatus;

    mqttStatus = MQTTAgentCommand_ProcessLoop( &mqttAgentContext, NULL, &returnFlags );

    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    /* Ensure that returnFlags are set as intended. */
    TEST_ASSERT_EQUAL( 0, returnFlags.packetId );
    TEST_ASSERT_TRUE( returnFlags.runProcessLoop );
    TEST_ASSERT_FALSE( returnFlags.addAcknowledgment );
    TEST_ASSERT_FALSE( returnFlags.endLoop );
}

/**
 * @brief Test MQTTAgentCommand_Publish() success case with QoS0.
 */
void test_MQTTAgentCommand_Publish_QoS0_success( void )
{
    MQTTAgentContext_t mqttAgentContext = { 0 };
    MQTTPublishInfo_t publishInfo = { 0 };
    MQTTAgentCommandFuncReturns_t returnFlags = { 0 };
    MQTTStatus_t mqttStatus;

    /* Initializing QOS. */
    publishInfo.qos = MQTTQoS0;

    MQTT_Publish_ExpectAndReturn( &( mqttAgentContext.mqttContext ), &publishInfo, 0, MQTTSuccess );

    mqttStatus = MQTTAgentCommand_Publish( &mqttAgentContext, &publishInfo, &returnFlags );

    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    /* Ensure that returnFlags are set as intended. */
    TEST_ASSERT_EQUAL( 0, returnFlags.packetId );
    TEST_ASSERT_TRUE( returnFlags.runProcessLoop );
    TEST_ASSERT_FALSE( returnFlags.addAcknowledgment );
    TEST_ASSERT_FALSE( returnFlags.endLoop );
}

/**
 * @brief Test MQTTAgentCommand_Publish() success case with QoS1.
 */
void test_MQTTAgentCommand_Publish_QoS1_success( void )
{
    MQTTAgentContext_t mqttAgentContext = { 0 };
    MQTTPublishInfo_t publishInfo = { 0 };
    MQTTAgentCommandFuncReturns_t returnFlags = { 0 };
    MQTTStatus_t mqttStatus;

    /* Initializing QOS. */
    publishInfo.qos = MQTTQoS1;

    MQTT_GetPacketId_ExpectAndReturn( &( mqttAgentContext.mqttContext ), 1 );
    MQTT_Publish_ExpectAndReturn( &( mqttAgentContext.mqttContext ), &publishInfo, 1, MQTTSuccess );

    mqttStatus = MQTTAgentCommand_Publish( &mqttAgentContext, &publishInfo, &returnFlags );

    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    /* Ensure that returnFlags are set as intended. */
    TEST_ASSERT_EQUAL( 1, returnFlags.packetId );
    TEST_ASSERT_TRUE( returnFlags.runProcessLoop );
    TEST_ASSERT_TRUE( returnFlags.addAcknowledgment );
    TEST_ASSERT_FALSE( returnFlags.endLoop );
}

/**
 * @brief Test MQTTAgentCommand_Publish() failure case with QoS0.
 */
void test_MQTTAgentCommand_Publish_QoS0_failure( void )
{
    MQTTAgentContext_t mqttAgentContext = { 0 };
    MQTTPublishInfo_t publishInfo = { 0 };
    MQTTAgentCommandFuncReturns_t returnFlags = { 0 };
    MQTTStatus_t mqttStatus;

    /* Initializing QOS. */
    publishInfo.qos = MQTTQoS0;

    MQTT_Publish_ExpectAndReturn( &( mqttAgentContext.mqttContext ), &publishInfo, 0, MQTTSendFailed );

    mqttStatus = MQTTAgentCommand_Publish( &mqttAgentContext, &publishInfo, &returnFlags );

    TEST_ASSERT_EQUAL( MQTTSendFailed, mqttStatus );
    /* Ensure that returnFlags are set as intended. */
    TEST_ASSERT_EQUAL( 0, returnFlags.packetId );
    TEST_ASSERT_TRUE( returnFlags.runProcessLoop );
    TEST_ASSERT_FALSE( returnFlags.addAcknowledgment );
    TEST_ASSERT_FALSE( returnFlags.endLoop );
}

/**
 * @brief Test MQTTAgentCommand_Publish() failure case with QoS1.
 */
void test_MQTTAgentCommand_Publish_QoS1_failure( void )
{
    MQTTAgentContext_t mqttAgentContext = { 0 };
    MQTTPublishInfo_t publishInfo = { 0 };
    MQTTAgentCommandFuncReturns_t returnFlags = { 0 };
    MQTTStatus_t mqttStatus;

    /* Initializing QOS. */
    publishInfo.qos = MQTTQoS1;

    MQTT_GetPacketId_ExpectAndReturn( &( mqttAgentContext.mqttContext ), 1 );
    MQTT_Publish_ExpectAndReturn( &( mqttAgentContext.mqttContext ), &publishInfo, 1, MQTTSendFailed );

    mqttStatus = MQTTAgentCommand_Publish( &mqttAgentContext, &publishInfo, &returnFlags );

    TEST_ASSERT_EQUAL( MQTTSendFailed, mqttStatus );
    /* Ensure that returnFlags are set as intended. */
    TEST_ASSERT_EQUAL( 1, returnFlags.packetId );
    TEST_ASSERT_TRUE( returnFlags.runProcessLoop );
    TEST_ASSERT_FALSE( returnFlags.addAcknowledgment );
    TEST_ASSERT_FALSE( returnFlags.endLoop );
}

/**
 * @brief Test that MQTTAgentCommand_Subscribe() works as intended.
 */
void test_MQTTAgentCommand_Subscribe_Success( void )
{
    MQTTAgentContext_t mqttAgentContext = { 0 };
    MQTTAgentSubscribeArgs_t subscribeArgs = { 0 };
    MQTTAgentCommandFuncReturns_t returnFlags = { 0 };
    MQTTStatus_t mqttStatus;

    MQTT_GetPacketId_ExpectAndReturn( &( mqttAgentContext.mqttContext ), 1 );
    MQTT_Subscribe_ExpectAndReturn( &( mqttAgentContext.mqttContext ),
                                    subscribeArgs.pSubscribeInfo,
                                    subscribeArgs.numSubscriptions,
                                    1,
                                    MQTTSuccess );
    mqttStatus = MQTTAgentCommand_Subscribe( &mqttAgentContext, &subscribeArgs, &returnFlags );

    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    /* Ensure that returnFlags are set as intended. */
    TEST_ASSERT_EQUAL( 1, returnFlags.packetId );
    TEST_ASSERT_TRUE( returnFlags.runProcessLoop );
    TEST_ASSERT_TRUE( returnFlags.addAcknowledgment );
    TEST_ASSERT_FALSE( returnFlags.endLoop );
}

/**
 * @brief Test MQTTAgentCommand_Subscribe() failure case.
 */
void test_MQTTAgentCommand_Subscribe_failure( void )
{
    MQTTAgentContext_t mqttAgentContext = { 0 };
    MQTTAgentSubscribeArgs_t subscribeArgs = { 0 };
    MQTTAgentCommandFuncReturns_t returnFlags = { 0 };
    MQTTStatus_t mqttStatus;

    MQTT_GetPacketId_ExpectAndReturn( &( mqttAgentContext.mqttContext ), 1 );
    MQTT_Subscribe_ExpectAndReturn( &( mqttAgentContext.mqttContext ),
                                    subscribeArgs.pSubscribeInfo,
                                    subscribeArgs.numSubscriptions,
                                    1,
                                    MQTTSendFailed );
    mqttStatus = MQTTAgentCommand_Subscribe( &mqttAgentContext, &subscribeArgs, &returnFlags );

    TEST_ASSERT_EQUAL( MQTTSendFailed, mqttStatus );
    /* Ensure that returnFlags are set as intended. */
    TEST_ASSERT_EQUAL( 1, returnFlags.packetId );
    TEST_ASSERT_TRUE( returnFlags.runProcessLoop );
    TEST_ASSERT_FALSE( returnFlags.addAcknowledgment );
    TEST_ASSERT_FALSE( returnFlags.endLoop );
}

/**
 * @brief Test that MQTTAgentCommand_Unsubscribe() works as intended.
 */
void test_MQTTAgentCommand_Unsubscribe( void )
{
    MQTTAgentContext_t mqttAgentContext = { 0 };
    MQTTAgentSubscribeArgs_t subscribeArgs = { 0 };
    MQTTAgentCommandFuncReturns_t returnFlags = { 0 };
    MQTTStatus_t mqttStatus;

    MQTT_GetPacketId_ExpectAndReturn( &( mqttAgentContext.mqttContext ), 1 );
    MQTT_Unsubscribe_ExpectAndReturn( &( mqttAgentContext.mqttContext ),
                                      subscribeArgs.pSubscribeInfo,
                                      subscribeArgs.numSubscriptions,
                                      1,
                                      MQTTSuccess );
    mqttStatus = MQTTAgentCommand_Unsubscribe( &mqttAgentContext, &subscribeArgs, &returnFlags );

    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    /* Ensure that returnFlags are set as intended. */
    TEST_ASSERT_EQUAL( 1, returnFlags.packetId );
    TEST_ASSERT_TRUE( returnFlags.runProcessLoop );
    TEST_ASSERT_TRUE( returnFlags.addAcknowledgment );
    TEST_ASSERT_FALSE( returnFlags.endLoop );
}

/**
 * @brief Test MQTTAgentCommand_Unsubscribe() failure case.
 */
void test_MQTTAgentCommand_Unsubscribe_failure( void )
{
    MQTTAgentContext_t mqttAgentContext = { 0 };
    MQTTAgentSubscribeArgs_t subscribeArgs = { 0 };
    MQTTAgentCommandFuncReturns_t returnFlags = { 0 };
    MQTTStatus_t mqttStatus;

    MQTT_GetPacketId_ExpectAndReturn( &( mqttAgentContext.mqttContext ), 1 );
    MQTT_Unsubscribe_ExpectAndReturn( &( mqttAgentContext.mqttContext ),
                                      subscribeArgs.pSubscribeInfo,
                                      subscribeArgs.numSubscriptions,
                                      1,
                                      MQTTSendFailed );
    mqttStatus = MQTTAgentCommand_Unsubscribe( &mqttAgentContext, &subscribeArgs, &returnFlags );

    TEST_ASSERT_EQUAL( MQTTSendFailed, mqttStatus );
    /* Ensure that returnFlags are set as intended. */
    TEST_ASSERT_EQUAL( 1, returnFlags.packetId );
    TEST_ASSERT_TRUE( returnFlags.runProcessLoop );
    TEST_ASSERT_FALSE( returnFlags.addAcknowledgment );
    TEST_ASSERT_FALSE( returnFlags.endLoop );
}

/**
 * @brief Test that MQTTAgentCommand_Disconnect() works as intended.
 */
void test_MQTTAgentCommand_Disconnect( void )
{
    MQTTAgentContext_t mqttAgentContext = { 0 };
    MQTTAgentCommandFuncReturns_t returnFlags = { 0 };
    MQTTStatus_t mqttStatus;

    MQTT_Disconnect_ExpectAndReturn( &( mqttAgentContext.mqttContext ), MQTTSuccess );
    mqttStatus = MQTTAgentCommand_Disconnect( &mqttAgentContext, NULL, &returnFlags );

    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    /* Ensure that returnFlags are set as intended. */
    TEST_ASSERT_EQUAL( 0, returnFlags.packetId );
    TEST_ASSERT_TRUE( returnFlags.endLoop );
    TEST_ASSERT_FALSE( returnFlags.addAcknowledgment );
    TEST_ASSERT_FALSE( returnFlags.runProcessLoop );
}

/**
 * @brief Test MQTTAgentCommand_Disconnect() failure case.
 */
void test_MQTTAgentCommand_Disconnect_failure( void )
{
    MQTTAgentContext_t mqttAgentContext = { 0 };
    MQTTAgentCommandFuncReturns_t returnFlags = { 0 };
    MQTTStatus_t mqttStatus;

    MQTT_Disconnect_ExpectAndReturn( &( mqttAgentContext.mqttContext ), MQTTSendFailed );
    mqttStatus = MQTTAgentCommand_Disconnect( &mqttAgentContext, NULL, &returnFlags );

    TEST_ASSERT_EQUAL( MQTTSendFailed, mqttStatus );
    /* Ensure that returnFlags are set as intended. */
    TEST_ASSERT_EQUAL( 0, returnFlags.packetId );
    TEST_ASSERT_TRUE( returnFlags.endLoop );
    TEST_ASSERT_FALSE( returnFlags.addAcknowledgment );
    TEST_ASSERT_FALSE( returnFlags.runProcessLoop );
}

/**
 * @brief Test that MQTTAgentCommand_Ping() works as intended.
 */
void test_MQTTAgentCommand_Ping( void )
{
    MQTTAgentContext_t mqttAgentContext = { 0 };
    MQTTAgentCommandFuncReturns_t returnFlags = { 0 };
    MQTTStatus_t mqttStatus;

    MQTT_Ping_ExpectAndReturn( &( mqttAgentContext.mqttContext ), MQTTSuccess );
    mqttStatus = MQTTAgentCommand_Ping( &mqttAgentContext, NULL, &returnFlags );

    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    /* Ensure that returnFlags are set as intended. */
    TEST_ASSERT_EQUAL( 0, returnFlags.packetId );
    TEST_ASSERT_TRUE( returnFlags.runProcessLoop );
    TEST_ASSERT_FALSE( returnFlags.addAcknowledgment );
    TEST_ASSERT_FALSE( returnFlags.endLoop );
}

/**
 * @brief Test MQTTAgentCommand_Ping() failure case.
 */
void test_MQTTAgentCommand_Ping_failure( void )
{
    MQTTAgentContext_t mqttAgentContext = { 0 };
    MQTTAgentCommandFuncReturns_t returnFlags = { 0 };
    MQTTStatus_t mqttStatus;

    MQTT_Ping_ExpectAndReturn( &( mqttAgentContext.mqttContext ), MQTTSendFailed );
    mqttStatus = MQTTAgentCommand_Ping( &mqttAgentContext, NULL, &returnFlags );

    TEST_ASSERT_EQUAL( MQTTSendFailed, mqttStatus );
    /* Ensure that returnFlags are set as intended. */
    TEST_ASSERT_EQUAL( 0, returnFlags.packetId );
    TEST_ASSERT_TRUE( returnFlags.runProcessLoop );
    TEST_ASSERT_FALSE( returnFlags.addAcknowledgment );
    TEST_ASSERT_FALSE( returnFlags.endLoop );
}

/**
 * @brief Test MQTTAgentCommand_Connect() success case.
 */
void test_MQTTAgentCommand_Connect( void )
{
    MQTTAgentContext_t mqttAgentContext = { 0 };
    MQTTAgentConnectArgs_t connectInfo = { 0 };
    MQTTAgentCommandFuncReturns_t returnFlags = { 0 };
    MQTTStatus_t mqttStatus;

    MQTT_Connect_ExpectAnyArgsAndReturn( MQTTSuccess );
    MQTTAgent_ResumeSession_ExpectAndReturn( &mqttAgentContext, connectInfo.sessionPresent, MQTTSuccess );

    mqttStatus = MQTTAgentCommand_Connect( &mqttAgentContext, &connectInfo, &returnFlags );

    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    /* Ensure that returnFlags are set as intended. */
    TEST_ASSERT_EQUAL( 0, returnFlags.packetId );
    TEST_ASSERT_FALSE( returnFlags.addAcknowledgment );
    TEST_ASSERT_FALSE( returnFlags.endLoop );
    TEST_ASSERT_FALSE( returnFlags.runProcessLoop );
}

/**
 * @brief Test MQTTAgentCommand_Connect() failure case.
 */
void test_MQTTAgentCommand_Connect_failure( void )
{
    MQTTAgentContext_t mqttAgentContext = { 0 };
    MQTTAgentConnectArgs_t connectInfo = { 0 };
    MQTTAgentCommandFuncReturns_t returnFlags = { 0 };
    MQTTStatus_t mqttStatus;

    MQTT_Connect_ExpectAnyArgsAndReturn( MQTTBadParameter );

    mqttStatus = MQTTAgentCommand_Connect( &mqttAgentContext, &connectInfo, &returnFlags );

    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );
    /* Ensure that returnFlags are set as intended. */
    TEST_ASSERT_EQUAL( 0, returnFlags.packetId );
    TEST_ASSERT_FALSE( returnFlags.addAcknowledgment );
    TEST_ASSERT_FALSE( returnFlags.endLoop );
    TEST_ASSERT_FALSE( returnFlags.runProcessLoop );
}

/**
 * @brief Test that MQTTAgentCommand_terminate() works as intended.
 */
void test_MQTTAgentCommand_terminate( void )
{
    MQTTAgentContext_t mqttAgentContext = { 0 };
    MQTTStatus_t mqttStatus;
    MQTTAgentCommandFuncReturns_t returnFlags = { 0 };
    Command_t command = { 0 };
    CommandContext_t commandContext = { 0 };

    mqttAgentContext.agentInterface.pMsgCtx = &globalMessageContext;
    mqttAgentContext.agentInterface.send = stubSend;
    mqttAgentContext.agentInterface.recv = stubReceive;
    mqttAgentContext.agentInterface.releaseCommand = stubReleaseCommand;
    mqttAgentContext.agentInterface.getCommand = stubGetCommand;

    command.pCommandCompleteCallback = stubCompletionCallback;
    command.pCmdContext = &commandContext;
    mqttAgentContext.agentInterface.pMsgCtx->pSentCommand = &command;

    mqttAgentContext.pPendingAcks[ 0 ].packetId = 1U;
    mqttAgentContext.pPendingAcks[ 0 ].pOriginalCommand = &command;

    mqttStatus = MQTTAgentCommand_Terminate( &mqttAgentContext, NULL, &returnFlags );


    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    /* Ensure that returnFlags are set as intended. */
    TEST_ASSERT_EQUAL( 0, returnFlags.packetId );
    TEST_ASSERT_TRUE( returnFlags.endLoop );
    TEST_ASSERT_FALSE( returnFlags.addAcknowledgment );
    TEST_ASSERT_FALSE( returnFlags.runProcessLoop );

    /* Ensure that callback is invoked. */
    TEST_ASSERT_EQUAL( 2, commandCompleteCallbackCount );
    TEST_ASSERT_EQUAL( MQTTBadResponse, command.pCmdContext->returnStatus );


    /* Ensure that acknowledgment is cleared. */
    TEST_ASSERT_EQUAL( 0, mqttAgentContext.pPendingAcks[ 0 ].packetId );
    TEST_ASSERT_EQUAL( NULL, mqttAgentContext.pPendingAcks[ 0 ].pOriginalCommand );

    /* Ensure that command is released. */
    TEST_ASSERT_EQUAL( 2, commandReleaseCallCount );

    /* Test MQTTAgentCommand_Terminate() with commandCallback as null. */
    receiveCounter = 0;
    commandCompleteCallbackCount = 0;
    commandReleaseCallCount = 0;
    command.pCommandCompleteCallback = NULL;
    mqttAgentContext.agentInterface.pMsgCtx->pSentCommand = &command;
    mqttAgentContext.pPendingAcks[ 0 ].packetId = 1U;
    mqttAgentContext.pPendingAcks[ 0 ].pOriginalCommand = &command;

    mqttStatus = MQTTAgentCommand_Terminate( &mqttAgentContext, NULL, &returnFlags );

    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    /* Ensure that returnFlags are set as intended. */
    TEST_ASSERT_EQUAL( 0, returnFlags.packetId );
    TEST_ASSERT_TRUE( returnFlags.endLoop );
    TEST_ASSERT_FALSE( returnFlags.addAcknowledgment );
    TEST_ASSERT_FALSE( returnFlags.runProcessLoop );

    /* Ensure that callback is not invoked. */
    TEST_ASSERT_EQUAL( 0, commandCompleteCallbackCount );

    /* Ensure that acknowledgment is cleared. */
    TEST_ASSERT_EQUAL( 0, mqttAgentContext.pPendingAcks[ 0 ].packetId );
    TEST_ASSERT_EQUAL( NULL, mqttAgentContext.pPendingAcks[ 0 ].pOriginalCommand );

    /* Ensure that command is released. */
    TEST_ASSERT_EQUAL( 2, commandReleaseCallCount );
}
