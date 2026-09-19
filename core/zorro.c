#define DDM_INTERNAL
/*
 * Zorro expansion bus enumeration
 *
 * Walks expansion.library's ConfigDev list (via FindConfigDev) and
 * creates a DDM device node for each installed Zorro board. A
 * "zorro-bus" parent device is created as a child of the root, and
 * each board becomes a child of zorro-bus with bus_type = BUS_TYPE_ZORRO.
 *
 * Board devices carry DT properties so that overlay files can match
 * them by manufacturer/product ID and graft sub-device nodes (e.g.
 * clockports) onto them.
 *
 * This is an internal function called during the DDM bootstrap
 * sequence — it is NOT an LVO.
 */
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <libraries/configvars.h> /* struct ConfigDev, ERT_* constants */
#include <libraries/expansion.h>  /* EXPANSIONNAME */
#include <proto/exec.h>
#include <proto/expansion.h>      /* FindConfigDev inline stubs */

#include "ddm_debug.h"
#include "ddm_util.h"
#include "ddm.h"
#include "ddm_protos.h" /* DDM_RegisterDevice, DDM_UnregisterDevice */
#include "zorro.h"

/* SysBase is defined in romtag.c */
extern struct ExecBase *SysBase;

/* Helper: store a u32 value as a big-endian property (FDT convention). */
static void add_u32_prop(struct device *dev, const char *name, uint32_t val)
{
    /* Big-endian 4 bytes */
    uint8_t buf[4];
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)(val >> 16);
    buf[2] = (uint8_t)(val >> 8);
    buf[3] = (uint8_t)(val);

    struct dt_property *prop = ddm_create_property(name, buf, 4);
    if (prop)
        ddm_add_property(dev, prop);
}

/* Helper: store a string property. */
static void add_string_prop(struct device *dev, const char *name, const char *val)
{
    uint32_t len = ddm_strlen(val) + 1; /* include NUL terminator */
    struct dt_property *prop = ddm_create_property(name, (uint8_t *)val, len);
    if (prop)
        ddm_add_property(dev, prop);
}

void zorro_enumerate(struct DDMBase *ddm)
{
    DBG_CORE("Zorro: enumerating boards\n");

    struct ExpansionBase *ExpansionBase = (struct ExpansionBase *)OpenLibrary(EXPANSIONNAME, 0);
    if (!ExpansionBase)
    {
        DBG_CORE("Zorro: ERR: cannot open expansion.library\n");
        return;
    }

    /* Create the zorro-bus parent device */
    struct device *bus_dev = ddm_create_device("zorro-bus");
    if (!bus_dev)
    {
        CloseLibrary(ExpansionBase);
        return;
    }
    bus_dev->parent = ddm->root;
    bus_dev->bus_type = BUS_TYPE_PLATFORM;

    if (!DDM_RegisterDevice(ddm, bus_dev))
    {
        DDM_UnregisterDevice(ddm, bus_dev);
        CloseLibrary(ExpansionBase);
        return;
    }

    /* Iterate over all ConfigDev entries */
    struct ConfigDev *cd = NULL;
    int count = 0;
    while ((cd = FindConfigDev(cd, -1, -1)))
    {
        uint16_t manuf = cd->cd_Rom.er_Manufacturer;
        uint8_t product = cd->cd_Rom.er_Product;
        uint32_t serial = cd->cd_Rom.er_SerialNumber;
        uint8_t er_type = cd->cd_Rom.er_Type;
        APTR board_addr = cd->cd_BoardAddr;
        ULONG board_size = cd->cd_BoardSize;

        const char *board_type = (er_type & ERT_TYPEMASK) == ERT_ZORROIII ? "zorro3" : "zorro2";

        DBG_CORE("Zorro: board manuf=%lx product=%lx serial=%lx addr=%lx size=%lx type=%s\n",
                 (unsigned long)manuf, (unsigned long)product, (unsigned long)serial,
                 (unsigned long)board_addr, (unsigned long)board_size, board_type);

        /* Build device name: zorro@<manuf>-<product> */
        char name_buf[48];
        /* Simple hex formatting without sprintf */
        int pos = 0;
        const char *prefix = "zorro@";
        while (prefix[pos])
        {
            name_buf[pos] = prefix[pos];
            pos++;
        }
        /* manufacturer as 4 hex digits */
        for (int shift = 12; shift >= 0; shift -= 4)
        {
            uint8_t nibble = (uint8_t)((manuf >> shift) & 0xf);
            name_buf[pos++] = nibble < 10 ? ('0' + nibble) : ('a' + nibble - 10);
        }
        name_buf[pos++] = '-';
        /* product as 2 hex digits */
        for (int shift = 4; shift >= 0; shift -= 4)
        {
            uint8_t nibble = (uint8_t)((product >> shift) & 0xf);
            name_buf[pos++] = nibble < 10 ? ('0' + nibble) : ('a' + nibble - 10);
        }
        name_buf[pos] = '\0';

        struct device *board_dev = ddm_create_device(name_buf);
        if (!board_dev)
            continue;

        board_dev->parent = bus_dev;
        board_dev->bus_type = BUS_TYPE_ZORRO;

        /* Add DT properties */
        add_u32_prop(board_dev, "manufacturer-id", (uint32_t)manuf);
        add_u32_prop(board_dev, "product-id", (uint32_t)product);
        add_u32_prop(board_dev, "serial-number", serial);
        add_u32_prop(board_dev, "reg", (uint32_t)board_addr);
        add_u32_prop(board_dev, "board-size", (uint32_t)board_size);
        add_string_prop(board_dev, "board-type", board_type);
        add_string_prop(board_dev, "compatible", "zorro");

        if (!DDM_RegisterDevice(ddm, board_dev))
        {
            DDM_UnregisterDevice(ddm, board_dev);
            continue;
        }

        count++;
    }

    DBG_CORE("Zorro: enumerated %ld board(s)\n", (long)count);

    CloseLibrary(ExpansionBase);
}
