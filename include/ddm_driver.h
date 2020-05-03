/*
 * Driver interface LVO stubs for the DDM framework
 *
 * These LVOs are implemented by every driver library/device that
 * participates in the DDM framework. They start at LVO -42
 * to avoid collision with the standard device LVOs:
 *   -6  Open
 *   -12 Close
 *   -18 Expunge
 *   -24 Reserved (null)
 *   -30 BeginIO  (devices only)
 *   -36 AbortIO  (devices only)
 *   -42 DDMDrv_Probe      <-- DDM driver interface starts here
 *   -48 DDMDrv_Remove
 *   -54 DDMDrv_Init       (reserved — merged into Probe)
 *   -60 DDMDrv_Shutdown
 *   -66 DDMDrv_Enumerate
 *   -72 DDMDrv_GetCompatible
 */
#ifndef DDM_DRIVER_H_
#define DDM_DRIVER_H_

#include "ddm.h"
#include "ddm_gcc.h" /* LVO stub macros */
#include <exec/libraries.h>
#include <exec/types.h>

/* ------------------------------------------------------------------ */
/* Probe return codes                                                 */
/* ------------------------------------------------------------------ */

#define DDMDRV_PROBE_OK 0 /* Driver claims and initialized the device */
#define DDMDRV_PROBE_DEFER                                                                                             \
    (-2) /* Driver matches but a dependency is not                                                                     \
          * ready yet; retry in a later pass */

/* ------------------------------------------------------------------ */
/* LVO -42: Probe                                                     */
/* ------------------------------------------------------------------ */

/* Check if this driver can handle the given device AND initialize it.
 * Called by the matching engine after compatible strings (or the
 * bus_type match callback) match. The driver should set up hardware,
 * create tasks, register with subsystems, etc.
 *
 * Returns:
 *   DDMDRV_PROBE_OK (0)  — device claimed and initialized successfully.
 *   DDMDRV_PROBE_DEFER   — driver matches, but a dependency is not yet
 *                         ready; the matching engine will retry later.
 *   other non-zero      — driver does not handle this device; the
 *                         engine tries the next driver.
 *
 * LVO stub macro: DDMDrv_Probe(drv, dev)
 */

/* ------------------------------------------------------------------ */
/* LVO -48: Remove                                                     */
/* ------------------------------------------------------------------ */

/* Called when the driver is being unbound from the device. The driver
 * should release any resources allocated during probe/init.
 *
 * LVO stub macro: DDMDrv_Remove(drv, dev)
 */

/* ------------------------------------------------------------------ */
/* LVO -54: Init (reserved)                                           */
/* ------------------------------------------------------------------ */

/* Previously a separate initialization step called after a successful
 * probe. Now merged into DDMDrv_Probe — the probe callback does both
 * check and init. This LVO slot is kept for binary stability but is
 * no longer called by the matching engine.
 *
 * LVO stub macro: DDMDrv_Init(drv, dev)
 */

/* ------------------------------------------------------------------ */
/* LVO -60: Shutdown                                                   */
/* ------------------------------------------------------------------ */

/* Shut down the device. Called before Remove during unloading.
 *
 * LVO stub macro: DDMDrv_Shutdown(drv, dev)
 */

/* ------------------------------------------------------------------ */
/* LVO -66: Enumerate                                                  */
/* ------------------------------------------------------------------ */

/* For bus drivers: enumerate children of the bus device. The driver
 * should create child device structs for devices discovered on the bus,
 * or acknowledge children already described in the device tree. Returns
 * 0 on success, non-zero on failure.
 *
 * LVO stub macro: DDMDrv_Enumerate(drv, bus)
 */

/* ------------------------------------------------------------------ */
/* LVO -72: GetCompatible                                              */
/* ------------------------------------------------------------------ */

/* Return a pointer to a NULL-terminated array of compatible strings.
 * The matching engine uses these to match drivers to devices.
 *
 * LVO stub macro: DDMDrv_GetCompatible(drv)
 */

/* ------------------------------------------------------------------ */
/* LVO stub macros                                                    */
/*                                                                    */
/* External callers (the matching engine in core/driver.c) use these   */
/* to make LVO calls through the driver's library base in a6.         */
/*                                                                    */
/* Driver implementation files do NOT define functions named DDMDrv_* */
/* — they define their own probe/remove/shutdown functions and place  */
/* them in the library's LVO function table. Therefore these macros   */
/* are always active; no DDM_INTERNAL guard is needed.               */
/* ------------------------------------------------------------------ */

#define DDMDrv_Probe(drv, dev) __DDM_LVO_RET_1A0(int32_t, -42, (drv), (dev))
#define DDMDrv_Remove(drv, dev) __DDM_LVO_VOID_1A0(-48, (drv), (dev))
#define DDMDrv_Init(drv, dev) __DDM_LVO_RET_1A0(int32_t, -54, (drv), (dev))
#define DDMDrv_Shutdown(drv, dev) __DDM_LVO_VOID_1A0(-60, (drv), (dev))
#define DDMDrv_Enumerate(drv, bus) __DDM_LVO_RET_1A0(int32_t, -66, (drv), (bus))
#define DDMDrv_GetCompatible(drv) __DDM_LVO_RET_1D0(const char **, -72, (drv), 0)

#endif /* DDM_DRIVER_H_ */
