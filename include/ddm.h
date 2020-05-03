/*
 * DDM (Device Driver Model) framework for AmigaOS
 *
 * The core of the DDM, inspired by Linux's
 * drivers/base. Provides struct device and struct device_driver,
 * driver registration, device registration, tree management, and
 * the matching engine that binds drivers to devices by compatible
 * strings. It also provides the interrupt framework (IRQ_* LVOs,
 * implemented in irq.c; see irq.h).
 *
 * The device tree parser is part of the core (implemented in
 * devicetree.c and parser.c); it populates this model from a DTS file.
 * Other subsystems (spi, mmc, ...) build on top of the core.
 */
#ifndef DDM_H_
#define DDM_H_

#include "ddm_gcc.h" /* GCC macros */
#include <dos/dos.h> /* BPTR */
#include <exec/libraries.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/types.h>

/* Forward declarations */
struct device;
struct device_driver;
struct bus_type;
struct device_node; /* defined in devicetree.h */
struct irq_domain;  /* defined in irq.h */
struct irq_desc;    /* defined in irq.h */

/* Tracking node for a library/device opened by the config loader.
 * Stored in DDMBase.loaded_libs; closed by ddm_expunge. */
struct loaded_lib
{
    struct Node node;
    struct Library *lib;   /* Library base (NULL for devices) */
    struct IORequest *ior; /* IORequest for devices (NULL for libs) */
    int16_t is_device;     /* TRUE if opened via OpenDevice */
};

/* ------------------------------------------------------------------ */
/* Bus type constants                                                  */
/* ------------------------------------------------------------------ */

#define BUS_TYPE_NONE 0
#define BUS_TYPE_PLATFORM 1
#define BUS_TYPE_PARALLEL_PORT 2
#define BUS_TYPE_SPI 3
#define BUS_TYPE_MMC 4
#define BUS_TYPE_GPIO 5
#define BUS_TYPE_CLOCKPORT 6

/* ------------------------------------------------------------------ */
/* Device flags                                                        */
/* ------------------------------------------------------------------ */

#define DEV_FLAG_BOUND 0x01     /* A driver is bound to this device */
#define DEV_FLAG_PROBED 0x02    /* DDMDrv_Probe was called successfully */
#define DEV_FLAG_RESERVED 0x04  /* Reserved (was DEV_FLAG_INITIALIZED) */
#define DEV_FLAG_FROM_TREE 0x08 /* Device was created from the parsed device tree */
#define DEV_FLAG_DEFERRED 0x10  /* Probe returned DDMDRV_PROBE_DEFER; retry later */

/* ------------------------------------------------------------------ */
/* Device - represents a hardware device in the model                 */
/* ------------------------------------------------------------------ */

struct device
{
    struct Node node; /* ln_Name = device name (e.g. "mmc-spi@0").
                       * Used for linking in the global device list. */
    struct device *parent;
    struct device *children; /* First child (linked list via next_sibling) */
    struct device *next_sibling;
    struct device_driver *driver; /* Bound driver (NULL if unmatched) */
    void *driver_data;            /* Private data for the bound driver */
    void *bus_data;               /* Private data for the bus subsystem */
    struct device_node *of_node;  /* Device tree node for this device (NULL if
                                   * not described by a device tree). Holds the
                                   * full path and properties. */
    uint32_t bus_type;            /* Which subsystem this device belongs to */
    uint32_t flags;
    struct irq_domain *irq_domain; /* Non-NULL if this device is an
                                    * interrupt controller. Set by
                                    * IRQ_CreateDomain; walked by
                                    * IRQ_FindDomain and DT_GetInterrupt. */
};

/* Convenience: device name is node.ln_Name */
#define dev_name(dev) ((dev)->node.ln_Name)

/* ------------------------------------------------------------------ */
/* Device driver - represents a driver for some device                */
/* ------------------------------------------------------------------ */

