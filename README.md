# Power-Pico third-party firmware

PlatformIO project for the STM32F411xE bootloader and application firmware. The original repository remains unchanged.

## Build

Install PlatformIO Core, then run from this directory:

```sh
pio run
```

This builds both environments and produces separate images under `.pio/build/bootloader/` and `.pio/build/power_pico/`. Build one environment with `pio run -e bootloader` or `pio run -e power_pico`.

The bootloader is linked at `0x08000000` with a 48 KiB limit. The application is linked at `0x08010000` with a 448 KiB limit. The images are not combined; flash them using the existing update/flash workflow.

## Layout

- `src/bootloader/`: bootloader, BSP, Ymodem, USB device, and linker script.
- `src/app/`: application, BSP, LVGL, FreeRTOS, USB device, and linker script.
- `platformio.ini`: shared target settings and per-firmware source filters, memory limits, and load addresses.
- `scripts/fpu-link.py`: STM32F411 Cortex-M4F hard-float linker setup.

Both environments have been build-verified. Hardware flashing and runtime behavior remain unverified.
