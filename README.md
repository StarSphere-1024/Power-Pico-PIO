# Power-Pico third-party firmware

Independent PlatformIO project for third-party firmware development. Existing firmware has not been migrated.

## Target

The upstream firmware targets STM32F411xE. This project uses PlatformIO's `ststm32` platform, `genericSTM32F411CE` board definition, and STM32Cube framework.

## Build

Install PlatformIO Core, then run from this directory:

```sh
pio run
```

Firmware sources belong in `src/`; project-specific PlatformIO settings belong in `platformio.ini`. Migration from the original firmware is intentionally deferred.
