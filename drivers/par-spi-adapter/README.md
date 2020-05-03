# par-spi.library

Driver for the
[amiga-par-to-spi-adapter](https://github.com/niklasekstrom/amiga-par-to-spi-adapter).
This is a parallel-port bus driver — a child of `cia-parallelport` in the
device tree — matched by the compatible string `amiga,par-spi-adapter`.

## What it does

On probe, the driver:

- **Initializes the parallel-port-to-SPI protocol** (the REQ/CLK/ACT
  handshake over the parallel port control lines). The protocol
  implementation is included directly in this file.
- **Registers as an SPI controller** with `spi.library`
  (`SPI_RegisterController`), providing `set_speed`, `select`,
  `deselect`, and `transfer` ops. The adapter supports two speeds:
  slow (~250 kHz, with delays) and fast (~3 MHz, no delays).
- **Registers as a GPIO controller** with `ddm.library`
  (`GPIO_RegisterController`), exposing the card-detect pin (read via
  CIA-A PRB bit-banging). Child `mmc-spi` devices reference this via
  `cd-gpios = <&par_spi 0 0>`.
- **Registers as a cascaded IRQ controller** with `ddm.library`,
  creating a child irq_domain that chains the parent CIA-A FLAG
  interrupt down to child devices.

## Relationship to other components

- Depends on **ddm.library** (core, IRQ, GPIO frameworks),
  **spi.library** (SPI bus), and **parallelport.library** (parallel
  port controller lookup).
- Child of **cia-parallelport** (`cia-parallelport.library`) in the
  device tree; obtains the parent parallel port controller via
  `PP_FindController`.
- Parent of **mmc-spi** (`mmc-spi.device`) in the device tree.

## Files

| File              | Description                                              |
|-------------------|----------------------------------------------------------|
| `par_spi_driver.c`| Driver implementation: SPI protocol, GPIO chip, IRQ chip |

## Build

Built by `build.sh` → `par-spi.library` (placed in `LIBS:`).
