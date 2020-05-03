/*
 * SPI subsystem - spi.library
 *
 * Provides the SPI bus framework. Controller drivers register with
 * this library, and SPI device drivers use the API to transfer data
 * to/from SPI devices.
 */
#ifndef SPI_H_
#define SPI_H_

#include "ddm.h"        /* struct device, struct device_driver */
#include "ddm_gcc.h"    /* LVO stub macros */
#include "devicetree.h" /* BUS_TYPE_SPI (via ddm.h), DT_GetPropertyU32 */
#include <dos/dos.h>    /* BPTR */
#include <exec/libraries.h>
#include <exec/types.h>

/* Bus type for SPI devices (must match BUS_TYPE_SPI in devicetree.h) */
/* #define BUS_TYPE_SPI  3  -- defined in devicetree.h */

/* ------------------------------------------------------------------ */
/* SPI controller                                                      */
/* ------------------------------------------------------------------ */

/* A SPI controller is a device that physically drives an SPI bus.
 * The controller driver fills in the function pointers.
 * struct spi_controller is embedded in the controller driver's
 * private data structure, or can be allocated separately.
 */
struct spi_controller
{
    /* Node for the controllers list in SpiBase. Must NOT reuse
     * dev->node, which is already on DDMBase->devices. */
    struct Node node;

    /* The device tree device for this controller */
    struct device *dev;

    /* Function pointers - filled in by the controller driver */

    /* Set the bus speed in Hz */
    void (*set_speed)(struct spi_controller *ctrl __asm("a0"), uint32_t speed __asm("d0"));

    /* Select/deselect the SPI device (assert/deassert CS) */
    void (*select)(struct spi_controller *ctrl __asm("a0"), uint16_t chip_select __asm("d0"));
    void (*deselect)(struct spi_controller *ctrl __asm("a0"), uint16_t chip_select __asm("d0"));

    /* Transfer data. For full-duplex, read and write simultaneously.
     * For half-duplex (our hardware), write then read separately.
     * buf and size describe the data. If read_buf is NULL, only write.
     * If write_buf is NULL, only read. */
    void (*transfer)(struct spi_controller *ctrl __asm("a0"), const uint8_t *write_buf __asm("a1"),
                     uint8_t *read_buf __asm("a2"), uint32_t size __asm("d0"));

    /* Private data for the controller driver */
    void *private;
};

/* ------------------------------------------------------------------ */
/* SPI device                                                          */
/* ------------------------------------------------------------------ */

/* A SPI device is a device on an SPI bus (e.g. an SD card).
 * Created by spi.library when a controller registers and enumerates
 * its children from the device tree.
 */
struct spi_device
{
    /* Node for the spi_devices list in SpiBase. Must NOT reuse
     * dev->node, which is already on DDMBase->devices. */
    struct Node node;

    /* The device tree device for this SPI device */
    struct device *dev;

    /* The controller this device is connected to */
    struct spi_controller *controller;

    /* Chip select number (from the "reg" property) */
    uint16_t chip_select;

    /* Maximum speed in Hz (from "spi-max-frequency" property, 0 = default) */
    uint32_t max_speed;
};

/* ------------------------------------------------------------------ */
/* SPI library base                                                    */
/* ------------------------------------------------------------------ */

struct SpiBase
{
    struct Library lib;
    BPTR seg_list;           /* Segment list (saved at init) */
    struct DDMBase *ddmbase; /* ddm.library (opened in spi_init, closed in spi_expunge) */
    struct List controllers; /* List of registered controllers */
    struct List spi_devices; /* List of SPI devices */
};

/* ------------------------------------------------------------------ */
/* LVO prototypes                                                      */
/*                                                                    */
/* These declare the real LVO functions implemented in spi.c.        */
/* External callers use the macro stubs below; the implementation     */
/* file (spi.c) defines SPI_INTERNAL to suppress the macros.         */
/* ------------------------------------------------------------------ */

