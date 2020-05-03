/*
 * par-spi-adapter device tree driver
 *
 * Implements the DDM interface for the parallel-port-
 * to-SPI adapter. On probe/init, it initializes the parallel port
 * protocol and registers as an SPI controller with spi.library, a
 * GPIO controller with ddm.library, and an IRQ controller with
 * ddm.library.
 *
 * The parallel-port-to-SPI protocol implementation is included here
 * directly (ported from spi-lib/spi.c).
 */
#include <dos/dos.h>
#include <exec/interrupts.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/exec.h>

#include "ddm_debug.h"

#include "ddm.h"
#include "ddm_driver.h"
#include "ddm_protos.h"
#include "devicetree.h" /* struct device, BUS_TYPE_* */
#include "gpio.h"
#include "irq.h" /* IRQ_AddHandler, etc. */
#include "parallelport.h"
#include "spi.h"

struct ExecBase *SysBase = NULL;
static struct DDMBase *DDMBase = NULL;

static struct SpiBase *SpiBase = NULL;
static struct Library *LibBase = NULL;
static struct ParallelPortBase *PPBase = NULL;
static BPTR saved_seg_list;

static const char *par_spi_compatible[] = {"amiga,par-spi-adapter", NULL};

/* The par-spi-adapter's SPI controller structure.
 * Contains the function pointers that spi.library calls. */
struct par_spi_controller
{
    struct spi_controller ctrl; /* Must be first */
};

/* The par-spi-adapter also acts as a GPIO controller (for the
 * gpio pin) and as a cascaded interrupt controller (chaining the
 * parent CIA interrupt to child devices). */
struct par_spi_gpio
{
    struct gpio_controller gpio; /* Must be first */
    struct irq_chip irq_chip;    /* IRQ chip for the cascaded domain */
};

/* Per-instance state. Allocated at probe, stored in dev->driver_data,
 * and freed at shutdown. Keeping this on the heap (rather than in
 * file-scope statics) lets multiple adapter instances coexist — each
 * probe gets its own private state. */
struct par_spi_instance
{
    struct parallel_port_controller *pp_ctrl; /* Parent parallel port controller */
    struct par_spi_controller spi;            /* SPI controller (embeds spi_controller) */
    struct par_spi_gpio gpio_irq;             /* GPIO + IRQ chip */
    struct irq_domain *irq_domain;            /* Child domain (cascaded) */
    int parent_virq;                          /* Parent virq we chain from */
    int current_speed;                        /* 0 = slow, 1 = fast */
    struct device *spi_dev;                   /* This device */
};

/* ------------------------------------------------------------------ */
/* Parallel-port-to-SPI protocol implementation                        */
/* ------------------------------------------------------------------ */

#define REQ_BIT PP_CTRLB_SELECT
#define CLK_BIT PP_CTRLB_PAPEROUT
#define ACT_BIT PP_CTRLB_BUSY

#define REQ_MASK (1 << REQ_BIT)
#define CLK_MASK (1 << CLK_BIT)
#define ACT_MASK (1 << ACT_BIT)

/* The adapter hardware supports only two speeds. The requested Hz
 * value from set_speed is mapped to the nearest mode internally. */
#define PAR_SPI_SLOW_HZ 250000  /* Slow mode: ~250 kHz (with delays) */
#define PAR_SPI_FAST_HZ 3000000 /* Fast mode: ~3 MHz (no delays) */

static int wait_until_active(struct par_spi_instance *inst)
{
    struct parallel_port_controller *pp = inst->pp_ctrl;
    int count = 32;
    uint8_t ctrl = pp->ops->read_ctrl(pp);
    while (count > 0 && (ctrl & ACT_MASK))
    {
        count--;
        ctrl = pp->ops->read_ctrl(pp);
    }
    return count;
}