struct device_driver
{
    struct Node node;         /* ln_Name = driver name.
                               * Used for linking in the driver list. */
    const char **compatible;  /* NULL-terminated array of compatible strings */
    struct Library *lib_base; /* Library/device base for LVO calls */
    uint32_t bus_type;        /* Which subsystem this driver belongs to */
    void *private;            /* Driver-private data */
};

/* Convenience: driver name is node.ln_Name */
#define drv_name(drv) ((drv)->node.ln_Name)

/* ------------------------------------------------------------------ */
/* Bus type - represents a subsystem (like spi, mmc, etc.)            */
/* ------------------------------------------------------------------ */

struct bus_type
{
    struct Node node;         /* ln_Name = bus name */
    uint32_t bus_type_id;     /* Bus type ID (one of the BUS_TYPE_* constants) */
    struct Library *lib_base; /* Subsystem library base */

    /* Optional bus-specific match function. If NULL, the matching
     * engine falls back to compatible-string matching (the default
     * for device-tree-described devices). If non-NULL, the engine
     * calls this instead, allowing non-compatible matching (e.g.
     * PCI vendor/device IDs). Returns TRUE if the driver matches. */
    int16_t (*match)(struct device *dev __asm("a0"), struct device_driver *drv __asm("a1"));
};

/* Convenience: bus name is node.ln_Name */
#define bus_name(bus) ((bus)->node.ln_Name)

/* ------------------------------------------------------------------ */
/* DDMBase - library base for ddm.library                             */
/* ------------------------------------------------------------------ */

struct DDMBase
{
    struct Library lib;
    BPTR seg_list;           /* Segment list (saved at init) */
    struct device *root;     /* Root of the device tree */
    struct List devices;     /* Global list of all devices */
    struct List drivers;     /* List of all registered drivers */
    struct List bus_types;   /* List of registered bus types */
    struct List controllers; /* List of registered GPIO controllers */

    /* Global virq space. One irq_desc pointer per allocated virq.
     * Indexed by virq; NULL slots are free. Allocated in ddm_init. */
    struct irq_desc **irq_descs;
    uint32_t nr_irqs;   /* Capacity of irq_descs (NR_IRQS) */
    uint32_t next_virq; /* Hint for the next free slot */

    /* Count of devices still in DEV_FLAG_DEFERRED state when
     * DDM_MatchAll exits. Non-zero means the matching engine hit
     * the pass limit with unresolved dependencies. Callers can
     * inspect this to detect stuck driver dependencies. */
    uint32_t deferred_count;

    /* Lazy bootstrap state. The framework bootstraps itself on the
     * first DDM_RegisterDriver call: it parses the device tree
     * (DT_ParseTree) and loads the config (DDM_LoadConfig), which
     * opens all remaining driver libraries and runs DDM_MatchAll.
     * This makes the framework self-initialising regardless of which
     * component is loaded first.
     *
     *   bootstrapped  — TRUE after bootstrap has completed (success
     *                   or failure). Prevents re-running on subsequent
     *                   driver registrations.
     *   bootstrapping — TRUE while bootstrap is in progress. Guards
     *                   against re-entry: drivers opened by
     *                   DDM_LoadConfig call DDM_RegisterDriver, see
     *                   this flag, and skip bootstrap. */
    int16_t bootstrapped;
    int16_t bootstrapping;

    /* Name of the driver that triggered the bootstrap (the first
     * DDM_RegisterDriver call). The config loader skips opening
     * this device to avoid loading a second copy while its init is
     * still on the call stack. NULL after bootstrap completes. */
    const char *bootstrap_trigger;

    /* Tracking list of libraries/devices opened by the config
     * loader (read_config_file). ddm_expunge walks this list and
     * closes each entry before freeing the library base. */
    struct List loaded_libs;
};

/* Capacity of the global virq space. This is a deliberate compile-time
 * constant — ample for the current hardware (the whole tree uses ~5
 * virqs today). It sizes the statically-allocated irq_desc[] table in
 * core/irq.c. Changing it requires recompiling the core (ddm.library);
 * subsystems and drivers are unaffected since they obtain virqs at
 * runtime via IRQ_CreateMapping. */
#define NR_IRQS 64

#endif /* DDM_H_ */
