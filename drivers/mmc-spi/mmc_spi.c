/*
 * mmc-spi.device - SD card driver over SPI for the DDM framework
 *
 * A single-file trackdisk.device-compatible block driver that talks to
 * SD cards (SD v2.0+: SDSC and SDHC/SDXC) over the DDM SPI subsystem,
 * registers cards with mmc.library, and exports a full trackdisk.device
 * interface (BeginIO/AbortIO).
 */

#include <string.h>

#include <devices/newstyle.h>
#include <devices/timer.h>
#include <devices/trackdisk.h>
#include <exec/devices.h>
#include <exec/errors.h>
#include <exec/execbase.h>
#include <exec/interrupts.h>
#include <exec/ports.h>
#include <exec/semaphores.h>
#include <exec/tasks.h>
#include <exec/types.h>
#include <libraries/dos.h>
#include <proto/alib.h>
#include <proto/exec.h>

#include "ddm_debug.h"

#include "ddm.h"
#include "ddm_driver.h"
#include "ddm_protos.h"
#include "devicetree.h"
#include "gpio.h"
#include "irq.h"
#include "mmc.h"
#include "spi.h"

/* --- Constants --- */

/* trackdisk.device */
#define TASK_STACK_SIZE 2048
#define TASK_PRIORITY 10

#define DEBOUNCE_TIMEOUT_US 100000

#define SIGB_CARD_CHANGE 31
#define SIGB_OP_REQUEST 30
#define SIGB_SHUTDOWN 29

#define SIGF_CARD_CHANGE (1 << SIGB_CARD_CHANGE)
#define SIGF_OP_REQUEST (1 << SIGB_OP_REQUEST)
#define SIGF_SHUTDOWN (1 << SIGB_SHUTDOWN)

#define NSD_QUERY_RESULT_LENGTH_REQUIRED 16

/* Drive geometry */
#define GEOM_CYL_SECTORS 4096
#define GEOM_HEADS 16

/* Timer utility (CIA-A TOD) */
#define TIMER_TICK_FREQ 50
#define TIMER_MILLIS(ms) (((uint32_t)(ms) * (TIMER_TICK_FREQ) + 999ul) / 1000ul)
#define TIMER_SECONDS(s) ((uint32_t)(s) * (TIMER_TICK_FREQ))

/* SD sectors */
#define SD_SECTOR_SIZE 512
#define SD_SECTOR_SHIFT 9

/* SD command opcodes */
#define CMD0 0             /* GO_IDLE_STATE */
#define CMD8 8             /* SEND_IF_COND */
#define CMD9 9             /* SEND_CSD */
#define CMD10 10           /* SEND_CID */
#define CMD12 12           /* STOP_TRANSMISSION */
#define CMD16 16           /* SET_BLOCKLEN */
#define CMD17 17           /* READ_SINGLE_BLOCK */
#define CMD18 18           /* READ_MULTIPLE_BLOCK */
#define CMD24 24           /* WRITE_BLOCK */
#define CMD25 25           /* WRITE_MULTIPLE_BLOCK */
#define CMD55 55           /* APP_CMD */
#define CMD58 58           /* READ_OCR */
#define ACMD23 (0x80 | 23) /* SET_WR_BLK_ERASE_COUNT (preceded by CMD55) */
#define ACMD41 (0x80 | 41) /* SD_SEND_OP_COND (preceded by CMD55) */

#define INIT_FREQUENCY 400000
#define MAX_FREQUENCY 50000000

#define READY_TIMEOUT_MS 500
#define INIT_TIMEOUT_MS 1000
#define MAX_RESPONSE_POLLS 10

/* R1 response flag bits */
#define R1_IDLE_STATE 0x01
#define R1_ILLEGAL_CMD 0x04

/* CMD8 SEND_IF_COND: argument and expected R7 response (voltage = 3.3V, check pattern = 0xAA) */
#define CMD8_IF_COND_ARG 0x1aa
#define CMD8_IF_COND_R7 0x000001aa

/* Data tokens */
#define DATA_TOKEN_SINGLE 0xfe
#define DATA_TOKEN_MULTI 0xfc
#define DATA_TOKEN_STOP 0xfd

static char device_name[] = "mmc-spi.device";
static char id_string[] = "mmc-spi 1.0 (29.8.2026)\n\r";

struct ExecBase *SysBase = NULL;
static BPTR saved_seg_list;

static struct List units;
static struct SignalSemaphore units_lock;

static struct DDMBase *DDMBase = NULL;
static struct SpiBase *SpiBase = NULL;
static struct MmcBase *MmcBase = NULL;

static struct device_driver mmc_spi_driver;

static const char *mmc_spi_compatible[] = {"mmc-spi", NULL};

enum sd_error
{
    SD_OK = 0,
    SD_ERR_NO_CARD = -1,
    SD_ERR_TIMEOUT = -2,
    SD_ERR_BAD_RESPONSE = -3,
    SD_ERR_UNSUPPORTED = -4
};

enum sd_card_type
{
    SD_TYPE_NONE = 0,
    SD_TYPE_SD20_SDSC, /* SD v2.0 standard capacity, byte-addressed */
    SD_TYPE_SD20_SDHC  /* SD v2.0 high/extended capacity, block-addressed */
};

struct sd_card_info
{
    enum sd_card_type type;
    uint32_t total_sectors;
    uint32_t block_size; /* shift value (e.g. 9 = 512 bytes) */
};

struct sd_context
{
    struct spi_device *spidev;
    struct SpiBase *spi_base;
    struct gpio_desc *cd_gpio;
    struct DDMBase *gpio_base;
    struct sd_card_info card_info;
};

/* Per-unit state. Allocated at probe, stored in dev->driver_data,
 * and freed at shutdown. Keeping this on the heap (rather than in
 * file-scope statics) lets multiple mmc-spi instances coexist — each
 * probe gets its own private state. */
struct mmc_spi_unit
{
    struct Node node;   /* linkage in units list */
    uint32_t unit_num;  /* DTS "device-unit" value */
    struct device *dev; /* device tree device */

    struct sd_context sd_ctx; /* SD card layer context */

    struct MsgPort mp; /* message port for I/O requests */
    struct Task *task; /* background task */

    volatile int16_t card_present;
    volatile int16_t card_opened;
    volatile uint32_t card_change_num;

