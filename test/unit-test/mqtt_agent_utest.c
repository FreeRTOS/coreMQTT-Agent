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
static uint32_t globalEntryTime = 0;

/* ========================================================================== */

/**
 * @brief A mocked timer query function that increments on every call.
 */
static uint32_t getTime( void )
{
    return globalEntryTime++;
}
/* ========================================================================== */

bool ( send )( AgentMessageContext_t * pMsgCtx,
                const void * pData,
                uint32_t blockTimeMs )
{
    return true;
}

bool ( receive )( AgentMessageContext_t * pMsgCtx,
                  void * pBuffer,
                  uint32_t blockTimeMs )
{
    return true;
}

bool ( commandRelease )( Command_t * pCommandToRelease )
{
    return true;
}

Command_t * ( getCommand )( uint32_t blockTimeMs )
{
    Command_t * command;
    return command;
}

void (CommandCallback )( void * pCmdCallbackContext,
                                     MQTTAgentReturnInfo_t * pReturnInfo ){
                                         return;
                                     }
/* ============================   UNITY FIXTURES ============================ */

/* Called before each test method. */
void setUp()
{
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


/**
 * @brief Test that MQTTAgent_Init is able to update the context object correctly.
 */

void test_MQTTAgent_Init_Happy_Path( void )
{
    MQTTAgentContext_t mqttAgentContext;
    AgentMessageInterface_t msgCtx;
    MQTTFixedBuffer_t networkBuffer;
    TransportInterface_t transportInterface;
    IncomingPublishCallback_t incomingCallback;
    void *incomingPacketContext;
    AgentMessageContext_t *msg;
    MQTTStatus_t mqttStatus;


    MQTT_Init_ExpectAnyArgsAndReturn(MQTTSuccess);
    msgCtx.pMsgCtx=msg;
    msgCtx.send=send;
    msgCtx.recv=receive;
    msgCtx.releaseCommand=commandRelease;
    msgCtx.getCommand=getCommand;
    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgCtx, &networkBuffer , &transportInterface, getTime, incomingCallback, incomingPacketContext);
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );
    TEST_ASSERT_EQUAL_PTR( incomingCallback, mqttAgentContext.pIncomingCallback );
    TEST_ASSERT_EQUAL_PTR( incomingPacketContext, mqttAgentContext.pIncomingCallbackContext );
    TEST_ASSERT_EQUAL_MEMORY( &msgCtx, &mqttAgentContext.agentInterface, sizeof( msgCtx ) );


}

/**
 * @brief Test that any NULL parameter causes MQTTAgent_Init to return MQTTBadParameter.
 */
void test_MQTTAgent_Init_Invalid_Params( void )
{
    MQTTAgentContext_t mqttAgentContext;
    AgentMessageInterface_t msgCtx;
    MQTTFixedBuffer_t networkBuffer;
    TransportInterface_t transportInterface;
    IncomingPublishCallback_t incomingCallback;
    void *incomingPacketContext;
    AgentMessageContext_t *msg;
    MQTTStatus_t mqttStatus;

    /* Check that MQTTBadParameter is returned if any NULL parameters are passed. */
    mqttStatus = MQTTAgent_Init( NULL, &msgCtx, &networkBuffer , &transportInterface, getTime, incomingCallback, incomingPacketContext);
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, NULL, &networkBuffer , &transportInterface, getTime, incomingCallback, incomingPacketContext);
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgCtx, &networkBuffer , NULL, getTime, incomingCallback, incomingPacketContext);
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    /* Test if NULL is passed for any of the function pointers. */
    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgCtx, &networkBuffer , &transportInterface, NULL, incomingCallback, incomingPacketContext);
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgCtx, &networkBuffer , &transportInterface, getTime, incomingCallback, incomingPacketContext);
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    msgCtx.pMsgCtx=msg;
    
    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgCtx, &networkBuffer , &transportInterface, getTime, incomingCallback, incomingPacketContext);
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    msgCtx.send=send;

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgCtx, &networkBuffer , &transportInterface, getTime, incomingCallback, incomingPacketContext);
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    msgCtx.recv=receive;

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgCtx, &networkBuffer , &transportInterface, getTime, incomingCallback, incomingPacketContext);
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

    msgCtx.releaseCommand=commandRelease;

    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgCtx, &networkBuffer , &transportInterface, getTime, incomingCallback, incomingPacketContext);
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

}

