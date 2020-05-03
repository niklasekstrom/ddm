/*
 * SPIder adapter device tree driver
 *
 * Implements the DDM interface for the SPIder clockport-to-
 * SPI adapter. On probe, it probes the SPIder firmware via the IDENT
 * register, then registers as an SPI controller with spi.library, a
 * GPIO controller with ddm.library, and an IRQ controller with
 * ddm.library (cascading the parent clockport interrupt).
 *
 * The SPIder FIFO protocol implementation is included here directly
 * (ported from spi-lib-spider/spi.c).
 */
#include <dos/dos.h>
#include <exec/interrupts.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/exec.h>

#include "ddm_debug.h"

#include "clockport.h"
#include "ddm.h"
#include "ddm_driver.h"
#include "ddm_protos.h"
#include "devicetree.h" /* struct device, BUS_TYPE_* */
#include "gpio.h"
#include "irq.h" /* IRQ_AddHandler, etc. */
#include "spi.h"

struct ExecBase *SysBase = NULL;
static struct DDMBase *DDMBase = NULL;

static struct SpiBase *SpiBase = NULL;
static struct ClockportBase *CPBase = NULL;
static struct Library *LibBase = NULL;
static BPTR saved_seg_list;

static const char *spider_compatible[] = {"amiga,spider", NULL};

/* ------------------------------------------------------------------ */
/* SPIder register definitions (from Firmware/protocol.h)              */
/* ------------------------------------------------------------------ */

#define REG_STATUS 0 /* RO */
#define REG_RESERVED_1 1
#define REG_UPPER_LENGTH 2  /* WO, Upper byte for lengths */
#define REG_CARD_DETECT 3   /* RO, Read CD */
#define REG_RX_HEAD 4       /* RO */
#define REG_RX_TAIL 5       /* RO */
#define REG_TX_HEAD 6       /* RO */
#define REG_TX_TAIL 7       /* RO */
#define REG_RX_DISCARD 8    /* WO, Lower byte */
#define REG_TX_FEED 9       /* WO, Lower byte */
#define REG_SPI_FREQ 10     /* WO, Set SPI frequency */
#define REG_SLAVE_SELECT 11 /* WO, Write SS */
#define REG_INT_FIRED 12    /* RW */
#define REG_INT_ARMED 13    /* WO */
#define REG_FIFO 14         /* RW, Write to TX, Read from RX */
#define REG_IDENT 15        /* RO */

#define STATUS_RX_DISCARD_EMPTY 0x01

#define IRQ_CD_CHANGED 1

#define TEN_KHZ 10000
#define ONE_MHZ 1000000

#define IDENT_SIZE 8

static const uint8_t ident_str[] = {0xff, 's', 'p', 'd', 'r'};

/* The spider's SPI controller structure. */
struct spider_controller
{
    struct spi_controller ctrl; /* Must be first */
};

/* The spider also acts as a GPIO controller (for the card-detect pin)
 * and as a cascaded interrupt controller (chaining the parent
 * clockport interrupt to child devices). */
struct spider_gpio
{
    struct gpio_controller gpio; /* Must be first */
    struct irq_chip irq_chip;    /* IRQ chip for the cascaded domain */
};

/* ------------------------------------------------------------------ */
/* Per-instance state. Allocated at probe, stored in dev->driver_data,
 * and freed at shutdown. Keeping this on the heap (rather than in
 * file-scope statics) lets multiple SPIder instances coexist — each
 * probe gets its own private state. */
struct spider_instance
{
    struct clockport_controller *cp_ctrl; /* Parent clockport controller */
    struct spider_controller spi;         /* SPI controller (embeds spi_controller) */
    struct spider_gpio gpio_irq;          /* GPIO + IRQ chip */
    struct irq_domain *irq_domain;        /* Child domain (cascaded) */
    int parent_virq;                      /* Parent virq we chain from */
};

/* ------------------------------------------------------------------ */
/* SPIder FIFO protocol implementation                                 */
/* ------------------------------------------------------------------ */

/* Convenience wrappers for register access through the clockport ops. */
static inline uint8_t cp_read(struct spider_instance *inst, uint16_t reg)
{
    return inst->cp_ctrl->ops->read_reg(inst->cp_ctrl, reg);
}

