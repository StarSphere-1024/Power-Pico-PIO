#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "gate.h"
#include "settings_support.h"
#include "user_AdcDataStrategy.h"
#include "user_CmdStrategy.h"

static const uint8_t calibration_bytes[24] = {
    0x00, 0x00, 0x90, 0x3f, 0x00, 0x00, 0x60, 0x3f,
    0x00, 0x00, 0x88, 0x3f, 0x00, 0x00, 0x80, 0x3e,
    0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0xc8, 0x42
};

static void make_legacy_image(uint8_t image[256])
{
    static const uint8_t settings[8] = { 50U, 1U, 1U, 0U, 180U, 0U, 2U, 0U };

    memset(image, 0xff, 256U);
    image[0] = 0x55U;
    image[1] = 0xaaU;
    image[0x10] = 0x50U;
    image[0x11] = 1U;
    image[0x12] = 0U;
    image[0x13] = 0U;
    image[0x14] = 0U;
    memcpy(&image[0x20], "-nope-\r\n", 8U);
    memcpy(&image[0x40], settings, sizeof(settings));
    memcpy(&image[0x48], calibration_bytes, sizeof(calibration_bytes));
}

static uint16_t crc16(const uint8_t *data, unsigned size)
{
    uint16_t crc = 0xffffU;

    for (unsigned i = 0U; i < size; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 0x8000U) != 0U
                ? (uint16_t)((crc << 1) ^ 0x1021U)
                : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void crc_record(uint8_t image[256], unsigned start, unsigned size)
{
    unsigned end = start + size - 4U;
    uint16_t crc = crc16(&image[start], end - start);

    image[end] = (uint8_t)crc;
    image[end + 1U] = (uint8_t)(crc >> 8);
}

static void set_generation(uint8_t image[256], unsigned address, uint32_t generation,
                           unsigned size)
{
    image[address + 4U] = (uint8_t)generation;
    image[address + 5U] = (uint8_t)(generation >> 8);
    image[address + 6U] = (uint8_t)(generation >> 16);
    image[address + 7U] = (uint8_t)(generation >> 24);
    crc_record(image, address, size);
}

static void migrate_fixture(uint8_t migrated[256])
{
    uint8_t legacy[256];

    make_legacy_image(legacy);
    settings_fake_reset(legacy);
    (void)EEPROM_SysSetting_Get();
    memcpy(migrated, settings_fake_image, sizeof(settings_fake_image));
}


static CmdTxFrame_t send_command(uint8_t opcode, const uint8_t *payload, uint16_t size)
{
    uint8_t frame[58] = { 0xa5U, 0x5aU, 1U, opcode, 0x34U, 0x12U, 0U, 0U };
    uint16_t crc;

    frame[6] = (uint8_t)size;
    frame[7] = (uint8_t)(size >> 8);
    if (size != 0U) {
        memcpy(frame + 8U, payload, size);
    }
    crc = crc16(frame + 2U, 6U + size);
    frame[8U + size] = (uint8_t)crc;
    frame[9U + size] = (uint8_t)(crc >> 8);
    CmdStrategy_ProcessRx(frame, (uint16_t)(size + 10U));
    return settings_fake_last_response;
}

static void assert_response(const CmdTxFrame_t *response, uint8_t opcode,
                            uint8_t status, uint16_t expected_length)
{
    uint16_t crc;

    TEST_ASSERT_EQUAL_UINT16(expected_length, response->length);
    TEST_ASSERT_EQUAL_UINT8(0xa5U, response->data[0]);
    TEST_ASSERT_EQUAL_UINT8(0x5aU, response->data[1]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(opcode | 0x80U), response->data[3]);
    TEST_ASSERT_EQUAL_UINT8(0x34U, response->data[4]);
    TEST_ASSERT_EQUAL_UINT8(0x12U, response->data[5]);
    TEST_ASSERT_EQUAL_UINT8(status, response->data[8]);
    crc = crc16(response->data + 2U, response->length - 4U);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)crc, response->data[response->length - 2U]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(crc >> 8), response->data[response->length - 1U]);
}

