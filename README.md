# Power-Pico third-party firmware

PlatformIO project for the STM32F411xE bootloader and application firmware. The original repository remains unchanged.

## Build

Install PlatformIO Core, then run from this directory:

```sh
pio run
```

This builds both environments and produces separate images under `.pio/build/bootloader/` and `.pio/build/power_pico/`. Build one environment with `pio run -e bootloader` or `pio run -e power_pico`.

The bootloader is linked at `0x08000000` with a 48 KiB limit. The application is linked at `0x08010000` with a 448 KiB limit. The images are not combined; flash them using the existing update/flash workflow.

The application links newlib-nano's `_printf_float` with `-Wl,-u,_printf_float` so `sprintf`/`snprintf` can render floating-point readings on the main, PD, and PPS pages. Without it, `%f` produces blank numeric text. Main-page voltage/current/power display after cold start was confirmed by the user; USB sampling and CRC-checked command responses were also verified.

## Layout

- `src/bootloader/`: bootloader, BSP, Ymodem, USB device, and linker script.
- `src/app/`: application, BSP, LVGL, FreeRTOS, USB device, and linker script.
- `platformio.ini`: shared target settings and per-firmware source filters, memory limits, and load addresses.
- `scripts/fpu-link.py`: STM32F411 Cortex-M4F hard-float linker setup.

## Bootloader compatibility

Install this project's bootloader via SWD before running the PIO application with its initial stack at `0x20020000`. The original bootloader switches MSP and then executes a C function epilogue that reads beyond SRAM. The fixed `jump_to_app()` switches MSP and immediately branches to the application reset handler without further stack access.

When replacing the bootloader, back up Flash and write only `0x08000000`–`0x0800BFFF`; preserve the upgrade metadata at `0x0800C000`–`0x0800FFFF` and the application from `0x08010000`. USB/YMODEM application updates also populate the metadata required for automatic boot.

Verified on hardware with the fixed bootloader and unchanged PIO application: automatic boot, Bootloader menu execution, USB sampling, and CRC-checked command responses. Cold-start display operation was confirmed by the user. Full runtime behavior has not been exhaustively tested.
