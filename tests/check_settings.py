"""Compile real storage/ADC code and exercise EEPROM migration and torn writes.

Run: python tests/check_settings.py (requires a native C compiler).
Optional: --image path/to/256-byte-eeprom.bin smoke-migrates a temporary copy.
No device access; the supplied image is never modified.
"""
import argparse
import binascii
import json
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STORAGE = ROOT / "src/app/BSP/EEPROM"
USER = ROOT / "src/app/User"
CALIBRATION = struct.pack("<6f", 1.125, 0.875, 1.0625, 0.25, -2.0, 100.0)

MAIN_H = r"""
#ifndef HOST_MAIN_H
#define HOST_MAIN_H
#include <stdint.h>
#include <stddef.h>
extern uint32_t host_primask;
static inline uint32_t __get_PRIMASK(void) { return host_primask; }
static inline void __disable_irq(void) { host_primask = 1; }
static inline void __enable_irq(void) { host_primask = 0; }
static inline void __set_PRIMASK(uint32_t value) { host_primask = value; }
void HAL_Delay(uint32_t delay);
void NVIC_SystemReset(void);
#endif
"""
I2C_H = r"""
#ifndef HOST_I2C_H
#define HOST_I2C_H
#include "main.h"
typedef struct { unsigned unused; } I2C_HandleTypeDef;
typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
extern I2C_HandleTypeDef hi2c1;
#define I2C_MEMADD_SIZE_8BIT 1
HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *, uint16_t, uint16_t,
                                  uint16_t, uint8_t *, uint16_t, uint32_t);
HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *, uint16_t, uint16_t,
                                   uint16_t, uint8_t *, uint16_t, uint32_t);
HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *, uint16_t, uint32_t, uint32_t);
#endif
"""
CMSIS_H = r"""
#ifndef HOST_CMSIS_H
#define HOST_CMSIS_H
#include "main.h"
typedef void *osThreadId_t;
typedef void *osMessageQueueId_t;
typedef int osStatus_t;
#define osOK 0
#define osErrorResource -1
#define osWaitForever UINT32_MAX
osStatus_t osMessageQueuePut(osMessageQueueId_t, const void *, uint8_t, uint32_t);
uint32_t osThreadFlagsSet(osThreadId_t, uint32_t);
#endif
"""
HARNESS = r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "BL24C02.h"
#include "i2c.h"
#include "gate.h"
#include "user_CmdStrategy.h"
#include "user_TasksInit.h"

uint32_t host_primask;
I2C_HandleTypeDef hi2c1;
static uint8_t image[256];
static const char *image_path;
static long cut_after = -1;
static unsigned written, writes;
static int fail_reads, inject_reentry, reentry_rejected;
static uint8_t gate_mode;