static void assert_calibration(void)
{
    ADC_Calibration_t calibration;

    Sys_Get_AdcCalibration(&calibration);
    TEST_ASSERT_FLOAT_WITHIN(0.000001f, 1.125f, calibration.low_scale_multiplier);
    TEST_ASSERT_FLOAT_WITHIN(0.000001f, 0.875f, calibration.mid_scale_multiplier);
    TEST_ASSERT_FLOAT_WITHIN(0.000001f, 1.0625f, calibration.high_scale_multiplier);
    TEST_ASSERT_FLOAT_WITHIN(0.000001f, 0.25f, calibration.low_offset_ua);
    TEST_ASSERT_FLOAT_WITHIN(0.000001f, -2.0f, calibration.mid_offset_ua);
    TEST_ASSERT_FLOAT_WITHIN(0.000001f, 100.0f, calibration.high_offset_ua);
}

void setUp(void)
{
    uint8_t blank[256];

    memset(blank, 0xff, sizeof(blank));
    settings_fake_reset(blank);
    CmdStrategy_Init();
}

void tearDown(void) {}

static void test_legacy_migration_persistence_and_unchanged_writes(void)
{
    uint8_t legacy[256];
    uint8_t migrated[256];
    uint8_t saved_image[256];
    ADC_Calibration_t calibration;

    make_legacy_image(legacy);
    settings_fake_reset(legacy);
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_MIGRATED, EEPROM_SysSetting_Get());
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_MIGRATED, Sys_Get_SettingsLoadStatus());
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_MIGRATED, Sys_Get_CalibrationLoadStatus());
    TEST_ASSERT_EQUAL_UINT8(50U, Sys_Get_BacklightLevel());
    TEST_ASSERT_EQUAL_UINT8(1U, Sys_Get_KeySoundEnable());
    TEST_ASSERT_EQUAL_UINT8(1U, Sys_Get_LanguageSelect());
    TEST_ASSERT_EQUAL_UINT16(180U, Sys_Get_Rotation());
    TEST_ASSERT_EQUAL_UINT8(GATE_MODE_MID, Sys_Get_CurrentRangeMode());
    TEST_ASSERT_EQUAL_UINT8(0U, Sys_Get_MainDisplayMode());
    assert_calibration();
    Sys_Get_AdcCalibration(&calibration);
    TEST_ASSERT_EQUAL_MEMORY(calibration_bytes, &calibration, sizeof(calibration));
    TEST_ASSERT_EQUAL_MEMORY(legacy, settings_fake_image, 0x60U);
    TEST_ASSERT_EQUAL_MEMORY(legacy + 0xe0U, settings_fake_image + 0xe0U, 0x20U);
    
    memcpy(migrated, settings_fake_image, sizeof(migrated));

    settings_fake_reset(migrated);
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_OK, EEPROM_SysSetting_Get());
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_OK, Sys_Get_SettingsLoadStatus());
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_OK, Sys_Get_CalibrationLoadStatus());
    TEST_ASSERT_EQUAL_UINT(0U, settings_fake_written);
    TEST_ASSERT_EQUAL_MEMORY(migrated, settings_fake_image, sizeof(migrated));
    TEST_ASSERT_TRUE(EEPROM_SysSetting_Save());
    TEST_ASSERT_EQUAL_UINT(0U, settings_fake_written);

    Sys_Set_BacklightLevel(70U);
    Sys_Set_MainDisplayMode(1U);
    TEST_ASSERT_TRUE(EEPROM_SysSetting_Save());
    TEST_ASSERT_EQUAL_MEMORY(migrated, settings_fake_image, 0x90U);
    TEST_ASSERT_EQUAL_MEMORY(migrated + 0xd0U, settings_fake_image + 0xd0U, 0x30U);
    memcpy(saved_image, settings_fake_image, sizeof(saved_image));

    settings_fake_reset(saved_image);
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_OK, EEPROM_SysSetting_Get());
    TEST_ASSERT_EQUAL_UINT8(70U, Sys_Get_BacklightLevel());
    TEST_ASSERT_EQUAL_UINT8(1U, Sys_Get_MainDisplayMode());
    TEST_ASSERT_TRUE(EEPROM_SysSetting_Save());
    TEST_ASSERT_EQUAL_UINT(0U, settings_fake_written);
    TEST_ASSERT_EQUAL_MEMORY(saved_image, settings_fake_image, sizeof(saved_image));
}

