#ifndef SETTINGS_TEST_MAIN_H
#define SETTINGS_TEST_MAIN_H

#include <stddef.h>
#include <stdint.h>

extern uint32_t settings_host_primask;
static inline uint32_t __get_PRIMASK(void) { return settings_host_primask; }
static inline void __disable_irq(void) { settings_host_primask = 1U; }
static inline void __enable_irq(void) { settings_host_primask = 0U; }
static inline void __set_PRIMASK(uint32_t value) { settings_host_primask = value; }

void HAL_Delay(uint32_t delay);
void NVIC_SystemReset(void);

#endif
