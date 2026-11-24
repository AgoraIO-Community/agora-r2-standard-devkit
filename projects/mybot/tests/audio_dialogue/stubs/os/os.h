#pragma once
#include <stdint.h>

uint32_t rtos_get_time(void);
uint32_t rtos_enter_global_critical(void);
void rtos_exit_global_critical(uint32_t flags);
