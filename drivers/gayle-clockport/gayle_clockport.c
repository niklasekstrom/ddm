/*
 * Gayle clockport driver - gayle-clockport.library
 *
 * Driver for the Amiga clockport as exposed via the Gayle chip. The
 * clockport is a fixed memory-mapped region (default 0xD80001) with
 * 16 registers at 4-byte stride. This driver reads the base address
 * from the device tree, registers a clockport controller (providing
 * register-access ops + fast-path FIFO), and
 * registers an interrupt controller using AddIntServer/RemIntServer
 * (the clockport interrupt is an Amiga system interrupt, not a CIA
 * interrupt).
 *
 * This driver is a direct child of the device tree root (the Amiga
 * motherboard). It registers a clockport controller on its device
 * node and enumerates child devices (such as spider) described in the
 * device tree.
 */
#include <exec/interrupts.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <hardware/intbits.h>
#include <proto/exec.h>

#include "ddm_debug.h"

#include "clockport.h"
#include "ddm.h"
#include "ddm_driver.h"
#include "ddm_protos.h"
#include "devicetree.h" /* DT_GetPropertyU32 */
#include "irq.h"

struct ExecBase *SysBase = NULL;
static struct DDMBase *DDMBase = NULL;
static struct ClockportBase *CPBase = NULL;
static struct Library *LibBase = NULL;
static BPTR saved_seg_list;

/* Per-instance state. Allocated at probe, stored in dev->driver_data,
 * and freed at shutdown. Keeping this on the heap (rather than in
 * file-scope statics) lets multiple clockport instances coexist with
 * different base addresses — each probe gets its own private state. */
struct gayle_cp_instance
{
    volatile uint8_t *base;               /* Memory-mapped base address */
    struct clockport_controller *cp_ctrl; /* Registered controller */
    struct Interrupt clockport_isr;       /* Amiga system interrupt server */
    struct irq_chip irq_chip;             /* IRQ chip for this instance */
    struct irq_domain *irq_domain;        /* Domain for this instance */
    int parent_virq;                      /* virq for hwirq 0 (the single
                                           * clockport interrupt) */
};

/* The compatible strings this driver matches */
static const char *gayle_cp_compatible[] = {"amiga,gayle-clockport", NULL};

/* ------------------------------------------------------------------ */
/* Clockport register access                                           */
/* ------------------------------------------------------------------ */

/* The clockport has 16 registers at 4-byte stride from the base
 * address. The base address is read from the device tree "reg"
 * property (default 0xD80001) and stored in the instance. */
static inline volatile uint8_t *cp_reg_addr(struct gayle_cp_instance *inst, uint16_t reg)
{
    return inst->base + ((reg) << 2);
}

static uint8_t gayle_read_reg(struct clockport_controller *ctrl __asm("a0"), uint16_t reg __asm("d0"))
{
    struct gayle_cp_instance *inst = ctrl->private;
    return *cp_reg_addr(inst, reg);
}

static void gayle_write_reg(struct clockport_controller *ctrl __asm("a0"), uint16_t reg __asm("d0"),
                            uint8_t val __asm("d1"))
{
    struct gayle_cp_instance *inst = ctrl->private;
    *cp_reg_addr(inst, reg) = val;
}
/* Fast-path batch register transfers. These run the tight loop
 * directly against the given register, avoiding per-byte
 * function-pointer indirection. */
static void gayle_read_reg_bytes(struct clockport_controller *ctrl __asm("a0"), uint16_t reg __asm("d0"),
                                 uint8_t *buf __asm("a1"), int16_t count __asm("d1"))
{
    if (!count)
        return;

    struct gayle_cp_instance *inst = ctrl->private;
    volatile uint8_t *r = cp_reg_addr(inst, reg);
    count--;
    do
    {
        *buf++ = *r;
    } while (--count >= 0);
}

