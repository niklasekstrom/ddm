# parallelport.library

The parallel port subsystem. Provides a bus API for devices on the
Amiga parallel port: controller registration/lookup and (deprecated)
interrupt helper functions. It is a peer of `ddm.library` — a separate
library that opens `ddm.library` at init and caches the base.

## What it does

- **Controller registration.** Controller drivers (e.g.
  `cia-parallelport`) call `PP_RegisterController` with a
  `struct parallel_port_controller` containing an ops table
  (`write_data`/`read_data`, `write_ctrl`/`read_ctrl`, batch
  `write_bytes_2e`/`read_bytes_2e`, etc.). Child devices call through
  these ops to access the hardware, never touching registers directly.
- **Device enumeration.** When a controller registers, the subsystem
  enumerates the controller's device-tree children, sets
  `bus_type = BUS_TYPE_PARALLEL_PORT`, and calls `DDM_MatchDevice`
  inline to bind a driver.
- **Interrupt helpers (deprecated).** `PP_AddInterrupt` /
  `PP_RemoveInterrupt` / `PP_EnableInterrupt` / `PP_DisableInterrupt`
  resolve a device's interrupt via the device tree and forward to the
  IRQ framework. New drivers should use `DT_GetInterrupt` +
  `IRQ_AddHandler` (or `GPIO_ToIrq` + `IRQ_AddHandler`) directly.

Hardware resource allocation (e.g. `misc.resource` for the CIA
parallel port bits) is the responsibility of the specific controller
driver, not this generic subsystem.

## Relationship to other components

- Depends on **ddm.library** (device tree, IRQ framework, matching
  engine).
- Controller drivers (`cia-parallelport`) register with this library.
- Bus drivers (`par-spi-adapter`) find their parent controller via
  `PP_FindController` and call through its ops.

## Files

| File             | Description                                                                                   |
|------------------|-----------------------------------------------------------------------------------------------|
| `parallelport.c` | Subsystem implementation: LVOs, controller registration, interrupt helpers                    |
| `parallelport.h` | Types (`parallel_port_controller`, `parallel_port_ops`, `ParallelPortBase`) + LVO stub macros |

## LVOs

| LVO  | Function               | Description                          |
|------|------------------------|--------------------------------------|
| -30  | PP_AddInterrupt        | Add interrupt handler (deprecated)   |
| -36  | PP_RemoveInterrupt     | Remove interrupt handler (deprecated)|
| -42  | PP_EnableInterrupt     | Enable a device's interrupt          |
| -48  | PP_DisableInterrupt    | Disable a device's interrupt         |
| -54  | PP_RegisterController  | Register a parallel port controller  |
| -60  | PP_UnregisterController| Unregister a controller              |
| -66  | PP_FindController      | Find controller for a device         |

## Build

Built by `build.sh` → `parallelport.library` (placed in `LIBS:`).
