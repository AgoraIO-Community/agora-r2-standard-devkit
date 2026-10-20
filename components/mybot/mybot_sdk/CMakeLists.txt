armino_build_get_property(armino_soc ARMINO_SOC)
if(NOT armino_soc STREQUAL "bk7259_ap")
    armino_component_register()
    return()
endif()

# The validated BK7259 RTSA SDK (v1.10.1 build 1278380) is vendored under
# ../../mybot_rtsa as include/ and lib/arm/.  Its header and static library are
# pinned below so the build cannot silently consume a different build.
set(_rtsa_sdk_dir "${CMAKE_CURRENT_LIST_DIR}/../mybot_rtsa")
get_filename_component(_rtsa_sdk_dir "${_rtsa_sdk_dir}" ABSOLUTE)
set(_rtsa_header "${_rtsa_sdk_dir}/include/agora_rtc_api.h")
set(_rtsa_library "${_rtsa_sdk_dir}/lib/arm/libagora-rtc-sdk.a")

set(MYBOT_AUDIO_PTIME_MS "60")
if(DEFINED ENV{MYBOT_AUDIO_PTIME_MS} AND NOT "$ENV{MYBOT_AUDIO_PTIME_MS}" STREQUAL "")
    set(MYBOT_AUDIO_PTIME_MS "$ENV{MYBOT_AUDIO_PTIME_MS}")
endif()
if(NOT MYBOT_AUDIO_PTIME_MS STREQUAL "60")
    message(FATAL_ERROR
        "BK7259 RTSA package build 1278380 is fixed at 60 ms; "
        "MYBOT_AUDIO_PTIME_MS must be 60")
endif()

set(_rtsa_header_sha256
    "29b1c21cbe416ee703eaa197ce623469619b1ed68cd3e9c66cb3abe2fafec7db")
set(_rtsa_library_sha256
    "40e8f93d9077eefe888e37b42ca0ddb5c7b2b6256b2f00cb7ba7c11c5551cb0f")

if(NOT EXISTS "${_rtsa_header}" OR NOT EXISTS "${_rtsa_library}")
    message(FATAL_ERROR
        "Vendored RTSA SDK is missing at ${_rtsa_sdk_dir}; expected "
        "include/agora_rtc_api.h and lib/arm/libagora-rtc-sdk.a from the "
        "validated BK7259 RTSA v1.10.1 build 1278380 package")
endif()
file(SHA256 "${_rtsa_header}" _actual_header_sha256)
file(SHA256 "${_rtsa_library}" _actual_library_sha256)
if(NOT _actual_header_sha256 STREQUAL _rtsa_header_sha256)
    message(FATAL_ERROR "Unexpected RTSA header SHA256: ${_actual_header_sha256}")
endif()
if(NOT _actual_library_sha256 STREQUAL _rtsa_library_sha256)
    message(FATAL_ERROR "Unexpected RTSA library SHA256: ${_actual_library_sha256}")
endif()

set(MYBOT_VERSION_MAJOR 1)
set(MYBOT_VERSION_MINOR 3)
set(MYBOT_VERSION_PATCH 0)
set(MYBOT_VERSION_PRERELEASE "")
set(MYBOT_VERSION_STRING "1.3.0")
set(_generated_include_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
file(MAKE_DIRECTORY "${_generated_include_dir}/mybot")
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/include/mybot/mybot_version.h.in"
    "${_generated_include_dir}/mybot/mybot_version.h"
    @ONLY
)

set(sdk_srcs
    src/core/mybot_app.c
    src/core/mybot_device_command.c
    src/core/mybot_device_lifecycle.c
    src/core/mybot_presenter.c
    src/core/mybot_state_model.c
    src/core/mybot_version.c
    src/media/mybot_audio.c
    src/media/mybot_media_pipeline.c
    src/media/mybot_video.c
    src/media/mybot_wake_words.c
    src/platform/mybot_announce.c
    src/platform/mybot_https.c
    src/platform/mybot_key.c
    src/platform/mybot_kv_store.c
    src/platform/mybot_lcd.c
    src/platform/mybot_platform.c
    src/platform/mybot_wifi.c
    src/rtc/mybot_agora_rtc.c
    src/service/mybot_device_client.c
    src/support/mybot_http_client.c
    src/support/mybot_json.c
    src/support/mybot_ringbuf.c
)