    /* Graceful task shutdown handshake */
    struct Task *shutdown_caller;
    volatile int16_t task_exited;

    struct Interrupt *remove_int; /* TD_REMOVE interrupt */
    struct IOStdReq *change_int;  /* TD_ADDCHANGEINT request */

    /* Card change interrupt (from cd-gpios via GPIO_ToIrq) */
    int card_change_virq;
    struct Interrupt card_change_int;

    /* MMC card registration */
    struct mmc_card mmc_card;
};

/* --- Timer utility (CIA-A TOD) --- */

/* CIA-A TOD registers. The Amiga CIA TOD counter is ALWAYS in linear
 * tick count mode (not BCD). Reading MSB latches the count; reading
 * LSB unlatches it. Read order is therefore h, m, l. */
static volatile uint8_t *const todl = (volatile uint8_t *)0xbfe801;
static volatile uint8_t *const todm = (volatile uint8_t *)0xbfe901;
static volatile uint8_t *const todh = (volatile uint8_t *)0xbfea01;

static uint32_t timer_get_tick_count()
{
    uint8_t l, m, h;

    h = *todh;
    m = *todm;
    l = *todl;
    return ((uint32_t)h << 16) | ((uint32_t)m << 8) | (uint32_t)l;
}

/* --- SD card protocol --- */

/* Dummy bytes written during reads (keeps the clock running so the
 * card can drive MISO). 16 bytes is enough for any single-token wait. */
static const uint8_t dummy_ff[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/* --- Command/response primitives --- */

/* Send a command and return the R1 response byte (or 0xFF on error).
 * For ACMD commands (bit 7 set in cmd), CMD55 is sent first. */
static uint8_t sd_send_cmd(struct sd_context *ctx, uint8_t cmd, uint32_t arg)
{
    uint8_t res;

    if (cmd & 0x80)
    {
        /* Application command: send CMD55 first */
        cmd &= 0x7f;
        res = sd_send_cmd(ctx, CMD55, 0);
        if (res > 1)
            return res;
    }

    /* Select the card and wait for ready, except for CMD12 (abort)
     * which is sent while the card may be busy. */
    if (cmd != CMD12)
    {
        SPI_Deselect(ctx->spi_base, ctx->spidev);
        SPI_Select(ctx->spi_base, ctx->spidev);
        /* Wait for not-busy */
        uint32_t deadline = timer_get_tick_count() + TIMER_MILLIS(READY_TIMEOUT_MS);
        do
        {
            SPI_Transfer(ctx->spi_base, ctx->spidev, NULL, &res, 1);
        } while (res != 0xff && (int32_t)(timer_get_tick_count() - deadline) < 0);
        if (res != 0xff)
            return 0xff;
    }

    /* Build the 6-byte command frame */
    uint8_t buf[6];
    buf[0] = 0x40 | cmd;
    buf[1] = (uint8_t)(arg >> 24);
    buf[2] = (uint8_t)(arg >> 16);
    buf[3] = (uint8_t)(arg >> 8);
    buf[4] = (uint8_t)(arg >> 0);
    if (cmd == CMD0)
        buf[5] = 0x95; /* valid CRC for CMD0 */
    else if (cmd == CMD8)
        buf[5] = 0x87; /* valid CRC for CMD8 */
    else
        buf[5] = 0x01; /* dummy CRC + stop bit */

    SPI_Transfer(ctx->spi_base, ctx->spidev, buf, NULL, sizeof(buf));

    /* CMD12: the first byte after STOP_TRANSMISSION is a stuff byte
     * that must be discarded before the R1 response. */
    if (cmd == CMD12)
        SPI_Transfer(ctx->spi_base, ctx->spidev, NULL, &res, 1);

    /* Poll for R1: the response has bit 7 clear (valid responses are
     * 0x00..0x7F). Up to MAX_RESPONSE_POLLS bytes. */
    for (int n = 0; n < MAX_RESPONSE_POLLS; n++)
    {
        SPI_Transfer(ctx->spi_base, ctx->spidev, NULL, &res, 1);
        if (!(res & 0x80))
            break;
    }

    return res;
}

/* Read the 4-byte R7 / OCR payload that follows an R1 response
 * (used by CMD8 and CMD58). Returns the 32-bit big-endian value. */
static uint32_t sd_read_r7(struct sd_context *ctx)
{
    uint8_t buf[4];
    SPI_Transfer(ctx->spi_base, ctx->spidev, NULL, buf, 4);
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
}

/* Wait for the card to become not-busy (MISO reads 0xFF). */
static int sd_wait_ready(struct sd_context *ctx)
{
    uint32_t deadline = timer_get_tick_count() + TIMER_MILLIS(READY_TIMEOUT_MS);
    uint8_t in;
    do
    {
        SPI_Transfer(ctx->spi_base, ctx->spidev, NULL, &in, 1);
    } while (in != 0xff && (int32_t)(timer_get_tick_count() - deadline) < 0);
    return (in == 0xff) ? SD_OK : SD_ERR_TIMEOUT;
}

/* Wait for a specific data token (non-0xFF). Returns SD_OK if the
 * expected token arrived, SD_ERR_TIMEOUT if the deadline expired, or
 * SD_ERR_BAD_RESPONSE if a different token arrived. */
static int sd_wait_token(struct sd_context *ctx, uint8_t token)
{
    uint32_t deadline = timer_get_tick_count() + TIMER_MILLIS(READY_TIMEOUT_MS);
    uint8_t in;
    do
    {
        SPI_Transfer(ctx->spi_base, ctx->spidev, NULL, &in, 1);
    } while (in == 0xff && (int32_t)(timer_get_tick_count() - deadline) < 0);
    if (in == 0xff)
        return SD_ERR_TIMEOUT;
    if (in != token)
        return SD_ERR_BAD_RESPONSE;
    return SD_OK;
}

/* Read a data block: wait for the 0xFE start token, then read `size`
 * data bytes + 2 CRC bytes (CRC discarded). */
static int sd_read_block(struct sd_context *ctx, uint8_t *buf, uint32_t size)
{
    int err = sd_wait_token(ctx, DATA_TOKEN_SINGLE);
    if (err != SD_OK)
        return err;

    uint8_t crc[2];
    SPI_Transfer(ctx->spi_base, ctx->spidev, NULL, buf, size);
    SPI_Transfer(ctx->spi_base, ctx->spidev, NULL, crc, 2); /* discard CRC */
    return SD_OK;
}

/* Write a data block: send the token, 512 data bytes, 2 dummy CRC
 * bytes, then read the data-response byte and wait for not-busy.
 * If token is DATA_TOKEN_STOP (0xFD), only the token is sent (no
 * data, no CRC, no response check) — used to end a multi-write. */
static int sd_write_block(struct sd_context *ctx, const uint8_t *buf, uint8_t token)
{
    if (sd_wait_ready(ctx) != SD_OK)
        return SD_ERR_TIMEOUT;

    SPI_Transfer(ctx->spi_base, ctx->spidev, &token, NULL, 1);

    if (token == DATA_TOKEN_STOP)
    {
        /* After STOP_TRAN, read one byte so the next sd_wait_ready
         * does not mistake it for an immediate ready. */
        uint8_t dummy;
        SPI_Transfer(ctx->spi_base, ctx->spidev, NULL, &dummy, 1);
        return SD_OK;
    }

    uint8_t crc[2] = {0xff, 0xff};
    SPI_Transfer(ctx->spi_base, ctx->spidev, buf, NULL, SD_SECTOR_SIZE);
    SPI_Transfer(ctx->spi_base, ctx->spidev, crc, NULL, 2);

    uint8_t resp;
    SPI_Transfer(ctx->spi_base, ctx->spidev, NULL, &resp, 1);
    if ((resp & 0x1f) != 0x05)
        return SD_ERR_BAD_RESPONSE;

    return sd_wait_ready(ctx);
}

/* --- Context init --- */

static void sd_init_context(struct sd_context *ctx, struct spi_device *spidev, struct SpiBase *spi_base,
                            struct gpio_desc *cd_gpio, struct DDMBase *gpio_base)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->spidev = spidev;
    ctx->spi_base = spi_base;
    ctx->cd_gpio = cd_gpio;
    ctx->gpio_base = gpio_base;
}

