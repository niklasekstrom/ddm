#define DDM_INTERNAL
/*
 * DDM framework - config file loader
 *
 * Loads a config file that lists driver libraries and devices to open.
 * Each opened library/device self-registers via DDM_RegisterDriver on
 * its init. After all are loaded, DDM_MatchAll is called to bind drivers
 * to devices.
 *
 * Config file format (one entry per line):
 *
 *   LIBS:spi.library
 *   DEVS:mmc-spi.device
 *   # Comments start with #
 */
#include <dos/dos.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <string.h> /* memset, strcmp, strcpy, strcat, strlen */

#include "ddm_debug.h"

#include "ddm.h"
#include "ddm_protos.h"
#include "ddm_util.h"

extern struct DDMBase *DDMBase;
extern struct ExecBase *SysBase;

/* Forward declaration */
int32_t DDM_LoadConfig(struct DDMBase *ddm __asm("a6"), const char *filename __asm("a0"));

/* ------------------------------------------------------------------ */
/* Line parsing                                                        */
/* ------------------------------------------------------------------ */

/* Skip leading whitespace */
static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

/* Check if line is blank or a comment */
static int16_t __attribute__((unused)) is_blank_or_comment(const char *line)
{
    const char *s = skip_ws(line);
    return (*s == '\0' || *s == '\n' || *s == '\r' || *s == '#');
}

/* Parse a config line. Returns the type ('L' for LIBS, 'D' for DEVS)
 * and copies the name into name_buf. Returns 0 on success, -1 on error
 * or blank line.
 */
static int parse_config_line(const char *line, char *type, char *name_buf, uint32_t buf_size)
{
    const char *s = skip_ws(line);
    if (*s == '\0' || *s == '\n' || *s == '\r' || *s == '#')
        return -1;

    /* Check for "LIBS:" or "DEVS:" prefix */
    if (s[0] == 'L' && s[1] == 'I' && s[2] == 'B' && s[3] == 'S' && s[4] == ':')
    {
        *type = 'L';
        s += 5;
    }
    else if (s[0] == 'D' && s[1] == 'E' && s[2] == 'V' && s[3] == 'S' && s[4] == ':')
    {
        *type = 'D';
        s += 5;
    }
    else
    {
        return -1;
    }

    /* Skip whitespace after prefix */
    s = skip_ws(s);

    /* Copy the name until end of line or whitespace */
    uint32_t i = 0;
    while (*s && *s != '\n' && *s != '\r' && *s != ' ' && *s != '\t' && i < buf_size - 1)
    {
        name_buf[i++] = *s++;
    }
    name_buf[i] = '\0';

    if (i == 0)
        return -1;

    return 0;
}

/* ------------------------------------------------------------------ */
/* File reading                                                        */
/* ------------------------------------------------------------------ */

/* Read a file line by line. Calls the callback for each non-blank line. */
static int32_t read_config_file(const char *filename, struct DDMBase *ddm)
{
    BPTR fh = Open((STRPTR)filename, MODE_OLDFILE);
    if (!fh)
        return -1;

    char line_buf[256];
    int32_t ret = 0;

    while (FGets(fh, line_buf, sizeof(line_buf)))
    {
        char type;
        char name_buf[128];

        if (parse_config_line(line_buf, &type, name_buf, sizeof(name_buf)) == 0)
        {
            if (type == 'L')
            {
                /* Open a library. It should self-register via
                 * DDM_RegisterDriver on its init. */
                DBG_CORE("DDM: config: opening library '%s'\n", name_buf);
                struct Library *lib = OpenLibrary(name_buf, 0);
                if (!lib)
                {
                    /* Failed to open library; continue with others */
                    DBG_CORE("DDM: ERR: failed to open library '%s'\n", name_buf);
                    continue;
                }
                /* Track the library so ddm_expunge can close it. */
                struct loaded_lib *entry =
                    (struct loaded_lib *)AllocMem(sizeof(struct loaded_lib), MEMF_ANY | MEMF_CLEAR);
                if (entry)
                {
                    entry->lib = lib;
                    entry->ior = NULL;
                    entry->is_device = FALSE;
                    AddTail(&ddm->loaded_libs, &entry->node);
                }
            }
            else if (type == 'D')
            {
                /* Skip the device that triggered the bootstrap. Its
                 * init_device is still on the call stack (it called
                 * DDM_RegisterDriver → DDM_LoadConfig → here), so the
                 * device isn't in Exec's device list yet. Calling
                 * OpenDevice would load a SECOND copy from disk with
                 * its own BSS. The second copy's units list is empty,
                 * and it gets added to the device list first, so fat95
                 * finds it instead of the first copy → "bad number".
                 *
                 * The bootstrap_trigger field records the driver name
                 * (e.g. "mmc-spi") that started the bootstrap. We map
                 * it to the device name by appending ".device". */
                if (ddm->bootstrap_trigger)
                {
                    /* The driver name (e.g. "mmc-spi") maps to the
                     * device name by appending ".device". */
                    char trigger_dev[128];
                    strcpy(trigger_dev, ddm->bootstrap_trigger);
                    strcat(trigger_dev, ".device");
                    if (strcmp(name_buf, trigger_dev) == 0)
                    {
                        DBG_CORE("DDM: config: skipping bootstrap trigger device '%s'\n", name_buf);
                        continue;
                    }
                }

                /* Open a device. Devices use OpenDevice with an IORequest.
                 * The device should self-register via DDM_RegisterDriver. */
                DBG_CORE("DDM: config: opening device '%s'\n", name_buf);
                struct IORequest *ior = (struct IORequest *)AllocMem(sizeof(struct IORequest), MEMF_ANY | MEMF_CLEAR);
                if (!ior)
                    continue;
                ior->io_Message.mn_Length = sizeof(struct IORequest);
                if (OpenDevice(name_buf, 0, ior, 0) == 0)
                {
                    /* Track the device so ddm_expunge can close it. */
                    struct loaded_lib *entry =
                        (struct loaded_lib *)AllocMem(sizeof(struct loaded_lib), MEMF_ANY | MEMF_CLEAR);
                    if (entry)
                    {
                        entry->lib = NULL;
                        entry->ior = ior;
                        entry->is_device = TRUE;
                        AddTail(&ddm->loaded_libs, &entry->node);
                    }
                }
                else
                {
                    DBG_CORE("DDM: ERR: failed to open device '%s'\n", name_buf);
                    FreeMem(ior, sizeof(struct IORequest));
                }
            }
        }
    }

    Close(fh);
    return ret;
}

/* ------------------------------------------------------------------ */
/* LVO -114: DDM_LoadConfig                                           */
/* ------------------------------------------------------------------ */

int32_t DDM_LoadConfig(struct DDMBase *ddm __asm("a6"), const char *filename __asm("a0"))
{
    DBG_CORE("DDM: LoadConfig '%s'\n", filename);
    int32_t ret = read_config_file(filename, ddm);
    if (ret < 0)
    {
        DBG_CORE("DDM: ERR: LoadConfig failed to read '%s'\n", filename);
        return ret;
    }

    /* Now that all drivers are loaded and registered, match them
     * to devices in the tree. */
    DDM_MatchAll(ddm);

    return 0;
}
