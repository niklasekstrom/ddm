/*
 * misc.resource LVO prototypes.
 *
 * These are AmigaOS resource LVOs (not DDM LVOs). The resource base
 * (misc.resource) is passed in a6.
 *
 * External callers use the macro stubs below; the implementation file
 * defines MISC_INTERNAL to suppress the macros.
 */
#ifndef MISC_PROTOS_H
#define MISC_PROTOS_H

#include "ddm_gcc.h" /* LVO stub macros */
#include <exec/types.h>

/* ------------------------------------------------------------------ */
/* LVO prototypes                                                      */
/* ------------------------------------------------------------------ */

/* LVO -6: Allocate a misc.resource unit. Returns NULL on success,
 * or the name of the current owner if already allocated. */
uint8_t *AllocMiscResource(struct Library *resource __asm("a6"), uint32_t unitNum __asm("d0"),
                           const char *name __asm("a1"));

/* LVO -12: Free a previously-allocated misc.resource unit. */
void FreeMiscResource(struct Library *resource __asm("a6"), uint32_t unitNum __asm("d0"));

/* ------------------------------------------------------------------ */
/* LVO stub macros                                                    */
/* ------------------------------------------------------------------ */

#ifndef MISC_INTERNAL

#define AllocMiscResource(resource, unitNum, name) __DDM_LVO_RET_2D0A1(uint8_t *, -6, (resource), (unitNum), (name))
#define FreeMiscResource(resource, unitNum) __DDM_LVO_VOID_1D0(-12, (resource), (unitNum))

#endif /* !MISC_INTERNAL */

#endif /* MISC_PROTOS_H */