/* --- CSD parsing --- */

/* Parse a 16-byte CSD register (received as 4 big-endian u32 words)
 * and fill in card_info->total_sectors and block_size.
 * Supports CSD v1.0 (SDSC) and CSD v2.0 (SDHC/SDXC). */
static int sd_parse_csd(struct sd_card_info *ci, const uint32_t *bits)
{
    uint32_t csd_version = (bits[0] >> 30) & 0x3;

    DBG_MMC_SPI("MMC-SPI: CSD %08lx %08lx %08lx %08lx\n", (unsigned long)bits[0], (unsigned long)bits[1],
                (unsigned long)bits[2], (unsigned long)bits[3]);

    if (csd_version == 0)
    {
        /* CSD v1.0 — SDSC (byte-addressed) */
        uint32_t c_size = ((bits[1] & 0x3ff) << 2) | (bits[2] >> 30);
        uint32_t c_size_mult = (bits[2] >> 15) & 0x7;
        uint32_t read_bl_len = (bits[1] >> 16) & 0xf;

        /* capacity in bytes = (C_SIZE+1) * 2^(C_SIZE_MULT+2) * 2^(READ_BL_LEN)
         * sectors = bytes / 512 */
        uint32_t block_len = 1ul << read_bl_len;
        uint32_t mult = 1ul << (c_size_mult + 2);
        ci->total_sectors = ((c_size + 1) * mult * block_len) >> SD_SECTOR_SHIFT;
        ci->block_size = read_bl_len; /* shift value */
        DBG_MMC_SPI("MMC-SPI: CSD v1.0 c_size=%ld mult=%ld bl_len=%ld sectors=%ld\n", (long)c_size, (long)mult,
                    (long)read_bl_len, (long)ci->total_sectors);
    }
    else if (csd_version == 1)
    {
        /* CSD v2.0 — SDHC/SDXC (block-addressed) */
        uint32_t c_size = ((bits[1] & 0x3f) << 16) | (bits[2] >> 16);

        ci->total_sectors = (c_size + 1) * 1024;
        ci->block_size = SD_SECTOR_SHIFT; /* always 512 */
        DBG_MMC_SPI("MMC-SPI: CSD v2.0 c_size=%ld sectors=%ld\n", (long)c_size, (long)ci->total_sectors);
    }
    else
    {
        DBG_MMC_SPI("MMC-SPI: ERR: unsupported CSD version %ld\n", (long)csd_version);
        return SD_ERR_UNSUPPORTED;
    }

    return SD_OK;
}

/* --- Card init (SD v2.0+ only) --- */