static inline void cp_write(struct spider_instance *inst, uint16_t reg, uint8_t val)
{
    inst->cp_ctrl->ops->write_reg(inst->cp_ctrl, reg, val);
}

void spider_select(struct spi_controller *ctrl __asm("a0"), uint16_t cs __asm("d0"))
{
    struct spider_instance *inst = ctrl->private;
    (void)cs;
    cp_write(inst, REG_SLAVE_SELECT, 1);
}

void spider_deselect(struct spi_controller *ctrl __asm("a0"), uint16_t cs __asm("d0"))
{
    struct spider_instance *inst = ctrl->private;
    (void)cs;
    cp_write(inst, REG_SLAVE_SELECT, 0);
}

int spider_gpio_get_value(struct gpio_controller *ctrl __asm("a0"), uint16_t pin __asm("d0"))
{
    struct spider_instance *inst = ctrl->private;

    /* The SPIder exposes a single GPIO pin (0) = card detect. */
    if (pin != 0)
        return -1;

    int present = cp_read(inst, REG_CARD_DETECT);

    /* Re-enable the CD changed interrupt. */
    cp_write(inst, REG_INT_ARMED, IRQ_CD_CHANGED);

    return present;
}

static int spider_gpio_get_direction(struct gpio_controller *ctrl __asm("a0"), uint16_t pin __asm("d0"))
{
    (void)ctrl;
    (void)pin;
    /* The SPIder pin is permanently input. */
    return GPIO_LINE_DIRECTION_IN;
}

void spider_set_speed(struct spi_controller *ctrl __asm("a0"), uint32_t speed __asm("d0"))
{
    struct spider_instance *inst = ctrl->private;

    /* SPI frequency encoding: val < 128 → val*10kHz, val >= 128 →
     * (val-128)*1MHz. Find the highest value that does not exceed
     * the requested speed. */
    uint8_t freq;

    if (speed >= ONE_MHZ)
    {
        uint32_t mhz = speed / ONE_MHZ;
        if (mhz > 127)
            mhz = 127;
        freq = (uint8_t)(128 + mhz);
    }
    else
    {
        uint32_t tenkhz = speed / TEN_KHZ;
        if (tenkhz > 127)
            tenkhz = 127;
        freq = (uint8_t)tenkhz;
    }

    cp_write(inst, REG_SPI_FREQ, freq);
}

/* Read size bytes from the SPIder RX FIFO. Ported from spi-lib-spider. */
static void spider_spi_read(struct spider_instance *inst, uint8_t *buf, uint16_t size)
{
    cp_write(inst, REG_UPPER_LENGTH, (size >> 8) & 0xff);
    cp_write(inst, REG_TX_FEED, size & 0xff);

    uint8_t rx_head = cp_read(inst, REG_RX_HEAD);
    uint8_t rx_tail;

    if (size == 1)
    {
        do
        {
            rx_tail = cp_read(inst, REG_RX_TAIL);
        } while (rx_head == rx_tail);

        buf[0] = cp_read(inst, REG_FIFO);
    }
    else
    {
        do
        {
            rx_tail = cp_read(inst, REG_RX_TAIL);

            uint8_t bytes_in_rx = rx_tail - rx_head;

            if (bytes_in_rx)
            {
                inst->cp_ctrl->ops->read_reg_bytes(inst->cp_ctrl, REG_FIFO, buf, bytes_in_rx);

                buf += bytes_in_rx;
                rx_head += bytes_in_rx;
                size -= bytes_in_rx;
            }
        } while (size);
    }
}