static void test_calibration_commands_are_read_only_and_crc_valid(void)
{
    uint8_t migrated[256];
    uint8_t before[256];
    uint8_t payload[24] = { 0xc3U, 0x3cU };
    float expected[6] = { 1.125f, 0.25f, 0.875f, -2.0f, 1.0625f, 100.0f };
    CmdTxFrame_t response;

    migrate_fixture(migrated);
    settings_fake_reset(migrated);
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_OK, EEPROM_SysSetting_Get());
    memcpy(before, settings_fake_image, sizeof(before));
    settings_fake_written = 0U;
    settings_fake_writes = 0U;
    CmdStrategy_Init();

    response = send_command(CMD_CAL_SET, payload, sizeof(payload));
    assert_response(&response, CMD_CAL_SET, 8U, 11U);
    response = send_command(CMD_CAL_RESET, payload, 2U);
    assert_response(&response, CMD_CAL_RESET, 8U, 11U);
    response = send_command(CMD_CAL_SET, payload, 23U);
    assert_response(&response, CMD_CAL_SET, 3U, 11U);
    payload[0] = 0U;
    response = send_command(CMD_CAL_RESET, payload, 2U);
    assert_response(&response, CMD_CAL_RESET, 4U, 11U);
    response = send_command(CMD_CAL_GET, NULL, 0U);
    assert_response(&response, CMD_CAL_GET, 0U, 35U);
    TEST_ASSERT_EQUAL_MEMORY(expected, response.data + 9U, sizeof(expected));
    TEST_ASSERT_EQUAL_UINT(5U, settings_fake_response_count);
    TEST_ASSERT_EQUAL_UINT(0U, settings_fake_written);
    TEST_ASSERT_EQUAL_UINT(0U, settings_fake_writes);
    TEST_ASSERT_EQUAL_MEMORY(before, settings_fake_image, sizeof(before));
    
}

static void test_calibration_integrity_recovery_and_metadata_damage(void)
{
    uint8_t migrated[256];
    uint8_t damaged[256];
    uint8_t before[256];
    CmdTxFrame_t response;

    migrate_fixture(migrated);
    memcpy(damaged, migrated, sizeof(damaged));
    damaged[0x60U + 8U] ^= 1U;
    settings_fake_reset(damaged);
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_RECOVERED, EEPROM_SysSetting_Get());
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_RECOVERED, Sys_Get_CalibrationLoadStatus());
    assert_calibration();
    TEST_ASSERT_EQUAL_MEMORY(damaged, settings_fake_image, sizeof(damaged));
    TEST_ASSERT_EQUAL_UINT(0U, settings_fake_written);

    damaged[0x48U] ^= 1U;
    memcpy(before, damaged, sizeof(before));
    settings_fake_reset(damaged);
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_CORRUPT, EEPROM_SysSetting_Get());
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_CORRUPT, Sys_Get_CalibrationLoadStatus());
    TEST_ASSERT_EQUAL_UINT(0U, settings_fake_written);
    CmdStrategy_Init();
    response = send_command(CMD_CAL_GET, NULL, 0U);
    assert_response(&response, CMD_CAL_GET, 5U, 11U);
    TEST_ASSERT_EQUAL_MEMORY(before, settings_fake_image, sizeof(before));

    memcpy(damaged, migrated, sizeof(damaged));
    damaged[0xd0U + 8U] ^= 1U;
    settings_fake_reset(damaged);
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_OK, EEPROM_SysSetting_Get());
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_OK, Sys_Get_CalibrationLoadStatus());
    assert_calibration();
    TEST_ASSERT_EQUAL_MEMORY(damaged, settings_fake_image, sizeof(damaged));

    damaged[0x60U + 8U] ^= 1U;
    memcpy(before, damaged, sizeof(before));
    settings_fake_reset(damaged);
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_CORRUPT, EEPROM_SysSetting_Get());
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_CORRUPT, Sys_Get_CalibrationLoadStatus());
    TEST_ASSERT_EQUAL_UINT(0U, settings_fake_written);
    TEST_ASSERT_EQUAL_MEMORY(before, settings_fake_image, sizeof(before));
    
}

