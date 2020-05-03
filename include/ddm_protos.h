/*
 * LVO stubs for ddm.library
 */
#ifndef DDM_PROTOS_H_
#define DDM_PROTOS_H_

#include "ddm.h"
#include "ddm_gcc.h" /* LVO stub macros */
#include <exec/libraries.h>
#include <exec/types.h>

/* ------------------------------------------------------------------ */
/* Function prototypes                                                 */
/*                                                                    */
/* These declare the real LVO functions implemented in core source.    */
/* External callers use the macro stubs below; implementation files   */
/* #undef the macros for the functions they define.                   */
/* ------------------------------------------------------------------ */

/* Register a device in the tree. The device must have its name
 * (node.ln_Name) and parent set. If parent is NULL, the device
 * becomes the root. Returns the device pointer, or NULL on failure.
 */
struct device *DDM_RegisterDevice(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"));

/* Unregister a device from the tree and free it. Also removes all
 * children recursively. Does nothing if dev is NULL.
 */
void DDM_UnregisterDevice(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"));

/* Register a driver. The driver must have its name (node.ln_Name),
 * compatible array, lib_base, and bus_type set. Returns the driver
 * pointer, or NULL on failure.
 */
struct device_driver *DDM_RegisterDriver(struct DDMBase *ddm __asm("a6"), struct device_driver *drv __asm("a0"));

/* Unregister a driver. Unbinds from any matched devices. */
void DDM_UnregisterDriver(struct DDMBase *ddm __asm("a6"), struct device_driver *drv __asm("a0"));

/* Match all unmatched devices with registered drivers. Returns 0 on
 * success, or a negative error code on failure.
 */
int32_t DDM_MatchAll(struct DDMBase *ddm __asm("a6"));

/* Try to match a specific device with a registered driver. Returns 0
 * if a driver was bound, -1 if no match found.
 */
int32_t DDM_MatchDevice(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"));

/* Find a device by full path (e.g. "/cia/parallel-port/par-spi-adapter"). */
struct device *DDM_FindDevice(struct DDMBase *ddm __asm("a6"), const char *path __asm("a0"));

/* Find the first device whose compatible list contains the given string. */
struct device *DDM_FindByCompatible(struct DDMBase *ddm __asm("a6"), const char *compatible __asm("a0"));

/* Get the parent of a device. Returns NULL for the root. */
struct device *DDM_GetParent(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"));

/* Get the first child of a device. Returns NULL if no children. */
struct device *DDM_GetChild(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"));

/* Get the next sibling of a device. Returns NULL if last sibling. */
struct device *DDM_GetNextSibling(struct DDMBase *ddm __asm("a6"), struct device *dev __asm("a0"));

/* Enumerate children of a bus device. Calls the bus driver's
 * DDMDrv_Enumerate LVO. Returns 0 on success, negative on error.
 */
int32_t DDM_EnumerateChildren(struct DDMBase *ddm __asm("a6"), struct device *bus __asm("a0"));

/* Register a bus type. Returns the bus_type pointer, or NULL on failure. */
struct bus_type *DDM_RegisterBusType(struct DDMBase *ddm __asm("a6"), struct bus_type *bus __asm("a0"));

/* Get a bus type by bus_type_id. Returns NULL if not found. */
struct bus_type *DDM_GetBusType(struct DDMBase *ddm __asm("a6"), uint32_t bus_type_id __asm("d0"));

/* Load a config file, open referenced libraries/devices (which then
 * self-register via DDM_RegisterDriver), and call DDM_MatchAll. Returns
 * 0 on success, negative on error.
 */
int32_t DDM_LoadConfig(struct DDMBase *ddm __asm("a6"), const char *filename __asm("a0"));

/* ------------------------------------------------------------------ */
/* LVO stub macros                                                    */
/*                                                                    */
/* External callers (drivers, subsystems) use these macros to make    */
/* LVO calls through the ddm.library base in a6.                      */
/*                                                                    */
/* Implementation files that DEFINE an LVO function must NOT see      */
/* these macros. Define DDM_INTERNAL before including this header     */
/* to suppress the macro definitions.                                 */
/* ------------------------------------------------------------------ */

#ifndef DDM_INTERNAL

#define DDM_RegisterDevice(ddm, dev) __DDM_LVO_RET_1A0(struct device *, -30, (ddm), (dev))
#define DDM_UnregisterDevice(ddm, dev) __DDM_LVO_VOID_1A0(-36, (ddm), (dev))
#define DDM_RegisterDriver(ddm, drv) __DDM_LVO_RET_1A0(struct device_driver *, -42, (ddm), (drv))
#define DDM_UnregisterDriver(ddm, drv) __DDM_LVO_VOID_1A0(-48, (ddm), (drv))
#define DDM_MatchAll(ddm) __DDM_LVO_RET_1D0(int32_t, -54, (ddm), 0)
#define DDM_MatchDevice(ddm, dev) __DDM_LVO_RET_1A0(int32_t, -60, (ddm), (dev))
#define DDM_FindDevice(ddm, path) __DDM_LVO_RET_1A0(struct device *, -66, (ddm), (path))
#define DDM_FindByCompatible(ddm, compatible) __DDM_LVO_RET_1A0(struct device *, -72, (ddm), (compatible))
#define DDM_GetParent(ddm, dev) __DDM_LVO_RET_1A0(struct device *, -78, (ddm), (dev))
#define DDM_GetChild(ddm, dev) __DDM_LVO_RET_1A0(struct device *, -84, (ddm), (dev))
#define DDM_GetNextSibling(ddm, dev) __DDM_LVO_RET_1A0(struct device *, -90, (ddm), (dev))
#define DDM_EnumerateChildren(ddm, bus) __DDM_LVO_RET_1A0(int32_t, -96, (ddm), (bus))
#define DDM_RegisterBusType(ddm, bus) __DDM_LVO_RET_1A0(struct bus_type *, -102, (ddm), (bus))
#define DDM_GetBusType(ddm, bus_type_id) __DDM_LVO_RET_1D0(struct bus_type *, -108, (ddm), (bus_type_id))
#define DDM_LoadConfig(ddm, filename) __DDM_LVO_RET_1A0(int32_t, -114, (ddm), (filename))

#endif /* !DDM_INTERNAL */

#endif /* DDM_PROTOS_H_ */
