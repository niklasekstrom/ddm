/*
 * GCC macros for the DDM framework.
 *
 * This header provides the macros needed to compile the DDM codebase
 * with m68k-amigaos-gcc. It is automatically included by ddm.h, which
 * is included (directly or transitively) by every source file in the
 * project.
 *
 * 1. Register parameters:
 *    void f(TYPE arg __asm("a0"))
 *
 * 2. LVO inline-asm stubs:
 *    statement-expression macro with inline asm (see below).
 *
 * 3. kprintf:
 *    GCC NDK: <clib/debug_protos.h>  (no proto/debug.h exists)
 */
#ifndef DDM_GCC_H_
#define DDM_GCC_H_

#include <stdint.h> /* uint32_t for LVO stub register variables */

/* ------------------------------------------------------------------ */
/* LVO call stub macros                                               */
/*                                                                    */
/* These generate inline-asm calls through a library base in a6.      */
/* They are used in headers to provide callable stubs for LVOs.       */
/*                                                                    */
/* In implementation files that DEFINE an LVO function, the           */
/* corresponding macro must be #undef'd before the definition.        */
/* We provide DDM_LVO_UNDEF() helpers for each library.               */
/* ------------------------------------------------------------------ */

/* Helper: declare the function prototype (so callers and impl match) */
/* This is done in the regular header sections. */

/* LVO stub generator macros.
 * Each produces a GCC statement-expression that:
 *   - loads the library base into a6
 *   - loads arguments into the specified registers
 *   - does jsr a6@(offset:W)
 *   - returns the d0 result
 *
 * Per the AmigaOS m68k calling convention, a jsr to an LVO clobbers
 * d0, d1, a0, and a1 (caller-saved scratch registers). a6 is preserved
 * (callee-saved). We use read-write constraints ("+a", "+d") for all
 * input registers that the jsr clobbers, so GCC's optimizer does not
 * assume they survive the call. a6 stays read-only ("a") since the
 * callee preserves it. Registers that are NOT inputs (e.g. d1 when it is
 * not an argument) are listed in the clobber list instead. */

