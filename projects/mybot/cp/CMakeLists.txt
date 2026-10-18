armino_component_register(
    SRCS cp_main.c
    INCLUDE_DIRS
        .
        ${ARMINO_PATH}/middleware/boards/bk7259/vnd_cal/include
    PRIV_REQUIRES vnd_cal
)
