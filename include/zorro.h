/*
 * Zorro expansion bus enumeration
 *
 * Declares the internal zorro_enumerate() function that walks
 * expansion.library's ConfigDev list and creates DDM device nodes
 * for each installed Zorro board. This is called during the DDM
 * bootstrap sequence (after the main device tree is parsed, before
 * overlays are applied and config is loaded).
 *
 * This is NOT an LVO — it is a direct call within ddm.library.
 */
#ifndef ZORRO_H_
#define ZORRO_H_

#include "ddm.h" /* struct DDMBase */

/* Enumerate all Zorro boards via expansion.library and create a DDM
 * device node for each. Creates a "zorro-bus" parent device (child of
 * the root) and one child per board with bus_type = BUS_TYPE_ZORRO.
 * Each board device has properties: manufacturer-id, product-id,
 * serial-number, reg (absolute base address), board-size, board-type
 * ("zorro2"/"zorro3"), compatible ("zorro").
 *
 * If expansion.library cannot be opened, the function is a no-op. */
void zorro_enumerate(struct DDMBase *ddm);

#endif /* ZORRO_H_ */