void par_spi_select(struct spi_controller *ctrl __asm("a0"), uint16_t cs __asm("d0"))
{
    struct par_spi_instance *inst = ctrl->private;
    struct parallel_port_controller *pp = inst->pp_ctrl;
    (void)cs;

    pp->ops->write_data(pp, 0xc1);

    uint8_t prev = pp->ops->read_ctrl(pp);
    pp->ops->write_ctrl(pp, prev & ~REQ_MASK);

    wait_until_active(inst);

    pp->ops->write_ctrl(pp, prev);
}

void par_spi_deselect(struct spi_controller *ctrl __asm("a0"), uint16_t cs __asm("d0"))
{
    struct par_spi_instance *inst = ctrl->private;
    struct parallel_port_controller *pp = inst->pp_ctrl;
    (void)cs;

    pp->ops->write_data(pp, 0xc0);

    uint8_t prev = pp->ops->read_ctrl(pp);
    pp->ops->write_ctrl(pp, prev & ~REQ_MASK);

    wait_until_active(inst);

    pp->ops->write_ctrl(pp, prev);
}

/* Read the adapter's single GPIO pin (card-detect). Used both during
 * init (before the GPIO controller is registered) and as the
 * gpio_controller::get_value callback. */
static int par_spi_read_gpio_pin(struct par_spi_instance *inst)
{
    struct parallel_port_controller *pp = inst->pp_ctrl;

    pp->ops->write_data(pp, 0xc2);

    uint8_t pra = pp->ops->read_ctrl(pp);
    pra &= ~REQ_MASK;
    pp->ops->write_ctrl(pp, pra);

    if (!wait_until_active(inst))
    {
        pra |= REQ_MASK;
        pp->ops->write_ctrl(pp, pra);
        return -1;
    }

    pp->ops->write_data_dir(pp, 0x00);

    pra ^= CLK_MASK;
    pp->ops->write_ctrl(pp, pra);

    int value = pp->ops->read_data(pp) & 1;

    pra |= REQ_MASK;
    pp->ops->write_ctrl(pp, pra);

    pp->ops->write_data_dir(pp, 0xff);

    return value;
}

int par_spi_gpio_get_value(struct gpio_controller *ctrl __asm("a0"), uint16_t pin __asm("d0"))
{
    struct par_spi_instance *inst = ctrl->private;

    /* The adapter exposes a single GPIO pin (0). The subsystem
     * validates the pin range via ngpio, but guard here too. */
    if (pin != 0)
        return -1;

    return par_spi_read_gpio_pin(inst);
}

static int par_spi_gpio_get_direction(struct gpio_controller *ctrl __asm("a0"), uint16_t pin __asm("d0"))
{
    (void)ctrl;
    (void)pin;
    /* The adapter pin is permanently input. */
    return GPIO_LINE_DIRECTION_IN;
}

void par_spi_set_speed(struct spi_controller *ctrl __asm("a0"), uint32_t speed __asm("d0"))
{
    struct par_spi_instance *inst = ctrl->private;
    struct parallel_port_controller *pp = inst->pp_ctrl;

    /* The adapter has only two hardware speeds. Use fast mode only
     * when the requested speed is at least PAR_SPI_FAST_HZ; otherwise
     * fall back to slow mode so we never exceed the requested clock. */
    int fast = (speed >= PAR_SPI_FAST_HZ);

    pp->ops->write_data(pp, fast ? 0xc5 : 0xc4);

    uint8_t prev = pp->ops->read_ctrl(pp);
    pp->ops->write_ctrl(pp, prev & ~REQ_MASK);

    wait_until_active(inst);

    pp->ops->write_ctrl(pp, prev);

    inst->current_speed = fast;
}

/* A slow SPI transfer takes 32 us (8 bits times 4us (250kHz)).
 * An E-cycle is 1.4 us. */
static void wait_40_us(struct par_spi_instance *inst)
{
    struct parallel_port_controller *pp = inst->pp_ctrl;
    for (int i = 0; i < 32; i++)
        (void)pp->ops->read_ctrl(pp);
}

