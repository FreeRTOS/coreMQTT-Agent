## coreMQTT Agent Library

The coreMQTT Agent library is a high level API that adds thread safety to the [coreMQTT](https://github.com/FreeRTOS/coreMQTT) library. The library provides thread safe equivalents to the coreMQTT's APIs, greatly simplifying its use in multi-threaded environments. The coreMQTT Agent library manages the MQTT connection by serializing the access to the coreMQTT library and reducing implementation overhead (e.g., removing the need for the application to repeatedly call to `MQTT_ProcessLoop`). This allows your multi-threaded applications to share the same MQTT connection, and enables you to design an embedded application without having to worry about coreMQTT thread safety.

This library has gone through code quality checks including verification that no function has a [GNU Complexity](https://www.gnu.org/software/complexity/manual/complexity.html) score over 8, and checks against deviations from mandatory rules in the [MISRA coding standard](https://www.misra.org.uk).  Deviations from the MISRA C:2012 guidelines are documented under [MISRA Deviations](MISRA.md). This library has also undergone both static code analysis from [Coverity static analysis](https://scan.coverity.com/), and validation of memory safety through the [CBMC automated reasoning tool](https://www.cprover.org/cbmc/).

See memory requirements for this library [here](./docs/doxygen/include/size_table.md).

## Cloning this repository
This repo uses [Git Submodules](https://git-scm.com/book/en/v2/Git-Tools-Submodules) to bring in dependent components.

To clone using HTTPS:
```
git clone https://github.com/FreeRTOS/coreMQTT-Agent.git --recurse-submodules
```
Using SSH:
```
git clone git@github.com:FreeRTOS/coreMQTT-Agent.git --recurse-submodules
```

If you have downloaded the repo without using the `--recurse-submodules` argument, you need to run:
```
git submodule update --init --recursive
```

## coreMQTT Agent Library Configurations

The MQTT Agent library uses the same `core_mqtt_config.h` configuration file as coreMQTT, with the addition of configuration constants listed at the top of [core_mqtt_agent.h](source/include/core_mqtt_agent.h) and [core_mqtt_agent_command_functions.h](source/include/core_mqtt_agent_command_functions.h). Documentation for these configurations can be found [here](https://freertos.github.io/coreMQTT-Agent/main/core_mqtt_agent_config.html).

To provide values for these configuration values, they must be either:
* Defined in `core_mqtt_config.h` used by coreMQTT
**OR**
* Passed as compile time preprocessor macros

### Configuration values to modify timing performance

#### Agent queue block time:
Some TCP stacks provide a select functionality which allows any given observer to be notified of any incoming packet on the socket. This allows us to make the MQTT-Agent event driven by calling `MQTTAgent_ProcessLoop` when the observer is notified of any incoming packets on the socket. FreeRTOS+TCP can also use socket callbacks to notify the socket owner of incoming data on the socket.
If your TCP stack does not provide this functionality, the agent will block on its event queue for a maximum of `MQTT_AGENT_MAX_EVENT_QUEUE_WAIT_TIME` milliseconds after which it calls `MQTTAgent_ProcessLoop`. This leads to delay between when the packet actually arrives and when it actually gets processed. Thus, if you want to make the MQTT Agent more responsive, you can make the `MQTT_AGENT_MAX_EVENT_QUEUE_WAIT_TIME` value smaller. This would cause the agent to check the socket more often thereby reducing the time between when an incoming packet is received and when it is processed.

NOTE: While making the above change it is very important to understand that reducing the `MQTT_AGENT_MAX_EVENT_QUEUE_WAIT_TIME` value would cause the agent process loop to run more often which might have an impact on the runtime of other tasks.


#### Socket read/recv timeout:
Most TCP stacks allow applications to block on a socket read (also called recv()) call when there is no data to be read. If you are using the MQTT Agent, then the socket read timeout must be set to 0 – that is, the socket must be non-blocking.
When the socket is set to blocking - every read operation of the socket blocks the Agent for the duration of the timeout. This stops the agent from processing other incoming requests such as processing a message which the application wants to publish.
This is emphasized in cases where QoS0 publishes are involved because in this case there is no incoming packet (e.g. PUB-ACK) to unblock the agent from the socket read operation. Thus, when the application decides to publish a message, that message has to wait in the agent queue as the agent is blocked. This leads to very slow turn-around time.


#### Agent task priority:
MQTT Agent is designed to work best when its priority is set to be higher than all the tasks using it. This allows the agent to quickly process all the requests from the application as well as incoming data from the MQTT message broker.
If the agent is of lower priority than the tasks using it then the user-task can continuously put requests in the agent queue without giving agent a chance to process those requests. This process would go on till the agent queue is fully filled and the producer task blocks. Only then the Agent task might get a chance to process a message from the queue.


## Porting the coreMQTT Agent Library
In order to use the MQTT Agent library on a platform, you need to supply thread safe functions for the agent's [messaging interface](source/include/core_mqtt_agent_message_interface.h).

### Messaging Interface
Each of the following functions must be thread safe.

| Function Pointer | Description |
| :-: | --- |
| `MQTTAgentMessageSend_t` | A function that sends commands (as `MQTTAgentCommand_t *` pointers) to be received by `MQTTAgent_CommandLoop`. This can be implemented by pushing to a thread safe queue. |
| `MQTTAgentMessageRecv_t` | A function used by `MQTTAgent_CommandLoop` to receive `MQTTAgentCommand_t *` pointers that were sent by API functions. This can be implemented by receiving from a thread safe queue. |
| `MQTTAgentCommandGet_t` | A function that returns a pointer to an allocated `MQTTAgentCommand_t` structure, which is used to hold information and arguments for a command to be executed in `MQTTAgent_CommandLoop()`. If using dynamic memory, this can be implemented using `malloc()`. |
| `MQTTAgentCommandRelease_t` | A function called to indicate that a command structure that had been allocated with the `MQTTAgentCommandGet_t` function pointer will no longer be used by the agent, so it may be freed or marked as not in use. If using dynamic memory, this can be implemented with `free()`. |

Reference implementations for the interface functions can be found in the [reference examples](#reference-examples) below.
### Additional Considerations

#### Static Memory
If only static allocation is used, then the `MQTTAgentCommandGet_t` and `MQTTAgentCommandRelease_t` could instead be implemented with a pool of `MQTTAgentCommand_t` structures, with a queue or semaphore used to control access and provide thread safety. The below [reference examples](#reference-examples) use static memory with a command pool.

#### Subscription Management
The MQTT Agent does not track subscriptions for MQTT topics. The receipt of any incoming PUBLISH packet will result in the invocation of a single `MQTTAgentIncomingPublishCallback_t` callback, which is passed to `MQTTAgent_Init()` for initialization. If it is desired for different handlers to be invoked for different incoming topics, then the publish callback will have to manage subscriptions and fan out messages. A platform independent subscription manager example is implemented in the [reference examples](#reference-examples) below.

## Building the Library

You can build the MQTT Agent source files that are in the [source](source/) directory, and add [source/include](source/include) to your compiler's include path. Additionally, the MQTT Agent library requires the coreMQTT library, whose files follow the same `source/` and `source/include` pattern as the agent library; its build instructions can be found [here](https://github.com/FreeRTOS/coreMQTT#building-the-library).

If using CMake, the [mqttAgentFilePaths.cmake](mqttAgentFilePaths.cmake) file contains the above information of the source files and the header include path from this repository. The same information is found for coreMQTT from `mqttFilePaths.cmake` in the [coreMQTT submodule](source/dependency/).

For a CMake example of building the MQTT Agent library with the `mqttAgentFilePaths.cmake` file, refer to the `coverity_analysis` library target in [test/CMakeLists.txt](test/CMakeLists.txt) file.

## Building Unit Tests

### Checkout CMock Submodule

To build unit tests, the submodule dependency of CMock is required. Use the following command to clone the submodule:
```
git submodule update --checkout --init --recursive test/unit-test/CMock
```

### Unit Test Platform Prerequisites

- For running unit tests
    - **C90 compiler** like gcc
    - **CMake 3.13.0 or later**
    - **Ruby 2.0.0 or later** is additionally required for the CMock test framework (that we use).
- For running the coverage target, **gcov** and **lcov** are additionally required.

### Steps to build **Unit Tests**

1. Go to the root directory of this repository. (Make sure that the **CMock** submodule is cloned as described [above](#checkout-cmock-submodule))

1. Run the *cmake* command:

    For Linux machines:
    ```
    cmake -S test -B build/ \
              -G "Unix Makefiles" \
              -DCMAKE_BUILD_TYPE=Debug  \
              -DBUILD_CLONE_SUBMODULES=ON \
              -DUNITTEST=1 \
              -DCMAKE_C_FLAGS='--coverage -Wall -Wextra -Wsign-compare -Werror -DNDEBUG -DLIBRARY_LOG_LEVEL=LOG_DEBUG'
    ```
    For Mac machines:

    ```
    cmake -S test -B build/ \
              -G "Unix Makefiles" \
              -DCMAKE_BUILD_TYPE=RelWithDebInfo  \
              -DBUILD_CLONE_SUBMODULES=ON \
              -DUNITTEST=1 \
              -DCMAKE_C_FLAGS='--coverage -Wall -Wextra -Wsign-compare -Werror -DNDEBUG -DLIBRARY_LOG_LEVEL=LOG_DEBUG' \
              -DCMAKE_C_STANDARD=99
    ```

1. Run this command to build the library and unit tests: `make -C build all`

1. The generated test executables will be present in `build/bin/tests` folder.

1. Run `cd build && ctest` to execute all tests and view the test run summary.

## CBMC

To learn more about CBMC and proofs specifically, review the training material [here](https://model-checking.github.io/cbmc-training).

The `test/cbmc/proofs` directory contains CBMC proofs.

In order to run these proofs you will need to install CBMC and other tools by following the instructions [here](https://model-checking.github.io/cbmc-training/installation.html).

## Reference examples

Please refer to the demos of the MQTT Agent library in the following locations for reference examples on FreeRTOS platforms:

| Location |
| :-: |
| [coreMQTT Agent Demos](https://github.com/FreeRTOS/coreMQTT-Agent-Demos) |
| [FreeRTOS/FreeRTOS](https://github.com/FreeRTOS/FreeRTOS/tree/main/FreeRTOS-Plus/Demo/coreMQTT_Windows_Simulator/MQTT_Multitask) |


## Documentation

The MQTT Agent API documentation can be found [here](https://freertos.github.io/coreMQTT-Agent/main/index.html).

### Generating documentation

The Doxygen references were created using Doxygen version 1.9.2. To generate the
Doxygen pages yourself, please run the following command from the root of this repository:

```shell
doxygen docs/doxygen/config.doxyfile
```

## Getting help
You can use your Github login to get support from both the FreeRTOS community and directly from the primary FreeRTOS developers on our [active support forum](https://forums.freertos.org). You can find a list of [frequently asked questions](https://www.freertos.org/Why-FreeRTOS/FAQs) here.

## Contributing

See [CONTRIBUTING.md](./.github/CONTRIBUTING.md) for information on contributing.

## License

This library is licensed under the MIT License. See the [LICENSE](LICENSE) file.
