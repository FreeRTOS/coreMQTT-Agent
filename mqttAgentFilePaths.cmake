# This file is to add source files and include directories
# into variables so that it can be reused from different repositories
# in their Cmake based build system by including this file.
#
# Files specific to the repository such as test runner, platform tests
# are not added to the variables.


# MQTT Agent library Public Include directories.
set( MQTT_AGENT_INCLUDE_PUBLIC_DIRS
     "${CMAKE_CURRENT_LIST_DIR}/source/include"
     "${CMAKE_CURRENT_LIST_DIR}/source/interface" )

# MQTT Agent library source files.
set( MQTT_AGENT_SOURCES
     "${MODULE_ROOT_DIR}/source/mqtt_agent.c" )

