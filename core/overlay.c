#define DDM_INTERNAL
/*
 * Device Tree Overlay support
 *
 * Parses an overlay DTS file and grafts child nodes onto matching
 * Zorro boards by manufacturer/product ID. The overlay file contains
 * fragment@N nodes, each with:
 *
 *   - zorro-match = <manufacturer product>;  (two u32s)
 *   - child nodes to graft onto the matching board
 *
 * The reg property of direct children is adjusted by adding the
 * board's base address (from the board's reg property), converting
 * offsets to absolute addresses.
 *
 * Phandle references in the overlay (e.g. interrupt-parent = <&label>)
 * are resolved against both overlay-internal labels and main-tree
 * labels persisted on DDMBase.dt_labels.
 */
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/exec.h>

#include "ddm_debug.h"
#include "ddm_util.h"
#include "ddm.h"
#include "ddm_protos.h" /* DDM_RegisterDevice, DDM_UnregisterDevice */
#include "devicetree.h"
#include "parser_internal.h"

/* SysBase is defined in romtag.c */
extern struct ExecBase *SysBase;

/* ------------------------------------------------------------------ */
/* Copy map: maps temp-tree devices to grafted devices for phandle fixup */
/* ------------------------------------------------------------------ */

struct copy_map_entry
{
    struct device *old_dev;
    struct device *new_dev;
};

struct copy_map
{
    struct copy_map_entry *entries;
    int count;
    int cap;
};

static void copy_map_init(struct copy_map *m)
{
    m->entries = NULL;
    m->count = 0;
    m->cap = 0;
}

static void copy_map_add(struct copy_map *m, struct device *old_dev, struct device *new_dev)
{
    if (m->count >= m->cap)
    {
        int new_cap = m->cap ? m->cap * 2 : 16;
        struct copy_map_entry *new_entries =
            (struct copy_map_entry *)AllocMem(new_cap * sizeof(struct copy_map_entry), MEMF_ANY | MEMF_CLEAR);
        if (!new_entries)
            return;
        for (int i = 0; i < m->count; i++)
            new_entries[i] = m->entries[i];
        if (m->entries)
            FreeMem(m->entries, m->cap * sizeof(struct copy_map_entry));
        m->entries = new_entries;
        m->cap = new_cap;
    }
    m->entries[m->count].old_dev = old_dev;
    m->entries[m->count].new_dev = new_dev;
    m->count++;
}

static struct device *copy_map_lookup(struct copy_map *m, struct device *old_dev)
{
    for (int i = 0; i < m->count; i++)
    {
        if (m->entries[i].old_dev == old_dev)
            return m->entries[i].new_dev;
    }
    return NULL;
}

static void copy_map_free(struct copy_map *m)
{
    if (m->entries)
        FreeMem(m->entries, m->cap * sizeof(struct copy_map_entry));
    m->entries = NULL;
    m->count = 0;
    m->cap = 0;
}

/* ------------------------------------------------------------------ */
/* Free a subtree (never-registered devices from overlay parsing)     */
/* ------------------------------------------------------------------ */

static void ddm_free_subtree(struct DDMBase *ddm, struct device *dev)
{
    if (!dev)
        return;

    /* Free children first */
    struct device *child = dev->children;
    while (child)
    {
        struct device *next = child->next_sibling;
        ddm_free_subtree(ddm, child);
        child = next;
    }

    /* Free of_node (device tree node) */
    if (dev->of_node)
    {
        struct dt_property *prop = dev->of_node->properties;
        while (prop)
        {
            struct dt_property *next = prop->next;
            if (prop->name)
                FreeMem(prop->name, ddm_strlen(prop->name) + 1);
            if (prop->value)
                FreeMem(prop->value, prop->length);
            FreeMem(prop, sizeof(struct dt_property));
            prop = next;
        }
        if (dev->of_node->path)
            FreeMem(dev->of_node->path, ddm_strlen(dev->of_node->path) + 1);
        if (dev->of_node->name)
            FreeMem(dev->of_node->name, ddm_strlen(dev->of_node->name) + 1);
        FreeMem(dev->of_node, sizeof(struct device_node));
    }

    /* Free ln_Name (allocated by ddm_create_device) */
    if (dev->node.ln_Name)
        FreeMem(dev->node.ln_Name, ddm_strlen(dev->node.ln_Name) + 1);

    FreeMem(dev, sizeof(struct device));
}

/* ------------------------------------------------------------------ */
/* Deep-copy a subtree, adjusting reg for direct children             */
/* ------------------------------------------------------------------ */

/* Deep-copy src device and its subtree. new_parent is the parent for
 * the copy. board_base is added to the reg property of the top-level
 * device (direct children of a fragment). Deeper children keep their
 * reg as-is. The copy is NOT registered — caller handles that.
 * Maps old→new devices in copy_map for phandle fixup. */
