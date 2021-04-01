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

/**
 * @file mqtt_agent.h
 * @brief Functions for running a coreMQTT client in a dedicated thread.
 */
#ifndef MQTT_AGENT_H
#define MQTT_AGENT_H

/* MQTT library includes. */
#include "core_mqtt.h"
#include "core_mqtt_state.h"

/* Queue include. */
#include "agent_message.h"

/**
 * @brief The maximum number of pending acknowledgments to track for a single
 * connection.
 *
 * @note The MQTT agent tracks MQTT commands (such as PUBLISH and SUBSCRIBE) th
 * at are still waiting to be acknowledged.  MQTT_AGENT_MAX_OUTSTANDING_ACKS set
 * the maximum number of acknowledgments that can be outstanding at any one time.
 * The higher this number is the greater the agent's RAM consumption will be.
 */
#ifndef MQTT_AGENT_MAX_OUTSTANDING_ACKS
    #define MQTT_AGENT_MAX_OUTSTANDING_ACKS    ( 20U )
#endif

/**
 * @brief Time in MS that the MQTT agent task will wait in the Blocked state (so
 * not using any CPU time) for a command to arrive in its command queue before
 * exiting the blocked state so it can call MQTT_ProcessLoop().
 *
 * @note It is important MQTT_ProcessLoop() is called often if there is known
 * MQTT traffic, but calling it too often can take processing time away from
 * lower priority tasks and waste CPU time and power.
 */
#ifndef MQTT_AGENT_MAX_EVENT_QUEUE_WAIT_TIME
    #define MQTT_AGENT_MAX_EVENT_QUEUE_WAIT_TIME    ( 1000 )
#endif

/*-----------------------------------------------------------*/

/**
 * @ingroup mqtt_agent_enum_types
 * @brief A type of command for interacting with the MQTT API.
 */
typedef enum CommandType
{
    NONE = 0,    /**< @brief No command received.  Must be zero (its memset() value). */
    PROCESSLOOP, /**< @brief Call MQTT_ProcessLoop(). */
    PUBLISH,     /**< @brief Call MQTT_Publish(). */
    SUBSCRIBE,   /**< @brief Call MQTT_Subscribe(). */
    UNSUBSCRIBE, /**< @brief Call MQTT_Unsubscribe(). */
    PING,        /**< @brief Call MQTT_Ping(). */
    CONNECT,     /**< @brief Call MQTT_Connect(). */
    DISCONNECT,  /**< @brief Call MQTT_Disconnect(). */
    TERMINATE,   /**< @brief Exit the command loop and stop processing commands. */
    NUM_COMMANDS /**< @brief The number of command types handled by the agent. */
} CommandType_t;

struct MQTTAgentContext;
typedef struct MQTTAgentContext   MQTTAgentContext_t;

struct Command;
typedef struct Command            Command_t;

/**
 * @ingroup mqtt_agent_struct_types
 * @brief Struct holding return codes and outputs from a command
 */
typedef struct MQTTAgentReturnInfo
{
    MQTTStatus_t returnCode; /**< Return code of the MQTT command. */
    uint8_t * pSubackCodes;  /**< Array of SUBACK statuses, for a SUBSCRIBE command. */
} MQTTAgentReturnInfo_t;

/**
 * @ingroup mqtt_agent_struct_types
 * @brief Information for a pending MQTT ack packet expected by the agent.
 */
typedef struct ackInfo
{
    uint16_t packetId;            /**< Packet ID of the pending acknowledgment. */
    Command_t * pOriginalCommand; /**< Command expecting acknowledgment. */
} AckInfo_t;

/**
 * @ingroup mqtt_agent_struct_types
 * @brief Struct containing context for a specific command.
 *
 * @note An instance of this struct and any variables it points to MUST stay
 * in scope until the associated command is processed, and its callback called.
 */
struct CommandContext;
typedef struct CommandContext CommandContext_t;

/**
 * @ingroup mqtt_agent_callback_types
 * @brief Callback function called when a command completes.
 *
 * @param[in] pCmdCallbackContext The callback context passed to the original command.
 * @param[in] pReturnInfo A struct of status codes and outputs from the command.
 *
 * @note A command should not be considered complete until this callback is
 * called, and the arguments that the command uses MUST stay in scope until such happens.
 *
 * @note The callback MUST NOT block as it runs in the context of the MQTT agent
 * task. If the callback calls any MQTT Agent API to enqueue a command, the
 * blocking time (blockTimeMs member of CommandInfo_t) MUST be zero. If the
 * application wants to enqueue command(s) with non-zero blocking time, the
 * callback can notify a different task to enqueue command(s) to the MQTT agent.
 */
