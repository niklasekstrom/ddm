/*
 * CIA parallel port driver - cia-parallelport.library
 *
 * Driver for the Amiga 8520 CIA chips as they relate to the parallel
 * port. CIA-A provides the FLAG interrupt line (used as a negative
 * edge-sensitive interrupt controller for parallel port devices) and
 * CIA-B provides the parallel port control signals. The driver knows
 * the fixed hardware addresses (CIA-A at 0xbfe001, CIA-B at 0xbfd000)
 * and does not require them in the device tree.
 *
 * This driver is a direct child of the device tree root (the Amiga
 * motherboard). It registers an interrupt controller on its device
 * node and enumerates child devices (such as par-spi-adapter) described
 * in the device tree.
 */
#include <dos/dos.h>
#include <exec/interrupts.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <hardware/cia.h>
#include <proto/exec.h>
#include <resources/cia.h>
#include <resources/misc.h>

#include "ddm_debug.h"

#include "cia_protos.h"
#include "ddm.h"
#include "ddm_driver.h"
#include "ddm_protos.h"
#include "irq.h"
#include "misc_protos.h"
#include "parallelport.h"

struct ExecBase *SysBase = NULL;
static struct DDMBase *DDMBase = NULL;
static struct Library *LibBase = NULL;
static BPTR saved_seg_list;

/* parallelport.library base (opened in lib_init, used to register our
 * controller so child devices can obtain the CIA register mapping). */
static struct ParallelPortBase *PPBase = NULL;

/* CIA-A resource base (for interrupt controller operations) */
static struct Library *ciaabase = NULL;

/* misc.resource base (for allocating the CIA parallel port bits) */
static struct Library *miscbase = NULL;

/* The parallel port exports a single interrupt to child devices
 * (hwirq 0). Internally, hwirq 0 maps to the CIA-A FLAG interrupt
 * (the ACK pin on the parallel port), which is CIA ICR bit 4
 * (CIAICRB_FLG). */
#define CIA_NUM_IRQS 1
static const uint32_t cia_pp_icr_bits[CIA_NUM_IRQS] = {CIAICRB_FLG};

/* Per-hwirq trampoline data: the virq to dispatch and the DDMBase
 * needed to call the LVO. */
struct cia_irq_trampoline
{
    int virq;
    struct DDMBase *ddm;
};

/* Per-instance state. Allocated at probe, stored in dev->driver_data,
 * and freed at shutdown. The CIA is inherently single-instance (fixed
 * hardware addresses), but we keep the pattern for consistency. */
struct cia_pp_instance
{
    struct parallel_port_controller *pp_ctrl;                /* Parallel port controller */
    struct irq_chip irq_chip;                                /* IRQ chip for this instance */
    struct irq_domain *irq_domain;                           /* Domain for this instance */
    struct cia_irq_trampoline cia_trampolines[CIA_NUM_IRQS]; /* Per-hwirq trampoline data */
    struct Interrupt cia_isrs[CIA_NUM_IRQS];                 /* Per-hwirq ISR nodes for AddICRVector */
};

/* Name used when claiming misc.resource units. */
static const char cia_pp_name[] = "cia-parallelport";

/* The compatible strings this driver matches */
static const char *cia_pp_compatible[] = {"amiga,cia-parallelport", NULL};

/* ------------------------------------------------------------------ */
/* Interrupt controller implementation                                */
/* ------------------------------------------------------------------ */

/* The CIA FLAG interrupt is negative edge-sensitive and fixed in
 * hardware — it cannot be reconfigured. The parallel port's ACK pin
 * is connected to CIA-A's FLAG line (ICR bit 4, CIAICRB_FLG).
 *
 * The CIA is a root controller. We create an irq_domain with 1 hwirq
 * (the parallel port ACK interrupt). Child devices request hwirq 0;
 * the chip callbacks translate it to CIAICRB_FLG when talking to the
 * CIA hardware. On startup (first handler) we register a trampoline
 * via AddICRVector; the trampoline calls IRQ_GenericHandleIrq on the
 * mapped virq. On shutdown (last handler) we RemICRVector. Mask/unmask
 * use AbleICR. */