static void gayle_write_reg_bytes(struct clockport_controller *ctrl __asm("a0"), uint16_t reg __asm("d0"),
                                  const uint8_t *buf __asm("a1"), int16_t count __asm("d1"))
{
    if (!count)
        return;

    struct gayle_cp_instance *inst = ctrl->private;
    volatile uint8_t *r = cp_reg_addr(inst, reg);
    count--;
    do
    {
        *r = *buf++;
    } while (--count >= 0);
}

static const struct clockport_ops gayle_cp_ops = {
    .read_reg = gayle_read_reg,
    .write_reg = gayle_write_reg,
    .read_reg_bytes = gayle_read_reg_bytes,
    .write_reg_bytes = gayle_write_reg_bytes,
};

/* ------------------------------------------------------------------ */
/* Interrupt controller implementation                                */
/* ------------------------------------------------------------------ */

/* The clockport interrupt is the Amiga system interrupt INTB_EXTER
 * (INT6). The driver registers a single Amiga system interrupt server
 * via AddIntServer in the chip's startup callback (gayle_irq_startup),
 * which is invoked by the framework when the first child chains on
 * this controller's virq (or when a direct handler is added). The
 * server calls IRQ_GenericHandleIrq on the virq for hwirq 0, which
 * runs the flow handler (and thus any registered child handlers,
 * e.g. spider).
 *
 * This is a root controller with a single hwirq (0). The chip's
 * startup/shutdown register/remove the Amiga system interrupt server;
 * individual device interrupt arming is handled by the child driver
 * (e.g. spider arms INT_ARMED on the SPIder hardware). */

/* The Amiga system interrupt server. Called by Exec when the
 * clockport interrupt fires. We dispatch to IRQ_GenericHandleIrq
 * for the virq mapped to hwirq 0. The instance is passed in a1 via
 * is_Data.
 *
 * NOTE: This is an AddIntServer callback, NOT a SetIntVector handler.
 * Exec calls it via jsr and expects rts. Using __attribute__((interrupt)) here
 * would generate rte, which pops a 6-byte exception frame (SR+PC) from
 * a stack that only has a 4-byte jsr return address — corrupting SR
 * and PC and crashing the system. Exec saves/restores all registers
 * around the entire server chain, so clobbering d0/d1/a0/a1 is safe. */
static void clockport_int_server(void *data __asm("a1"))
{
    struct gayle_cp_instance *inst = data;
    IRQ_GenericHandleIrq(DDMBase, inst->parent_virq);
}

static void gayle_irq_startup(struct irq_chip *chip __asm("a0"), uint32_t hwirq __asm("d0"))
{
    (void)hwirq;
    struct gayle_cp_instance *inst = chip->dev->driver_data;
    if (!inst)
        return;
    /* Register the Amiga system interrupt server. This is called by
     * the framework (via chip_startup) when the first child chains on
     * this controller's virq, or when a direct handler is added. */
    AddIntServer(INTB_EXTER, &inst->clockport_isr);
}

static void gayle_irq_shutdown(struct irq_chip *chip __asm("a0"), uint32_t hwirq __asm("d0"))
{
    (void)hwirq;
    struct gayle_cp_instance *inst = chip->dev->driver_data;
    if (!inst)
        return;
    /* Remove the Amiga system interrupt server. This is called by
     * the framework (via chip_shutdown) when the last child unchains
     * or the domain is destroyed. */
    RemIntServer(INTB_EXTER, &inst->clockport_isr);
}

static void gayle_irq_mask(struct irq_chip *chip __asm("a0"), uint32_t hwirq __asm("d0"))
{
    (void)chip;
    (void)hwirq;
    /* No per-line mask for the shared Amiga system interrupt. */
}

static void gayle_irq_unmask(struct irq_chip *chip __asm("a0"), uint32_t hwirq __asm("d0"))
{
    (void)chip;
    (void)hwirq;
    /* No per-line unmask for the shared Amiga system interrupt. */
}