/* Write size bytes to the SPIder TX FIFO. Ported from spi-lib-spider. */
static void spider_spi_write(struct spider_instance *inst, const uint8_t *buf, uint16_t size)
{
    cp_write(inst, REG_UPPER_LENGTH, (size >> 8) & 0xff);
    cp_write(inst, REG_RX_DISCARD, size & 0xff);

    uint8_t tx_head;
    uint8_t tx_tail = cp_read(inst, REG_TX_TAIL);

    if (size == 1)
    {
        uint8_t next_tx_tail = tx_tail + 1;
        do
        {
            tx_head = cp_read(inst, REG_TX_HEAD);
        } while (next_tx_tail == tx_head);

        cp_write(inst, REG_FIFO, buf[0]);
    }
    else
    {
        do
        {
            tx_head = cp_read(inst, REG_TX_HEAD);

            uint8_t bytes_in_tx = tx_tail - tx_head;
            uint8_t free_space = 255 - bytes_in_tx;

            if (free_space)
            {
                if (free_space > size)
                    free_space = size;

                inst->cp_ctrl->ops->write_reg_bytes(inst->cp_ctrl, REG_FIFO, buf, free_space);

                buf += free_space;
                tx_tail += free_space;
                size -= free_space;
            }
        } while (size);
    }

    while ((cp_read(inst, REG_STATUS) & STATUS_RX_DISCARD_EMPTY) == 0)
    {
        // Wait until rx_discard is empty.
    }
}

void spider_transfer(struct spi_controller *ctrl __asm("a0"), const uint8_t *write_buf __asm("a1"),
                     uint8_t *read_buf __asm("a2"), uint32_t size __asm("d0"))
{
    struct spider_instance *inst = ctrl->private;
    DBG_SPIDER("SPIDER: transfer size=%lu wr=%ld rd=%ld\n", size, (int32_t)(write_buf != NULL),
               (int32_t)(read_buf != NULL));

    if (write_buf && !read_buf)
    {
        /* Write only */
        spider_spi_write(inst, write_buf, (uint16_t)size);
    }
    else if (read_buf && !write_buf)
    {
        /* Read only */
        spider_spi_read(inst, read_buf, (uint16_t)size);
    }
    else if (write_buf && read_buf)
    {
        /* Full-duplex: SPIder is half-duplex, so write then read. */
        spider_spi_write(inst, write_buf, (uint16_t)size);
        spider_spi_read(inst, read_buf, (uint16_t)size);
    }
}

/* ------------------------------------------------------------------ */
/* Probe / initialize                                                  */
/* ------------------------------------------------------------------ */

/* Probe the SPIder firmware via the IDENT register. Returns 0 on
 * success (firmware found, major version 1), negative on failure. */
static int spider_probe_interface(struct spider_instance *inst)
{
    uint8_t read_bytes[IDENT_SIZE];

    for (short i = 0; i < IDENT_SIZE; i++)
        read_bytes[i] = cp_read(inst, REG_IDENT);

    int16_t found = FALSE;
    short pos = 0;

    for (short start = 0; start < IDENT_SIZE && !found; start++)
    {
        found = TRUE;
        pos = start;

        for (short i = 0; i < (short)sizeof(ident_str); i++)
        {
            if (read_bytes[pos] != ident_str[i])
            {
                found = FALSE;
                break;
            }
            pos = (pos + 1) & (IDENT_SIZE - 1);
        }
    }

    if (!found)
        return -1;

    uint8_t fw_major_ver = read_bytes[pos];

    /* Only major version 1 is currently supported. */
    if (fw_major_ver != 1)
        return -2;

    return 0;
}

int spider_initialize(struct device *dev, struct spider_instance *inst)
{
    /* Open clockport.library for controller lookup. */
    CPBase = (struct ClockportBase *)OpenLibrary("clockport.library", 0);
    if (!CPBase)
        return -1;

    /* Find our parent clockport controller.
     * All hardware access goes through its ops table. */
    inst->cp_ctrl = CP_FindController(CPBase, dev->parent);
    if (!inst->cp_ctrl)
    {
        CloseLibrary((struct Library *)CPBase);
        CPBase = NULL;
        return -4;
    }

    /* Probe the SPIder firmware */
    if (spider_probe_interface(inst) < 0)
    {
        CloseLibrary((struct Library *)CPBase);
        CPBase = NULL;
        inst->cp_ctrl = NULL;
        return -6;
    }

    /* Initialize the SPIder hardware */
    cp_write(inst, REG_SPI_FREQ, 40); /* 400 kHz default */
    cp_write(inst, REG_INT_ARMED, 0);
    cp_write(inst, REG_INT_FIRED, 0);

    return cp_read(inst, REG_CARD_DETECT);
}