static int sd_open(struct sd_context *ctx)
{
    struct sd_card_info *ci = &ctx->card_info;

    SPI_SetSpeed(ctx->spi_base, ctx->spidev, INIT_FREQUENCY);
    ci->type = SD_TYPE_NONE;
    ci->total_sectors = 0;
    ci->block_size = SD_SECTOR_SHIFT;

    /* Send 80 dummy clocks with CS high to wake the card.
     * 10 bytes = 80 clock edges. */
    SPI_Deselect(ctx->spi_base, ctx->spidev);
    SPI_Transfer(ctx->spi_base, ctx->spidev, dummy_ff, NULL, 10);

    /* CMD0: GO_IDLE_STATE — expect R1 = 0x01 (idle) */
    uint8_t r1 = sd_send_cmd(ctx, CMD0, 0);
    if (r1 != R1_IDLE_STATE)
    {
        DBG_MMC_SPI("MMC-SPI: CMD0 failed r1=%02lx\n", (unsigned long)r1);
        return SD_ERR_BAD_RESPONSE;
    }

    /* CMD8: SEND_IF_COND — arg 0x1AA, expect R1=0x01 + R7=0x000001AA.
     * If this fails, the card is not SD v2.0 (we don't support v1.x). */
    r1 = sd_send_cmd(ctx, CMD8, CMD8_IF_COND_ARG);
    if (r1 != R1_IDLE_STATE)
    {
        DBG_MMC_SPI("MMC-SPI: CMD8 failed r1=%02lx (not SD v2.0)\n", (unsigned long)r1);
        return SD_ERR_UNSUPPORTED;
    }
    {
        uint32_t r7 = sd_read_r7(ctx);
        if (r7 != CMD8_IF_COND_R7)
        {
            DBG_MMC_SPI("MMC-SPI: CMD8 R7 mismatch %08lx\n", (unsigned long)r7);
            return SD_ERR_UNSUPPORTED;
        }
    }

    /* ACMD41 with HCS bit (bit 30) — wait for idle bit to clear.
     * Timeout after ~1 second. */
    uint32_t deadline = timer_get_tick_count() + TIMER_MILLIS(INIT_TIMEOUT_MS);
    do
    {
        r1 = sd_send_cmd(ctx, ACMD41, 1ul << 30);
        if ((int32_t)(timer_get_tick_count() - deadline) >= 0)
        {
            DBG_MMC_SPI("MMC-SPI: ACMD41 timeout\n");
            return SD_ERR_TIMEOUT;
        }
    } while (r1 != 0);

    /* CMD58: READ_OCR — check CCS bit (bit 30) to determine
     * block-addressed (SDHC/SDXC) vs byte-addressed (SDSC). */
    r1 = sd_send_cmd(ctx, CMD58, 0);
    if (r1 != 0)
    {
        DBG_MMC_SPI("MMC-SPI: CMD58 failed r1=%02lx\n", (unsigned long)r1);
        return SD_ERR_BAD_RESPONSE;
    }
    {
        uint32_t ocr = sd_read_r7(ctx);
        if (ocr & (1ul << 30))
        {
            ci->type = SD_TYPE_SD20_SDHC;
            DBG_MMC_SPI("MMC-SPI: SDHC/SDXC (block-addressed)\n");
        }
        else
        {
            ci->type = SD_TYPE_SD20_SDSC;
            DBG_MMC_SPI("MMC-SPI: SDSC (byte-addressed)\n");
            /* SDSC: set block length to 512 */
            r1 = sd_send_cmd(ctx, CMD16, SD_SECTOR_SIZE);
            if (r1 != 0)
            {
                DBG_MMC_SPI("MMC-SPI: CMD16 failed r1=%02lx\n", (unsigned long)r1);
                return SD_ERR_BAD_RESPONSE;
            }
        }
    }

    /* CMD9: SEND_CSD — read 16-byte CSD block and parse it. */
    r1 = sd_send_cmd(ctx, CMD9, 0);
    if (r1 != 0)
    {
        DBG_MMC_SPI("MMC-SPI: CMD9 failed r1=%02lx\n", (unsigned long)r1);
        return SD_ERR_BAD_RESPONSE;
    }
    uint32_t resp[4];
    int err = sd_read_block(ctx, (uint8_t *)resp, sizeof(resp));
    if (err != SD_OK)
    {
        DBG_MMC_SPI("MMC-SPI: CSD read failed err=%ld\n", (long)err);
        return err;
    }
    err = sd_parse_csd(ci, resp);
    if (err != SD_OK)
        return err;

    /* CMD10: SEND_CID — read 16-byte CID block (stored for debugging). */
    r1 = sd_send_cmd(ctx, CMD10, 0);
    if (r1 == 0)
        sd_read_block(ctx, (uint8_t *)resp, sizeof(resp));

    /* Switch to the fast clock. SPI_SetSpeed clamps to the
     * spi_device.max_speed from the device tree. */
    SPI_SetSpeed(ctx->spi_base, ctx->spidev, MAX_FREQUENCY);

    SPI_Deselect(ctx->spi_base, ctx->spidev);

    DBG_MMC_SPI("MMC-SPI: card ready type=%ld sectors=%ld\n", (long)ci->type, (long)ci->total_sectors);
    return SD_OK;
}

/* --- Block read --- */

static int sd_read(struct sd_context *ctx, uint8_t *buf, uint32_t sector, uint32_t count)
{
    struct sd_card_info *ci = &ctx->card_info;

    if (ci->type == SD_TYPE_NONE)
        return SD_ERR_NO_CARD;

    /* SDSC uses byte addressing; SDHC uses block addressing */
    if (ci->type == SD_TYPE_SD20_SDSC)
        sector <<= SD_SECTOR_SHIFT;

    int err = SD_OK;

    if (count == 1)
    {
        if (sd_send_cmd(ctx, CMD17, sector) != 0)
            err = SD_ERR_BAD_RESPONSE;
        else
            err = sd_read_block(ctx, buf, SD_SECTOR_SIZE);
    }
    else if (count > 1)
    {
        if (sd_send_cmd(ctx, CMD18, sector) != 0)
        {
            err = SD_ERR_BAD_RESPONSE;
        }
        else
        {
            do
            {
                err = sd_read_block(ctx, buf, SD_SECTOR_SIZE);
                if (err != SD_OK)
                    break;
                buf += SD_SECTOR_SIZE;
            } while (--count);
            /* Always send CMD12 to stop multi-read, even on error */
            sd_send_cmd(ctx, CMD12, 0);
        }
    }

    SPI_Deselect(ctx->spi_base, ctx->spidev);
    return err;
}

/* --- Block write --- */

static int sd_write(struct sd_context *ctx, const uint8_t *buf, uint32_t sector, uint32_t count)
{
    struct sd_card_info *ci = &ctx->card_info;

    if (ci->type == SD_TYPE_NONE)
        return SD_ERR_NO_CARD;

    if (ci->type == SD_TYPE_SD20_SDSC)
        sector <<= SD_SECTOR_SHIFT;

    int err = SD_OK;

    if (count == 1)
    {
        if (sd_send_cmd(ctx, CMD24, sector) != 0)
            err = SD_ERR_BAD_RESPONSE;
        else
            err = sd_write_block(ctx, buf, DATA_TOKEN_SINGLE);
    }
    else if (count > 1)
    {
        /* Pre-erase the sector count for faster multi-write */
        sd_send_cmd(ctx, ACMD23, count);

        if (sd_send_cmd(ctx, CMD25, sector) != 0)
        {
            err = SD_ERR_BAD_RESPONSE;
        }
        else
        {
            do
            {
                err = sd_write_block(ctx, buf, DATA_TOKEN_MULTI);
                if (err != SD_OK)
                    break;
                buf += SD_SECTOR_SIZE;
            } while (--count);

            /* Send STOP_TRAN token to end multi-write */
            if (err == SD_OK)
                err = sd_write_block(ctx, NULL, DATA_TOKEN_STOP);
        }
    }

    SPI_Deselect(ctx->spi_base, ctx->spidev);
    return err;
}

