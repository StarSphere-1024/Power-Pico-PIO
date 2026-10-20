#ifndef SETTINGS_TEST_I2C_H
#define SETTINGS_TEST_I2C_H

#include "main.h"

typedef struct { unsigned unused; } I2C_HandleTypeDef;
typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;

extern I2C_HandleTypeDef hi2c1;
#define I2C_MEMADD_SIZE_8BIT 1U

HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *handle, uint16_t device,
                                   uint16_t address, uint16_t mode, uint8_t *data,
                                   uint16_t size, uint32_t timeout);
HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *handle, uint16_t device,
                                    uint16_t address, uint16_t mode, uint8_t *data,
                                    uint16_t size, uint32_t timeout);
HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *handle, uint16_t device,
                                        uint32_t retries, uint32_t timeout);

#endif
