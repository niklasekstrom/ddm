/*
 * Internal parser interface — shared between parser.c and overlay.c
 *
 * This header is NOT part of the public API. It declares internal
 * functions that are shared between the main DTS parser (parser.c)
 * and the overlay applier (overlay.c).
 *
 * The parser_state struct is defined in parser.c and is opaque to
 * overlay.c. parse_tree_internal allocates it on the heap and returns
 * a pointer; dt_free_parser_state frees it.
 *
 * Only files within ddm.library that define DDM_INTERNAL should include
 * this header.
 */
#ifndef PARSER_INTERNAL_H_
#define PARSER_INTERNAL_H_

#include "ddm.h" /* struct device, struct DDMBase */
#include <exec/types.h>

/* Opaque parser state — defined in parser.c */
struct parser_state;

/* Shared file reader (used by DT_ParseTree and DT_ApplyOverlay) */
char *dt_read_file(const char *filename, uint32_t *out_len);

/* Internal parsing core. Parses src into root's children. When
 * skip_register is TRUE, child devices are linked manually (not
 * registered via DDM_RegisterDevice) — used by overlay parsing.
 *
 * Allocates a parser_state on the heap and returns it via *out_ps
 * (caller must free it via dt_free_parser_state). Returns 0 on
 * success, -1 on failure (out_ps is set to NULL on failure). */
int32_t parse_tree_internal(struct DDMBase *ddm, const char *src, uint32_t file_len,
                            struct device *root, int skip_register,
                            struct parser_state **out_ps);

/* Free a parser_state returned by parse_tree_internal, including all
 * internal resources (cur_tok, labels, phandle_refs). Does NOT free
 * the source buffer or the parsed device tree. NULL-safe. */
void dt_free_parser_state(struct parser_state *ps);

#endif /* PARSER_INTERNAL_H_ */