static void sd_close(struct sd_context *ctx)
{
    ctx->card_info.type = SD_TYPE_NONE;
    ctx->card_info.total_sectors = 0;
}

/* --- Device driver helpers (trackdisk.device) --- */

/* --- Unit lookup --- */

static struct mmc_spi_unit *find_unit(uint32_t unitnum)
{
    ObtainSemaphore(&units_lock);
    for (struct mmc_spi_unit *unit = (struct mmc_spi_unit *)units.lh_Head; unit->node.ln_Succ;
         unit = (struct mmc_spi_unit *)unit->node.ln_Succ)
    {
        if (unit->unit_num == unitnum)
        {
            ReleaseSemaphore(&units_lock);
            return unit;
        }
    }
    ReleaseSemaphore(&units_lock);
    return NULL;
}

/* --- Card change ISR --- */

/* Called via the DDM IRQ dispatch (a regular C function call through
 * a function pointer), NOT by the AmigaOS interrupt dispatcher. */
static void change_isr(void *data __asm("a1"))
{
    struct mmc_spi_unit *unit = (struct mmc_spi_unit *)data;
    Signal(unit->task, SIGF_CARD_CHANGE);
}

/* --- Geometry --- */

static uint32_t device_get_geometry(struct mmc_spi_unit *unit, struct IOStdReq *ior)
{
    struct DriveGeometry *geom = (struct DriveGeometry *)ior->io_Data;
    const struct sd_card_info *ci = &unit->sd_ctx.card_info;

    if (ci->type == SD_TYPE_NONE)
        return TDERR_DiskChanged;

    geom->dg_SectorSize = 1 << ci->block_size;
    geom->dg_TotalSectors = ci->total_sectors;
    geom->dg_Cylinders = ci->total_sectors / GEOM_CYL_SECTORS;
    geom->dg_CylSectors = GEOM_CYL_SECTORS;
    geom->dg_Heads = GEOM_HEADS;
    geom->dg_TrackSectors = GEOM_CYL_SECTORS / GEOM_HEADS;
    geom->dg_BufMemType = MEMF_PUBLIC;
    geom->dg_DeviceType = DG_DIRECT_ACCESS;
    geom->dg_Flags = DGF_REMOVABLE;
    return 0;
}

/* --- Card change handling --- */

/* Debounce the card-detect switch via timer.device. Creates and
 * tears down its own timer request on each call. */
static void wait_debounce()
{
    struct MsgPort *mp = CreatePort(NULL, 0);
    if (mp)
    {
        struct timerequest *tr = (struct timerequest *)CreateExtIO(mp, sizeof(struct timerequest));
        if (tr)
        {
            if (!OpenDevice(TIMERNAME, UNIT_VBLANK, (struct IORequest *)tr, 0))
            {
                tr->tr_node.io_Command = TR_ADDREQUEST;
                tr->tr_time.tv_secs = 0;
                tr->tr_time.tv_micro = DEBOUNCE_TIMEOUT_US;
                DoIO((struct IORequest *)tr);

                CloseDevice((struct IORequest *)tr);
            }
            DeleteExtIO((struct IORequest *)tr);
        }
        DeletePort(mp);
    }
}

static void handle_changed(struct mmc_spi_unit *unit)
{
    DBG_MMC_SPI("MMC-SPI: handle_changed enter\n");

    wait_debounce();

    int res = GPIO_GetValue(DDMBase, unit->sd_ctx.cd_gpio);
    DBG_MMC_SPI("MMC-SPI: handle_changed gpio=%ld\n", (long)res);

    if (res == 1 && sd_open(&unit->sd_ctx) == SD_OK)
        unit->card_opened = TRUE;
    else
        unit->card_opened = FALSE;

    ObtainSemaphore(&units_lock);
    unit->card_present = (res == 1);
    unit->card_change_num++;
    ReleaseSemaphore(&units_lock);

    DBG_MMC_SPI("MMC-SPI: card_present=%ld change_num=%ld opened=%ld\n", (long)unit->card_present,
                (long)unit->card_change_num, (long)unit->card_opened);

    /* TD_REMOVE: notify on removal only (card absent). */
    if (unit->remove_int && !unit->card_present)
        Cause(unit->remove_int);

    /* TD_ADDCHANGEINT: notify on both insert and remove. */
    if (unit->change_int)
        Cause((struct Interrupt *)unit->change_int->io_Data);
}

/* --- 64-bit offset to sector conversion --- */

static uint32_t offset_to_sd_sectors(uint32_t high_offset, uint32_t low_offset)
{
    return (high_offset << (32 - SD_SECTOR_SHIFT)) | (low_offset >> SD_SECTOR_SHIFT);
}

/* --- Request processing (runs in background task) --- */

static void process_request(struct mmc_spi_unit *unit, struct IOStdReq *ior)
{
    if (!unit->card_present)
        ior->io_Error = TDERR_DiskChanged;
    else if (!unit->card_opened)
        ior->io_Error = TDERR_NotSpecified;
    else
    {
        switch (ior->io_Command)
        {
        case TD_GETGEOMETRY:
            ior->io_Error = device_get_geometry(unit, ior);
            break;

        case TD_FORMAT:
        case CMD_WRITE:
            ior->io_Actual = 0;
            /* fall through */
        case TD_FORMAT64:
        case TD_WRITE64:
        case NSCMD_TD_FORMAT64:
        case NSCMD_TD_WRITE64:
            if (sd_write(&unit->sd_ctx, (uint8_t *)ior->io_Data, offset_to_sd_sectors(ior->io_Actual, ior->io_Offset),
                         ior->io_Length >> SD_SECTOR_SHIFT) == SD_OK)
                ior->io_Actual = ior->io_Length;
            else
                ior->io_Error = TDERR_NotSpecified;
            break;

        case CMD_READ:
            ior->io_Actual = 0;
            /* fall through */
        case TD_READ64:
        case NSCMD_TD_READ64:
            if (sd_read(&unit->sd_ctx, (uint8_t *)ior->io_Data, offset_to_sd_sectors(ior->io_Actual, ior->io_Offset),
                        ior->io_Length >> SD_SECTOR_SHIFT) == SD_OK)
                ior->io_Actual = ior->io_Length;
            else
                ior->io_Error = TDERR_NotSpecified;
            break;

        default:
            ior->io_Error = IOERR_NOCMD;
            break;
        }
    }

    ReplyMsg(&ior->io_Message);
}

