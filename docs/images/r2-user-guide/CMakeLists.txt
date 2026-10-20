# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
project(r2_manual_ui LANGUAGES C CXX)
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(PLATFORM_DIR "${REPOSITORY}/components/mybot/mybot_sdk/platforms/bk7259")
set(SDK_ROOT "${REPOSITORY}/components/mybot/mybot_sdk")
# Reuse the exact LVGL host configuration used by the product UI tests.
add_subdirectory("${REPOSITORY}/projects/mybot/tests/touch_ui" touch_ui EXCLUDE_FROM_ALL)
add_executable(r2_manual_ui render-ui.cc
    "${PLATFORM_DIR}/display/lvgl_view.cc"
    "${PLATFORM_DIR}/display/lvgl_fonts.c"
    "${PLATFORM_DIR}/display/pixel_fonts.c"
    "${PLATFORM_DIR}/bk7259_expression.c")
target_link_libraries(r2_manual_ui PRIVATE lvgl_host m)
target_include_directories(r2_manual_ui PRIVATE
    "${PLATFORM_DIR}/display" "${PLATFORM_DIR}/include" "${SDK_ROOT}/include")
target_compile_definitions(r2_manual_ui PRIVATE
    CONFIG_MYBOT_LANGUAGE_ZH_CN=1 CONFIG_MYBOT_LVGL_UI_ANIMATIONS=1)
target_compile_options(r2_manual_ui PRIVATE -Wall -Wextra -Werror)
