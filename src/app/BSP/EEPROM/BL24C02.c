#include "BL24C02.h"
#include "i2c.h"

#include <float.h>
#include <stddef.h>

// hardware settings
#include "gate.h"
#include "lcd_init.h"

#define EEPROM_PAGE_SIZE             16U
#define EEPROM_LEGACY_SETTINGS_ADDR   0x40U
#define EEPROM_LEGACY_SETTINGS_SIZE   32U
#define EEPROM_CALIBRATION_ADDR       0x60U
#define EEPROM_USER_SLOT_A_ADDR       0x90U
#define EEPROM_USER_SLOT_B_ADDR       0xB0U
#define EEPROM_CAL_CRC_ADDR           0xD0U
#define EEPROM_CALIBRATION_SIZE       48U
#define EEPROM_USER_SLOT_SIZE         32U
#define EEPROM_CAL_CRC_SIZE           16U

#define RECORD_HEADER_SIZE            8U
#define RECORD_COMMIT_SIZE            2U
#define RECORD_SCHEMA                 1U
#define RECORD_COMMIT_0               0xA5U
#define RECORD_COMMIT_1               0x5AU

#define USER_PAYLOAD_SIZE             7U
#define CALIBRATION_PAYLOAD_SIZE      24U
#define CAL_CRC_PAYLOAD_SIZE          4U

#define USER_MAGIC_0                  0x55U
#define USER_MAGIC_1                  0x53U
#define CAL_MAGIC_0                   0x43U
#define CAL_MAGIC_1                   0x41U
#define CAL_CRC_MAGIC_0               0x43U
#define CAL_CRC_MAGIC_1               0x4DU

typedef char EEPROM_FloatMustBeBinary32[
    (sizeof(float) == 4U && FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128)
        ? 1 : -1];

typedef struct {
    uint8_t backlight_level;
    uint8_t key_sound_enable;
    uint8_t language_select;
    uint16_t rotation;
    uint8_t current_range_mode;
    uint8_t main_display_mode;
} SysSettings_T;

typedef enum {
    RECORD_BLANK,
    RECORD_VALID,
    RECORD_UNSUPPORTED,
    RECORD_CORRUPT
} RecordState_T;

typedef struct {
    RecordState_T state;
    uint32_t generation;
    bool committed;
} RecordInfo_T;

static SysSettings_T sys_settings = {
    .backlight_level = 50U,
    .key_sound_enable = 1U,
    .language_select = 0U,
    .rotation = 0U,
    .current_range_mode = GATE_MODE_AUTO,
    .main_display_mode = 0U,
};

static ADC_Calibration_t adc_calibration = {
    .low_scale_multiplier = 1.0f,
    .mid_scale_multiplier = 1.0f,
    .high_scale_multiplier = 1.0f,
    .low_offset_ua = 0.0f,
    .mid_offset_ua = 0.0f,
    .high_offset_ua = 0.0f,
};

static EEPROM_LoadStatus_t settings_load_status = EEPROM_LOAD_DEFAULTS;
static EEPROM_LoadStatus_t calibration_load_status = EEPROM_LOAD_DEFAULTS;
static bool storage_busy;
static bool user_write_blocked = true;
static bool user_record_active;
static uint8_t user_active_slot;
static uint32_t user_active_generation;
static uint8_t user_active_payload[USER_PAYLOAD_SIZE];

