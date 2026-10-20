#ifndef SETTINGS_TEST_CMSIS_OS_H
#define SETTINGS_TEST_CMSIS_OS_H

#include <stdint.h>

typedef void *osThreadId_t;
typedef void *osMessageQueueId_t;
typedef int osStatus_t;
#define osOK 0
#define osErrorResource -1
#define osWaitForever UINT32_MAX

osStatus_t osMessageQueuePut(osMessageQueueId_t queue, const void *value,
                            uint8_t priority, uint32_t timeout);
uint32_t osThreadFlagsSet(osThreadId_t thread, uint32_t flags);

#endif