static void persist(void) {
    FILE *file = fopen(image_path, "wb");
    assert(file && fwrite(image, 1, sizeof(image), file) == sizeof(image));
    assert(fclose(file) == 0);
}
HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *handle, uint16_t device,
        uint16_t address, uint16_t mode, uint8_t *data, uint16_t size, uint32_t timeout) {
    (void)handle; (void)device; (void)mode; (void)timeout;
    assert(address + size <= sizeof(image));
    if (fail_reads) return HAL_ERROR;
    memcpy(data, image + address, size);
    return HAL_OK;
}
HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *handle, uint16_t device,
        uint16_t address, uint16_t mode, uint8_t *data, uint16_t size, uint32_t timeout) {
    (void)handle; (void)device; (void)mode; (void)timeout;
    assert(address + size <= sizeof(image));
    assert((address % 16) + size <= 16);
    /* Neither the legacy calibration nor old settings may ever be rewritten. */
    assert(address + size <= 0x40 || address >= 0x60);
    if (inject_reentry) {
        inject_reentry = 0;
        reentry_rejected = !EEPROM_SysSetting_Save();
        assert(reentry_rejected);
    }
    ++writes;
    for (unsigned i = 0; i < size; ++i) {
        if (cut_after >= 0 && written == (unsigned long)cut_after) {
            persist();
            exit(99); /* A new invocation gives the next boot fresh BSS state. */
        }
        image[address + i] = data[i];
        ++written;
    }
    return HAL_OK;
}
HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *handle, uint16_t device,
        uint32_t retries, uint32_t timeout) {
    (void)handle; (void)device; (void)retries; (void)timeout;
    return HAL_OK;
}
void HAL_Delay(uint32_t delay) { (void)delay; }
void LCD_Set_Light(uint8_t value) { (void)value; }
void LCD_SetRotation(uint16_t value) { (void)value; }
void Gate_Set_Mode(uint8_t mode) { gate_mode = mode; }
uint8_t Gate_Get_Mode(void) { return gate_mode; }
uint8_t Gate_get_status(void) { return gate_mode; }
void flow_route_selection(uint8_t mode) { gate_mode = mode; }
uint64_t GetMicrosecondCounter(void) { return 0; }
void USER_USB_DEVICE_DeInit(void) { abort(); }
void NVIC_SystemReset(void) { abort(); }
osThreadId_t MessageSendTaskHandle = (void *)1;
osMessageQueueId_t CmdTxQueue = (void *)1;
static CmdTxFrame_t last_response;
static unsigned response_count;
osStatus_t osMessageQueuePut(osMessageQueueId_t queue, const void *value,
                            uint8_t priority, uint32_t timeout) {
    (void)priority; (void)timeout;
    assert(queue == CmdTxQueue);
    last_response = *(const CmdTxFrame_t *)value;
    ++response_count;
    return osOK;
}
uint32_t osThreadFlagsSet(osThreadId_t thread, uint32_t flags) {
    assert(thread == MessageSendTaskHandle);
    return flags;
}
static uint16_t wire_crc(const uint8_t *data, unsigned size) {
    uint16_t crc = 0xffff;
    for (unsigned i = 0; i < size; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}
static void request(uint8_t opcode, const uint8_t *payload, unsigned size,
                    uint8_t expected_status) {
    uint8_t frame[58] = {0xa5, 0x5a, 1, opcode, 0x34, 0x12, size, 0};
    if (size) memcpy(frame + 8, payload, size);
    uint16_t crc = wire_crc(frame + 2, 6 + size);
    frame[8 + size] = crc;
    frame[9 + size] = crc >> 8;
    unsigned before = response_count;
    CmdStrategy_ProcessRx(frame, size + 10);
    assert(response_count == before + 1);
    assert(last_response.data[3] == (opcode | 0x80));
    assert(last_response.data[4] == 0x34 && last_response.data[5] == 0x12);
    assert(last_response.data[8] == expected_status);
    unsigned length = last_response.length;
    crc = wire_crc(last_response.data + 2, length - 4);
    assert(last_response.data[length - 2] == (uint8_t)crc);
    assert(last_response.data[length - 1] == (uint8_t)(crc >> 8));
}

static const char *status_name(EEPROM_LoadStatus_t value) {
    switch (value) {
        case EEPROM_LOAD_OK: return "OK";
        case EEPROM_LOAD_MIGRATED: return "MIGRATED";
        case EEPROM_LOAD_DEFAULTS: return "DEFAULTS";
        case EEPROM_LOAD_RECOVERED: return "RECOVERED";
        case EEPROM_LOAD_IO_ERROR: return "IO_ERROR";
        case EEPROM_LOAD_CORRUPT: return "CORRUPT";
        case EEPROM_LOAD_UNSUPPORTED: return "UNSUPPORTED";
    }
    abort();
}
int main(int argc, char **argv) {
    assert(argc >= 3);
    image_path = argv[1];
    FILE *file = fopen(image_path, "rb");
    assert(file && fread(image, 1, sizeof(image), file) == sizeof(image));
    assert(fgetc(file) == EOF && fclose(file) == 0);
    const char *action = argv[2];
    long budget = argc > 3 ? strtol(argv[3], NULL, 10) : -1;
    int saving = strcmp(action, "save") == 0 || strcmp(action, "reentry") == 0
                 || strcmp(action, "save-readerror") == 0;
    if (!saving) cut_after = budget;
    fail_reads = strcmp(action, "readerror") == 0 || strcmp(action, "save-readerror") == 0;
    host_primask = strcmp(action, "masked") == 0;
    uint32_t initial_primask = host_primask;
    EEPROM_LoadStatus_t result = EEPROM_SysSetting_Get();
    assert(host_primask == initial_primask);
    int saved = -1;
    if (saving) {
        written = writes = 0;
        cut_after = budget;
        inject_reentry = strcmp(action, "reentry") == 0;
        Sys_Set_BacklightLevel(70);
        Sys_Set_MainDisplayMode(1);
        saved = EEPROM_SysSetting_Save();
    } else if (strcmp(action, "unchanged") == 0) {
        written = writes = 0;
        saved = EEPROM_SysSetting_Save();
    }
    if (strcmp(action, "commands") == 0) {
        uint8_t before[256];
        uint8_t payload[24] = {0xc3, 0x3c};
        memcpy(before, image, sizeof(image));
        written = writes = 0;
        CmdStrategy_Init();
        request(CMD_CAL_SET, payload, 24, 8); /* Read-only, not generic failure. */
        request(CMD_CAL_RESET, payload, 2, 8);
        request(CMD_CAL_SET, payload, 23, 3);
        payload[0] = 0;
        request(CMD_CAL_RESET, payload, 2, 4);
        EEPROM_LoadStatus_t state = Sys_Get_CalibrationLoadStatus();
        int available = state == EEPROM_LOAD_OK || state == EEPROM_LOAD_MIGRATED
                        || state == EEPROM_LOAD_RECOVERED;
        request(CMD_CAL_GET, NULL, 0, available ? 0 : 5);
        assert(written == 0 && memcmp(before, image, sizeof(image)) == 0);
    }
    ADC_Calibration_t calibration;
    Sys_Get_AdcCalibration(&calibration);
    persist();
    printf("{\"result\":\"%s\",\"settings\":\"%s\",\"calibration_status\":\"%s\","
           "\"brightness\":%u,\"language\":%u,\"rotation\":%u,\"range\":%u,\"display\":%u,"
           "\"saved\":%d,\"written\":%u,\"writes\":%u,\"reentry_rejected\":%d,\"calibration\":\"",
           status_name(result), status_name(Sys_Get_SettingsLoadStatus()),
           status_name(Sys_Get_CalibrationLoadStatus()), Sys_Get_BacklightLevel(),
           Sys_Get_LanguageSelect(), Sys_Get_Rotation(), Sys_Get_CurrentRangeMode(),
           Sys_Get_MainDisplayMode(), saved, written, writes, reentry_rejected);
    const uint8_t *bytes = (const uint8_t *)&calibration;
    for (unsigned i = 0; i < sizeof(calibration); ++i) printf("%02x", bytes[i]);
    puts("\"}");
    return 0;
}
"""


def legacy_image():
    data = bytearray(b"\xff" * 256)
    data[:2] = b"\x55\xaa"
    data[0x10:0x15] = b"\x50\x01\x00\x00\x00"
    data[0x20:0x28] = b"-nope-\r\n"
    data[0x40:0x48] = bytes((50, 1, 1, 0, 180, 0, 2, 0))
    data[0x48:0x60] = CALIBRATION
    return data


def record_crc(image, start, size):
    value = binascii.crc_hqx(image[start:start + size - 4], 0xFFFF)
    struct.pack_into("<H", image, start + size - 4, value)


def run_checks(executable, directory, external_image):
    path = directory / "eeprom.bin"

    def boot(image, action="load", cut=-1):
        path.write_bytes(image)
        process = subprocess.run([str(executable), str(path), action, str(cut)],
                                 text=True, capture_output=True)
        assert process.returncode in (0, 99), process.stderr or process.stdout
        result = json.loads(process.stdout) if process.returncode == 0 else None
        return result, bytearray(path.read_bytes())

    original = legacy_image()
    first, migrated = boot(original)
    assert first["settings"] == "MIGRATED" and first["calibration_status"] == "MIGRATED", first
    assert first["brightness"] == 50 and first["rotation"] == 180 and first["range"] == 2
    assert first["calibration"] == CALIBRATION.hex() and first["display"] == 0
    assert migrated[:0x60] == original[:0x60]
    assert migrated[0xE0:] == original[0xE0:]
    stable, reloaded = boot(migrated)
    assert stable["settings"] == "OK" and stable["calibration_status"] == "OK", stable
    assert stable["written"] == 0 and reloaded == migrated
    unchanged, _ = boot(migrated, "unchanged")
    assert unchanged["saved"] == 1 and unchanged["written"] == 0, unchanged
    masked, _ = boot(migrated, "masked")
    assert masked["result"] == "OK"
    commands, preserved = boot(migrated, "commands")
    assert commands["written"] == 0 and preserved == migrated

    saved, updated = boot(migrated, "save")
    assert saved["saved"] == 1 and saved["brightness"] == 70 and saved["display"] == 1, saved
    assert updated[:0x90] == migrated[:0x90] and updated[0xD0:] == migrated[0xD0:]
    after, _ = boot(updated)
    assert after["brightness"] == 70 and after["display"] == 1
    busy, _ = boot(migrated, "reentry")
    assert busy["saved"] == 1 and busy["reentry_rejected"] == 1, busy

    for cut in range(saved["written"]):
        interrupted, torn = boot(migrated, "save", cut)
        assert interrupted is None, (cut, interrupted)
        recovered, _ = boot(torn)
        assert (recovered["brightness"], recovered["display"]) in ((50, 0), (70, 1)), (cut, recovered)
        assert recovered["calibration"] == CALIBRATION.hex()
        assert torn[:0x90] == migrated[:0x90] and torn[0xD0:] == migrated[0xD0:]

    for cut in range(first["written"]):
        interrupted, torn = boot(original, cut=cut)
        assert interrupted is None, (cut, interrupted)
        recovered, repaired = boot(torn)
        assert recovered["calibration"] == CALIBRATION.hex(), (cut, recovered)
        assert recovered["settings"] in ("OK", "MIGRATED"), (cut, recovered)
        assert recovered["brightness"] == 50 and repaired[:0x60] == original[:0x60]

    damaged = updated.copy()
    damaged[0xB0 + 8] ^= 1
    recovered, _ = boot(damaged)
    assert recovered["brightness"] == 50 and recovered["settings"] == "RECOVERED", recovered
    damaged[0x90 + 8] ^= 1
    corrupt, unchanged_image = boot(damaged)
    assert corrupt["settings"] == "CORRUPT" and corrupt["written"] == 0, corrupt
    assert unchanged_image == damaged
    rejected, _ = boot(damaged, "save")
    assert rejected["saved"] == 0 and rejected["written"] == 0, rejected

    damaged = migrated.copy()
    damaged[0x60 + 8] ^= 1
    recovered, preserved = boot(damaged)
    assert recovered["calibration_status"] == "RECOVERED", recovered
    assert recovered["calibration"] == CALIBRATION.hex() and preserved == damaged
    damaged[0x48] ^= 1  # Still numerically plausible, but no longer matches the frozen CRC.
    corrupt, preserved = boot(damaged)
    assert corrupt["calibration_status"] == "CORRUPT" and preserved == damaged, corrupt
    commands, preserved = boot(damaged, "commands")
    assert commands["calibration_status"] == "CORRUPT" and preserved == damaged

    metadata_bad = migrated.copy()
    metadata_bad[0xD0 + 8] ^= 1
    healthy, preserved = boot(metadata_bad)
    assert healthy["calibration"] == CALIBRATION.hex() and preserved == metadata_bad
    metadata_bad[0x60 + 8] ^= 1
    corrupt, preserved = boot(metadata_bad)
    assert corrupt["calibration_status"] == "CORRUPT" and preserved == metadata_bad, corrupt

    for start, size, domain in ((0x90, 32, "settings"), (0x60, 48, "calibration_status")):
        future = migrated.copy()
        future[start + 2] = 99
        record_crc(future, start, size)
        unknown, preserved = boot(future)
        assert unknown[domain] == "UNSUPPORTED" and preserved == future, unknown
        if domain == "settings":
            rejected, _ = boot(future, "save")
            assert rejected["saved"] == 0 and rejected["written"] == 0, rejected

    future = updated.copy()
    future[0xB0 + 2] = 99
    record_crc(future, 0xB0, 32)
    unknown, preserved = boot(future, "save")
    assert unknown["settings"] == "UNSUPPORTED" and unknown["saved"] == 0 and preserved == future, unknown

    wrap = updated.copy()
    struct.pack_into("<I", wrap, 0x90 + 4, 0xFFFFFFFF)
    struct.pack_into("<I", wrap, 0xB0 + 4, 0)
    record_crc(wrap, 0x90, 32)
    record_crc(wrap, 0xB0, 32)
    wrapped, _ = boot(wrap)
    assert wrapped["brightness"] == 70, wrapped
    ambiguous = wrap.copy()
    struct.pack_into("<I", ambiguous, 0x90 + 4, 0x80000000)
    record_crc(ambiguous, 0x90, 32)
    invalid, preserved = boot(ambiguous)
    assert invalid["settings"] == "CORRUPT" and preserved == ambiguous, invalid
    equal = updated.copy()
    equal[0xB0 + 4:0xB0 + 8] = equal[0x90 + 4:0x90 + 8]
    record_crc(equal, 0xB0, 32)
    invalid, preserved = boot(equal)
    assert invalid["settings"] == "CORRUPT" and preserved == equal, invalid

    separate = original.copy()
    separate[0x40] = 255
    independent, _ = boot(separate)
    assert independent["calibration"] == CALIBRATION.hex()
    assert independent["calibration_status"] == "MIGRATED", independent
    unavailable, untouched = boot(original, "readerror")
    assert unavailable["settings"] == "IO_ERROR" and unavailable["calibration_status"] == "IO_ERROR"
    assert untouched == original
    rejected, untouched = boot(updated, "save-readerror")
    assert rejected["saved"] == 0 and rejected["written"] == 0 and untouched == updated, rejected
    blank = bytearray(b"\xff" * 256)
    defaults, default_image = boot(blank)
    assert defaults["settings"] == "DEFAULTS", defaults
    assert defaults["calibration_status"] not in ("OK", "MIGRATED", "RECOVERED"), defaults
    assert default_image[0x60:0x90] == blank[0x60:0x90]
    assert default_image[0xD0:0xE0] == blank[0xD0:0xE0]

    if external_image:
        raw = external_image.read_bytes()
        assert len(raw) == 256, "External EEPROM image must contain exactly 256 bytes"
        smoke, converted = boot(raw)
        assert converted[:0x60] == raw[:0x60]
        assert smoke["calibration"] == raw[0x48:0x60].hex(), smoke
        assert smoke["calibration_status"] in ("OK", "MIGRATED", "RECOVERED"), smoke
        print("PASS: real EEPROM backup migrated in a temporary copy:", json.dumps(smoke))
    print(f"PASS: migration, {first['written']} initial-migration and {saved['written']} save byte-cut "
          "boundaries, calibration integrity/recovery, domain isolation, unknown schemas, "
          "sequence wrap/conflicts, I/O errors, unchanged writes, reentry and PRIMASK")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="power-pico-settings-") as temporary:
        directory = Path(temporary)
        (directory / "main.h").write_text(MAIN_H)
        (directory / "i2c.h").write_text(I2C_H)
        (directory / "adc.h").write_text("")
        (directory / "FreeRTOS.h").write_text("")
        (directory / "cmsis_os.h").write_text(CMSIS_H)
        (directory / "usb_device.h").write_text("void USER_USB_DEVICE_DeInit(void);\n")
        (directory / "lcd_init.h").write_text(
            '#include "main.h"\nvoid LCD_Set_Light(uint8_t);\nvoid LCD_SetRotation(uint16_t);\n')
        (directory / "tim.h").write_text('#include "main.h"\nuint64_t GetMicrosecondCounter(void);\n')
        (directory / "check.c").write_text(HARNESS)
        executable = directory / "check_settings"
        subprocess.run([
            "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-ffunction-sections", "-fdata-sections", "-I", str(directory),
            "-I", str(STORAGE), "-I", str(USER), "-I", str(ROOT / "src/app/BSP/GATE"),
            "-I", str(USER / "Tasks/Inc"),
            str(directory / "check.c"), str(STORAGE / "BL24C02.c"),
            str(USER / "user_AdcDataStrategy.c"), str(USER / "user_CmdStrategy.c"),
            "-Wl,--gc-sections", "-lm", "-o", str(executable),
        ], check=True)
        run_checks(executable, directory, args.image)


if __name__ == "__main__":
    main()
