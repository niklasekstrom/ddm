# Device Driver Model (DDM)

## DDM Architecture

This project implements a Linux-inspired device driver model for the Amiga
computer. The core is `ddm.library`, which provides `struct device`,
`struct device_driver`, compatible-string matching, and bus enumeration —
mirroring Linux's `drivers/base`. It also provides the interrupt framework
(`IRQ_*` functions, inspired by Linux's `irq_chip` / `irq_domain`), the device
tree parser (`DT_*` functions), and the GPIO framework (`GPIO_*` functions,
inspired by Linux's gpiolib). Other subsystems (spi, mmc, parallelport,
clockport) build on top of the core.

### Device Tree Hierarchy

```text
root
├── cia-parallelport    CIA-A/CIA-B (parallel port + FLAG interrupt)
│   └── par-spi-adapter  Parallel-to-SPI adapter
│       ├── [GPIO controller]  Card-detect pin (bit-banged via CIA-A PRB)
│       ├── [IRQ controller]   Cascades CIA-A /FLAG to child devices
│       └── spi-bus      Logical SPI bus
│           └── mmc-spi@0   SD/MMC card (chip select 0, unit 0)
│
└── gayle-clockport     Gayle clockport memory region + system interrupt
    └── spider         Individual Computers SPIder (clockport SPI adapter)
        ├── [GPIO controller]  Card-detect pin
        ├── [IRQ controller]   Cascades the clockport interrupt
        └── spi-bus      Logical SPI bus
            └── mmc-spi@0   SD/MMC card (chip select 0, unit 1)
```

The par-spi-adapter and spider nodes each act as both a GPIO controller
(providing the card-detect pin via `cd-gpios`) and an interrupt controller
(cascading their parent interrupt to child devices). Each mmc-spi device
references its card-detect pin via `cd-gpios = <&par_spi 0 0>` /
`<&spider 0 0>`. The card-change interrupt is derived from that GPIO at
runtime via `GPIO_ToIrq` rather than via an `interrupt-parent` property.

### How It Works

1. **ddm.library** provides the core framework: `struct device`,
   `struct device_driver`, compatible-string matching, and bus enumeration.
   This is the heart of the DDM, inspired by Linux's
   `drivers/base`. It also provides the interrupt controller abstraction
   (`struct irq_chip` / `struct irq_domain` / `struct irq_desc`) and
   handler management (`IRQ_AddHandler`, etc.), inspired by Linux's
   `irq_chip` / `irq_domain`.

2. **Device tree parser** (part of ddm.library) parses a DTS text file
   into `struct device` + `struct device_node` entries (populating the
   DDM) and provides property access (`DT_GetProperty`, `DT_GetInterrupt`).

3. **Config file** (`boards/ddm.conf`) lists the libraries and
   devices to load. Each self-registers its driver(s) on init via
   `DDM_RegisterDriver`. After all are loaded, `DDM_MatchAll` binds drivers
   to devices.

4. **Subsystem libraries** (spi, mmc, parallelport, clockport) provide
   bus-specific frameworks. Controller drivers register with them;
   device drivers use their APIs. (The GPIO framework lives in core
   `ddm.library`, not a separate subsystem.)

5. **Bus drivers** (cia-parallelport, par-spi-adapter, gayle-clockport,
   spider) enumerate child devices and register them with the tree. The
   par-spi-adapter and spider drivers also register as GPIO controllers
   (for the card-detect pin) and interrupt controllers (cascading their
   parent interrupt).

6. **mmc-spi.device** is both a DDM driver (matched by
   `compatible = "mmc-spi"`) and an AmigaOS trackdisk.device, exporting
   the standard BeginIO/AbortIO interface for filesystem mounting. It
   resolves the card-detect GPIO and card-change interrupt from the
   device tree.

### LVO Layout

The DDM driver interface uses LVOs starting at -42, after the
standard device LVOs (Open -6, Close -12, Expunge -18, Reserved -24,
BeginIO -30, AbortIO -36):

| LVO  | Function             |
|------|----------------------|
| -42  | DDMDrv_Probe         |
| -48  | DDMDrv_Remove        |
| -54  | DDMDrv_Init          |
| -60  | DDMDrv_Shutdown      |
| -66  | DDMDrv_Enumerate     |
| -72  | DDMDrv_GetCompatible |

`DDMDrv_Init` (-54) is a reserved slot — initialization was merged into
`DDMDrv_Probe`, so the matching engine no longer calls it.

### Building

Run `build.sh` at the project root. This builds all components in
dependency order using [amiga-gcc](https://codeberg.org/bebbo/amiga-gcc).
Copy the output files to your Amiga:

- `LIBS:` — ddm.library, spi.library, mmc.library, parallelport.library,
  clockport.library, cia-parallelport.library, par-spi.library,
  gayle-clockport.library, spider.library
- `DEVS:` — mmc-spi.device, ddm.conf, a1200.dts
  (`ddm.conf` and `a1200.dts` live in `boards/`; copy them to `DEVS:`)

## Disclaimer

The code in this repository was written by AI, specifically by the
GitHub Copilot extension in VS Code using the GLM-5.2 open-weight model.
