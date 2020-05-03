/*
 * Clockport subsystem - clockport.library
 *
 * Provides a bus API for devices on the Amiga clockport: controller
 * registration/lookup. Hardware resource allocation (the clockport
 * memory region) is the responsibility of the specific controller
 * driver, not this generic subsystem.
 */
#ifndef CLOCKPORT_H_
#define CLOCKPORT_H_

#include "ddm.h"     /* struct device, BUS_TYPE_* */
#include "ddm_gcc.h" /* LVO stub macros */
#include <dos/dos.h> /* BPTR */
#include <exec/libraries.h>
#include <exec/types.h>

/* Bus type for clockport devices */
/* #define BUS_TYPE_CLOCKPORT  6  -- defined in ddm.h */

/* ------------------------------------------------------------------ */
/* Clockport operations interface                                      */
/* ------------------------------------------------------------------ */

/* A clockport controller provides this operations table. Child
 * devices (e.g. spider) call through these function pointers to
 * access the clockport registers, never touching the memory-mapped
 * base address directly. This mirrors Linux's bus operations. */
struct clockport_controller; /* forward declaration */

struct clockport_ops
{
    /* Read a register (4-bit address, 0-15). */
    uint8_t (*read_reg)(struct clockport_controller *ctrl __asm("a0"), uint16_t reg __asm("d0"));

    /* Write a register (4-bit address, 0-15). */
    void (*write_reg)(struct clockport_controller *ctrl __asm("a0"), uint16_t reg __asm("d0"), uint8_t val __asm("d1"));

    /* Fast-path batch register transfers.
     * read_reg_bytes reads count bytes from the given register into buf.
     * write_reg_bytes writes count bytes from buf to the given register. */
    void (*read_reg_bytes)(struct clockport_controller *ctrl __asm("a0"), uint16_t reg __asm("d0"),
                           uint8_t *buf __asm("a1"), int16_t count __asm("d1"));
    void (*write_reg_bytes)(struct clockport_controller *ctrl __asm("a0"), uint16_t reg __asm("d0"),
                            const uint8_t *buf __asm("a1"), int16_t count __asm("d1"));
};

/* ------------------------------------------------------------------ */
/* Clockport controller                                                */
/* ------------------------------------------------------------------ */

/* A clockport controller is a device that physically provides a
 * clockport. The controller driver fills in the ops table and
 * registers with clockport.library. Child devices find their
 * parent's controller via CP_FindController and call through its ops. */
struct clockport_controller
{
    struct Node node;                /* For the controllers list */
    struct device *dev;              /* Device tree device for this controller */
    const struct clockport_ops *ops; /* Operations table */
    void *private;                   /* Controller-specific private data */
};

/* ------------------------------------------------------------------ */
/* Clockport library base                                              */
/* ------------------------------------------------------------------ */

struct ClockportBase
{
    struct Library lib;
    BPTR seg_list;           /* Segment list (saved at init) */
    struct DDMBase *ddmbase; /* ddm.library (DDM_GetChild, DDM_MatchDevice) */
    struct List controllers; /* Registered clockport controllers */
};

/* ------------------------------------------------------------------ */
/* LVO prototypes                                                      */
/*                                                                    */
/* These declare the real LVO functions implemented in clockport.c.   */
/* External callers use the macro stubs below; the implementation     */
/* file defines CP_INTERNAL to suppress the macros.                  */
/* ------------------------------------------------------------------ */

/* LVO -30: Register a clockport controller. The controller's dev
 * must be set and ops must be filled in. Returns 0 on success,
 * negative on failure. */
int32_t CP_RegisterController(struct ClockportBase *cp __asm("a6"), struct clockport_controller *ctrl __asm("a0"));

/* LVO -36: Unregister a clockport controller. */
void CP_UnregisterController(struct ClockportBase *cp __asm("a6"), struct clockport_controller *ctrl __asm("a0"));

/* LVO -42: Find the clockport controller for a given device tree
 * device. Walks the registered controllers and returns the one whose
 * dev matches, or NULL if not found. */
struct clockport_controller *CP_FindController(struct ClockportBase *cp __asm("a6"), struct device *dev __asm("a0"));

/* ------------------------------------------------------------------ */
/* LVO stub macros                                                    */
/*                                                                    */
/* External callers use these macros to make LVO calls through the     */
/* clockport.library base in a6.                                      */
/*                                                                    */
/* The implementation file must define CP_INTERNAL before including   */
/* this header to suppress the macro definitions.                     */
/* ------------------------------------------------------------------ */

#ifndef CP_INTERNAL

#define CP_RegisterController(cp, ctrl) __DDM_LVO_RET_1A0(int32_t, -30, (cp), (ctrl))
#define CP_UnregisterController(cp, ctrl) __DDM_LVO_VOID_1A0(-36, (cp), (ctrl))
#define CP_FindController(cp, dev) __DDM_LVO_RET_1A0(struct clockport_controller *, -42, (cp), (dev))

#endif /* !CP_INTERNAL */

#endif /* CLOCKPORT_H_ */
