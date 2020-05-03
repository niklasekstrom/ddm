#define DDM_INTERNAL
/*
 * DDM framework - driver registration and matching
 */
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/exec.h>

#include "ddm_debug.h"

#include "ddm.h"
#include "ddm_driver.h" /* DDMDrv_ LVO stubs */
#include "ddm_protos.h" /* DDM_LoadConfig LVO stub */
#include "ddm_util.h"
#include "devicetree.h" /* struct dt_property, DT_ParseTree LVO stub */

extern struct DDMBase *DDMBase;

/* SysBase is defined in romtag.c; declared here so exec functions work */
extern struct ExecBase *SysBase;

/* Forward declarations — these are defined later in this file but used
 * by ddm_driver_matches and DDM_MatchDevice. */
struct bus_type *DDM_GetBusType(struct DDMBase *ddm __asm("a6"), uint32_t bus_type_id __asm("d0"));

/* ------------------------------------------------------------------ */
/* Internal helpers                                                   */

/* Find the "compatible" property of a device and check if it contains
 * the given compatible string. The compatible property is a NUL-separated
 * string list, stored on the device's of_node.
 */
static int16_t dt_device_matches_compatible(struct device *dev, const char *compatible)
{
    if (!dev->of_node)
        return FALSE;

    /* Check the device's "compatible" property on its of_node */
    struct dt_property *prop = dev->of_node->properties;
    while (prop)
    {
        if (ddm_strcmp(prop->name, "compatible") == 0)
        {
            const char *s = (const char *)prop->value;
            uint32_t remaining = prop->length;
            while (remaining > 0)
            {
                if (ddm_strcmp(s, compatible) == 0)
                    return TRUE;
                uint32_t slen = ddm_strlen(s) + 1;
                s += slen;
                remaining -= slen;
            }
            break;
        }
        prop = prop->next;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* LVO -42: DDM_RegisterDriver                                        */
/* ------------------------------------------------------------------ */

/* Find an already-registered driver by its ln_Name. Used to make
 * DDM_RegisterDriver idempotent: if the same driver registers twice
 * (e.g. the bootstrap trigger component is re-opened mid-init by
 * DDM_LoadConfig), the duplicate is silently ignored. */
static struct device_driver *find_driver_by_name(struct DDMBase *ddm, const char *name)
{
    struct device_driver *drv = (struct device_driver *)ddm->drivers.lh_Head;
    while (drv->node.ln_Succ)
    {
        if (ddm_strcmp(drv->node.ln_Name, name) == 0)
            return drv;
        drv = (struct device_driver *)drv->node.ln_Succ;
    }
    return NULL;
}

struct device_driver *DDM_RegisterDriver(struct DDMBase *ddm __asm("a6"), struct device_driver *drv __asm("a0"))
{
    if (!drv)
        return NULL;

    /* Must have a name */
    if (!drv->node.ln_Name)
        return NULL;

    /* Must have a compatible array */
    if (!drv->compatible)
        return NULL;

    /* Must have a library base for LVO calls */
    if (!drv->lib_base)
        return NULL;

    /* Idempotent: if a driver with the same name is already registered,
     * return the existing one. This handles the bootstrap trigger
     * component being re-opened mid-init by DDM_LoadConfig (which lists
     * all drivers in ddm.conf). Without this, the re-open would re-init
     * the trigger and register a duplicate driver. */
    struct device_driver *existing = find_driver_by_name(ddm, drv->node.ln_Name);
    if (existing)
    {
        DBG_CORE("DDM: driver '%s' already registered (idempotent re-open)\n", drv->node.ln_Name);
        return existing;
    }

    DBG_CORE("DDM: register driver '%s' bus=%lu\n", drv->node.ln_Name, drv->bus_type);
    AddTail(&ddm->drivers, &drv->node);

    /* Lazy bootstrap: on the first driver registration, parse the
     * device tree and load the config. DDM_LoadConfig opens all
     * remaining driver libraries (which call DDM_RegisterDriver —
     * they see bootstrapping=TRUE and skip this block) and then
     * calls DDM_MatchAll to bind drivers to devices.
     *
     * This runs inside an LVO, so ddm.library is already public
     * (the caller opened it first). Nested OpenLibrary("ddm.library")
     * calls during DDM_LoadConfig just bump OpenCnt — no reload,
     * no recursion. The bootstrapping flag guards against re-entry
     * from drivers loaded by DDM_LoadConfig. */
    if (!ddm->bootstrapped && !ddm->bootstrapping)
    {
        ddm->bootstrapping = TRUE;
        ddm->bootstrap_trigger = drv->node.ln_Name;

        if (DT_ParseTree(ddm, "DEVS:a1200.dts") == 0)
            DDM_LoadConfig(ddm, "DEVS:ddm.conf");

        /* Mark as done regardless of success/failure to prevent
         * retry loops on every subsequent registration. */
        ddm->bootstrapping = FALSE;
        ddm->bootstrap_trigger = NULL;
        ddm->bootstrapped = TRUE;
    }

    return drv;
}

/* ------------------------------------------------------------------ */
/* LVO -48: DDM_UnregisterDriver                                      */
/* ------------------------------------------------------------------ */

/* Unbind a driver from all devices it's bound to */
static void dt_unbind_driver(struct DDMBase *ddm, struct device_driver *drv)
{
    struct device *dev = (struct device *)ddm->devices.lh_Head;
    while (dev->node.ln_Succ)
    {
        struct device *next = (struct device *)dev->node.ln_Succ;
        if (dev->driver == drv)
        {
            /* Call shutdown then remove if probed */
            if (dev->flags & DEV_FLAG_PROBED)
            {
                DDMDrv_Shutdown(drv->lib_base, dev);
                DDMDrv_Remove(drv->lib_base, dev);
                dev->flags &= ~DEV_FLAG_PROBED;
            }
            dev->driver = NULL;
            dev->driver_data = NULL;
            dev->flags &= ~(DEV_FLAG_BOUND | DEV_FLAG_DEFERRED);
        }
        dev = next;
    }
}

void DDM_UnregisterDriver(struct DDMBase *ddm __asm("a6"), struct device_driver *drv __asm("a0"))
{
    if (!drv)
        return;
    dt_unbind_driver(ddm, drv);
    Remove(&drv->node);
}

/* ------------------------------------------------------------------ */
/* LVO -60: DDM_MatchDevice                                           */
/* ------------------------------------------------------------------ */

/* Check whether a driver matches a device, using either the bus_type's
 * custom match callback (if registered) or the default compatible-string
 * matching. Returns TRUE if the driver matches.
 */
static int16_t ddm_driver_matches(struct DDMBase *ddm, struct device *dev, struct device_driver *drv)
{
    /* If the bus type has a custom match callback, use it */
    struct bus_type *bus = DDM_GetBusType(ddm, dev->bus_type);
    if (bus && bus->match)
        return bus->match(dev, drv);

    /* Default: compatible-string matching */
    const char **compat = drv->compatible;
    while (*compat)
    {
        if (dt_device_matches_compatible(dev, *compat))
            return TRUE;
        compat++;
    }
    return FALSE;
}

/* Try to match a single device with a registered driver. Returns:
 *   0                   — a driver was bound.
 *   DDMDRV_PROBE_DEFER  — a matching driver deferred; retry later.
 *   -1                  — no match found.
 */
int32_t DDM_MatchDevice(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"))
{
    if (!dev || (dev->flags & DEV_FLAG_BOUND))
        return -1;

    struct device_driver *drv = (struct device_driver *)ddm->drivers.lh_Head;
    while (drv->node.ln_Succ)
    {
        /* Capture next before probe — DDMDrv_Probe may trigger
         * subsystem init that registers new drivers, mutating
         * the driver list and invalidating drv->node.ln_Succ. */
        struct device_driver *next = (struct device_driver *)drv->node.ln_Succ;

        /* Filter by bus type (strict match) */
        if (drv->bus_type == dev->bus_type)
        {
            if (ddm_driver_matches(ddm, dev, drv))
            {
                /* Match found. Call probe (does check + init). */
                DBG_CORE("DDM: match '%s' -> '%s', probing\n", dev_name(dev) ? dev_name(dev) : "(unnamed)",
                         drv->node.ln_Name);
                int32_t ret = DDMDrv_Probe(drv->lib_base, dev);
                if (ret == DDMDRV_PROBE_OK)
                {
                    /* Probe succeeded, bind driver */
                    dev->driver = drv;
                    dev->flags |= (DEV_FLAG_BOUND | DEV_FLAG_PROBED);
                    dev->flags &= ~DEV_FLAG_DEFERRED;
                    DBG_CORE("DDM: bound '%s' to '%s'\n", dev_name(dev) ? dev_name(dev) : "(unnamed)",
                             drv->node.ln_Name);
                    return 0;
                }
                if (ret == DDMDRV_PROBE_DEFER)
                {
                    /* Driver matches but a dependency isn't ready.
                     * Mark deferred so DDM_MatchAll retries later. */
                    dev->flags |= DEV_FLAG_DEFERRED;
                    DBG_CORE("DDM: probe deferred for '%s' by '%s'\n", dev_name(dev) ? dev_name(dev) : "(unnamed)",
                             drv->node.ln_Name);
                    return DDMDRV_PROBE_DEFER;
                }
                /* Probe rejected the device, try next driver */
                DBG_CORE("DDM: probe rejected '%s' (ret=%ld), trying next\n",
                         dev_name(dev) ? dev_name(dev) : "(unnamed)", ret);
            }
        }
        drv = next;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* LVO -54: DDM_MatchAll                                              */
/* ------------------------------------------------------------------ */

/* Match all unmatched devices with registered drivers. Subsystems own
 * child enumeration: when a controller driver's probe registers with
 * its subsystem (e.g. SPI_RegisterController), the subsystem creates
 * bus-specific child devices and calls DDM_MatchDevice on them inline.
 * This means the entire device-tree chain binds in a single pass for
 * most topologies.
 *
 * If a driver returns DDMDRV_PROBE_DEFER, the device is marked deferred
 * and retried in subsequent passes. The loop stops when a full pass
 * produces no new bindings and no deferrals.
 */
int32_t DDM_MatchAll(struct DDMBase *ddm __asm("a6"))
{
    int16_t need_another_pass;
    int pass = 0;
    int max_passes = 32;

    do
    {
        need_another_pass = FALSE;
        pass++;
        if (pass > max_passes)
            break;

        struct device *dev = (struct device *)ddm->devices.lh_Head;
        while (dev->node.ln_Succ)
        {
            /* Capture next before matching — DDM_MatchDevice may
             * trigger a controller probe that registers children
             * with a subsystem, adding them to ddm->devices and
             * potentially invalidating dev->node.ln_Succ. */
            struct device *next = (struct device *)dev->node.ln_Succ;

            if (!(dev->flags & DEV_FLAG_BOUND))
            {
                int32_t ret = DDM_MatchDevice(ddm, dev);
                if (ret == 0)
                {
                    /* Device bound — a controller probe may have
                     * registered children with a subsystem, which
                     * matched them inline. Keep iterating. */
                    need_another_pass = TRUE;
                }
                else if (ret == DDMDRV_PROBE_DEFER)
                {
                    /* Deferred — retry in the next pass */
                    need_another_pass = TRUE;
                }
                else
                {
                    /* No match — clear deferred flag if set */
                    dev->flags &= ~DEV_FLAG_DEFERRED;
                }
            }
            dev = next;
        }
    } while (need_another_pass);

    /* Count devices still deferred on exit so callers can detect
     * unresolved dependencies (e.g. a driver waiting on a subsystem
     * that never registered). */
    uint32_t deferred = 0;
    struct device *dev = (struct device *)ddm->devices.lh_Head;
    while (dev->node.ln_Succ)
    {
        if (dev->flags & DEV_FLAG_DEFERRED)
            deferred++;
        dev = (struct device *)dev->node.ln_Succ;
    }
    ddm->deferred_count = deferred;

    if (deferred > 0)
    {
        DBG_CORE("DDM: %lu device(s) still deferred after %ld pass(es)\n", deferred, (long)pass);
        struct device *d = (struct device *)ddm->devices.lh_Head;
        while (d->node.ln_Succ)
        {
            if (d->flags & DEV_FLAG_DEFERRED)
                DBG_CORE("  deferred: %s\n", dev_name(d) ? dev_name(d) : "(unnamed)");
            d = (struct device *)d->node.ln_Succ;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* LVO -96: DDM_EnumerateChildren                                     */
/* ------------------------------------------------------------------ */

int32_t DDM_EnumerateChildren(struct DDMBase *ddm __asm("a6"), struct device *bus __asm("a0"))
{
    (void)ddm;
    if (!bus || !bus->driver)
        return -1;

    /* Call the bus driver's enumerate LVO */
    return DDMDrv_Enumerate(bus->driver->lib_base, bus);
}

/* ------------------------------------------------------------------ */
/* LVO -102: DDM_RegisterBusType                                      */
/* ------------------------------------------------------------------ */

struct bus_type *DDM_RegisterBusType(struct DDMBase *ddm __asm("a6"), struct bus_type *bus __asm("a0"))
{
    if (!bus || !bus->node.ln_Name)
        return NULL;
    AddTail(&ddm->bus_types, &bus->node);
    return bus;
}

/* ------------------------------------------------------------------ */
/* LVO -108: DDM_GetBusType                                           */
/* ------------------------------------------------------------------ */

struct bus_type *DDM_GetBusType(struct DDMBase *ddm __asm("a6"), uint32_t bus_type_id __asm("d0"))
{
    struct bus_type *bus = (struct bus_type *)ddm->bus_types.lh_Head;
    while (bus->node.ln_Succ)
    {
        if (bus->bus_type_id == bus_type_id)
            return bus;
        bus = (struct bus_type *)bus->node.ln_Succ;
    }
    return NULL;
}
