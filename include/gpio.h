/*
 * GPIO framework - part of ddm.library
 *
 * Provides a GPIO pin framework. Controller drivers register with
 * the core, and device drivers use the API to read/write GPIO
 * pins resolved from device tree properties (e.g. cd-gpios).
 *
 * This mirrors Linux's gpiod API: a named property is resolved to a
 * gpio_desc, which is then used to get/set the pin value.
 */
#ifndef GPIO_H_
#define GPIO_H_

#include "ddm.h"     /* struct DDMBase, struct device */
#include "ddm_gcc.h" /* LVO stub macros */
#include "irq.h"     /* struct irq_chip, irq_chained_handler_t */
#include <dos/dos.h> /* BPTR */
#include <exec/libraries.h>
#include <exec/nodes.h>
#include <exec/types.h>

/* Bus type for GPIO controllers (BUS_TYPE_GPIO is defined in ddm.h) */

/* ------------------------------------------------------------------ */
/* Direction constants (mirrors Linux GPIO_LINE_DIRECTION_*)          */
/* ------------------------------------------------------------------ */

#define GPIO_LINE_DIRECTION_OUT 0
#define GPIO_LINE_DIRECTION_IN 1

/* ------------------------------------------------------------------ */
/* Device-tree flag bits (cell 2 of the GPIO specifier)               */
/* ------------------------------------------------------------------ */

/* These mirror the Linux #define GPIO_* flags found in
 * include/dt-bindings/gpio/gpio.h. They are stored in the
 * device tree and translated into per-line runtime flags by
 * GPIO_Get. */
#define GPIO_ACTIVE_LOW 0x1
#define GPIO_OPEN_DRAIN 0x2
#define GPIO_OPEN_SOURCE 0x4
#define GPIO_PULL_UP 0x8
#define GPIO_PULL_DOWN 0x10
#define GPIO_PULL_DISABLE 0x20

/* ------------------------------------------------------------------ */
/* Per-line runtime flags (struct gpio_desc.flags)                    */
/* ------------------------------------------------------------------ */

#define GPIOD_FLAG_REQUESTED (1 << 0)
#define GPIOD_FLAG_IS_OUT (1 << 1)
#define GPIOD_FLAG_ACTIVE_LOW (1 << 2)
#define GPIOD_FLAG_OPEN_DRAIN (1 << 3)
#define GPIOD_FLAG_OPEN_SOURCE (1 << 4)
#define GPIOD_FLAG_PULL_UP (1 << 5)
#define GPIOD_FLAG_PULL_DOWN (1 << 6)
#define GPIOD_FLAG_USED_AS_IRQ (1 << 7)

/* ------------------------------------------------------------------ */
/* Acquire flags (gflags argument to GPIO_Get)                         */
/* ------------------------------------------------------------------ */

/* Mirrors Linux's enum gpiod_flags. Determines the direction applied
 * to the line at acquire time. GPIOD_ASIS leaves the direction
 * unchanged. */
#define GPIOD_ASIS 0
#define GPIOD_IN 1
#define GPIOD_OUT_LOW 2
#define GPIOD_OUT_HIGH 3

/* ------------------------------------------------------------------ */
/* Direction capability flags for gpio_controller.supported_modes     */
/* ------------------------------------------------------------------ */

/* These describe what the controller hardware can do, not the DTS
 * flags (which carry polarity info like GPIO_ACTIVE_LOW). When
 * supported_modes is 0 the framework uses the Linux model: callback
 * presence + get_direction determine capabilities. */
#define GPIO_MODE_INPUT 0x01  /* Pin can be read / configured as input  */
#define GPIO_MODE_OUTPUT 0x02 /* Pin can be driven / configured as output */

/* ------------------------------------------------------------------ */
/* GPIO controller                                                     */
/* ------------------------------------------------------------------ */