void spider_shutdown(struct device *dev, struct spider_instance *inst)
{
    (void)dev;
    /* The chained handler and parent virq are cleaned up by
     * GPIO_UnregisterController (called from spider_drv_shutdown),
     * which now owns the auto-created irq_domain teardown. */

    if (inst->cp_ctrl)
    {
        cp_write(inst, REG_INT_ARMED, 0);
        cp_write(inst, REG_INT_FIRED, 0);
    }

    if (CPBase)
    {
        CloseLibrary((struct Library *)CPBase);
        CPBase = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Cascaded IRQ controller (chains parent interrupt to child devices) */
/* ------------------------------------------------------------------ */

/* The spider chained handler. Called by the parent domain's flow
 * handler when the clockport interrupt fires (via
 * IRQ_GenericHandleIrq on the parent virq, which invokes our chained
 * handler). We check REG_INT_FIRED, clear INT_ARMED + INT_FIRED, then
 * dispatch to the child virq via IRQ_GenericHandleIrq. The instance
 * is passed as the chained_data. */
static void spider_chained_handler(void *data __asm("a0"))
{
    struct gpio_controller *ctrl = data;
    struct spider_instance *inst = ctrl->private;

    uint8_t fired = cp_read(inst, REG_INT_FIRED);
    if (!fired)
        return;

    /* Clear the interrupt on the SPIder hardware */
    cp_write(inst, REG_INT_ARMED, 0);
    cp_write(inst, REG_INT_FIRED, 0);

    /* Dispatch the child virq (hwirq 0 in our domain). */
    int child_virq = inst->irq_domain->revmap[0];
    if (child_virq >= 0)
        IRQ_GenericHandleIrq(DDMBase, child_virq);
}

static void spider_irq_mask(struct irq_chip *chip __asm("a0"), uint32_t hwirq __asm("d0"))
{
    (void)chip;
    (void)hwirq;
    /* Masking is handled by arming/disarming the SPIder hardware,
     * which the child driver controls. No per-line mask here. */
}

static void spider_irq_unmask(struct irq_chip *chip __asm("a0"), uint32_t hwirq __asm("d0"))
{
    (void)chip;
    (void)hwirq;
    /* See mask. */
}

static int32_t spider_irq_set_type(struct irq_chip *chip __asm("a0"), uint32_t hwirq __asm("d0"),
                                   uint32_t trigger_type __asm("d1"))
{
    (void)chip;
    (void)hwirq;
    /* The SPIder hardware detects both edges of the card-present
     * signal and fires REG_INT_FIRED accordingly, so we present an
     * edge-both interrupt to our children (e.g. mmc-spi). Reject
     * level-triggered types. */
    if (trigger_type != IRQ_TYPE_NONE && trigger_type != IRQ_TYPE_EDGE_RISING &&
        trigger_type != IRQ_TYPE_EDGE_FALLING && trigger_type != IRQ_TYPE_EDGE_BOTH)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* GPIO + IRQ controller setup                                        */
/* ------------------------------------------------------------------ */

int spider_gpio_init(struct device *dev, struct spider_instance *inst)
{
    /* Set up the GPIO controller.
     * The SPIder exposes a single GPIO pin (pin 0) that is
     * input-only: card detect, read via REG_CARD_DETECT. The
     * instance is handed to the ops via gpio.private. */
    inst->gpio_irq.gpio.dev = dev;
    inst->gpio_irq.gpio.get_value = spider_gpio_get_value;
    inst->gpio_irq.gpio.set_value = NULL;
    inst->gpio_irq.gpio.get_direction = spider_gpio_get_direction;
    inst->gpio_irq.gpio.direction_input = NULL;
    inst->gpio_irq.gpio.direction_output = NULL;
    inst->gpio_irq.gpio.ngpio = 1;
    inst->gpio_irq.gpio.supported_modes = 0;
    inst->gpio_irq.gpio.private = inst;

    /* Set up the cascaded IRQ chip. The spider has a single child
     * interrupt (hwirq 0 = card-detect changed). Setting irq_chip
     * and chained_handler on the gpio_controller lets
     * GPIO_RegisterController auto-create the hierarchical domain
     * and set the chained handler on the parent virq. */
    inst->gpio_irq.irq_chip.name = "spider";
    inst->gpio_irq.irq_chip.chip_data = NULL;
    inst->gpio_irq.irq_chip.startup = NULL;
    inst->gpio_irq.irq_chip.shutdown = NULL;
    inst->gpio_irq.irq_chip.mask = spider_irq_mask;
    inst->gpio_irq.irq_chip.unmask = spider_irq_unmask;
    inst->gpio_irq.irq_chip.ack = NULL;
    inst->gpio_irq.irq_chip.eoi = NULL;
    inst->gpio_irq.irq_chip.enable = NULL;
    inst->gpio_irq.irq_chip.disable = NULL;
    inst->gpio_irq.irq_chip.set_type = spider_irq_set_type;

    /* Point the gpio_controller at the irq_chip and chained handler.
     * GPIO_RegisterController will resolve the parent interrupt,
     * create the hierarchy, and set the chained handler. */
    inst->gpio_irq.gpio.irq_chip = &inst->gpio_irq.irq_chip;
    inst->gpio_irq.gpio.chained_handler = spider_chained_handler;

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
int32_t spider_probe(struct Library *lib __asm("a6"), struct device *dev __asm("a0"))
{
    (void)lib;

    DBG_SPIDER("SPIDER: probe dev='%s'\n", dev->node.ln_Name ? dev->node.ln_Name : "(unnamed)");

    /* Allocate per-instance state so multiple SPIder nodes can
     * coexist. Stored in dev->driver_data and freed at shutdown. */
    struct spider_instance *inst =
        (struct spider_instance *)AllocMem(sizeof(struct spider_instance), MEMF_ANY | MEMF_CLEAR);
    if (!inst)
        return -1;
    dev->driver_data = inst;

    /* Initialize the SPIder hardware and probe the firmware */
    int res = spider_initialize(dev, inst);
    if (res < 0)
    {
        DBG_SPIDER("SPIDER: ERR: initialize failed code=%ld\n", (long)res);
        FreeMem(inst, sizeof(struct spider_instance));
        dev->driver_data = NULL;
        return -1;
    }

    /* Set up the GPIO + IRQ controller (cascades parent interrupt) */
    if (spider_gpio_init(dev, inst) < 0)
    {
        spider_shutdown(dev, inst);
        FreeMem(inst, sizeof(struct spider_instance));
        dev->driver_data = NULL;
        return -1;
    }

    /* Set up the SPI controller. The instance is handed to the ops
     * via ctrl->private. */
    inst->spi.ctrl.dev = dev;
    inst->spi.ctrl.set_speed = spider_set_speed;
    inst->spi.ctrl.select = spider_select;
    inst->spi.ctrl.deselect = spider_deselect;
    inst->spi.ctrl.transfer = spider_transfer;
    inst->spi.ctrl.private = inst;

    /* Register with spi.library. SPI_RegisterController enumerates
     * the controller's device-tree children and matches them inline. */
    if (SpiBase)
        SPI_RegisterController(SpiBase, &inst->spi.ctrl);

    DBG_SPIDER("SPIDER: probe ok\n");
    return 0;
}

/* LVO -48: DDMDrv_Remove */
void spider_remove(struct Library *lib __asm("a6"), struct device *dev __asm("a0"))
{
    (void)lib;
    (void)dev;
}

/* LVO -54: DDMDrv_Init (reserved — merged into Probe) */
int32_t spider_init(struct Library *lib __asm("a6"), struct device *dev __asm("a0"))
{
    (void)lib;
    (void)dev;
    return 0;
}

/* LVO -60: DDMDrv_Shutdown */
void spider_drv_shutdown(struct Library *lib __asm("a6"), struct device *dev __asm("a0"))
{
    (void)lib;
    struct spider_instance *inst = dev->driver_data;
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

    spider_shutdown(dev, inst);

    FreeMem(inst, sizeof(struct spider_instance));
    dev->driver_data = NULL;
}

/* LVO -66: DDMDrv_Enumerate - no-op. Child enumeration is now owned by
 * spi.library (SPI_RegisterController), which creates spi_device structs
 * for device-tree children and matches them inline. */
int32_t spider_enumerate(struct Library *lib __asm("a6"), struct device *bus __asm("a0"))
{
    (void)lib;
    (void)bus;
    return 0;
}

/* LVO -72: DDMDrv_GetCompatible */
const char **spider_get_compatible(struct Library *lib __asm("a6"))
{
    (void)lib;
    return spider_compatible;
}

/* ------------------------------------------------------------------ */
/* Library init / open / close / expunge                              */
/* ------------------------------------------------------------------ */

static struct device_driver spider_driver;

static BPTR spider_lib_expunge(struct Library *lib __asm("a6"));

static struct Library *spider_lib_open(struct Library *lib __asm("a6"))
{
    lib->lib_OpenCnt++;
    return lib;
}

static BPTR spider_lib_close(struct Library *lib __asm("a6"))
{
    lib->lib_OpenCnt--;
    if (lib->lib_OpenCnt == 0 && (lib->lib_Flags & LIBF_DELEXP))
        return spider_lib_expunge(lib);
    return 0;
}

static BPTR spider_lib_expunge(struct Library *lib __asm("a6"))
{
    if (lib->lib_OpenCnt != 0)
    {
        lib->lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    if (DDMBase)
        DDM_UnregisterDriver(DDMBase, &spider_driver);
    if (SpiBase)
        CloseLibrary((struct Library *)SpiBase);
    if (CPBase)
        CloseLibrary((struct Library *)CPBase);
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

static struct Library *spider_lib_init(struct ExecBase *sys_base __asm("a6"), BPTR seg_list __asm("a0"),
                                       struct Library *lib __asm("d0"))
{
    SysBase = sys_base;
    lib->lib_Node.ln_Type = NT_LIBRARY;
    lib->lib_Node.ln_Name = "spider.library";
    lib->lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    lib->lib_Version = 1;
    lib->lib_Revision = 0;
    lib->lib_IdString = (void *)"spider driver 1.0 (August 2026)\n\r";
    lib->lib_OpenCnt = 0;
    saved_seg_list = seg_list;

    LibBase = lib;

    /* Open ddm.library (for DDM_ functions) */
    DDMBase = (struct DDMBase *)OpenLibrary("ddm.library", 0);
    if (!DDMBase)
        return NULL;

    /* Open spi.library (for SPI_RegisterController) */
    SpiBase = (struct SpiBase *)OpenLibrary("spi.library", 0);
    if (!SpiBase)
    {
        CloseLibrary((struct Library *)DDMBase);
        DDMBase = NULL;
        return NULL;
    }

    /* Register the driver */
    spider_driver.node.ln_Name = "spider";
    spider_driver.compatible = spider_compatible;
    spider_driver.lib_base = lib;
    spider_driver.bus_type = BUS_TYPE_CLOCKPORT;

    if (!DDM_RegisterDriver(DDMBase, &spider_driver))
        return NULL;

    return lib;
}

static uint32_t library_vectors[] = {
    (uint32_t)spider_lib_open,       /* -6 */
    (uint32_t)spider_lib_close,      /* -12 */
    (uint32_t)spider_lib_expunge,    /* -18 */
    (uint32_t)noexec,                /* -24 */
    (uint32_t)noexec,                /* -30 */
    (uint32_t)noexec,                /* -36 */
    (uint32_t)spider_probe,          /* -42 */
    (uint32_t)spider_remove,         /* -48 */
    (uint32_t)noexec,                /* -54 (reserved) */
    (uint32_t)spider_drv_shutdown,   /* -60 */
    (uint32_t)spider_enumerate,      /* -66 */
    (uint32_t)spider_get_compatible, /* -72 */
    -1,
};

uint32_t auto_init_tables[] = {
    sizeof(struct Library),
    (uint32_t)library_vectors,
    0,
    (uint32_t)spider_lib_init,
};

const struct Resident romtag = {
    .rt_MatchWord = RTC_MATCHWORD,
    .rt_MatchTag = (void *)&romtag,
    .rt_EndSkip = (void *)(&romtag + 1),
    .rt_Flags = RTF_AUTOINIT,
    .rt_Version = 1,
    .rt_Type = NT_LIBRARY,
    .rt_Pri = 0,
    .rt_Name = "spider.library",
    .rt_IdString = "spider driver 1.0 (August 2026)\n\r",
    .rt_Init = auto_init_tables,
};
