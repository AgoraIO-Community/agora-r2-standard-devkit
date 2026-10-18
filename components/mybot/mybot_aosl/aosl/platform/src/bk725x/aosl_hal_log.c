#include <stdio.h>
#include <os/os.h>
#include <os/mem.h>

#include <hal/aosl_hal_log.h>

#define AOSL_LOG_BUFFER_SIZE (8 * 1024)

int aosl_hal_printf(const char *format, va_list args)
{
  char *buffer = psram_malloc(AOSL_LOG_BUFFER_SIZE);
  int result;

  if (buffer == NULL) {
    return -1;
  }

  result = vsnprintf(buffer, AOSL_LOG_BUFFER_SIZE, format, args);
  printf("[%u]%s", (unsigned int)rtos_get_time(), buffer);
  psram_free(buffer);

  return result < 0 ? -1 : result;
}