static void test_settings_and_calibration_domains_are_isolated(void)
{
    uint8_t legacy[256];
    uint8_t migrated[256];
    uint8_t damaged[256];

    make_legacy_image(legacy);
    legacy[0x40U] = 255U;
    settings_fake_reset(legacy);
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_MIGRATED, EEPROM_SysSetting_Get());
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_DEFAULTS, Sys_Get_SettingsLoadStatus());
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_MIGRATED, Sys_Get_CalibrationLoadStatus());
    TEST_ASSERT_EQUAL_UINT8(50U, Sys_Get_BacklightLevel());
    assert_calibration();
    TEST_ASSERT_EQUAL_MEMORY(legacy, settings_fake_image, 0x60U);

    migrate_fixture(migrated);
    memcpy(damaged, migrated, sizeof(damaged));
    damaged[0xb0U + 8U] ^= 1U;
    settings_fake_reset(damaged);
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_RECOVERED, EEPROM_SysSetting_Get());
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_RECOVERED, Sys_Get_SettingsLoadStatus());
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_OK, Sys_Get_CalibrationLoadStatus());
    TEST_ASSERT_EQUAL_UINT8(50U, Sys_Get_BacklightLevel());
    assert_calibration();
    TEST_ASSERT_EQUAL_MEMORY(damaged, settings_fake_image, sizeof(damaged));
    
}

static void test_unknown_schemas_preserve_records_and_block_saves(void)
{
    uint8_t migrated[256];
    uint8_t future[256];
    uint8_t updated[256];

    migrate_fixture(migrated);
    memcpy(future, migrated, sizeof(future));
    future[0x90U + 2U] = 99U;
    crc_record(future, 0x90U, 32U);
    settings_fake_reset(future);
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_UNSUPPORTED, EEPROM_SysSetting_Get());
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_UNSUPPORTED, Sys_Get_SettingsLoadStatus());
    TEST_ASSERT_EQUAL_MEMORY(future, settings_fake_image, sizeof(future));
    Sys_Set_BacklightLevel(70U);
    TEST_ASSERT_FALSE(EEPROM_SysSetting_Save());
    TEST_ASSERT_EQUAL_UINT(0U, settings_fake_written);
    TEST_ASSERT_EQUAL_MEMORY(future, settings_fake_image, sizeof(future));

    settings_fake_reset(migrated);
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_OK, EEPROM_SysSetting_Get());
    Sys_Set_BacklightLevel(70U);
    TEST_ASSERT_TRUE(EEPROM_SysSetting_Save());
    memcpy(updated, settings_fake_image, sizeof(updated));
    memcpy(future, updated, sizeof(future));
    future[0xb0U + 2U] = 99U;
    crc_record(future, 0xb0U, 32U);
    settings_fake_reset(future);
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_UNSUPPORTED, EEPROM_SysSetting_Get());
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_UNSUPPORTED, Sys_Get_SettingsLoadStatus());
    Sys_Set_BacklightLevel(60U);
    TEST_ASSERT_FALSE(EEPROM_SysSetting_Save());
    TEST_ASSERT_EQUAL_UINT(0U, settings_fake_written);
    TEST_ASSERT_EQUAL_MEMORY(future, settings_fake_image, sizeof(future));

    memcpy(future, migrated, sizeof(future));
    future[0x60U + 2U] = 99U;
    crc_record(future, 0x60U, 48U);
    settings_fake_reset(future);
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_UNSUPPORTED, EEPROM_SysSetting_Get());
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_UNSUPPORTED, Sys_Get_CalibrationLoadStatus());
    TEST_ASSERT_EQUAL_MEMORY(future, settings_fake_image, sizeof(future));
    TEST_ASSERT_EQUAL_UINT(0U, settings_fake_written);
    
}

