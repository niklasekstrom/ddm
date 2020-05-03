#define DDM_INTERNAL
/*
 * Device Tree - devicetree functions (part of ddm.library)
 *
 * Parses a DTS text file into struct device + struct device_node
 * entries (populating the DDM) and provides property access
 * and interrupt parsing from DTS properties.
 */
#include <exec/interrupts.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/exec.h>

#include "ddm_debug.h"

#include "ddm.h"
#include "ddm_protos.h"
#include "ddm_util.h"
#include "devicetree.h"
#include "irq.h" /* struct irq_domain, IRQ_TYPE_*, IRQ_* LVOs */

/* SysBase is defined in romtag.c; declared here so exec functions work */
extern struct ExecBase *SysBase;

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
/* LVO -204: DT_GetProperty                                           */
/* ------------------------------------------------------------------ */

const struct dt_property *DT_GetProperty(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"),
                                         const char *name __asm("a1"))
{
    (void)ddm;
    if (!dev || !name)
        return NULL;
    return dt_find_property(dev, name);
}

/* ------------------------------------------------------------------ */
/* LVO -210: DT_GetPropertyString                                     */
/* ------------------------------------------------------------------ */

const char *DT_GetPropertyString(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"),
                                 const char *name __asm("a1"))
{
    (void)ddm;
    if (!dev || !name)
        return NULL;
    const struct dt_property *prop = dt_find_property(dev, name);
    if (!prop)
        return NULL;
    return (const char *)prop->value;
}

/* ------------------------------------------------------------------ */
/* LVO -216: DT_GetPropertyU32                                        */
/* ------------------------------------------------------------------ */

int32_t DT_GetPropertyU32(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"), const char *name __asm("a1"),
                          uint32_t *out __asm("d0"))
{
    (void)ddm;
    if (!dev || !name || !out)
        return -1;
    const struct dt_property *prop = dt_find_property(dev, name);
    if (!prop || prop->length < 4)
    {
        DBG_DT("DT: GetPropertyU32 '%s' not found\n", name);
        return -1;
    }
    /* Big-endian (FDT convention) to native */
    *out = ((uint32_t)prop->value[0] << 24) | ((uint32_t)prop->value[1] << 16) | ((uint32_t)prop->value[2] << 8) |
           ((uint32_t)prop->value[3]);
    DBG_DT("DT: GetPropertyU32 '%s' = 0x%08lx\n", name, *out);
    return 0;
}

/* ------------------------------------------------------------------ */
/* LVO -222: DT_GetInterrupt                                          */
/* ------------------------------------------------------------------ */

/* Resolve interrupt #index from dev's DTS properties.
 * Reads the "interrupts" property (array of u32 cells whose count is
 * determined by the interrupt controller's #interrupt-cells property).
 * Reads "interrupt-parent" to find the controller's irq_domain; if not
 * present, walks up the tree to find one via IRQ_FindDomain.
 * Translates the specifier via the domain, allocates a virq via
 * IRQ_AllocVirq, and applies the trigger type via IRQ_SetType.
 * Returns the virq (>= 0), or -1 on failure.
 */
int DT_GetInterrupt(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"), uint32_t index __asm("d0"))
{
    (void)ddm;
    if (!dev)
        return -1;

    /* Read the interrupts property */
    const struct dt_property *prop = dt_find_property(dev, "interrupts");
    if (!prop)
        return -1;

    /* Find the interrupt controller's irq_domain */
    struct irq_domain *domain = NULL;
    struct device *ctrl_dev = NULL;

    /* Check for explicit interrupt-parent property */
    const struct dt_property *parent_prop = dt_find_property(dev, "interrupt-parent");
    if (parent_prop && parent_prop->length >= 4)
    {
        /* The interrupt-parent value is a phandle (device pointer
         * stored as a u32 after phandle resolution). */
        uint32_t phandle = ((uint32_t)parent_prop->value[0] << 24) | ((uint32_t)parent_prop->value[1] << 16) |
                           ((uint32_t)parent_prop->value[2] << 8) | ((uint32_t)parent_prop->value[3]);
        if (phandle != 0)
        {
            ctrl_dev = (struct device *)phandle;
            domain = ctrl_dev->irq_domain;
        }
    }

    /* If no explicit parent, walk up the tree via IRQ_FindDomain. */
    if (!domain)
    {
        domain = IRQ_FindDomain(ddm, dev);
        if (domain)
            ctrl_dev = domain->dev;
    }

    if (!domain || !ctrl_dev)
        return -1;

    /* Read #interrupt-cells from the controller's device tree node.
     * Default to 2 if not specified. */
    uint32_t int_cells = 2;
    const struct dt_property *cells_prop = dt_find_property(ctrl_dev, "#interrupt-cells");
    if (cells_prop && cells_prop->length >= 4)
    {
        int_cells = ((uint32_t)cells_prop->value[0] << 24) | ((uint32_t)cells_prop->value[1] << 16) |
                    ((uint32_t)cells_prop->value[2] << 8) | ((uint32_t)cells_prop->value[3]);
    }

    /* Each interrupt spec is int_cells u32 cells */
    uint32_t spec_size = int_cells * 4;
    uint32_t offset = index * spec_size;
    if (offset + spec_size > prop->length)
        return -1;

    /* Translate the specifier via the domain. If the domain has a
     * custom translate, use it; otherwise use the framework's
     * default 2-cell <hwirq trigger> translation. */
    uint32_t hwirq = 0;
    uint32_t trigger_type = 0; /* IRQ_TYPE_NONE */
    int32_t ret;
    if (domain->translate)
        ret = domain->translate(domain, &prop->value[offset], int_cells, &hwirq, &trigger_type);
    else
        ret = default_translate(domain, &prop->value[offset], int_cells, &hwirq, &trigger_type);
    if (ret < 0)
        return -1;

    /* Allocate a virq for this hwirq in the domain. */
    int virq = IRQ_AllocVirq(ddm, domain, hwirq);
    if (virq < 0)
        return -1;

    /* Apply the trigger type if one was specified. */
    if (trigger_type != IRQ_TYPE_NONE)
        IRQ_SetType(ddm, virq, trigger_type);

    return virq;
}

/* ------------------------------------------------------------------ */
/* LVO -228: DT_FreeInterrupt                                         */
/* ------------------------------------------------------------------ */

void DT_FreeInterrupt(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"))
{
    IRQ_DisposeMapping(ddm, virq);
}
