armino_component_register(
    SRCS ap_main.c bk7259_gimbal_cli.c
    INCLUDE_DIRS .
    PRIV_REQUIRES mybot_sdk mybot_aosl media_service bk_uid psa_mbedtls bk_cli
)
