#!/bin/bash
# Top-level build script for the device driver model (DDM) project.

set -e

# GCC AmigaOS toolchain (m68k-amigaos-gcc 6.5.0b)
AMIGA_GCC="${AMIGA_GCC:-/opt/amiga}"
CC="$AMIGA_GCC/bin/m68k-amigaos-gcc"
NDK_INCLUDE="$AMIGA_GCC/m68k-amigaos/ndk-include"
NDK_LIB="$AMIGA_GCC/m68k-amigaos/lib"

if [ ! -x "$CC" ]; then
    echo "Error: m68k-amigaos-gcc not found at $CC" >&2
    echo "Set AMIGA_GCC to your m68k-amigaos-gcc installation directory." >&2
    exit 1
fi

if [ ! -d "$NDK_INCLUDE" ]; then
    echo "Error: NDK includes not found at $NDK_INCLUDE" >&2
    echo "Set AMIGA_GCC to your m68k-amigaos-gcc installation directory." >&2
    exit 1
fi

# GCC flags for AmigaOS m68k development:
#   -m68000    : target the 68000 CPU (A500/A600/A1200 base)
#   -noixemul  : don't use ixemul (use direct AmigaOS calls)
#   -nostdlib  : don't link standard C library startup
#   -O2        : optimization level 2
#   -Wall      : enable common warnings
CFLAGS="-m68000 -noixemul -nostdlib -O2 -Wall -I$NDK_INCLUDE -L$NDK_LIB"

# Optional debug/trace output via debug.lib (kprintf to serial).
#
# Set DDM_DEBUG=1 to enable ALL subsystems, or set individual flags:
#   DDM_DEBUG_CORE, DDM_DEBUG_IRQ, DDM_DEBUG_GPIO, DDM_DEBUG_DT,
#   DDM_DEBUG_SPI, DDM_DEBUG_MMC, DDM_DEBUG_PP, DDM_DEBUG_CP,
#   DDM_DEBUG_CIA, DDM_DEBUG_PAR_SPI, DDM_DEBUG_GAYLE,
#   DDM_DEBUG_SPIDER, DDM_DEBUG_MMC_SPI
DEBUG_FLAGS=""

# add_debug_flag <env_var> <define>
# If $env_var is 1, append -D<define> to DEBUG_FLAGS.
add_debug_flag() {
    if [ "${!1:-0}" = "1" ]; then
        DEBUG_FLAGS="$DEBUG_FLAGS -D$2"
    fi
}

# Master flag enables all subsystem flags (the header cascades them).
add_debug_flag DDM_DEBUG       DDM_DEBUG
# Per-subsystem flags (usable on their own, without DDM_DEBUG).
add_debug_flag DDM_DEBUG_CORE     DDM_DEBUG_CORE
add_debug_flag DDM_DEBUG_IRQ      DDM_DEBUG_IRQ
add_debug_flag DDM_DEBUG_GPIO     DDM_DEBUG_GPIO
add_debug_flag DDM_DEBUG_DT       DDM_DEBUG_DT
add_debug_flag DDM_DEBUG_SPI      DDM_DEBUG_SPI
add_debug_flag DDM_DEBUG_MMC      DDM_DEBUG_MMC
add_debug_flag DDM_DEBUG_PP       DDM_DEBUG_PP
add_debug_flag DDM_DEBUG_CP       DDM_DEBUG_CP
add_debug_flag DDM_DEBUG_CIA      DDM_DEBUG_CIA
add_debug_flag DDM_DEBUG_PAR_SPI  DDM_DEBUG_PAR_SPI
add_debug_flag DDM_DEBUG_GAYLE    DDM_DEBUG_GAYLE
add_debug_flag DDM_DEBUG_SPIDER   DDM_DEBUG_SPIDER
add_debug_flag DDM_DEBUG_MMC_SPI  DDM_DEBUG_MMC_SPI

CFLAGS="$CFLAGS$DEBUG_FLAGS"

# Libraries linked into every component.
#   -lamiga : AmigaOS exec/dos/etc. functions
#   -ldebug : debug.lib (kprintf for debug output)
#   -lc     : C library (strcmp, strlen, memset, __mulsi3, __udivsi3, etc.)
LIBS="-lamiga -ldebug -lc"

# Project root (directory containing this script)
ROOT="$(cd "$(dirname "$0")" && pwd)"

# Output directory — keeps binaries out of the source tree
BUILD="$ROOT/build"
mkdir -p "$BUILD"

# --- Build steps (dependency order) ----------------------------------------
# Each step cd's into its source directory (for relative -I paths), runs
# m68k-amigaos-gcc, and outputs to $BUILD/.

echo "Building ddm.library..."
( cd "$ROOT/core" && $CC $CFLAGS -I../include \
    model.c driver.c config.c irq.c romtag.c devicetree.c parser.c gpio.c ddm_util.c zorro.c overlay.c \
    $LIBS -o "$BUILD/ddm.library" ) || exit 1

echo "Building spi.library..."
( cd "$ROOT/subsystems/spi" && $CC $CFLAGS -I../../include \
    spi.c $LIBS -o "$BUILD/spi.library" ) || exit 1

echo "Building mmc.library..."
( cd "$ROOT/subsystems/mmc" && $CC $CFLAGS -I../../include \
    mmc.c $LIBS -o "$BUILD/mmc.library" ) || exit 1

echo "Building parallelport.library..."
( cd "$ROOT/subsystems/parallelport" && $CC $CFLAGS -I../../include \
    parallelport.c $LIBS -o "$BUILD/parallelport.library" ) || exit 1

echo "Building clockport.library..."
( cd "$ROOT/subsystems/clockport" && $CC $CFLAGS -I../../include \
    clockport.c $LIBS -o "$BUILD/clockport.library" ) || exit 1

echo "Building cia-parallelport.library..."
( cd "$ROOT/drivers/cia-parallelport" && $CC $CFLAGS -I../../include -I../../subsystems/parallelport \
    cia_parallelport.c $LIBS -o "$BUILD/cia-parallelport.library" ) || exit 1

echo "Building par-spi.library..."
( cd "$ROOT/drivers/par-spi-adapter" && $CC $CFLAGS -I../../include -I../../subsystems/spi -I../../subsystems/parallelport \
    par_spi_driver.c $LIBS -o "$BUILD/par-spi.library" ) || exit 1

echo "Building gayle-clockport.library..."
( cd "$ROOT/drivers/gayle-clockport" && $CC $CFLAGS -I../../include -I../../subsystems/clockport \
    gayle_clockport.c $LIBS -o "$BUILD/gayle-clockport.library" ) || exit 1

echo "Building spider.library..."
( cd "$ROOT/drivers/spider" && $CC $CFLAGS -I../../include -I../../subsystems/spi -I../../subsystems/clockport \
    spider_driver.c $LIBS -o "$BUILD/spider.library" ) || exit 1

echo "Building mmc-spi.device..."
( cd "$ROOT/drivers/mmc-spi" && $CC $CFLAGS -I../../include -I../../subsystems/spi -I../../subsystems/mmc \
    mmc_spi.c $LIBS -o "$BUILD/mmc-spi.device" ) || exit 1