static void spi_write_slow(struct par_spi_instance *inst __asm("a1"), const uint8_t *buf __asm("a0"),
                           uint32_t size __asm("d0"))
{
    struct parallel_port_controller *pp = inst->pp_ctrl;
    uint8_t ctrl = pp->ops->read_ctrl(pp);

    if (size <= 64) /* WRITE1: 00xxxxxx */
    {
        pp->ops->write_data(pp, (size - 1) & 0x3f);

        ctrl &= ~REQ_MASK;
        pp->ops->write_ctrl(pp, ctrl);

        wait_until_active(inst);
    }
    else /* WRITE2: 10xxxxxx 0xxxxxxx */
    {
        pp->ops->write_data(pp, 0x80 | (((size - 1) >> 7) & 0x3f));

        ctrl &= ~REQ_MASK;
        pp->ops->write_ctrl(pp, ctrl);

        wait_until_active(inst);

        pp->ops->write_data(pp, (size - 1) & 0x7f);

        ctrl ^= CLK_MASK;
        pp->ops->write_ctrl(pp, ctrl);
    }

    for (int i = 0; i < size; i++)
    {
        pp->ops->write_data(pp, *buf++);

        ctrl ^= CLK_MASK;
        pp->ops->write_ctrl(pp, ctrl);

        wait_40_us(inst);
    }

    ctrl |= REQ_MASK;
    pp->ops->write_ctrl(pp, ctrl);
}

static void spi_read_slow(struct par_spi_instance *inst __asm("a1"), uint8_t *buf __asm("a0"),
                          uint32_t size __asm("d0"))
{
    struct parallel_port_controller *pp = inst->pp_ctrl;
    uint8_t ctrl = pp->ops->read_ctrl(pp);

    if (size <= 64) /* READ1: 01xxxxxx */
    {
        pp->ops->write_data(pp, 0x40 | ((size - 1) & 0x3f));

        ctrl &= ~REQ_MASK;
        pp->ops->write_ctrl(pp, ctrl);

        wait_until_active(inst);
    }
    else /* READ2: 10xxxxxx 1xxxxxxx */
    {
        pp->ops->write_data(pp, 0x80 | (((size - 1) >> 7) & 0x3f));

        ctrl &= ~REQ_MASK;
        pp->ops->write_ctrl(pp, ctrl);

        wait_until_active(inst);

        pp->ops->write_data(pp, 0x80 | ((size - 1) & 0x7f));

        ctrl ^= CLK_MASK;
        pp->ops->write_ctrl(pp, ctrl);
    }

    pp->ops->write_data_dir(pp, 0);

    for (int i = 0; i < size; i++)
    {
        wait_40_us(inst);

        ctrl ^= CLK_MASK;
        pp->ops->write_ctrl(pp, ctrl);

        *buf++ = pp->ops->read_data(pp);
    }

    ctrl |= REQ_MASK;
    pp->ops->write_ctrl(pp, ctrl);

    pp->ops->write_data_dir(pp, 0xff);
}

/* ------------------------------------------------------------------ */
/* Fast-path SPI transfers (C implementation, replaces assembly)      */
/* ------------------------------------------------------------------ */

/* The fast protocol uses the same command framing as the slow path
 * but skips the 40us inter-bit delay. The adapter hardware latches
 * data on the CLK edge, so we toggle CLK and write/read as fast as
 * the CPU can cycle. */

