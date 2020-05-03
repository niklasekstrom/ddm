# spi.library

The SPI bus subsystem. Provides a bus framework for SPI controller
drivers and SPI device drivers, inspired by Linux's `spi subsystem`.
It is a peer of `ddm.library` — a separate library that opens
`ddm.library` at init and caches the base to call DDM LVOs.

## What it does

- **Controller registration.** Controller drivers (e.g.
  `par-spi-adapter`, `spider`) call `SPI_RegisterController` with a
  `struct spi_controller` containing function pointers
  (`set_speed`, `select`, `deselect`, `transfer`).
- **Device enumeration.** When a controller registers, the subsystem
  enumerates the controller's device-tree children, creates a
  `struct spi_device` for each (reading `reg` for chip select and
  `spi-max-frequency` for max speed), sets `bus_type = BUS_TYPE_SPI`,
  and calls `DDM_MatchDevice` inline to bind a driver.
- **Device lookup.** SPI device drivers (e.g. `mmc-spi.device`) find
  their `spi_device` via `SPI_FindDevice`.

## Relationship to other components

- Depends on **ddm.library** (device tree, matching engine).
- Controller drivers (`par-spi-adapter`, `spider`) register with this
  library.
- Device drivers (`mmc-spi.device`) use this library to find their SPI
  device and call the controller's transfer ops.

## Files

| File    | Description                                                                 |
|---------|-----------------------------------------------------------------------------|
| `spi.c` | Subsystem implementation: LVOs, controller registration, device enumeration |
| `spi.h` | Types (`spi_controller`, `spi_device`, `SpiBase`) + LVO stub macros         |

## LVOs

| LVO  | Function                 | Description                                     |
|------|--------------------------|-------------------------------------------------|
| -30  | SPI_RegisterController   | Register an SPI controller + enumerate children |
| -36  | SPI_UnregisterController | Unregister a controller + free devices          |
| -42  | SPI_FindDevice           | Find the spi_device for a device tree node      |

## Build

Built by `build.sh` → `spi.library` (placed in `LIBS:`).