static struct device *ddm_copy_subtree(struct device *src, struct device *new_parent,
                                      uint32_t board_base, int adjust_reg,
                                      struct copy_map *map)
{
    struct device *copy = ddm_create_device(src->node.ln_Name);
    if (!copy)
        return NULL;

    copy->parent = new_parent;
    copy->bus_type = src->bus_type;
    copy->flags = src->flags;

    /* Deep-copy properties */
    if (src->of_node)
    {
        struct dt_property *prop = src->of_node->properties;
        while (prop)
        {
            struct dt_property *new_prop = ddm_create_property(prop->name, prop->value, prop->length);
            if (new_prop)
            {
                /* Adjust reg property: add board_base to the u32 value */
                if (adjust_reg && ddm_strcmp(prop->name, "reg") == 0 && prop->length >= 4)
                {
                    uint32_t reg_val = ((uint32_t)prop->value[0] << 24) |
                                        ((uint32_t)prop->value[1] << 16) |
                                        ((uint32_t)prop->value[2] << 8) |
                                        ((uint32_t)prop->value[3]);
                    reg_val += board_base;
                    new_prop->value[0] = (uint8_t)(reg_val >> 24);
                    new_prop->value[1] = (uint8_t)(reg_val >> 16);
                    new_prop->value[2] = (uint8_t)(reg_val >> 8);
                    new_prop->value[3] = (uint8_t)(reg_val);
                }
                ddm_add_property(copy, new_prop);
            }
            prop = prop->next;
        }
    }

    /* Add to map for phandle fixup */
    copy_map_add(map, src, copy);

    /* Recursively copy children (reg NOT adjusted for deeper levels) */
    struct device *child = src->children;
    while (child)
    {
        struct device *child_copy = ddm_copy_subtree(child, copy, board_base, 0, map);
        if (child_copy)
        {
            child_copy->next_sibling = copy->children;
            copy->children = child_copy;
        }
        child = child->next_sibling;
    }

    return copy;
}

/* ------------------------------------------------------------------ */
/* Register a subtree depth-first (parent before children)            */
/* ------------------------------------------------------------------ */

static void register_subtree(struct DDMBase *ddm, struct device *dev)
{
    /* Register this device (parent already set, but DDM_RegisterDevice
     * handles linking into parent's children list and path building) */
    if (!DDM_RegisterDevice(ddm, dev))
    {
        DBG_DT("Overlay: ERR: failed to register '%s'\n", dev->node.ln_Name);
        return;
    }

    struct device *child = dev->children;
    while (child)
    {
        struct device *next = child->next_sibling;
        register_subtree(ddm, child);
        child = next;
    }
}

/* ------------------------------------------------------------------ */
/* Read a u32 from a property value at a given byte offset             */
/* ------------------------------------------------------------------ */

static uint32_t read_be32(const uint8_t *buf)
{
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) | ((uint32_t)buf[3]);
}

/* ------------------------------------------------------------------ */
/* Find a Zorro board by manufacturer/product ID                       */
/* ------------------------------------------------------------------ */

