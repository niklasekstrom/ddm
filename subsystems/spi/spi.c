#define SPI_INTERNAL
/*
 * SPI subsystem implementation - spi.library
 */
#include <clib/alib_protos.h> /* NewList */
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <stddef.h> /* offsetof */

#include "ddm_debug.h"

#include "ddm.h"
#include "ddm_protos.h"
#include "ddm_util.h"
#include "devicetree.h" /* DT_GetPropertyU32 */
#include "spi.h"

struct ExecBase *SysBase = NULL;
static struct SpiBase *SpiBase = NULL;

/* ------------------------------------------------------------------ */
/* LVO -30: SPI_RegisterController                                    */
/* ------------------------------------------------------------------ */

int32_t SPI_RegisterController(struct SpiBase *sb __asm("a6"), struct spi_controller *ctrl __asm("a0"))
{
    if (!ctrl || !ctrl->dev)
        return -1;

    DBG_SPI("SPI: register controller dev='%s'\n", ctrl->dev->node.ln_Name ? ctrl->dev->node.ln_Name : "(unnamed)");

    /* Add to controller list (uses ctrl->node, NOT dev->node,
     * which is already on DDMBase->devices). */
    AddTail(&sb->controllers, &ctrl->node);

    /* Enumerate the controller's children from the device tree.
     * Each child becomes a spi_device. */
    struct device *child = DDM_GetChild(sb->ddmbase, ctrl->dev);
    while (child)
    {
        /* Create a spi_device for this child */
        struct spi_device *spidev = (struct spi_device *)AllocMem(sizeof(struct spi_device), MEMF_ANY | MEMF_CLEAR);
        if (!spidev)
            return -1;

        spidev->dev = child;
        spidev->controller = ctrl;

        /* Get chip select from "reg" property */
        uint32_t cs = 0;
        DT_GetPropertyU32(sb->ddmbase, child, "reg", &cs);
        spidev->chip_select = (uint16_t)cs;

        /* Get max speed from "spi-max-frequency" property */
        uint32_t max_freq = 0;
        DT_GetPropertyU32(sb->ddmbase, child, "spi-max-frequency", &max_freq);
        spidev->max_speed = max_freq;

        DBG_SPI("SPI:   child '%s' cs=%lu max_freq=%lu\n", child->node.ln_Name ? child->node.ln_Name : "(unnamed)",
                (unsigned long)spidev->chip_select, max_freq);

        /* Store the spi_device pointer in the child's bus_data */
        child->bus_data = spidev;

        /* Children of an SPI controller are SPI bus devices.
         * Set the bus type before matching so the strict bus_type
         * filter works. */
        child->bus_type = BUS_TYPE_SPI;

        /* Add to spi_devices list (uses spidev->node, NOT dev->node). */
        AddTail(&sb->spi_devices, &spidev->node);

        /* Try to match this device with a registered driver */
        DDM_MatchDevice(sb->ddmbase, child);

        child = DDM_GetNextSibling(sb->ddmbase, child);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* LVO -36: SPI_UnregisterController                                  */
/* ------------------------------------------------------------------ */

void SPI_UnregisterController(struct SpiBase *sb __asm("a6"), struct spi_controller *ctrl __asm("a0"))
{
    if (!ctrl)
        return;

    /* Remove all spi_devices that belong to this controller */
    struct Node *node, *next;
    for (node = sb->spi_devices.lh_Head; node->ln_Succ; node = next)
    {
        next = node->ln_Succ;
        /* The list stores spidev->node. Recover the spi_device via
         * offsetof(struct spi_device, node). */
        struct spi_device *spidev = (struct spi_device *)((uint8_t *)node - offsetof(struct spi_device, node));

        if (spidev->controller == ctrl)
        {
            Remove(node);
            if (spidev->dev)
                spidev->dev->bus_data = NULL;
            FreeMem(spidev, sizeof(struct spi_device));
        }
    }

    /* Remove from controller list */
    Remove(&ctrl->node);
}

/* ------------------------------------------------------------------ */
/* LVO -42: SPI_FindDevice                                            */
/* ------------------------------------------------------------------ */

struct spi_device *SPI_FindDevice(struct SpiBase *sb __asm("a6"), struct device *dev __asm("a0"))
{
    if (!dev)
        return NULL;

    /* The spi_device pointer is stored in the device's bus_data */
    return (struct spi_device *)dev->bus_data;
}

/* ------------------------------------------------------------------ */
/* LVO -48: SPI_SetSpeed                                              */
/* ------------------------------------------------------------------ */

void SPI_SetSpeed(struct SpiBase *sb __asm("a6"), struct spi_device *spidev __asm("a0"), uint32_t speed __asm("d0"))
{
    if (!spidev || !spidev->controller || !spidev->controller->set_speed)
        return;
    /* Clamp the requested speed to the device's maximum (from the
     * "spi-max-frequency" device tree property). A max_speed of 0
     * means "no limit / use controller default". */
    if (spidev->max_speed && speed > spidev->max_speed)
        speed = spidev->max_speed;
    spidev->controller->set_speed(spidev->controller, speed);
}

/* ------------------------------------------------------------------ */
/* LVO -54: SPI_Select                                                */
/* ------------------------------------------------------------------ */

void SPI_Select(struct SpiBase *sb __asm("a6"), struct spi_device *spidev __asm("a0"))
{
    if (!spidev || !spidev->controller || !spidev->controller->select)
        return;
    spidev->controller->select(spidev->controller, spidev->chip_select);
}

/* ------------------------------------------------------------------ */
/* LVO -60: SPI_Deselect                                             */
/* ------------------------------------------------------------------ */

void SPI_Deselect(struct SpiBase *sb __asm("a6"), struct spi_device *spidev __asm("a0"))
{
    if (!spidev || !spidev->controller || !spidev->controller->deselect)
        return;
    spidev->controller->deselect(spidev->controller, spidev->chip_select);
}

/* ------------------------------------------------------------------ */
/* LVO -66: SPI_Transfer                                             */
/* ------------------------------------------------------------------ */

void SPI_Transfer(struct SpiBase *sb __asm("a6"), struct spi_device *spidev __asm("a0"),
                  const uint8_t *write_buf __asm("a1"), uint8_t *read_buf __asm("a2"), uint32_t size __asm("d0"))
{
    if (!spidev || !spidev->controller || !spidev->controller->transfer)
        return;
    DBG_SPI("SPI: transfer cs=%lu size=%lu\n", (unsigned long)spidev->chip_select, size);
    spidev->controller->transfer(spidev->controller, write_buf, read_buf, size);
}

/* ------------------------------------------------------------------ */
/* LVO -72: SPI_RegisterDriver                                        */
/* ------------------------------------------------------------------ */

int32_t SPI_RegisterDriver(struct SpiBase *sb __asm("a6"), struct device_driver *drv __asm("a0"))
{
    if (!drv)
        return -1;

    /* Register with the DDM framework */
    if (!DDM_RegisterDriver(sb->ddmbase, drv))
        return -1;

    return 0;
}

/* ------------------------------------------------------------------ */
/* LVO -84: SPI_UnregisterDriver                                      */
/* ------------------------------------------------------------------ */

void SPI_UnregisterDriver(struct SpiBase *sb __asm("a6"), struct device_driver *drv __asm("a0"))
{
    if (!drv)
        return;
    DDM_UnregisterDriver(sb->ddmbase, drv);
}

/* ------------------------------------------------------------------ */
/* Library init / open / close / expunge                              */
/* ------------------------------------------------------------------ */

static BPTR spi_expunge(struct SpiBase *sb __asm("a6"));

static struct Library *spi_open(struct SpiBase *sb __asm("a6"))
{
    sb->lib.lib_OpenCnt++;
    return (struct Library *)sb;
}

static BPTR spi_close(struct SpiBase *sb __asm("a6"))
{
    sb->lib.lib_OpenCnt--;
    if (sb->lib.lib_OpenCnt == 0 && (sb->lib.lib_Flags & LIBF_DELEXP))
        return spi_expunge(sb);
    return 0;
}

static BPTR spi_expunge(struct SpiBase *sb __asm("a6"))
{
    if (sb->lib.lib_OpenCnt != 0)
    {
        sb->lib.lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    BPTR seg_list = sb->seg_list;
    Remove(&sb->lib.lib_Node);

    if (sb->ddmbase)
        CloseLibrary((struct Library *)sb->ddmbase);

    FreeMem((char *)sb - sb->lib.lib_NegSize, sb->lib.lib_NegSize + sb->lib.lib_PosSize);
    SpiBase = NULL;
    return seg_list;
}

static int32_t noexec(void)
{
    return -1;
}

static struct SpiBase *spi_init(struct ExecBase *sys_base __asm("a6"), BPTR seg_list __asm("a0"),
                                struct SpiBase *sb __asm("d0"))
{
    SysBase = sys_base;
    sb->lib.lib_Node.ln_Type = NT_LIBRARY;
    sb->lib.lib_Node.ln_Name = "spi.library";
    sb->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    sb->lib.lib_Version = 1;
    sb->lib.lib_Revision = 0;
    sb->lib.lib_IdString = (void *)"spi 1.0 (August 2026)\n\r";
    sb->lib.lib_OpenCnt = 0;
    sb->seg_list = seg_list;

    NewList(&sb->controllers);
    NewList(&sb->spi_devices);

    sb->ddmbase = NULL;
    SpiBase = sb;

    /* Open ddm.library (for DDM_ functions and DT_* device tree functions) */
    sb->ddmbase = (struct DDMBase *)OpenLibrary("ddm.library", 0);

    return sb;
}

static uint32_t library_vectors[] = {
    (uint32_t)spi_open,                 /* -6 */
    (uint32_t)spi_close,                /* -12 */
    (uint32_t)spi_expunge,              /* -18 */
    (uint32_t)noexec,                   /* -24 */
    (uint32_t)SPI_RegisterController,   /* -30 */
    (uint32_t)SPI_UnregisterController, /* -36 */
    (uint32_t)SPI_FindDevice,           /* -42 */
    (uint32_t)SPI_SetSpeed,             /* -48 */
    (uint32_t)SPI_Select,               /* -54 */
    (uint32_t)SPI_Deselect,             /* -60 */
    (uint32_t)SPI_Transfer,             /* -66 */
    (uint32_t)SPI_RegisterDriver,       /* -72 */
    (uint32_t)SPI_UnregisterDriver,     /* -78 */
    -1,
};

uint32_t auto_init_tables[] = {
    sizeof(struct SpiBase),
    (uint32_t)library_vectors,
    0,
    (uint32_t)spi_init,
};

const struct Resident romtag = {
    .rt_MatchWord = RTC_MATCHWORD,
    .rt_MatchTag = (void *)&romtag,
    .rt_EndSkip = (void *)(&romtag + 1),
    .rt_Flags = RTF_AUTOINIT,
    .rt_Version = 1,
    .rt_Type = NT_LIBRARY,
    .rt_Pri = 0,
    .rt_Name = "spi.library",
    .rt_IdString = "spi 1.0 (August 2026)\n\r",
    .rt_Init = auto_init_tables,
};
