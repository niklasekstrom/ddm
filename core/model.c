#define DDM_INTERNAL
/*
 * DDM framework - core implementation
 *
 * Device registration, tree management, and device finding.
 */
#include <exec/interrupts.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/exec.h>

#include "ddm_debug.h"

#include "ddm.h"
#include "ddm_driver.h" /* DDMDrv_Shutdown, DDMDrv_Remove LVO stubs */
#include "ddm_util.h"
#include "devicetree.h" /* struct device_node, struct dt_property */

extern struct DDMBase *DDMBase;

/* SysBase is defined in romtag.c; declared here so exec functions work */
extern struct ExecBase *SysBase;

/* ------------------------------------------------------------------ */
/* Path generation                                                     */
/* ------------------------------------------------------------------ */

/* Build the full path for a device by walking up to the root.
 * Returns an allocated string, or NULL on failure. The caller must
 * free the returned string.
 */
static char *dt_build_path(struct device *dev)
{
    /* Count total length needed */
    uint32_t total = 0;
    struct device *d = dev;
    while (d)
    {
        total += ddm_strlen(dev_name(d)) + 1; /* +1 for '/' */
        d = d->parent;
    }
    if (total == 0)
        total = 1; /* just "/" for root */

    char *path = (char *)AllocMem(total + 1, MEMF_ANY | MEMF_CLEAR);
    if (!path)
        return NULL;

    /* Build path by collecting segments and assembling in reverse.
     * Count the depth first, then allocate the segment arrays. */
    int depth = 0;
    {
        struct device *dd = dev;
        while (dd)
        {
            depth++;
            dd = dd->parent;
        }
    }

    const char **segs = (const char **)AllocMem(depth * sizeof(char *), MEMF_ANY);
    uint32_t *seglens = (uint32_t *)AllocMem(depth * sizeof(uint32_t), MEMF_ANY);
    if (!segs || !seglens)
    {
        if (segs)
            FreeMem(segs, depth * sizeof(char *));
        if (seglens)
            FreeMem(seglens, depth * sizeof(uint32_t));
        FreeMem(path, total + 1);
        return NULL;
    }

    {
        int i = 0;
        struct device *dd = dev;
        while (dd && i < depth)
        {
            segs[i] = dev_name(dd);
            seglens[i] = ddm_strlen(dev_name(dd));
            i++;
            dd = dd->parent;
        }
    }

    /* Assemble: /seg0/seg1/.../segN
     * The root device has ln_Name = "/", so the last segment (i == 0
     * after reversal, i.e. i == depth-1 in the loop) is "/". We must
     * not prepend another "/" to it, or we get "//cia-...". */
    uint32_t pos = 0;
    for (int i = depth - 1; i >= 0; i--)
    {
        /* Don't prepend '/' for the root segment (name is already "/") */
        if (!(i == depth - 1 && seglens[i] == 1 && segs[i][0] == '/'))
            path[pos++] = '/';
        for (uint32_t j = 0; j < seglens[i]; j++)
            path[pos++] = segs[i][j];
    }
    path[pos] = '\0';

    /* If device is root (no parent), path is "/" */
    if (pos == 0)
    {
        path[0] = '/';
        path[1] = '\0';
    }

    FreeMem(segs, depth * sizeof(char *));
    FreeMem(seglens, depth * sizeof(uint32_t));
    return path;
}

/* ------------------------------------------------------------------ */
/* Property lookup on of_node                                          */
/* ------------------------------------------------------------------ */

