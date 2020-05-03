# cia-parallelport.library

Driver for the Amiga 8520 CIA chips as they relate to the parallel
port. This is a platform-bus driver — a direct child of the device
tree root (the Amiga motherboard) — matched by the compatible string
`amiga,cia-parallelport`.

## What it does

- **Claims the CIA hardware.** Opens `ciaa.resource` (for interrupt
  controller operations) and `misc.resource` (to allocate the parallel
  port bits), using the fixed hardware addresses CIA-A at `0xbfe001`
  and CIA-B at `0xbfd000`. These addresses are hard-wired on the Amiga
  and are not read from the device tree.
- **Registers a parallel port controller** with `parallelport.library`
  (`PP_RegisterController`), providing the data/control register access
  ops that child devices (e.g. `par-spi-adapter`) call through.
- **Registers an interrupt controller** on its device node. The
  parallel port ACK pin is wired to CIA-A's FLAG line (ICR bit 4,
  `CIAICRB_FLG`), a negative edge-sensitive interrupt. The driver
  creates an irq_domain with a single hwirq (0) and uses
  `AddICRVector`/`RemICRVector`/`AbleICR` to manage it.
- **Enumerates child devices** described in the device tree (such as
  `par-spi-adapter`), which are then matched by the DDM engine.

## Relationship to other components

- Depends on **ddm.library** (core framework, IRQ framework) and
  **parallelport.library** (parallel port bus subsystem).
- Parent of **par-spi-adapter** (`par-spi.library`) in the device tree.
- The CIA FLAG interrupt it exposes is cascaded down to child devices
  via the par-spi-adapter's own cascaded irq_domain.

## Files

| File                 | Description                                               |
|----------------------|-----------------------------------------------------------|
| `cia_parallelport.c` | Driver implementation: probe, IRQ chip, controller ops    |
| `cia_protos.h`       | LVO stubs for `ciaa.resource` (`AddICRVector`, etc.)      |
| `misc_protos.h`      | LVO stubs for `misc.resource` (`AllocMiscResource`, etc.) |

## Build

Built by `build.sh` → `cia-parallelport.library` (placed in `LIBS:`).
