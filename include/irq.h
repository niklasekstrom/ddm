/*
 * Interrupt framework for AmigaOS
 *
 * Provides the interrupt controller abstraction and handler
 * management, modelled on Linux's irq_chip / irq_domain / irq_desc
 * trinity, adapted to AmigaOS.
 *
 *   struct irq_chip    - per-controller hardware operations (mask,
 *                        unmask, ack, eoi, set_type, ...). The Linux
 *                        irq_chip, minus the bookkeeping that lives in
 *                        irq_desc.
 *   struct irq_domain  - a controller's translation + hwirq->virq
 *                        mapping. The Linux irq_domain. Domains form a
 *                        hierarchy via ->parent for cascaded
 *                        controllers (GPIO banks, spider, ...).
 *   struct irq_desc    - per-virq state: the bound chip + domain +
 *                        hwirq, the flow handler, the handler list
 *                        (shared interrupts), and a disable depth.
 *
 * Drivers never see hwirq. Interrupts are identified by a small
 * non-negative integer (virq) allocated from DDMBase->irq_descs.
 * DT_GetInterrupt() returns a virq; IRQ_AddHandler() etc. take a
 * virq.
 *
 * These operations are exposed as LVOs of ddm.library (-120..-192),
 * not a separate irq.library. Callers pass their ddm.library base
 * (struct DDMBase *) as the first argument.
 */
#ifndef IRQ_H_
#define IRQ_H_

#include "ddm.h"             /* struct device */
#include "ddm_gcc.h"         /* LVO stub macros */
#include <dos/dos.h>         /* BPTR */
#include <exec/interrupts.h> /* struct Interrupt */
#include <exec/libraries.h>
#include <exec/lists.h> /* struct List */
#include <exec/types.h>

/* ------------------------------------------------------------------ */
/* Interrupt trigger types (matching Linux linux/irq.h)               */
/* ------------------------------------------------------------------ */

#define IRQ_TYPE_NONE 0x00000000
#define IRQ_TYPE_EDGE_FALLING 0x00000002
#define IRQ_TYPE_EDGE_RISING 0x00000001
#define IRQ_TYPE_LEVEL_LOW 0x00000008
#define IRQ_TYPE_LEVEL_HIGH 0x00000004
#define IRQ_TYPE_EDGE_BOTH (IRQ_TYPE_EDGE_FALLING | IRQ_TYPE_EDGE_RISING)

/* ------------------------------------------------------------------ */
/* Forward declarations                                               */
/* ------------------------------------------------------------------ */

struct irq_chip;
struct irq_domain;
struct irq_desc;

/* Default 2-cell <hwirq trigger> device-tree translation. Exported
 * so devicetree.c can use it instead of duplicating the logic. */
int32_t default_translate(struct irq_domain *domain __asm("a0"), const uint8_t *cells __asm("a1"),
                          uint32_t ncells __asm("d0"), uint32_t *hwirq __asm("a2"), uint32_t *trigger_type __asm("d1"));

/* ------------------------------------------------------------------ */
/* irq_chip - per-controller hardware operations                      */
/* ------------------------------------------------------------------ */

/* An irq_chip is the low-level hardware interface for one interrupt
 * controller, mirroring Linux's struct irq_chip. It carries no
 * per-irq state; that lives in struct irq_desc. All callbacks receive
 * the chip's own data pointer (chip_data) and the hardware irq. */
struct irq_chip
{
    const char *name;
    struct device *dev; /* Device this controller belongs to */
    void *chip_data;    /* Controller-private data (e.g. ciaabase) */

    /* Acknowledge (clear) the interrupt at the hardware. Called by
     * the flow handler before dispatch for edge interrupts. May be NULL
     * if the hardware auto-clears (e.g. CIA FLAG edge). */
    void (*ack)(struct irq_chip *chip __asm("a0"), uint32_t hwirq __asm("d0"));

    /* Mask / unmask a single interrupt line. NULL means the line is
     * always enabled at this controller (e.g. the shared Amiga INT6). */
    void (*mask)(struct irq_chip *chip __asm("a0"), uint32_t hwirq __asm("d0"));
    void (*unmask)(struct irq_chip *chip __asm("a0"), uint32_t hwirq __asm("d0"));

