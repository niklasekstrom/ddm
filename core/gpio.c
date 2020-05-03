#define DDM_INTERNAL
/*
 * GPIO framework implementation - part of ddm.library
 */
#include <clib/alib_protos.h> /* NewList */
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/exec.h>

#include "ddm_debug.h"

#include "ddm.h"        /* struct DDMBase */
#include "devicetree.h" /* DT_GetProperty */
#include "gpio.h"
#include "irq.h" /* IRQ_* LVOs */

extern struct ExecBase *SysBase;
extern struct DDMBase *DDMBase;

/* ------------------------------------------------------------------ */
/* LVO -234: GPIO_RegisterController                                   */
/* ------------------------------------------------------------------ */

/* Framework passthrough chained handler: used when a GPIO controller
 * has an irq_chip but no custom chained_handler (e.g. par-spi, where
 * the parent interrupt auto-clears and there is no status register to
 * check). The controller's private data is passed through.
 *
 * Limitation: without a status register, the framework cannot know
 * which specific pin fired. We therefore dispatch ALL mapped child
 * virqs. This is safe because individual device handlers are expected
 * to check their own status and no-op if not theirs. */
static void gpio_passthrough_chained_handler(void *data __asm("a0"))
{
    struct gpio_controller *ctrl = data;
    if (!ctrl || !ctrl->irq_chip || !ctrl->irq_chip->dev)
        return;

    struct irq_domain *domain = ctrl->irq_chip->dev->irq_domain;
    if (!domain || domain->max_irq == 0)
        return;

    /* If the controller can report which pins have pending IRQs,
     * only dispatch those — avoiding spurious calls to every mapped
     * child handler. */
    if (ctrl->get_irq_status)
    {
        uint32_t status = ctrl->get_irq_status(ctrl);
        uint32_t i;
        for (i = 0; i < domain->max_irq && i < 32; i++)
        {
            if (!(status & (1UL << i)))
                continue;
            int child_virq = domain->revmap[i];
            if (child_virq >= 0)
                IRQ_GenericHandleIrq(DDMBase, child_virq);
        }
        return;
    }

    /* No status register — dispatch all mapped child virqs. */
    uint32_t i;
    for (i = 0; i < domain->max_irq; i++)
    {
        int child_virq = domain->revmap[i];
        if (child_virq >= 0)
            IRQ_GenericHandleIrq(DDMBase, child_virq);
    }
}

