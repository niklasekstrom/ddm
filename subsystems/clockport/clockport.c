#define CP_INTERNAL
/*
 * Clockport subsystem implementation - clockport.library
 */
#include <clib/alib_protos.h> /* NewList */
#include <exec/interrupts.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/exec.h>

#include "ddm_debug.h"

#include "clockport.h"
#include "ddm.h"        /* struct device, BUS_TYPE_* */
#include "ddm_protos.h" /* DDM_GetChild, DDM_MatchDevice */

struct ExecBase *SysBase = NULL;
static struct ClockportBase *CPBase = NULL;

/* ------------------------------------------------------------------ */
/* LVO -30: CP_RegisterController                                    */
/* ------------------------------------------------------------------ */

int32_t CP_RegisterController(struct ClockportBase *cp __asm("a6"), struct clockport_controller *ctrl __asm("a0"))
{
    if (!ctrl || !ctrl->dev)
        return -1;
    DBG_CP("CP: register controller dev='%s'\n", ctrl->dev->node.ln_Name ? ctrl->dev->node.ln_Name : "(unnamed)");
    AddTail(&cp->controllers, &ctrl->node);

    /* Enumerate the controller's children from the device tree and
     * match them with registered drivers. This mirrors spi.library's
     * SPI_RegisterController: the subsystem owns child enumeration so
     * bus drivers don't need to. */
    struct device *child = DDM_GetChild(cp->ddmbase, ctrl->dev);
    while (child)
    {
        /* Children of a clockport controller are clockport bus devices.
         * Set the bus type before matching. */
        child->bus_type = BUS_TYPE_CLOCKPORT;

        /* Try to match this device with a registered driver */
        DDM_MatchDevice(cp->ddmbase, child);

        child = DDM_GetNextSibling(cp->ddmbase, child);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* LVO -36: CP_UnregisterController                                   */
/* ------------------------------------------------------------------ */

void CP_UnregisterController(struct ClockportBase *cp __asm("a6"), struct clockport_controller *ctrl __asm("a0"))
{
    (void)cp;
    if (!ctrl)
        return;
    Remove(&ctrl->node);
}

/* ------------------------------------------------------------------ */
/* LVO -42: CP_FindController                                         */
/* ------------------------------------------------------------------ */

struct clockport_controller *CP_FindController(struct ClockportBase *cp __asm("a6"), struct device *dev __asm("a0"))
{
    if (!dev)
        return NULL;
    struct Node *node = cp->controllers.lh_Head;
    while (node->ln_Succ)
    {
        struct clockport_controller *ctrl = (struct clockport_controller *)node;
        if (ctrl->dev == dev)
            return ctrl;
        node = node->ln_Succ;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Library init / open / close / expunge                              */
/* ------------------------------------------------------------------ */

static BPTR cp_expunge(struct ClockportBase *cp __asm("a6"));

static struct Library *cp_open(struct ClockportBase *cp __asm("a6"))
{
    cp->lib.lib_OpenCnt++;
    return (struct Library *)cp;
}

static BPTR cp_close(struct ClockportBase *cp __asm("a6"))
{
    cp->lib.lib_OpenCnt--;
    if (cp->lib.lib_OpenCnt == 0 && (cp->lib.lib_Flags & LIBF_DELEXP))
        return cp_expunge(cp);
    return 0;
}

static BPTR cp_expunge(struct ClockportBase *cp __asm("a6"))
{
    if (cp->lib.lib_OpenCnt != 0)
    {
        cp->lib.lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    BPTR seg_list = cp->seg_list;
    Remove(&cp->lib.lib_Node);

    if (cp->ddmbase)
        CloseLibrary((struct Library *)cp->ddmbase);

    FreeMem((char *)cp - cp->lib.lib_NegSize, cp->lib.lib_NegSize + cp->lib.lib_PosSize);
    CPBase = NULL;
    return seg_list;
}

static int32_t noexec(void)
{
    return -1;
}

static struct ClockportBase *cp_init(struct ExecBase *sys_base __asm("a6"), BPTR seg_list __asm("a0"),
                                     struct ClockportBase *cp __asm("d0"))
{
    SysBase = sys_base;
    cp->lib.lib_Node.ln_Type = NT_LIBRARY;
    cp->lib.lib_Node.ln_Name = "clockport.library";
    cp->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    cp->lib.lib_Version = 1;
    cp->lib.lib_Revision = 0;
    cp->lib.lib_IdString = (void *)"clockport 1.0 (August 2026)\n\r";
    cp->lib.lib_OpenCnt = 0;
    cp->seg_list = seg_list;

    cp->ddmbase = NULL;
    NewList(&cp->controllers);

    /* Open ddm.library for child enumeration (DDM_GetChild, etc.) */
    cp->ddmbase = (struct DDMBase *)OpenLibrary("ddm.library", 0);

    CPBase = cp;
    return cp;
}

static uint32_t library_vectors[] = {
    (uint32_t)cp_open,                 /* -6 */
    (uint32_t)cp_close,                /* -12 */
    (uint32_t)cp_expunge,              /* -18 */
    (uint32_t)noexec,                  /* -24 */
    (uint32_t)CP_RegisterController,   /* -30 */
    (uint32_t)CP_UnregisterController, /* -36 */
    (uint32_t)CP_FindController,       /* -42 */
    -1,
};

uint32_t auto_init_tables[] = {
    sizeof(struct ClockportBase),
    (uint32_t)library_vectors,
    0,
    (uint32_t)cp_init,
};

const struct Resident romtag = {
    .rt_MatchWord = RTC_MATCHWORD,
    .rt_MatchTag = (void *)&romtag,
    .rt_EndSkip = (void *)(&romtag + 1),
    .rt_Flags = RTF_AUTOINIT,
    .rt_Version = 1,
    .rt_Type = NT_LIBRARY,
    .rt_Pri = 0,
    .rt_Name = "clockport.library",
    .rt_IdString = "clockport 1.0 (August 2026)\n\r",
    .rt_Init = auto_init_tables,
};