static struct device *find_zorro_board(struct DDMBase *ddm, uint32_t manuf, uint32_t product)
{
    struct device *dev = (struct device *)ddm->devices.lh_Head;
    while (dev->node.ln_Succ)
    {
        if (dev->bus_type == BUS_TYPE_ZORRO && dev->of_node)
        {
            struct dt_property *prop = dev->of_node->properties;
            uint32_t dev_manuf = 0xFFFFFFFF;
            uint32_t dev_product = 0xFFFFFFFF;
            while (prop)
            {
                if (ddm_strcmp(prop->name, "manufacturer-id") == 0 && prop->length >= 4)
                    dev_manuf = read_be32(prop->value);
                else if (ddm_strcmp(prop->name, "product-id") == 0 && prop->length >= 4)
                    dev_product = read_be32(prop->value);
                prop = prop->next;
            }
            if (dev_manuf == manuf && dev_product == product)
                return dev;
        }
        dev = (struct device *)dev->node.ln_Succ;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* LVO -318: DT_ApplyOverlay                                         */
/* ------------------------------------------------------------------ */

int32_t DT_ApplyOverlay(struct DDMBase *ddm __asm("a6"), const char *filename __asm("a0"))
{
    DBG_DT("Overlay: applying '%s'\n", filename);

    uint32_t file_len = 0;
    char *src = dt_read_file(filename, &file_len);
    if (!src)
    {
        DBG_DT("Overlay: no overlay file '%s' (non-fatal)\n", filename);
        return -1;
    }
    DBG_DT("Overlay: parsed %lu bytes\n", file_len);

    /* Create a temp root device (not registered — just a container) */
    struct device *temp_root = ddm_create_device("");
    if (!temp_root)
    {
        FreeMem(src, file_len + 1);
        return -1;
    }
    temp_root->node.ln_Name = ddm_strdup("/");
    temp_root->parent = NULL;

    /* Parse the overlay into temp_root with skip_register=TRUE */
    struct parser_state *ps = NULL;
    if (parse_tree_internal(ddm, src, file_len, temp_root, TRUE, &ps) < 0)
    {
        DBG_DT("Overlay: ERR: parse failed\n");
        ddm_free_subtree(ddm, temp_root);
        FreeMem(src, file_len + 1);
        return -1;
    }

    /* Walk temp_root's children (fragment@N nodes) */
    struct copy_map map;
    copy_map_init(&map);

    struct device *fragment = temp_root->children;
    while (fragment)
    {
        struct device *next_fragment = fragment->next_sibling;

        DBG_DT("Overlay: processing fragment '%s'\n", fragment->node.ln_Name);

        /* Read zorro-match property (2 u32s: manufacturer, product) */
        if (!fragment->of_node)
        {
            fragment = next_fragment;
            continue;
        }

        struct dt_property *match_prop = NULL;
        struct dt_property *prop = fragment->of_node->properties;
        while (prop)
        {
            if (ddm_strcmp(prop->name, "zorro-match") == 0)
            {
                match_prop = prop;
                break;
            }
            prop = prop->next;
        }

        if (!match_prop || match_prop->length < 8)
        {
            DBG_DT("Overlay: fragment '%s' has no zorro-match, skipping\n",
                   fragment->node.ln_Name);
            fragment = next_fragment;
            continue;
        }

        uint32_t manuf = read_be32(match_prop->value);
        uint32_t product = read_be32(match_prop->value + 4);
        DBG_DT("Overlay: fragment match manuf=%lx product=%lx\n",
               (unsigned long)manuf, (unsigned long)product);

        /* Find matching Zorro board(s) */
        struct device *board = find_zorro_board(ddm, manuf, product);
        if (!board)
        {
            DBG_DT("Overlay: no board matches manuf=%lx product=%lx\n",
                   (unsigned long)manuf, (unsigned long)product);
            fragment = next_fragment;
            continue;
        }

        /* Read board base address from reg property */
        uint32_t board_base = 0;
        prop = board->of_node->properties;
        while (prop)
        {
            if (ddm_strcmp(prop->name, "reg") == 0 && prop->length >= 4)
            {
                board_base = read_be32(prop->value);
                break;
            }
            prop = prop->next;
        }
        DBG_DT("Overlay: board '%s' base=%lx\n", board->node.ln_Name,
               (unsigned long)board_base);

        /* Copy fragment's children onto the board */
        struct device *frag_child = fragment->children;
        while (frag_child)
        {
            struct device *next_child = frag_child->next_sibling;

            struct device *grafted = ddm_copy_subtree(frag_child, board, board_base, 1, &map);
            if (grafted)
            {
                register_subtree(ddm, grafted);
                DBG_DT("Overlay: grafted '%s' onto '%s'\n",
                       grafted->node.ln_Name, board->node.ln_Name);
            }

            frag_child = next_child;
        }

        fragment = next_fragment;
    }

    /* Fix up phandle references: the pass-2 resolution in
     * parse_tree_internal stored device pointers (big-endian u32) in
     * the temp-tree properties. The deep-copy copied those values into
     * the grafted properties. We now walk all grafted devices and
     * replace any pointer that refers to a temp-tree device with the
     * corresponding grafted device (via copy_map). Pointers to
     * main-tree devices are left as-is. */
    for (int i = 0; i < map.count; i++)
    {
        struct device *grafted = map.entries[i].new_dev;
        if (!grafted->of_node)
            continue;

        struct dt_property *gprop = grafted->of_node->properties;
        while (gprop)
        {
            /* Check if this property could contain a phandle (4-byte
             * aligned values that look like device pointers). We check
             * every 4-byte chunk. */
            for (uint32_t off = 0; off + 4 <= gprop->length; off += 4)
            {
                uint32_t val = read_be32(gprop->value + off);
                struct device *target = (struct device *)(uintptr_t)val;
                if (target)
                {
                    /* Check if this is a temp-tree device */
                    struct device *grafted_target = copy_map_lookup(&map, target);
                    if (grafted_target)
                    {
                        /* Replace with grafted device pointer */
                        uint32_t new_val = (uint32_t)(uintptr_t)grafted_target;
                        gprop->value[off] = (uint8_t)(new_val >> 24);
                        gprop->value[off + 1] = (uint8_t)(new_val >> 16);
                        gprop->value[off + 2] = (uint8_t)(new_val >> 8);
                        gprop->value[off + 3] = (uint8_t)(new_val);
                    }
                }
            }
            gprop = gprop->next;
        }
    }

    /* Free temp tree (all devices, of_nodes, properties) */
    ddm_free_subtree(ddm, temp_root);

    /* Free parser state */
    dt_free_parser_state(ps);

    copy_map_free(&map);
    FreeMem(src, file_len + 1);

    DBG_DT("Overlay: done\n");
    return 0;
}