/* A GPIO controller is a device that can read/write individual GPIO
 * pins. The controller driver fills in the function pointers and
 * declares its capabilities (pin count and supported directions).
 * struct gpio_controller is embedded in the controller driver's
 * private data structure, or can be allocated separately.
 *
 * The framework allocates a per-pin descriptor table (descs) of
 * ngpio entries at registration time. GPIO_Get returns a pointer
 * into this table (not a freshly allocated object), so per-pin
 * state (requested, direction, flags) persists across calls.
 *
 * In Linux this maps to struct gpio_chip.
 */
struct gpio_controller
{
    struct Node node; /* For the controllers list (in DDMBase) */

    /* The device tree device for this controller */
    struct device *dev;

    /* Optional: called when a line is requested (GPIO_Get). Return
     * 0 on success, non-zero to reject the request. May be NULL. */
    int (*request)(struct gpio_controller *ctrl __asm("a0"), uint16_t pin __asm("d0"));

    /* Optional: called when a line is released (GPIO_Free). May be NULL. */
    void (*free)(struct gpio_controller *ctrl __asm("a0"), uint16_t pin __asm("d0"));

    /* Read the current value of a pin. Returns 1 (high) or 0 (low),
     * or -1 if the pin is unsupported. */
    int (*get_value)(struct gpio_controller *ctrl __asm("a0"), uint16_t pin __asm("d0"));

    /* Set the output value of a pin (optional, NULL if read-only). */
    void (*set_value)(struct gpio_controller *ctrl __asm("a0"), uint16_t pin __asm("d0"), int value __asm("d1"));

    /* Optional: return the current direction of a pin
     * (GPIO_LINE_DIRECTION_OUT or GPIO_LINE_DIRECTION_IN). If NULL,
     * the framework tracks direction via the IS_OUT flag. */
    int (*get_direction)(struct gpio_controller *ctrl __asm("a0"), uint16_t pin __asm("d0"));

    /* Configure a pin as input (optional, NULL if not supported). */
    int (*direction_input)(struct gpio_controller *ctrl __asm("a0"), uint16_t pin __asm("d0"));

    /* Configure a pin as output with an initial value (optional). */
    int (*direction_output)(struct gpio_controller *ctrl __asm("a0"), uint16_t pin __asm("d0"), int value __asm("d1"));

    /* Optional: apply a configuration (e.g. debounce, pull). The
     * config argument is a packed uint32_t. May be NULL. */
    int (*set_config)(struct gpio_controller *ctrl __asm("a0"), uint16_t pin __asm("d0"), uint32_t config __asm("d1"));

    /* Number of GPIO pins this controller exposes. 0 means "not
     * declared" — the framework skips pin-range validation
     * (backward compatible) and allocates a single desc. */
    uint16_t ngpio;

    /* Bitmask of supported pin directions (GPIO_MODE_INPUT and/or
     * GPIO_MODE_OUTPUT). 0 means "not declared" — the framework
     * uses the Linux model (callback presence + get_direction). */
    uint32_t supported_modes;

    /* Per-pin descriptor table, allocated by GPIO_RegisterController.
     * ngpio entries (or 1 if ngpio == 0). */
    struct gpio_desc *descs;

    /* Optional IRQ chip. If non-NULL, GPIO_RegisterController
     * automatically creates a hierarchical irq_domain for this
     * controller (parent = the device's interrupt-parent domain)
     * and sets a chained handler on the parent virq. This mirrors
     * Linux's gpiolib irq_domain auto-creation for gpio_chip that
     * are also interrupt-controller. NULL if this GPIO controller
     * is not an interrupt controller. */
    struct irq_chip *irq_chip;

    /* Optional chained handler for the parent virq. Called when the
     * parent interrupt fires, before dispatching to child virqs.
     * Typically acks a controller status register, then the
     * framework dispatches the child virq. If NULL, the framework
     * uses a passthrough handler that just calls
     * IRQ_GenericHandleIrq on the child virq (for controllers with
     * no status register, e.g. par-spi). */
    irq_chained_handler_t chained_handler;