/* The trampoline ISR. Registered with AddICRVector; called by Exec
 * when the CIA ICR bit fires. We call IRQ_GenericHandleIrq to run the
 * flow handler for the mapped virq.
 *
 * NOTE: AddICRVector callbacks are called via jsr and must return via
 * rts. Using __attribute__((interrupt)) would generate rte, which pops a
 * 6-byte exception frame from a stack that only has a 4-byte jsr
 * return address — corrupting SR and PC and crashing the system.
 * Exec saves/restores all registers around the CIA server chain, so
 * clobbering d0/d1/a0/a1 is safe. */
static void cia_irq_trampoline(void *data __asm("a1"))
{
    struct cia_irq_trampoline *tr = data;
    IRQ_GenericHandleIrq(tr->ddm, tr->virq);
}

static void cia_irq_startup(struct irq_chip *chip __asm("a0"), uint32_t hwirq __asm("d0"))
{
    struct cia_pp_instance *inst = chip->dev->driver_data;
    if (!ciaabase || hwirq >= CIA_NUM_IRQS)
        return;
    uint32_t icr_bit = cia_pp_icr_bits[hwirq];
    struct cia_irq_trampoline *tr = &inst->cia_trampolines[hwirq];
    /* Resolve the virq for this hwirq from the domain revmap. */
    tr->virq = chip->dev->irq_domain->revmap[hwirq];
    tr->ddm = DDMBase;
    struct Interrupt *err;
    Disable();
    err = AddICRVector(ciaabase, icr_bit, &inst->cia_isrs[hwirq]);
    Enable();
    DBG_CIA("CIA: startup hwirq=%lu virq=%ld icr_bit=%lu\n", hwirq, (long)tr->virq, icr_bit);
    (void)err; /* failure handled by caller via return; here best-effort */
}

static void cia_irq_shutdown(struct irq_chip *chip __asm("a0"), uint32_t hwirq __asm("d0"))
{
    struct cia_pp_instance *inst = chip->dev->driver_data;
    if (!ciaabase || hwirq >= CIA_NUM_IRQS)
        return;
    uint32_t icr_bit = cia_pp_icr_bits[hwirq];
    Disable();
    RemICRVector(ciaabase, icr_bit, &inst->cia_isrs[hwirq]);
    Enable();
}

static void cia_irq_mask(struct irq_chip *chip __asm("a0"), uint32_t hwirq __asm("d0"))
{
    (void)chip;
    if (!ciaabase || hwirq >= CIA_NUM_IRQS)
        return;
    AbleICR(ciaabase, (1 << cia_pp_icr_bits[hwirq]));
}

static void cia_irq_unmask(struct irq_chip *chip __asm("a0"), uint32_t hwirq __asm("d0"))
{
    (void)chip;
    if (!ciaabase || hwirq >= CIA_NUM_IRQS)
        return;
    AbleICR(ciaabase, CIAICRF_SETCLR | (1 << cia_pp_icr_bits[hwirq]));
}

