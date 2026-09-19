/*
 * Device Tree for AmigaOS
 *
 * The device tree functions parse a DTS text file into struct device
 * nodes (populating the DDM) and provide property access.
 * They are part of the DDM core (ddm.library).
 */
#ifndef DEVICETREE_H_
#define DEVICETREE_H_

#include "ddm.h"     /* struct device */
#include "ddm_gcc.h" /* LVO stub macros */
#include "irq.h"     /* IRQ_TYPE_* */
#include <dos/dos.h> /* BPTR */
#include <exec/libraries.h>
#include <exec/types.h>

/* ------------------------------------------------------------------ */
/* Device tree property                                                */
/* ------------------------------------------------------------------ */

/* A property has a name and a value. The value is a byte array of
 * length bytes. For string properties, the value is a NUL-terminated
 * string. For u32 properties, the value is 4 bytes in big-endian
 * (network byte order), matching the Flattened Device Tree convention.
 */
struct dt_property
{
    struct dt_property *next;
    char *name;
    uint32_t length;
    uint8_t *value;
};

/* ------------------------------------------------------------------ */
/* Device node - the device tree representation of a device           */
/* ------------------------------------------------------------------ */

/* A device node holds the device-tree-specific data for a struct
 * device: the node name, full path, and properties. It is attached
 * to a struct device via the of_node pointer. Devices not described
 * by a device tree have of_node == NULL.
 */
struct device_node
{
    char *name;                     /* Node name (e.g. "mmc-spi@0") */
    char *path;                     /* Full path (e.g. "/cia/parallel-port/.../mmc-spi@0") */
    struct dt_property *properties; /* Linked list of properties */
};

/* ------------------------------------------------------------------ */
/* LVO prototypes                                                      */
/*                                                                    */
/* These declare the real LVO functions implemented in core/devicetree.c */
/* and core/parser.c. External callers use the macro stubs below;     */
/* implementation files define DDM_INTERNAL to suppress the macros.   */
/* ------------------------------------------------------------------ */
/* The DT_* functions are part of ddm.library. Callers pass their      */
/* DDMBase * (obtained via OpenLibrary("ddm.library")) in a6.           */

/* LVO -198: Parse a device tree text file and build the device tree.
 * Creates struct device + struct device_node entries and registers
 * them with ddm.library. Returns 0 on success, negative on
 * error. */
int32_t DT_ParseTree(struct DDMBase *ddm __asm("a6"), const char *filename __asm("a0"));

/* LVO -204: Get a property by name from a device's device node.
 * Returns NULL if not found or the device has no device node. */
const struct dt_property *DT_GetProperty(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"),
                                         const char *name __asm("a1"));

/* LVO -210: Get a string property by name. Returns NULL if not found
 * or not a string. */
const char *DT_GetPropertyString(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"),
                                 const char *name __asm("a1"));

/* LVO -216: Get a U32 property by name. Returns 0 on success (value
 * written to *out), or -1 if the property was not found. The value
 * is converted from big-endian (FDT convention) to native byte order. */
int32_t DT_GetPropertyU32(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"), const char *name __asm("a1"),
                          uint32_t *out __asm("d0"));

/* LVO -222: Resolve interrupt #index from dev's DTS properties
 * (interrupts and interrupt-parent). Finds the interrupt-parent's
 * irq_domain, translates the specifier, allocates a virq via
 * IRQ_AllocVirq, and applies the trigger type via IRQ_SetType.
 * Returns the virq (>= 0), or -1 on failure. The caller must dispose
 * the mapping with DT_FreeInterrupt when done. */
int DT_GetInterrupt(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"), uint32_t index __asm("d0"));

/* LVO -228: Dispose of a virq mapping previously allocated by
 * DT_GetInterrupt. A thin wrapper over IRQ_DisposeMapping. */
void DT_FreeInterrupt(struct DDMBase *ddm __asm("a6"), int virq __asm("d0"));

/* LVO -318: Parse a device tree overlay file and graft child nodes
 * onto matching Zorro boards by manufacturer/product ID. The overlay
 * file contains fragment@N nodes with a zorro-match property (two u32s:
 * manufacturer, product) and child nodes to graft onto matching
 * boards. reg offsets are converted to absolute addresses by adding
 * the board's base address. Returns 0 on success, -1 on failure
 * (including file-not-found, which is non-fatal). */
int32_t DT_ApplyOverlay(struct DDMBase *ddm __asm("a6"), const char *filename __asm("a0"));

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

#define DT_ParseTree(ddm, filename) __DDM_LVO_RET_1A0(int32_t, -198, (ddm), (filename))
#define DT_GetProperty(ddm, dev, name) __DDM_LVO_RET_2A0A1(const struct dt_property *, -204, (ddm), (dev), (name))
#define DT_GetPropertyString(ddm, dev, name) __DDM_LVO_RET_2A0A1(const char *, -210, (ddm), (dev), (name))
#define DT_GetPropertyU32(ddm, dev, name, out) __DDM_LVO_RET_3A0A1D0(int32_t, -216, (ddm), (dev), (name), (out))
#define DT_GetInterrupt(ddm, dev, index) __DDM_LVO_RET_2A0D0(int, -222, (ddm), (dev), (index))
#define DT_FreeInterrupt(ddm, virq) __DDM_LVO_VOID_1D0(-228, (ddm), (virq))
#define DT_ApplyOverlay(ddm, filename) __DDM_LVO_RET_1A0(int32_t, -318, (ddm), (filename))

#endif /* !DDM_INTERNAL */

#endif /* DEVICETREE_H_ */
