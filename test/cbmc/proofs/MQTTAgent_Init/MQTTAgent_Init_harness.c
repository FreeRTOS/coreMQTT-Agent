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

/* MQTT agent include. */
#include "mqtt_agent.h"

void harness()
{

  MQTTAgentContext_t * pMqttAgentContext;
  AgentMessageInterface_t * pMsgInterface;
  MQTTFixedBuffer_t * pNetworkBuffer;
  TransportInterface_t * pTransportInterface;
  MQTTGetCurrentTimeFunc_t getCurrentTimeMs;
  IncomingPublishCallback_t incomingCallback;
  void * pIncomingPacketContext;

  pMqttAgentContext = mallocCanFail( sizeof( MQTTAgentContext_t ) );
  pMsgInterface = mallocCanFail( sizeof( AgentMessageInterface_t ) );
  pNetworkBuffer = mallocCanFail( sizeof( MQTTFixedBuffer_t ) );
  pTransportInterface = mallocCanFail( sizeof( TransportInterface_t ) );
  /* The incoming packet context can be a pointer to any type.
   * In this harness, we are using int32_t as that will be good enough
   * for memory safety proofs. */
  pIncomingPacketContext = mallocCanFail( sizeof( int32_t ) );

  MQTTAgent_Init( pMqttAgentContext,
                  pMsgInterface,
                  pNetworkBuffer,
                  pTransportInterface,
                  getCurrentTimeMs,
                  incomingCallback,
                  pIncomingPacketContext );
}