    /* End-of-interrupt for level controllers that require an explicit
     * EOI rather than unmask. If NULL, the flow handler uses unmask. */
    void (*eoi)(struct irq_chip *chip __asm("a0"), uint32_t hwirq __asm("d0"));

    /* Enable / disable the line. Defaults to mask/unmask if NULL. */
    void (*enable)(struct irq_chip *chip __asm("a0"), uint32_t hwirq __asm("d0"));
    void (*disable)(struct irq_chip *chip __asm("a0"), uint32_t hwirq __asm("d0"));

    /* Startup / shutdown: called once on first handler / last handler
     * removal, respectively. Use these to register/unregister an ISR
     * with the underlying Amiga interrupt resource (e.g. AddICRVector
     * / RemICRVector for CIA). If NULL, the framework falls back to
     * enable / disable. This mirrors Linux's irq_startup / irq_shutdown
     * vs. irq_enable / irq_disable (mask/unmask). */
    void (*startup)(struct irq_chip *chip __asm("a0"), uint32_t hwirq __asm("d0"));
    void (*shutdown)(struct irq_chip *chip __asm("a0"), uint32_t hwirq __asm("d0"));

    /* Configure the trigger type. Returns 0 on success, negative if
     * the requested type is not supported by this controller/line.
     * This replaces the old supported_types bitmask: the framework
     * validates a requested type by calling set_type, exactly as
     * Linux's irq_set_irq_type() does. May be NULL for controllers
     * with a fixed, unconfigurable type (the framework then accepts
     * any type at alloc time and relies on the hardware behaviour). */
    int32_t (*set_type)(struct irq_chip *chip __asm("a0"), uint32_t hwirq __asm("d0"),
                        uint32_t trigger_type __asm("d1"));
};

/* ------------------------------------------------------------------ */
/* irq_domain - translation + hwirq->virq mapping                     */
/* ------------------------------------------------------------------ */

/* An irq_domain binds an irq_chip to a device and provides the
 * device-tree interrupt specifier translation, mirroring Linux's
 * struct irq_domain. Domains form a hierarchy via ->parent for
 * cascaded controllers: a child domain's parent virq is dispatched
 * by the parent's flow handler, which calls IRQ_GenericHandleIrq()
 * on the child's mapped virq (see IRQ_SetChainedHandler).
 *
 * The domain owns a linear hwirq->virq map of size max_irq. virqs are
 * allocated lazily by IRQ_AllocVirq() (irq_create_mapping). */
struct irq_domain
{
    struct device *dev;        /* Device this domain is attached to */
    struct irq_chip *chip;     /* Chip for this domain */
    struct irq_domain *parent; /* Parent domain (NULL for a root domain) */

    /* Translate a device-tree interrupt specifier into (hwirq,
     * trigger_type). `cells` is the raw cell array from the
     * "interrupts" property (big-endian u32 each), `ncells` is the
     * number of cells (from #interrupt-cells). Returns 0 on success,
     * negative on a malformed specifier. If NULL, the framework uses
     * the default 2-cell <hwirq trigger> translation. */
    int32_t (*translate)(struct irq_domain *domain __asm("a0"), const uint8_t *cells __asm("a1"),
                         uint32_t ncells __asm("d0"), uint32_t *hwirq __asm("a2"), uint32_t *trigger_type __asm("d1"));

    uint32_t max_irq; /* Size of the hwirq->virq map */
    int *revmap;      /* hwirq -> virq, or -1 if unmapped.
                       * Allocated with the domain. */
};

/* ------------------------------------------------------------------ */
/* irq_desc - per-virq state                                          */
/* ------------------------------------------------------------------ */

/* One irq_desc per allocated virq. Carries the bound chip/domain/hwirq,
 * the flow handler that implements the ack/mask/dispatch sequence for
 * the trigger type, the list of registered handlers (shared
 * interrupts), and a disable depth for nested IRQ_Disable/IRQ_Enable.
 *
 * The handler list is a struct List of AmigaOS struct Interrupt nodes
 * (their is_Node is a struct Node carrying ln_Pri for priority
 * ordering). This reuses the existing AmigaOS interrupt server
 * abstraction. */

/* Flow handler: implements the ack/mask/dispatch/unmask sequence for
 * the desc's trigger type. Set at alloc time from the trigger type,
 * or overridden by IRQ_SetChainedHandler for a cascaded virq. */
