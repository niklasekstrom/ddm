#define PP_INTERNAL
/*
 * Parallel port subsystem implementation - parallelport.library
 */
#include <clib/alib_protos.h> /* NewList */
#include <exec/interrupts.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/exec.h>

#include "ddm_debug.h"

#include "ddm.h"        /* struct device, BUS_TYPE_* */
#include "ddm_protos.h" /* DDM_GetChild, DDM_MatchDevice */
#include "devicetree.h" /* DT_GetInterrupt, DT_FreeInterrupt */
#include "irq.h"        /* IRQ_AddHandler, IRQ_DisposeMapping, etc. */
#include "parallelport.h"

struct ExecBase *SysBase = NULL;
static struct ParallelPortBase *PPBase = NULL;

/* ------------------------------------------------------------------ */
/* LVO -30: PP_AddInterrupt                                           */
/* ------------------------------------------------------------------ */

/* Deprecated: drivers should use DT_GetInterrupt + IRQ_AddHandler (or
 * GPIO_ToIrq + IRQ_AddHandler) directly. Kept for backward compat;
 * resolves the device's interrupt via the device tree and forwards. */
int32_t PP_AddInterrupt(struct ParallelPortBase *pp __asm("a6"), struct device *dev __asm("a0"),
                        struct Interrupt *isr __asm("a1"))
{
    int virq = DT_GetInterrupt(pp->ddmbase, dev, 0);
    if (virq < 0)
        return -1;

    int32_t ret = IRQ_AddHandler(pp->ddmbase, virq, isr, IRQ_TYPE_EDGE_BOTH);
    /* Note: do not dispose the mapping here — the caller will need
     * it for remove/enable/disable. The mapping is disposed when the
     * device shuts down (via IRQ_DisposeMapping or DT_FreeInterrupt). */
    return ret;
}

/* ------------------------------------------------------------------ */
/* LVO -36: PP_RemoveInterrupt                                        */
/* ------------------------------------------------------------------ */

void PP_RemoveInterrupt(struct ParallelPortBase *pp __asm("a6"), struct device *dev __asm("a0"),
                        struct Interrupt *isr __asm("a1"))
{
    int virq = DT_GetInterrupt(pp->ddmbase, dev, 0);
    if (virq < 0)
        return;

    IRQ_RemoveHandler(pp->ddmbase, virq, isr);
    IRQ_DisposeMapping(pp->ddmbase, virq);
}

/* ------------------------------------------------------------------ */
/* LVO -42: PP_EnableInterrupt                                        */
/* ------------------------------------------------------------------ */

void PP_EnableInterrupt(struct ParallelPortBase *pp __asm("a6"), struct device *dev __asm("a0"))
{
    int virq = DT_GetInterrupt(pp->ddmbase, dev, 0);
    if (virq < 0)
        return;

    IRQ_Enable(pp->ddmbase, virq);
}

/* ------------------------------------------------------------------ */
/* LVO -48: PP_DisableInterrupt                                        */
/* ------------------------------------------------------------------ */

void PP_DisableInterrupt(struct ParallelPortBase *pp __asm("a6"), struct device *dev __asm("a0"))
{
    int virq = DT_GetInterrupt(pp->ddmbase, dev, 0);
    if (virq < 0)
        return;

    IRQ_Disable(pp->ddmbase, virq);
}

/* ------------------------------------------------------------------ */
/* LVO -54: PP_RegisterController                                    */
/* ------------------------------------------------------------------ */

int32_t PP_RegisterController(struct ParallelPortBase *pp __asm("a6"),
                              struct parallel_port_controller *ctrl __asm("a0"))
{
    if (!ctrl || !ctrl->dev)
        return -1;
    DBG_PP("PP: register controller dev='%s'\n", ctrl->dev->node.ln_Name ? ctrl->dev->node.ln_Name : "(unnamed)");
    AddTail(&pp->controllers, &ctrl->node);

