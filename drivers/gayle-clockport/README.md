# gayle-clockport.library

Driver for the Amiga clockport as exposed via the Gayle chip. This is
a platform-bus driver — a direct child of the device tree root (the
Amiga motherboard) — matched by the compatible string
`amiga,gayle-clockport`.

## What it does

- **Claims the clockport memory region.** The clockport is a
  memory-mapped region with 16 registers at 4-byte stride. The base
  address is read from the device tree `reg` property (default
  `0xD80001`).
- **Registers a clockport controller** with `clockport.library`
  (`CP_RegisterController`), providing register-access ops
  (`read_reg`/`write_reg`) and fast-path batch transfer ops
  (`read_reg_bytes`/`write_reg_bytes`) that child devices (e.g.
  `spider`) call through.
- **Registers an interrupt controller** on its device node. The
  clockport interrupt is an Amiga system interrupt (not a CIA
  interrupt), so the driver uses `AddIntServer`/`RemIntServer` rather
  than `AddICRVector`. It creates an irq_domain with a single hwirq
  (0).
- **Enumerates child devices** described in the device tree (such as
  `spider`), which are then matched by the DDM engine.

## Relationship to other components

- Depends on **ddm.library** (core framework, IRQ framework) and
  **clockport.library** (clockport bus subsystem).
- Parent of **spider** (`spider.library`) in the device tree.
- The clockport system interrupt it exposes is cascaded down to child
  devices via the spider driver's own cascaded irq_domain.

## Files

| File                 | Description                                             |
|----------------------|---------------------------------------------------------|
| `gayle_clockport.c`  | Driver implementation: probe, register access, IRQ chip |

## Build

Built by `build.sh` → `gayle-clockport.library` (placed in `LIBS:`).
