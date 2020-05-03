/*
 * Parallel port subsystem - parallelport.library
 *
 * Provides a bus API for devices on the parallel port: controller
 * registration/lookup and interrupt handling via the device tree IRQ
 * framework. Hardware resource allocation (e.g. misc.resource for the
 * CIA parallel port bits) is the responsibility of the specific
 * controller driver, not this generic subsystem.
 */
#ifndef PARALLELPORT_H_
#define PARALLELPORT_H_

#include "ddm_gcc.h"    /* LVO stub macros */
#include "devicetree.h" /* struct device, DT_GetInterrupt */
#include "irq.h"        /* IRQ_AddHandler, etc. (part of ddm.library) */
#include <dos/dos.h>    /* BPTR */
#include <exec/interrupts.h>
#include <exec/libraries.h>
#include <exec/types.h>
#include <stdint.h> /* uint8_t, int16_t for batch ops */

/* Bus type for parallel port devices */
/* #define BUS_TYPE_PARALLEL_PORT  2  -- defined in devicetree.h */

/* ------------------------------------------------------------------ */
/* Parallel port operations interface                                  */
/* ------------------------------------------------------------------ */

/* A parallel port controller provides this operations table. Child
 * devices (e.g. par-spi-adapter) call through these function pointers
 * to access the hardware, never touching registers directly. This
 * mirrors Linux's struct parport_operations. */
struct parallel_port_controller; /* forward declaration */

struct parallel_port_ops
{
    /* Data register */
    void (*write_data)(struct parallel_port_controller *ctrl __asm("a0"), uint8_t value __asm("d0"));
    uint8_t (*read_data)(struct parallel_port_controller *ctrl __asm("a0"));

    /* Data direction register (0 = input, 0xff = output) */
    void (*write_data_dir)(struct parallel_port_controller *ctrl __asm("a0"), uint8_t dir __asm("d0"));
    uint8_t (*read_data_dir)(struct parallel_port_controller *ctrl __asm("a0"));

    /* Control/handshake register */
    void (*write_ctrl)(struct parallel_port_controller *ctrl __asm("a0"), uint8_t value __asm("d0"));
    uint8_t (*read_ctrl)(struct parallel_port_controller *ctrl __asm("a0"));

    /* Control direction register */
    void (*write_ctrl_dir)(struct parallel_port_controller *ctrl __asm("a0"), uint8_t dir __asm("d0"));
    uint8_t (*read_ctrl_dir)(struct parallel_port_controller *ctrl __asm("a0"));

    /* Fast-path batch transfers (the "2-E-cycle" loop).
     * The function reads the control register once, then loops count times,
     * toggling clk_mask on the control register each byte. The caller
     * re-reads the control register afterward to obtain the final
     * CLK state for REQ assertion. */
    void (*write_bytes_2e)(struct parallel_port_controller *ctrl __asm("a0"), uint8_t clk_mask __asm("d0"),
                           const uint8_t *buf __asm("a1"), int16_t count __asm("d1"));
    void (*read_bytes_2e)(struct parallel_port_controller *ctrl __asm("a0"), uint8_t clk_mask __asm("d0"),
                          uint8_t *buf __asm("a1"), int16_t count __asm("d1"));
};

/* ------------------------------------------------------------------ */
/* Control/handshake register bits                                     */
/* ------------------------------------------------------------------ */

/* On the Amiga the parallel port control signals live in CIA-B port A
 * (0xbfd000), shared with serial port control bits. Only the three
 * parallel-port-relevant bits are defined here. */
#define PP_CTRLB_BUSY 0     /* printer busy (input)  */
#define PP_CTRLB_PAPEROUT 1 /* printer paper out (input)  */
#define PP_CTRLB_SELECT 2   /* printer select (output) */

#define PP_CTRLF_BUSY (1 << PP_CTRLB_BUSY)
#define PP_CTRLF_PAPEROUT (1 << PP_CTRLB_PAPEROUT)
#define PP_CTRLF_SELECT (1 << PP_CTRLB_SELECT)

/* ------------------------------------------------------------------ */
/* Parallel port controller                                            */
/* ------------------------------------------------------------------ */

/* A parallel port controller is a device that physically provides a
 * parallel port. The controller driver fills in the ops table and
 * registers with parallelport.library. Child devices find their
 * parent's controller via PP_FindController and call through its ops. */