static void spi_write_fast(struct par_spi_instance *inst __asm("a1"), const uint8_t *buf __asm("a0"),
                           uint32_t size __asm("d0"))
{
    struct parallel_port_controller *pp = inst->pp_ctrl;
    uint8_t ctrl = pp->ops->read_ctrl(pp);

    if (size <= 64) /* WRITE1: 00xxxxxx */
    {
        pp->ops->write_data(pp, (size - 1) & 0x3f);

        ctrl &= ~REQ_MASK;
        pp->ops->write_ctrl(pp, ctrl);

        wait_until_active(inst);
    }
    else /* WRITE2: 10xxxxxx 0xxxxxxx */
    {
        pp->ops->write_data(pp, 0x80 | (((size - 1) >> 7) & 0x3f));

        ctrl &= ~REQ_MASK;
        pp->ops->write_ctrl(pp, ctrl);

        wait_until_active(inst);

        pp->ops->write_data(pp, (size - 1) & 0x7f);

        ctrl ^= CLK_MASK;
        pp->ops->write_ctrl(pp, ctrl);
    }

    pp->ops->write_bytes_2e(pp, CLK_MASK, buf, (int16_t)size);

    ctrl = pp->ops->read_ctrl(pp);
    ctrl |= REQ_MASK;
    pp->ops->write_ctrl(pp, ctrl);
}

static void spi_read_fast(struct par_spi_instance *inst __asm("a1"), uint8_t *buf __asm("a0"),
                          uint32_t size __asm("d0"))
{
    struct parallel_port_controller *pp = inst->pp_ctrl;
    uint8_t ctrl = pp->ops->read_ctrl(pp);

    if (size <= 64) /* READ1: 01xxxxxx */
    {
        pp->ops->write_data(pp, 0x40 | ((size - 1) & 0x3f));

        ctrl &= ~REQ_MASK;
        pp->ops->write_ctrl(pp, ctrl);

        wait_until_active(inst);
    }
    else /* READ2: 10xxxxxx 1xxxxxxx */
    {
        pp->ops->write_data(pp, 0x80 | (((size - 1) >> 7) & 0x3f));

        ctrl &= ~REQ_MASK;
        pp->ops->write_ctrl(pp, ctrl);

        wait_until_active(inst);

        pp->ops->write_data(pp, 0x80 | ((size - 1) & 0x7f));

        ctrl ^= CLK_MASK;
        pp->ops->write_ctrl(pp, ctrl);
    }

    pp->ops->write_data_dir(pp, 0);

    pp->ops->read_bytes_2e(pp, CLK_MASK, buf, (int16_t)size);

    ctrl = pp->ops->read_ctrl(pp);
    ctrl |= REQ_MASK;
    pp->ops->write_ctrl(pp, ctrl);

    pp->ops->write_data_dir(pp, 0xff);
}

void par_spi_transfer(struct spi_controller *ctrl __asm("a0"), const uint8_t *write_buf __asm("a1"),
                      uint8_t *read_buf __asm("a2"), uint32_t size __asm("d0"))
{
    struct par_spi_instance *inst = ctrl->private;
    DBG_PAR_SPI("PAR-SPI: transfer size=%lu wr=%ld rd=%ld\n", size, (int32_t)(write_buf != NULL),
                (int32_t)(read_buf != NULL));

    if (write_buf && !read_buf)
    {
        /* Write only */
        if (inst->current_speed)
            spi_write_fast(inst, write_buf, size);
        else
            spi_write_slow(inst, write_buf, size);
    }
    else if (read_buf && !write_buf)
    {
        /* Read only */
        if (inst->current_speed)
            spi_read_fast(inst, read_buf, size);
        else
            spi_read_slow(inst, read_buf, size);
    }
    else if (write_buf && read_buf)
    {
        /* Full-duplex: our hardware is half-duplex, so write then read.
         * This shouldn't normally happen for SPI devices. */
        spi_write_slow(inst, write_buf, size);
        spi_read_slow(inst, read_buf, size);
    }
}