typedef void (* CommandCallback_t )( void * pCmdCallbackContext,
                                     MQTTAgentReturnInfo_t * pReturnInfo );

/**
 * @ingroup mqtt_agent_callback_types
 * @brief Callback function called when receiving a publish.
 *
 * @param[in] pMqttAgentContext The context of the MQTT agent.
 * @param[in] packetId The packet ID of the received publish.
 * @param[in] pPublishInfo Deserialized publish information.
 *
 * @note The callback MUST NOT block as it runs in the context of the MQTT agent
 * task. If the callback calls any MQTT Agent API to enqueue a command, the
 * blocking time (blockTimeMs member of CommandInfo_t) MUST be zero. If the
 * application wants to enqueue command(s) with non-zero blocking time, the
 * callback can notify a different task to enqueue command(s) to the MQTT agent.
 */
typedef void (* IncomingPublishCallback_t )( MQTTAgentContext_t * pMqttAgentContext,
                                             uint16_t packetId,
                                             MQTTPublishInfo_t * pPublishInfo );

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

/**
 * @ingroup mqtt_agent_struct_types
 * @brief Information used by each MQTT agent. A context will be initialized by
 * MQTTAgent_Init(), and every API function will accept a pointer to the
 * initalized struct.
 */
struct MQTTAgentContext
{
    MQTTContext_t mqttContext;                                 /**< MQTT connection information used by coreMQTT. */
    AgentMessageInterface_t agentInterface;                    /**< Struct of function pointers for agent messaging. */
    AckInfo_t pPendingAcks[ MQTT_AGENT_MAX_OUTSTANDING_ACKS ]; /**< List of pending acknowledgment packets. */
    IncomingPublishCallback_t pIncomingCallback;               /**< Callback to invoke for incoming publishes. */
    void * pIncomingCallbackContext;                           /**< Context for incoming publish callback. */
    bool packetReceivedInLoop;                                 /**< Whether a MQTT_ProcessLoop() call received a packet. */
};

/**
 * @ingroup mqtt_agent_struct_types
 * @brief Struct holding arguments for a SUBSCRIBE or UNSUBSCRIBE call.
 */
typedef struct MQTTAgentSubscribeArgs
{
    MQTTSubscribeInfo_t * pSubscribeInfo; /**< @brief List of MQTT subscriptions. */
    size_t numSubscriptions;              /**< @brief Number of elements in `pSubscribeInfo`. */
} MQTTAgentSubscribeArgs_t;

/**
 * @ingroup mqtt_agent_struct_types
 * @brief Struct holding arguments for a CONNECT call.
 */
typedef struct MQTTAgentConnectArgs
{
    MQTTConnectInfo_t * pConnectInfo; /**< @brief MQTT CONNECT packet information. */
    MQTTPublishInfo_t * pWillInfo;    /**< @brief Optional Last Will and Testament. */
    uint32_t timeoutMs;               /**< @brief Maximum timeout for a CONNACK packet. */
    bool sessionPresent;              /**< @brief Output flag set if a previous session was present. */
} MQTTAgentConnectArgs_t;

/**
 * @ingroup mqtt_agent_struct_types
 * @brief Struct holding arguments that are common to every command.
 */
typedef struct CommandInfo
{
    CommandCallback_t cmdCompleteCallback;          /**< @brief Callback to invoke upon completion. */
    CommandContext_t * pCmdCompleteCallbackContext; /**< @brief Context for completion callback. */
    uint32_t blockTimeMs;                           /**< @brief Maximum block time for enqueueing the command. */
} CommandInfo_t;

/**
 * @ingroup mqtt_agent_struct_types
 * @brief The commands sent from the APIs to the MQTT agent task.
 *
 * @note The structure used to pass information from the public facing API into the
 * agent task. */
struct Command
{
    CommandType_t commandType;                  /**< @brief Type of command. */
    void * pArgs;                               /**< @brief Arguments of command. */
    CommandCallback_t pCommandCompleteCallback; /**< @brief Callback to invoke upon completion. */
    CommandContext_t * pCmdContext;             /**< @brief Context for completion callback. */
};

