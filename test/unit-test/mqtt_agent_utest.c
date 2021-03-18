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
 * @brief MQTT client identifier.
 */
#define MQTT_CLIENT_IDENTIFIER         "testclient"

/**
 * @brief A sample network context that we set to NULL.
 */
#define MQTT_SAMPLE_NETWORK_CONTEXT    ( NULL )

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


/* ============================   UNITY FIXTURES ============================ */

/* Called before each test method. */
void setUp()
{
    globalEntryTime = 0;
    commandReleaseCallCount = 0;
    globalMessageContext.pSentCommand = NULL;
    pCommandToReturn = NULL;
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
static bool mockSend( AgentMessageContext_t * pMsgCtx,
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
static bool mockSendFail( AgentMessageContext_t * pMsgCtx,
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
static bool mockReceive( AgentMessageContext_t * pMsgCtx,
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
static Command_t * mockGetCommand( uint32_t blockTimeMs )
{
    return pCommandToReturn;
}

/**
 * @brief A mocked function to release an allocated command.
 */
static bool mockReleaseCommand( Command_t * pCommandToRelease )
{
    ( void ) pCommandToRelease;
    commandReleaseCallCount++;
    return true;
}

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

static void mockCompletionCallback( void * pCommandCompletionContext,
                                    MQTTAgentReturnInfo_t * pReturnInfo )
{
    CommandContext_t * pCastContext;

    pCastContext = ( CommandContext_t * ) pCommandCompletionContext;

    if( pCastContext != NULL )
    {
        pCastContext->returnStatus = pReturnInfo->returnCode;
    }
}

/**
 * @brief A mocked timer query function that increments on every call.
 */
static uint32_t getTime( void )
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

/* ========================================================================== */

/**
 * @brief Test that MQTTAgent_Init is able to update the context object correctly.
 */

void test_MQTTAgent_Init_Happy_Path( void )
{
}

/**
 * @brief Test that any NULL parameter causes MQTTAgent_Init to return MQTTBadParameter.
 */
void test_MQTT_Init_Invalid_Params( void )
{
}

/* ========================================================================== */

/**
 * @brief Test that the parameter validation for API functions that create
 * commands (publish, subscribe, connect, etc.) works as intended.
 */
void test_MQTTAgent_API_Create_Command_Invalid_Params( void )
{
    MQTTAgentContext_t agentContext = { 0 };
    MQTTStatus_t mqttStatus;
    CommandInfo_t commandInfo = { 0 };

    setupAgentContext( &agentContext );

    /* Use Terminate since it's one that doesn't have an additional pointer param. */
    mqttStatus = MQTTAgent_Terminate( &agentContext, NULL );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Terminate( NULL, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    /* Various NULL parameters for the agent interface. */
    agentContext.agentInterface.send = NULL;
    mqttStatus = MQTTAgent_Terminate( &agentContext, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    agentContext.agentInterface.send = mockSend;
    agentContext.agentInterface.recv = NULL;
    mqttStatus = MQTTAgent_Terminate( &agentContext, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    agentContext.agentInterface.recv = mockReceive;
    agentContext.agentInterface.getCommand = NULL;
    mqttStatus = MQTTAgent_Terminate( &agentContext, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    agentContext.agentInterface.getCommand = mockGetCommand;
    agentContext.agentInterface.releaseCommand = NULL;
    mqttStatus = MQTTAgent_Terminate( &agentContext, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    agentContext.agentInterface.releaseCommand = mockReleaseCommand;
    agentContext.agentInterface.pMsgCtx = NULL;
    mqttStatus = MQTTAgent_Terminate( &agentContext, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    agentContext.agentInterface.pMsgCtx = &globalMessageContext;
    /* Invalid packet ID to indicate uninitialized context. */
    agentContext.mqttContext.nextPacketId = MQTT_PACKET_ID_INVALID;
    mqttStatus = MQTTAgent_Terminate( &agentContext, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );
}

/**
 * @brief Test error cases when a command cannot be obtained or sent
 * for API functions that create commands.
 */
void test_MQTTAgent_API_Create_Command_Allocation_Error( void )
{
    MQTTAgentContext_t agentContext = { 0 };
    MQTTStatus_t mqttStatus;
    CommandInfo_t commandInfo = { 0 };
    Command_t command = { 0 };

    setupAgentContext( &agentContext );

    /* Use Ping since it's one that doesn't have an additional pointer param. */
    pCommandToReturn = NULL;
    mqttStatus = MQTTAgent_Ping( &agentContext, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTNoMemory, mqttStatus );

    pCommandToReturn = &command;
    agentContext.agentInterface.send = mockSendFail;
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
void test_MQTTAgent_API_Create_Command_No_Ack_Space( void )
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
 * @brief Test that MQTTAgent_Subscribe() works as intended.
 */
void test_MQTTAgent_Subscribe( void )
{
    MQTTAgentContext_t agentContext;
    MQTTStatus_t mqttStatus;
    CommandInfo_t commandInfo = { 0 };
    Command_t command = { 0 };
    MQTTSubscribeInfo_t subscribeInfo = { 0 };
    MQTTAgentSubscribeArgs_t subscribeArgs = { 0 };

    setupAgentContext( &agentContext );
    pCommandToReturn = &command;

    /* We have already tested the generic cases of NULL parameters above,
     * so here we only need to test one for coverage. */
    mqttStatus = MQTTAgent_Subscribe( NULL, &subscribeArgs, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Subscribe( &agentContext, NULL, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    /* Incorrect subscribe args. */
    mqttStatus = MQTTAgent_Subscribe( &agentContext, &subscribeArgs, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    subscribeArgs.pSubscribeInfo = &subscribeInfo;
    subscribeArgs.numSubscriptions = 0U;
    mqttStatus = MQTTAgent_Subscribe( &agentContext, &subscribeArgs, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

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
 * @brief Test that MQTTAgent_Unsubscribe() works as intended.
 */
void test_MQTTAgent_Unsubscribe( void )
{
    MQTTAgentContext_t agentContext;
    MQTTStatus_t mqttStatus;
    CommandInfo_t commandInfo = { 0 };
    Command_t command = { 0 };
    MQTTSubscribeInfo_t subscribeInfo = { 0 };
    MQTTAgentSubscribeArgs_t subscribeArgs = { 0 };

    setupAgentContext( &agentContext );
    pCommandToReturn = &command;
    commandInfo.cmdCompleteCallback = mockCompletionCallback;

    /* We have already tested the generic cases of NULL parameters above,
     * so here we only need to test one for coverage. */
    mqttStatus = MQTTAgent_Unsubscribe( NULL, &subscribeArgs, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Unsubscribe( &agentContext, NULL, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    /* Incorrect subscribe args. */
    mqttStatus = MQTTAgent_Unsubscribe( &agentContext, &subscribeArgs, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    subscribeArgs.pSubscribeInfo = &subscribeInfo;
    subscribeArgs.numSubscriptions = 0U;
    mqttStatus = MQTTAgent_Unsubscribe( &agentContext, &subscribeArgs, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    /* Success case. */
    subscribeArgs.pSubscribeInfo = &subscribeInfo;
    subscribeArgs.numSubscriptions = 1U;
    mqttStatus = MQTTAgent_Unsubscribe( &agentContext, &subscribeArgs, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    TEST_ASSERT_EQUAL_PTR( &command, globalMessageContext.pSentCommand );
    TEST_ASSERT_EQUAL( UNSUBSCRIBE, command.commandType );
    TEST_ASSERT_EQUAL_PTR( &subscribeArgs, command.pArgs );
    TEST_ASSERT_EQUAL_PTR( mockCompletionCallback, command.pCommandCompleteCallback );
}

/* ========================================================================== */

/**
 * @brief Test that MQTTAgent_ProcessLoop() works as intended.
 */
void test_MQTTAgent_ProcessLoop( void )
{
    MQTTAgentContext_t agentContext;
    MQTTStatus_t mqttStatus;
    CommandInfo_t commandInfo = { 0 };
    Command_t command = { 0 };

    setupAgentContext( &agentContext );
    pCommandToReturn = &command;

    /* We have already tested generic cases of NULL parameters above,
     * so here we only need to test a few one for coverage. */
    mqttStatus = MQTTAgent_ProcessLoop( NULL, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    /* Success case. */
    mqttStatus = MQTTAgent_ProcessLoop( &agentContext, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    TEST_ASSERT_EQUAL_PTR( &command, globalMessageContext.pSentCommand );
    TEST_ASSERT_EQUAL( PROCESSLOOP, command.commandType );
    TEST_ASSERT_NULL( command.pArgs );
    TEST_ASSERT_NULL( command.pCommandCompleteCallback );
}

/**
 * @brief Test that MQTTAgent_Disconnect() works as intended.
 */
void test_MQTTAgent_Disconnect( void )
{
    MQTTAgentContext_t agentContext;
    MQTTStatus_t mqttStatus;
    CommandInfo_t commandInfo = { 0 };
    Command_t command = { 0 };

    setupAgentContext( &agentContext );
    pCommandToReturn = &command;
    commandInfo.cmdCompleteCallback = mockCompletionCallback;

    /* We have already tested generic cases of NULL parameters above,
     * so here we only need to test a few one for coverage. */
    mqttStatus = MQTTAgent_Disconnect( NULL, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    /* Success case. */
    mqttStatus = MQTTAgent_Disconnect( &agentContext, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    TEST_ASSERT_EQUAL_PTR( &command, globalMessageContext.pSentCommand );
    TEST_ASSERT_EQUAL( DISCONNECT, command.commandType );
    TEST_ASSERT_NULL( command.pArgs );
    TEST_ASSERT_EQUAL_PTR( mockCompletionCallback, command.pCommandCompleteCallback );
}

/**
 * @brief Test that MQTTAgent_Ping() works as intended.
 */
void test_MQTTAgent_Ping( void )
{
    MQTTAgentContext_t agentContext;
    MQTTStatus_t mqttStatus;
    CommandInfo_t commandInfo = { 0 };
    Command_t command = { 0 };

    setupAgentContext( &agentContext );
    pCommandToReturn = &command;
    commandInfo.cmdCompleteCallback = mockCompletionCallback;

    /* We have already tested generic cases of NULL parameters above,
     * so here we only need to test a few one for coverage. */
    mqttStatus = MQTTAgent_Ping( NULL, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    /* Success case. */
    mqttStatus = MQTTAgent_Ping( &agentContext, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    TEST_ASSERT_EQUAL_PTR( &command, globalMessageContext.pSentCommand );
    TEST_ASSERT_EQUAL( PING, command.commandType );
    TEST_ASSERT_NULL( command.pArgs );
    TEST_ASSERT_EQUAL_PTR( mockCompletionCallback, command.pCommandCompleteCallback );
}

/**
 * @brief Test that MQTTAgent_Terminate() works as intended.
 */
void test_MQTTAgent_Terminate( void )
{
    MQTTAgentContext_t agentContext;
    MQTTStatus_t mqttStatus;
    CommandInfo_t commandInfo = { 0 };
    Command_t command = { 0 };

    setupAgentContext( &agentContext );
    pCommandToReturn = &command;
    commandInfo.cmdCompleteCallback = mockCompletionCallback;

    /* We have already tested cases of NULL parameters with Terminate,
     * so we only need to test the success case. */
    mqttStatus = MQTTAgent_Terminate( &agentContext, &commandInfo );
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    TEST_ASSERT_EQUAL_PTR( &command, globalMessageContext.pSentCommand );
    TEST_ASSERT_EQUAL( TERMINATE, command.commandType );
    TEST_ASSERT_NULL( command.pArgs );
    TEST_ASSERT_EQUAL_PTR( mockCompletionCallback, command.pCommandCompleteCallback );
}