/**
 * @brief Test that any NULL parameter causes MQTTAgent_ResumeSession to return MQTTIllegalState.
 */
void test_MQTTAgent_ResumeSession_Invalid_Params( void )
{
    MQTTAgentContext_t mqttAgentContext;
    bool sessionPresent=true;
    MQTTStatus_t mqttStatus;

    AgentMessageInterface_t msgCtx;
    MQTTFixedBuffer_t networkBuffer;
    TransportInterface_t transportInterface;
    IncomingPublishCallback_t incomingCallback;
    void *incomingPacketContext;
    AgentMessageContext_t *msg;


    MQTT_Init_ExpectAnyArgsAndReturn(MQTTSuccess);
    msgCtx.pMsgCtx=msg;
    msgCtx.send=send;
    msgCtx.recv=receive;
    msgCtx.releaseCommand=commandRelease;
    msgCtx.getCommand=getCommand;
    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgCtx, &networkBuffer , &transportInterface, getTime, incomingCallback, incomingPacketContext);


    /* Check that MQTTIllegalState is returned if any NULL parameters are passed. */
    mqttStatus = MQTTAgent_ResumeSession( NULL,sessionPresent);
    TEST_ASSERT_EQUAL( MQTTIllegalState, mqttStatus );

    MQTT_Init_ExpectAnyArgsAndReturn(MQTTSuccess);
    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgCtx, &networkBuffer , &transportInterface, getTime, incomingCallback, incomingPacketContext);
    MQTTContext_t mqttContext;    
    mqttContext.nextPacketId=0;
    mqttAgentContext.mqttContext=mqttContext;
    
    mqttStatus = MQTTAgent_ResumeSession( &mqttAgentContext,sessionPresent);
    TEST_ASSERT_EQUAL( MQTTIllegalState, mqttStatus );

}


void test_MQTTAgent_ResumeSession_session_present_with_packetId_invalid( void )
{
    MQTTAgentContext_t mqttAgentContext;
    bool sessionPresent=true;
    MQTTStatus_t mqttStatus;
    AgentMessageInterface_t msgCtx;
    MQTTFixedBuffer_t networkBuffer;
    TransportInterface_t transportInterface;
    IncomingPublishCallback_t incomingCallback;
    void *incomingPacketContext;
    AgentMessageContext_t *msg;

    MQTT_Init_ExpectAnyArgsAndReturn(MQTTSuccess);
    msgCtx.pMsgCtx=msg;
    msgCtx.send=send;
    msgCtx.recv=receive;
    msgCtx.releaseCommand=commandRelease;
    msgCtx.getCommand=getCommand;
    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgCtx, &networkBuffer , &transportInterface, getTime, incomingCallback, incomingPacketContext);


    // MQTTContext_t mqttContext;                                 
    Command_t command;
    void * args;
    command.pArgs=args;
    // mqttAgentContext.mqttContext=mqttContext;
    mqttAgentContext.mqttContext.nextPacketId = 1;
    MQTT_PublishToResend_IgnoreAndReturn(MQTT_PACKET_ID_INVALID);
    mqttStatus = MQTTAgent_ResumeSession( &mqttAgentContext,sessionPresent);
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );

}