static int32_t cia_irq_set_type(struct irq_chip *chip __asm("a0"), uint32_t hwirq __asm("d0"),
                                uint32_t trigger_type __asm("d1"))
{
    (void)chip;
    (void)hwirq;
    /* CIA interrupt trigger type is fixed in hardware.
     * Only IRQ_TYPE_EDGE_FALLING is supported. */
    if (trigger_type != IRQ_TYPE_EDGE_FALLING)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Parallel port operations                                            */
/* ------------------------------------------------------------------ */

/* Fixed CIA hardware register addresses. These are hardcoded for the
 * Amiga motherboard (CIA-A at 0xbfe001, CIA-B at 0xbfd000) and never
 * change at runtime, so they are defined as compile-time constants
 * rather than stored in a private data structure. */
#define CIA_PP_DATA ((volatile uint8_t *)0xbfe101)     /* CIA-A PRB  */
#define CIA_PP_DATA_DIR ((volatile uint8_t *)0xbfe301) /* CIA-A DDRB */
#define CIA_PP_CTRL ((volatile uint8_t *)0xbfd000)     /* CIA-B PRA  */
#define CIA_PP_CTRL_DIR ((volatile uint8_t *)0xbfd200) /* CIA-B DDRA */

static void cia_write_data(struct parallel_port_controller *ctrl __asm("a0"), uint8_t value __asm("d0"))
{
    (void)ctrl;
    *CIA_PP_DATA = value;
}

static uint8_t cia_read_data(struct parallel_port_controller *ctrl __asm("a0"))
{
    (void)ctrl;
    return *CIA_PP_DATA;
}

static void cia_write_data_dir(struct parallel_port_controller *ctrl __asm("a0"), uint8_t dir __asm("d0"))
{
    (void)ctrl;
    *CIA_PP_DATA_DIR = dir;
}

static uint8_t cia_read_data_dir(struct parallel_port_controller *ctrl __asm("a0"))
{
    (void)ctrl;
    return *CIA_PP_DATA_DIR;
}

static void cia_write_ctrl(struct parallel_port_controller *ctrl __asm("a0"), uint8_t value __asm("d0"))
{
    (void)ctrl;
    *CIA_PP_CTRL = value;
}

static uint8_t cia_read_ctrl(struct parallel_port_controller *ctrl __asm("a0"))
{
    (void)ctrl;
    return *CIA_PP_CTRL;
}

static void cia_write_ctrl_dir(struct parallel_port_controller *ctrl __asm("a0"), uint8_t dir __asm("d0"))
{
    (void)ctrl;
    *CIA_PP_CTRL_DIR = dir;
}

static uint8_t cia_read_ctrl_dir(struct parallel_port_controller *ctrl __asm("a0"))
{
    (void)ctrl;
    return *CIA_PP_CTRL_DIR;
}

/* Fast-path batch transfers. These run the tight 2-E-cycle byte
 * loop directly against the CIA registers, avoiding the per-byte
 * function-pointer indirection through the ops table. The caller
 * (par-spi-adapter) handles REQ assertion and data_dir changes
 * around these calls.
 *
 * The idea of [1|2|3]E transfers was described in this blog post:
 * https://lallafa.de/blog/2015/09/amiga-parallel-port-how-fast-can-you-go/
 *
 * The loop is unrolled by two with both control-register states
 * precomputed, mirroring the original hand-optimised assembly in
 * spi_low.asm. This eliminates the per-byte XOR and halves the
 * loop-control overhead, keeping the CPU ahead of the E-cycle
 * bottleneck on stock 7 MHz machines. */
static void cia_write_bytes_2e(struct parallel_port_controller *ctrl __asm("a0"), uint8_t clk_mask __asm("d0"),
                               const uint8_t *buf __asm("a1"), int16_t count __asm("d1"))
{
    (void)ctrl;

    volatile uint8_t *ctrl_ptr = CIA_PP_CTRL;
    volatile uint8_t *data_ptr = CIA_PP_DATA;

    uint8_t cval = *ctrl_ptr;
    uint8_t cval_alt;

    /* Handle an odd leading byte so the main loop always copies pairs. */
    if (count & 1)
    {
        *data_ptr = *buf++;
        cval ^= clk_mask;
        *ctrl_ptr = cval;
    }

    cval_alt = cval ^ clk_mask;

    count >>= 1;
    if (!count)
        return;

    count--;
    do
    {
        *data_ptr = *buf++;
        *ctrl_ptr = cval_alt;
        *data_ptr = *buf++;
        *ctrl_ptr = cval;
    } while (--count >= 0);
}

static void cia_read_bytes_2e(struct parallel_port_controller *ctrl __asm("a0"), uint8_t clk_mask __asm("d0"),
                              uint8_t *buf __asm("a1"), int16_t count __asm("d1"))
{
    (void)ctrl;

    volatile uint8_t *ctrl_ptr = CIA_PP_CTRL;
    volatile uint8_t *data_ptr = CIA_PP_DATA;

    uint8_t cval = *ctrl_ptr;
    uint8_t cval_alt;

    /* Handle an odd leading byte so the main loop always copies pairs. */
    if (count & 1)
    {
        cval ^= clk_mask;
        *ctrl_ptr = cval;
        *buf++ = *data_ptr;
    }

    cval_alt = cval ^ clk_mask;

    count >>= 1;
    if (!count)
        return;

    count--;
    do
    {
        *ctrl_ptr = cval_alt;
        *buf++ = *data_ptr;
        *ctrl_ptr = cval;
        *buf++ = *data_ptr;
    } while (--count >= 0);
}

static const struct parallel_port_ops cia_pp_ops = {
    .write_data = cia_write_data,
    .read_data = cia_read_data,
    .write_data_dir = cia_write_data_dir,
    .read_data_dir = cia_read_data_dir,
    .write_ctrl = cia_write_ctrl,
    .read_ctrl = cia_read_ctrl,
    .write_ctrl_dir = cia_write_ctrl_dir,
    .read_ctrl_dir = cia_read_ctrl_dir,
    .write_bytes_2e = cia_write_bytes_2e,
    .read_bytes_2e = cia_read_bytes_2e,
};

/* ------------------------------------------------------------------ */
/* Driver interface LVOs                                               */
/* ------------------------------------------------------------------ */

/* LVO -42: DDMDrv_Probe - claim hardware, register irq + parallel port
 * controller. The matching engine calls this once after a compatible
 * match; it does both check and init. Returns 0 on success. */
int32_t cia_pp_probe(struct Library *lib __asm("a6"), struct device *dev __asm("a0"))
{
    (void)lib;
    int success = 0;

    DBG_CIA("CIA: probe dev='%s'\n", dev->node.ln_Name ? dev->node.ln_Name : "(unnamed)");

    /* Allocate per-instance state */
    struct cia_pp_instance *inst = AllocMem(sizeof(*inst), MEMF_ANY | MEMF_CLEAR);
    if (!inst)
        return -1;
    dev->driver_data = inst;

    /* Claim the CIA parallel port hardware via misc.resource. This is
     * CIA-specific, so it belongs here rather than in the generic
     * parallelport subsystem. Done before anything else so that on
     * failure the device is not marked probed and children are
     * not enumerated. */
    if (!miscbase)
    {
        miscbase = (struct Library *)OpenResource(MISCNAME);
        if (!miscbase)
        {
            success = -1;
            goto fail_free;
        }
    }

    if (AllocMiscResource(miscbase, MR_PARALLELPORT, cia_pp_name))
    {
        success = -3;
        goto fail_free;
    }

    if (AllocMiscResource(miscbase, MR_PARALLELBITS, cia_pp_name))
    {
        FreeMiscResource(miscbase, MR_PARALLELPORT);
        success = -4;
        goto fail_free;
    }

    /* Open ciaa.resource for the FLAG interrupt controller */
    if (!ciaabase)
    {
        ciaabase = (struct Library *)OpenResource(CIAANAME);
        if (!ciaabase)
        {
            FreeMiscResource(miscbase, MR_PARALLELBITS);
            FreeMiscResource(miscbase, MR_PARALLELPORT);
            success = -1;
            goto fail_free;
        }
    }

    /* Register the interrupt controller on this device node.
     * Descendants (e.g., par-spi-adapter) reference it via
     * interrupt-parent = <&cia_parallelport>. We create an irq_domain
     * with 1 hwirq (the parallel port ACK interrupt). Child devices
     * request hwirq 0; the chip callbacks translate it to CIAICRB_FLG
     * when talking to the CIA hardware. */
    inst->irq_chip.name = "cia-parallelport";
    inst->irq_chip.chip_data = NULL;
    inst->irq_chip.startup = cia_irq_startup;
    inst->irq_chip.shutdown = cia_irq_shutdown;
    inst->irq_chip.mask = cia_irq_mask;
    inst->irq_chip.unmask = cia_irq_unmask;
    inst->irq_chip.ack = NULL; /* CIA auto-clears on ICR read */
    inst->irq_chip.eoi = NULL;
    inst->irq_chip.enable = NULL;  /* default to unmask */
    inst->irq_chip.disable = NULL; /* default to mask */
    inst->irq_chip.set_type = cia_irq_set_type;
    inst->irq_domain = IRQ_CreateDomain(DDMBase, dev, &inst->irq_chip, CIA_NUM_IRQS);

    /* Initialize the per-hwirq ISR nodes. */
    {
        uint32_t i;
        for (i = 0; i < CIA_NUM_IRQS; i++)
        {
            inst->cia_isrs[i].is_Node.ln_Type = NT_INTERRUPT;
            inst->cia_isrs[i].is_Node.ln_Pri = 0;
            inst->cia_isrs[i].is_Node.ln_Name = "cia-parallelport";
            inst->cia_isrs[i].is_Data = &inst->cia_trampolines[i];
            inst->cia_isrs[i].is_Code = (void *)cia_irq_trampoline;
        }
    }

    /* Register the parallel port controller so child devices (e.g.
     * par-spi-adapter) can access the CIA hardware through the ops
     * table via PP_FindController, never touching registers directly.
     * PP_RegisterController enumerates the controller's children
     * from the device tree and matches them inline. */
    if (PPBase)
    {
        inst->pp_ctrl =
            (struct parallel_port_controller *)AllocMem(sizeof(struct parallel_port_controller), MEMF_ANY | MEMF_CLEAR);
        if (inst->pp_ctrl)
        {
            inst->pp_ctrl->dev = dev;
            inst->pp_ctrl->ops = &cia_pp_ops;
            PP_RegisterController(PPBase, inst->pp_ctrl);
        }
    }

    return 0;

fail_free:
    DBG_CIA("CIA: ERR: probe failed code=%ld\n", (long)success);
    FreeMem(inst, sizeof(*inst));
    dev->driver_data = NULL;
    return success;
}

/* LVO -48: DDMDrv_Remove */
void cia_pp_remove(struct Library *lib __asm("a6"), struct device *dev __asm("a0"))
{
    (void)lib;
    (void)dev;
}

/* LVO -54: DDMDrv_Init (reserved — merged into Probe) */
int32_t cia_pp_init(struct Library *lib __asm("a6"), struct device *dev __asm("a0"))
{
    (void)lib;
    (void)dev;
    return 0;
}

/* LVO -60: DDMDrv_Shutdown */
void cia_pp_shutdown(struct Library *lib __asm("a6"), struct device *dev __asm("a0"))
{
    (void)lib;

    struct cia_pp_instance *inst = dev->driver_data;
    if (!inst)
        return;

    /* Destroy the irq domain. This disposes all mappings, removes
     * any remaining handlers, and shuts down the chip (RemICRVector
     * for any active hwirqs). */
    if (inst->irq_domain)
        IRQ_DestroyDomain(DDMBase, inst->irq_domain);

    /* Unregister and free the parallel port controller */
    if (PPBase && inst->pp_ctrl)
    {
        PP_UnregisterController(PPBase, inst->pp_ctrl);
        FreeMem(inst->pp_ctrl, sizeof(struct parallel_port_controller));
    }

    /* Release the CIA parallel port hardware. */
    if (miscbase)
    {
        FreeMiscResource(miscbase, MR_PARALLELBITS);
        FreeMiscResource(miscbase, MR_PARALLELPORT);
    }

    FreeMem(inst, sizeof(*inst));
    dev->driver_data = NULL;
}

/* LVO -66: DDMDrv_Enumerate - no-op. Child enumeration is now owned by
 * parallelport.library (PP_RegisterController), which enumerates the
 * controller's device-tree children and matches them inline. */
int32_t cia_pp_enumerate(struct Library *lib __asm("a6"), struct device *bus __asm("a0"))
{
    (void)lib;
    (void)bus;
    return 0;
}

/* LVO -72: DTDrv_GetCompatible */
const char **cia_pp_get_compatible(struct Library *lib __asm("a6"))
{
    (void)lib;
    return cia_pp_compatible;
}

/* ------------------------------------------------------------------ */
/* Driver registration                                                 */
/* ------------------------------------------------------------------ */

static struct device_driver cia_pp_driver;

static BPTR cia_pp_lib_expunge(struct Library *lib __asm("a6"));

static struct Library *cia_pp_lib_open(struct Library *lib __asm("a6"))
{
    lib->lib_OpenCnt++;
    return lib;
}

static BPTR cia_pp_lib_close(struct Library *lib __asm("a6"))
{
    lib->lib_OpenCnt--;
    if (lib->lib_OpenCnt == 0 && (lib->lib_Flags & LIBF_DELEXP))
        return cia_pp_lib_expunge(lib);
    return 0;
}

static BPTR cia_pp_lib_expunge(struct Library *lib __asm("a6"))
{
    if (lib->lib_OpenCnt != 0)
    {
        lib->lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    /* Unregister driver */
    if (DDMBase)
        DDM_UnregisterDriver(DDMBase, &cia_pp_driver);
    if (PPBase)
    {
        CloseLibrary((struct Library *)PPBase);
        PPBase = NULL;
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

static struct Library *cia_pp_lib_init(struct ExecBase *sys_base __asm("a6"), BPTR seg_list __asm("a0"),
                                       struct Library *lib __asm("d0"))
{
    SysBase = sys_base;
    lib->lib_Node.ln_Type = NT_LIBRARY;
    lib->lib_Node.ln_Name = "cia-parallelport.library";
    lib->lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    lib->lib_Version = 1;
    lib->lib_Revision = 0;
    lib->lib_IdString = (void *)"cia-parallelport driver 1.0 (August 2026)\n\r";
    lib->lib_OpenCnt = 0;
    saved_seg_list = seg_list;

    LibBase = lib;

    /* Open ddm.library (for DDM_ functions) */
    DDMBase = (struct DDMBase *)OpenLibrary("ddm.library", 0);
    if (!DDMBase)
        return NULL;

    /* Open parallelport.library so we can register our controller */
    PPBase = (struct ParallelPortBase *)OpenLibrary("parallelport.library", 0);
    if (!PPBase)
    {
        CloseLibrary((struct Library *)DDMBase);
        DDMBase = NULL;
        return NULL;
    }

    /* Register the driver */
    cia_pp_driver.node.ln_Name = "cia-parallelport";
    cia_pp_driver.compatible = cia_pp_compatible;
    cia_pp_driver.lib_base = lib;
    cia_pp_driver.bus_type = BUS_TYPE_PLATFORM;

    if (!DDM_RegisterDriver(DDMBase, &cia_pp_driver))
        return NULL;

    return lib;
}

static uint32_t library_vectors[] = {
    (uint32_t)cia_pp_lib_open,       /* -6 */
    (uint32_t)cia_pp_lib_close,      /* -12 */
    (uint32_t)cia_pp_lib_expunge,    /* -18 */
    (uint32_t)noexec,                /* -24 */
    (uint32_t)noexec,                /* -30 (unused for library) */
    (uint32_t)noexec,                /* -36 (unused for library) */
    (uint32_t)cia_pp_probe,          /* -42: DDMDrv_Probe */
    (uint32_t)cia_pp_remove,         /* -48: DDMDrv_Remove */
    (uint32_t)noexec,                /* -54: DDMDrv_Init (reserved) */
    (uint32_t)cia_pp_shutdown,       /* -60: DDMDrv_Shutdown */
    (uint32_t)cia_pp_enumerate,      /* -66: DDMDrv_Enumerate */
    (uint32_t)cia_pp_get_compatible, /* -72: DDMDrv_GetCompatible */
    -1,
};

uint32_t auto_init_tables[] = {
    sizeof(struct Library),
    (uint32_t)library_vectors,
    0,
    (uint32_t)cia_pp_lib_init,
};

const struct Resident romtag = {
    .rt_MatchWord = RTC_MATCHWORD,
    .rt_MatchTag = (void *)&romtag,
    .rt_EndSkip = (void *)(&romtag + 1),
    .rt_Flags = RTF_AUTOINIT,
    .rt_Version = 1,
    .rt_Type = NT_LIBRARY,
    .rt_Pri = 0,
    .rt_Name = "cia-parallelport.library",
    .rt_IdString = "cia-parallelport driver 1.0 (August 2026)\n\r",
    .rt_Init = auto_init_tables,
};