static uint32_t IRQ_SaveAndDisable(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

static void IRQ_Restore(uint32_t primask)
{
    __set_PRIMASK(primask);
}

static bool Storage_Begin(void)
{
    uint32_t primask = IRQ_SaveAndDisable();
    bool acquired = !storage_busy;

    if (acquired) {
        storage_busy = true;
    }
    IRQ_Restore(primask);
    return acquired;
}

static void Storage_End(void)
{
    uint32_t primask = IRQ_SaveAndDisable();
    storage_busy = false;
    IRQ_Restore(primask);
}

static uint16_t ReadU16LE(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t ReadU32LE(const uint8_t *data)
{
    return (uint32_t)data[0]
        | ((uint32_t)data[1] << 8)
        | ((uint32_t)data[2] << 16)
        | ((uint32_t)data[3] << 24);
}

static void WriteU16LE(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static void WriteU32LE(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static uint16_t EEPROM_Crc16(const uint8_t *data, uint16_t length)
{
    uint16_t crc = 0xFFFFU;
    uint16_t index;

    for (index = 0U; index < length; index++) {
        uint8_t bit;

        crc ^= (uint16_t)data[index] << 8;
        for (bit = 0U; bit < 8U; bit++) {
            crc = (crc & 0x8000U) != 0U
                ? (uint16_t)((crc << 1) ^ 0x1021U)
                : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static bool BytesAre(const uint8_t *data, uint16_t length, uint8_t value)
{
    uint16_t index;

    for (index = 0U; index < length; index++) {
        if (data[index] != value) {
            return false;
        }
    }
    return true;
}

static bool Record_IsBlank(const uint8_t *record, uint16_t size)
{
    return BytesAre(record, size, 0xFFU);
}

static bool Record_HasCommit(const uint8_t *record, uint16_t size)
{
    return record[size - 2U] == RECORD_COMMIT_0
        && record[size - 1U] == RECORD_COMMIT_1;
}

static RecordInfo_T Record_Parse(const uint8_t *record,
                                 uint16_t size,
                                 uint8_t magic_0,
                                 uint8_t magic_1,
                                 uint8_t expected_payload_size)
{
    RecordInfo_T info = { RECORD_CORRUPT, 0U, false };
    uint16_t crc_offset = (uint16_t)(size - 4U);
    uint8_t payload_size;
    uint16_t index;

    if (Record_IsBlank(record, size)) {
        info.state = RECORD_BLANK;
        return info;
    }

    info.committed = Record_HasCommit(record, size);
    if (record[0] != magic_0 || record[1] != magic_1
        || !info.committed
        || ReadU16LE(&record[crc_offset]) != EEPROM_Crc16(record, crc_offset)) {
        return info;
    }

    payload_size = record[3];
    if (payload_size > (uint8_t)(crc_offset - RECORD_HEADER_SIZE)) {
        return info;
    }
    info.generation = ReadU32LE(&record[4]);
    if (record[2] != RECORD_SCHEMA) {
        info.state = RECORD_UNSUPPORTED;
        return info;
    }
    if (payload_size != expected_payload_size) {
        return info;
    }
    for (index = (uint16_t)(RECORD_HEADER_SIZE + payload_size);
         index < crc_offset;
         index++) {
        if (record[index] != 0U) {
            return info;
        }
    }

    info.state = RECORD_VALID;
    return info;
}

static void Record_Build(uint8_t *record,
                         uint16_t size,
                         uint8_t magic_0,
                         uint8_t magic_1,
                         uint32_t generation,
                         const uint8_t *payload,
                         uint8_t payload_size)
{
    uint16_t crc_offset = (uint16_t)(size - 4U);

    memset(record, 0, size);
    record[0] = magic_0;
    record[1] = magic_1;
    record[2] = RECORD_SCHEMA;
    record[3] = payload_size;
    WriteU32LE(&record[4], generation);
    memcpy(&record[RECORD_HEADER_SIZE], payload, payload_size);
    WriteU16LE(&record[crc_offset], EEPROM_Crc16(record, crc_offset));
    record[size - 2U] = RECORD_COMMIT_0;
    record[size - 1U] = RECORD_COMMIT_1;
}

static bool Record_Commit(uint8_t address, const uint8_t *record, uint16_t size)
{
    uint8_t invalid_marker[RECORD_COMMIT_SIZE] = { 0U, 0U };
    uint8_t verify[EEPROM_CALIBRATION_SIZE];
    uint16_t body_size = (uint16_t)(size - RECORD_COMMIT_SIZE);

    if (!EEPROM_WriteBytes((uint8_t)(address + body_size),
                           invalid_marker,
                           sizeof(invalid_marker))
        || !EEPROM_ReadBytes((uint8_t)(address + body_size),
                             verify,
                             sizeof(invalid_marker))
        || memcmp(verify, invalid_marker, sizeof(invalid_marker)) != 0) {
        return false;
    }
    if (!EEPROM_WriteBytes(address, record, body_size)
        || !EEPROM_ReadBytes(address, verify, body_size)
        || memcmp(verify, record, body_size) != 0) {
        return false;
    }
    if (!EEPROM_WriteBytes((uint8_t)(address + body_size),
                           &record[body_size],
                           RECORD_COMMIT_SIZE)
        || !EEPROM_ReadBytes(address, verify, size)
        || memcmp(verify, record, size) != 0) {
        return false;
    }
    return true;
}

static bool Record_MatchesInterruptedPrefix(const uint8_t *stored,
                                            const uint8_t *expected,
                                            uint16_t size)
{
    uint16_t body_size = (uint16_t)(size - RECORD_COMMIT_SIZE);
    uint16_t prefix;
    uint8_t marker_0 = stored[body_size];
    uint8_t marker_1 = stored[body_size + 1U];

    if (Record_IsBlank(stored, size)) {
        return true;
    }
    for (prefix = 0U; prefix <= body_size; prefix++) {
        if (memcmp(stored, expected, prefix) != 0
            || !BytesAre(&stored[prefix], (uint16_t)(body_size - prefix), 0xFFU)) {
            continue;
        }
        if ((marker_0 == 0x00U && marker_1 == 0xFFU && prefix == 0U)
            || (marker_0 == 0x00U && marker_1 == 0x00U)
            || (marker_0 == RECORD_COMMIT_0 && marker_1 == 0x00U
                && prefix == body_size)) {
            return true;
        }
    }
    return false;
}

bool EEPROM_ReadBytes(uint8_t address, uint8_t *data, uint16_t length)
{
    if (data == NULL || length == 0U || ((uint32_t)address + length) > 256U) {
        return false;
    }

    return HAL_I2C_Mem_Read(&hi2c1,
                            BL24C02_ADDRESS << 1,
                            address,
                            I2C_MEMADD_SIZE_8BIT,
                            data,
                            length,
                            50U) == HAL_OK;
}

bool EEPROM_WritePage(uint8_t address, const uint8_t *data, uint8_t length)
{
    if (data == NULL
        || length == 0U
        || length > EEPROM_PAGE_SIZE
        || ((address & (EEPROM_PAGE_SIZE - 1U)) + length) > EEPROM_PAGE_SIZE
        || ((uint16_t)address + length) > 256U) {
        return false;
    }
    if (HAL_I2C_Mem_Write(&hi2c1,
                          BL24C02_ADDRESS << 1,
                          address,
                          I2C_MEMADD_SIZE_8BIT,
                          (uint8_t *)data,
                          length,
                          50U) != HAL_OK) {
        return false;
    }

    return HAL_I2C_IsDeviceReady(&hi2c1,
                                 BL24C02_ADDRESS << 1,
                                 20U,
                                 2U) == HAL_OK;
}

bool EEPROM_WriteBytes(uint8_t address, const uint8_t *data, uint16_t length)
{
    uint16_t current_address = address;
    uint16_t remaining = length;

    if (data == NULL || length == 0U || ((uint32_t)address + length) > 256U) {
        return false;
    }

    while (remaining != 0U) {
        uint8_t page_space = (uint8_t)(EEPROM_PAGE_SIZE
            - (current_address & (EEPROM_PAGE_SIZE - 1U)));
        uint8_t chunk_length = remaining < page_space
            ? (uint8_t)remaining
            : page_space;

        if (!EEPROM_WritePage((uint8_t)current_address, data, chunk_length)) {
            return false;
        }
        current_address += chunk_length;
        data += chunk_length;
        remaining -= chunk_length;
    }

    return true;
}

static uint8_t EEPROM_Init_Check_Internal(void)
{
    uint8_t check_buff[2];

    HAL_Delay(10U);
    if (!EEPROM_ReadBytes(0U, check_buff, sizeof(check_buff))) {
        return EEPROM_ERROR;
    }
    if (check_buff[0] == 0x55U && check_buff[1] == 0xAAU) {
        return EEPROM_OK;
    }

    check_buff[0] = 0x55U;
    check_buff[1] = 0xAAU;
    if (!EEPROM_WritePage(0U, check_buff, sizeof(check_buff))) {
        return EEPROM_ERROR;
    }
    memset(check_buff, 0, sizeof(check_buff));
    if (!EEPROM_ReadBytes(0U, check_buff, sizeof(check_buff))) {
        return EEPROM_ERROR;
    }

    return (check_buff[0] == 0x55U && check_buff[1] == 0xAAU)
        ? EEPROM_OK
        : EEPROM_ERROR;
}

uint8_t EEPROM_Init_Check(void)
{
    uint8_t result;

    if (!Storage_Begin()) {
        return EEPROM_ERROR;
    }
    result = EEPROM_Init_Check_Internal();
    Storage_End();
    return result;
}

static void SysSettings_SetDefault(SysSettings_T *settings)
{
    settings->backlight_level = 50U;
    settings->key_sound_enable = 1U;
    settings->language_select = 0U;
    settings->rotation = 0U;
    settings->current_range_mode = GATE_MODE_AUTO;
    settings->main_display_mode = 0U;
}

static bool UserSettings_IsValid(const SysSettings_T *settings)
{
    bool rotation_valid = settings->rotation == 0U
        || settings->rotation == 90U
        || settings->rotation == 180U
        || settings->rotation == 270U;
    bool range_valid = settings->current_range_mode == GATE_MODE_AUTO
        || settings->current_range_mode == GATE_MODE_LOW
        || settings->current_range_mode == GATE_MODE_MID
        || settings->current_range_mode == GATE_MODE_HIGH;

    return settings->backlight_level <= 100U
        && settings->key_sound_enable <= 1U
        && settings->language_select <= 1U
        && rotation_valid
        && range_valid
        && settings->main_display_mode <= 1U;
}

static void UserSettings_Encode(const SysSettings_T *settings,
                                uint8_t payload[USER_PAYLOAD_SIZE])
{
    payload[0] = settings->backlight_level;
    payload[1] = settings->key_sound_enable;
    payload[2] = settings->language_select;
    WriteU16LE(&payload[3], settings->rotation);
    payload[5] = settings->current_range_mode;
    payload[6] = settings->main_display_mode;
}

static void UserSettings_Decode(const uint8_t payload[USER_PAYLOAD_SIZE],
                                SysSettings_T *settings)
{
    settings->backlight_level = payload[0];
    settings->key_sound_enable = payload[1];
    settings->language_select = payload[2];
    settings->rotation = ReadU16LE(&payload[3]);
    settings->current_range_mode = payload[5];
    settings->main_display_mode = payload[6];
}

static bool LegacyUserSettings_Decode(const uint8_t legacy[EEPROM_LEGACY_SETTINGS_SIZE],
                                      SysSettings_T *settings)
{
    settings->backlight_level = legacy[0];
    settings->key_sound_enable = legacy[1];
    settings->language_select = legacy[2];
    settings->rotation = ReadU16LE(&legacy[4]);
    settings->current_range_mode = legacy[6];
    settings->main_display_mode = 0U;
    return UserSettings_IsValid(settings);
}

static void User_RecordBuild(uint8_t record[EEPROM_USER_SLOT_SIZE],
                             uint32_t generation,
                             const uint8_t payload[USER_PAYLOAD_SIZE])
{
    Record_Build(record,
                 EEPROM_USER_SLOT_SIZE,
                 USER_MAGIC_0,
                 USER_MAGIC_1,
                 generation,
                 payload,
                 USER_PAYLOAD_SIZE);
}

static bool User_Commit(uint8_t slot,
                        uint32_t generation,
                        const uint8_t payload[USER_PAYLOAD_SIZE])
{
    uint8_t record[EEPROM_USER_SLOT_SIZE];
    uint8_t address = slot == 0U ? EEPROM_USER_SLOT_A_ADDR : EEPROM_USER_SLOT_B_ADDR;

    User_RecordBuild(record, generation, payload);
    return Record_Commit(address, record, sizeof(record));
}

static bool Legacy_Read(uint8_t legacy[EEPROM_LEGACY_SETTINGS_SIZE])
{
    return EEPROM_ReadBytes(EEPROM_LEGACY_SETTINGS_ADDR,
                            legacy,
                            EEPROM_LEGACY_SETTINGS_SIZE);
}

static void User_SetLoaded(const SysSettings_T *settings)
{
    uint32_t primask = IRQ_SaveAndDisable();
    sys_settings = *settings;
    IRQ_Restore(primask);
}

static void User_Activate(uint8_t slot,
                          uint32_t generation,
                          const uint8_t payload[USER_PAYLOAD_SIZE])
{
    user_record_active = true;
    user_active_slot = slot;
    user_active_generation = generation;
    memcpy(user_active_payload, payload, USER_PAYLOAD_SIZE);
}

static bool User_LoadLegacyAndMigrate(const uint8_t *slot_a, bool slot_a_is_blank)
{
    uint8_t legacy[EEPROM_LEGACY_SETTINGS_SIZE];
    uint8_t payload[USER_PAYLOAD_SIZE];
    uint8_t expected[EEPROM_USER_SLOT_SIZE];
    SysSettings_T imported;
    bool legacy_valid;

    if (!Legacy_Read(legacy)) {
        settings_load_status = EEPROM_LOAD_IO_ERROR;
        user_write_blocked = true;
        return false;
    }
    SysSettings_SetDefault(&imported);
    legacy_valid = LegacyUserSettings_Decode(legacy, &imported);

    if (!slot_a_is_blank && !legacy_valid) {
        SysSettings_SetDefault(&imported);
        User_SetLoaded(&imported);
        settings_load_status = EEPROM_LOAD_CORRUPT;
        user_write_blocked = true;
        return false;
    }
    if (!legacy_valid) {
        SysSettings_SetDefault(&imported);
        User_SetLoaded(&imported);
        settings_load_status = EEPROM_LOAD_DEFAULTS;
        user_write_blocked = false;
        return true;
    }

    UserSettings_Encode(&imported, payload);
    User_RecordBuild(expected, 0U, payload);
    if (!slot_a_is_blank
        && !Record_MatchesInterruptedPrefix(slot_a, expected, sizeof(expected))) {
        SysSettings_SetDefault(&imported);
        User_SetLoaded(&imported);
        settings_load_status = EEPROM_LOAD_CORRUPT;
        user_write_blocked = true;
        return false;
    }

    User_SetLoaded(&imported);
    if (User_Commit(0U, 0U, payload)) {
        User_Activate(0U, 0U, payload);
        settings_load_status = EEPROM_LOAD_MIGRATED;
    } else {
        settings_load_status = EEPROM_LOAD_IO_ERROR;
        user_write_blocked = false;
    }
    return true;
}

static int Generation_Compare(uint32_t left, uint32_t right)
{
    uint32_t delta = left - right;

    if (delta == 0U) {
        return 0;
    }
    if (delta == 0x80000000UL) {
        return 2;
    }
    return delta < 0x80000000UL ? 1 : -1;
}

static void User_LoadDomain(void)
{
    uint8_t slots[2][EEPROM_USER_SLOT_SIZE];
    RecordInfo_T info[2];
    SysSettings_T decoded[2];
    uint8_t payload[2][USER_PAYLOAD_SIZE];
    uint8_t addresses[2] = { EEPROM_USER_SLOT_A_ADDR, EEPROM_USER_SLOT_B_ADDR };
    bool valid[2] = { false, false };
    bool unknown[2] = { false, false };
    bool blank[2];
    int selected = -1;
    uint8_t index;

    user_record_active = false;
    user_write_blocked = false;
    if (!EEPROM_ReadBytes(addresses[0], slots[0], EEPROM_USER_SLOT_SIZE)
        || !EEPROM_ReadBytes(addresses[1], slots[1], EEPROM_USER_SLOT_SIZE)) {
        SysSettings_T defaults;
        SysSettings_SetDefault(&defaults);
        User_SetLoaded(&defaults);
        settings_load_status = EEPROM_LOAD_IO_ERROR;
        user_write_blocked = true;
        return;
    }

    for (index = 0U; index < 2U; index++) {
        info[index] = Record_Parse(slots[index],
                                   EEPROM_USER_SLOT_SIZE,
                                   USER_MAGIC_0,
                                   USER_MAGIC_1,
                                   USER_PAYLOAD_SIZE);
        blank[index] = info[index].state == RECORD_BLANK;
        if (info[index].state == RECORD_VALID) {
            UserSettings_Decode(&slots[index][RECORD_HEADER_SIZE], &decoded[index]);
            if (UserSettings_IsValid(&decoded[index])) {
                valid[index] = true;
                UserSettings_Encode(&decoded[index], payload[index]);
            } else {
                info[index].state = RECORD_CORRUPT;
            }
        } else if (info[index].state == RECORD_UNSUPPORTED) {
            unknown[index] = true;
        }
    }

    if (unknown[0] || unknown[1]) {
        int unknown_slot = unknown[0] ? 0 : 1;
        int supported_slot = valid[0] ? 0 : (valid[1] ? 1 : -1);

        if (supported_slot >= 0
            && Generation_Compare(info[supported_slot].generation,
                                  info[unknown_slot].generation) == 1) {
            User_SetLoaded(&decoded[supported_slot]);
            User_Activate((uint8_t)supported_slot,
                          info[supported_slot].generation,
                          payload[supported_slot]);
        } else {
            SysSettings_T defaults;
            SysSettings_SetDefault(&defaults);
            User_SetLoaded(&defaults);
        }
        settings_load_status = EEPROM_LOAD_UNSUPPORTED;
        user_write_blocked = true;
        return;
    }

    if (valid[0] && valid[1]) {
        int order = Generation_Compare(info[0].generation, info[1].generation);

        if (order == 0 && memcmp(slots[0], slots[1], EEPROM_USER_SLOT_SIZE) == 0) {
            selected = 0;
        } else if (order == 1 || order == -1) {
            selected = order == 1 ? 0 : 1;
        } else {
            SysSettings_T defaults;
            SysSettings_SetDefault(&defaults);
            User_SetLoaded(&defaults);
            settings_load_status = EEPROM_LOAD_CORRUPT;
            user_write_blocked = true;
            return;
        }
    } else if (valid[0] || valid[1]) {
        selected = valid[0] ? 0 : 1;
    }

    if (selected >= 0) {
        uint8_t other = (uint8_t)(1 - selected);

        User_SetLoaded(&decoded[selected]);
        User_Activate((uint8_t)selected, info[selected].generation, payload[selected]);
        settings_load_status = info[other].state == RECORD_CORRUPT
            ? EEPROM_LOAD_RECOVERED
            : EEPROM_LOAD_OK;
        return;
    }

    if (blank[0] && blank[1]) {
        (void)User_LoadLegacyAndMigrate(slots[0], true);
        return;
    }

    if (!info[0].committed && blank[1]) {
        (void)User_LoadLegacyAndMigrate(slots[0], false);
        return;
    }

    {
        SysSettings_T defaults;
        SysSettings_SetDefault(&defaults);
        User_SetLoaded(&defaults);
    }
    settings_load_status = EEPROM_LOAD_CORRUPT;
    user_write_blocked = true;
}

static void Calibration_FloatToLE(uint8_t *destination, float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    WriteU32LE(destination, bits);
}

static float Calibration_FloatFromLE(const uint8_t *source)
{
    uint32_t bits = ReadU32LE(source);
    float value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void Calibration_Encode(const ADC_Calibration_t *calibration,
                               uint8_t payload[CALIBRATION_PAYLOAD_SIZE])
{
    Calibration_FloatToLE(&payload[0], calibration->low_scale_multiplier);
    Calibration_FloatToLE(&payload[4], calibration->mid_scale_multiplier);
    Calibration_FloatToLE(&payload[8], calibration->high_scale_multiplier);
    Calibration_FloatToLE(&payload[12], calibration->low_offset_ua);
    Calibration_FloatToLE(&payload[16], calibration->mid_offset_ua);
    Calibration_FloatToLE(&payload[20], calibration->high_offset_ua);
}

static void Calibration_Decode(const uint8_t payload[CALIBRATION_PAYLOAD_SIZE],
                               ADC_Calibration_t *calibration)
{
    calibration->low_scale_multiplier = Calibration_FloatFromLE(&payload[0]);
    calibration->mid_scale_multiplier = Calibration_FloatFromLE(&payload[4]);
    calibration->high_scale_multiplier = Calibration_FloatFromLE(&payload[8]);
    calibration->low_offset_ua = Calibration_FloatFromLE(&payload[12]);
    calibration->mid_offset_ua = Calibration_FloatFromLE(&payload[16]);
    calibration->high_offset_ua = Calibration_FloatFromLE(&payload[20]);
}

static bool LegacyCalibration_Decode(const uint8_t legacy[EEPROM_LEGACY_SETTINGS_SIZE],
                                     ADC_Calibration_t *calibration)
{
    Calibration_Decode(&legacy[8], calibration);
    return ADC_Calibration_IsValid(calibration);
}

static void Calibration_Activate(const ADC_Calibration_t *calibration)
{
    uint32_t primask = IRQ_SaveAndDisable();
    adc_calibration = *calibration;
    IRQ_Restore(primask);
}

static void Calibration_RecordBuild(uint8_t record[EEPROM_CALIBRATION_SIZE],
                                    const ADC_Calibration_t *calibration)
{
    uint8_t payload[CALIBRATION_PAYLOAD_SIZE];

    Calibration_Encode(calibration, payload);
    Record_Build(record,
                 EEPROM_CALIBRATION_SIZE,
                 CAL_MAGIC_0,
                 CAL_MAGIC_1,
                 0U,
                 payload,
                 CALIBRATION_PAYLOAD_SIZE);
}

// Metadata payload v1: LE CRC16 of legacy bytes 0x48..0x5F, then LE raw length 24.

static void Calibration_MetadataBuild(uint8_t record[EEPROM_CAL_CRC_SIZE],
                                      const uint8_t legacy[EEPROM_LEGACY_SETTINGS_SIZE])
{
    uint8_t payload[CAL_CRC_PAYLOAD_SIZE];

    WriteU16LE(&payload[0], EEPROM_Crc16(&legacy[8], CALIBRATION_PAYLOAD_SIZE));
    WriteU16LE(&payload[2], CALIBRATION_PAYLOAD_SIZE);
    Record_Build(record,
                 EEPROM_CAL_CRC_SIZE,
                 CAL_CRC_MAGIC_0,
                 CAL_CRC_MAGIC_1,
                 0U,
                 payload,
                 CAL_CRC_PAYLOAD_SIZE);
}

static bool Calibration_MetadataMatchesLegacy(const uint8_t *metadata,
                                             const uint8_t legacy[EEPROM_LEGACY_SETTINGS_SIZE])
{
    const uint8_t *payload = &metadata[RECORD_HEADER_SIZE];

    return ReadU16LE(&payload[2]) == CALIBRATION_PAYLOAD_SIZE
        && ReadU16LE(payload) == EEPROM_Crc16(&legacy[8], CALIBRATION_PAYLOAD_SIZE);
}

static bool Calibration_Commit(uint8_t record[EEPROM_CALIBRATION_SIZE])
{
    return Record_Commit(EEPROM_CALIBRATION_ADDR, record, EEPROM_CALIBRATION_SIZE);
}

static bool Calibration_MetadataCommit(uint8_t record[EEPROM_CAL_CRC_SIZE])
{
    return Record_Commit(EEPROM_CAL_CRC_ADDR, record, EEPROM_CAL_CRC_SIZE);
}

static void Calibration_SetDefaults(void)
{
    ADC_Calibration_t defaults;

    ADC_Calibration_SetDefault(&defaults);
    Calibration_Activate(&defaults);
}

static void Calibration_LoadDomain(void)
{
    uint8_t primary[EEPROM_CALIBRATION_SIZE];
    uint8_t metadata[EEPROM_CAL_CRC_SIZE];
    uint8_t legacy[EEPROM_LEGACY_SETTINGS_SIZE];
    uint8_t expected_primary[EEPROM_CALIBRATION_SIZE];
    uint8_t expected_metadata[EEPROM_CAL_CRC_SIZE];
    RecordInfo_T primary_info;
    RecordInfo_T metadata_info;
    ADC_Calibration_t calibration;
    bool primary_blank;
    bool metadata_blank;
    bool metadata_valid;

    if (!EEPROM_ReadBytes(EEPROM_CALIBRATION_ADDR, primary, sizeof(primary))
        || !EEPROM_ReadBytes(EEPROM_CAL_CRC_ADDR, metadata, sizeof(metadata))) {
        Calibration_SetDefaults();
        calibration_load_status = EEPROM_LOAD_IO_ERROR;
        return;
    }

    primary_info = Record_Parse(primary,
                               EEPROM_CALIBRATION_SIZE,
                               CAL_MAGIC_0,
                               CAL_MAGIC_1,
                               CALIBRATION_PAYLOAD_SIZE);
    metadata_info = Record_Parse(metadata,
                                 EEPROM_CAL_CRC_SIZE,
                                 CAL_CRC_MAGIC_0,
                                 CAL_CRC_MAGIC_1,
                                 CAL_CRC_PAYLOAD_SIZE);
    primary_blank = primary_info.state == RECORD_BLANK;
    metadata_blank = metadata_info.state == RECORD_BLANK;

    if (primary_info.state == RECORD_VALID) {
        Calibration_Decode(&primary[RECORD_HEADER_SIZE], &calibration);
        if (ADC_Calibration_IsValid(&calibration)) {
            Calibration_Activate(&calibration);
            calibration_load_status = EEPROM_LOAD_OK;
            return;
        }
        primary_info.state = RECORD_CORRUPT;
    } else if (primary_info.state == RECORD_UNSUPPORTED) {
        Calibration_SetDefaults();
        calibration_load_status = EEPROM_LOAD_UNSUPPORTED;
        return;
    }

    if (metadata_info.state == RECORD_UNSUPPORTED) {
        Calibration_SetDefaults();
        calibration_load_status = EEPROM_LOAD_UNSUPPORTED;
        return;
    }
    if (metadata_info.state == RECORD_VALID
        && ReadU16LE(&metadata[RECORD_HEADER_SIZE + 2U]) != CALIBRATION_PAYLOAD_SIZE) {
        metadata_info.state = RECORD_CORRUPT;
    }
    metadata_valid = metadata_info.state == RECORD_VALID;

    if (!Legacy_Read(legacy)) {
        Calibration_SetDefaults();
        calibration_load_status = EEPROM_LOAD_IO_ERROR;
        return;
    }
    if (!LegacyCalibration_Decode(legacy, &calibration)) {
        Calibration_SetDefaults();
        calibration_load_status = (primary_blank && metadata_blank)
            ? EEPROM_LOAD_DEFAULTS
            : EEPROM_LOAD_CORRUPT;
        return;
    }
    Calibration_RecordBuild(expected_primary, &calibration);
    Calibration_MetadataBuild(expected_metadata, legacy);

    if (metadata_valid) {
        if (!Calibration_MetadataMatchesLegacy(metadata, legacy)) {
            Calibration_SetDefaults();
            calibration_load_status = EEPROM_LOAD_CORRUPT;
            return;
        }

        if (!primary_info.committed
            && Record_MatchesInterruptedPrefix(primary, expected_primary, sizeof(primary))) {
            if (Calibration_Commit(expected_primary)) {
                Calibration_Activate(&calibration);
                calibration_load_status = EEPROM_LOAD_MIGRATED;
            } else {
                Calibration_Activate(&calibration);
                calibration_load_status = EEPROM_LOAD_IO_ERROR;
            }
            return;
        }

        Calibration_Activate(&calibration);
        calibration_load_status = EEPROM_LOAD_RECOVERED;
        return;
    }

    if (!metadata_info.committed
        && (metadata_blank
            || Record_MatchesInterruptedPrefix(metadata,
                                               expected_metadata,
                                               sizeof(metadata)))
        && primary_blank) {
        if (!Calibration_MetadataCommit(expected_metadata)) {
            Calibration_Activate(&calibration);
            calibration_load_status = EEPROM_LOAD_IO_ERROR;
            return;
        }
        if (!Calibration_Commit(expected_primary)) {
            Calibration_Activate(&calibration);
            calibration_load_status = EEPROM_LOAD_IO_ERROR;
            return;
        }
        Calibration_Activate(&calibration);
        calibration_load_status = EEPROM_LOAD_MIGRATED;
        return;
    }

    Calibration_SetDefaults();
    calibration_load_status = metadata_info.state == RECORD_CORRUPT
        || primary_info.state == RECORD_CORRUPT
        ? EEPROM_LOAD_CORRUPT
        : EEPROM_LOAD_DEFAULTS;
}

static EEPROM_LoadStatus_t LoadStatus_Worse(EEPROM_LoadStatus_t left,
                                            EEPROM_LoadStatus_t right)
{
    return left > right ? left : right;
}

EEPROM_LoadStatus_t EEPROM_SysSetting_Get(void)
{
    EEPROM_LoadStatus_t result;
    uint32_t primask;

    if (!Storage_Begin()) {
        primask = IRQ_SaveAndDisable();
        settings_load_status = EEPROM_LOAD_IO_ERROR;
        calibration_load_status = EEPROM_LOAD_IO_ERROR;
        IRQ_Restore(primask);
        return EEPROM_LOAD_IO_ERROR;
    }

    primask = IRQ_SaveAndDisable();
    SysSettings_SetDefault(&sys_settings);
    ADC_Calibration_SetDefault(&adc_calibration);
    user_record_active = false;
    user_write_blocked = true;
    IRQ_Restore(primask);

    if (EEPROM_Init_Check_Internal() != EEPROM_OK) {
        settings_load_status = EEPROM_LOAD_IO_ERROR;
        calibration_load_status = EEPROM_LOAD_IO_ERROR;
    } else {
        User_LoadDomain();
        Calibration_LoadDomain();
    }
    result = LoadStatus_Worse(settings_load_status, calibration_load_status);
    Storage_End();
    return result;
}

EEPROM_LoadStatus_t Sys_Get_SettingsLoadStatus(void)
{
    uint32_t primask = IRQ_SaveAndDisable();
    EEPROM_LoadStatus_t status = settings_load_status;
    IRQ_Restore(primask);
    return status;
}

EEPROM_LoadStatus_t Sys_Get_CalibrationLoadStatus(void)
{
    uint32_t primask = IRQ_SaveAndDisable();
    EEPROM_LoadStatus_t status = calibration_load_status;
    IRQ_Restore(primask);
    return status;
}

bool EEPROM_SysSetting_Save(void)
{
    SysSettings_T snapshot;
    uint8_t payload[USER_PAYLOAD_SIZE];
    uint8_t target_slot;
    uint32_t generation;
    uint32_t primask;
    bool saved;

    if (!Storage_Begin()) {
        return false;
    }
    primask = IRQ_SaveAndDisable();
    snapshot = sys_settings;
    IRQ_Restore(primask);

    if (user_write_blocked || !UserSettings_IsValid(&snapshot)) {
        Storage_End();
        return false;
    }
    UserSettings_Encode(&snapshot, payload);
    if (user_record_active
        && memcmp(payload, user_active_payload, USER_PAYLOAD_SIZE) == 0) {
        Storage_End();
        return true;
    }

    target_slot = user_record_active ? (uint8_t)(user_active_slot ^ 1U) : 0U;
    generation = user_record_active ? user_active_generation + 1U : 0U;
    saved = User_Commit(target_slot, generation, payload);
    if (saved) {
        User_Activate(target_slot, generation, payload);
    }
    Storage_End();
    return saved;
}

bool EEPROM_UpdateCommand_Write(bool is_update)
{
    const uint8_t *command = (const uint8_t *)(is_update ? "update\r\n" : "-nope-\r\n");
    uint8_t verify[8];
    bool result;

    if (!Storage_Begin()) {
        return false;
    }
    result = EEPROM_WritePage(0x20U, command, sizeof(verify))
        && EEPROM_ReadBytes(0x20U, verify, sizeof(verify))
        && memcmp(command, verify, sizeof(verify)) == 0;
    Storage_End();
    return result;
}

bool EEPROM_UpdateCommand_Check(void)
{
    char command[9] = {0};
    bool result;

    if (!Storage_Begin()) {
        return false;
    }
    result = EEPROM_ReadBytes(0x20U, (uint8_t *)command, 8U)
        && strcmp(command, "update\r\n") == 0;
    Storage_End();
    return result;
}

void Sys_Set_BacklightLevel(uint8_t level)
{
    if (level <= 100U) {
        uint32_t primask = IRQ_SaveAndDisable();
        sys_settings.backlight_level = level;
        IRQ_Restore(primask);
        LCD_Set_Light(level);
    }
}

void Sys_Set_KeySoundEnable(bool enable)
{
    uint32_t primask = IRQ_SaveAndDisable();
    sys_settings.key_sound_enable = enable ? 1U : 0U;
    IRQ_Restore(primask);
}

void Sys_Set_LanguageSelect(uint8_t lang)
{
    if (lang <= 1U) {
        uint32_t primask = IRQ_SaveAndDisable();
        sys_settings.language_select = lang;
        IRQ_Restore(primask);
    }
}

void Sys_Set_Rotation(uint16_t rotation)
{
    if (rotation == 0U || rotation == 90U || rotation == 180U || rotation == 270U) {
        uint32_t primask = IRQ_SaveAndDisable();
        sys_settings.rotation = rotation;
        IRQ_Restore(primask);
        LCD_SetRotation(rotation);
    }
}

void Sys_Set_CurrentRangeMode(uint8_t mode)
{
    if (mode == GATE_MODE_AUTO
        || mode == GATE_MODE_LOW
        || mode == GATE_MODE_MID
        || mode == GATE_MODE_HIGH) {
        uint32_t primask = IRQ_SaveAndDisable();
        sys_settings.current_range_mode = mode;
        IRQ_Restore(primask);
        Gate_Set_Mode(mode);
    }
}

void Sys_Set_MainDisplayMode(uint8_t mode)
{
    if (mode <= 1U) {
        uint32_t primask = IRQ_SaveAndDisable();
        sys_settings.main_display_mode = mode;
        IRQ_Restore(primask);
    }
}

uint8_t Sys_Get_BacklightLevel(void)
{
    uint32_t primask = IRQ_SaveAndDisable();
    uint8_t value = sys_settings.backlight_level;
    IRQ_Restore(primask);
    return value;
}

uint8_t Sys_Get_KeySoundEnable(void)
{
    uint32_t primask = IRQ_SaveAndDisable();
    uint8_t value = sys_settings.key_sound_enable;
    IRQ_Restore(primask);
    return value;
}

uint8_t Sys_Get_LanguageSelect(void)
{
    uint32_t primask = IRQ_SaveAndDisable();
    uint8_t value = sys_settings.language_select;
    IRQ_Restore(primask);
    return value;
}

uint16_t Sys_Get_Rotation(void)
{
    uint32_t primask = IRQ_SaveAndDisable();
    uint16_t value = sys_settings.rotation;
    IRQ_Restore(primask);
    return value;
}

uint8_t Sys_Get_CurrentRangeMode(void)
{
    uint32_t primask = IRQ_SaveAndDisable();
    uint8_t value = sys_settings.current_range_mode;
    IRQ_Restore(primask);
    return value;
}

uint8_t Sys_Get_MainDisplayMode(void)
{
    uint32_t primask = IRQ_SaveAndDisable();
    uint8_t value = sys_settings.main_display_mode;
    IRQ_Restore(primask);
    return value;
}

void Sys_Get_AdcCalibration(ADC_Calibration_t *calibration)
{
    uint32_t primask;

    if (calibration == NULL) {
        return;
    }
    primask = IRQ_SaveAndDisable();
    *calibration = adc_calibration;
    IRQ_Restore(primask);
}