int par_spi_initialize(struct device *dev, struct par_spi_instance *inst)
{
    int success = 0;
    inst->spi_dev = dev;

    /* Open parallelport.library for controller lookup and interrupts.
     * The parent controller driver is responsible for claiming
     * the parallel port hardware. */
    PPBase = (struct ParallelPortBase *)OpenLibrary("parallelport.library", 0);
    if (!PPBase)
        return -1;

    /* Find our parent parallel port controller.
     * All hardware access goes through its ops table. */
    inst->pp_ctrl = PP_FindController(PPBase, dev->parent);
    if (!inst->pp_ctrl)
    {
        success = -4;
        goto fail_out1;
    }

    struct parallel_port_controller *pp = inst->pp_ctrl;

    pp->ops->write_ctrl(pp, (pp->ops->read_ctrl(pp) & ~ACT_MASK) | (REQ_MASK | CLK_MASK));
    pp->ops->write_ctrl_dir(pp, (pp->ops->read_ctrl_dir(pp) & ~ACT_MASK) | (REQ_MASK | CLK_MASK));

    pp->ops->write_data(pp, 0xff);
    pp->ops->write_data_dir(pp, 0xff);

    /* Check that the adapter hardware is present */
    int gpio_value = par_spi_read_gpio_pin(inst);
    if (gpio_value < 0)
    {
        success = -6;
        goto fail_out4;
    }

    return gpio_value;

fail_out4:
    pp->ops->write_ctrl_dir(pp, pp->ops->read_ctrl_dir(pp) & ~(ACT_MASK | REQ_MASK | CLK_MASK));
    pp->ops->write_data_dir(pp, 0);

fail_out1:
    CloseLibrary((struct Library *)PPBase);
    PPBase = NULL;
    return success;
}