int32_t GPIO_RegisterController(struct DDMBase *ddm __asm("a6"), struct gpio_controller *ctrl __asm("a0"))
{
    if (!ctrl || !ctrl->dev)
        return -1;

    DBG_GPIO("GPIO: register controller dev='%s' ngpio=%lu\n",
             ctrl->dev->node.ln_Name ? ctrl->dev->node.ln_Name : "(unnamed)", (unsigned long)ctrl->ngpio);
    AddTail(&ddm->controllers, &ctrl->node);

    /* Allocate the per-pin descriptor table. If ngpio is 0
     * (backward compat), allocate a single desc. */
    uint32_t num_descs = ctrl->ngpio;
    if (num_descs == 0)
        num_descs = 1;

    ctrl->descs = (struct gpio_desc *)AllocMem(num_descs * sizeof(struct gpio_desc), MEMF_ANY | MEMF_CLEAR);
    if (!ctrl->descs)
    {
        Remove(&ctrl->node);
        return -1;
    }

    /* Initialize each descriptor. */
    uint32_t i;
    for (i = 0; i < num_descs; i++)
    {
        ctrl->descs[i].controller = ctrl;
        ctrl->descs[i].pin = (uint16_t)i;
        ctrl->descs[i].flags = 0;
        ctrl->descs[i].label = NULL;

        /* Seed the IS_OUT flag. If the controller provides
         * get_direction, use it. Otherwise, if direction_input is
         * NULL but direction_output is non-NULL, assume output.
         * If both are NULL, assume input (safe default). */
        if (ctrl->get_direction)
        {
            if (ctrl->get_direction(ctrl, (uint16_t)i) == GPIO_LINE_DIRECTION_OUT)
                ctrl->descs[i].flags |= GPIOD_FLAG_IS_OUT;
        }
        else if (!ctrl->direction_input && ctrl->direction_output)
        {
            ctrl->descs[i].flags |= GPIOD_FLAG_IS_OUT;
        }
    }

    /* If this GPIO controller is also an interrupt controller,
     * automatically create a hierarchical irq_domain and set a
     * chained handler on the parent virq. This mirrors Linux's
     * gpiolib, where a gpio_chip that is also an interrupt-controller
     * gets an irq_domain auto-created at registration time. */
    ctrl->parent_virq = -1;
    if (ctrl->irq_chip && DDMBase)
    {
        struct device *dev = ctrl->dev;

        /* Resolve the parent interrupt (from interrupt-parent on
         * this device's DT node). DT_GetInterrupt returns a virq in
         * the parent domain. */
        int parent_virq = DT_GetInterrupt(DDMBase, dev, 0);
        if (parent_virq >= 0)
        {
            /* Find the parent domain via the device tree. */
            struct irq_domain *parent_domain = IRQ_FindDomain(DDMBase, dev->parent);
            if (parent_domain)
            {
                /* Create a hierarchical child domain. The number of
                 * child irqs equals the number of GPIO pins. */
                uint32_t max_irq = ctrl->ngpio;
                if (max_irq == 0)
                    max_irq = 1; /* backward compat: single irq */

                struct irq_domain *domain = IRQ_CreateHierarchy(DDMBase, parent_domain, dev, ctrl->irq_chip, max_irq);
                if (domain)
                {
                    /* Set the chained handler on the parent virq.
                     * Use the controller's custom handler if provided,
                     * otherwise the framework passthrough. */
                    irq_chained_handler_t handler = ctrl->chained_handler;
                    if (!handler)
                        handler = gpio_passthrough_chained_handler;

                    IRQ_SetChainedHandler(DDMBase, parent_virq, handler, ctrl);
                    ctrl->parent_virq = parent_virq;
                }
                else
                {
                    /* Domain creation failed; dispose the parent virq. */
                    DT_FreeInterrupt(DDMBase, parent_virq);
                }
            }
            else
            {
                /* No parent domain found; dispose the parent virq. */
                DT_FreeInterrupt(DDMBase, parent_virq);
            }
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* LVO -240: GPIO_UnregisterController                                */
/* ------------------------------------------------------------------ */

void GPIO_UnregisterController(struct DDMBase *ddm __asm("a6"), struct gpio_controller *ctrl __asm("a0"))
{
    (void)ddm;
    if (!ctrl)
        return;

    /* Free the per-pin descriptor table. */
    if (ctrl->descs)
    {
        uint32_t num_descs = ctrl->ngpio;
        if (num_descs == 0)
            num_descs = 1;
        FreeMem(ctrl->descs, num_descs * sizeof(struct gpio_desc));
        ctrl->descs = NULL;
    }

    /* If this controller had an auto-created irq_domain, tear it
     * down. IRQ_DestroyDomain disposes all child mappings and
     * removes handlers. We also clear the chained handler on the
     * parent virq and dispose the parent mapping. */
    if (ctrl->irq_chip && ctrl->dev && ctrl->dev->irq_domain && DDMBase)
    {
        if (ctrl->parent_virq >= 0)
        {
            IRQ_SetChainedHandler(DDMBase, ctrl->parent_virq, NULL, NULL);
            DT_FreeInterrupt(DDMBase, ctrl->parent_virq);
            ctrl->parent_virq = -1;
        }
        IRQ_DestroyDomain(DDMBase, ctrl->dev->irq_domain);
    }

    Remove(&ctrl->node);
}

/* ------------------------------------------------------------------ */
/* LVO -246: GPIO_FindController                                      */
/* ------------------------------------------------------------------ */

struct gpio_controller *GPIO_FindController(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"))
{
    if (!dev)
        return NULL;

    struct Node *node = ddm->controllers.lh_Head;
    while (node->ln_Succ)
    {
        struct gpio_controller *ctrl = (struct gpio_controller *)node;
        if (ctrl->dev == dev)
            return ctrl;
        node = node->ln_Succ;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* LVO -252: GPIO_Get                                                 */
/* ------------------------------------------------------------------ */

/* Build a property name from con_id + suffix. Returns the buffer
 * pointer, or NULL if the name would overflow. */
static const char *gpio_build_propname(const char *con_id, const char *suffix, char *buf, uint32_t bufsize)
{
    if (!con_id)
        return suffix; /* "gpios" / "gpio" without a prefix */

    uint32_t len = 0;
    const char *p = con_id;
    while (*p && len < bufsize)
    {
        buf[len++] = *p++;
    }
    p = suffix;
    while (*p && len < bufsize)
    {
        buf[len++] = *p++;
    }
    if (len >= bufsize)
        return NULL;
    buf[len] = '\0';
    return buf;
}

/* Resolve a named GPIO property on a device to a gpio_desc.
 * The con_id is suffixed with "-gpios" (then "-gpio") to form the
 * property name. The property value is <phandle specifier...> where
 * the phandle is one u32 cell and the specifier length is determined
 * by the GPIO controller's #gpio-cells property. The phandle resolves
 * to the GPIO controller device. */
struct gpio_desc *GPIO_Get(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"),
                           const char *con_id __asm("a1"), uint32_t gflags __asm("d0"))
{
    if (!dev || !con_id)
        return NULL;

    /* Try "<con_id>-gpios" then "<con_id>-gpio". If con_id is NULL,
     * try "gpios" then "gpio". */
    char propname[64];
    const struct dt_property *prop = NULL;

    const char *name = gpio_build_propname(con_id, "-gpios", propname, sizeof(propname));
    if (name)
        prop = DT_GetProperty(DDMBase, dev, name);

    if (!prop)
    {
        name = gpio_build_propname(con_id, "-gpio", propname, sizeof(propname));
        if (name)
            prop = DT_GetProperty(DDMBase, dev, name);
    }

    if (!prop || prop->length < 8)
        return NULL;

    /* Extract phandle (4-byte BE → device pointer) */
    uint32_t phandle = ((uint32_t)prop->value[0] << 24) | ((uint32_t)prop->value[1] << 16) |
                       ((uint32_t)prop->value[2] << 8) | ((uint32_t)prop->value[3]);
    if (phandle == 0)
        return NULL;

    struct device *ctrl_dev = (struct device *)phandle;

    /* Read #gpio-cells from the controller's device tree node.
     * Default to 2 if not specified (backward compatible). */
    uint32_t gpio_cells = 2;
    uint32_t cells_val;
    if (DT_GetPropertyU32(DDMBase, ctrl_dev, "#gpio-cells", &cells_val) == 0)
        gpio_cells = cells_val;

    /* Total spec size: 1 (phandle) + gpio_cells */
    uint32_t gpio_spec_size = (1 + gpio_cells) * 4;
    if (prop->length < gpio_spec_size)
        return NULL;

    /* Extract pin number from first specifier cell (4-byte BE) */
    uint16_t pin = (uint16_t)(((uint32_t)prop->value[4] << 24) | ((uint32_t)prop->value[5] << 16) |
                              ((uint32_t)prop->value[6] << 8) | ((uint32_t)prop->value[7]));

    DBG_GPIO("GPIO: get '%s' -> ctrl='%s' pin=%lu\n", con_id,
             ctrl_dev->node.ln_Name ? ctrl_dev->node.ln_Name : "(unnamed)", (unsigned long)pin);

    /* Extract DT flags from second specifier cell if present (4-byte BE) */
    uint32_t dt_flags = 0;
    if (gpio_cells >= 2)
    {
        dt_flags = ((uint32_t)prop->value[8] << 24) | ((uint32_t)prop->value[9] << 16) |
                   ((uint32_t)prop->value[10] << 8) | ((uint32_t)prop->value[11]);
    }

    /* Find the controller registered for this device */
    struct gpio_controller *ctrl = NULL;
    struct Node *node = ddm->controllers.lh_Head;
    while (node->ln_Succ)
    {
        struct gpio_controller *c = (struct gpio_controller *)node;
        if (c->dev == ctrl_dev)
        {
            ctrl = c;
            break;
        }
        node = node->ln_Succ;
    }

    if (!ctrl)
        return NULL;

    /* Validate pin number against the controller's declared range. */
    if (ctrl->ngpio > 0 && pin >= ctrl->ngpio)
        return NULL;

    struct gpio_desc *desc = &ctrl->descs[pin];

    /* Reject if already requested (Linux returns -EBUSY). */
    if (desc->flags & GPIOD_FLAG_REQUESTED)
        return NULL;

    /* Translate DT flags to runtime flags. */
    if (dt_flags & GPIO_ACTIVE_LOW)
        desc->flags |= GPIOD_FLAG_ACTIVE_LOW;
    if (dt_flags & GPIO_OPEN_DRAIN)
        desc->flags |= GPIOD_FLAG_OPEN_DRAIN;
    if (dt_flags & GPIO_OPEN_SOURCE)
        desc->flags |= GPIOD_FLAG_OPEN_SOURCE;
    if (dt_flags & GPIO_PULL_UP)
        desc->flags |= GPIOD_FLAG_PULL_UP;
    if (dt_flags & GPIO_PULL_DOWN)
        desc->flags |= GPIOD_FLAG_PULL_DOWN;

    /* Call the controller's request callback (if any). */
    if (ctrl->request)
    {
        if (ctrl->request(ctrl, pin) != 0)
        {
            /* Request rejected — clear the flags we just set. */
            desc->flags &= ~(GPIOD_FLAG_ACTIVE_LOW | GPIOD_FLAG_OPEN_DRAIN | GPIOD_FLAG_OPEN_SOURCE |
                             GPIOD_FLAG_PULL_UP | GPIOD_FLAG_PULL_DOWN);
            return NULL;
        }
    }

    desc->flags |= GPIOD_FLAG_REQUESTED;

    /* Apply the acquire flags to set direction. */
    switch (gflags)
    {
    case GPIOD_IN:
        GPIO_DirectionInput(DDMBase, desc);
        break;
    case GPIOD_OUT_LOW:
        GPIO_DirectionOutput(DDMBase, desc, 0);
        break;
    case GPIOD_OUT_HIGH:
        GPIO_DirectionOutput(DDMBase, desc, 1);
        break;
    case GPIOD_ASIS:
    default:
        break;
    }

    return desc;
}

/* ------------------------------------------------------------------ */
/* LVO -258: GPIO_Free                                                */
/* ------------------------------------------------------------------ */

void GPIO_Free(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"))
{
    (void)ddm;
    if (!desc || !desc->controller)
        return;

    if (!(desc->flags & GPIOD_FLAG_REQUESTED))
        return;

    if (desc->controller->free)
        desc->controller->free(desc->controller, desc->pin);

    desc->flags &= ~(GPIOD_FLAG_REQUESTED | GPIOD_FLAG_ACTIVE_LOW | GPIOD_FLAG_OPEN_DRAIN | GPIOD_FLAG_OPEN_SOURCE |
                     GPIOD_FLAG_PULL_UP | GPIOD_FLAG_PULL_DOWN | GPIOD_FLAG_USED_AS_IRQ);
    desc->label = NULL;
}

/* ------------------------------------------------------------------ */
/* LVO -264: GPIO_GetValue                                            */
/* ------------------------------------------------------------------ */

int GPIO_GetValue(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"))
{
    (void)ddm;
    if (!desc || !desc->controller)
        return -1;

    if (!desc->controller->get_value)
        return -1;

    int raw = desc->controller->get_value(desc->controller, desc->pin);
    if (raw < 0)
        return raw;

    /* Apply active-low inversion */
    if (desc->flags & GPIOD_FLAG_ACTIVE_LOW)
        return raw ? 0 : 1;
    return raw ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* LVO -270: GPIO_SetValue                                            */
/* ------------------------------------------------------------------ */

int GPIO_SetValue(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"), int value __asm("d0"))
{
    (void)ddm;
    if (!desc || !desc->controller)
        return -1;

    /* Must be configured as output. */
    if (!(desc->flags & GPIOD_FLAG_IS_OUT))
        return -1;

    struct gpio_controller *ctrl = desc->controller;

    /* Open-drain emulation: high = high-Z (direction_input),
     * low = drive 0 (direction_output(0)). */
    if (desc->flags & GPIOD_FLAG_OPEN_DRAIN)
    {
        if (value)
            return GPIO_DirectionInput(DDMBase, desc);
        else
            return GPIO_DirectionOutput(DDMBase, desc, 0);
    }

    /* Open-source emulation: high = drive 1 (direction_output(1)),
     * low = high-Z (direction_input). */
    if (desc->flags & GPIOD_FLAG_OPEN_SOURCE)
    {
        if (value)
            return GPIO_DirectionOutput(DDMBase, desc, 1);
        else
            return GPIO_DirectionInput(DDMBase, desc);
    }

    if (!ctrl->set_value)
        return -1;

    /* Apply active-low inversion */
    if (desc->flags & GPIOD_FLAG_ACTIVE_LOW)
        value = value ? 0 : 1;

    ctrl->set_value(ctrl, desc->pin, value);
    return 0;
}

/* ------------------------------------------------------------------ */
/* LVO -276: GPIO_DirectionInput                                      */
/* ------------------------------------------------------------------ */

int GPIO_DirectionInput(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"))
{
    (void)ddm;
    if (!desc || !desc->controller)
        return -1;

    struct gpio_controller *ctrl = desc->controller;

    /* If the controller provides get_direction and the pin is
     * already input, this is a no-op success. */
    if (ctrl->get_direction && ctrl->get_direction(ctrl, desc->pin) == GPIO_LINE_DIRECTION_IN)
    {
        desc->flags &= ~GPIOD_FLAG_IS_OUT;
        return 0;
    }

    /* If capabilities are declared, enforce them */
    if (ctrl->supported_modes != 0)
    {
        if (!(ctrl->supported_modes & GPIO_MODE_INPUT))
            return -1; /* Input not supported */

        /* Input is supported. If there's no callback, the pin is
         * permanently input — this is a no-op success. */
        if (!ctrl->direction_input)
        {
            desc->flags &= ~GPIOD_FLAG_IS_OUT;
            return 0;
        }
    }
    else
    {
        /* Linux model: no capabilities declared, use NULL check */
        if (!ctrl->direction_input)
            return -1;
    }

    int ret = ctrl->direction_input(ctrl, desc->pin);
    if (ret == 0)
        desc->flags &= ~GPIOD_FLAG_IS_OUT;
    return ret;
}

/* ------------------------------------------------------------------ */
/* LVO -282: GPIO_DirectionOutput                                     */
/* ------------------------------------------------------------------ */

int GPIO_DirectionOutput(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"), int value __asm("d0"))
{
    (void)ddm;
    if (!desc || !desc->controller)
        return -1;

    /* Reject if the line is locked as an IRQ. */
    if (desc->flags & GPIOD_FLAG_USED_AS_IRQ)
        return -1;

    struct gpio_controller *ctrl = desc->controller;

    /* If capabilities are declared, enforce them */
    if (ctrl->supported_modes != 0)
    {
        if (!(ctrl->supported_modes & GPIO_MODE_OUTPUT))
            return -1; /* Output not supported */

        /* Output is supported but no callback — defensive failure */
        if (!ctrl->direction_output)
            return -1;
    }
    else
    {
        /* Linux model: no capabilities declared, use NULL check */
        if (!ctrl->direction_output)
            return -1;
    }

    /* Apply active-low inversion to the initial value */
    if (desc->flags & GPIOD_FLAG_ACTIVE_LOW)
        value = value ? 0 : 1;

    int ret = ctrl->direction_output(ctrl, desc->pin, value);
    if (ret == 0)
        desc->flags |= GPIOD_FLAG_IS_OUT;
    return ret;
}

/* ------------------------------------------------------------------ */
/* LVO -288: GPIO_ToIrq                                              */
/* ------------------------------------------------------------------ */

/* Translate a GPIO pin to its interrupt (virq). Allocates the virq
 * mapping in the controller's irq_domain on first use (Linux
 * irq_create_mapping). Rejects output lines (unless open-drain) and
 * marks the line as used-as-IRQ. Returns the virq, or -1 if the
 * controller has no irq_chip or the mapping fails. */
int GPIO_ToIrq(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"))
{
    (void)ddm;
    if (!desc || !desc->controller)
        return -1;

    struct gpio_controller *ctrl = desc->controller;
    if (!ctrl->irq_chip || !ctrl->dev || !ctrl->dev->irq_domain)
        return -1;

    if (!DDMBase)
        return -1;

    /* Reject output lines unless open-drain (Linux lock-as-irq). */
    if ((desc->flags & GPIOD_FLAG_IS_OUT) && !(desc->flags & GPIOD_FLAG_OPEN_DRAIN))
        return -1;

    struct irq_domain *domain = ctrl->dev->irq_domain;
    if (desc->pin >= domain->max_irq)
        return -1;

    int virq = IRQ_AllocVirq(DDMBase, domain, (uint32_t)desc->pin);
    if (virq >= 0)
        desc->flags |= GPIOD_FLAG_USED_AS_IRQ;
    return virq;
}

/* ------------------------------------------------------------------ */
/* LVO -294: GPIO_GetDirection                                        */
/* ------------------------------------------------------------------ */

int GPIO_GetDirection(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"))
{
    (void)ddm;
    if (!desc || !desc->controller)
        return -1;

    if (desc->controller->get_direction)
        return desc->controller->get_direction(desc->controller, desc->pin);

    return (desc->flags & GPIOD_FLAG_IS_OUT) ? GPIO_LINE_DIRECTION_OUT : GPIO_LINE_DIRECTION_IN;
}

/* ------------------------------------------------------------------ */
/* LVO -300: GPIO_GetRawValue                                         */
/* ------------------------------------------------------------------ */

int GPIO_GetRawValue(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"))
{
    (void)ddm;
    if (!desc || !desc->controller)
        return -1;

    if (!desc->controller->get_value)
        return -1;

    return desc->controller->get_value(desc->controller, desc->pin);
}

/* ------------------------------------------------------------------ */
/* LVO -306: GPIO_SetRawValue                                         */
/* ------------------------------------------------------------------ */

int GPIO_SetRawValue(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"), int value __asm("d0"))
{
    (void)ddm;
    if (!desc || !desc->controller)
        return -1;

    if (!(desc->flags & GPIOD_FLAG_IS_OUT))
        return -1;

    if (!desc->controller->set_value)
        return -1;

    desc->controller->set_value(desc->controller, desc->pin, value);
    return 0;
}

/* ------------------------------------------------------------------ */
/* LVO -312: GPIO_SetConfig                                           */
/* ------------------------------------------------------------------ */

int GPIO_SetConfig(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"), uint32_t config __asm("d0"))
{
    (void)ddm;
    if (!desc || !desc->controller)
        return -1;

    if (!desc->controller->set_config)
        return -1;

    return desc->controller->set_config(desc->controller, desc->pin, config);
}