void test_MQTTAgent_ResumeSession_failed_publish( void )
{
    MQTTAgentContext_t mqttAgentContext;
    bool sessionPresent=true;
    MQTTStatus_t mqttStatus;
    AgentMessageInterface_t msgCtx;
    MQTTFixedBuffer_t networkBuffer;
    TransportInterface_t transportInterface;
    IncomingPublishCallback_t incomingCallback;
    void *incomingPacketContext;
    AgentMessageContext_t *msg;

    MQTT_Init_ExpectAnyArgsAndReturn(MQTTSuccess);
    msgCtx.pMsgCtx=msg;
    msgCtx.send=send;
    msgCtx.recv=receive;
    msgCtx.releaseCommand=commandRelease;
    msgCtx.getCommand=getCommand;
    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgCtx, &networkBuffer , &transportInterface, getTime, incomingCallback, incomingPacketContext);

    // MQTTContext_t mqttContext;                                 
    Command_t command;
    MQTTPublishInfo_t args;
    command.pArgs=&args;
    // mqttAgentContext.mqttContext=mqttContext;
    mqttAgentContext.mqttContext.nextPacketId = 1;
    AckInfo_t ackInfo;
    ackInfo.packetId=1;
    ackInfo.pOriginalCommand=&command;
    mqttAgentContext.pPendingAcks[ 0 ]=ackInfo;
    MQTT_PublishToResend_IgnoreAndReturn(1);
    MQTT_Publish_IgnoreAndReturn(MQTTBadParameter);
    
    //MQTT_Publish_IgnoreAndReturn(MQTTSuccess);
    //MQTT_PublishToResend_IgnoreAndReturn(MQTT_PACKET_ID_INVALID);
    mqttStatus = MQTTAgent_ResumeSession( &mqttAgentContext,sessionPresent);
    TEST_ASSERT_EQUAL( MQTTBadParameter, mqttStatus );

}



void test_MQTTAgent_ResumeSession_ack_packetid_not_match( void )
{
    MQTTAgentContext_t mqttAgentContext;
    bool sessionPresent=true;
    MQTTStatus_t mqttStatus;
    AgentMessageInterface_t msgCtx;
    MQTTFixedBuffer_t networkBuffer;
    TransportInterface_t transportInterface;
    IncomingPublishCallback_t incomingCallback;
    void *incomingPacketContext;
    AgentMessageContext_t *msg;

    MQTT_Init_ExpectAnyArgsAndReturn(MQTTSuccess);
    msgCtx.pMsgCtx=msg;
    msgCtx.send=send;
    msgCtx.recv=receive;
    msgCtx.releaseCommand=commandRelease;
    msgCtx.getCommand=getCommand;
    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgCtx, &networkBuffer , &transportInterface, getTime, incomingCallback, incomingPacketContext);

    // MQTTContext_t mqttContext;                                 
    Command_t command;
    MQTTPublishInfo_t args;
    command.pArgs=&args;
    // mqttAgentContext.mqttContext=mqttContext;
    mqttAgentContext.mqttContext.nextPacketId = 1;
    AckInfo_t ackInfo;
    ackInfo.packetId=1;
    ackInfo.pOriginalCommand=&command;
    mqttAgentContext.pPendingAcks[ 0 ]=ackInfo;
    MQTT_PublishToResend_IgnoreAndReturn(2);
    //MQTT_Publish_IgnoreAndReturn(MQTTBadParameter);
    
    //MQTT_Publish_IgnoreAndReturn(MQTTSuccess);
    MQTT_PublishToResend_IgnoreAndReturn(MQTT_PACKET_ID_INVALID);
    mqttStatus = MQTTAgent_ResumeSession( &mqttAgentContext,sessionPresent);
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );

}