typedef void (*irq_flow_handler_t)(struct irq_desc *desc __asm("a0"));

/* Chained handler: a driver-supplied callback invoked in place of the
 * generic flow handler for a cascaded virq. The handler typically
 * inspects a status register, acks the child controller, and calls
 * IRQ_GenericHandleIrq() on the mapped child virq. `data` is the
 * caller-supplied opaque pointer from IRQ_SetChainedHandler. */
typedef void (*irq_chained_handler_t)(void *data __asm("a0"));

struct irq_desc
{
    struct irq_chip *chip;     /* Chip servicing this virq */
    struct irq_domain *domain; /* Domain that allocated this virq */
    uint32_t hwirq;            /* Hardware irq within the chip */
    uint32_t trigger_type;     /* Configured trigger type (IRQ_TYPE_*) */

    irq_flow_handler_t handle_irq;         /* Flow handler (edge/level/...) */
    irq_chained_handler_t chained_handler; /* Driver chained handler */
    void *chained_data;                    /* Data for chained_handler */

    struct List handlers;   /* List of struct Interrupt (shared) */
    uint32_t handler_count; /* Number of handlers in the list */

    int32_t disable_depth; /* 0 = enabled; >0 = disabled (nested) */
    uint32_t status;       /* IRQ_DESC_* flags */
    void *chip_data;       /* Per-irq controller-private data */
};

/* irq_desc status flags */
#define IRQ_DESC_DISABLED 0x01 /* disable_depth > 0 */
#define IRQ_DESC_CHAINED 0x02  /* handle_irq is a chained handler */
#define IRQ_DESC_NOAUTOEN 0x04 /* do not auto-enable at add_handler */

/* ------------------------------------------------------------------ */
/* LVO prototypes                                                      */
/*                                                                    */
/* These declare the real LVO functions implemented in core/irq.c.    */
/* External callers use the macro stubs below; implementation files   */
/* define DDM_INTERNAL to suppress the macros.                        */
/* ------------------------------------------------------------------ */

/* LVO -120: Create an irq_domain for `chip` on `dev`, with a linear
 * hwirq->virq map of `max_irq` entries. Sets dev->irq_domain. Returns
 * the domain, or NULL on failure (dev/chip NULL, max_irq 0, or
 * dev already has a domain). */
struct irq_domain *IRQ_CreateDomain(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"),
                                    struct irq_chip *chip __asm("a1"), uint32_t max_irq __asm("d0"));

/* LVO -126: Find the nearest irq_domain by walking up the device tree
 * from dev (including dev itself). Returns the domain, or NULL. */
