# Power-Pico third-party firmware

PlatformIO project for the Power-Pico's STM32F411xE application and bootloader.

## Build

Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html), then run:

```sh
pio run                              # Application (default)
pio run -e bootloader                 # Bootloader
pio run -e bootloader -e power_pico    # Both
```

Images are built separately under `.pio/build/<environment>/`.

| Image | Flash address | Size limit |
| --- | --- | --- |
| Bootloader | `0x08000000` | 48 KiB |
| Application | `0x08010000` | 448 KiB |

## Upload

Use a DAPLink/CMSIS-DAP probe over SWD; no serial port is needed.

**Before first use, install this project's bootloader.** The original bootloader's application handoff accesses memory beyond SRAM with this application's initial stack pointer.

Back up Flash before replacing the bootloader. Write only `0x08000000–0x0800BFFF`; preserve upgrade metadata at `0x0800C000–0x0800FFFF` and the application from `0x08010000`.

```sh
pio run -e bootloader -t upload    # Only when installing/replacing the bootloader
pio run -t upload                  # Application
```

Application upload preserves the bootloader and updates the upgrade metadata needed for automatic boot. The upload script backs up the metadata sector to `.pio/build/power_pico/upgrade-info-before-upload.bin`, preserves bytes outside the upgrade record, verifies both images, and resets the target. The backup is overwritten on the next application upload.

## Settings and calibration

- First boot migrates legacy EEPROM settings and calibration, leaving the legacy region unchanged.
- User settings use alternating CRC-protected records; ordinary saves do not modify calibration.
- Calibration is read-only: USB GET (`0x10`) remains available; valid SET (`0x13`) and RESET (`0x14`) requests return status `8`.
- If neither calibration record nor validated legacy backup is usable, the firmware warns and uses nominal parameters in RAM. Calibration GET reports unavailable; nominal parameters are never saved as calibration.
- Downgrading to the old firmware restores its view of the frozen legacy settings, not subsequent changes.

## Fonts

Latin text uses Barlow Semi Condensed Medium; Chinese uses Glow Sans SC Normal Regular. Source fonts and SIL OFL licenses are in [`assets/fonts/`](assets/fonts/); generated LVGL fonts are checked in, so normal builds need no font tools.

To regenerate, install Node.js/npm and [uv](https://docs.astral.sh/uv/), then run:

```sh
npm ci --prefix assets/fonts --ignore-scripts
uv run --with-requirements assets/fonts/requirements.txt python assets/fonts/generate_fonts.py
```

When adding Chinese labels, update `ZH_MESSAGES` in [`generate_fonts.py`](assets/fonts/generate_fonts.py) alongside translations. Do not edit generated C files.

## Host checks

With a native GCC toolchain and Python installed, run from the project root; no board is needed:

```sh
pio test -e native_fonts -e native_settings
python tests/check_settings.py
python tests/check_daplink_upload.py
```

These check fonts/translations, settings/calibration, simulated interrupted EEPROM writes, and upload metadata handling. `check_settings.py --image PATH` also checks migration of a temporary copy of a 256-byte EEPROM dump without modifying the input. Simulation does not guarantee physical power-cut behavior.

## Layout

- [`src/app/`](src/app/): application, UI, BSP, USB adapters, and linker script.
- [`src/bootloader/`](src/bootloader/): bootloader, BSP, USB/YMODEM update support, and linker script.
- [`lib/`](lib/): vendored LVGL, FreeRTOS, and STM32 USB device libraries.
- [`scripts/`](scripts/): PlatformIO build/upload hooks.
- [`tests/`](tests/): host checks and test support.
- [`platformio.ini`](platformio.ini): build environments, dependencies, and memory layout.
