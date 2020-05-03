# mmc-spi.device

SD card driver over SPI for the DDM (Device Driver Model) framework on
AmigaOS (m68k).

## Overview

`mmc-spi.device` is a trackdisk.device-compatible block driver that talks
to SD cards (SD v2.0+: SDSC and SDHC/SDXC) over the DDM SPI subsystem. It:

1. Registers as a DDM driver (compatible `"mmc-spi"`, bus type `BUS_TYPE_SPI`).
2. On probe, finds its SPI device via `spi.library`, resolves the
   card-detect GPIO from the device tree, sets up a card-change interrupt
   (`GPIO_ToIrq` / `IRQ_AddHandler`), creates a background task, and
   registers the card with `mmc.library`. The background task opens
   `timer.device` on demand for card-detect debounce.
3. Exports a full `trackdisk.device` interface (`BeginIO`/`AbortIO`) so
   AmigaOS file systems and tools can use the SD card as a block device.

The driver is **SD v2.0+ only**. It requires CMD8 (SEND_IF_COND) to
succeed; SD v1.x and MMC cards are not supported. Both SDSC
(byte-addressed, CSD v1.0) and SDHC/SDXC (block-addressed, CSD v2.0)
are supported, selected by the OCR CCS bit.

## DDM integration

```
                 ┌─────────────┐
   device tree   │  a1200.dts  │  mmc-spi@0 { compatible="mmc-spi";
                 └──────┬──────┘    device-unit; reg; spi-max-frequency;
                        │           cd-gpios }
                        ▼
  ┌──────────────────────────────────────────────┐
  │  DDM matching engine (ddm.library)           │
  │  matches "mmc-spi" → mmc-spi.device driver   │
  └──────────────┬───────────────────────────────┘
                 │ DDMDrv_Probe
                 ▼
  ┌───────────────────────────────────────────────┐
  │  mmc-spi.device                               │
  │  ├── SPI_FindDevice  → spi_device (CS, speed) │
  │  ├── GPIO_Get("cd")  → card-detect pin        │
  │  ├── GPIO_ToIrq      → card-change virq       │
  │  ├── sd_open/sd_read/sd_write over SPI        │
  │  ├── MMC_RegisterCard → mmc.library registry  │
  │  └── trackdisk BeginIO → background task      │
  └───────────────────────────────────────────────┘
```

### Device tree properties

| Property            | Description                                   |
|---------------------|-----------------------------------------------|
| `compatible`        | Must be `"mmc-spi"`                           |
| `device-unit`       | AmigaOS open() unit number                    |
| `reg`               | SPI chip-select number                        |
| `spi-max-frequency` | Max bus speed in Hz (clamped at runtime)      |
| `cd-gpios`          | Card-detect GPIO (active-high = card present) |

The `mmc-spi@0` nodes already exist in `boards/a1200.dts` under both
`par-spi-adapter` and `spider` SPI controllers.

## SD v2.0+ init sequence

```
CMD0  (GO_IDLE)      → R1 = 0x01 (idle)
CMD8  (SEND_IF_COND)  → R1 = 0x01, R7 = 0x000001AA  (verify SD v2.0)
ACMD41 (HCS bit 30)  → R1 = 0x00 (ready)            (loop with timeout)
CMD58 (READ_OCR)      → R1 = 0x00, OCR              (CCS bit → SDHC vs SDSC)
CMD9  (SEND_CSD)      → 16-byte CSD block            (parse v1.0 or v2.0)
CMD10 (SEND_CID)      → 16-byte CID block            (debugging)
SPI_SetSpeed(MAX_FREQUENCY)
```

### CSD parsing

- **CSD v2.0** (SDHC/SDXC): `total_sectors = (C_SIZE + 1) * 1024`,
  block size fixed at 512.
- **CSD v1.0** (SDSC): `total_sectors = (C_SIZE+1) * (C_SIZE_MULT+8) *
  2^READ_BL_LEN / 512`, block size from `READ_BL_LEN`.

## Timer

Two timers are used for different purposes:

- **CIA-A TOD** (direct register read at `0xBFE801/0xBFE901/0xBFEA01`):
  50 Hz linear tick counter for SD protocol timeouts (busy-wait, no OS
  calls). Safe to use during SPI transfers.
- **timer.device** (UNIT_VBLANK): used only for card-detect debounce
  (100 ms `TR_ADDREQUEST`) in the background task.

## CRC

Minimal CRC: hardcoded CRC byte for CMD0 (`0x95`) and CMD8 (`0x87`),
dummy `0x01` for all other commands, dummy `0xFF`/`0xFF` for data
blocks. No CRC verification on data reads.

## Build

```sh
./build.sh
```

This compiles `drivers/mmc-spi/mmc_spi.c` into `build/mmc-spi.device`.

### Debug

```sh
DDM_DEBUG_MMC_SPI=1 ./build.sh
```

Enables `DBG_MMC_SPI(...)` trace output to the serial port via
`kprintf`.