/* --- Background task --- */

static void task_run()
{
    struct mmc_spi_unit *unit = (struct mmc_spi_unit *)FindTask(NULL)->tc_UserData;

    DBG_MMC_SPI("MMC-SPI: task started unit=%ld\n", (long)unit->unit_num);

    /* Allocate used signals, so that CreatePort() in wait_debounce gets a fresh signal. */
    AllocSignal(SIGB_CARD_CHANGE);
    AllocSignal(SIGB_OP_REQUEST);
    AllocSignal(SIGB_SHUTDOWN);

    if (unit->card_present && sd_open(&unit->sd_ctx) == SD_OK)
    {
        DBG_MMC_SPI("MMC-SPI: sd_open ok\n");
        unit->card_opened = TRUE;
    }
    else
    {
        DBG_MMC_SPI("MMC-SPI: sd_open failed present=%ld\n", (long)unit->card_present);
    }

    for (;;)
    {
        DBG_MMC_SPI("MMC-SPI: task waiting\n");
        uint32_t sigs = Wait(SIGF_CARD_CHANGE | SIGF_OP_REQUEST | SIGF_SHUTDOWN);
        DBG_MMC_SPI("MMC-SPI: task woke sigs=0x%08lx\n", (unsigned long)sigs);

        if (sigs & SIGF_SHUTDOWN)
        {
            unit->task_exited = TRUE;
            if (unit->shutdown_caller)
                Signal(unit->shutdown_caller, SIGF_SHUTDOWN);
            return;
        }

        if (sigs & SIGF_CARD_CHANGE)
            handle_changed(unit);

        if (sigs & SIGF_OP_REQUEST)
        {
            int16_t first = TRUE;
            struct IOStdReq *ior;

            while ((ior = (struct IOStdReq *)GetMsg(&unit->mp)))
            {
                if (!first && (SetSignal(0, SIGF_CARD_CHANGE) & SIGF_CARD_CHANGE))
                    handle_changed(unit);
                process_request(unit, ior);
                first = FALSE;
            }
        }
    }
}

/* --- trackdisk.device BeginIO / AbortIO --- */

static const uint16_t supported_commands[] = {CMD_RESET,
                                              CMD_READ,
                                              CMD_WRITE,
                                              CMD_UPDATE,
                                              CMD_CLEAR,
                                              TD_MOTOR,
                                              TD_FORMAT,
                                              TD_REMOVE,
                                              TD_CHANGENUM,
                                              TD_CHANGESTATE,
                                              TD_PROTSTATUS,
                                              TD_GETDRIVETYPE,
                                              TD_ADDCHANGEINT,
                                              TD_REMCHANGEINT,
                                              TD_GETGEOMETRY,
                                              TD_READ64,
                                              TD_WRITE64,
                                              TD_FORMAT64,
                                              NSCMD_DEVICEQUERY,
                                              NSCMD_TD_READ64,
                                              NSCMD_TD_WRITE64,
                                              NSCMD_TD_FORMAT64,
                                              0};

static void begin_io(struct Library *dev __asm("a6"), struct IOStdReq *ior __asm("a1"))
{
    (void)dev;

    if (!ior)
        return;

    struct mmc_spi_unit *unit = (struct mmc_spi_unit *)ior->io_Unit;
    if (!unit)
    {
        ior->io_Error = IOERR_OPENFAIL;
        return;
    }

    ior->io_Error = 0;

    switch (ior->io_Command)
    {
    /* --- Synchronous (quick) commands --- */
    case CMD_RESET:
    case CMD_CLEAR:
    case CMD_UPDATE:
    case TD_MOTOR:
    case TD_PROTSTATUS:
        ior->io_Actual = 0;
        break;

    case TD_CHANGESTATE:
        ior->io_Actual = unit->card_present ? 0 : 1;
        break;

    case TD_CHANGENUM:
        ior->io_Actual = unit->card_change_num;
        break;

    case TD_GETDRIVETYPE:
        ior->io_Actual = DG_DIRECT_ACCESS;
        break;

    case TD_REMOVE:
        unit->remove_int = (struct Interrupt *)ior->io_Data;
        break;

    case TD_ADDCHANGEINT:
        if (unit->change_int)
            ior->io_Error = IOERR_ABORTED;
        else
        {
            unit->change_int = ior;
            ior->io_Flags &= ~IOF_QUICK;
            ior = NULL; /* don't reply — caller waits for Cause() */
        }
        break;

    case TD_REMCHANGEINT:
        if (unit->change_int == ior)
            unit->change_int = NULL;
        break;

    case NSCMD_DEVICEQUERY:
        if (ior->io_Length >= NSD_QUERY_RESULT_LENGTH_REQUIRED)
        {
            struct NSDeviceQueryResult *result = ior->io_Data;
            result->nsdqr_DevQueryFormat = 0;
            result->nsdqr_SizeAvailable = NSD_QUERY_RESULT_LENGTH_REQUIRED;
            result->nsdqr_DeviceType = NSDEVTYPE_TRACKDISK;
            result->nsdqr_DeviceSubType = 0;
            result->nsdqr_SupportedCommands = (void *)supported_commands;
            ior->io_Actual = NSD_QUERY_RESULT_LENGTH_REQUIRED;
        }
        else
        {
            ior->io_Error = IOERR_BADLENGTH;
        }
        break;

    /* --- Asynchronous (queued to background task) --- */
    case TD_GETGEOMETRY:
    case TD_FORMAT:
    case CMD_WRITE:
    case CMD_READ:
    case TD_READ64:
    case TD_WRITE64:
    case TD_FORMAT64:
    case NSCMD_TD_READ64:
    case NSCMD_TD_WRITE64:
    case NSCMD_TD_FORMAT64:
        PutMsg(&unit->mp, (struct Message *)&ior->io_Message);
        ior->io_Flags &= ~IOF_QUICK;
        ior = NULL; /* don't reply here — task will ReplyMsg */
        break;

    default:
        ior->io_Error = IOERR_NOCMD;
    }

    if (ior && !(ior->io_Flags & IOF_QUICK))
        ReplyMsg(&ior->io_Message);
}