    /* Optional: return a bitmask of pins with pending interrupts.
     * Bit i corresponds to pin i. If non-NULL, the passthrough
     * handler only dispatches child virqs for pins whose bit is
     * set, avoiding spurious dispatch to all mapped virqs. If NULL,
     * the passthrough handler dispatches all mapped child virqs
     * (the original behavior). */
    uint32_t (*get_irq_status)(struct gpio_controller *ctrl __asm("a0"));

    /* Parent virq for the auto-created domain. Set by
     * GPIO_RegisterController when it resolves the parent interrupt
     * via DT_GetInterrupt. Used by GPIO_UnregisterController to
     * clear the chained handler and dispose the mapping. -1 if no
     * domain was created. */
    int parent_virq;

    /* Private data for the controller driver */
    void *private;
};

/* ------------------------------------------------------------------ */
/* GPIO descriptor                                                    */
/* ------------------------------------------------------------------ */

/* A resolved GPIO pin, the result of GPIO_Get. Drivers pass this to
 * GPIO_GetValue / GPIO_SetValue. The descriptor is owned by the
 * controller's descs[] table (not allocated per call), so the pointer
 * remains valid until the controller is unregistered. Consumers
 * should treat this struct as opaque.
 *
 * In Linux this maps to struct gpio_desc.
 */
struct gpio_desc
{
    struct gpio_controller *controller;
    uint16_t pin;      /* Pin number on the controller */
    uint32_t flags;    /* Runtime flags (GPIOD_FLAG_*) */
    const char *label; /* Consumer name (optional) */
};

/* ------------------------------------------------------------------ */
/* LVO prototypes                                                      */
/*                                                                    */
/* These declare the real LVO functions implemented in core/gpio.c.   */
/* External callers use the macro stubs below; implementation files   */
/* define DDM_INTERNAL to suppress the macros.                        */
/* ------------------------------------------------------------------ */

/* LVO -234: Register a GPIO controller. The controller's dev must be
 * set. Returns 0 on success. */
int32_t GPIO_RegisterController(struct DDMBase *ddm __asm("a6"), struct gpio_controller *ctrl __asm("a0"));

/* LVO -240: Unregister a GPIO controller. */
void GPIO_UnregisterController(struct DDMBase *ddm __asm("a6"), struct gpio_controller *ctrl __asm("a0"));

/* LVO -246: Find the GPIO controller for a given device tree device.
 * Returns the controller, or NULL if not found. */
