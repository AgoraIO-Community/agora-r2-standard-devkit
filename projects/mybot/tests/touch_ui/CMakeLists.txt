# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
project(bk7259_touch_ui_tests LANGUAGES C CXX)
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(LVGL_DIR "${REPOSITORY}/bk_avdk_smp/ap/components/lvgl/lvgl_v9")
set(PLATFORM_DIR "${REPOSITORY}/components/mybot/mybot_sdk/platforms/bk7259")
set(SDK_ROOT "${REPOSITORY}/components/mybot/mybot_sdk")
file(GLOB_RECURSE LVGL_SOURCES CONFIGURE_DEPENDS "${LVGL_DIR}/src/*.c")
list(FILTER LVGL_SOURCES EXCLUDE REGEX "/drivers/")
add_library(lvgl_host STATIC ${LVGL_SOURCES})
target_include_directories(lvgl_host PUBLIC "${LVGL_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/stubs")
target_compile_definitions(lvgl_host PUBLIC
    LV_CONF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/host_lv_conf.h" LV_KCONFIG_IGNORE=1)
option(TOUCH_UI_SANITIZERS "Enable address and undefined-behavior checks" OFF)
if(TOUCH_UI_SANITIZERS)
    target_compile_options(lvgl_host PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(lvgl_host PUBLIC -fsanitize=address,undefined)
endif()
foreach(variant zh en noanim)
    add_executable(touch_ui_${variant} test_touch_ui.cc
        "${PLATFORM_DIR}/display/lvgl_view.cc"
        "${PLATFORM_DIR}/display/lvgl_fonts.c"
        "${PLATFORM_DIR}/display/pixel_fonts.c"
        "${PLATFORM_DIR}/bk7259_expression.c")
    target_link_libraries(touch_ui_${variant} PRIVATE lvgl_host m)
    target_include_directories(touch_ui_${variant} PRIVATE
        "${PLATFORM_DIR}/display" "${PLATFORM_DIR}/include"
        "${SDK_ROOT}/include")
    target_compile_options(touch_ui_${variant} PRIVATE -Wall -Wextra -Werror)
    if(variant STREQUAL zh)
        target_compile_definitions(touch_ui_${variant} PRIVATE CONFIG_MYBOT_LANGUAGE_ZH_CN=1)
    else()
        target_compile_definitions(touch_ui_${variant} PRIVATE CONFIG_MYBOT_LANGUAGE_ZH_CN=0)
    endif()
    if(variant STREQUAL noanim)
        target_compile_definitions(touch_ui_${variant} PRIVATE CONFIG_MYBOT_LVGL_UI_ANIMATIONS=0)
    else()
        target_compile_definitions(touch_ui_${variant} PRIVATE CONFIG_MYBOT_LVGL_UI_ANIMATIONS=1)
    endif()
endforeach()