static uint32_t abort_io(struct Library *dev __asm("a6"), struct IORequest *ior __asm("a1"))
{
    (void)dev;
    (void)ior;
    return IOERR_NOCMD;
}

/* --- DDM driver LVOs --- */

/* LVO -42: DDMDrv_Probe */
int32_t mmc_spi_probe(struct Library *drv __asm("a6"), struct device *dev __asm("a0"))
{
    (void)drv;

    uint32_t unit_num = 0;
    if (DT_GetPropertyU32(DDMBase, dev, "device-unit", &unit_num) != 0)
    {
        DBG_MMC_SPI("MMC-SPI: ERR: missing device-unit property\n");
        return -1;
    }

    DBG_MMC_SPI("MMC-SPI: probe dev='%s' unit=%ld\n", dev->node.ln_Name ? dev->node.ln_Name : "(unnamed)",
                (long)unit_num);

    struct mmc_spi_unit *unit = (struct mmc_spi_unit *)AllocMem(sizeof(*unit), MEMF_CLEAR | MEMF_PUBLIC);
    if (!unit)
        return -1;

    unit->unit_num = unit_num;
    unit->dev = dev;

    /* Find our SPI device via spi.library */
    struct spi_device *spidev = SPI_FindDevice(SpiBase, dev);
    if (!spidev)
    {
        DBG_MMC_SPI("MMC-SPI: ERR: SPI device not found\n");
        FreeMem(unit, sizeof(*unit));
        return -1;
    }

    /* Resolve card-detect GPIO from the device tree ("cd-gpios") */
    struct gpio_desc *cd_gpio = GPIO_Get(DDMBase, dev, "cd", GPIOD_IN);
    if (!cd_gpio)
    {
        DBG_MMC_SPI("MMC-SPI: ERR: cd-gpios not found\n");
        FreeMem(unit, sizeof(*unit));
        return -1;
    }

    sd_init_context(&unit->sd_ctx, spidev, SpiBase, cd_gpio, DDMBase);

    /* Check initial card presence BEFORE CreateTask to avoid a race
     * where the task reads card_present before we set it. */
    int res = GPIO_GetValue(DDMBase, cd_gpio);
    unit->card_present = (res == 1);
    DBG_MMC_SPI("MMC-SPI: card_present=%ld\n", (long)unit->card_present);

    /* Create the background task. Forbid()/Permit() around creation
     * and setup so the task (higher priority) cannot run until
     * tc_UserData and message ports are initialized. */
    Forbid();

    unit->task = CreateTask((CONST_STRPTR)device_name, TASK_PRIORITY, (char *)&task_run, TASK_STACK_SIZE);
    if (!unit->task)
    {
        Permit();
        DBG_MMC_SPI("MMC-SPI: ERR: CreateTask failed\n");
        GPIO_Free(DDMBase, cd_gpio);
        FreeMem(unit, sizeof(*unit));
        return -1;
    }

    unit->task->tc_UserData = (void *)unit;

    /* Set up the I/O message port (signalled by the background task) */
    unit->mp.mp_Node.ln_Type = NT_MSGPORT;
    unit->mp.mp_Flags = PA_SIGNAL;
    unit->mp.mp_SigBit = SIGB_OP_REQUEST;
    unit->mp.mp_SigTask = unit->task;
    NewList(&unit->mp.mp_MsgList);

    Permit();

    /* Set up the card-change interrupt */
    unit->card_change_virq = GPIO_ToIrq(DDMBase, cd_gpio);
    if (unit->card_change_virq >= 0)
    {
        unit->card_change_int.is_Node.ln_Name = "mmc-spi";
        unit->card_change_int.is_Node.ln_Type = NT_INTERRUPT;
        unit->card_change_int.is_Code = change_isr;
        unit->card_change_int.is_Data = (void *)unit;

        DBG_MMC_SPI("MMC-SPI: card-change virq=%ld\n", (long)unit->card_change_virq);

        if (IRQ_AddHandler(DDMBase, unit->card_change_virq, &unit->card_change_int, IRQ_TYPE_EDGE_BOTH) == 0)
        {
            IRQ_Disable(DDMBase, unit->card_change_virq);
            IRQ_Enable(DDMBase, unit->card_change_virq);
        }
    }

    /* Register as an MMC card with mmc.library */
    unit->mmc_card.dev = dev;
    unit->mmc_card.card_type = 0;
    unit->mmc_card.total_sectors = 0;
    unit->mmc_card.block_size_shift = SD_SECTOR_SHIFT;
    unit->mmc_card.block_size = SD_SECTOR_SIZE;
    unit->mmc_card.private = NULL;
    MMC_RegisterCard(MmcBase, &unit->mmc_card);

    /* Store the unit as driver_data so shutdown can find it */
    dev->driver_data = (void *)unit;

    ObtainSemaphore(&units_lock);
    AddTail(&units, &unit->node);
    ReleaseSemaphore(&units_lock);

    DBG_MMC_SPI("MMC-SPI: probe ok unit=%ld\n", (long)unit_num);
    return 0;
}

/* LVO -48: DDMDrv_Remove */
void mmc_spi_remove(struct Library *drv __asm("a6"), struct device *dev __asm("a0"))
{
    (void)drv;
    (void)dev;
}

/* LVO -54: DDMDrv_Init (reserved — merged into Probe) */
int32_t mmc_spi_init(struct Library *drv __asm("a6"), struct device *dev __asm("a0"))
{
    (void)drv;
    (void)dev;
    return 0;
}

