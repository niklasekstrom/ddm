/*
 * CIA.resource LVO prototypes.
 *
 * These are AmigaOS resource LVOs (not DDM LVOs). The resource base
 * (ciaa.resource or ciab.resource) is passed in a6.
 *
 * External callers use the macro stubs below; the implementation file
 * defines CIA_INTERNAL to suppress the macros.
 */
#ifndef CIA_PROTOS_H
#define CIA_PROTOS_H

#include "ddm_gcc.h" /* LVO stub macros */
#include <exec/interrupts.h>
#include <exec/libraries.h>
#include <exec/types.h>

/* ------------------------------------------------------------------ */
/* LVO prototypes                                                      */
/* ------------------------------------------------------------------ */

/* LVO -6: Add an interrupt handler for a CIA interrupt bit. */
struct Interrupt *AddICRVector(struct Library *resource __asm("a6"), int32_t iCRBit __asm("d0"),
                               struct Interrupt *interrupt __asm("a1"));

/* LVO -12: Remove a previously-added interrupt handler. */
void RemICRVector(struct Library *resource __asm("a6"), int32_t iCRBit __asm("d0"),
                  struct Interrupt *interrupt __asm("a1"));

/* LVO -18: Enable/disable CIA interrupt bits (mask). */
int16_t AbleICR(struct Library *resource __asm("a6"), int32_t mask __asm("d0"));

/* LVO -24: Set CIA interrupt bits (mask). */
int16_t SetICR(struct Library *resource __asm("a6"), int32_t mask __asm("d0"));

/* ------------------------------------------------------------------ */
/* LVO stub macros                                                    */
/* ------------------------------------------------------------------ */

#ifndef CIA_INTERNAL

#define AddICRVector(resource, iCRBit, interrupt)                                                                      \
    __DDM_LVO_RET_2D0A1(struct Interrupt *, -6, (resource), (iCRBit), (interrupt))
#define RemICRVector(resource, iCRBit, interrupt) __DDM_LVO_VOID_2D0A1(-12, (resource), (iCRBit), (interrupt))
#define AbleICR(resource, mask) __DDM_LVO_RET_1D0(int16_t, -18, (resource), (mask))
#define SetICR(resource, mask) __DDM_LVO_RET_1D0(int16_t, -24, (resource), (mask))

#endif /* !CIA_INTERNAL */

#endif /* CIA_PROTOS_H */