struct parallel_port_controller
{
    struct Node node;                    /* For the controllers list */
    struct device *dev;                  /* Device tree device for this controller */
    const struct parallel_port_ops *ops; /* Operations table */
    void *private;                       /* Controller-specific private data */
};

/* ------------------------------------------------------------------ */
/* Parallel port library base                                          */
/* ------------------------------------------------------------------ */

struct ParallelPortBase
{
    struct Library lib;
    BPTR seg_list;           /* Segment list (saved at init) */
    struct DDMBase *ddmbase; /* ddm.library (DDM_GetChild, IRQ_AddHandler, DT_*, etc.) */
    struct List controllers; /* Registered parallel port controllers */
};

/* ------------------------------------------------------------------ */
/* LVO prototypes                                                      */
/*                                                                    */
/* These declare the real LVO functions implemented in parallelport.c. */
/* External callers use the macro stubs below; the implementation     */
/* file defines PP_INTERNAL to suppress the macros.                  */
/* ------------------------------------------------------------------ */

/* LVO -30: Add an interrupt handler on the parallel port FLAG line.
 * Deprecated: drivers should use DT_GetInterrupt + IRQ_AddHandler (or
 * GPIO_ToIrq + IRQ_AddHandler) directly. Kept for backward compat.
 * Returns 0 on success, -1 if the interrupt can't be resolved. */
int32_t PP_AddInterrupt(struct ParallelPortBase *pp __asm("a6"), struct device *dev __asm("a0"),
                        struct Interrupt *isr __asm("a1"));

/* LVO -36: Remove an interrupt handler. Deprecated (see PP_AddInterrupt). */
void PP_RemoveInterrupt(struct ParallelPortBase *pp __asm("a6"), struct device *dev __asm("a0"),
                        struct Interrupt *isr __asm("a1"));

/* LVO -42: Enable the interrupt. Deprecated (see PP_AddInterrupt). */
void PP_EnableInterrupt(struct ParallelPortBase *pp __asm("a6"), struct device *dev __asm("a0"));

/* LVO -48: Disable the interrupt. Deprecated (see PP_AddInterrupt). */
void PP_DisableInterrupt(struct ParallelPortBase *pp __asm("a6"), struct device *dev __asm("a0"));

/* LVO -54: Register a parallel port controller. The controller's dev
 * must be set and ops must be filled in. Returns 0 on success,
 * negative on failure. */
int32_t PP_RegisterController(struct ParallelPortBase *pp __asm("a6"),
                              struct parallel_port_controller *ctrl __asm("a0"));

/* LVO -60: Unregister a parallel port controller. */
void PP_UnregisterController(struct ParallelPortBase *pp __asm("a6"),
                             struct parallel_port_controller *ctrl __asm("a0"));

/* LVO -66: Find the parallel port controller for a given device tree
 * device. Walks the registered controllers and returns the one whose
 * dev matches, or NULL if not found. */
struct parallel_port_controller *PP_FindController(struct ParallelPortBase *pp __asm("a6"),
                                                   struct device *dev __asm("a0"));

/* ------------------------------------------------------------------ */
/* LVO stub macros                                                    */
/*                                                                    */
/* External callers use these macros to make LVO calls through the     */
/* parallelport.library base in a6.                                   */
/*                                                                    */
/* The implementation file must define PP_INTERNAL before including   */
/* this header to suppress the macro definitions.                     */
/* ------------------------------------------------------------------ */

#ifndef PP_INTERNAL

#define PP_AddInterrupt(pp, dev, isr) __DDM_LVO_RET_2A0A1(int32_t, -30, (pp), (dev), (isr))
#define PP_RemoveInterrupt(pp, dev, isr) __DDM_LVO_VOID_2A0A1(-36, (pp), (dev), (isr))
#define PP_EnableInterrupt(pp, dev) __DDM_LVO_VOID_1A0(-42, (pp), (dev))
#define PP_DisableInterrupt(pp, dev) __DDM_LVO_VOID_1A0(-48, (pp), (dev))
#define PP_RegisterController(pp, ctrl) __DDM_LVO_RET_1A0(int32_t, -54, (pp), (ctrl))
#define PP_UnregisterController(pp, ctrl) __DDM_LVO_VOID_1A0(-60, (pp), (ctrl))
#define PP_FindController(pp, dev) __DDM_LVO_RET_1A0(struct parallel_port_controller *, -66, (pp), (dev))

#endif /* !PP_INTERNAL */

#endif /* PARALLELPORT_H_ */
