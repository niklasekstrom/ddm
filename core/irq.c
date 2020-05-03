#define DDM_INTERNAL
/*
 * Interrupt framework - part of ddm.library
 *
 * Implements the Linux-style irq_chip / irq_domain / irq_desc model
 * adapted to AmigaOS. See include/irq.h for the data structures.
 *
 *   - A global virq space (DDMBase->irq_descs, NR_IRQS entries) maps
 *     small integers to struct irq_desc. Drivers and DT_GetInterrupt
 *     work in virqs; hwirq is internal to the framework + chip.
 *   - Each irq_domain owns a linear hwirq->virq reverse map. virqs are
 *     allocated lazily by IRQ_AllocVirq (irq_create_mapping).
 *   - Generic flow handlers (handle_edge_irq / handle_level_irq /
 *     handle_fasteoi_irq / handle_simple_irq) implement the
 *     ack/mask/dispatch/unmask sequence from the trigger type. A
 *     cascaded virq's flow handler is overridden by a driver chained
 *     handler set via IRQ_SetChainedHandler.
 *   - Shared interrupts: each irq_desc holds a struct List of AmigaOS
 *     struct Interrupt nodes (is_Node.ln_Pri orders dispatch).
 *
 * These functions are exposed as LVOs of ddm.library (-120..-192).
 * The virq-space state lives on DDMBase, so (unlike the old
 * stateless implementation) the LVOs need the ddm argument.
 */
#include <clib/alib_protos.h> /* NewList, Enqueue, Remove */
#include <exec/interrupts.h>
#include <exec/libraries.h>
#include <exec/lists.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/exec.h>

#include "ddm_debug.h"

#include "ddm.h" /* struct device, struct DDMBase, NR_IRQS */
#include "irq.h" /* struct irq_chip, irq_domain, irq_desc, IRQ_TYPE_* */

/* ------------------------------------------------------------------ */
/* Internal state                                                     */
/* ------------------------------------------------------------------ */

/* Set by ddm_init (romtag.c) so framework-owned flow handlers and
 * trampolines can reach the virq table without a6 = DDMBase. Driver
 * ISRs use their own cached DDMBase to call LVOs. */
extern struct DDMBase *DDMBase;

/* Forward decl: used by irq_alloc before its definition below. */
irq_flow_handler_t irq_select_flow_handler(uint32_t trigger_type);

/* ------------------------------------------------------------------ */
/* Default device-tree translation (2-cell <hwirq trigger>)          */
/* ------------------------------------------------------------------ */