#define __DDM_LVO_VOID_1A0(offset, a6val, a0val)                                                                       \
    ({                                                                                                                 \
        register void *__a6 __asm("a6") = (void *)(a6val);                                                             \
        register void *__a0 __asm("a0") = (void *)(a0val);                                                             \
        __asm volatile("jsr %%a6@(" #offset ":W)" : "+a"(__a0) : "a"(__a6) : "d0", "d1", "a1", "cc", "memory");        \
    })

#define __DDM_LVO_RET_1A0(rettype, offset, a6val, a0val)                                                               \
    ({                                                                                                                 \
        register void *__a6 __asm("a6") = (void *)(a6val);                                                             \
        register void *__a0 __asm("a0") = (void *)(a0val);                                                             \
        register void *__ret __asm("d0");                                                                              \
        __asm volatile("jsr %%a6@(" #offset ":W)" : "=d"(__ret), "+a"(__a0) : "a"(__a6) : "d1", "a1", "cc", "memory"); \
        (rettype)(long) __ret;                                                                                         \
    })

#define __DDM_LVO_RET_1D0(rettype, offset, a6val, d0val)                                                               \
    ({                                                                                                                 \
        register void *__a6 __asm("a6") = (void *)(a6val);                                                             \
        register uint32_t __d0 __asm("d0") = (uint32_t)(d0val);                                                        \
        __asm volatile("jsr %%a6@(" #offset ":W)" : "+d"(__d0) : "a"(__a6) : "d1", "a0", "a1", "cc", "memory");        \
        (rettype)(long) __d0;                                                                                          \
    })

#define __DDM_LVO_RET_2A0A1(rettype, offset, a6val, a0val, a1val)                                                      \
    ({                                                                                                                 \
        register void *__a6 __asm("a6") = (void *)(a6val);                                                             \
        register void *__a0 __asm("a0") = (void *)(a0val);                                                             \
        register void *__a1 __asm("a1") = (void *)(a1val);                                                             \
        register void *__ret __asm("d0");                                                                              \
        __asm volatile("jsr %%a6@(" #offset ":W)"                                                                      \
                       : "=d"(__ret), "+a"(__a0), "+a"(__a1)                                                           \
                       : "a"(__a6)                                                                                     \
                       : "d1", "cc", "memory");                                                                        \
        (rettype)(long) __ret;                                                                                         \
    })

#define __DDM_LVO_RET_2A0D0(rettype, offset, a6val, a0val, d0val)                                                      \
    ({                                                                                                                 \
        register void *__a6 __asm("a6") = (void *)(a6val);                                                             \
        register void *__a0 __asm("a0") = (void *)(a0val);                                                             \
        register uint32_t __d0 __asm("d0") = (uint32_t)(d0val);                                                        \
        __asm volatile("jsr %%a6@(" #offset ":W)" : "+d"(__d0), "+a"(__a0) : "a"(__a6) : "d1", "a1", "cc", "memory");  \
        (rettype)(long) __d0;                                                                                          \
    })

#define __DDM_LVO_RET_3A0A1D0(rettype, offset, a6val, a0val, a1val, d0val)                                             \
    ({                                                                                                                 \
        register void *__a6 __asm("a6") = (void *)(a6val);                                                             \
        register void *__a0 __asm("a0") = (void *)(a0val);                                                             \
        register void *__a1 __asm("a1") = (void *)(a1val);                                                             \
        register uint32_t __d0 __asm("d0") = (uint32_t)(d0val);                                                        \
        __asm volatile("jsr %%a6@(" #offset ":W)"                                                                      \
                       : "+d"(__d0), "+a"(__a0), "+a"(__a1)                                                            \
                       : "a"(__a6)                                                                                     \
                       : "d1", "cc", "memory");                                                                        \
        (rettype)(long) __d0;                                                                                          \
    })

#define __DDM_LVO_RET_3A0A1A2(rettype, offset, a6val, a0val, a1val, a2val)                                             \
    ({                                                                                                                 \
        register void *__a6 __asm("a6") = (void *)(a6val);                                                             \
        register void *__a0 __asm("a0") = (void *)(a0val);                                                             \
        register void *__a1 __asm("a1") = (void *)(a1val);                                                             \
        register void *__a2 __asm("a2") = (void *)(a2val);                                                             \
        register void *__ret __asm("d0");                                                                              \
        __asm volatile("jsr %%a6@(" #offset ":W)"                                                                      \
                       : "=d"(__ret), "+a"(__a0), "+a"(__a1), "+a"(__a2)                                               \
                       : "a"(__a6)                                                                                     \
                       : "d1", "cc", "memory");                                                                        \
        (rettype)(long) __ret;                                                                                         \
    })

#define __DDM_LVO_RET_3A0D0D1(rettype, offset, a6val, a0val, d0val, d1val)                                             \
    ({                                                                                                                 \
        register void *__a6 __asm("a6") = (void *)(a6val);                                                             \
        register void *__a0 __asm("a0") = (void *)(a0val);                                                             \
        register uint32_t __d0 __asm("d0") = (uint32_t)(d0val);                                                        \
        register uint32_t __d1 __asm("d1") = (uint32_t)(d1val);                                                        \
        __asm volatile("jsr %%a6@(" #offset ":W)"                                                                      \
                       : "+d"(__d0), "+a"(__a0), "+d"(__d1)                                                            \
                       : "a"(__a6)                                                                                     \
                       : "a1", "cc", "memory");                                                                        \
        (rettype)(long) __d0;                                                                                          \
    })

#define __DDM_LVO_RET_4A0A1A2D0(rettype, offset, a6val, a0val, a1val, a2val, d0val)                                    \
    ({                                                                                                                 \
        register void *__a6 __asm("a6") = (void *)(a6val);                                                             \
        register void *__a0 __asm("a0") = (void *)(a0val);                                                             \
        register void *__a1 __asm("a1") = (void *)(a1val);                                                             \
        register void *__a2 __asm("a2") = (void *)(a2val);                                                             \
        register uint32_t __d0 __asm("d0") = (uint32_t)(d0val);                                                        \
        __asm volatile("jsr %%a6@(" #offset ":W)"                                                                      \
                       : "+d"(__d0), "+a"(__a0), "+a"(__a1), "+a"(__a2)                                                \
                       : "a"(__a6)                                                                                     \
                       : "d1", "cc", "memory");                                                                        \
        (rettype)(long) __d0;                                                                                          \
    })

#define __DDM_LVO_RET_4A0A1D0D1(rettype, offset, a6val, a0val, a1val, d0val, d1val)                                    \
    ({                                                                                                                 \
        register void *__a6 __asm("a6") = (void *)(a6val);                                                             \
        register void *__a0 __asm("a0") = (void *)(a0val);                                                             \
        register void *__a1 __asm("a1") = (void *)(a1val);                                                             \
        register uint32_t __d0 __asm("d0") = (uint32_t)(d0val);                                                        \
        register uint32_t __d1 __asm("d1") = (uint32_t)(d1val);                                                        \
        __asm volatile("jsr %%a6@(" #offset ":W)"                                                                      \
                       : "+d"(__d0), "+a"(__a0), "+a"(__a1), "+d"(__d1)                                                \
                       : "a"(__a6)                                                                                     \
                       : "cc", "memory");                                                                              \
        (rettype)(long) __d0;                                                                                          \
    })

#define __DDM_LVO_VOID_2A0A1(offset, a6val, a0val, a1val)                                                              \
    ({                                                                                                                 \
        register void *__a6 __asm("a6") = (void *)(a6val);                                                             \
        register void *__a0 __asm("a0") = (void *)(a0val);                                                             \
        register void *__a1 __asm("a1") = (void *)(a1val);                                                             \
        __asm volatile("jsr %%a6@(" #offset ":W)" : "+a"(__a0), "+a"(__a1) : "a"(__a6) : "d0", "d1", "cc", "memory");  \
    })

#define __DDM_LVO_VOID_2A0D0(offset, a6val, a0val, d0val)                                                              \
    ({                                                                                                                 \
        register void *__a6 __asm("a6") = (void *)(a6val);                                                             \
        register void *__a0 __asm("a0") = (void *)(a0val);                                                             \
        register uint32_t __d0 __asm("d0") = (uint32_t)(d0val);                                                        \
        __asm volatile("jsr %%a6@(" #offset ":W)" : "+a"(__a0), "+d"(__d0) : "a"(__a6) : "d1", "a1", "cc", "memory");  \
    })

#define __DDM_LVO_VOID_1D0(offset, a6val, d0val)                                                                       \
    ({                                                                                                                 \
        register void *__a6 __asm("a6") = (void *)(a6val);                                                             \
        register uint32_t __d0 __asm("d0") = (uint32_t)(d0val);                                                        \
        __asm volatile("jsr %%a6@(" #offset ":W)" : "+d"(__d0) : "a"(__a6) : "d1", "a0", "a1", "cc", "memory");        \
    })

#define __DDM_LVO_VOID_3A0A1D0(offset, a6val, a0val, a1val, d0val)                                                     \
    ({                                                                                                                 \
        register void *__a6 __asm("a6") = (void *)(a6val);                                                             \
        register void *__a0 __asm("a0") = (void *)(a0val);                                                             \
        register void *__a1 __asm("a1") = (void *)(a1val);                                                             \
        register uint32_t __d0 __asm("d0") = (uint32_t)(d0val);                                                        \
        __asm volatile("jsr %%a6@(" #offset ":W)"                                                                      \
                       : "+a"(__a0), "+a"(__a1), "+d"(__d0)                                                            \
                       : "a"(__a6)                                                                                     \
                       : "d1", "cc", "memory");                                                                        \
    })

#define __DDM_LVO_VOID_3A0D0D1(offset, a6val, a0val, d0val, d1val)                                                     \
    ({                                                                                                                 \
        register void *__a6 __asm("a6") = (void *)(a6val);                                                             \
        register void *__a0 __asm("a0") = (void *)(a0val);                                                             \
        register uint32_t __d0 __asm("d0") = (uint32_t)(d0val);                                                        \
        register uint32_t __d1 __asm("d1") = (uint32_t)(d1val);                                                        \
        __asm volatile("jsr %%a6@(" #offset ":W)"                                                                      \
                       : "+a"(__a0), "+d"(__d0), "+d"(__d1)                                                            \
                       : "a"(__a6)                                                                                     \
                       : "a1", "cc", "memory");                                                                        \
    })

#define __DDM_LVO_VOID_4A0A1A2D0(offset, a6val, a0val, a1val, a2val, d0val)                                            \
    ({                                                                                                                 \
        register void *__a6 __asm("a6") = (void *)(a6val);                                                             \
        register void *__a0 __asm("a0") = (void *)(a0val);                                                             \
        register void *__a1 __asm("a1") = (void *)(a1val);                                                             \
        register void *__a2 __asm("a2") = (void *)(a2val);                                                             \
        register uint32_t __d0 __asm("d0") = (uint32_t)(d0val);                                                        \
        __asm volatile("jsr %%a6@(" #offset ":W)"                                                                      \
                       : "+a"(__a0), "+a"(__a1), "+a"(__a2), "+d"(__d0)                                                \
                       : "a"(__a6)                                                                                     \
                       : "d1", "cc", "memory");                                                                        \
    })

#define __DDM_LVO_VOID_4A0A1D0D1(offset, a6val, a0val, a1val, d0val, d1val)                                            \
    ({                                                                                                                 \
        register void *__a6 __asm("a6") = (void *)(a6val);                                                             \
        register void *__a0 __asm("a0") = (void *)(a0val);                                                             \
        register void *__a1 __asm("a1") = (void *)(a1val);                                                             \
        register uint32_t __d0 __asm("d0") = (uint32_t)(d0val);                                                        \
        register uint32_t __d1 __asm("d1") = (uint32_t)(d1val);                                                        \
        __asm volatile("jsr %%a6@(" #offset ":W)"                                                                      \
                       : "+a"(__a0), "+a"(__a1), "+d"(__d0), "+d"(__d1)                                                \
                       : "a"(__a6)                                                                                     \
                       : "cc", "memory");                                                                              \
    })

#define __DDM_LVO_RET_2D0A1(rettype, offset, a6val, d0val, a1val)                                                      \
    ({                                                                                                                 \
        register void *__a6 __asm("a6") = (void *)(a6val);                                                             \
        register uint32_t __d0 __asm("d0") = (uint32_t)(d0val);                                                        \
        register void *__a1 __asm("a1") = (void *)(a1val);                                                             \
        __asm volatile("jsr %%a6@(" #offset ":W)" : "+d"(__d0), "+a"(__a1) : "a"(__a6) : "d1", "a0", "cc", "memory");  \
        (rettype)(long) __d0;                                                                                          \
    })

#define __DDM_LVO_RET_3D0A1A2(rettype, offset, a6val, d0val, a1val, a2val)                                             \
    ({                                                                                                                 \
        register void *__a6 __asm("a6") = (void *)(a6val);                                                             \
        register uint32_t __d0 __asm("d0") = (uint32_t)(d0val);                                                        \
        register void *__a1 __asm("a1") = (void *)(a1val);                                                             \
        register void *__a2 __asm("a2") = (void *)(a2val);                                                             \
        __asm volatile("jsr %%a6@(" #offset ":W)"                                                                      \
                       : "+d"(__d0), "+a"(__a1), "+a"(__a2)                                                            \
                       : "a"(__a6)                                                                                     \
                       : "d1", "a0", "cc", "memory");                                                                  \
        (rettype)(long) __d0;                                                                                          \
    })

#define __DDM_LVO_VOID_2D0A1(offset, a6val, d0val, a1val)                                                              \
    ({                                                                                                                 \
        register void *__a6 __asm("a6") = (void *)(a6val);                                                             \
        register uint32_t __d0 __asm("d0") = (uint32_t)(d0val);                                                        \
        register void *__a1 __asm("a1") = (void *)(a1val);                                                             \
        __asm volatile("jsr %%a6@(" #offset ":W)" : "+d"(__d0), "+a"(__a1) : "a"(__a6) : "d1", "a0", "cc", "memory");  \
    })

#endif /* DDM_GCC_H_ */
