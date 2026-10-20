#ifndef SETTINGS_TEST_SUPPORT_H
#define SETTINGS_TEST_SUPPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "BL24C02.h"
#include "i2c.h"
#include "user_TasksInit.h"

extern uint8_t settings_fake_image[256];
extern uint32_t settings_host_primask;
extern unsigned settings_fake_written;
extern unsigned settings_fake_writes;
extern bool settings_fake_fail_reads;
extern bool settings_fake_inject_reentry;
extern bool settings_fake_reentry_rejected;

extern uint8_t settings_fake_gate_mode;
extern CmdTxFrame_t settings_fake_last_response;
extern unsigned settings_fake_response_count;

void settings_fake_reset(const uint8_t image[256]);
void settings_fake_attach_file(const char *path);
void settings_fake_set_cut_after(long byte_count);
void settings_fake_persist(void);
const char *settings_status_name(EEPROM_LoadStatus_t status);

#endif
