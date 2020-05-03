/*
 * MMC subsystem - mmc.library
 *
 * Provides the MMC/SD card block device framework. Card drivers
 * (like mmc-spi.device) register with this library.
 */
#ifndef MMC_H_
#define MMC_H_

#include "ddm.h"     /* struct device */
#include "ddm_gcc.h" /* LVO stub macros */
#include <dos/dos.h> /* BPTR */
#include <exec/libraries.h>
#include <exec/types.h>

/* Bus type for MMC devices (must match BUS_TYPE_MMC in devicetree.h) */
/* #define BUS_TYPE_MMC  4  -- defined in devicetree.h */

/* ------------------------------------------------------------------ */
/* MMC card                                                            */
/* ------------------------------------------------------------------ */

/* An MMC card is a block device. The card driver registers it with
 * mmc.library so that other components can find it. */
struct mmc_card
{
    /* Node for the cards list in MmcBase. Must NOT reuse dev->node,
     * which is already on DDMBase->devices. */
    struct Node node;

    /* The device tree device for this card */
    struct device *dev;

    /* Card type (driver-specific) */
    uint32_t card_type;

    /* Total number of sectors */
    uint32_t total_sectors;

    /* Block size shift (e.g. 9 for 512-byte blocks) */
    uint32_t block_size_shift;

    /* Block size in bytes */
    uint32_t block_size;

    /* Private data for the card driver */
    void *private;
};

/* ------------------------------------------------------------------ */
/* MMC library base                                                    */
/* ------------------------------------------------------------------ */

struct MmcBase
{
    struct Library lib;
    BPTR seg_list;     /* Segment list (saved at init) */
    struct List cards; /* List of registered MMC cards */
};

/* ------------------------------------------------------------------ */
/* LVO prototypes                                                      */
/*                                                                    */
/* These declare the real LVO functions implemented in mmc.c.         */
/* External callers use the macro stubs below; the implementation     */
/* file (mmc.c) defines MMC_INTERNAL to suppress the macros.         */
/* ------------------------------------------------------------------ */

/* LVO -30: Register an MMC card. Returns 0 on success. */
int32_t MMC_RegisterCard(struct MmcBase *mb __asm("a6"), struct mmc_card *card __asm("a0"));

/* LVO -36: Unregister an MMC card. */
void MMC_UnregisterCard(struct MmcBase *mb __asm("a6"), struct mmc_card *card __asm("a0"));

/* LVO -42: Find the MMC card for a given device tree device.
 * Returns the mmc_card, or NULL if not found. */
struct mmc_card *MMC_FindCard(struct MmcBase *mb __asm("a6"), struct device *dev __asm("a0"));

/* LVO -48: Get the first registered MMC card (for single-card systems). */
struct mmc_card *MMC_GetFirstCard(struct MmcBase *mb __asm("a6"));

/* ------------------------------------------------------------------ */
/* LVO stub macros                                                    */
/*                                                                    */
/* External callers use these macros to make LVO calls through the     */
/* mmc.library base in a6.                                            */
/*                                                                    */
/* The implementation file (mmc.c) must define MMC_INTERNAL before     */
/* including this header to suppress the macro definitions.          */
/* ------------------------------------------------------------------ */

#ifndef MMC_INTERNAL

#define MMC_RegisterCard(mb, card) __DDM_LVO_RET_1A0(int32_t, -30, (mb), (card))
#define MMC_UnregisterCard(mb, card) __DDM_LVO_VOID_1A0(-36, (mb), (card))
#define MMC_FindCard(mb, dev) __DDM_LVO_RET_1A0(struct mmc_card *, -42, (mb), (dev))
#define MMC_GetFirstCard(mb) __DDM_LVO_RET_1D0(struct mmc_card *, -48, (mb), 0)

#endif /* !MMC_INTERNAL */

#endif /* MMC_H_ */