/* LVO -30: Register a SPI controller. The controller's dev must be
 * set. Creates spi_device structs for the controller's DTS children
 * and triggers matching. Returns 0 on success. */
int32_t SPI_RegisterController(struct SpiBase *sb __asm("a6"), struct spi_controller *ctrl __asm("a0"));

/* LVO -36: Unregister a SPI controller. Removes all its spi_devices. */
void SPI_UnregisterController(struct SpiBase *sb __asm("a6"), struct spi_controller *ctrl __asm("a0"));

/* LVO -42: Find the SPI device for a given device tree device.
 * Returns the spi_device, or NULL if not found. */
struct spi_device *SPI_FindDevice(struct SpiBase *sb __asm("a6"), struct device *dev __asm("a0"));

/* LVO -48: Set the bus speed (in Hz) for a SPI device's controller. */
void SPI_SetSpeed(struct SpiBase *sb __asm("a6"), struct spi_device *spidev __asm("a0"), uint32_t speed __asm("d0"));

/* LVO -54: Select a SPI device (assert CS via its controller). */
void SPI_Select(struct SpiBase *sb __asm("a6"), struct spi_device *spidev __asm("a0"));

/* LVO -60: Deselect a SPI device (deassert CS via its controller). */
void SPI_Deselect(struct SpiBase *sb __asm("a6"), struct spi_device *spidev __asm("a0"));

/* LVO -66: Transfer data to/from a SPI device.
 * If read_buf is NULL, only writes. If write_buf is NULL, only reads. */
void SPI_Transfer(struct SpiBase *sb __asm("a6"), struct spi_device *spidev __asm("a0"),
                  const uint8_t *write_buf __asm("a1"), uint8_t *read_buf __asm("a2"), uint32_t size __asm("d0"));

/* LVO -72: Register a SPI driver (a driver for SPI devices).
 * The driver self-registers on library/device init. */
int32_t SPI_RegisterDriver(struct SpiBase *sb __asm("a6"), struct device_driver *drv __asm("a0"));

/* LVO -78: Unregister a SPI driver. */
void SPI_UnregisterDriver(struct SpiBase *sb __asm("a6"), struct device_driver *drv __asm("a0"));

/* ------------------------------------------------------------------ */
/* LVO stub macros                                                    */
/*                                                                    */
/* External callers use these macros to make LVO calls through the     */
/* spi.library base in a6.                                            */
/*                                                                    */
/* The implementation file (spi.c) must define SPI_INTERNAL before    */
/* including this header to suppress the macro definitions.           */
/* ------------------------------------------------------------------ */

#ifndef SPI_INTERNAL

#define SPI_RegisterController(sb, ctrl) __DDM_LVO_RET_1A0(int32_t, -30, (sb), (ctrl))
#define SPI_UnregisterController(sb, ctrl) __DDM_LVO_VOID_1A0(-36, (sb), (ctrl))
#define SPI_FindDevice(sb, dev) __DDM_LVO_RET_1A0(struct spi_device *, -42, (sb), (dev))
#define SPI_SetSpeed(sb, spidev, speed) __DDM_LVO_VOID_2A0D0(-48, (sb), (spidev), (speed))
#define SPI_Select(sb, spidev) __DDM_LVO_VOID_1A0(-54, (sb), (spidev))
#define SPI_Deselect(sb, spidev) __DDM_LVO_VOID_1A0(-60, (sb), (spidev))
#define SPI_Transfer(sb, spidev, write_buf, read_buf, size)                                                            \
    __DDM_LVO_VOID_4A0A1A2D0(-66, (sb), (spidev), (write_buf), (read_buf), (size))
#define SPI_RegisterDriver(sb, drv) __DDM_LVO_RET_1A0(int32_t, -72, (sb), (drv))
#define SPI_UnregisterDriver(sb, drv) __DDM_LVO_VOID_1A0(-78, (sb), (drv))

#endif /* !SPI_INTERNAL */

#endif /* SPI_H_ */
