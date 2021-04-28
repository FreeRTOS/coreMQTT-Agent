## coreMQTT Agent - Using coreMQTT

The coreMQTT Agent library is a high level API that adds thread safety to the [coreMQTT](https://github.com/FreeRTOS/coreMQTT) library. It lets you create a dedicated MQTT agent task that manages an MQTT connection in the background as a standalone task. The library provides thread safe equivalents to the coreMQTT's APIs, so it can be used in multi-threaded environments.

coreMQTT is an MIT licensed open source C MQTT client library for microcontrollers and small microprocessor based IoT devices. Its design is intentionally simple to ensure it has no dependency on any other library or operating system, and to better enable static analysis including [memory safety proofs](https://www.freertos.org/2020/02/ensuring-the-memory-safety-of-freertos-part-1.html). That simplicity and lack of operating system dependency (coreMQTT does not require multithreading at all) means coreMQTT does not build thread safety directly into its implementation. Instead thread safety must be provided by higher level software.

This library has gone through code quality checks including verification that no function has a [GNU Complexity](https://www.gnu.org/software/complexity/manual/complexity.html) score over 8, and checks against deviations from mandatory rules in the [MISRA coding standard](https://www.misra.org.uk/MISRAHome/MISRAC2012/tabid/196/Default.aspx).  Deviations from the MISRA C:2012 guidelines are documented under [MISRA Deviations](MISRA.md). This library has also undergone both static code analysis from [Coverity static analysis](https://scan.coverity.com/), and validation of memory safety and proof of functional correctness through the [CBMC automated reasoning tool](https://www.cprover.org/cbmc/).

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

The MQTT Agent library exposes build configuration macros that are required for building the library.
However, the MQTT Agent library does not include its own configuration file, but exposes configurable macros and their default values at the top of [core_mqtt_agent.h](source/include/core_mqtt_agent.h) and [core_mqtt_agent_command_functions.h](source/include/core_mqtt_agent_command_functions.h). Also, the library inherits any macros defined in coreMQTT's `core_mqtt_config.h`.

To provide custom values for the configuration values, they must be either:
* Defined in `core_mqtt_config.h` used by coreMQTT
**OR**
* Passed as compile time preprocessor macros

## Building the Library

The [mqttAgentFilePaths.cmake](mqttAgentFilePaths.cmake) file contains the information of all source files and the header include path from this repository. In addition to these files, the coreMQTT library is required to build the MQTT Agent library.

For a CMake example of building the MQTT Agent library with the `mqttAgentFilePaths.cmake` file, refer to the `coverity_analysis` library target in [test/CMakeLists.txt](test/CMakeLists.txt) file.

## Building Unit Tests

### Checkout CMock Submodule

To build unit tests, the submodule dependency of CMock is required. Use the following command to clone the submodule:
```
git submodule update --checkout --init --recursive test/unit-test/CMock
```

### Platform Prerequisites

- For running unit tests
    - **C90 compiler** like gcc
    - **CMake 3.13.0 or later**
    - **Ruby 2.0.0 or later** is additionally required for the CMock test framework (that we use).
- For running the coverage target, **gcov** and **lcov** are additionally required.

### Steps to build **Unit Tests**

1. Go to the root directory of this repository. (Make sure that the **CMock** submodule is cloned as described [above](#checkout-cmock-submodule))

1. Run the *cmake* command: `cmake -S test -B build`

1. Run this command to build the library and unit tests: `make -C build all`

1. The generated test executables will be present in `build/bin/tests` folder.

1. Run `cd build && ctest` to execute all tests and view the test run summary.


## Generating documentation

The Doxygen references were created using Doxygen version 1.8.20. To generate the
Doxygen pages, please run the following command from the root of this repository:

```shell
doxygen docs/doxygen/config.doxyfile
```

## Getting help
You can use your Github login to get support from both the FreeRTOS community and directly from the primary FreeRTOS developers on our [active support forum](https://forums.freertos.org).  The [FAQ](https://www.freertos.org/FAQ.html) provides another support resource.

## Contributing

See [CONTRIBUTING.md](./.github/CONTRIBUTING.md) for information on contributing.

## License

This library is licensed under the MIT License. See the [LICENSE](LICENSE) file.
