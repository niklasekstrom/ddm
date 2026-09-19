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

### Zorro Expansion Boards

Zorro boards are auto-configured by `expansion.library` at boot, but
their sub-devices (e.g. clockports, SPI controllers) are not
enumerable — they must be described by a device tree overlay. The DDM
handles this in three steps during bootstrap:

1. **Main tree** (`DEVS:a1200.dts`) is parsed, describing on-board
   devices (CIA parallel port, Gayle clockport, etc.).

2. **Zorro enumeration** (`zorro_enumerate`) opens `expansion.library`
   and calls `FindConfigDev` to discover all Zorro boards. Each board
   becomes a `struct device` with `bus_type = BUS_TYPE_ZORRO` and
   properties: `manufacturer-id`, `product-id`, `serial-number`,
   `reg` (absolute base address), `board-size`, `board-type`
   (`"zorro2"` or `"zorro3"`), and `compatible = "zorro"`.

3. **Overlay application** (`DT_ApplyOverlay` on `DEVS:zorro-overlays.dts`)
   parses the overlay file, which contains `fragment@N` nodes. Each
   fragment has a `zorro-match = <manufacturer product>` property and
   child nodes to graft onto the matching board. The `reg` property of
   direct children is treated as an offset from the board base and is
   converted to an absolute address when grafted. Phandle references
   (e.g. `interrupt-parent = <&label>`) work across the overlay and
   the main tree.

```text
root
├── (main tree devices: cia-parallelport, gayle-clockport, ...)
│
└── zorro-bus                         (created by zorro_enumerate)
    └── zorro@828-22                   (Zorro II board, manuf=0x0828, product=0x22)
        │                              reg = <0xE80000>  (board base)
        │
        └── clockport@8000             (grafted from overlay fragment@0)
            │                          reg = <0xE88000>  (base + 0x8000)
            │                          amiga-interrupt = <13>  (INT6)
            │
            └── spider                  (grafted from overlay)
                └── mmc-spi@0           (grafted from overlay)
```

The `amiga-interrupt` property on a clockport node selects which Amiga
system interrupt line the board uses (default `INTB_EXTER` = 13 = INT6
for the on-board Gayle clockport). Zorro boards may route the clockport
interrupt to a different line.

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
- `DEVS:` — mmc-spi.device, ddm.conf, a1200.dts, zorro-overlays.dts
  (`ddm.conf`, `a1200.dts`, and `zorro-overlays.dts` live in `boards/`;
  copy them to `DEVS:`)

## Disclaimer

The code in this repository was written by AI, specifically by the
GitHub Copilot extension in VS Code using the GLM-5.2 open-weight model.
