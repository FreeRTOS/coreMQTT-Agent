/*
 * coreMQTT-Agent V1.0.0
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
 * @file agent_message_stubs.h
 * @brief Stub functions to interact with queues.
 */

#include "mqtt_agent.h"
#include "agent_message_stubs.h"


bool AgentMessageSendStub( AgentMessageContext_t * pMsgCtx,
                           const void * pData,
                           uint32_t blockTimeMs )
{
    /* For the proofs, returning a non deterministic boolean value
     * will be good enough. */
    return nondet_bool();
}

bool AgentMessageRecvStub( AgentMessageContext_t * pMsgCtx,
                           void * pBuffer,
                           uint32_t blockTimeMs )
{
    /* For the proofs, returning a non deterministic boolean value
     * will be good enough. */
    return nondet_bool();
}