static int32_t gayle_irq_set_type(struct irq_chip *chip __asm("a0"), uint32_t hwirq __asm("d0"),
                                  uint32_t trigger_type __asm("d1"))
{
    (void)chip;
    (void)hwirq;
    /* The clockport interrupt is routed to the Amiga system
     * interrupt INT6 (INTB_EXTER), which is a shared line used by
     * various other devices as well. The clockport drives it
     * active-low level triggered, so only level-low (or NONE) is
     * accepted; edge trigger types are rejected to avoid
     * misconfiguring the shared line. */
    if (trigger_type != IRQ_TYPE_NONE && trigger_type != IRQ_TYPE_LEVEL_LOW)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Driver interface LVOs                                               */
/* ------------------------------------------------------------------ */

/* LVO -42: DDMDrv_Probe - claim hardware, register irq + clockport
 * controller. The matching engine calls this once after a compatible
 * match; it does both check and init. Returns 0 on success. */
int32_t gayle_cp_probe(struct Library *lib __asm("a6"), struct device *dev __asm("a0"))
{
    (void)lib;

    DBG_GAYLE("GAYLE-CP: probe dev='%s'\n", dev->node.ln_Name ? dev->node.ln_Name : "(unnamed)");

    /* Allocate per-instance state so multiple clockport nodes can
     * coexist with different base addresses. Stored in
     * dev->driver_data and freed at shutdown. */
    struct gayle_cp_instance *inst =
        (struct gayle_cp_instance *)AllocMem(sizeof(struct gayle_cp_instance), MEMF_ANY | MEMF_CLEAR);
    if (!inst)
        return -1;
    dev->driver_data = inst;

    /* Read the clockport base address from the device tree "reg"
     * property. Default to 0xD80001 if not specified. */
    uint32_t base_addr = 0xD80001;
    DT_GetPropertyU32(DDMBase, dev, "reg", &base_addr);
    inst->base = (volatile uint8_t *)base_addr;
    DBG_GAYLE("GAYLE-CP: base=0x%08lx\n", base_addr);

    /* Register the interrupt controller on this device node.
     * Descendants (e.g. spider) reference it via
     * interrupt-parent = <&gayle_clockport>. We create an irq_domain
     * with 1 hwirq (the single clockport interrupt). The Amiga system
     * interrupt server (registered below) dispatches via
     * IRQ_GenericHandleIrq on the virq for hwirq 0. */
    inst->irq_chip.name = "gayle-clockport";
    inst->irq_chip.chip_data = NULL;
    inst->irq_chip.startup = gayle_irq_startup;
    inst->irq_chip.shutdown = gayle_irq_shutdown;
    inst->irq_chip.mask = gayle_irq_mask;
    inst->irq_chip.unmask = gayle_irq_unmask;
    inst->irq_chip.ack = NULL;
    inst->irq_chip.eoi = NULL;
    inst->irq_chip.enable = NULL;
    inst->irq_chip.disable = NULL;
    inst->irq_chip.set_type = gayle_irq_set_type;
    inst->irq_domain = IRQ_CreateDomain(DDMBase, dev, &inst->irq_chip, 1);

    /* Pre-allocate the virq for hwirq 0 so the system interrupt
     * server can dispatch to it. */
    inst->parent_virq = IRQ_AllocVirq(DDMBase, inst->irq_domain, 0);

    /* Initialize the Amiga system interrupt server node. The server
     * is actually registered (AddIntServer) in gayle_irq_startup,
     * which the framework calls when the first child chains on this
     * controller's virq. is_Data carries the instance so the ISR
     * can recover it. */
    inst->clockport_isr.is_Node.ln_Type = NT_INTERRUPT;
    inst->clockport_isr.is_Node.ln_Pri = -60;
    inst->clockport_isr.is_Node.ln_Name = "gayle-clockport";
    inst->clockport_isr.is_Data = inst;
    inst->clockport_isr.is_Code = (void *)clockport_int_server;

    /* Register the clockport controller so child devices (e.g.
     * spider) can access the clockport registers through the ops
     * table via CP_FindController, never touching registers directly.
     * CP_RegisterController enumerates the controller's children
     * from the device tree and matches them inline. The instance is
     * handed to the ops via ctrl->private. */
    if (CPBase)
    {
        inst->cp_ctrl =
            (struct clockport_controller *)AllocMem(sizeof(struct clockport_controller), MEMF_ANY | MEMF_CLEAR);
        if (inst->cp_ctrl)
        {
            inst->cp_ctrl->dev = dev;
            inst->cp_ctrl->ops = &gayle_cp_ops;
            inst->cp_ctrl->private = inst;
            CP_RegisterController(CPBase, inst->cp_ctrl);
        }
    }

    return 0;
}

/* LVO -48: DDMDrv_Remove */
void gayle_cp_remove(struct Library *lib __asm("a6"), struct device *dev __asm("a0"))
{
    (void)lib;
    (void)dev;
}

/* LVO -54: DDMDrv_Init (reserved — merged into Probe) */
int32_t gayle_cp_init(struct Library *lib __asm("a6"), struct device *dev __asm("a0"))
{
    (void)lib;
    (void)dev;
    return 0;
}

/* LVO -60: DDMDrv_Shutdown */
void gayle_cp_shutdown(struct Library *lib __asm("a6"), struct device *dev __asm("a0"))
{
    (void)lib;
    struct gayle_cp_instance *inst = dev->driver_data;
    if (!inst)
        return;

    /* Destroy the irq domain. This disposes all mappings. The Amiga
     * system interrupt server is removed in gayle_irq_shutdown
     * (called via chip_shutdown when the child clears its chained
     * handler during its own teardown). */
    if (inst->irq_domain)
        IRQ_DestroyDomain(DDMBase, inst->irq_domain);

    /* Unregister and free the clockport controller */
    if (CPBase && inst->cp_ctrl)
    {
        CP_UnregisterController(CPBase, inst->cp_ctrl);
        FreeMem(inst->cp_ctrl, sizeof(struct clockport_controller));
    }

    FreeMem(inst, sizeof(struct gayle_cp_instance));
    dev->driver_data = NULL;
}

/* LVO -66: DDMDrv_Enumerate - no-op. Child enumeration is now owned by
 * clockport.library (CP_RegisterController), which enumerates the
 * controller's device-tree children and matches them inline. */
int32_t gayle_cp_enumerate(struct Library *lib __asm("a6"), struct device *bus __asm("a0"))
{
    (void)lib;
    (void)bus;
    return 0;
}

/* LVO -72: DTDrv_GetCompatible */
const char **gayle_cp_get_compatible(struct Library *lib __asm("a6"))
{
    (void)lib;
    return gayle_cp_compatible;
}

/* ------------------------------------------------------------------ */
/* Driver registration                                                 */
/* ------------------------------------------------------------------ */

static struct device_driver gayle_cp_driver;

static BPTR gayle_cp_lib_expunge(struct Library *lib __asm("a6"));

static struct Library *gayle_cp_lib_open(struct Library *lib __asm("a6"))
{
    lib->lib_OpenCnt++;
    return lib;
}

static BPTR gayle_cp_lib_close(struct Library *lib __asm("a6"))
{
    lib->lib_OpenCnt--;
    if (lib->lib_OpenCnt == 0 && (lib->lib_Flags & LIBF_DELEXP))
        return gayle_cp_lib_expunge(lib);
    return 0;
}

static BPTR gayle_cp_lib_expunge(struct Library *lib __asm("a6"))
{
    if (lib->lib_OpenCnt != 0)
    {
        lib->lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    /* Unregister driver */
    if (DDMBase)
        DDM_UnregisterDriver(DDMBase, &gayle_cp_driver);
    if (CPBase)
    {
        CloseLibrary((struct Library *)CPBase);
        CPBase = NULL;
    }
    if (DDMBase)
    {
        CloseLibrary((struct Library *)DDMBase);
        DDMBase = NULL;
    }
    BPTR seg_list = saved_seg_list;
    Remove(&lib->lib_Node);
    FreeMem((char *)lib - lib->lib_NegSize, lib->lib_NegSize + lib->lib_PosSize);
    LibBase = NULL;
    return seg_list;
}

static int32_t noexec(void)
{
    return -1;
}

static struct Library *gayle_cp_lib_init(struct ExecBase *sys_base __asm("a6"), BPTR seg_list __asm("a0"),
                                         struct Library *lib __asm("d0"))
{
    SysBase = sys_base;
    lib->lib_Node.ln_Type = NT_LIBRARY;
    lib->lib_Node.ln_Name = "gayle-clockport.library";
    lib->lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    lib->lib_Version = 1;
    lib->lib_Revision = 0;
    lib->lib_IdString = (void *)"gayle-clockport driver 1.0 (August 2026)\n\r";
    lib->lib_OpenCnt = 0;
    saved_seg_list = seg_list;

    LibBase = lib;

    /* Open ddm.library (for DDM_ functions and DT_* device tree functions) */
    DDMBase = (struct DDMBase *)OpenLibrary("ddm.library", 0);
    if (!DDMBase)
        return NULL;

    /* Open clockport.library so we can register our controller */
    CPBase = (struct ClockportBase *)OpenLibrary("clockport.library", 0);
    if (!CPBase)
    {
        CloseLibrary((struct Library *)DDMBase);
        DDMBase = NULL;
        return NULL;
    }

    /* Register the driver */
    gayle_cp_driver.node.ln_Name = "gayle-clockport";
    gayle_cp_driver.compatible = gayle_cp_compatible;
    gayle_cp_driver.lib_base = lib;
    gayle_cp_driver.bus_type = BUS_TYPE_PLATFORM;

    if (!DDM_RegisterDriver(DDMBase, &gayle_cp_driver))
        return NULL;

    return lib;
}

static uint32_t library_vectors[] = {
    (uint32_t)gayle_cp_lib_open,       /* -6 */
    (uint32_t)gayle_cp_lib_close,      /* -12 */
    (uint32_t)gayle_cp_lib_expunge,    /* -18 */
    (uint32_t)noexec,                  /* -24 */
    (uint32_t)noexec,                  /* -30 (unused for library) */
    (uint32_t)noexec,                  /* -36 (unused for library) */
    (uint32_t)gayle_cp_probe,          /* -42: DDMDrv_Probe */
    (uint32_t)gayle_cp_remove,         /* -48: DDMDrv_Remove */
    (uint32_t)noexec,                  /* -54: DDMDrv_Init (reserved) */
    (uint32_t)gayle_cp_shutdown,       /* -60: DDMDrv_Shutdown */
    (uint32_t)gayle_cp_enumerate,      /* -66: DDMDrv_Enumerate */
    (uint32_t)gayle_cp_get_compatible, /* -72: DDMDrv_GetCompatible */
    -1,
};

uint32_t auto_init_tables[] = {
    sizeof(struct Library),
    (uint32_t)library_vectors,
    0,
    (uint32_t)gayle_cp_lib_init,
};

const struct Resident romtag = {
    .rt_MatchWord = RTC_MATCHWORD,
    .rt_MatchTag = (void *)&romtag,
    .rt_EndSkip = (void *)(&romtag + 1),
    .rt_Flags = RTF_AUTOINIT,
    .rt_Version = 1,
    .rt_Type = NT_LIBRARY,
    .rt_Pri = 0,
    .rt_Name = "gayle-clockport.library",
    .rt_IdString = "gayle-clockport driver 1.0 (August 2026)\n\r",
    .rt_Init = auto_init_tables,
};
