# clockport.library

The clockport subsystem. Provides a bus API for devices on the Amiga
clockport: controller registration and lookup. It is a peer of
`ddm.library` — a separate library that opens `ddm.library` at init
and caches the base.

## What it does

- **Controller registration.** Controller drivers (e.g.
  `gayle-clockport`) call `CP_RegisterController` with a
  `struct clockport_controller` containing an ops table
  (`read_reg`/`write_reg` for single-register access,
  `read_reg_bytes`/`write_reg_bytes` for fast-path batch transfers).
  Child devices call through these ops to access the clockport
  registers, never touching the memory-mapped base address directly.
- **Device enumeration.** When a controller registers, the subsystem
  enumerates the controller's device-tree children, sets
  `bus_type = BUS_TYPE_CLOCKPORT`, and calls `DDM_MatchDevice` inline
  to bind a driver.

Hardware resource allocation (the clockport memory region) is the
responsibility of the specific controller driver, not this generic
subsystem.

## Relationship to other components

- Depends on **ddm.library** (device tree, matching engine).
- Controller drivers (`gayle-clockport`) register with this library.
- Bus drivers (`spider`) find their parent controller via
  `CP_FindController` and call through its ops.

## Files

| File           | Description                                                                        |
|----------------|------------------------------------------------------------------------------------|
| `clockport.c`  | Subsystem implementation: LVOs, controller registration                            |
| `clockport.h`  | Types (`clockport_controller`, `clockport_ops`, `ClockportBase`) + LVO stub macros |

## LVOs

| LVO  | Function                | Description                          |
|------|-------------------------|--------------------------------------|
| -30  | CP_RegisterController   | Register a clockport controller      |
| -36  | CP_UnregisterController | Unregister a controller              |
| -42  | CP_FindController       | Find controller for a device         |

## Build

Built by `build.sh` → `clockport.library` (placed in `LIBS:`).
