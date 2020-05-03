/*
 * DDM framework - resident tag and auto-init
 */
#define DDM_INTERNAL
#include <clib/alib_protos.h> /* NewList */
#include <exec/interrupts.h>
#include <exec/io.h> /* struct IORequest */
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <exec/resident.h>
#include <exec/types.h>
#include <proto/dos.h> /* CloseDevice */
#include <proto/exec.h>

#include "ddm_debug.h"

#include "ddm.h"
#include "gpio.h" /* struct gpio_controller, gpio_desc (GPIO_* forward decls) */
#include "irq.h"  /* struct irq_chip, irq_domain, irq_desc (IRQ_* forward decls) */

/* Library version, name, and id string */
char library_name[] = "ddm.library";
char id_string[] = "ddm 1.0 (1.9.2026)\n\r";

/* Forward declarations of LVO functions (implemented in model.c, driver.c, config.c) */
struct device *DDM_RegisterDevice(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"));
void DDM_UnregisterDevice(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"));
struct device_driver *DDM_RegisterDriver(struct DDMBase *ddm __asm("a6"), struct device_driver *drv __asm("a0"));
void DDM_UnregisterDriver(struct DDMBase *ddm __asm("a6"), struct device_driver *drv __asm("a0"));
int32_t DDM_MatchAll(struct DDMBase *ddm __asm("a6"));
int32_t DDM_MatchDevice(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"));
struct device *DDM_FindDevice(struct DDMBase *ddm __asm("a6"), const char *path __asm("a0"));
struct device *DDM_FindByCompatible(struct DDMBase *ddm __asm("a6"), const char *compatible __asm("a0"));
struct device *DDM_GetParent(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"));
struct device *DDM_GetChild(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"));
struct device *DDM_GetNextSibling(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"));
int32_t DDM_EnumerateChildren(struct DDMBase *ddm __asm("a6"), struct device *bus __asm("a0"));
struct bus_type *DDM_RegisterBusType(struct DDMBase *ddm __asm("a6"), struct bus_type *bus __asm("a0"));
struct bus_type *DDM_GetBusType(struct DDMBase *ddm __asm("a6"), uint32_t bus_type_id __asm("d0"));
int32_t DDM_LoadConfig(struct DDMBase *ddm __asm("a6"), const char *filename __asm("a0"));

/* IRQ framework functions (implemented in irq.c, exposed as LVOs -120..-192) */
struct irq_domain *IRQ_CreateDomain(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"),
                                    struct irq_chip *chip __asm("a1"), uint32_t max_irq __asm("d0"));
struct irq_domain *IRQ_FindDomain(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"));
int32_t IRQ_AddHandler(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"), struct Interrupt *isr __asm("a1"),
                       uint32_t flags __asm("a2"));
void IRQ_RemoveHandler(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"), struct Interrupt *isr __asm("a1"));
void IRQ_Enable(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"));
void IRQ_Disable(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"));
int32_t IRQ_SetType(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"), uint32_t trigger_type __asm("a1"));
void IRQ_DestroyDomain(struct DDMBase *ddm __asm("a6"), struct irq_domain *domain __asm("a0"));
int IRQ_AllocVirq(struct DDMBase *ddm __asm("a6"), struct irq_domain *domain __asm("a0"), uint32_t hwirq __asm("d0"));
void IRQ_DisposeMapping(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"));
struct irq_domain *IRQ_CreateHierarchy(struct DDMBase *ddm __asm("a6"), struct irq_domain *parent_domain __asm("a0"),
                                       struct device *dev __asm("a1"), struct irq_chip *chip __asm("a2"),
                                       uint32_t max_irq __asm("d0"));
void IRQ_SetChainedHandler(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"),
                           irq_chained_handler_t handler __asm("a0"), void *data __asm("a1"));
void IRQ_GenericHandleIrq(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"));

/* Device tree functions (implemented in devicetree.c, parser.c, exposed as LVOs -198..-228) */
int32_t DT_ParseTree(struct DDMBase *ddm __asm("a6"), const char *filename __asm("a0"));
const struct dt_property *DT_GetProperty(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"),
                                         const char *name __asm("a1"));
const char *DT_GetPropertyString(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"),
                                 const char *name __asm("a1"));
int32_t DT_GetPropertyU32(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"), const char *name __asm("a1"),
                          uint32_t *out __asm("d0"));
int DT_GetInterrupt(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"), uint32_t index __asm("d0"));
void DT_FreeInterrupt(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"));

/* GPIO framework functions (implemented in gpio.c, exposed as LVOs -234..-312) */
int32_t GPIO_RegisterController(struct DDMBase *ddm __asm("a6"), struct gpio_controller *ctrl __asm("a0"));
void GPIO_UnregisterController(struct DDMBase *ddm __asm("a6"), struct gpio_controller *ctrl __asm("a0"));
struct gpio_controller *GPIO_FindController(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"));
struct gpio_desc *GPIO_Get(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"),
                           const char *con_id __asm("a1"), uint32_t gflags __asm("d0"));
void GPIO_Free(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"));
int GPIO_GetValue(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"));
int GPIO_SetValue(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"), int value __asm("d0"));
int GPIO_DirectionInput(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"));
int GPIO_DirectionOutput(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"), int value __asm("d0"));
int GPIO_ToIrq(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"));
int GPIO_GetDirection(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"));
int GPIO_GetRawValue(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"));
int GPIO_SetRawValue(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"), int value __asm("d0"));
int GPIO_SetConfig(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"), uint32_t config __asm("d0"));

/* Global base pointer, accessible to other translation units */
struct DDMBase *DDMBase = NULL;

/* Global SysBase, set during library init. Used by exec functions
 * (AllocMem, FreeMem, AddTail, etc.) in all translation units. */
struct ExecBase *SysBase = NULL;

/* Global DOSBase, opened during library init. Used by dos functions
 * (Open, FGets, Close) in config.c. */
struct DosLibrary *DOSBase = NULL;

/* Forward declarations for open/close/expunge */
static struct Library *ddm_open(struct DDMBase *ddm __asm("a6"));
static BPTR ddm_close(struct DDMBase *ddm __asm("a6"));
static BPTR ddm_expunge(struct DDMBase *ddm __asm("a6"));

/* No-exec stub for the reserved LVO slot */
static int32_t noexec(void)
{
    return -1;
}

/* ------------------------------------------------------------------ */
/* Library vectors                                                     */
/* ------------------------------------------------------------------ */

static uint32_t library_vectors[] = {
    (uint32_t)ddm_open,                  /* -6:  Open */
    (uint32_t)ddm_close,                 /* -12: Close */
    (uint32_t)ddm_expunge,               /* -18: Expunge */
    (uint32_t)noexec,                    /* -24: Reserved */
    (uint32_t)DDM_RegisterDevice,        /* -30 */
    (uint32_t)DDM_UnregisterDevice,      /* -36 */
    (uint32_t)DDM_RegisterDriver,        /* -42 */
    (uint32_t)DDM_UnregisterDriver,      /* -48 */
    (uint32_t)DDM_MatchAll,              /* -54 */
    (uint32_t)DDM_MatchDevice,           /* -60 */
    (uint32_t)DDM_FindDevice,            /* -66 */
    (uint32_t)DDM_FindByCompatible,      /* -72 */
    (uint32_t)DDM_GetParent,             /* -78 */
    (uint32_t)DDM_GetChild,              /* -84 */
    (uint32_t)DDM_GetNextSibling,        /* -90 */
    (uint32_t)DDM_EnumerateChildren,     /* -96 */
    (uint32_t)DDM_RegisterBusType,       /* -102 */
    (uint32_t)DDM_GetBusType,            /* -108 */
    (uint32_t)DDM_LoadConfig,            /* -114 */
    (uint32_t)IRQ_CreateDomain,          /* -120 */
    (uint32_t)IRQ_FindDomain,            /* -126 */
    (uint32_t)IRQ_AddHandler,            /* -132 */
    (uint32_t)IRQ_RemoveHandler,         /* -138 */
    (uint32_t)IRQ_Enable,                /* -144 */
    (uint32_t)IRQ_Disable,               /* -150 */
    (uint32_t)IRQ_SetType,               /* -156 */
    (uint32_t)IRQ_DestroyDomain,         /* -162 */
    (uint32_t)IRQ_AllocVirq,             /* -168 */
    (uint32_t)IRQ_DisposeMapping,        /* -174 */
    (uint32_t)IRQ_CreateHierarchy,       /* -180 */
    (uint32_t)IRQ_SetChainedHandler,     /* -186 */
    (uint32_t)IRQ_GenericHandleIrq,      /* -192 */
    (uint32_t)DT_ParseTree,              /* -198 */
    (uint32_t)DT_GetProperty,            /* -204 */
    (uint32_t)DT_GetPropertyString,      /* -210 */
    (uint32_t)DT_GetPropertyU32,         /* -216 */
    (uint32_t)DT_GetInterrupt,           /* -222 */
    (uint32_t)DT_FreeInterrupt,          /* -228 */
    (uint32_t)GPIO_RegisterController,   /* -234 */
    (uint32_t)GPIO_UnregisterController, /* -240 */
    (uint32_t)GPIO_FindController,       /* -246 */
    (uint32_t)GPIO_Get,                  /* -252 */
    (uint32_t)GPIO_Free,                 /* -258 */
    (uint32_t)GPIO_GetValue,             /* -264 */
    (uint32_t)GPIO_SetValue,             /* -270 */
    (uint32_t)GPIO_DirectionInput,       /* -276 */
    (uint32_t)GPIO_DirectionOutput,      /* -282 */
    (uint32_t)GPIO_ToIrq,                /* -288 */
    (uint32_t)GPIO_GetDirection,         /* -294 */
    (uint32_t)GPIO_GetRawValue,          /* -300 */
    (uint32_t)GPIO_SetRawValue,          /* -306 */
    (uint32_t)GPIO_SetConfig,            /* -312 */
    -1,
};

/* ------------------------------------------------------------------ */
/* Open / Close / Expunge                                             */
/* ------------------------------------------------------------------ */

static struct Library *ddm_open(struct DDMBase *ddm __asm("a6"))
{
    DBG_CORE("DDM: open (cnt=%lu)\n", (unsigned long)(ddm->lib.lib_OpenCnt + 1));
    ddm->lib.lib_OpenCnt++;
    return (struct Library *)ddm;
}

static BPTR ddm_close(struct DDMBase *ddm __asm("a6"))
{
    DBG_CORE("DDM: close (cnt=%lu)\n", (unsigned long)ddm->lib.lib_OpenCnt);
    ddm->lib.lib_OpenCnt--;
    if (ddm->lib.lib_OpenCnt == 0 && (ddm->lib.lib_Flags & LIBF_DELEXP))
        return ddm_expunge(ddm);
    return 0;
}

static BPTR ddm_expunge(struct DDMBase *ddm __asm("a6"))
{
    if (ddm->lib.lib_OpenCnt != 0)
    {
        ddm->lib.lib_Flags |= LIBF_DELEXP;
        return 0;
    }

    /* Close libraries/devices opened by the config loader. */
    struct Node *ln = ddm->loaded_libs.lh_Head;
    while (ln->ln_Succ)
    {
        struct Node *next = ln->ln_Succ;
        struct loaded_lib *entry = (struct loaded_lib *)ln;
        if (entry->is_device && entry->ior)
        {
            CloseDevice(entry->ior);
            FreeMem(entry->ior, sizeof(struct IORequest));
        }
        else if (entry->lib)
        {
            CloseLibrary(entry->lib);
        }
        FreeMem(entry, sizeof(struct loaded_lib));
        ln = next;
    }

    /* Free the virq table and any remaining descs. */
    if (ddm->irq_descs)
    {
        uint32_t i;
        for (i = 0; i < ddm->nr_irqs; i++)
        {
            if (ddm->irq_descs[i])
                FreeMem(ddm->irq_descs[i], sizeof(struct irq_desc));
        }
        FreeMem(ddm->irq_descs, ddm->nr_irqs * sizeof(struct irq_desc *));
        ddm->irq_descs = NULL;
    }

    BPTR seg_list = ddm->seg_list;
    Remove(&ddm->lib.lib_Node);
    FreeMem((char *)ddm - ddm->lib.lib_NegSize, ddm->lib.lib_NegSize + ddm->lib.lib_PosSize);
    DDMBase = NULL;

    /* Close dos.library (opened in ddm_init for config loading) */
    if (DOSBase)
    {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
    }

    return seg_list;
}

/* ------------------------------------------------------------------ */
/* Init                                                               */
/* ------------------------------------------------------------------ */

static struct DDMBase *ddm_init(struct ExecBase *sys_base __asm("a6"), BPTR seg_list __asm("a0"),
                                struct DDMBase *ddm __asm("d0"))
{
    /* Set global SysBase for exec function calls in all translation units */
    SysBase = sys_base;

    /* Open dos.library for config file loading (Open/FGets/Close in config.c) */
    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 0);

    ddm->lib.lib_Node.ln_Type = NT_LIBRARY;
    ddm->lib.lib_Node.ln_Name = library_name;
    ddm->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    ddm->lib.lib_Version = 1;
    ddm->lib.lib_Revision = 0;
    ddm->lib.lib_IdString = (void *)id_string;
    ddm->lib.lib_OpenCnt = 0;
    ddm->seg_list = seg_list;

    /* Initialize lists */
    NewList(&ddm->devices);
    NewList(&ddm->drivers);
    NewList(&ddm->bus_types);
    NewList(&ddm->controllers);
    NewList(&ddm->loaded_libs);

    ddm->root = NULL;
    ddm->deferred_count = 0;
    ddm->bootstrapped = FALSE;
    ddm->bootstrapping = FALSE;
    ddm->bootstrap_trigger = NULL;

    /* Allocate the global virq table. */
    ddm->irq_descs = (struct irq_desc **)AllocMem(NR_IRQS * sizeof(struct irq_desc *), MEMF_ANY | MEMF_CLEAR);
    ddm->nr_irqs = NR_IRQS;
    ddm->next_virq = 0;

    DDMBase = ddm;
    DBG_CORE("DDM: init v%ld.%ld (NR_IRQS=%lu)\n", (long)2, (long)0, (unsigned long)NR_IRQS);
    return ddm;
}

/* ------------------------------------------------------------------ */
/* Auto-init tables                                                    */
/* ------------------------------------------------------------------ */

/* The auto-init table format:
 * [0] = size of library base (positive size)
 * [1] = pointer to function pointer table (vectors)
 * [2] = size of data area to initialize to zero
 * [3] = init function pointer
 */
uint32_t auto_init_tables[] = {
    sizeof(struct DDMBase),
    (uint32_t)library_vectors,
    0,
    (uint32_t)ddm_init,
};

/* ------------------------------------------------------------------ */
/* Resident tag                                                        */
/* ------------------------------------------------------------------ */

const struct Resident romtag = {
    .rt_MatchWord = RTC_MATCHWORD,
    .rt_MatchTag = (void *)&romtag,
    .rt_EndSkip = (void *)(&romtag + 1),
    .rt_Flags = RTF_AUTOINIT,
    .rt_Version = 1,
    .rt_Type = NT_LIBRARY,
    .rt_Pri = 0,
    .rt_Name = library_name,
    .rt_IdString = id_string,
    .rt_Init = auto_init_tables,
};
