/*
 * Centralized debug/trace macros for the DDM framework.
 *
 * Each subsystem has its own compile-time flag and macro. When the
 * flag is defined, the macro expands to a call to kprintf() (provided
 * by debug.lib, linked via -ldebug), which writes to the serial port.
 * When the flag is not defined, the macro expands to ((void)0) and
 * emits no code — the arguments are not evaluated.
 *
 * Usage:
 *
 *   #include "ddm_debug.h"
 *
 *   DBG_SPI("transfer: cs=%lu speed=%lu bytes=%ld\n", cs, speed, n);
 *   DBG_IRQ("ERR: add handler virq=%ld failed\n", (long)virq);
 *
 * Severity is conveyed by convention in the format string prefix:
 *   "ERR: ..."   — errors
 *   "WARN: ..."  — warnings
 *   "INFO: ..."  — informational
 *   (no prefix)  — verbose trace
 *
 * Compile-time flags (set via -D on the gcc command line, or via the
 * build.sh environment variables of the same name):
 *
 *   DDM_DEBUG          Master flag — enables ALL subsystem flags below.
 *
 *   DDM_DEBUG_CORE     core/model.c, driver.c, config.c, romtag.c, ddm_util.c
 *   DDM_DEBUG_IRQ      core/irq.c
 *   DDM_DEBUG_GPIO     core/gpio.c
 *   DDM_DEBUG_DT       core/devicetree.c, parser.c
 *   DDM_DEBUG_SPI      subsystems/spi/spi.c
 *   DDM_DEBUG_MMC      subsystems/mmc/mmc.c
 *   DDM_DEBUG_PP       subsystems/parallelport/parallelport.c
 *   DDM_DEBUG_CP       subsystems/clockport/clockport.c
 *   DDM_DEBUG_CIA      drivers/cia-parallelport/cia_parallelport.c
 *   DDM_DEBUG_PAR_SPI  drivers/par-spi-adapter/par_spi_driver.c
 *   DDM_DEBUG_GAYLE    drivers/gayle-clockport/gayle_clockport.c
 *   DDM_DEBUG_SPIDER   drivers/spider/spider_driver.c
 *   DDM_DEBUG_MMC_SPI  drivers/mmc-spi/mmc_spi_device.c, sd.c, timer.c
 *
 * kprintf() uses raw serial I/O and does not call any OS services, so
 * it is safe to call from interrupt handlers.
 */
#ifndef DDM_DEBUG_H_
#define DDM_DEBUG_H_

#include <exec/types.h>

/* ------------------------------------------------------------------ */
/* Master flag: DDM_DEBUG enables every subsystem flag.               */
/* ------------------------------------------------------------------ */
#ifdef DDM_DEBUG
#ifndef DDM_DEBUG_CORE
#define DDM_DEBUG_CORE 1
#endif
#ifndef DDM_DEBUG_IRQ
#define DDM_DEBUG_IRQ 1
#endif
#ifndef DDM_DEBUG_GPIO
#define DDM_DEBUG_GPIO 1
#endif
#ifndef DDM_DEBUG_DT
#define DDM_DEBUG_DT 1
#endif
#ifndef DDM_DEBUG_SPI
#define DDM_DEBUG_SPI 1
#endif
#ifndef DDM_DEBUG_MMC
#define DDM_DEBUG_MMC 1
#endif
#ifndef DDM_DEBUG_PP
#define DDM_DEBUG_PP 1
#endif
#ifndef DDM_DEBUG_CP
#define DDM_DEBUG_CP 1
#endif
#ifndef DDM_DEBUG_CIA
#define DDM_DEBUG_CIA 1
#endif
#ifndef DDM_DEBUG_PAR_SPI
#define DDM_DEBUG_PAR_SPI 1
#endif
#ifndef DDM_DEBUG_GAYLE
#define DDM_DEBUG_GAYLE 1
#endif
#ifndef DDM_DEBUG_SPIDER
#define DDM_DEBUG_SPIDER 1
#endif
#ifndef DDM_DEBUG_MMC_SPI
#define DDM_DEBUG_MMC_SPI 1
#endif
#endif /* DDM_DEBUG */

