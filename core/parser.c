#define DDM_INTERNAL
/*
 * Device Tree framework - DTS-like text parser
 *
 * Parses a text file describing the device tree into struct device nodes.
 *
 * The format is a simplified version of the Linux Device Tree Source (DTS):
 *
 *   / {
 *       node-name@address {
 *           compatible = "string1", "string2";
 *           reg = <0x1234>;
 *           property = "value";
 *           #address-cells = <1>;
 *
 *           child-node@0 {
 *               compatible = "child";
 *               reg = <0>;
 *           };
 *       };
 *   };
 *
 * Supported property value types:
 *   - Strings: "hello", "world"
 *   - Integers: <0x1234>, <42>, <0>
 *   - Multiple strings: "a", "b", "c"  (concatenated with NUL separators)
 *   - Multiple integers: <0x01 0x02 0x03>  (concatenated as u32 array)
 *   - Phandle references: <&label>  (resolved in a second pass)
 *   - Labels: label: node-name { ... }  (for phandle references)
 */
#include <dos/dos.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include "ddm_debug.h"

#include "ddm.h"
#include "ddm_protos.h"
#include "ddm_util.h"
#include "devicetree.h"
#include "parser_internal.h"

extern struct ExecBase *SysBase;

/* ------------------------------------------------------------------ */
/* Forward declarations                                               */
/* ------------------------------------------------------------------ */

int32_t DT_ParseTree(struct DDMBase *ddm __asm("a6"), const char *filename __asm("a0"));

/* ------------------------------------------------------------------ */
/* Token types                                                        */
/* ------------------------------------------------------------------ */

enum token_type
{
    TOK_EOF,
    TOK_LBRACE,      /* { */
    TOK_RBRACE,      /* } */
    TOK_SEMICOLON,   /* ; */
    TOK_EQUALS,      /* = */
    TOK_COMMA,       /* , */
    TOK_COLON,       /* : */
    TOK_ANGLE_OPEN,  /* < */
    TOK_ANGLE_CLOSE, /* > */
    TOK_STRING,      /* "..." */
    TOK_IDENT,       /* identifier or node name */
    TOK_PHANDLE,     /* &label */
    TOK_SLASH,       /* / (root marker) */
};

struct token
{
    enum token_type type;
    char *text; /* For TOK_STRING, TOK_IDENT, TOK_PHANDLE: the text content */
    uint32_t line;
};

/* ------------------------------------------------------------------ */
/* Parser state                                                       */
/* ------------------------------------------------------------------ */

#define INITIAL_LABELS 16
#define INITIAL_PHANDLE_REFS 16

struct label_entry
{
    char *label;
    struct device *dev;
};

/* Tracks a phandle reference within a property value, to be resolved
 * in pass 2 after the full tree is built. */
struct phandle_ref
{
    struct device *dev;       /* Device that owns the property */
    struct dt_property *prop; /* Property containing the phandle */
    uint32_t offset;          /* Byte offset within prop->value */
    char *label;              /* Label name to resolve */
};

struct parser_state
{
    const char *src;      /* Source text */
    uint32_t pos;         /* Current position */
    uint32_t line;        /* Current line number */
    struct token cur_tok; /* Current token */
    struct DDMBase *ddm;
    struct label_entry *labels;
    int num_labels;
    int cap_labels;
    struct phandle_ref *phandle_refs;
    int num_phandle_refs;
    int cap_phandle_refs;
    int skip_register; /* TRUE for overlay parsing: link children
                        * manually instead of calling DDM_RegisterDevice */
};

static int grow_phandle_refs(struct parser_state *ps);

/* ------------------------------------------------------------------ */
/* Tokenizer                                                          */
/* ------------------------------------------------------------------ */

