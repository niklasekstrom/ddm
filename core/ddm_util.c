/*
 * Shared string utilities - part of ddm.library
 *
 * Implementation of the ddm_strlen / ddm_strcmp / ddm_strdup helpers
 * declared in include/ddm_util.h. These replace the per-file static
 * copies that were duplicated across the core and subsystems.
 *
 * Also hosts the shared device/property creation helpers
 * (ddm_create_device / ddm_create_property / ddm_add_property) used
 * by both the main DTS parser (parser.c) and the overlay applier
 * (overlay.c).
 */
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/exec.h>

#include "ddm_debug.h"

#include "ddm_util.h"
#include "ddm.h"
#include "devicetree.h" /* struct device_node, struct dt_property */

/* SysBase is defined in romtag.c; declared here so exec functions work */
extern struct ExecBase *SysBase;

/* ------------------------------------------------------------------ */
/* String helpers                                                     */
/* ------------------------------------------------------------------ */

uint32_t ddm_strlen(const char *s)
{
    uint32_t len = 0;
    if (s)
        while (s[len])
            len++;
    return len;
}

int ddm_strcmp(const char *a, const char *b)
{
    if (!a)
        a = "";
    if (!b)
        b = "";
    while (*a && *a == *b)
    {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

char *ddm_strdup(const char *s)
{
    if (!s)
        return NULL;
    uint32_t len = ddm_strlen(s);
    char *dup = (char *)AllocMem(len + 1, MEMF_ANY | MEMF_CLEAR);
    if (dup)
    {
        for (uint32_t i = 0; i <= len; i++)
            dup[i] = s[i];
    }
    return dup;
}

/* ------------------------------------------------------------------ */
/* Device / property creation helpers                                 */
/* ------------------------------------------------------------------ */

/* Create a new device with an attached (empty) device_node.
 * ln_Name and of_node->name are ddm_strdup'd from 'name'.
 * The device is initialised with bus_type = BUS_TYPE_PLATFORM and
 * flags = DEV_FLAG_FROM_TREE. Returns NULL on allocation failure. */
struct device *ddm_create_device(const char *name)
{
    struct device *dev = (struct device *)AllocMem(sizeof(struct device), MEMF_ANY | MEMF_CLEAR);
    if (!dev)
        return NULL;

    dev->node.ln_Name = ddm_strdup(name);
    if (!dev->node.ln_Name)
    {
        FreeMem(dev, sizeof(struct device));
        return NULL;
    }

    /* Create a device_node to hold the device-tree-specific data
     * (name, path, properties). Link it via of_node. */
    struct device_node *dn = (struct device_node *)AllocMem(sizeof(struct device_node), MEMF_ANY | MEMF_CLEAR);
    if (!dn)
    {
        FreeMem(dev->node.ln_Name, ddm_strlen(dev->node.ln_Name) + 1);
        FreeMem(dev, sizeof(struct device));
        return NULL;
    }
    dn->name = ddm_strdup(name);
    dn->path = NULL;
    dn->properties = NULL;

    dev->of_node = dn;
    dev->parent = NULL;
    dev->children = NULL;
    dev->next_sibling = NULL;
    dev->driver = NULL;
    dev->driver_data = NULL;
    dev->bus_data = NULL;
    dev->bus_type = BUS_TYPE_PLATFORM;
    dev->flags = DEV_FLAG_FROM_TREE;
    return dev;
}

/* Create a new dt_property with a ddm_strdup'd name and a copy of
 * 'value' (length bytes). For length == 0 the value pointer is set
 * to NULL. Returns NULL on allocation failure. */
struct dt_property *ddm_create_property(const char *name, uint8_t *value, uint32_t length)
{
    struct dt_property *prop = (struct dt_property *)AllocMem(sizeof(struct dt_property), MEMF_ANY | MEMF_CLEAR);
    if (!prop)
        return NULL;

    prop->name = ddm_strdup(name);
    if (!prop->name)
    {
        FreeMem(prop, sizeof(struct dt_property));
        return NULL;
    }

    prop->length = length;
    if (length > 0)
    {
        prop->value = (uint8_t *)AllocMem(length, MEMF_ANY | MEMF_CLEAR);
        if (!prop->value)
        {
            FreeMem(prop->name, ddm_strlen(prop->name) + 1);
            FreeMem(prop, sizeof(struct dt_property));
            return NULL;
        }
        for (uint32_t i = 0; i < length; i++)
            prop->value[i] = value[i];
    }
    else
    {
        prop->value = NULL;
    }

    prop->next = NULL;
    return prop;
}

/* Append a property to a device's of_node property list. */
void ddm_add_property(struct device *dev, struct dt_property *prop)
{
    if (!dev->of_node)
        return;
    if (!dev->of_node->properties)
    {
        dev->of_node->properties = prop;
    }
    else
    {
        struct dt_property *p = dev->of_node->properties;
        while (p->next)
            p = p->next;
        p->next = prop;
    }
}
