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

/* String length. Returns 0 for NULL. */
uint32_t ddm_strlen(const char *s);

/* String compare. Returns the difference of the first differing
 * byte (like strcmp). NULL-safe: treats NULL as an empty string. */
int ddm_strcmp(const char *a, const char *b);

/* Duplicate a string using AllocMem(MEMF_ANY|MEMF_CLEAR).
 * Returns NULL if s is NULL or allocation fails.
 * Caller must FreeMem the result with ddm_strlen(result) + 1. */
char *ddm_strdup(const char *s);

#endif /* DDM_UTIL_H_ */