/*-----------------------------------------------------------*/

/**
 * @brief Perform any initialization the MQTT agent requires before it can
 * be used. Must be called before any other function.
 *
 * @param[in] pMqttAgentContext Pointer to struct to initialize.
 * @param[in] pMsgInterface Command interface to use for allocating and sending commands.
 * @param[in] pNetworkBuffer Pointer to network buffer to use.
 * @param[in] pTransportInterface Transport interface to use with the MQTT
 * library.  See https://www.freertos.org/network-interface.html
 * @param[in] getCurrentTimeMs Pointer to a function that returns a count value
 * that increments every millisecond.
 * @param[in] incomingCallback The callback to execute when receiving publishes.
 * @param[in] pIncomingPacketContext A pointer to a context structure defined by
 * the application writer.
 *
 * @note The @p pIncomingPacketContext context provided for the incoming publish
 * callback MUST remain in scope throughout the period that the agent task is running.
 *
 * @return Appropriate status code from MQTT_Init().
 */
/* @[declare_mqtt_agent_init] */
MQTTStatus_t MQTTAgent_Init( MQTTAgentContext_t * pMqttAgentContext,
                             AgentMessageInterface_t * pMsgInterface,
                             MQTTFixedBuffer_t * pNetworkBuffer,
                             TransportInterface_t * pTransportInterface,
                             MQTTGetCurrentTimeFunc_t getCurrentTimeMs,
                             IncomingPublishCallback_t incomingCallback,
                             void * pIncomingPacketContext );
/* @[declare_mqtt_agent_init] */

/**
 * @brief Process commands from the command queue in a loop.
 *
 * @param[in] pMqttAgentContext The MQTT agent to use.
 *
 * @return appropriate error code, or `MQTTSuccess` from a successful disconnect
 * or termination.
 */
/* @[declare_mqtt_agent_commandloop] */
MQTTStatus_t MQTTAgent_CommandLoop( MQTTAgentContext_t * pMqttAgentContext );
/* @[declare_mqtt_agent_commandloop] */

/**
 * @brief Resume a session by resending publishes if a session is present in
 * the broker, or clear state information if not.
 *
 * @param[in] pMqttAgentContext The MQTT agent to use.
 * @param[in] sessionPresent The session present flag from the broker.
 *
 * @note This function is NOT thread-safe and should only be called
 * from the context of the task responsible for #MQTTAgent_CommandLoop.
 *
 * @return `MQTTSuccess` if it succeeds in resending publishes, else an
 * appropriate error code from `MQTT_Publish()`
 */
/* @[declare_mqtt_agent_resumesession] */
MQTTStatus_t MQTTAgent_ResumeSession( MQTTAgentContext_t * pMqttAgentContext,
                                      bool sessionPresent );
/* @[declare_mqtt_agent_resumesession] */

/**
 * @brief Add a command to call MQTT_Subscribe() for an MQTT connection.
 *
 * @param[in] pMqttAgentContext The MQTT agent to use.
 * @param[in] pSubscriptionArgs Struct describing topic to subscribe to.
 * @param[in] pCommandInfo The information pertaining to the command, including:
 *  - cmdCompleteCallback Optional callback to invoke when the command completes.
 *  - pCmdCompleteCallbackContext Optional completion callback context.
 *  - blockTimeMs The maximum amount of time in milliseconds to wait for the
 *    command to be posted to the MQTT agent, should the agent's event queue
 *    be full. Tasks wait in the Blocked state so don't use any CPU time.
 *
 * @note The context passed to the callback through pCmdContext member of
 * @p pCommandInfo parameter MUST remain in scope at least until the callback
 * has been executed by the agent task.
 *
 * @return `MQTTSuccess` if the command was posted to the MQTT agent's event queue.
 * Otherwise an enumerated error code.
 */
/* @[declare_mqtt_agent_subscribe] */
MQTTStatus_t MQTTAgent_Subscribe( MQTTAgentContext_t * pMqttAgentContext,
                                  MQTTAgentSubscribeArgs_t * pSubscriptionArgs,
                                  CommandInfo_t * pCommandInfo );
/* @[declare_mqtt_agent_subscribe] */

