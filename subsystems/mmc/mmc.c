#define MMC_INTERNAL
/*
 * MMC subsystem implementation - mmc.library
 */
#include <clib/alib_protos.h> /* NewList */
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <stddef.h> /* offsetof */

#include "ddm_debug.h"

#include "ddm.h"
#include "mmc.h"

struct ExecBase *SysBase = NULL;
static struct MmcBase *MmcBase = NULL;

/* ------------------------------------------------------------------ */
/* LVO -30: MMC_RegisterCard                                         */
/* ------------------------------------------------------------------ */

int32_t MMC_RegisterCard(struct MmcBase *mb __asm("a6"), struct mmc_card *card __asm("a0"))
{
    if (!card || !card->dev)
        return -1;

    DBG_MMC("MMC: register card dev='%s'\n", card->dev->node.ln_Name ? card->dev->node.ln_Name : "(unnamed)");

    /* Store the mmc_card pointer in the device's bus_data */
    card->dev->bus_data = card;

    /* Add to cards list (uses card->node, NOT dev->node,
     * which is already on DDMBase->devices). */
    AddTail(&mb->cards, &card->node);

    return 0;
}

/* ------------------------------------------------------------------ */
/* LVO -36: MMC_UnregisterCard                                       */
/* ------------------------------------------------------------------ */

void MMC_UnregisterCard(struct MmcBase *mb __asm("a6"), struct mmc_card *card __asm("a0"))
{
    if (!card)
        return;

    if (card->dev)
        card->dev->bus_data = NULL;

    Remove(&card->node);
}

/* ------------------------------------------------------------------ */
/* LVO -42: MMC_FindCard                                             */
/* ------------------------------------------------------------------ */

struct mmc_card *MMC_FindCard(struct MmcBase *mb __asm("a6"), struct device *dev __asm("a0"))
{
    if (!dev)
        return NULL;
    return (struct mmc_card *)dev->bus_data;
}

/* ------------------------------------------------------------------ */
/* LVO -48: MMC_GetFirstCard                                         */
/* ------------------------------------------------------------------ */

struct mmc_card *MMC_GetFirstCard(struct MmcBase *mb __asm("a6"))
{
    struct Node *node = mb->cards.lh_Head;
    if (!node->ln_Succ)
        return NULL;

    /* The list stores card->node. Recover the mmc_card via offsetof. */
    return (struct mmc_card *)((uint8_t *)node - offsetof(struct mmc_card, node));
}

/* ------------------------------------------------------------------ */
/* Library init / open / close / expunge                              */
/* ------------------------------------------------------------------ */

static BPTR mmc_expunge(struct MmcBase *mb __asm("a6"));

static struct Library *mmc_open(struct MmcBase *mb __asm("a6"))
{
    mb->lib.lib_OpenCnt++;
    return (struct Library *)mb;
}

static BPTR mmc_close(struct MmcBase *mb __asm("a6"))
{
    mb->lib.lib_OpenCnt--;
    if (mb->lib.lib_OpenCnt == 0 && (mb->lib.lib_Flags & LIBF_DELEXP))
        return mmc_expunge(mb);
    return 0;
}

static BPTR mmc_expunge(struct MmcBase *mb __asm("a6"))
{
    if (mb->lib.lib_OpenCnt != 0)
    {
        mb->lib.lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    BPTR seg_list = mb->seg_list;
    Remove(&mb->lib.lib_Node);
    FreeMem((char *)mb - mb->lib.lib_NegSize, mb->lib.lib_NegSize + mb->lib.lib_PosSize);
    MmcBase = NULL;
    return seg_list;
}

static int32_t noexec(void)
{
    return -1;
}

static struct MmcBase *mmc_init(struct ExecBase *sys_base __asm("a6"), BPTR seg_list __asm("a0"),
                                struct MmcBase *mb __asm("d0"))
{
    SysBase = sys_base;
    mb->lib.lib_Node.ln_Type = NT_LIBRARY;
    mb->lib.lib_Node.ln_Name = "mmc.library";
    mb->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    mb->lib.lib_Version = 1;
    mb->lib.lib_Revision = 0;
    mb->lib.lib_IdString = (void *)"mmc 1.0 (August 2026)\n\r";
    mb->lib.lib_OpenCnt = 0;
    mb->seg_list = seg_list;

    NewList(&mb->cards);

    MmcBase = mb;

    return mb;
}

static uint32_t library_vectors[] = {
    (uint32_t)mmc_open,           /* -6 */
    (uint32_t)mmc_close,          /* -12 */
    (uint32_t)mmc_expunge,        /* -18 */
    (uint32_t)noexec,             /* -24 */
    (uint32_t)MMC_RegisterCard,   /* -30 */
    (uint32_t)MMC_UnregisterCard, /* -36 */
    (uint32_t)MMC_FindCard,       /* -42 */
    (uint32_t)MMC_GetFirstCard,   /* -48 */
    -1,
};

uint32_t auto_init_tables[] = {
    sizeof(struct MmcBase),
    (uint32_t)library_vectors,
    0,
    (uint32_t)mmc_init,
};

const struct Resident romtag = {
    .rt_MatchWord = RTC_MATCHWORD,
    .rt_MatchTag = (void *)&romtag,
    .rt_EndSkip = (void *)(&romtag + 1),
    .rt_Flags = RTF_AUTOINIT,
    .rt_Version = 1,
    .rt_Type = NT_LIBRARY,
    .rt_Pri = 0,
    .rt_Name = "mmc.library",
    .rt_IdString = "mmc 1.0 (August 2026)\n\r",
    .rt_Init = auto_init_tables,
};