int32_t default_translate(struct irq_domain *domain __asm("a0"), const uint8_t *cells __asm("a1"),
                          uint32_t ncells __asm("d0"), uint32_t *hwirq __asm("a2"), uint32_t *trigger_type __asm("d1"))
{
    (void)domain;
    if (ncells < 1 || !cells)
        return -1;

    /* Cells are big-endian u32. */
    *hwirq = ((uint32_t)cells[0] << 24) | ((uint32_t)cells[1] << 16) | ((uint32_t)cells[2] << 8) | ((uint32_t)cells[3]);
    *trigger_type = 0;
    if (ncells >= 2)
    {
        *trigger_type =
            ((uint32_t)cells[4] << 24) | ((uint32_t)cells[5] << 16) | ((uint32_t)cells[6] << 8) | ((uint32_t)cells[7]);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* virq allocation                                                     */
/* ------------------------------------------------------------------ */

/* Allocate a free virq slot and a fresh irq_desc bound to (domain,
 * hwirq). Returns the virq, or -1 if the table is full. The desc's
 * flow handler is selected from trigger_type; callers may override
 * it (e.g. IRQ_SetChainedHandler). */
static int irq_alloc(struct DDMBase *ddm, struct irq_domain *domain, uint32_t hwirq, uint32_t trigger_type)
{
    if (!ddm || !ddm->irq_descs)
        return -1;

    /* Linear scan from the hint for a free slot. */
    uint32_t i;
    for (i = 0; i < ddm->nr_irqs; i++)
    {
        uint32_t idx = (ddm->next_virq + i) % ddm->nr_irqs;
        if (!ddm->irq_descs[idx])
        {
            struct irq_desc *desc = (struct irq_desc *)AllocMem(sizeof(struct irq_desc), MEMF_ANY | MEMF_CLEAR);
            if (!desc)
                return -1;

            desc->chip = domain ? domain->chip : NULL;
            desc->domain = domain;
            desc->hwirq = hwirq;
            desc->trigger_type = trigger_type;
            desc->disable_depth = 0;
            desc->status = 0;
            desc->handler_count = 0;
            NewList(&desc->handlers);

            /* Select the flow handler from the trigger type. */
            desc->handle_irq = irq_select_flow_handler(trigger_type);

            ddm->irq_descs[idx] = desc;
            ddm->next_virq = (idx + 1) % ddm->nr_irqs;
            return (int)idx;
        }
    }
    return -1; /* table full */
}

/* Free a virq slot and its desc. Caller must have removed all
 * handlers and cleared any chained handler. */
static void irq_free(struct DDMBase *ddm, int virq)
{
    if (!ddm || virq < 0 || (uint32_t)virq >= ddm->nr_irqs)
        return;
    struct irq_desc *desc = ddm->irq_descs[virq];
    if (!desc)
        return;
    ddm->irq_descs[virq] = NULL;
    FreeMem(desc, sizeof(struct irq_desc));
}

/* Look up the desc for a virq. NULL if invalid/unallocated. */
static struct irq_desc *irq_get_desc(struct DDMBase *ddm, int virq)
{
    if (!ddm || virq < 0 || (uint32_t)virq >= ddm->nr_irqs)
        return NULL;
    return ddm->irq_descs[virq];
}

/* ------------------------------------------------------------------ */
/* Generic flow handlers                                              */
/* ------------------------------------------------------------------ */

/* Dispatch the handler list for a desc. Handlers are AmigaOS struct
 * Interrupt nodes; is_Code is called with is_Data in a1. The list is
 * kept in ln_Pri order by Enqueue at add time, so we walk it head to
 * tail. */
static void irq_dispatch_handlers(struct irq_desc *desc)
{
    struct Interrupt *isr;
    struct Node *node;

    for (node = desc->handlers.lh_Head; node->ln_Succ; node = node->ln_Succ)
    {
        isr = (struct Interrupt *)node;
        if (isr->is_Code)
        {
            /* AmigaOS interrupt convention: is_Code is called with
             * is_Data in a1. Use __asm("a1") to match handlers that
             * declare their data parameter with __asm("a1"). */
            void (*code)(void *__asm("a1")) = (void (*)(void *__asm("a1")))isr->is_Code;
            code(isr->is_Data);
        }
    }
}

/* Edge: ack then dispatch. No mask (edge interrupts are not masked
 * during dispatch; a new edge may be latched by the hardware). */
static void handle_edge_irq(struct irq_desc *desc __asm("a0"))
{
    if (!desc || !desc->chip)
        return;
    if (desc->chip->ack)
        desc->chip->ack(desc->chip, desc->hwirq);
    irq_dispatch_handlers(desc);
}

/* Level: mask, ack, dispatch, then unmask (or eoi). Masking prevents
 * re-entry while the handler runs. */
static void handle_level_irq(struct irq_desc *desc __asm("a0"))
{
    if (!desc || !desc->chip)
        return;
    if (desc->chip->mask)
        desc->chip->mask(desc->chip, desc->hwirq);
    if (desc->chip->ack)
        desc->chip->ack(desc->chip, desc->hwirq);
    irq_dispatch_handlers(desc);
    if (desc->chip->eoi)
        desc->chip->eoi(desc->chip, desc->hwirq);
    else if (desc->chip->unmask)
        desc->chip->unmask(desc->chip, desc->hwirq);
}

/* Fast-EOI: dispatch then eoi. The hardware holds the line masked
 * until EOI. */
static void __attribute__((unused)) handle_fasteoi_irq(struct irq_desc *desc __asm("a0"))
{
    if (!desc || !desc->chip)
        return;
    irq_dispatch_handlers(desc);
    if (desc->chip->eoi)
        desc->chip->eoi(desc->chip, desc->hwirq);
    else if (desc->chip->unmask)
        desc->chip->unmask(desc->chip, desc->hwirq);
}

/* Simple: dispatch only. Used for cascaded/chained virqs whose ack/
 * mask is handled by the chained handler, and for controllers with
 * no flow requirements. */
static void handle_simple_irq(struct irq_desc *desc __asm("a0"))
{
    if (!desc)
        return;
    irq_dispatch_handlers(desc);
}

/* Select a flow handler for a trigger type. Falls back to
 * handle_simple_irq for IRQ_TYPE_NONE (the caller will typically
 * override it via IRQ_SetChainedHandler or set_type later). */
irq_flow_handler_t irq_select_flow_handler(uint32_t trigger_type)
{
    if (trigger_type & (IRQ_TYPE_EDGE_FALLING | IRQ_TYPE_EDGE_RISING))
        return handle_edge_irq;
    if (trigger_type & (IRQ_TYPE_LEVEL_LOW | IRQ_TYPE_LEVEL_HIGH))
        return handle_level_irq;
    return handle_simple_irq;
}

/* ------------------------------------------------------------------ */
/* Chip enable/disable helpers                                        */
/* ------------------------------------------------------------------ */

static void chip_enable(struct irq_chip *chip, uint32_t hwirq)
{
    if (!chip)
        return;
    if (chip->enable)
        chip->enable(chip, hwirq);
    else if (chip->unmask)
        chip->unmask(chip, hwirq);
}

static void chip_disable(struct irq_chip *chip, uint32_t hwirq)
{
    if (!chip)
        return;
    if (chip->disable)
        chip->disable(chip, hwirq);
    else if (chip->mask)
        chip->mask(chip, hwirq);
}

/* Startup: called on first handler. Falls back to enable. */
static void chip_startup(struct irq_chip *chip, uint32_t hwirq)
{
    if (!chip)
        return;
    if (chip->startup)
        chip->startup(chip, hwirq);
    else
        chip_enable(chip, hwirq);
}

/* Shutdown: called on last handler. Falls back to disable. */
static void chip_shutdown(struct irq_chip *chip, uint32_t hwirq)
{
    if (!chip)
        return;
    if (chip->shutdown)
        chip->shutdown(chip, hwirq);
    else
        chip_disable(chip, hwirq);
}

/* ------------------------------------------------------------------ */
/* LVO -120: IRQ_CreateDomain                                         */
/* ------------------------------------------------------------------ */

struct irq_domain *IRQ_CreateDomain(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"),
                                    struct irq_chip *chip __asm("a1"), uint32_t max_irq __asm("d0"))
{
    if (!ddm || !dev || !chip || max_irq == 0)
        return NULL;
    if (dev->irq_domain)
        return NULL; /* already a controller */

    struct irq_domain *domain = (struct irq_domain *)AllocMem(sizeof(struct irq_domain), MEMF_ANY | MEMF_CLEAR);
    if (!domain)
        return NULL;

    int *revmap = (int *)AllocMem(max_irq * sizeof(int), MEMF_ANY | MEMF_CLEAR);
    if (!revmap)
    {
        FreeMem(domain, sizeof(struct irq_domain));
        return NULL;
    }
    /* Mark all hwirqs unmapped. memset 0 above left them 0, so set to -1. */
    uint32_t i;
    for (i = 0; i < max_irq; i++)
        revmap[i] = -1;

    domain->dev = dev;
    domain->chip = chip;
    domain->parent = NULL;
    domain->translate = NULL; /* default_translate used when NULL */
    domain->max_irq = max_irq;
    domain->revmap = revmap;

    chip->dev = dev;
    dev->irq_domain = domain;
    DBG_IRQ("IRQ: create domain dev='%s' max_irq=%lu\n", dev->node.ln_Name ? dev->node.ln_Name : "(unnamed)", max_irq);
    return domain;
}

/* ------------------------------------------------------------------ */
/* LVO -126: IRQ_FindDomain                                           */
/* ------------------------------------------------------------------ */

struct irq_domain *IRQ_FindDomain(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"))
{
    (void)ddm;
    while (dev)
    {
        if (dev->irq_domain)
            return dev->irq_domain;
        dev = dev->parent;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* LVO -168: IRQ_AllocVirq                                           */
/* ------------------------------------------------------------------ */

/* Allocate (or look up) a virq for hwirq within domain. Mirrors
 * Linux's irq_create_mapping. */
int IRQ_AllocVirq(struct DDMBase *ddm __asm("a6"), struct irq_domain *domain __asm("a0"), uint32_t hwirq __asm("d0"))
{
    if (!ddm || !domain || hwirq >= domain->max_irq)
        return -1;

    /* Already mapped? */
    if (domain->revmap[hwirq] >= 0)
        return domain->revmap[hwirq];

    int virq = irq_alloc(ddm, domain, hwirq, IRQ_TYPE_NONE);
    if (virq < 0)
    {
        DBG_IRQ("IRQ: ERR: alloc virq failed hwirq=%lu\n", hwirq);
        return -1;
    }
    domain->revmap[hwirq] = virq;
    DBG_IRQ("IRQ: alloc virq=%ld hwirq=%lu\n", (long)virq, hwirq);
    return virq;
}

/* ------------------------------------------------------------------ */
/* LVO -174: IRQ_DisposeMapping                                       */
/* ------------------------------------------------------------------ */

void IRQ_DisposeMapping(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"))
{
    if (!ddm)
        return;
    struct irq_desc *desc = irq_get_desc(ddm, virq);
    if (!desc)
        return;

    /* If handlers remain, keep the desc (just unmap). Otherwise free.
     *
     * With-handlers semantics: if handlers remain, the desc stays
     * alive but desc->domain is NULLed and the revmap entry is
     * cleared. The handlers will still fire (the chip pointer is
     * retained) but IRQ_FindDomain won't find this virq anymore.
     * This is by design — the caller should remove handlers before
     * disposing the mapping. */
    if (desc->handler_count == 0)
    {
        if (desc->domain && desc->hwirq < desc->domain->max_irq)
            desc->domain->revmap[desc->hwirq] = -1;
        irq_free(ddm, virq);
    }
    else
    {
        /* Unmap from the domain but keep the desc alive for the
         * remaining handlers. */
        if (desc->domain && desc->hwirq < desc->domain->max_irq)
            desc->domain->revmap[desc->hwirq] = -1;
        desc->domain = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* LVO -180: IRQ_CreateHierarchy                                      */
/* ------------------------------------------------------------------ */

struct irq_domain *IRQ_CreateHierarchy(struct DDMBase *ddm __asm("a6"), struct irq_domain *parent_domain __asm("a0"),
                                       struct device *dev __asm("a1"), struct irq_chip *chip __asm("a2"),
                                       uint32_t max_irq __asm("d0"))
{
    if (!ddm || !parent_domain || !dev || !chip || max_irq == 0)
        return NULL;

    struct irq_domain *child = IRQ_CreateDomain(ddm, dev, chip, max_irq);
    if (!child)
        return NULL;
    child->parent = parent_domain;
    return child;
}

/* ------------------------------------------------------------------ */
/* LVO -186: IRQ_SetChainedHandler                                    */
/* ------------------------------------------------------------------ */

void IRQ_SetChainedHandler(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"),
                           irq_chained_handler_t handler __asm("a0"), void *data __asm("a1"))
{
    if (!ddm)
        return;
    struct irq_desc *desc = irq_get_desc(ddm, virq);
    if (!desc)
        return;

    if (handler)
    {
        /* Start the parent interrupt if this is the first time we're
         * chaining it. This mirrors Linux's irq_set_chained_handler,
         * which calls irq_startup(). Without this, controllers that
         * register their hardware interrupt in chip_startup (e.g.
         * the CIA, which calls AddICRVector) never get enabled when
         * used as a chained interrupt parent. */
        if (!(desc->status & IRQ_DESC_CHAINED))
            chip_startup(desc->chip, desc->hwirq);

        desc->chained_handler = handler;
        desc->chained_data = data;
        desc->status |= IRQ_DESC_CHAINED;
        /* The flow handler now just invokes the chained handler. */
        desc->handle_irq = NULL;
    }
    else
    {
        /* Shut down the parent interrupt if it was previously chained,
         * so that chip_startup/chip_shutdown stay balanced. This
         * unregisters the hardware interrupt (e.g. RemICRVector for
         * the CIA) before the desc is freed by IRQ_DisposeMapping. */
        if (desc->status & IRQ_DESC_CHAINED)
            chip_shutdown(desc->chip, desc->hwirq);

        desc->chained_handler = NULL;
        desc->chained_data = NULL;
        desc->status &= ~IRQ_DESC_CHAINED;
        desc->handle_irq = irq_select_flow_handler(desc->trigger_type);
    }
}

/* ------------------------------------------------------------------ */
/* LVO -192: IRQ_GenericHandleIrq                                     */
/* ------------------------------------------------------------------ */

/* Dispatch entry from an ISR. Runs the flow handler (or the chained
 * handler) for virq. This is the AmigaOS analogue of Linux's
 * generic_handle_irq(). */
void IRQ_GenericHandleIrq(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"))
{
    if (!ddm)
        return;
    struct irq_desc *desc = irq_get_desc(ddm, virq);
    if (!desc)
    {
        DBG_IRQ("IRQ: GenericHandleIrq virq=%ld no desc\n", (long)virq);
        return;
    }

    if (desc->status & IRQ_DESC_CHAINED)
    {
        if (desc->chained_handler)
            desc->chained_handler(desc->chained_data);
    }
    else if (desc->handle_irq)
    {
        desc->handle_irq(desc);
    }
    else
    {
        /* No flow handler set: default to simple dispatch. */
        irq_dispatch_handlers(desc);
    }
}

/* ------------------------------------------------------------------ */
/* LVO -132: IRQ_AddHandler                                           */
/* ------------------------------------------------------------------ */

/* Add a handler to a virq. `flags` carries the requested trigger type;
 * the framework validates it via the chip's set_type (if any). On
 * the first handler the irq is started (set_type + chip enable). */
int32_t IRQ_AddHandler(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"), struct Interrupt *isr __asm("a1"),
                       uint32_t flags __asm("a2"))
{
    if (!ddm || !isr)
        return -1;
    struct irq_desc *desc = irq_get_desc(ddm, virq);
    if (!desc || !desc->chip)
        return -1;

    /* Validate / apply the trigger type if one is requested. */
    if (flags != IRQ_TYPE_NONE)
    {
        if (desc->chip->set_type)
        {
            int32_t ret = desc->chip->set_type(desc->chip, desc->hwirq, flags);
            if (ret < 0)
                return -2; /* type not supported */
        }
        desc->trigger_type = flags;
        /* Re-select the flow handler for the new type, unless this
         * virq is chained (chained handler overrides the flow). */
        if (!(desc->status & IRQ_DESC_CHAINED))
            desc->handle_irq = irq_select_flow_handler(flags);
    }

    /* Enqueue in priority order (is_Node.ln_Pri). */
    isr->is_Node.ln_Type = NT_INTERRUPT;
    Enqueue(&desc->handlers, &isr->is_Node);
    desc->handler_count++;

    DBG_IRQ("IRQ: add handler virq=%ld hwirq=%lu count=%lu type=%lu\n", (long)virq, desc->hwirq, desc->handler_count,
            flags);

    /* Start the irq on the first handler, unless NOAUTOEN. */
    if (desc->handler_count == 1 && !(desc->status & IRQ_DESC_NOAUTOEN))
    {
        chip_startup(desc->chip, desc->hwirq);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* LVO -138: IRQ_RemoveHandler                                        */
/* ------------------------------------------------------------------ */

void IRQ_RemoveHandler(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"), struct Interrupt *isr __asm("a1"))
{
    if (!ddm || !isr)
        return;
    struct irq_desc *desc = irq_get_desc(ddm, virq);
    if (!desc)
        return;

    Remove(&isr->is_Node);
    if (desc->handler_count > 0)
        desc->handler_count--;

    /* Shut down the irq on the last handler. */
    if (desc->handler_count == 0)
        chip_shutdown(desc->chip, desc->hwirq);
}

/* ------------------------------------------------------------------ */
/* LVO -144: IRQ_Enable                                               */
/* ------------------------------------------------------------------ */

void IRQ_Enable(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"))
{
    if (!ddm)
        return;
    struct irq_desc *desc = irq_get_desc(ddm, virq);
    if (!desc)
        return;
    if (desc->disable_depth > 0)
        desc->disable_depth--;
    if (desc->disable_depth == 0)
    {
        desc->status &= ~IRQ_DESC_DISABLED;
        chip_enable(desc->chip, desc->hwirq);
        DBG_IRQ("IRQ: enable virq=%ld hwirq=%lu\n", (long)virq, desc->hwirq);
    }
}

/* ------------------------------------------------------------------ */
/* LVO -150: IRQ_Disable                                              */
/* ------------------------------------------------------------------ */

void IRQ_Disable(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"))
{
    if (!ddm)
        return;
    struct irq_desc *desc = irq_get_desc(ddm, virq);
    if (!desc)
        return;
    if (desc->disable_depth == 0)
    {
        desc->status |= IRQ_DESC_DISABLED;
        chip_disable(desc->chip, desc->hwirq);
        DBG_IRQ("IRQ: disable virq=%ld hwirq=%lu\n", (long)virq, desc->hwirq);
    }
    desc->disable_depth++;
}

/* ------------------------------------------------------------------ */
/* LVO -156: IRQ_SetType                                              */
/* ------------------------------------------------------------------ */

int32_t IRQ_SetType(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"), uint32_t trigger_type __asm("a1"))
{
    if (!ddm)
        return -1;
    struct irq_desc *desc = irq_get_desc(ddm, virq);
    if (!desc || !desc->chip)
        return -1;
    if (desc->chip->set_type)
    {
        int32_t ret = desc->chip->set_type(desc->chip, desc->hwirq, trigger_type);
        if (ret < 0)
        {
            DBG_IRQ("IRQ: ERR: set_type failed virq=%ld type=%lu\n", (long)virq, trigger_type);
            return ret;
        }
    }
    desc->trigger_type = trigger_type;
    if (!(desc->status & IRQ_DESC_CHAINED))
        desc->handle_irq = irq_select_flow_handler(trigger_type);
    DBG_IRQ("IRQ: set_type virq=%ld type=%lu\n", (long)virq, trigger_type);
    return 0;
}

/* ------------------------------------------------------------------ */
/* LVO -162: IRQ_DestroyDomain                                        */
/* ------------------------------------------------------------------ */

void IRQ_DestroyDomain(struct DDMBase *ddm __asm("a6"), struct irq_domain *domain __asm("a0"))
{
    if (!ddm || !domain)
        return;

    /* Dispose all mappings in this domain. */
    uint32_t i;
    for (i = 0; i < domain->max_irq; i++)
    {
        int virq = domain->revmap[i];
        if (virq >= 0)
        {
            struct irq_desc *desc = irq_get_desc(ddm, virq);
            if (desc)
            {
                /* Force-remove any handlers and shut down the irq. */
                if (desc->handler_count > 0)
                {
                    struct Node *node = desc->handlers.lh_Head;
                    while (node->ln_Succ)
                    {
                        struct Node *next = node->ln_Succ;
                        Remove(node);
                        node = next;
                    }
                    desc->handler_count = 0;
                    chip_shutdown(desc->chip, desc->hwirq);
                }
                desc->domain = NULL;
                irq_free(ddm, virq);
            }
            domain->revmap[i] = -1;
        }
    }

    /* Detach from the device. */
    if (domain->dev && domain->dev->irq_domain == domain)
        domain->dev->irq_domain = NULL;

    FreeMem(domain->revmap, domain->max_irq * sizeof(int));
    FreeMem(domain, sizeof(struct irq_domain));
}