/**
 * @brief Add a command to call MQTT_Unsubscribe() for an MQTT connection.
 *
 * @param[in] pMqttAgentContext The MQTT agent to use.
 * @param[in] pSubscriptionArgs List of topics to unsubscribe from.
 * @param[in] pCommandInfo The information pertaining to the command, including:
 *  - cmdCompleteCallback Optional callback to invoke when the command completes.
 *  - pCmdCompleteCallbackContext Optional completion callback context.
 *  - blockTimeMs The maximum amount of time in milliseconds to wait for the
 *    command to be posted to the MQTT agent, should the agent's event queue
 *    be full. Tasks wait in the Blocked state so don't use any CPU time.
 *
 * @note The context passed to the callback through pCmdContext member of
 * @p pCommandInfo parameter MUST remain in scope at least until the callback
 * has been executed by the agent task.
 *
 * @return `MQTTSuccess` if the command was posted to the MQTT agent's event queue.
 * Otherwise an enumerated error code.
 */
/* @[declare_mqtt_agent_unsubscribe] */
MQTTStatus_t MQTTAgent_Unsubscribe( MQTTAgentContext_t * pMqttAgentContext,
                                    MQTTAgentSubscribeArgs_t * pSubscriptionArgs,
                                    CommandInfo_t * pCommandInfo );
/* @[declare_mqtt_agent_unsubscribe] */

/**
 * @brief Add a command to call MQTT_Publish() for an MQTT connection.
 *
 * @param[in] pMqttAgentContext The MQTT agent to use.
 * @param[in] pPublishInfo MQTT PUBLISH information.
 * @param[in] pCommandInfo The information pertaining to the command, including:
 *  - cmdCompleteCallback Optional callback to invoke when the command completes.
 *  - pCmdCompleteCallbackContext Optional completion callback context.
 *  - blockTimeMs The maximum amount of time in milliseconds to wait for the
 *    command to be posted to the MQTT agent, should the agent's event queue
 *    be full. Tasks wait in the Blocked state so don't use any CPU time.
 *
 * @note The context passed to the callback through pCmdContext member of
 * @p pCommandInfo parameter MUST remain in scope at least until the callback
 * has been executed by the agent task.
 *
 * @return `MQTTSuccess` if the command was posted to the MQTT agent's event queue.
 * Otherwise an enumerated error code.
 */
/* @[declare_mqtt_agent_publish] */
MQTTStatus_t MQTTAgent_Publish( MQTTAgentContext_t * pMqttAgentContext,
                                MQTTPublishInfo_t * pPublishInfo,
                                CommandInfo_t * pCommandInfo );
/* @[declare_mqtt_agent_publish] */

/**
 * @brief Send a message to the MQTT agent purely to trigger an iteration of its loop,
 * which will result in a call to MQTT_ProcessLoop().  This function can be used to
 * wake the MQTT agent task when it is known data may be available on the connected
 * socket.
 *
 * @param[in] pMqttAgentContext The MQTT agent to use.
 * @param[in] pCommandInfo The information pertaining to the command, including:
 *  - cmdCompleteCallback Optional callback to invoke when the command completes.
 *  - pCmdCompleteCallbackContext Optional completion callback context.
 *  - blockTimeMs The maximum amount of time in milliseconds to wait for the
 *    command to be posted to the MQTT agent, should the agent's event queue
 *    be full. Tasks wait in the Blocked state so don't use any CPU time.
 *
 * @return `MQTTSuccess` if the command was posted to the MQTT agent's event queue.
 * Otherwise an enumerated error code.
 */
/* @[declare_mqtt_agent_processloop] */
MQTTStatus_t MQTTAgent_ProcessLoop( MQTTAgentContext_t * pMqttAgentContext,
                                    CommandInfo_t * pCommandInfo );
/* @[declare_mqtt_agent_processloop] */

/**
 * @brief Add a command to call MQTT_Ping() for an MQTT connection.
 *
 * @param[in] pMqttAgentContext The MQTT agent to use.
 * @param[in] pCommandInfo The information pertaining to the command, including:
 *  - cmdCompleteCallback Optional callback to invoke when the command completes.
 *  - pCmdCompleteCallbackContext Optional completion callback context.
 *  - blockTimeMs The maximum amount of time in milliseconds to wait for the
 *    command to be posted to the MQTT agent, should the agent's event queue
 *    be full. Tasks wait in the Blocked state so don't use any CPU time.
 *
 * @note The context passed to the callback through pCmdContext member of
 * @p pCommandInfo parameter MUST remain in scope at least until the callback
 * has been executed by the agent task.
 *
 * @return `MQTTSuccess` if the command was posted to the MQTT agent's event queue.
 * Otherwise an enumerated error code.
 */
