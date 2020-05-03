# spider.library

Driver for the [SPIder](https://github.com/niklasekstrom/spider)
clockport-to-SPI adapter. This is a clockport-bus driver — a child of
`gayle-clockport` in the device tree — matched by the compatible string
`amiga,spider`.

## What it does

On probe, the driver:

- **Probes the SPIder firmware** by reading the IDENT register and
  checking for the expected identification string. If the hardware is
  absent, the probe returns `-1` and the branch stays unmatched.
- **Registers as an SPI controller** with `spi.library`
  (`SPI_RegisterController`), providing `set_speed`, `select`,
  `deselect`, and `transfer` ops. The SPIder FIFO protocol
  implementation (register-based, via the parent clockport controller)
  is included directly in this file.
- **Registers as a GPIO controller** with `ddm.library`
  (`GPIO_RegisterController`), exposing the card-detect pin (read via
  the SPIder `REG_CARD_DETECT` register). Child `mmc-spi` devices
  reference this via `cd-gpios = <&spider 0 0>`.
- **Registers as a cascaded IRQ controller** with `ddm.library`,
  creating a child irq_domain that chains the parent clockport
  interrupt down to child devices.

## Relationship to other components

- Depends on **ddm.library** (core, IRQ, GPIO frameworks),
  **spi.library** (SPI bus), and **clockport.library** (clockport
  controller lookup).
- Child of **gayle-clockport** (`gayle-clockport.library`) in the
  device tree; obtains the parent clockport controller via
  `CP_FindController`.
- Parent of **mmc-spi** (`mmc-spi.device`) in the device tree.

## Files

| File              | Description                                                      |
|-------------------|------------------------------------------------------------------|
| `spider_driver.c` | Driver implementation: SPIder FIFO protocol, GPIO chip, IRQ chip |

## Build

Built by `build.sh` → `spider.library` (placed in `LIBS:`).
