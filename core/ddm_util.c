/*
 * Shared string utilities - part of ddm.library
 *
 * Implementation of the ddm_strlen / ddm_strcmp / ddm_strdup helpers
 * declared in include/ddm_util.h. These replace the per-file static
 * copies that were duplicated across the core and subsystems.
 */
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/exec.h>

#include "ddm_debug.h"

#include "ddm_util.h"

/* SysBase is defined in romtag.c; declared here so exec functions work */
extern struct ExecBase *SysBase;

/* ------------------------------------------------------------------ */
/* String helpers                                                     */
/* ------------------------------------------------------------------ */

uint32_t ddm_strlen(const char *s)
{
    uint32_t len = 0;
    if (s)
        while (s[len])
            len++;
    return len;
}

int ddm_strcmp(const char *a, const char *b)
{
    if (!a)
        a = "";
    if (!b)
        b = "";
    while (*a && *a == *b)
    {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

char *ddm_strdup(const char *s)
{
    if (!s)
        return NULL;
    uint32_t len = ddm_strlen(s);
    char *dup = (char *)AllocMem(len + 1, MEMF_ANY | MEMF_CLEAR);
    if (dup)
    {
        for (uint32_t i = 0; i <= len; i++)
            dup[i] = s[i];
    }
    return dup;
}
