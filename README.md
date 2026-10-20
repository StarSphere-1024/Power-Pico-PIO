# Power-Pico third-party firmware

PlatformIO project for the STM32F411xE bootloader and application firmware. The original repository remains unchanged.

## Build

Install PlatformIO Core, then run from this directory:

```sh
pio run
```

This builds the application by default. Build the bootloader explicitly with `pio run -e bootloader`, or build both with `pio run -e bootloader -e power_pico`. Images are produced under `.pio/build/bootloader/` and `.pio/build/power_pico/`.

The bootloader is linked at `0x08000000` with a 48 KiB limit. The application is linked at `0x08010000` with a 448 KiB limit. The images are not combined.

The application links newlib-nano's `_printf_float` with `-Wl,-u,_printf_float` so `sprintf`/`snprintf` can render floating-point readings on the main, PD, and PPS pages. Without it, `%f` produces blank numeric text. Main-page voltage/current/power display after cold start was confirmed by the user; USB sampling and CRC-checked command responses were also verified.

The display monitor averages 100 ms of samples, and the main-page numeric labels update every 100 ms (nominal 10 Hz). ADC acquisition remains at 10,000 five-channel scans per second; USB packet format and PD/PPS page timers are unchanged. The application build and a host smoke run of `Process_ADC_Chunk()` verified average updates at 100 ms boundaries; physical display refresh has not been verified on hardware for this change.

`defaultTask` has a 512-byte CMSIS thread stack. Its former 128-byte allocation overwrote the FreeRTOS stack guard during a delay/context switch and entered `vApplicationStackOverflowHook()`, stopping scheduling and making the buttons unresponsive. The stack comes from the existing 15 KiB FreeRTOS heap; increasing it consumes 384 more heap bytes without increasing the linker-reported RAM total. After building and flashing the fix, SWD breakpoints confirmed key scanning and LVGL timer handling; RTOS time advanced another 65 seconds to approximately 107 seconds uptime without an overflow, with 412 untouched stack bytes and 768 bytes minimum-ever free heap in that run. The debugger was disconnected with the target running. Physical button/UI response still requires manual confirmation.

## Fonts

The application uses **Barlow Semi Condensed Medium 1.408** for Latin text, numbers, and units, and **Glow Sans SC Normal Regular 0.93** for Chinese. Barlow's `tnum` alternate digit outlines are baked into the exported character map, with kerning disabled; LVGL needs no runtime OpenType shaping. The zero has no dot or slash. Main-page numeric fields are right-aligned so equal-width digits do not move when readings change; their unit labels use a separate 36px font for improved readability. Visible micro-units use U+00B5 (`µA`, `µW`), without changing internal measurement units or USB data.

Unmodified source fonts and their SIL OFL 1.1 licenses are in `assets/fonts/`; generated 4-bpp, uncompressed LVGL fonts are checked into `src/app/User/GUI/font/`. Source hashes and provenance are recorded in `assets/fonts/generate_fonts.py`. Chinese fonts contain only the UI's required glyphs and use matching Barlow fonts for Latin fallback. The chart's play indicator uses Glow Sans's Unicode triangle instead of a private-use icon-font glyph.