/* @[declare_mqtt_agent_ping] */
MQTTStatus_t MQTTAgent_Ping( MQTTAgentContext_t * pMqttAgentContext,
                             CommandInfo_t * pCommandInfo );
/* @[declare_mqtt_agent_ping] */

/**
 * @brief Add a command to call MQTT_Connect() for an MQTT connection. If a session
 * is resumed with the broker, it will also resend the necessary QoS1/2 publishes.
 *
 * @param[in] pMqttAgentContext The MQTT agent to use.
 * @param[in] pConnectArgs Struct holding args for MQTT_Connect().
 * @param[in] pCommandInfo The information pertaining to the command, including:
 *  - cmdCompleteCallback Optional callback to invoke when the command completes.
 *  - pCmdCompleteCallbackContext Optional completion callback context.
 *  - blockTimeMs The maximum amount of time in milliseconds to wait for the
 *    command to be posted to the MQTT agent, should the agent's event queue
 *    be full. Tasks wait in the Blocked state so don't use any CPU time.
 *
 * @note The context passed to the callback through pCmdContext member of
 * @p pCommandInfo parameter MUST remain in scope at least until the callback
 * has been executed by the agent task.
 *
 * @return `MQTTSuccess` if the command was posted to the MQTT agent's event queue.
 * Otherwise an enumerated error code.
 */
/* @[declare_mqtt_agent_connect] */
MQTTStatus_t MQTTAgent_Connect( MQTTAgentContext_t * pMqttAgentContext,
                                MQTTAgentConnectArgs_t * pConnectArgs,
                                CommandInfo_t * pCommandInfo );
/* @[declare_mqtt_agent_connect] */

/**
 * @brief Add a command to disconnect an MQTT connection.
 *
 * @param[in] pMqttAgentContext The MQTT agent to use.
 * @param[in] pCommandInfo The information pertaining to the command, including:
 *  - cmdCompleteCallback Optional callback to invoke when the command completes.
 *  - pCmdCompleteCallbackContext Optional completion callback context.
 *  - blockTimeMs The maximum amount of time in milliseconds to wait for the
 *    command to be posted to the MQTT agent, should the agent's event queue
 *    be full. Tasks wait in the Blocked state so don't use any CPU time.
 *
 * @note The context passed to the callback through pCmdContext member of
 * @p pCommandInfo parameter MUST remain in scope at least until the callback
 * has been executed by the agent task.
 *
 * @return `MQTTSuccess` if the command was posted to the MQTT agent's event queue.
 * Otherwise an enumerated error code.
 */
/* @[declare_mqtt_agent_disconnect] */
MQTTStatus_t MQTTAgent_Disconnect( MQTTAgentContext_t * pMqttAgentContext,
                                   CommandInfo_t * pCommandInfo );
/* @[declare_mqtt_agent_disconnect] */

/**
 * @brief Add a termination command to the command queue.
 *
 * @param[in] pMqttAgentContext The MQTT agent to use.
 * @param[in] pCommandInfo The information pertaining to the command, including:
 *  - cmdCompleteCallback Optional callback to invoke when the command completes.
 *  - pCmdCompleteCallbackContext Optional completion callback context.
 *  - blockTimeMs The maximum amount of time in milliseconds to wait for the
 *    command to be posted to the MQTT agent, should the agent's event queue
 *    be full. Tasks wait in the Blocked state so don't use any CPU time.
 *
 * @note The context passed to the callback through pCmdContext member of
 * @p pCommandInfo parameter MUST remain in scope at least until the callback
 * has been executed by the agent task.
 *
 * @return `MQTTSuccess` if the command was posted to the MQTT agent's event queue.
 * Otherwise an enumerated error code.
 */
/* @[declare_mqtt_agent_terminate] */
MQTTStatus_t MQTTAgent_Terminate( MQTTAgentContext_t * pMqttAgentContext,
                                  CommandInfo_t * pCommandInfo );
/* @[declare_mqtt_agent_terminate] */

#endif /* MQTT_AGENT_H */