static const struct dt_property *dt_find_property(struct device *dev, const char *name)
{
    if (!dev->of_node)
        return NULL;
    struct dt_property *prop = dev->of_node->properties;
    while (prop)
    {
        if (ddm_strcmp(prop->name, name) == 0)
            return prop;
        prop = prop->next;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* LVO -30: DDM_RegisterDevice                                        */
/* ------------------------------------------------------------------ */

struct device *DDM_RegisterDevice(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"))
{
    if (!dev)
        return NULL;

    /* Must have a name */
    if (!dev->node.ln_Name)
        return NULL;

    /* Link into parent's children list */
    if (dev->parent)
    {
        dev->next_sibling = dev->parent->children;
        dev->parent->children = dev;
    }
    else
    {
        /* No parent: this is the root */
        if (ddm->root)
        {
            /* Root already exists */
            return NULL;
        }
        ddm->root = dev;
    }

    /* Build path and store on of_node (if the device has one) */
    if (dev->of_node)
        dev->of_node->path = dt_build_path(dev);

    /* Add to global device list */
    AddTail(&ddm->devices, &dev->node);

    DBG_CORE("DDM: register device '%s' path='%s'\n", dev->node.ln_Name,
             (dev->of_node && dev->of_node->path) ? dev->of_node->path : "(no path)");
    return dev;
}

/* ------------------------------------------------------------------ */
/* LVO -36: DDM_UnregisterDevice                                      */
/* ------------------------------------------------------------------ */

/* Recursively free a device and all its children */
static void dt_free_device(struct DDMBase *ddm, struct device *dev)
{
    if (!dev)
        return;

    /* If a driver is bound to this device, unbind it first.
     * This mirrors dt_unbind_driver in driver.c: call shutdown
     * and remove if the device was probed, then clear the
     * binding flags. */
    if (dev->driver)
    {
        if (dev->flags & DEV_FLAG_PROBED)
        {
            DDMDrv_Shutdown(dev->driver->lib_base, dev);
            DDMDrv_Remove(dev->driver->lib_base, dev);
            dev->flags &= ~DEV_FLAG_PROBED;
        }
        dev->driver = NULL;
        dev->driver_data = NULL;
        dev->flags &= ~(DEV_FLAG_BOUND | DEV_FLAG_DEFERRED);
    }

    /* Free children first */
    struct device *child = dev->children;
    while (child)
    {
        struct device *next = child->next_sibling;
        dt_free_device(ddm, child);
        child = next;
    }

    /* Remove from global device list */
    Remove(&dev->node);

    /* Remove from parent's children list */
    if (dev->parent)
    {
        struct device **pp = &dev->parent->children;
        while (*pp)
        {
            if (*pp == dev)
            {
                *pp = dev->next_sibling;
                break;
            }
            pp = &(*pp)->next_sibling;
        }
    }
    else if (ddm->root == dev)
    {
        ddm->root = NULL;
    }

    /* Free of_node (device tree node) if present */
    if (dev->of_node)
    {
        /* Free properties */
        struct dt_property *prop = dev->of_node->properties;
        while (prop)
        {
            struct dt_property *next = prop->next;
            if (prop->name)
                FreeMem(prop->name, ddm_strlen(prop->name) + 1);
            if (prop->value)
                FreeMem(prop->value, prop->length);
            FreeMem(prop, sizeof(struct dt_property));
            prop = next;
        }

        /* Free path */
        if (dev->of_node->path)
            FreeMem(dev->of_node->path, ddm_strlen(dev->of_node->path) + 1);

        /* Free name */
        if (dev->of_node->name)
            FreeMem(dev->of_node->name, ddm_strlen(dev->of_node->name) + 1);

        /* Free the device_node struct */
        FreeMem(dev->of_node, sizeof(struct device_node));
    }

    /* Free the device struct itself (but not ln_Name, which is
     * owned by the caller or allocated separately) — EXCEPT for
     * tree devices, whose ln_Name was allocated by the parser. */
    if ((dev->flags & DEV_FLAG_FROM_TREE) && dev->node.ln_Name)
        FreeMem(dev->node.ln_Name, ddm_strlen(dev->node.ln_Name) + 1);

    FreeMem(dev, sizeof(struct device));
}

void DDM_UnregisterDevice(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"))
{
    DBG_CORE("DDM: unregister device '%s'\n", (dev && dev->node.ln_Name) ? dev->node.ln_Name : "(unnamed)");
    dt_free_device(ddm, dev);
}

/* ------------------------------------------------------------------ */
/* LVO -66: DDM_FindDevice                                            */
/* ------------------------------------------------------------------ */

/* Parse a path like "/cia/parallel-port/par-spi-adapter" and find the
 * corresponding device. Returns NULL if not found.
 */
struct device *DDM_FindDevice(struct DDMBase *ddm __asm("a6"), const char *path __asm("a0"))
{
    if (!path || !ddm->root)
        return NULL;

    /* Root path "/" */
    if (path[0] == '/' && path[1] == '\0')
        return ddm->root;

    /* Must start with '/' */
    if (path[0] != '/')
        return NULL;

    struct device *dev = ddm->root;
    const char *p = path + 1; /* skip leading '/' */

    while (*p && dev)
    {
        /* Extract segment name */
        const char *start = p;
        while (*p && *p != '/')
            p++;
        uint32_t seglen = p - start;

        /* Find child matching this segment */
        struct device *child = dev->children;
        struct device *found = NULL;
        while (child)
        {
            uint32_t namelen = ddm_strlen(dev_name(child));
            if (namelen == seglen)
            {
                int16_t match = TRUE;
                for (uint32_t i = 0; i < seglen; i++)
                {
                    if (dev_name(child)[i] != start[i])
                    {
                        match = FALSE;
                        break;
                    }
                }
                if (match)
                {
                    found = child;
                    break;
                }
            }
            child = child->next_sibling;
        }

        dev = found;

        /* Skip the '/' */
        if (*p == '/')
            p++;
    }

    return dev;
}

/* ------------------------------------------------------------------ */
/* LVO -72: DDM_FindByCompatible                                      */
/* ------------------------------------------------------------------ */

struct device *DDM_FindByCompatible(struct DDMBase *ddm __asm("a6"), const char *compatible __asm("a0"))
{
    if (!compatible)
        return NULL;

    struct device *dev = (struct device *)ddm->devices.lh_Head;
    while (dev->node.ln_Succ)
    {
        const struct dt_property *prop = dt_find_property(dev, "compatible");
        if (prop)
        {
            /* compatible is a NUL-separated string list */
            const char *s = (const char *)prop->value;
            uint32_t remaining = prop->length;
            while (remaining > 0)
            {
                if (ddm_strcmp(s, compatible) == 0)
                    return dev;
                uint32_t slen = ddm_strlen(s) + 1;
                s += slen;
                remaining -= slen;
            }
        }
        dev = (struct device *)dev->node.ln_Succ;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* LVO -78/-84/-90: Tree traversal                                    */
/* ------------------------------------------------------------------ */

struct device *DDM_GetParent(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"))
{
    (void)ddm;
    return dev ? dev->parent : NULL;
}

struct device *DDM_GetChild(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"))
{
    (void)ddm;
    return dev ? dev->children : NULL;
}

struct device *DDM_GetNextSibling(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"))
{
    (void)ddm;
    return dev ? dev->next_sibling : NULL;
}