void test_MQTTAgent_ResumeSession_publish_ack_success( void )
{
    MQTTAgentContext_t mqttAgentContext;
    bool sessionPresent=true;
    MQTTStatus_t mqttStatus;
    AgentMessageInterface_t msgCtx;
    MQTTFixedBuffer_t networkBuffer;
    TransportInterface_t transportInterface;
    IncomingPublishCallback_t incomingCallback;
    void *incomingPacketContext;
    AgentMessageContext_t *msg;

    MQTT_Init_ExpectAnyArgsAndReturn(MQTTSuccess);
    msgCtx.pMsgCtx=msg;
    msgCtx.send=send;
    msgCtx.recv=receive;
    msgCtx.releaseCommand=commandRelease;
    msgCtx.getCommand=getCommand;
    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgCtx, &networkBuffer , &transportInterface, getTime, incomingCallback, incomingPacketContext);

    // MQTTContext_t mqttContext;                                 
    Command_t command;
    MQTTPublishInfo_t args;
    command.pArgs=&args;
    // mqttAgentContext.mqttContext=mqttContext;
    mqttAgentContext.mqttContext.nextPacketId = 1;
    AckInfo_t ackInfo;
    ackInfo.packetId=0;
    ackInfo.pOriginalCommand=&command;
    mqttAgentContext.pPendingAcks[ 0 ]=ackInfo;
    MQTT_PublishToResend_IgnoreAndReturn(0);

    MQTT_PublishToResend_IgnoreAndReturn(MQTT_PACKET_ID_INVALID);
    mqttStatus = MQTTAgent_ResumeSession( &mqttAgentContext,sessionPresent);
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );

}


void test_MQTTAgent_ResumeSession_no_session_present( void )
{
    MQTTAgentContext_t mqttAgentContext;
    bool sessionPresent=true;
    MQTTStatus_t mqttStatus;
    AgentMessageInterface_t msgCtx;
    MQTTFixedBuffer_t networkBuffer;
    TransportInterface_t transportInterface;
    IncomingPublishCallback_t incomingCallback;
    void *incomingPacketContext;
    AgentMessageContext_t *msg;

    MQTT_Init_ExpectAnyArgsAndReturn(MQTTSuccess);
    msgCtx.pMsgCtx=msg;
    msgCtx.send=send;
    msgCtx.recv=receive;
    msgCtx.releaseCommand=commandRelease;
    msgCtx.getCommand=getCommand;
    mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgCtx, &networkBuffer , &transportInterface, getTime, incomingCallback, incomingPacketContext);


    // MQTTContext_t mqttContext;                                 
    Command_t command;
    command.pCommandCompleteCallback=NULL;
    // mqttAgentContext.mqttContext=mqttContext;
    mqttAgentContext.mqttContext.nextPacketId = 1;
    mqttAgentContext.pPendingAcks[ 0 ].packetId=MQTT_PACKET_ID_INVALID;
    mqttStatus = MQTTAgent_ResumeSession( &mqttAgentContext,false);
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );

    
    // MQTT_Init_ExpectAnyArgsAndReturn(MQTTSuccess);
    // mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgCtx, &networkBuffer , &transportInterface, getTime, incomingCallback, incomingPacketContext);
    // mqttAgentContext.mqttContext=mqttContext;
    // mqttAgentContext.mqttContext.nextPacketId = 1;
    AckInfo_t ackInfo;
    ackInfo.packetId=1;
    ackInfo.pOriginalCommand=&command;
    mqttAgentContext.pPendingAcks[ 0 ]=ackInfo; 
    mqttStatus = MQTTAgent_ResumeSession( &mqttAgentContext,false);
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );

    command.pCommandCompleteCallback=CommandCallback;
    //  MQTT_Init_ExpectAnyArgsAndReturn(MQTTSuccess);
    // mqttStatus = MQTTAgent_Init( &mqttAgentContext, &msgCtx, &networkBuffer , &transportInterface, getTime, incomingCallback, incomingPacketContext);
    // mqttAgentContext.mqttContext=mqttContext;
    // mqttAgentContext.mqttContext.nextPacketId = 1;
    mqttAgentContext.pPendingAcks[ 0 ]=ackInfo; 
    mqttStatus = MQTTAgent_ResumeSession( &mqttAgentContext,false);
    TEST_ASSERT_EQUAL( MQTTSuccess, mqttStatus );


}