static void skip_whitespace_and_comments(struct parser_state *ps)
{
    while (ps->src[ps->pos])
    {
        char c = ps->src[ps->pos];
        if (c == ' ' || c == '\t' || c == '\r')
        {
            ps->pos++;
        }
        else if (c == '\n')
        {
            ps->pos++;
            ps->line++;
        }
        else if (c == '/' && ps->src[ps->pos + 1] == '/')
        {
            /* Line comment */
            ps->pos += 2;
            while (ps->src[ps->pos] && ps->src[ps->pos] != '\n')
                ps->pos++;
        }
        else if (c == '/' && ps->src[ps->pos + 1] == '*')
        {
            /* Block comment */
            ps->pos += 2;
            while (ps->src[ps->pos])
            {
                if (ps->src[ps->pos] == '*' && ps->src[ps->pos + 1] == '/')
                {
                    ps->pos += 2;
                    break;
                }
                if (ps->src[ps->pos] == '\n')
                    ps->line++;
                ps->pos++;
            }
        }
        else
        {
            break;
        }
    }
}

static int is_ident_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
           c == '.' || c == ',' || c == '@' || c == '#' || c == '+' || c == '?';
}

static int is_ident_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '#' || c == '\\';
}

/* Read the next token. Returns 0 on success, -1 on EOF/error. */
static int next_token(struct parser_state *ps)
{
    skip_whitespace_and_comments(ps);

    /* Free previous token text */
    if (ps->cur_tok.text)
    {
        FreeMem(ps->cur_tok.text, ddm_strlen(ps->cur_tok.text) + 1);
        ps->cur_tok.text = NULL;
    }

    ps->cur_tok.line = ps->line;

    char c = ps->src[ps->pos];
    if (c == '\0')
    {
        ps->cur_tok.type = TOK_EOF;
        return 0;
    }

    switch (c)
    {
    case '{':
        ps->pos++;
        ps->cur_tok.type = TOK_LBRACE;
        return 0;
    case '}':
        ps->pos++;
        ps->cur_tok.type = TOK_RBRACE;
        return 0;
    case ';':
        ps->pos++;
        ps->cur_tok.type = TOK_SEMICOLON;
        return 0;
    case '=':
        ps->pos++;
        ps->cur_tok.type = TOK_EQUALS;
        return 0;
    case ',':
        ps->pos++;
        ps->cur_tok.type = TOK_COMMA;
        return 0;
    case ':':
        ps->pos++;
        ps->cur_tok.type = TOK_COLON;
        return 0;
    case '<':
        ps->pos++;
        ps->cur_tok.type = TOK_ANGLE_OPEN;
        return 0;
    case '>':
        ps->pos++;
        ps->cur_tok.type = TOK_ANGLE_CLOSE;
        return 0;
    case '/':
        ps->pos++;
        ps->cur_tok.type = TOK_SLASH;
        return 0;
    case '&': {
        ps->pos++;
        uint32_t start = ps->pos;
        while (is_ident_char(ps->src[ps->pos]))
            ps->pos++;
        uint32_t len = ps->pos - start;
        if (len == 0)
            return -1;
        ps->cur_tok.type = TOK_PHANDLE;
        ps->cur_tok.text = (char *)AllocMem(len + 1, MEMF_ANY | MEMF_CLEAR);
        for (uint32_t i = 0; i < len; i++)
            ps->cur_tok.text[i] = ps->src[start + i];
        ps->cur_tok.text[len] = '\0';
        return 0;
    }
    case '"': {
        ps->pos++; /* skip opening quote */
        uint32_t start = ps->pos;
        while (ps->src[ps->pos] && ps->src[ps->pos] != '"')
        {
            if (ps->src[ps->pos] == '\n')
                ps->line++;
            ps->pos++;
        }
        uint32_t len = ps->pos - start;
        if (ps->src[ps->pos] != '"')
            return -1; /* unterminated string */
        ps->pos++;     /* skip closing quote */
        ps->cur_tok.type = TOK_STRING;
        ps->cur_tok.text = (char *)AllocMem(len + 1, MEMF_ANY | MEMF_CLEAR);
        for (uint32_t i = 0; i < len; i++)
            ps->cur_tok.text[i] = ps->src[start + i];
        ps->cur_tok.text[len] = '\0';
        return 0;
    }
    default:
        break;
    }

    /* Identifier or number */
    if (is_ident_start(c) || (c >= '0' && c <= '9'))
    {
        uint32_t start = ps->pos;
        while (is_ident_char(ps->src[ps->pos]))
            ps->pos++;
        uint32_t len = ps->pos - start;
        ps->cur_tok.type = TOK_IDENT;
        ps->cur_tok.text = (char *)AllocMem(len + 1, MEMF_ANY | MEMF_CLEAR);
        for (uint32_t i = 0; i < len; i++)
            ps->cur_tok.text[i] = ps->src[start + i];
        ps->cur_tok.text[len] = '\0';
        return 0;
    }

    return -1; /* unknown character */
}