/* ------------------------------------------------------------------ */
/* Pull in the kprintf declaration only when any debug flag is active. */
/* ------------------------------------------------------------------ */
#if defined(DDM_DEBUG_CORE) || defined(DDM_DEBUG_IRQ) || defined(DDM_DEBUG_GPIO) || defined(DDM_DEBUG_DT) ||           \
    defined(DDM_DEBUG_SPI) || defined(DDM_DEBUG_MMC) || defined(DDM_DEBUG_PP) || defined(DDM_DEBUG_CP) ||              \
    defined(DDM_DEBUG_CIA) || defined(DDM_DEBUG_PAR_SPI) || defined(DDM_DEBUG_GAYLE) || defined(DDM_DEBUG_SPIDER) ||   \
    defined(DDM_DEBUG_MMC_SPI)
#ifdef __GNUC__
#include <clib/debug_protos.h> /* kprintf (GCC NDK has no proto/debug.h) */
#else
#include <proto/debug.h> /* kprintf */
#endif
#endif

/* ------------------------------------------------------------------ */
/* Per-subsystem macros.                                              */
/*                                                                    */
/* When enabled, the macro calls kprintf via the DDM_KPRINTF wrapper, */
/* which casts the format string to CONST_STRPTR (const unsigned      */
/* char *) to match kprintf's declaration in the AmigaOS NDK.         */
/* String literals in C are char *, so without this cast GCC warns    */
/* about pointer-sign mismatch.                                       */
/*                                                                    */
/* When disabled, the macro is a C99 variadic macro that discards     */
/* its arguments and evaluates to ((void)0), producing no code.       */
/* ------------------------------------------------------------------ */

#define DDM_KPRINTF(fmt, ...) kprintf((CONST_STRPTR)(fmt), ##__VA_ARGS__)

#ifdef DDM_DEBUG_CORE
#define DBG_CORE DDM_KPRINTF
#else
#define DBG_CORE(...) ((void)0)
#endif

#ifdef DDM_DEBUG_IRQ
#define DBG_IRQ DDM_KPRINTF
#else
#define DBG_IRQ(...) ((void)0)
#endif

#ifdef DDM_DEBUG_GPIO
#define DBG_GPIO DDM_KPRINTF
#else
#define DBG_GPIO(...) ((void)0)
#endif

#ifdef DDM_DEBUG_DT
#define DBG_DT DDM_KPRINTF
#else
#define DBG_DT(...) ((void)0)
#endif

#ifdef DDM_DEBUG_SPI
#define DBG_SPI DDM_KPRINTF
#else
#define DBG_SPI(...) ((void)0)
#endif

#ifdef DDM_DEBUG_MMC
#define DBG_MMC DDM_KPRINTF
#else
#define DBG_MMC(...) ((void)0)
#endif

#ifdef DDM_DEBUG_PP
#define DBG_PP DDM_KPRINTF
#else
#define DBG_PP(...) ((void)0)
#endif

#ifdef DDM_DEBUG_CP
#define DBG_CP DDM_KPRINTF
#else
#define DBG_CP(...) ((void)0)
#endif

#ifdef DDM_DEBUG_CIA
#define DBG_CIA DDM_KPRINTF
#else
#define DBG_CIA(...) ((void)0)
#endif

#ifdef DDM_DEBUG_PAR_SPI
#define DBG_PAR_SPI DDM_KPRINTF
#else
#define DBG_PAR_SPI(...) ((void)0)
#endif

#ifdef DDM_DEBUG_GAYLE
#define DBG_GAYLE DDM_KPRINTF
#else
#define DBG_GAYLE(...) ((void)0)
#endif

#ifdef DDM_DEBUG_SPIDER
#define DBG_SPIDER DDM_KPRINTF
#else
#define DBG_SPIDER(...) ((void)0)
#endif

#ifdef DDM_DEBUG_MMC_SPI
#define DBG_MMC_SPI DDM_KPRINTF
#else
#define DBG_MMC_SPI(...) ((void)0)
#endif

#endif /* DDM_DEBUG_H_ */
