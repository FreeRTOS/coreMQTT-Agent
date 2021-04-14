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
 * @file agent_message.h
 * @brief Functions to interact with queues.
 */
#ifndef AGENT_MESSAGE_H
#define AGENT_MESSAGE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Declare here so interface functions can use. */
struct Command;
typedef struct Command               Command_t;

/**
 * @ingroup mqtt_agent_struct_types
 * @brief Context with which tasks may deliver messages to the agent.
 */
struct AgentMessageContext;
typedef struct AgentMessageContext   AgentMessageContext_t;

/**
 * @brief Send a message to the specified context.
 * Must be thread safe.
 *
 * @param[in] pMsgCtx An #AgentMessageContext_t.
 * @param[in] pCommandToSend Pointer to address to send to queue.
 * @param[in] blockTimeMs Block time to wait for a send.
 *
 * @return `true` if send was successful, else `false`.
 */
typedef bool ( * AgentMessageSend_t )( AgentMessageContext_t * pMsgCtx,
                                       Command_t * const * pCommandToSend,
                                       uint32_t blockTimeMs );

/**
 * @brief Receive a message from the specified context.
 * Must be thread safe.
 *
 * @param[in] pMsgCtx An #AgentMessageContext_t.
 * @param[out] pReceivedCommand Pointer to write address of received command.
 * @param[in] blockTimeMs Block time to wait for a receive.
 *
 * @return `true` if receive was successful, else `false`.
 */
typedef bool ( * AgentMessageRecv_t )( AgentMessageContext_t * pMsgCtx,
                                       Command_t ** pReceivedCommand,
                                       uint32_t blockTimeMs );

/**
 * @brief Obtain a Command_t structure.
 *
 * @note Command_t structures hold everything the MQTT agent needs to process a
 * command that originates from application.  Examples of commands are PUBLISH and
 * SUBSCRIBE. The Command_t structure must persist for the duration of the command's
 * operation.
 *
 * @param[in] blockTimeMs The length of time the calling task should remain in the
 * Blocked state (so not consuming any CPU time) to wait for a Command_t structure to
 * become available should one not be immediately at the time of the call.
 *
 * @return A pointer to a Command_t structure if one becomes available before
 * blockTimeMs time expired, otherwise NULL.
 */
typedef Command_t * ( * AgentCommandGet_t )( uint32_t blockTimeMs );

/**
 * @brief Give a Command_t structure back to the application.
 *
 * @note Command_t structures hold everything the MQTT agent needs to process a
 * command that originates from application.  Examples of commands are PUBLISH and
 * SUBSCRIBE. The Command_t structure must persist for the duration of the command's
 * operation.
 *
 * @param[in] pCommandToRelease A pointer to the Command_t structure to return to
 * the application. The structure must first have been obtained by calling
 * an #AgentCommandGet_t, otherwise it will have no effect.
 *
 * @return true if the Command_t structure was returned to the application, otherwise false.
 */
typedef bool ( * AgentCommandRelease_t )( Command_t * pCommandToRelease );

/**
 * @ingroup mqtt_agent_struct_types
 * @brief Function pointers and contexts used for sending and receiving commands,
 * and allocating memory for them.
 */
typedef struct AgentMessageInterface
{
    AgentMessageContext_t * pMsgCtx;      /**< Context with which tasks may deliver messages to the agent. */
    AgentMessageSend_t send;              /**< Function to send a command to the agent. */
    AgentMessageRecv_t recv;              /**< Function for the agent to receive a command. */
    AgentCommandGet_t getCommand;         /**< Function to obtain a pointer to an allocated command. */
    AgentCommandRelease_t releaseCommand; /**< Function to release an allocated command. */
} AgentMessageInterface_t;

#endif /* AGENT_MESSAGE_H */