static void test_settings_corruption_recovers_and_blocks_unsafe_writes(void)
{
    uint8_t migrated[256];
    uint8_t updated[256];
    uint8_t damaged[256];

    migrate_fixture(migrated);
    settings_fake_reset(migrated);
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_OK, EEPROM_SysSetting_Get());
    Sys_Set_BacklightLevel(70U);
    Sys_Set_MainDisplayMode(1U);
    TEST_ASSERT_TRUE(EEPROM_SysSetting_Save());
    memcpy(updated, settings_fake_image, sizeof(updated));

    memcpy(damaged, updated, sizeof(damaged));
    damaged[0xb0U + 8U] ^= 1U;
    settings_fake_reset(damaged);
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_RECOVERED, EEPROM_SysSetting_Get());
    TEST_ASSERT_EQUAL_UINT8(50U, Sys_Get_BacklightLevel());
    TEST_ASSERT_EQUAL_UINT8(0U, Sys_Get_MainDisplayMode());
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_OK, Sys_Get_CalibrationLoadStatus());
    TEST_ASSERT_EQUAL_MEMORY(damaged, settings_fake_image, sizeof(damaged));

    damaged[0x90U + 8U] ^= 1U;
    settings_fake_reset(damaged);
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_CORRUPT, EEPROM_SysSetting_Get());
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_CORRUPT, Sys_Get_SettingsLoadStatus());
    Sys_Set_BacklightLevel(70U);
    TEST_ASSERT_FALSE(EEPROM_SysSetting_Save());
    TEST_ASSERT_EQUAL_UINT(0U, settings_fake_written);
    TEST_ASSERT_EQUAL_MEMORY(damaged, settings_fake_image, sizeof(damaged));
    
}

static void test_generation_wrap_and_conflicts(void)
{
    uint8_t migrated[256];
    uint8_t updated[256];
    uint8_t test_image[256];

    migrate_fixture(migrated);
    settings_fake_reset(migrated);
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_OK, EEPROM_SysSetting_Get());
    Sys_Set_BacklightLevel(70U);
    TEST_ASSERT_TRUE(EEPROM_SysSetting_Save());
    memcpy(updated, settings_fake_image, sizeof(updated));

    memcpy(test_image, updated, sizeof(test_image));
    set_generation(test_image, 0x90U, 0xffffffffU, 32U);
    set_generation(test_image, 0xb0U, 0U, 32U);
    settings_fake_reset(test_image);
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_OK, EEPROM_SysSetting_Get());
    TEST_ASSERT_EQUAL_UINT8(70U, Sys_Get_BacklightLevel());

    memcpy(test_image, updated, sizeof(test_image));
    set_generation(test_image, 0x90U, 0x80000000U, 32U);
    set_generation(test_image, 0xb0U, 0U, 32U);
    settings_fake_reset(test_image);
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_CORRUPT, EEPROM_SysSetting_Get());
    TEST_ASSERT_EQUAL_MEMORY(test_image, settings_fake_image, sizeof(test_image));

    memcpy(test_image, updated, sizeof(test_image));
    set_generation(test_image, 0xb0U, 0U, 32U);
    settings_fake_reset(test_image);
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_CORRUPT, EEPROM_SysSetting_Get());
    TEST_ASSERT_EQUAL_MEMORY(test_image, settings_fake_image, sizeof(test_image));
    
}

