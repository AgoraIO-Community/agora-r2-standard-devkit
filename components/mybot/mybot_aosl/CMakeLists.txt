armino_build_get_property(armino_soc ARMINO_SOC)
if(NOT armino_soc STREQUAL "bk7259_ap")
    armino_component_register()
    return()
endif()

set(AOSL_DIR "${CMAKE_CURRENT_LIST_DIR}/aosl")
get_filename_component(AOSL_DIR "${AOSL_DIR}" ABSOLUTE)

if(NOT EXISTS "${AOSL_DIR}/cmake/build.cmake")
    message(FATAL_ERROR "mybot_aosl vendored source is missing at ${AOSL_DIR}")
endif()

set(CONFIG_PLATFORM bk725x)
set(CONFIG_AOSL_IPV6 y)
set(CONFIG_AOSL_MEM_STAT n)
set(CONFIG_AOSL_MEM_DUMP n)
set(AOSL_DECLARE_PROJECT OFF)
set(AOSL_ADD_OPTIONS)
set(AOSL_ADD_DEFINITIONS)
set(AOSL_ADD_INCLUDES_PUBLIC)
set(AOSL_ADD_INCLUDES_PRIVATE)
set(AOSL_ADD_SOURCES)
include("${AOSL_DIR}/cmake/build.cmake")

list(FILTER AOSL_ADD_SOURCES EXCLUDE REGEX "/test/aosl_test\\.c$")

armino_component_register(
    SRCS "${AOSL_ADD_SOURCES}"
    INCLUDE_DIRS "${AOSL_ADD_INCLUDES_PUBLIC}"
    PRIV_INCLUDE_DIRS "${AOSL_ADD_INCLUDES_PRIVATE}"
    PRIV_REQUIRES bk_common bk_rtos lwip_intf_v2_1 bk_vfs bk_uid driver
)

component_compile_options(${AOSL_ADD_OPTIONS} ${AOSL_ADD_DEFINITIONS})
target_compile_features(${COMPONENT_LIB} PRIVATE c_std_99)
