# ddm.library

The core library of the DDM framework, inspired by
Linux's `drivers/base`. Provides `struct device` and `struct device_driver`
types, device registration, tree management, driver registration, and the
matching engine that binds drivers to devices by compatible strings. It also
provides the interrupt framework (`IRQ_*` functions, inspired by Linux's
`irq_chip` / `irq_domain`) — see `irq.c` and `include/irq.h`.

The device tree parser (`devicetree.c`, `parser.c`) and GPIO framework
(`gpio.c`) are part of the core; the parser populates the DDM from a DTS
file and provides property access (`DT_*` functions), while the GPIO
framework provides `GPIO_*` functions for resolving and driving pins from
device tree properties. The core also provides shared string helpers
(`ddm_strlen`/`ddm_strcmp`/`ddm_strdup` in `ddm_util.c`, declared in
`include/ddm_util.h`) used across the core and subsystems. Other
subsystems (spi, mmc, parallelport, clockport) build on top of the core.

## Building

```bash
./build.sh
```

Produces `ddm.library`, which should be placed in `LIBS:`.

## LVO Summary

| LVO   | Function              | Description                      |
|-------|-----------------------|----------------------------------|
| -6    | Open                  | Standard library open            |
| -12   | Close                 | Standard library close           |
| -18   | Expunge               | Standard library expunge         |
| -24   | (reserved)            |                                  |
| -30   | DDM_RegisterDevice    | Register a device in the tree    |
| -36   | DDM_UnregisterDevice  | Unregister and free a device     |
| -42   | DDM_RegisterDriver    | Register a driver                |
| -48   | DDM_UnregisterDriver  | Unregister a driver              |
| -54   | DDM_MatchAll          | Match all devices with drivers   |
| -60   | DDM_MatchDevice       | Match a single device            |
| -66   | DDM_FindDevice        | Find device by path              |
| -72   | DDM_FindByCompatible  | Find device by compatible string |
| -78   | DDM_GetParent         | Get parent device                |
| -84   | DDM_GetChild          | Get first child device           |
| -90   | DDM_GetNextSibling    | Get next sibling device          |
| -96   | DDM_EnumerateChildren | Enumerate bus children           |
| -102  | DDM_RegisterBusType   | Register a bus type              |
| -108  | DDM_GetBusType        | Get bus type by id               |
| -114  | DDM_LoadConfig        | Load config and match drivers    |

## Interrupt Framework LVOs

The interrupt framework is part of the core (implemented in `irq.c`).
Modelled on Linux's `irq_chip` / `irq_domain` / `irq_desc` trinity, it
provides virtual IRQ (virq) allocation, handler management, and cascaded
domain support. See `include/irq.h` for the full type definitions.

| LVO   | Function              | Description                              |
|-------|-----------------------|------------------------------------------|
| -120  | IRQ_CreateDomain      | Create irq_domain for chip on dev        |
| -126  | IRQ_FindDomain        | Find nearest irq_domain up the tree      |
| -132  | IRQ_AddHandler        | Add handler to a virq                    |
| -138  | IRQ_RemoveHandler     | Remove handler from a virq               |
| -144  | IRQ_Enable            | Enable a virq (dec disable_depth)        |
| -150  | IRQ_Disable           | Disable a virq (inc disable_depth)       |
| -156  | IRQ_SetType           | Configure trigger type for a virq        |
| -162  | IRQ_DestroyDomain     | Destroy irq_domain + dispose mappings    |
| -168  | IRQ_AllocVirq         | Allocate/look up virq for hwirq          |
| -174  | IRQ_DisposeMapping    | Dispose a virq mapping                   |
| -180  | IRQ_CreateHierarchy   | Create child irq_domain under parent     |
| -186  | IRQ_SetChainedHandler | Set chained handler for cascaded virq    |
| -192  | IRQ_GenericHandleIrq | Dispatch entry for a virq                 |

## Device Tree LVOs

The device tree parser is part of the core (implemented in `devicetree.c`
and `parser.c`). These LVOs provide DTS parsing and property access.

| LVO   | Function           | Description                       |
|-------|--------------------|-----------------------------------|
| -198  | DT_ParseTree       | Parse a DTS file into device tree |
| -204  | DT_GetProperty     | Get a raw property blob           |
| -210  | DT_GetPropertyString | Get a string property           |
| -216  | DT_GetPropertyU32  | Get a 32-bit property             |
| -222  | DT_GetInterrupt    | Resolve an interrupt specifier    |
| -228  | DT_FreeInterrupt   | Free an interrupt mapping         |

## GPIO Framework LVOs

The GPIO framework is part of the core (implemented in `gpio.c`). It
mirrors Linux's gpiod API: a named device-tree property is resolved to
a `gpio_desc`, which is then used to get/set the pin value. Controllers
that also act as interrupt controllers get an auto-created hierarchical
irq_domain. See `include/gpio.h` for the full type definitions.

| LVO   | Function                  | Description                                |
|-------|---------------------------|--------------------------------------------|
| -234  | GPIO_RegisterController   | Register a GPIO controller                 |
| -240  | GPIO_UnregisterController | Unregister a GPIO controller               |
| -246  | GPIO_FindController       | Find controller for a device               |
| -252  | GPIO_Get                  | Resolve named GPIO property to gpio_desc   |
| -258  | GPIO_Free                 | Release a gpio_desc                        |
| -264  | GPIO_GetValue             | Read logical value (active-low aware)      |
| -270  | GPIO_SetValue             | Set output value (active-low aware)        |
| -276  | GPIO_DirectionInput       | Configure pin as input                     |
| -282  | GPIO_DirectionOutput      | Configure pin as output with initial value |
| -288  | GPIO_ToIrq                | Translate pin to interrupt (virq)          |
| -294  | GPIO_GetDirection         | Get current pin direction                  |
| -300  | GPIO_GetRawValue          | Read raw physical value                    |
| -306  | GPIO_SetRawValue          | Set raw physical value                     |
| -312  | GPIO_SetConfig            | Apply config (debounce, pull, ...)         |

## Driver Interface LVOs

Implemented by every driver library/device at LVO -42 onwards (after the standard device LVOs at -30/-36):

| LVO   | Function             | Description                     |
|-------|----------------------|---------------------------------|
| -42   | DDMDrv_Probe         | Check if driver handles device  |
| -48   | DDMDrv_Remove        | Unbind from device              |
| -54   | DDMDrv_Init          | Reserved (merged into Probe)    |
| -60   | DDMDrv_Shutdown      | Shut down the device            |
| -66   | DDMDrv_Enumerate     | Enumerate bus children          |
| -72   | DDMDrv_GetCompatible | Return compatible string array  |
