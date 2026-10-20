#include "settings_support.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gate.h"
#include "user_CmdStrategy.h"

uint8_t settings_fake_image[256];
uint32_t settings_host_primask;
unsigned settings_fake_written;
unsigned settings_fake_writes;
bool settings_fake_fail_reads;
bool settings_fake_inject_reentry;
bool settings_fake_reentry_rejected;

uint8_t settings_fake_gate_mode;
CmdTxFrame_t settings_fake_last_response;
unsigned settings_fake_response_count;
osThreadId_t MessageSendTaskHandle = (void *)1;
osMessageQueueId_t CmdTxQueue = (void *)1;
I2C_HandleTypeDef hi2c1;

static const char *settings_fake_image_path;
static long settings_fake_cut_after = -1;

void settings_fake_reset(const uint8_t image[256])
{
    memcpy(settings_fake_image, image, sizeof(settings_fake_image));
    settings_host_primask = 0U;
    settings_fake_written = 0U;
    settings_fake_writes = 0U;
    settings_fake_fail_reads = false;
    settings_fake_inject_reentry = false;
    settings_fake_reentry_rejected = false;
    
    settings_fake_gate_mode = GATE_MODE_AUTO;
    memset(&settings_fake_last_response, 0, sizeof(settings_fake_last_response));
    settings_fake_response_count = 0U;
    settings_fake_image_path = NULL;
    settings_fake_cut_after = -1;
}

void settings_fake_attach_file(const char *path)
{
    settings_fake_image_path = path;
}

void settings_fake_set_cut_after(long byte_count)
{
    settings_fake_cut_after = byte_count;
}

void settings_fake_persist(void)
{
    FILE *file;

    if (settings_fake_image_path == NULL) {
        return;
    }
    file = fopen(settings_fake_image_path, "wb");
    assert(file != NULL);
    assert(fwrite(settings_fake_image, 1U, sizeof(settings_fake_image), file)
           == sizeof(settings_fake_image));
    assert(fclose(file) == 0);
}

HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *handle, uint16_t device,
        uint16_t address, uint16_t mode, uint8_t *data, uint16_t size, uint32_t timeout)
{
    (void)handle;
    (void)device;
    (void)mode;
    (void)timeout;
    assert((uint32_t)address + size <= sizeof(settings_fake_image));
    if (settings_fake_fail_reads) {
        return HAL_ERROR;
    }
    memcpy(data, settings_fake_image + address, size);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *handle, uint16_t device,
        uint16_t address, uint16_t mode, uint8_t *data, uint16_t size, uint32_t timeout)
{
    (void)handle;
    (void)device;
    (void)mode;
    (void)timeout;
    assert((uint32_t)address + size <= sizeof(settings_fake_image));
    assert((address & 15U) + size <= 16U);
    /* Legacy calibration and settings are immutable, including on torn writes. */
    assert((uint32_t)address + size <= 0x40U || address >= 0x60U);
    if (settings_fake_inject_reentry) {
        settings_fake_inject_reentry = false;
        settings_fake_reentry_rejected = !EEPROM_SysSetting_Save();
    }
    ++settings_fake_writes;
    for (unsigned i = 0U; i < size; ++i) {
        if (settings_fake_cut_after >= 0
            && settings_fake_written == (unsigned long)settings_fake_cut_after) {
            settings_fake_persist();
            exit(99);
        }
        settings_fake_image[address + i] = data[i];
        ++settings_fake_written;
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *handle, uint16_t device,
        uint32_t retries, uint32_t timeout)
{
    (void)handle;
    (void)device;
    (void)retries;
    (void)timeout;
    return HAL_OK;
}

void HAL_Delay(uint32_t delay) { (void)delay; }
void LCD_Set_Light(uint8_t value) { (void)value; }
void LCD_SetRotation(uint16_t value) { (void)value; }
void Gate_Set_Mode(uint8_t mode) { settings_fake_gate_mode = mode; }
uint8_t Gate_Get_Mode(void) { return settings_fake_gate_mode; }
uint8_t Gate_get_status(void) { return settings_fake_gate_mode; }
void flow_route_selection(uint8_t mode) { settings_fake_gate_mode = mode; }
uint64_t GetMicrosecondCounter(void) { return 0U; }
void USER_USB_DEVICE_DeInit(void) { abort(); }
void NVIC_SystemReset(void) { abort(); }

osStatus_t osMessageQueuePut(osMessageQueueId_t queue, const void *value,
                             uint8_t priority, uint32_t timeout)
{
    (void)priority;
    (void)timeout;
    assert(queue == CmdTxQueue);
    settings_fake_last_response = *(const CmdTxFrame_t *)value;
    ++settings_fake_response_count;
    return osOK;
}

uint32_t osThreadFlagsSet(osThreadId_t thread, uint32_t flags)
{
    assert(thread == MessageSendTaskHandle);
    return flags;
}

const char *settings_status_name(EEPROM_LoadStatus_t status)
{
    switch (status) {
        case EEPROM_LOAD_OK: return "OK";
        case EEPROM_LOAD_DEFAULTS: return "DEFAULTS";
        case EEPROM_LOAD_MIGRATED: return "MIGRATED";
        case EEPROM_LOAD_RECOVERED: return "RECOVERED";
        case EEPROM_LOAD_CORRUPT: return "CORRUPT";
        case EEPROM_LOAD_UNSUPPORTED: return "UNSUPPORTED";
        case EEPROM_LOAD_IO_ERROR: return "IO_ERROR";
    }
    abort();
}