set(platform_srcs
    platforms/bk7259/bk7259_platform.c
    platforms/bk7259/bk7259_audio.c
    platforms/bk7259/bk7259_volume.c
    platforms/bk7259/bk7259_announce.c
    platforms/bk7259/bk7259_ogg_pcm.c
    platforms/bk7259/bk7259_assets.c
    platforms/bk7259/bk7259_prompt.c
    platforms/bk7259/bk7259_kv_store.c
    platforms/bk7259/bk7259_key.c
    platforms/bk7259/bk7259_factory_reset.c
    platforms/bk7259/bk7259_expression.c
    platforms/bk7259/bk7259_lcd.c
    platforms/bk7259/bk7259_touch.c
    platforms/bk7259/display/lvgl_view.cc
    platforms/bk7259/display/lvgl_fonts.c
    platforms/bk7259/display/pixel_fonts.c
    platforms/bk7259/bk7259_wifi.c
    platforms/bk7259/bk7259_wifi_portal.c
    platforms/bk7259/bk7259_https.c
)

if(CONFIG_MYBOT_VIDEO)
    list(APPEND platform_srcs platforms/bk7259/bk7259_video.c)
endif()

if(CONFIG_MYBOT_GIMBAL)
    list(APPEND platform_srcs platforms/bk7259/bk7259_gimbal.c)
endif()

set(mybot_priv_requires
    mybot_aosl
    audio_pipeline
    onboard_mic_stream
    onboard_speaker_stream
    raw_stream
    aec_v3_algorithm
    bk_display
    lvgl
    media_service
    bk_frame_buffer
    bk_peripheral
    bk_pm
    easy_flash
    bk_event
    bk_wifi
    bk_netif
    json
    psa_mbedtls
    lwip_intf_v2_1
    bk_common
    bk_rtos
    bk_libs
)
if(CONFIG_MYBOT_VIDEO)
    list(APPEND mybot_priv_requires multimedia_device_service)
endif()

armino_component_register(
    SRCS "${sdk_srcs}" "${platform_srcs}"
    INCLUDE_DIRS
        "${_generated_include_dir}"
        include
        platforms/bk7259/include
    PRIV_INCLUDE_DIRS
        src/internal
        "${_rtsa_sdk_dir}/include"
        platforms/bk7259
        # app_camera/app_codec are solution-owned video helper headers.  Keep
        # this private so the public MyBot include surface stays platform-only.
        "${CMAKE_CURRENT_LIST_DIR}/../../multimedia_device_service/include"
    PRIV_REQUIRES ${mybot_priv_requires}
)

add_prebuilt_library(mybot_rtsa "${_rtsa_library}" PRIV_REQUIRES mybot_aosl)
target_link_libraries(${COMPONENT_LIB} INTERFACE mybot_rtsa)

target_compile_features(${COMPONENT_LIB} PRIVATE c_std_99 cxx_std_11)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -fvisibility=hidden
    $<$<COMPILE_LANGUAGE:C>:-Werror=incompatible-pointer-types>
)
# Keep the device-service URL scheme and SDK transport policy in sync.
target_compile_definitions(${COMPONENT_LIB}
    PRIVATE MYBOT_BUILDING_LIBRARY
    PUBLIC
        MYBOT_AUDIO_PTIME_MS=${MYBOT_AUDIO_PTIME_MS}
        MYBOT_CLOUD_AEC=0
        MYBOT_WAKE_WORDS=0
        MYBOT_ENABLE_VIDEO=$<BOOL:${CONFIG_MYBOT_VIDEO}>
        MYBOT_AI_QOS=1
        MYBOT_FAST_SEND_MULTIPLIER=3
        MYBOT_ENABLE_HTTPS=$<BOOL:${CONFIG_MYBOT_HTTPS}>
        MYBOT_ALLOW_INSECURE_HTTP=$<NOT:$<BOOL:${CONFIG_MYBOT_HTTPS}>>
)