static void test_io_errors_reentry_and_primask_restoration(void)
{
    uint8_t legacy[256];
    uint8_t migrated[256];
    uint8_t before[256];

    make_legacy_image(legacy);
    settings_fake_reset(legacy);
    settings_fake_fail_reads = true;
    memcpy(before, settings_fake_image, sizeof(before));
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_IO_ERROR, EEPROM_SysSetting_Get());
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_IO_ERROR, Sys_Get_SettingsLoadStatus());
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_IO_ERROR, Sys_Get_CalibrationLoadStatus());
    TEST_ASSERT_EQUAL_MEMORY(before, settings_fake_image, sizeof(before));
    Sys_Set_BacklightLevel(70U);
    TEST_ASSERT_FALSE(EEPROM_SysSetting_Save());
    TEST_ASSERT_EQUAL_UINT(0U, settings_fake_written);
    TEST_ASSERT_EQUAL_MEMORY(before, settings_fake_image, sizeof(before));
    TEST_ASSERT_EQUAL_UINT32(0U, settings_host_primask);

    migrate_fixture(migrated);
    TEST_ASSERT_EQUAL_UINT32(0U, settings_host_primask);
    settings_fake_reset(migrated);
    settings_host_primask = 1U;
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_OK, EEPROM_SysSetting_Get());
    TEST_ASSERT_EQUAL_UINT32(1U, settings_host_primask);
    TEST_ASSERT_EQUAL_UINT8(50U, Sys_Get_BacklightLevel());
    TEST_ASSERT_EQUAL_UINT32(1U, settings_host_primask);
    Sys_Set_BacklightLevel(70U);
    TEST_ASSERT_EQUAL_UINT32(1U, settings_host_primask);
    settings_fake_inject_reentry = true;
    TEST_ASSERT_TRUE(EEPROM_SysSetting_Save());
    TEST_ASSERT_TRUE(settings_fake_reentry_rejected);
    TEST_ASSERT_EQUAL_UINT32(1U, settings_host_primask);
}

static void test_blank_defaults_and_masked_state(void)
{
    uint8_t blank[256];

    memset(blank, 0xff, sizeof(blank));
    settings_fake_reset(blank);
    settings_host_primask = 1U;
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_DEFAULTS, EEPROM_SysSetting_Get());
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_DEFAULTS, Sys_Get_SettingsLoadStatus());
    TEST_ASSERT_EQUAL_INT(EEPROM_LOAD_DEFAULTS, Sys_Get_CalibrationLoadStatus());
    TEST_ASSERT_EQUAL_UINT32(1U, settings_host_primask);
    TEST_ASSERT_EQUAL_MEMORY(blank + 0x60U, settings_fake_image + 0x60U, 0x30U);
    TEST_ASSERT_EQUAL_MEMORY(blank + 0xd0U, settings_fake_image + 0xd0U, 0x10U);
    
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_legacy_migration_persistence_and_unchanged_writes);
    RUN_TEST(test_calibration_commands_are_read_only_and_crc_valid);
    RUN_TEST(test_calibration_integrity_recovery_and_metadata_damage);
    RUN_TEST(test_settings_and_calibration_domains_are_isolated);
    RUN_TEST(test_unknown_schemas_preserve_records_and_block_saves);
    RUN_TEST(test_settings_corruption_recovers_and_blocks_unsafe_writes);
    RUN_TEST(test_generation_wrap_and_conflicts);
    RUN_TEST(test_io_errors_reentry_and_primask_restoration);
    RUN_TEST(test_blank_defaults_and_masked_state);
    return UNITY_END();
}