struct irq_domain *IRQ_FindDomain(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"));

/* LVO -132: Add a handler to a resolved virq. `flags` carries the
 * requested trigger type; the framework validates it via the chip's
 * set_type (if any). On the first handler the irq is started (chip
 * enable + set_type). Returns 0 on success, negative on error. */
int32_t IRQ_AddHandler(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"), struct Interrupt *isr __asm("a1"),
                       uint32_t flags __asm("a2"));

/* LVO -138: Remove a handler from a virq. On the last handler the irq
 * is shut down (chip disable). */
void IRQ_RemoveHandler(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"), struct Interrupt *isr __asm("a1"));

/* LVO -144: Enable a virq (decrements disable_depth; unmask when it
 * reaches 0). */
void IRQ_Enable(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"));

/* LVO -150: Disable a virq (increments disable_depth; mask on 0->1). */
void IRQ_Disable(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"));

/* LVO -156: Configure the trigger type for a virq. Calls the chip's
 * set_type and persists the type into the desc. Returns 0 on success,
 * negative if not supported. */
int32_t IRQ_SetType(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"), uint32_t trigger_type __asm("a1"));

/* LVO -162: Destroy an irq_domain. Disposes all mappings, clears
 * dev->irq_domain, and frees the domain + revmap. The caller must
 * have removed all handlers first. */
void IRQ_DestroyDomain(struct DDMBase *ddm __asm("a6"), struct irq_domain *domain __asm("a0"));

/* LVO -168: Allocate (or look up) a virq for `hwirq` within `domain`
 * (irq_create_mapping). Returns the virq, or -1 on failure. */
int IRQ_AllocVirq(struct DDMBase *ddm __asm("a6"), struct irq_domain *domain __asm("a0"), uint32_t hwirq __asm("d0"));

/* LVO -174: Dispose of a virq mapping (irq_dispose_mapping). Frees the
 * irq_desc if the handler list is empty; otherwise just decrements
 * the refcount. No-op if virq is invalid. */
void IRQ_DisposeMapping(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"));

/* LVO -180: Create a child irq_domain hierarchically under
 * `parent_domain`, for `chip` on `dev`, with a linear map of `max_irq`.
 * The child inherits translation from the parent if its own
 * translate is NULL. Returns the domain, or NULL on failure. */
struct irq_domain *IRQ_CreateHierarchy(struct DDMBase *ddm __asm("a6"), struct irq_domain *parent_domain __asm("a0"),
                                       struct device *dev __asm("a1"), struct irq_chip *chip __asm("a2"),
                                       uint32_t max_irq __asm("d0"));

/* LVO -186: Set a chained handler for `virq`. The chained handler runs
 * in place of the generic flow handler when the parent virq fires;
 * it typically acks a child controller and calls
 * IRQ_GenericHandleIrq() on the mapped child virq. `data` is passed
 * to the handler. Passing NULL handler clears the chained handler
 * and restores the generic flow handler. */
void IRQ_SetChainedHandler(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"),
                           irq_chained_handler_t handler __asm("a0"), void *data __asm("a1"));

/* LVO -192: Dispatch entry for a virq. Called from an ISR (driver or
 * framework trampoline) to run the flow handler for `virq`. This is
 * the AmigaOS analogue of Linux's generic_handle_irq(). */
void IRQ_GenericHandleIrq(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"));

/* ------------------------------------------------------------------ */
/* LVO stub macros                                                    */
/*                                                                    */
/* External callers (drivers, subsystems, other core files) use these  */
/* to make LVO calls through the ddm.library base in a6.              */
/*                                                                    */
/* Implementation files that DEFINE an LVO function must NOT see      */
/* these macros. Define DDM_INTERNAL before including this header     */
/* to suppress the macro definitions.                                 */
/* ------------------------------------------------------------------ */

#ifndef DDM_INTERNAL

#define IRQ_CreateDomain(ddm, dev, chip, max_irq)                                                                      \
    __DDM_LVO_RET_3A0A1D0(struct irq_domain *, -120, (ddm), (dev), (chip), (max_irq))
#define IRQ_FindDomain(ddm, dev) __DDM_LVO_RET_1A0(struct irq_domain *, -126, (ddm), (dev))
#define IRQ_AddHandler(ddm, virq, isr, flags) __DDM_LVO_RET_3D0A1A2(int32_t, -132, (ddm), (virq), (isr), (flags))
#define IRQ_RemoveHandler(ddm, virq, isr) __DDM_LVO_VOID_2D0A1(-138, (ddm), (virq), (isr))
#define IRQ_Enable(ddm, virq) __DDM_LVO_VOID_1D0(-144, (ddm), (virq))
#define IRQ_Disable(ddm, virq) __DDM_LVO_VOID_1D0(-150, (ddm), (virq))
#define IRQ_SetType(ddm, virq, trigger_type) __DDM_LVO_RET_2D0A1(int32_t, -156, (ddm), (virq), (trigger_type))
#define IRQ_DestroyDomain(ddm, domain) __DDM_LVO_VOID_1A0(-162, (ddm), (domain))
#define IRQ_AllocVirq(ddm, domain, hwirq) __DDM_LVO_RET_2A0D0(int, -168, (ddm), (domain), (hwirq))
#define IRQ_DisposeMapping(ddm, virq) __DDM_LVO_VOID_1D0(-174, (ddm), (virq))
#define IRQ_CreateHierarchy(ddm, parent_domain, dev, chip, max_irq)                                                    \
    __DDM_LVO_RET_4A0A1A2D0(struct irq_domain *, -180, (ddm), (parent_domain), (dev), (chip), (max_irq))
#define IRQ_SetChainedHandler(ddm, virq, handler, data) __DDM_LVO_VOID_3A0A1D0(-186, (ddm), (handler), (data), (virq))
#define IRQ_GenericHandleIrq(ddm, virq) __DDM_LVO_VOID_1D0(-192, (ddm), (virq))

#endif /* !DDM_INTERNAL */

#endif /* IRQ_H_ */
