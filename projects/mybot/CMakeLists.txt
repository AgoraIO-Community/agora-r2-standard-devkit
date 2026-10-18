cmake_minimum_required(VERSION 3.5)

include($ENV{ARMINO_TOOLS_PATH}/build_tools/cmake/project.cmake)

# components/mybot is a container whose sub-components (mybot_sdk, mybot_aosl)
# are discovered automatically.  Register the camera/codec helper component
# explicitly because this repository does not yet provide a container
# CMakeLists.txt under components/mybot.
set(EXTRA_COMPONENTS_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/mybot
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/multimedia_device_service
)

project(app)