struct gpio_controller *GPIO_FindController(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"));

/* LVO -252: Resolve a named GPIO property on a device to a gpio_desc.
 * The con_id is suffixed with "-gpios" (then "-gpio") to form the
 * property name looked up in the device tree. The property value is a
 * <phandle pin flags> triple. gflags (GPIOD_ASIS/IN/OUT_LOW/OUT_HIGH)
 * sets the direction at acquire time. Returns the gpio_desc (a pointer
 * into the controller's table), or NULL on failure. The caller must
 * release it with GPIO_Free. */
struct gpio_desc *GPIO_Get(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"),
                           const char *con_id __asm("a1"), uint32_t gflags __asm("d0"));

/* LVO -258: Release a gpio_desc previously acquired by GPIO_Get.
 * Calls the controller's free callback (if any) and clears the
 * requested flag. Does not free memory (the desc is owned by the
 * controller's table). */
void GPIO_Free(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"));

/* LVO -264: Read the logical value of a GPIO pin. Returns 1 (active)
 * or 0 (inactive), or -1 on error. Applies active-low inversion. */
int GPIO_GetValue(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"));

/* LVO -270: Set the output value of a GPIO pin. Applies active-low
 * inversion and open-drain/open-source emulation. Returns 0 on
 * success, -1 if the pin is not configured as output. */
int GPIO_SetValue(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"), int value __asm("d0"));

/* LVO -276: Configure a GPIO pin as input. Returns 0 on success, -1
 * if unsupported. */
int GPIO_DirectionInput(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"));

/* LVO -282: Configure a GPIO pin as output with an initial value.
 * Returns 0 on success, -1 if unsupported or if the line is locked
 * as an IRQ. */
int GPIO_DirectionOutput(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"), int value __asm("d0"));

/* LVO -288: Translate a GPIO pin to its interrupt (virq). Allocates
 * the virq mapping in the controller's irq_domain. Rejects output
 * lines (unless open-drain) and marks the line as used-as-IRQ.
 * Returns the virq, or -1 if the controller has no irq_chip or the
 * mapping fails. */
int GPIO_ToIrq(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"));

/* LVO -294: Return the current direction of a GPIO pin
 * (GPIO_LINE_DIRECTION_OUT or GPIO_LINE_DIRECTION_IN). Returns -1
 * on error. */
int GPIO_GetDirection(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"));

/* LVO -300: Read the raw physical value of a GPIO pin (no active-low
 * inversion). Returns 1 (high) or 0 (low), or -1 on error. */
int GPIO_GetRawValue(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"));

/* LVO -306: Set the raw physical value of a GPIO pin (no active-low
 * inversion, no open-drain emulation). Returns 0 on success, -1 if
 * the pin is not configured as output. */
int GPIO_SetRawValue(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"), int value __asm("d0"));

/* LVO -312: Apply a configuration to a GPIO pin (e.g. debounce, pull).
 * Returns 0 on success, -1 if unsupported. */
int GPIO_SetConfig(struct DDMBase *ddm __asm("a6"), struct gpio_desc *desc __asm("a0"), uint32_t config __asm("d0"));

/* ------------------------------------------------------------------ */
/* LVO stub macros                                                    */
/*                                                                    */
/* External callers use these macros to make LVO calls through the     */
/* ddm.library base in a6.                                            */
/*                                                                    */
/* Implementation files that DEFINE an LVO function must NOT see      */
/* these macros. Define DDM_INTERNAL before including this header     */
/* to suppress the macro definitions.                                 */
/* ------------------------------------------------------------------ */

#ifndef DDM_INTERNAL

#define GPIO_RegisterController(ddm, ctrl) __DDM_LVO_RET_1A0(int32_t, -234, (ddm), (ctrl))
#define GPIO_UnregisterController(ddm, ctrl) __DDM_LVO_VOID_1A0(-240, (ddm), (ctrl))
#define GPIO_FindController(ddm, dev) __DDM_LVO_RET_1A0(struct gpio_controller *, -246, (ddm), (dev))
#define GPIO_Get(ddm, dev, con_id, gflags)                                                                             \
    __DDM_LVO_RET_3A0A1D0(struct gpio_desc *, -252, (ddm), (dev), (con_id), (gflags))
#define GPIO_Free(ddm, desc) __DDM_LVO_VOID_1A0(-258, (ddm), (desc))
#define GPIO_GetValue(ddm, desc) __DDM_LVO_RET_1A0(int, -264, (ddm), (desc))
#define GPIO_SetValue(ddm, desc, value) __DDM_LVO_RET_2A0D0(int, -270, (ddm), (desc), (value))
#define GPIO_DirectionInput(ddm, desc) __DDM_LVO_RET_1A0(int, -276, (ddm), (desc))
#define GPIO_DirectionOutput(ddm, desc, value) __DDM_LVO_RET_2A0D0(int, -282, (ddm), (desc), (value))
#define GPIO_ToIrq(ddm, desc) __DDM_LVO_RET_1A0(int, -288, (ddm), (desc))
#define GPIO_GetDirection(ddm, desc) __DDM_LVO_RET_1A0(int, -294, (ddm), (desc))
#define GPIO_GetRawValue(ddm, desc) __DDM_LVO_RET_1A0(int, -300, (ddm), (desc))
#define GPIO_SetRawValue(ddm, desc, value) __DDM_LVO_RET_2A0D0(int, -306, (ddm), (desc), (value))
#define GPIO_SetConfig(ddm, desc, config) __DDM_LVO_RET_2A0D0(int, -312, (ddm), (desc), (config))

#endif /* !DDM_INTERNAL */

#endif /* GPIO_H_ */
