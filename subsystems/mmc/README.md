# mmc.library

The MMC/SD card subsystem. Provides a registry for MMC/SD block
devices so that card drivers and other components can find cards by
their device-tree device. It is a peer of `ddm.library` — a separate
library.

## What it does

- **Card registration.** Card drivers (e.g. `mmc-spi.device`) call
  `MMC_RegisterCard` with a `struct mmc_card` describing the card
  (type, total sectors, block size). The card is stored in the
  device's `bus_data` and added to the cards list.
- **Card lookup.** Components can find a card by its device-tree device
  (`MMC_FindCard`) or get the first registered card
  (`MMC_GetFirstCard`).

## Relationship to other components

- Depends on **ddm.library** (device types).
- Card drivers (`mmc-spi.device`) register cards with this library.
- This is a lightweight registry — it does not perform I/O or manage
  the card protocol; that is the card driver's job.

## Files

| File    | Description                                              |
|---------|----------------------------------------------------------|
| `mmc.c` | Subsystem implementation: LVOs, card registration/lookup |
| `mmc.h` | Types (`mmc_card`, `MmcBase`) + LVO stub macros          |

## LVOs

| LVO  | Function           | Description                              |
|------|--------------------|------------------------------------------|
| -30  | MMC_RegisterCard   | Register an MMC card                     |
| -36  | MMC_UnregisterCard | Unregister an MMC card                   |
| -42  | MMC_FindCard       | Find the mmc_card for a device tree node |
| -48  | MMC_GetFirstCard   | Get the first registered card            |

## Build

Built by `build.sh` → `mmc.library` (placed in `LIBS:`).
