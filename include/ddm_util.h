/*
 * Shared string utilities for the DDM framework
 *
 * These replace the per-file static copies (dt_strlen, p_strlen,
 * s_strlen, c_strlen, etc.) that were duplicated across the core
 * and subsystems. Implemented in core/ddm_util.c, part of ddm.library.
 */
#ifndef DDM_UTIL_H_
#define DDM_UTIL_H_

#include <exec/types.h>

/* Forward declarations to avoid pulling in the full ddm.h header
 * in every consumer. The structs are defined in ddm.h. */
struct device;
struct dt_property;

/* String length. Returns 0 for NULL. */
uint32_t ddm_strlen(const char *s);

/* String compare. Returns the difference of the first differing
 * byte (like strcmp). NULL-safe: treats NULL as an empty string. */
int ddm_strcmp(const char *a, const char *b);

/* Duplicate a string using AllocMem(MEMF_ANY|MEMF_CLEAR).
 * Returns NULL if s is NULL or allocation fails.
 * Caller must FreeMem the result with ddm_strlen(result) + 1. */
char *ddm_strdup(const char *s);

/* Create a new device with an attached (empty) device_node.
 * ln_Name and of_node->name are ddm_strdup'd from 'name'.
 * Returns NULL on allocation failure. */
struct device *ddm_create_device(const char *name);

/* Create a new dt_property with a ddm_strdup'd name and a copy of
 * 'value' (length bytes). For length == 0 the value pointer is NULL.
 * Returns NULL on allocation failure. */
struct dt_property *ddm_create_property(const char *name, uint8_t *value, uint32_t length);

/* Append a property to a device's of_node property list. */
void ddm_add_property(struct device *dev, struct dt_property *prop);

#endif /* DDM_UTIL_H_ */