/* LVO -60: DDMDrv_Shutdown */
void mmc_spi_shutdown(struct Library *drv __asm("a6"), struct device *dev __asm("a0"))
{
    (void)drv;

    struct mmc_spi_unit *unit = (struct mmc_spi_unit *)dev->driver_data;
    if (!unit)
        return;

    /* Remove from the units list */
    ObtainSemaphore(&units_lock);
    Remove(&unit->node);
    ReleaseSemaphore(&units_lock);

    /* Remove card-change interrupt */
    if (unit->card_change_virq >= 0)
    {
        IRQ_Disable(DDMBase, unit->card_change_virq);
        IRQ_RemoveHandler(DDMBase, unit->card_change_virq, &unit->card_change_int);
        IRQ_DisposeMapping(DDMBase, unit->card_change_virq);
        unit->card_change_virq = -1;
    }

    /* Free the card-detect GPIO descriptor */
    if (unit->sd_ctx.cd_gpio)
    {
        GPIO_Free(DDMBase, unit->sd_ctx.cd_gpio);
        unit->sd_ctx.cd_gpio = NULL;
    }

    /* Unregister MMC card */
    if (MmcBase)
        MMC_UnregisterCard(MmcBase, &unit->mmc_card);

    sd_close(&unit->sd_ctx);

    /* Gracefully shut down the background task */
    if (unit->task)
    {
        unit->shutdown_caller = FindTask(NULL);
        unit->task_exited = FALSE;
        SetSignal(0, SIGF_SHUTDOWN);
        Signal(unit->task, SIGF_SHUTDOWN);
        while (!unit->task_exited)
            Wait(SIGF_SHUTDOWN);
        DeleteTask(unit->task);
        unit->task = NULL;
    }

    dev->driver_data = NULL;
    FreeMem(unit, sizeof(*unit));
}

/* LVO -66: DDMDrv_Enumerate — SD cards don't enumerate children */
int32_t mmc_spi_enumerate(struct Library *drv __asm("a6"), struct device *bus __asm("a0"))
{
    (void)drv;
    (void)bus;
    return 0;
}

/* LVO -72: DDMDrv_GetCompatible */
const char **mmc_spi_get_compatible(struct Library *drv __asm("a6"))
{
    (void)drv;
    return mmc_spi_compatible;
}

/* --- Device open / close / expunge --- */

static void open(struct Library *dev __asm("a6"), struct IORequest *ior __asm("a1"), uint32_t unitnum __asm("d0"),
                 uint32_t flags __asm("d1"))
{
    (void)flags;

    DBG_MMC_SPI("MMC-SPI: open\n");

    ior->io_Error = IOERR_OPENFAIL;
    ior->io_Message.mn_Node.ln_Type = NT_REPLYMSG;

    struct mmc_spi_unit *unit = find_unit(unitnum);
    if (!unit)
        return;

    ior->io_Unit = (struct Unit *)unit;
    dev->lib_OpenCnt++;
    ior->io_Error = 0;
}

static BPTR expunge(struct Library *dev __asm("a6"));

static BPTR close(struct Library *dev __asm("a6"), struct IORequest *ior __asm("a1"))
{
    DBG_MMC_SPI("MMC-SPI: close\n");

    ior->io_Device = NULL;
    ior->io_Unit = NULL;

    dev->lib_OpenCnt--;

    if (dev->lib_OpenCnt == 0 && (dev->lib_Flags & LIBF_DELEXP))
        return expunge(dev);

    return 0;
}

static BPTR expunge(struct Library *dev __asm("a6"))
{
    (void)dev;
    /* We stay resident; no expunge. */
    return 0;
}

static int32_t noexec()
{
    return -1;
}

/* --- Device init (called by Resident/AutoInit) --- */

static int16_t device_initialized = FALSE;

static struct Library *init_device(struct ExecBase *sys_base __asm("a6"), BPTR seg_list __asm("a0"),
                                   struct Library *dev __asm("d0"))
{
    SysBase = sys_base;
    saved_seg_list = seg_list;

    dev->lib_Node.ln_Type = NT_DEVICE;
    dev->lib_Node.ln_Name = device_name;
    dev->lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    dev->lib_Version = 1;
    dev->lib_Revision = 0;
    dev->lib_IdString = (void *)id_string;

    DBG_MMC_SPI("MMC-SPI: init device\n");

    if (device_initialized)
    {
        DBG_MMC_SPI("MMC-SPI: already initialized, skipping re-init\n");
        return dev;
    }
    device_initialized = TRUE;

    NewList(&units);
    InitSemaphore(&units_lock);

    DDMBase = (struct DDMBase *)OpenLibrary("ddm.library", 0);
    if (!DDMBase)
        return NULL;

    SpiBase = (struct SpiBase *)OpenLibrary("spi.library", 0);
    if (!SpiBase)
    {
        CloseLibrary((struct Library *)DDMBase);
        DDMBase = NULL;
        return NULL;
    }

    MmcBase = (struct MmcBase *)OpenLibrary("mmc.library", 0);
    if (!MmcBase)
    {
        CloseLibrary((struct Library *)SpiBase);
        SpiBase = NULL;
        CloseLibrary((struct Library *)DDMBase);
        DDMBase = NULL;
        return NULL;
    }

    mmc_spi_driver.node.ln_Name = "mmc-spi";
    mmc_spi_driver.compatible = mmc_spi_compatible;
    mmc_spi_driver.lib_base = dev;
    mmc_spi_driver.bus_type = BUS_TYPE_SPI;

    if (!DDM_RegisterDriver(DDMBase, &mmc_spi_driver))
        return NULL;

    return dev;
}

/* --- Vector table and auto-init --- */

static uint32_t device_vectors[] = {
    (uint32_t)open,                   /* -6  */
    (uint32_t)close,                  /* -12 */
    (uint32_t)expunge,                /* -18 */
    (uint32_t)noexec,                 /* -24 */
    (uint32_t)begin_io,               /* -30 */
    (uint32_t)abort_io,               /* -36 */
    (uint32_t)mmc_spi_probe,          /* -42 */
    (uint32_t)mmc_spi_remove,         /* -48 */
    (uint32_t)noexec,                 /* -54 (reserved) */
    (uint32_t)mmc_spi_shutdown,       /* -60 */
    (uint32_t)mmc_spi_enumerate,      /* -66 */
    (uint32_t)mmc_spi_get_compatible, /* -72 */
    -1,
};

uint32_t auto_init_tables[] = {
    sizeof(struct Library),
    (uint32_t)device_vectors,
    0,
    (uint32_t)init_device,
};

const struct Resident romtag = {
    .rt_MatchWord = RTC_MATCHWORD,
    .rt_MatchTag = (void *)&romtag,
    .rt_EndSkip = (void *)(&romtag + 1),
    .rt_Flags = RTF_AUTOINIT,
    .rt_Version = 1,
    .rt_Type = NT_DEVICE,
    .rt_Pri = 0,
    .rt_Name = device_name,
    .rt_IdString = id_string,
    .rt_Init = auto_init_tables,
};