void par_spi_shutdown(struct device *dev, struct par_spi_instance *inst)
{
    (void)dev;

    /* The chained handler and parent virq are cleaned up by
     * GPIO_UnregisterController (called from par_spi_drv_shutdown),
     * which now owns the auto-created irq_domain teardown. */

    if (inst->pp_ctrl)
    {
        struct parallel_port_controller *pp = inst->pp_ctrl;
        pp->ops->write_ctrl_dir(pp, pp->ops->read_ctrl_dir(pp) & ~(ACT_MASK | REQ_MASK | CLK_MASK));
        pp->ops->write_data_dir(pp, 0);
    }

    if (PPBase)
    {
        CloseLibrary((struct Library *)PPBase);
        PPBase = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Cascaded IRQ controller (chains parent interrupt to child devices) */
/* ------------------------------------------------------------------ */

/* The par-spi chained handler. Called by the parent domain's flow
 * handler when the CIA interrupt fires. The CIA auto-clears the
 * interrupt, so we just dispatch the child virq — no ack or register
 * check needed. The gpio_controller is passed as the chained_data;
 * we recover the instance from ctrl->private. */
static void par_spi_chained_handler(void *data __asm("a0"))
{
    struct gpio_controller *ctrl = data;
    struct par_spi_instance *inst = ctrl->private;

    /* Dispatch the child virq (hwirq 0 in our domain). */
    int child_virq = inst->irq_domain->revmap[0];
    if (child_virq >= 0)
        IRQ_GenericHandleIrq(DDMBase, child_virq);
}

static void par_spi_irq_mask(struct irq_chip *chip __asm("a0"), uint32_t hwirq __asm("d0"))
{
    (void)chip;
    (void)hwirq;
    /* No per-line mask; the CIA line is shared. */
}

static void par_spi_irq_unmask(struct irq_chip *chip __asm("a0"), uint32_t hwirq __asm("d0"))
{
    (void)chip;
    (void)hwirq;
    /* See mask. */
}

static int32_t par_spi_irq_set_type(struct irq_chip *chip __asm("a0"), uint32_t hwirq __asm("d0"),
                                    uint32_t trigger_type __asm("d1"))
{
    (void)chip;
    (void)hwirq;
    /* We cascade a fixed edge interrupt from the parent. The AVR
     * detects both edges of the card-present signal and drives the
     * IRQ line low for each, so we accept any edge trigger type.
     * Reject level-triggered types — the hardware is edge-only. */
    if (trigger_type != IRQ_TYPE_NONE && trigger_type != IRQ_TYPE_EDGE_RISING &&
        trigger_type != IRQ_TYPE_EDGE_FALLING && trigger_type != IRQ_TYPE_EDGE_BOTH)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* GPIO + IRQ controller setup                                        */
/* ------------------------------------------------------------------ */

int par_spi_gpio_init(struct device *dev, struct par_spi_instance *inst)
{
    /* Set up the GPIO controller.
     * The adapter exposes a single GPIO pin (pin 0) that is
     * input-only: it can be read but not driven. We declare this
     * via ngpio and supported_modes so the subsystem rejects
     * out-of-range pins and unsupported direction/output calls.
     * The direction_input callback is left NULL — the subsystem
     * treats a NULL callback for a declared GPIO_MODE_INPUT as a
     * no-op success (the pin is permanently input). */
    inst->gpio_irq.gpio.dev = dev;
    inst->gpio_irq.gpio.get_value = par_spi_gpio_get_value;
    inst->gpio_irq.gpio.set_value = NULL;
    inst->gpio_irq.gpio.get_direction = par_spi_gpio_get_direction;
    inst->gpio_irq.gpio.direction_input = NULL;
    inst->gpio_irq.gpio.direction_output = NULL;
    inst->gpio_irq.gpio.ngpio = 1;
    inst->gpio_irq.gpio.supported_modes = 0;
    inst->gpio_irq.gpio.private = inst;

    /* Set up the cascaded IRQ chip. The par-spi adapter has a single
     * child interrupt (hwirq 0 = card-detect changed). Setting
     * irq_chip and chained_handler on the gpio_controller lets
     * GPIO_RegisterController auto-create the hierarchical domain
     * and set the chained handler on the parent virq. */
    inst->gpio_irq.irq_chip.name = "par-spi";
    inst->gpio_irq.irq_chip.chip_data = NULL;
    inst->gpio_irq.irq_chip.startup = NULL;
    inst->gpio_irq.irq_chip.shutdown = NULL;
    inst->gpio_irq.irq_chip.mask = par_spi_irq_mask;
    inst->gpio_irq.irq_chip.unmask = par_spi_irq_unmask;
    inst->gpio_irq.irq_chip.ack = NULL;
    inst->gpio_irq.irq_chip.eoi = NULL;
    inst->gpio_irq.irq_chip.enable = NULL;
    inst->gpio_irq.irq_chip.disable = NULL;
    inst->gpio_irq.irq_chip.set_type = par_spi_irq_set_type;

    /* Point the gpio_controller at the irq_chip and chained handler.
     * GPIO_RegisterController will resolve the parent interrupt,
     * create the hierarchy, and set the chained handler. We use a
     * custom chained handler (par_spi_chained_handler) for clarity,
     * though the framework passthrough would also work since the
     * CIA auto-clears and there is no status register. */
    inst->gpio_irq.gpio.irq_chip = &inst->gpio_irq.irq_chip;
    inst->gpio_irq.gpio.chained_handler = par_spi_chained_handler;

    /* Register the GPIO controller with the core. This also
     * creates the irq_domain and sets the chained handler. */
    GPIO_RegisterController(DDMBase, &inst->gpio_irq.gpio);

    /* The domain was created by GPIO_RegisterController. Save it
     * for shutdown. */
    inst->irq_domain = dev->irq_domain;
    inst->parent_virq = inst->gpio_irq.gpio.parent_virq;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Driver interface LVOs                                               */
/* ------------------------------------------------------------------ */

/* LVO -42: DDMDrv_Probe - initialize the adapter and register as SPI
 * controller. The matching engine calls this once after a compatible
 * match; it does both check and init. Returns 0 on success. */
int32_t par_spi_probe(struct Library *lib __asm("a6"), struct device *dev __asm("a0"))
{
    (void)lib;

    DBG_PAR_SPI("PAR-SPI: probe dev='%s'\n", dev->node.ln_Name ? dev->node.ln_Name : "(unnamed)");

    /* Allocate per-instance state */
    struct par_spi_instance *inst = AllocMem(sizeof(*inst), MEMF_ANY | MEMF_CLEAR);
    if (!inst)
        return -1;

    dev->driver_data = inst;

    /* Initialize the parallel port SPI protocol */
    int res = par_spi_initialize(dev, inst);
    if (res < 0)
    {
        DBG_PAR_SPI("PAR-SPI: ERR: initialize failed code=%ld\n", (long)res);
        goto fail_free;
    }

    /* Set up the GPIO + IRQ controller (cascades parent interrupt) */
    if (par_spi_gpio_init(dev, inst) < 0)
        goto fail_shutdown;

    /* Set up the SPI controller */
    inst->spi.ctrl.dev = dev;
    inst->spi.ctrl.set_speed = par_spi_set_speed;
    inst->spi.ctrl.select = par_spi_select;
    inst->spi.ctrl.deselect = par_spi_deselect;
    inst->spi.ctrl.transfer = par_spi_transfer;
    inst->spi.ctrl.private = inst;

    /* Register with spi.library. SPI_RegisterController enumerates
     * the controller's device-tree children and matches them inline. */
    if (SpiBase)
        SPI_RegisterController(SpiBase, &inst->spi.ctrl);

    DBG_PAR_SPI("PAR-SPI: probe ok\n");
    return 0;

fail_shutdown:
    par_spi_shutdown(dev, inst);
fail_free:
    DBG_PAR_SPI("PAR-SPI: ERR: probe failed\n");
    FreeMem(inst, sizeof(*inst));
    dev->driver_data = NULL;
    return -1;
}

/* LVO -48: DDMDrv_Remove */
void par_spi_remove(struct Library *lib __asm("a6"), struct device *dev __asm("a0"))
{
    (void)lib;
    (void)dev;
}

/* LVO -54: DDMDrv_Init (reserved — merged into Probe) */
int32_t par_spi_init(struct Library *lib __asm("a6"), struct device *dev __asm("a0"))
{
    (void)lib;
    (void)dev;
    return 0;
}

/* LVO -60: DDMDrv_Shutdown */
void par_spi_drv_shutdown(struct Library *lib __asm("a6"), struct device *dev __asm("a0"))
{
    (void)lib;

    struct par_spi_instance *inst = dev->driver_data;
    if (!inst)
        return;

    /* Unregister from spi.library */
    if (SpiBase)
        SPI_UnregisterController(SpiBase, &inst->spi.ctrl);

    /* Unregister from the core. This also tears down the
     * auto-created irq_domain (disposes child mappings, removes
     * handlers, clears the chained handler on the parent virq,
     * and disposes the parent virq mapping). */
    GPIO_UnregisterController(DDMBase, &inst->gpio_irq.gpio);

    par_spi_shutdown(dev, inst);

    FreeMem(inst, sizeof(*inst));
    dev->driver_data = NULL;
}

/* LVO -66: DDMDrv_Enumerate - no-op. Child enumeration is now owned by
 * spi.library (SPI_RegisterController), which creates spi_device structs
 * for device-tree children and matches them inline. */
int32_t par_spi_enumerate(struct Library *lib __asm("a6"), struct device *bus __asm("a0"))
{
    (void)lib;
    (void)bus;
    return 0;
}

/* LVO -72: DDMDrv_GetCompatible */
const char **par_spi_get_compatible(struct Library *lib __asm("a6"))
{
    (void)lib;
    return par_spi_compatible;
}

/* ------------------------------------------------------------------ */
/* Library init / open / close / expunge                              */
/* ------------------------------------------------------------------ */

static struct device_driver par_spi_driver;

static BPTR par_spi_lib_expunge(struct Library *lib __asm("a6"));

static struct Library *par_spi_lib_open(struct Library *lib __asm("a6"))
{
    lib->lib_OpenCnt++;
    return lib;
}

static BPTR par_spi_lib_close(struct Library *lib __asm("a6"))
{
    lib->lib_OpenCnt--;
    if (lib->lib_OpenCnt == 0 && (lib->lib_Flags & LIBF_DELEXP))
        return par_spi_lib_expunge(lib);
    return 0;
}

static BPTR par_spi_lib_expunge(struct Library *lib __asm("a6"))
{
    if (lib->lib_OpenCnt != 0)
    {
        lib->lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    if (DDMBase)
        DDM_UnregisterDriver(DDMBase, &par_spi_driver);
    if (SpiBase)
        CloseLibrary((struct Library *)SpiBase);
    if (DDMBase)
        CloseLibrary((struct Library *)DDMBase);
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

static struct Library *par_spi_lib_init(struct ExecBase *sys_base __asm("a6"), BPTR seg_list __asm("a0"),
                                        struct Library *lib __asm("d0"))
{
    SysBase = sys_base;
    lib->lib_Node.ln_Type = NT_LIBRARY;
    lib->lib_Node.ln_Name = "par-spi.library";
    lib->lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    lib->lib_Version = 1;
    lib->lib_Revision = 0;
    lib->lib_IdString = (void *)"par-spi-adapter driver 1.0 (August 2026)\n\r";
    lib->lib_OpenCnt = 0;
    saved_seg_list = seg_list;

    LibBase = lib;

    /* Open ddm.library (for DDM_ functions) */
    DDMBase = (struct DDMBase *)OpenLibrary("ddm.library", 0);
    if (!DDMBase)
        return NULL;

    SpiBase = (struct SpiBase *)OpenLibrary("spi.library", 0);
    if (!SpiBase)
    {
        CloseLibrary((struct Library *)DDMBase);
        DDMBase = NULL;
        return NULL;
    }

    /* Register the driver */
    par_spi_driver.node.ln_Name = "par-spi-adapter";
    par_spi_driver.compatible = par_spi_compatible;
    par_spi_driver.lib_base = lib;
    par_spi_driver.bus_type = BUS_TYPE_PARALLEL_PORT;

    if (!DDM_RegisterDriver(DDMBase, &par_spi_driver))
        return NULL;

    return lib;
}

static uint32_t library_vectors[] = {
    (uint32_t)par_spi_lib_open,       /* -6 */
    (uint32_t)par_spi_lib_close,      /* -12 */
    (uint32_t)par_spi_lib_expunge,    /* -18 */
    (uint32_t)noexec,                 /* -24 */
    (uint32_t)noexec,                 /* -30 */
    (uint32_t)noexec,                 /* -36 */
    (uint32_t)par_spi_probe,          /* -42 */
    (uint32_t)par_spi_remove,         /* -48 */
    (uint32_t)noexec,                 /* -54 (reserved) */
    (uint32_t)par_spi_drv_shutdown,   /* -60 */
    (uint32_t)par_spi_enumerate,      /* -66 */
    (uint32_t)par_spi_get_compatible, /* -72 */
    -1,
};

uint32_t auto_init_tables[] = {
    sizeof(struct Library),
    (uint32_t)library_vectors,
    0,
    (uint32_t)par_spi_lib_init,
};

const struct Resident romtag = {
    .rt_MatchWord = RTC_MATCHWORD,
    .rt_MatchTag = (void *)&romtag,
    .rt_EndSkip = (void *)(&romtag + 1),
    .rt_Flags = RTF_AUTOINIT,
    .rt_Version = 1,
    .rt_Type = NT_LIBRARY,
    .rt_Pri = 0,
    .rt_Name = "par-spi.library",
    .rt_IdString = "par-spi-adapter driver 1.0 (August 2026)\n\r",
    .rt_Init = auto_init_tables,
};