    /* Enumerate the controller's children from the device tree and match
     * them with registered drivers. This mirrors spi.library's
     * SPI_RegisterController: the subsystem owns child enumeration so
     * bus drivers don't need to. */
    struct device *child = DDM_GetChild(pp->ddmbase, ctrl->dev);
    while (child)
    {
        /* Children of a parallel port controller are parallel port
         * bus devices. Set the bus type before matching. */
        child->bus_type = BUS_TYPE_PARALLEL_PORT;

        /* Try to match this device with a registered driver */
        DDM_MatchDevice(pp->ddmbase, child);

        child = DDM_GetNextSibling(pp->ddmbase, child);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* LVO -60: PP_UnregisterController                                   */
/* ------------------------------------------------------------------ */

void PP_UnregisterController(struct ParallelPortBase *pp __asm("a6"), struct parallel_port_controller *ctrl __asm("a0"))
{
    (void)pp;
    if (!ctrl)
        return;
    Remove(&ctrl->node);
}

/* ------------------------------------------------------------------ */
/* LVO -66: PP_FindController                                         */
/* ------------------------------------------------------------------ */

struct parallel_port_controller *PP_FindController(struct ParallelPortBase *pp __asm("a6"),
                                                   struct device *dev __asm("a0"))
{
    if (!dev)
        return NULL;
    struct Node *node = pp->controllers.lh_Head;
    while (node->ln_Succ)
    {
        struct parallel_port_controller *ctrl = (struct parallel_port_controller *)node;
        if (ctrl->dev == dev)
            return ctrl;
        node = node->ln_Succ;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Library init / open / close / expunge                              */
/* ------------------------------------------------------------------ */

static BPTR pp_expunge(struct ParallelPortBase *pp __asm("a6"));

static struct Library *pp_open(struct ParallelPortBase *pp __asm("a6"))
{
    pp->lib.lib_OpenCnt++;
    return (struct Library *)pp;
}

static BPTR pp_close(struct ParallelPortBase *pp __asm("a6"))
{
    pp->lib.lib_OpenCnt--;
    if (pp->lib.lib_OpenCnt == 0 && (pp->lib.lib_Flags & LIBF_DELEXP))
        return pp_expunge(pp);
    return 0;
}

static BPTR pp_expunge(struct ParallelPortBase *pp __asm("a6"))
{
    if (pp->lib.lib_OpenCnt != 0)
    {
        pp->lib.lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    BPTR seg_list = pp->seg_list;
    Remove(&pp->lib.lib_Node);

    if (pp->ddmbase)
        CloseLibrary((struct Library *)pp->ddmbase);

    FreeMem((char *)pp - pp->lib.lib_NegSize, pp->lib.lib_NegSize + pp->lib.lib_PosSize);
    PPBase = NULL;
    return seg_list;
}

static int32_t noexec(void)
{
    return -1;
}

static struct ParallelPortBase *pp_init(struct ExecBase *sys_base __asm("a6"), BPTR seg_list __asm("a0"),
                                        struct ParallelPortBase *pp __asm("d0"))
{
    SysBase = sys_base;
    pp->lib.lib_Node.ln_Type = NT_LIBRARY;
    pp->lib.lib_Node.ln_Name = "parallelport.library";
    pp->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    pp->lib.lib_Version = 1;
    pp->lib.lib_Revision = 0;
    pp->lib.lib_IdString = (void *)"parallelport 1.0 (August 2026)\n\r";
    pp->lib.lib_OpenCnt = 0;
    pp->seg_list = seg_list;

    pp->ddmbase = NULL;
    NewList(&pp->controllers);

    /* Open ddm.library for child enumeration (DDM_GetChild, etc.),
     * IRQ handler management (IRQ_AddHandler, etc.), and DT_* device
     * tree functions (DT_GetInterrupt) — all now part of ddm.library */
    pp->ddmbase = (struct DDMBase *)OpenLibrary("ddm.library", 0);

    PPBase = pp;
    return pp;
}

static uint32_t library_vectors[] = {
    (uint32_t)pp_open,                 /* -6 */
    (uint32_t)pp_close,                /* -12 */
    (uint32_t)pp_expunge,              /* -18 */
    (uint32_t)noexec,                  /* -24 */
    (uint32_t)PP_AddInterrupt,         /* -30 */
    (uint32_t)PP_RemoveInterrupt,      /* -36 */
    (uint32_t)PP_EnableInterrupt,      /* -42 */
    (uint32_t)PP_DisableInterrupt,     /* -48 */
    (uint32_t)PP_RegisterController,   /* -54 */
    (uint32_t)PP_UnregisterController, /* -60 */
    (uint32_t)PP_FindController,       /* -66 */
    -1,
};

uint32_t auto_init_tables[] = {
    sizeof(struct ParallelPortBase),
    (uint32_t)library_vectors,
    0,
    (uint32_t)pp_init,
};

const struct Resident romtag = {
    .rt_MatchWord = RTC_MATCHWORD,
    .rt_MatchTag = (void *)&romtag,
    .rt_EndSkip = (void *)(&romtag + 1),
    .rt_Flags = RTF_AUTOINIT,
    .rt_Version = 1,
    .rt_Type = NT_LIBRARY,
    .rt_Pri = 0,
    .rt_Name = "parallelport.library",
    .rt_IdString = "parallelport 1.0 (August 2026)\n\r",
    .rt_Init = auto_init_tables,
};
