#ifndef __BL24C02_H
#define __BL24C02_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "user_AdcDataStrategy.h"

#define BL24C02_ADDRESS	0x50
#define EEPROM_OK          0U
#define EEPROM_ERROR       1U
typedef enum {
    EEPROM_LOAD_OK = 0,
    EEPROM_LOAD_DEFAULTS,
    EEPROM_LOAD_MIGRATED,
    EEPROM_LOAD_RECOVERED,
    EEPROM_LOAD_CORRUPT,
    EEPROM_LOAD_UNSUPPORTED,
    EEPROM_LOAD_IO_ERROR
} EEPROM_LoadStatus_t;

// Status ordering defines aggregate severity; IO errors include a busy transaction.
EEPROM_LoadStatus_t EEPROM_SysSetting_Get(void);
EEPROM_LoadStatus_t Sys_Get_SettingsLoadStatus(void);
EEPROM_LoadStatus_t Sys_Get_CalibrationLoadStatus(void);
bool EEPROM_SysSetting_Save(void);
uint8_t EEPROM_Init_Check(void);
bool EEPROM_UpdateCommand_Write(bool is_update);
bool EEPROM_UpdateCommand_Check(void);

// set functions
void Sys_Set_BacklightLevel(uint8_t level);
void Sys_Set_KeySoundEnable(bool enable);
void Sys_Set_LanguageSelect(uint8_t lang);
void Sys_Set_Rotation(uint16_t rotation);
void Sys_Set_CurrentRangeMode(uint8_t mode);
void Sys_Set_MainDisplayMode(uint8_t mode);

// get functions
uint8_t Sys_Get_BacklightLevel(void);
uint8_t Sys_Get_KeySoundEnable(void);
uint8_t Sys_Get_LanguageSelect(void);
uint16_t Sys_Get_Rotation(void);
uint8_t Sys_Get_CurrentRangeMode(void);
uint8_t Sys_Get_MainDisplayMode(void);
void Sys_Get_AdcCalibration(ADC_Calibration_t *calibration);

bool EEPROM_ReadBytes(uint8_t address, uint8_t *data, uint16_t length);
bool EEPROM_WritePage(uint8_t address, const uint8_t *data, uint8_t length);
bool EEPROM_WriteBytes(uint8_t address, const uint8_t *data, uint16_t length);

#endif