Normal firmware builds do not require font-generation tools. To regenerate from the vendored sources, install Node.js/npm and [uv](https://docs.astral.sh/uv/), then run:

```sh
npm ci --prefix assets/fonts --ignore-scripts
uv run --with-requirements assets/fonts/requirements.txt python assets/fonts/generate_fonts.py
```

The converter and fontTools versions are pinned. When adding Chinese labels, update the generator's per-size `ZH_MESSAGES` mapping alongside the translations; do not edit generated C files. Regeneration was verified byte-for-byte reproducible.

Run `python tests/check_ui_fonts.py` with CMake and a native C compiler to check exported digit spacing, the unmarked zero, micro-unit glyphs, Chinese translations, and Latin fallback through LVGL's actual font API. The application build and native LVGL rendering of startup, main, chart, settings/popups, PD, and both PPS panels were verified in English and Chinese, including main-page unit boundaries, negative readings, and `255:59:59` timer fit. The firmware has not been flashed or visually checked on the physical LCD for this font change. Ah accumulation is not part of this change.

## Persistent settings and immutable calibration

The application no longer stores a raw C structure. User settings and calibration
have independent versioned records in the 256-byte BL24C02:

| Address | Purpose |
| --- | --- |
| `0x00–0x01` | Existing EEPROM marker, unchanged format |
| `0x10–0x14` | Reserved for the bootloader's old settings |
| `0x20–0x27` | Existing bootloader update command, unchanged format |
| `0x40–0x5F` | Frozen legacy application settings; never rewritten |
| `0x60–0x8F` | One immutable calibration record, 48 bytes |
| `0x90–0xAF`, `0xB0–0xCF` | User settings A/B records, 32 bytes each |
| `0xD0–0xDF` | Independent integrity record for legacy calibration |
| `0xE0–0xFF` | Unused |

All records use an 8-byte header: two-byte magic (`CA`, `US`, or `CM`), schema
version, payload length, and a little-endian 32-bit generation. Payload starts at
offset 8; unused payload bytes are zero. CRC16-CCITT (polynomial `0x1021`, initial
value `0xFFFF`, no final XOR) occupies the fourth- and third-last bytes and covers
every preceding byte, including the header and padding. The final two bytes are
the `A5 5A` commit marker. Integer and IEEE-754 binary32 fields use explicit
little-endian encoding, independent of C structure layout.

Schema 1 user payload is seven bytes: brightness, key sound, language, rotation
(`u16`), current range, and main display mode (`0=power`, `1=Ah`). Each user record
has 20 bytes of payload capacity, leaving 13 bytes for later fields. The display
mode is persisted through its getter/setter API only; there is no Ah selector or
accumulation implementation in this storage change. Future schemas require an
explicit decoder/migration; incrementing a version does not make an old firmware
understand new fields.

First boot imports legacy user fields independently of calibration validity.
Calibration comes from the six binary32 values at `0x48–0x5F`, ordered low/mid/high
multipliers followed by low/mid/high offsets. After semantic validation, migration
first commits a `CM` record containing the legacy bytes' CRC16 and their length
(`u16`, 24), then commits the `CA` record. Interrupted initial writes are resumed
only when their bytes match a prefix of the expected migration record. The old
region stays untouched. Adding a CRC establishes integrity from migration onward;
it cannot prove that the legacy values were historically correct.

Ordinary saves write only the inactive user slot: invalidate and verify its
marker, write and read back the body, then write and verify the commit marker.
The previous valid slot is retained; identical settings cause no EEPROM write.
Generation ordering handles wraparound and rejects ambiguous/conflicting
records. A short interrupt-state-preserving transaction guard rejects concurrent
storage operations without holding interrupts disabled during I2C transfers.

An invalid calibration primary may use the legacy backup only when both its
independent integrity record and semantic validation pass. An established
calibration primary is not automatically repaired, and mismatching legacy data
is never given a new CRC baseline. If neither source is usable, the application
uses nominal conversion parameters in RAM, shows a persistent warning, and
rejects calibration GET as unavailable; it does not persist nominal calibration.
Calibration SET (`0x13`) and RESET (`0x14`) return read-only status `8` after
request-length/password validation. GET remains `0x10`.

Unknown user schemas block saving. Corrupt established user records do not
silently reimport stale legacy settings; I/O failures also block saving when the
existing state could not be loaded. Startup warnings identify unavailable data
or recovery from legacy calibration and can be dismissed with Y/N. Failed user
saves show a transient, coalesced error banner. Firmware update proceeds only
after its EEPROM command has been written and verified. Downgrading to the old
raw-structure firmware sees only the frozen legacy settings, not later changes.

Run `python tests/check_settings.py` with a native C compiler. It executes the
real storage, calibration validation, and command parser against a
256-byte EEPROM model, using a fresh process for each simulated boot. Checks
cover every written-byte interruption point during initial migration (102) and
a user save (34), read-only calibration commands, backup integrity, independent
domains, unknown schemas, generation wrap/conflicts, I/O failure, reentry, and
preservation of interrupt state. `--image PATH` additionally smoke-migrates a
temporary copy of a 256-byte EEPROM dump; the input file is never modified.
The saved device EEPROM dump was exercised this way. Byte-cut simulation is not
a physical EEPROM power-cut guarantee; hardware corruption patterns can instead
lead to a visible failure requiring external recovery.

Verified on the device through DAPLink: application and metadata flash verification
passed; readback preserved the bootloader and metadata bytes outside the upgrade
record. Physical EEPROM readback confirmed first-boot migration, record CRCs,
unchanged legacy bytes, and exact calibration preservation. USB calibration GET
returned the original coefficients; SET/RESET returned read-only status `8`.
Debugger-injected events at the natural key-scan return exercised the actual
queue/UI save path: brightness `50 → 40` survived reset, then `40 → 50` restored
the original setting in the alternate slot. Calibration and its backup CRC did
not change. A later stable boot performed no EEPROM writes; 600 valid ADC packets
were captured over USB. Final load statuses were OK, fault registers and stack
overflow state were clear, and the debugger detached with the target running.

During inspection, a debug reset interrupted startup I2C activity, leaving SDA
low and the bus BUSY. Both configuration domains correctly reported I/O failure
and calibration GET returned an error. A debugger-driven open-drain bus-clear
sequence and reset restored normal loading without changing EEPROM data.
Firmware bus recovery was not added. Evidence and before/after images are under
`.debug/settings-hardware-20261020T090234Z/`. Physical LCD/button response,
external power cycling, and physical power-cut behavior remain unverified.

## Upload

DAPLink/CMSIS-DAP over SWD is the default upload interface. Upload only the application with:

```sh
pio run -t upload
```

`scripts/daplink-upload.py` registers CMSIS-DAP support for the generic board and, for application uploads, backs up the full metadata sector to `.pio/build/power_pico/upgrade-info-before-upload.bin`. It calculates the bootloader-compatible STM32 CRC32, updates the 16-byte upgrade record, and preserves the rest of that sector. OpenOCD programs and verifies the application and metadata, then resets the target. The bootloader is not rewritten. The backup is replaced on the next upload.

Upload the bootloader only when intentionally replacing it: `pio run -e bootloader -t upload`. This does not update the application or its metadata. No serial upload port is needed for CMSIS-DAP.

Verified using `pio run -t upload` with DAPLink: both flash verifications passed, reset entered the application, and metadata readback preserved all bytes outside the upgrade record. Run `python tests/check_daplink_upload.py` to check CRC word order, partial-word handling, and metadata preservation without hardware.


## Layout

- `src/bootloader/`: bootloader, BSP, Ymodem, USB device adapters, and linker script.
- `src/app/`: application, BSP, USB device adapters, linker script, and `Core/Inc/FreeRTOSConfig.h`.
- `lib/LVGL/`: vendored LVGL 9.5.0, `lv_conf.h`, and LCD display port.
- `lib/FreeRTOS/`: vendored FreeRTOS 10.3.1 with the CMSIS-RTOS V2 wrapper.
- `lib/STM32_USB_Device_Library/`: shared vendored USB device core and CDC class, including the original license. The identical application and bootloader copies were consolidated without source changes.
- `platformio.ini`: shared target settings and USB library dependency, application-only LVGL/FreeRTOS dependencies, per-firmware source filters, memory limits, and load addresses.
- `scripts/fpu-link.py`: STM32F411 Cortex-M4F hard-float linker setup.

Each private library's `library.json` owns its source filters and include paths. LVGL excludes `src/drivers/`; FreeRTOS selects only the GCC Cortex-M4F port and `heap_4.c`; the USB library selects only `Core/Src` and `Class/CDC/Src`. All three retain direct object linking (`libArchive: false`) as before extraction. Library versions and configuration are unchanged; the bootloader does not depend on LVGL or FreeRTOS.

Each firmware keeps its own `USB_DEVICE/App` and `USB_DEVICE/Target`, including descriptors, CDC callbacks, `usbd_conf.h`, and HAL adaptation. The shared USB library is compiled separately for each environment using that firmware's include paths.

The library extractions were verified by building both environments and checking USB core/CDC and adapter symbols, USB interrupt vectors, and application RTOS interrupt vectors. The application has since been reflashed through DAPLink and confirmed to enter application code after reset; USB sampling and command responses have not been rechecked after extraction.

## Bootloader compatibility

Install this project's bootloader via SWD before running the PIO application with its initial stack at `0x20020000`. The original bootloader switches MSP and then executes a C function epilogue that reads beyond SRAM. The fixed `jump_to_app()` switches MSP and immediately branches to the application reset handler without further stack access.

When replacing the bootloader, back up Flash and write only `0x08000000`–`0x0800BFFF`; preserve the upgrade metadata at `0x0800C000`–`0x0800FFFF` and the application from `0x08010000`. USB/YMODEM application updates also populate the metadata required for automatic boot.

Verified on hardware with the fixed bootloader and unchanged PIO application: automatic boot, Bootloader menu execution, USB sampling, and CRC-checked command responses. Cold-start display operation was confirmed by the user. Full runtime behavior has not been exhaustively tested.