/* Peek at the current token without consuming it */
static struct token *peek_token(struct parser_state *ps)
{
    return &ps->cur_tok;
}

/* Consume the current token and read the next */
static int consume_token(struct parser_state *ps)
{
    return next_token(ps);
}

/* ------------------------------------------------------------------ */
/* Property value parsing                                             */
/* ------------------------------------------------------------------ */

/* Parse a property value. The value can be a string, a <u32> cell,
 * or a list of these separated by commas. The result is assembled
 * into a byte array.
 *
 * Returns: allocated buffer in *out_buf, length in *out_len.
 * Returns 0 on success, -1 on error.
 */
static int parse_property_value(struct parser_state *ps, uint8_t **out_buf, uint32_t *out_len, uint32_t *out_size)
{
    /* We build the value incrementally. Start with a small buffer. */
    uint32_t buf_size = 64;
    uint32_t buf_len = 0;
    uint8_t *buf = (uint8_t *)AllocMem(buf_size, MEMF_ANY | MEMF_CLEAR);
    if (!buf)
        return -1;

    int16_t first = TRUE;

    while (1)
    {
        struct token *tok = peek_token(ps);

        if (tok->type == TOK_SEMICOLON)
        {
            break;
        }

        if (!first && tok->type == TOK_COMMA)
        {
            consume_token(ps);
            tok = peek_token(ps);
        }
        first = FALSE;

        if (tok->type == TOK_STRING)
        {
            /* String value: copy including NUL terminator */
            uint32_t slen = ddm_strlen(tok->text) + 1;
            if (buf_len + slen > buf_size)
            {
                /* Grow buffer */
                uint32_t new_size = buf_size * 2;
                while (buf_len + slen > new_size)
                    new_size *= 2;
                uint8_t *new_buf = (uint8_t *)AllocMem(new_size, MEMF_ANY | MEMF_CLEAR);
                if (!new_buf)
                    goto fail;
                for (uint32_t i = 0; i < buf_len; i++)
                    new_buf[i] = buf[i];
                FreeMem(buf, buf_size);
                buf = new_buf;
                buf_size = new_size;
            }
            for (uint32_t i = 0; i < slen; i++)
                buf[buf_len + i] = (uint8_t)tok->text[i];
            buf_len += slen;
            consume_token(ps);
        }
        else if (tok->type == TOK_ANGLE_OPEN)
        {
            consume_token(ps); /* consume '<' */

            /* Parse one or more u32 values until '>' */
            while (1)
            {
                tok = peek_token(ps);
                if (tok->type == TOK_ANGLE_CLOSE)
                {
                    consume_token(ps);
                    break;
                }
                if (tok->type == TOK_PHANDLE)
                {
                    /* Phandle reference: store as a placeholder u32 (0).
                     * Record the label and offset for pass-2 resolution. */
                    if (buf_len + 4 > buf_size)
                    {
                        uint32_t new_size = buf_size * 2;
                        while (buf_len + 4 > new_size)
                            new_size *= 2;
                        uint8_t *new_buf = (uint8_t *)AllocMem(new_size, MEMF_ANY | MEMF_CLEAR);
                        if (!new_buf)
                            goto fail;
                        for (uint32_t i = 0; i < buf_len; i++)
                            new_buf[i] = buf[i];
                        FreeMem(buf, buf_size);
                        buf = new_buf;
                        buf_size = new_size;
                    }
                    /* Placeholder: 0, will be resolved in pass 2.
                     * Store the label name in the parser state for
                     * later resolution. We store the offset within
                     * the value buffer; the property pointer is filled
                     * in after create_property. */
                    if (ps->num_phandle_refs >= ps->cap_phandle_refs)
                    {
                        if (grow_phandle_refs(ps) < 0)
                            goto fail;
                    }
                    ps->phandle_refs[ps->num_phandle_refs].offset = buf_len;
                    ps->phandle_refs[ps->num_phandle_refs].label = ddm_strdup(tok->text);
                    ps->phandle_refs[ps->num_phandle_refs].prop = NULL;
                    ps->num_phandle_refs++;
                    buf[buf_len] = 0;
                    buf[buf_len + 1] = 0;
                    buf[buf_len + 2] = 0;
                    buf[buf_len + 3] = 0;
                    buf_len += 4;
                    consume_token(ps);
                }
                else if (tok->type == TOK_IDENT)
                {
                    /* Parse integer (hex or decimal) */
                    uint32_t val = 0;
                    const char *s = tok->text;
                    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
                    {
                        s += 2;
                        while (*s)
                        {
                            char c = *s;
                            if (c >= '0' && c <= '9')
                                val = val * 16 + (c - '0');
                            else if (c >= 'a' && c <= 'f')
                                val = val * 16 + (c - 'a' + 10);
                            else if (c >= 'A' && c <= 'F')
                                val = val * 16 + (c - 'A' + 10);
                            s++;
                        }
                    }
                    else
                    {
                        while (*s >= '0' && *s <= '9')
                        {
                            val = val * 10 + (*s - '0');
                            s++;
                        }
                    }

                    if (buf_len + 4 > buf_size)
                    {
                        uint32_t new_size = buf_size * 2;
                        while (buf_len + 4 > new_size)
                            new_size *= 2;
                        uint8_t *new_buf = (uint8_t *)AllocMem(new_size, MEMF_ANY | MEMF_CLEAR);
                        if (!new_buf)
                            goto fail;
                        for (uint32_t i = 0; i < buf_len; i++)
                            new_buf[i] = buf[i];
                        FreeMem(buf, buf_size);
                        buf = new_buf;
                        buf_size = new_size;
                    }
                    /* Store as big-endian u32 */
                    buf[buf_len] = (uint8_t)(val >> 24);
                    buf[buf_len + 1] = (uint8_t)(val >> 16);
                    buf[buf_len + 2] = (uint8_t)(val >> 8);
                    buf[buf_len + 3] = (uint8_t)(val);
                    buf_len += 4;
                    consume_token(ps);
                }
                else
                {
                    /* Unexpected token */
                    goto fail;
                }
            }
        }
        else
        {
            /* Unexpected token */
            break;
        }
    }

    *out_buf = buf;
    *out_len = buf_len;
    *out_size = buf_size;
    return 0;

fail:
    FreeMem(buf, buf_size);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Property creation                                                  */
/* ------------------------------------------------------------------ */
/* The create_property / create_device / add_property helpers have been
 * moved to ddm_util.c as ddm_create_property / ddm_create_device /
 * ddm_add_property so that overlay.c can reuse them. */

static void __attribute__((unused)) free_property(struct dt_property *prop)
{
    if (!prop)
        return;
    if (prop->name)
        FreeMem(prop->name, ddm_strlen(prop->name) + 1);
    if (prop->value)
        FreeMem(prop->value, prop->length);
    FreeMem(prop, sizeof(struct dt_property));
}

/* ------------------------------------------------------------------ */
/* Device creation                                                    */
/* ------------------------------------------------------------------ */
/* create_device and add_property are now provided by ddm_util.c as
 * ddm_create_device / ddm_add_property. */

/* ------------------------------------------------------------------ */
/* Node parsing                                                       */
/* ------------------------------------------------------------------ */

/* Grow the labels array when full. Returns 0 on success, -1 on
 * allocation failure. */
static int grow_labels(struct parser_state *ps)
{
    int new_cap = ps->cap_labels * 2;
    struct label_entry *new_arr =
        (struct label_entry *)AllocMem(new_cap * sizeof(struct label_entry), MEMF_ANY | MEMF_CLEAR);
    if (!new_arr)
        return -1;
    for (int i = 0; i < ps->num_labels; i++)
        new_arr[i] = ps->labels[i];
    FreeMem(ps->labels, ps->cap_labels * sizeof(struct label_entry));
    ps->labels = new_arr;
    ps->cap_labels = new_cap;
    return 0;
}

/* Grow the phandle_refs array when full. Returns 0 on success, -1 on
 * allocation failure. */
static int grow_phandle_refs(struct parser_state *ps)
{
    int new_cap = ps->cap_phandle_refs * 2;
    struct phandle_ref *new_arr =
        (struct phandle_ref *)AllocMem(new_cap * sizeof(struct phandle_ref), MEMF_ANY | MEMF_CLEAR);
    if (!new_arr)
        return -1;
    for (int i = 0; i < ps->num_phandle_refs; i++)
        new_arr[i] = ps->phandle_refs[i];
    FreeMem(ps->phandle_refs, ps->cap_phandle_refs * sizeof(struct phandle_ref));
    ps->phandle_refs = new_arr;
    ps->cap_phandle_refs = new_cap;
    return 0;
}

/* Register a label for later phandle resolution */
static int register_label(struct parser_state *ps, const char *label, struct device *dev)
{
    if (ps->num_labels >= ps->cap_labels)
    {
        if (grow_labels(ps) < 0)
            return -1;
    }
    ps->labels[ps->num_labels].label = ddm_strdup(label);
    ps->labels[ps->num_labels].dev = dev;
    ps->num_labels++;
    return 0;
}

/* Find a device by label (for phandle resolution) */
static struct device *find_by_label(struct parser_state *ps, const char *label)
{
    /* First check the parser's local labels (current file's labels) */
    for (int i = 0; i < ps->num_labels; i++)
    {
        if (ddm_strcmp(ps->labels[i].label, label) == 0)
            return ps->labels[i].dev;
    }
    /* Fall back to persisted main-tree labels on DDMBase (allows
     * overlay files to cross-reference devices from the main DTS). */
    if (ps->ddm && ps->ddm->dt_labels.lh_Head)
    {
        struct Node *n = ps->ddm->dt_labels.lh_Head;
        while (n && n != (struct Node *)ps->ddm->dt_labels.lh_TailPred)
        {
            if (ddm_strcmp(n->ln_Name, label) == 0)
                return ((struct dt_label *)n)->dev;
            n = n->ln_Succ;
        }
    }
    return NULL;
}

/* Parse a node body (the content between { and }).
 * parent is the parent device (NULL for root).
 * Returns 0 on success, -1 on error.
 */
static int parse_node_body(struct parser_state *ps, struct device *parent)
{
    while (1)
    {
        struct token *tok = peek_token(ps);

        if (tok->type == TOK_RBRACE)
        {
            consume_token(ps); /* consume '}' */
            return 0;
        }

        if (tok->type == TOK_EOF)
        {
            return -1; /* unexpected EOF */
        }

        /* Expect an identifier (property name, node name, or label) */
        if (tok->type != TOK_IDENT)
        {
            /* Unexpected token, skip it */
            consume_token(ps);
            continue;
        }

        char *name = ddm_strdup(tok->text);
        consume_token(ps);
        tok = peek_token(ps);

        /* Check for label: "label: node-name { ... }" */
        char *label = NULL;
        if (tok->type == TOK_COLON)
        {
            /* This is a label. The next token should be the node name. */
            consume_token(ps); /* consume ':' */
            label = name;
            tok = peek_token(ps);
            if (tok->type != TOK_IDENT)
            {
                FreeMem(label, ddm_strlen(label) + 1);
                return -1;
            }
            name = ddm_strdup(tok->text);
            consume_token(ps);
            tok = peek_token(ps);
        }

        if (tok->type == TOK_LBRACE)
        {
            /* It's a child node */
            consume_token(ps); /* consume '{' */

            struct device *child = ddm_create_device(name);
            if (!child)
            {
                if (label)
                    FreeMem(label, ddm_strlen(label) + 1);
                FreeMem(name, ddm_strlen(name) + 1);
                return -1;
            }
            child->parent = parent;

            if (label)
            {
                register_label(ps, label, child);
                FreeMem(label, ddm_strlen(label) + 1);
            }

            /* Register the device in the DDM, or link manually when
             * skip_register is set (overlay parsing — the caller
             * controls registration). */
            if (ps->skip_register)
            {
                child->next_sibling = parent->children;
                parent->children = child;
            }
            else
            {
                if (!DDM_RegisterDevice(ps->ddm, child))
                {
                    DDM_UnregisterDevice(ps->ddm, child);
                    FreeMem(name, ddm_strlen(name) + 1);
                    return -1;
                }
            }

            FreeMem(name, ddm_strlen(name) + 1);

            /* Parse the child's body recursively */
            if (parse_node_body(ps, child) < 0)
                return -1;

            /* Optional semicolon after node */
            tok = peek_token(ps);
            if (tok->type == TOK_SEMICOLON)
                consume_token(ps);
        }
        else if (tok->type == TOK_EQUALS)
        {
            /* It's a property */
            consume_token(ps); /* consume '=' */

            /* Record how many phandle refs exist before parsing the
             * value, so we can associate new ones with this property. */
            int refs_before = ps->num_phandle_refs;

            uint8_t *value_buf = NULL;
            uint32_t value_len = 0;
            uint32_t value_size = 0;
            if (parse_property_value(ps, &value_buf, &value_len, &value_size) < 0)
            {
                FreeMem(name, ddm_strlen(name) + 1);
                if (label)
                    FreeMem(label, ddm_strlen(label) + 1);
                return -1;
            }

            /* Expect semicolon */
            tok = peek_token(ps);
            if (tok->type != TOK_SEMICOLON)
            {
                FreeMem(value_buf, value_size);
                FreeMem(name, ddm_strlen(name) + 1);
                if (label)
                    FreeMem(label, ddm_strlen(label) + 1);
                return -1;
            }
            consume_token(ps);

            /* Create and add the property (ddm_create_property copies the data) */
            struct dt_property *prop = ddm_create_property(name, value_buf, value_len);
            if (prop)
                ddm_add_property(parent, prop);

            /* Associate any new phandle refs with this device+property */
            for (int i = refs_before; i < ps->num_phandle_refs; i++)
            {
                ps->phandle_refs[i].dev = parent;
                ps->phandle_refs[i].prop = prop;
            }

            FreeMem(value_buf, value_size);
            FreeMem(name, ddm_strlen(name) + 1);
            if (label)
                FreeMem(label, ddm_strlen(label) + 1);
        }
        else if (tok->type == TOK_SEMICOLON)
        {
            /* Boolean property (no value), e.g. "interrupt-controller;" */
            consume_token(ps);
            struct dt_property *prop = ddm_create_property(name, NULL, 0);
            if (prop)
                ddm_add_property(parent, prop);
            FreeMem(name, ddm_strlen(name) + 1);
            if (label)
                FreeMem(label, ddm_strlen(label) + 1);
        }
        else
        {
            /* Unexpected token after ident */
            FreeMem(name, ddm_strlen(name) + 1);
            if (label)
                FreeMem(label, ddm_strlen(label) + 1);
            return -1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* File reading                                                        */
/* ------------------------------------------------------------------ */

/* Read a file into memory. Returns allocated buffer and its length.
 * Returns NULL on failure. Non-static so overlay.c can reuse it.
 */
char *dt_read_file(const char *filename, uint32_t *out_len)
{
    BPTR fh = Open((STRPTR)filename, MODE_OLDFILE);
    if (!fh)
        return NULL;

    /* Read in chunks */
    uint32_t buf_size = 4096;
    uint32_t buf_len = 0;
    char *buf = (char *)AllocMem(buf_size, MEMF_ANY);
    if (!buf)
    {
        Close(fh);
        return NULL;
    }

    while (1)
    {
        if (buf_len + 1024 > buf_size)
        {
            uint32_t new_size = buf_size * 2;
            char *new_buf = (char *)AllocMem(new_size, MEMF_ANY);
            if (!new_buf)
            {
                FreeMem(buf, buf_size);
                Close(fh);
                return NULL;
            }
            for (uint32_t i = 0; i < buf_len; i++)
                new_buf[i] = buf[i];
            FreeMem(buf, buf_size);
            buf = new_buf;
            buf_size = new_size;
        }

        int32_t n = Read(fh, buf + buf_len, 1024);
        if (n <= 0)
            break;
        buf_len += n;
    }

    Close(fh);

    /* NUL-terminate */
    if (buf_len + 1 > buf_size)
    {
        char *new_buf = (char *)AllocMem(buf_size + 1, MEMF_ANY);
        if (!new_buf)
        {
            FreeMem(buf, buf_size);
            return NULL;
        }
        for (uint32_t i = 0; i < buf_len; i++)
            new_buf[i] = buf[i];
        FreeMem(buf, buf_size);
        buf = new_buf;
        buf_size += 1;
    }
    buf[buf_len] = '\0';

    /* Shrink the buffer to the exact size needed (file_len + 1) so that
     * the caller's FreeMem(src, file_len + 1) passes the correct size.
     * Without this, the buffer may be larger than file_len + 1, causing
     * a FreeMem size mismatch. */
    if (buf_size > buf_len + 1)
    {
        uint32_t exact = buf_len + 1;
        char *shrunk = (char *)AllocMem(exact, MEMF_ANY);
        if (shrunk)
        {
            for (uint32_t i = 0; i < exact; i++)
                shrunk[i] = buf[i];
            FreeMem(buf, buf_size);
            buf = shrunk;
            buf_size = exact;
        }
    }

    *out_len = buf_len;
    return buf;
}

/* ------------------------------------------------------------------ */
/* Internal parsing core (shared by DT_ParseTree and overlay parser) */
/* ------------------------------------------------------------------ */

int32_t parse_tree_internal(struct DDMBase *ddm, const char *src, uint32_t file_len,
                            struct device *root, int skip_register,
                            struct parser_state **out_ps)
{
    (void)file_len; /* src is already NUL-terminated; length not needed here */

    struct parser_state *ps = (struct parser_state *)AllocMem(sizeof(struct parser_state), MEMF_ANY | MEMF_CLEAR);
    if (!ps)
    {
        *out_ps = NULL;
        return -1;
    }

    ps->src = src;
    ps->pos = 0;
    ps->line = 1;
    ps->cur_tok.text = NULL;
    ps->cur_tok.type = TOK_EOF;
    ps->ddm = ddm;
    ps->num_labels = 0;
    ps->cap_labels = INITIAL_LABELS;
    ps->labels = (struct label_entry *)AllocMem(ps->cap_labels * sizeof(struct label_entry), MEMF_ANY | MEMF_CLEAR);
    ps->num_phandle_refs = 0;
    ps->cap_phandle_refs = INITIAL_PHANDLE_REFS;
    ps->phandle_refs =
        (struct phandle_ref *)AllocMem(ps->cap_phandle_refs * sizeof(struct phandle_ref), MEMF_ANY | MEMF_CLEAR);
    ps->skip_register = skip_register;
    if (!ps->labels || !ps->phandle_refs)
        goto fail;

    /* Read first token */
    if (next_token(ps) < 0)
        goto fail;

    /* Expect '/' for root */
    struct token *tok = peek_token(ps);
    if (tok->type != TOK_SLASH)
        goto fail;
    consume_token(ps);

    /* Expect '{' */
    tok = peek_token(ps);
    if (tok->type != TOK_LBRACE)
        goto fail;
    consume_token(ps);

    /* Parse root body (root device is created by the caller) */
    if (parse_node_body(ps, root) < 0)
        goto fail;

    /* Optional semicolon after the root node (e.g. "};\n") */
    tok = peek_token(ps);
    if (tok->type == TOK_SEMICOLON)
        consume_token(ps);

    /* Expect EOF */
    tok = peek_token(ps);
    if (tok->type != TOK_EOF)
        goto fail;

    /* Pass 2: resolve phandle references.
     * For each phandle ref, find the device by label and store its
     * pointer as a big-endian u32 in the property value. */
    for (int i = 0; i < ps->num_phandle_refs; i++)
    {
        struct phandle_ref *ref = &ps->phandle_refs[i];
        if (!ref->prop || !ref->label)
            continue;
        struct device *target = find_by_label(ps, ref->label);
        if (target)
        {
            /* Store the device pointer as a big-endian u32 */
            uint32_t val = (uint32_t)(uintptr_t)target;
            ref->prop->value[ref->offset] = (uint8_t)(val >> 24);
            ref->prop->value[ref->offset + 1] = (uint8_t)(val >> 16);
            ref->prop->value[ref->offset + 2] = (uint8_t)(val >> 8);
            ref->prop->value[ref->offset + 3] = (uint8_t)(val);
        }
    }

    /* Return parser state to caller (they own cleanup now) */
    *out_ps = ps;
    return 0;

fail:
    dt_free_parser_state(ps);
    *out_ps = NULL;
    return -1;
}

/* Free a parser_state returned by parse_tree_internal, including all
 * internal resources and the struct itself. Does NOT free the source
 * buffer or the parsed device tree. NULL-safe. */
void dt_free_parser_state(struct parser_state *ps)
{
    if (!ps)
        return;
    if (ps->cur_tok.text)
        FreeMem(ps->cur_tok.text, ddm_strlen(ps->cur_tok.text) + 1);
    if (ps->labels)
    {
        for (int i = 0; i < ps->num_labels; i++)
            if (ps->labels[i].label)
                FreeMem(ps->labels[i].label, ddm_strlen(ps->labels[i].label) + 1);
        FreeMem(ps->labels, ps->cap_labels * sizeof(struct label_entry));
    }
    if (ps->phandle_refs)
    {
        for (int i = 0; i < ps->num_phandle_refs; i++)
            if (ps->phandle_refs[i].label)
                FreeMem(ps->phandle_refs[i].label, ddm_strlen(ps->phandle_refs[i].label) + 1);
        FreeMem(ps->phandle_refs, ps->cap_phandle_refs * sizeof(struct phandle_ref));
    }
    FreeMem(ps, sizeof(struct parser_state));
}

/* ------------------------------------------------------------------ */
/* LVO -198: DT_ParseTree                                            */
/* ------------------------------------------------------------------ */

int32_t DT_ParseTree(struct DDMBase *ddm __asm("a6"), const char *filename __asm("a0"))
{
    DBG_DT("DT: ParseTree '%s'\n", filename);
    uint32_t file_len = 0;
    char *src = dt_read_file(filename, &file_len);
    if (!src)
    {
        DBG_DT("DT: ERR: failed to read '%s'\n", filename);
        return -1;
    }
    DBG_DT("DT: parsed %lu bytes\n", file_len);

    /* Create root device */
    struct device *root = ddm_create_device("");
    if (!root)
    {
        FreeMem(src, file_len + 1);
        return -1;
    }
    root->node.ln_Name = ddm_strdup("/");
    root->parent = NULL;

    if (!DDM_RegisterDevice(ddm, root))
    {
        DDM_UnregisterDevice(ddm, root);
        FreeMem(src, file_len + 1);
        return -1;
    }

    struct parser_state *ps = NULL;
    if (parse_tree_internal(ddm, src, file_len, root, FALSE, &ps) < 0)
    {
        DDM_UnregisterDevice(ddm, root);
        FreeMem(src, file_len + 1);
        return -1;
    }

    /* Persist labels to ddm->dt_labels so overlay files can
     * cross-reference main-tree devices via phandle properties. */
    for (int i = 0; i < ps->num_labels; i++)
    {
        struct dt_label *dl = (struct dt_label *)AllocMem(sizeof(struct dt_label), MEMF_ANY | MEMF_CLEAR);
        if (!dl)
            break;
        dl->node.ln_Name = ps->labels[i].label; /* transfer ownership */
        dl->dev = ps->labels[i].dev;
        AddTail(&ddm->dt_labels, &dl->node);
        ps->labels[i].label = NULL; /* mark as transferred */
    }

    /* Free parser state (labels already transferred or freed here) */
    dt_free_parser_state(ps);
    FreeMem(src, file_len + 1);
    return 0;
}
