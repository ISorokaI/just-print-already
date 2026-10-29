/*
 * rastertozjs.c — CUPS raster filter for ZjStream printers
 *
 * Converts CUPS raster input to Zenographics ZjStream format with JBIG
 * compression. Supports multiple printer models:
 *
 *   Model 0 (2300DL):  Minolta/QMS 2300DL, 2200DL, 2430DL, HP LJ 1000/1005
 *   Model 1 (HP1020):  HP LaserJet 1018, 1020, 1022, M1319, P2035
 *   Model 2 (HP Pro):  HP LaserJet Pro P1102, P1102w, P1566, P1606dn
 *   Model 3 (HP Pro CP): HP LaserJet Pro CP1025nw (color)
 *   Model 4 (HP M1132): HP LaserJet Professional M1132 MFP (hp-m1132-arm64)
 *
 * Pipeline: PDF → cgpdftoraster (macOS built-in) → rastertozjs → printer
 *
 * Based on foo2zjs from the foo2zjs project by Rick Richardson.
 * Rewritten as a native CUPS raster filter.
 *
 * License: GPL v2 or later (same as foo2zjs)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <cups/cups.h>
#include <cups/raster.h>
#include <cups/ppd.h>
#include "jbig.h"
#include "zjs.h"

/* Model variants */
#define MODEL_2300DL    0
#define MODEL_HP1020    1
#define MODEL_HP_PRO    2
#define MODEL_HP_PRO_CP 3
/* hp-m1132-arm64: the M1132 stalls the USB pipe on the Model 2 stream.
 * Model 4 copies what HP's own Mac filter sends: PJL with CRLF and
 * ENTER LANGUAGE=ZJS, START_DOC with DMCOLLATE only, page items in ID order
 * with ECONOMODE, no RET and no START_PLANE/END_PLANE (Phase 5). */
#define MODEL_HP_M1132  4

/* hp-m1132-arm64: the model comes from *cupsModelNumber in the PPD ($PPD);
 * DEFAULT_MODEL is only the fallback when there is no PPD (offline tests). */
#ifndef DEFAULT_MODEL
#define DEFAULT_MODEL   MODEL_HP_M1132
#endif
#if DEFAULT_MODEL < MODEL_2300DL || DEFAULT_MODEL > MODEL_HP_M1132
#error "DEFAULT_MODEL must be 0..4"
#endif

/* hp-m1132-arm64: the ppd* API is deprecated in libcups but still the only
 * way for a raster filter to see the queue's PPD defaults. */
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

/* ---------- ZJS output helpers ---------- */

static void
chunk_write_rsvd(unsigned long type, unsigned int rsvd,
                 unsigned long items, unsigned long size, FILE *fp)
{
    ZJ_HEADER chunk;
    chunk.type = be32(type);
    chunk.items = be32(items);
    chunk.size = be32(sizeof(ZJ_HEADER) + size);
    chunk.reserved = be16(rsvd);
    chunk.signature = 0x5a5a;
    if (fwrite(&chunk, 1, sizeof(ZJ_HEADER), fp) == 0)
    {
        fprintf(stderr, "ERROR: rastertozjs: chunk_write failed\n");
        exit(1);
    }
}

static void
chunk_write(unsigned long type, unsigned long items, unsigned long size,
            FILE *fp)
{
    chunk_write_rsvd(type, 0, items, size, fp);
}

static void
item_uint32_write(unsigned short item, unsigned long value, FILE *fp)
{
    ZJ_ITEM_UINT32 rec;
    rec.header.size = be32(sizeof(ZJ_ITEM_UINT32));
    rec.header.item = be16(item);
    rec.header.type = ZJIT_UINT32;
    rec.header.param = 0;
    rec.value = be32(value);
    if (fwrite(&rec, 1, sizeof(ZJ_ITEM_UINT32), fp) == 0)
    {
        fprintf(stderr, "ERROR: rastertozjs: item_uint32_write failed\n");
        exit(1);
    }
}

/* ---------- JBIG compressed data chain ---------- */

typedef struct _BIE_CHAIN {
    unsigned char   *data;
    size_t          len;
    struct _BIE_CHAIN *next;
} BIE_CHAIN;

static void
free_chain(BIE_CHAIN *chain)
{
    BIE_CHAIN *next;
    next = chain;
    while ((chain = next))
    {
        next = chain->next;
        if (chain->data)
            free(chain->data);
        free(chain);
    }
}

static long JbgOptions[5] = {
    JBG_ILEAVE | JBG_SMID,
    JBG_DELAY_AT | JBG_LRLTWO | JBG_TPDON | JBG_TPBON | JBG_DPON,
    128,
    16,
    0
};

static void
output_jbig(unsigned char *start, size_t len, void *cbarg)
{
    BIE_CHAIN *current, **root = (BIE_CHAIN **)cbarg;
    int size = 65536;

    if (*root == NULL)
    {
        *root = malloc(sizeof(BIE_CHAIN));
        if (!*root)
        {
            fprintf(stderr, "ERROR: rastertozjs: malloc failed\n");
            exit(1);
        }
        (*root)->data = NULL;
        (*root)->next = NULL;
        (*root)->len = 0;
        size = 20;
    }

    current = *root;
    while (current->next)
        current = current->next;

    while (len > 0)
    {
        int amt, left;

        if (!current->data)
        {
            current->data = malloc(size);
            if (!current->data)
            {
                fprintf(stderr, "ERROR: rastertozjs: malloc failed\n");
                exit(1);
            }
        }

        left = size - current->len;
        amt = (len > (size_t)left) ? left : (int)len;
        memcpy(current->data + current->len, start, amt);
        current->len += amt;
        len -= amt;
        start += amt;

        if (current->len == (size_t)size)
        {
            current->next = malloc(sizeof(BIE_CHAIN));
            if (!current->next)
            {
                fprintf(stderr, "ERROR: rastertozjs: malloc failed\n");
                exit(1);
            }
            current = current->next;
            current->data = NULL;
            current->next = NULL;
            current->len = 0;
        }
    }
}

/* ---------- ZJS page/plane output ---------- */

#define PADTO   4
#define EXTRA_PAD 16

/* A usable chain starts with the 20-byte BIH and has at least one BID */
static int
chain_valid(const BIE_CHAIN *root)
{
    return root && root->next && root->len == 20;
}

static void
write_plane(int planeNum, BIE_CHAIN **root, FILE *fp, int model)
{
    BIE_CHAIN *current;
    BIE_CHAIN *next;
    int i, len, pad_len;

    if (planeNum)
    {
        if (model == MODEL_HP_PRO_CP)
            chunk_write_rsvd(ZJT_START_PLANE, 1 * 12,
                             1, 1 * sizeof(ZJ_ITEM_UINT32), fp);
        else
            chunk_write(ZJT_START_PLANE, 1, 1 * sizeof(ZJ_ITEM_UINT32), fp);
        item_uint32_write(ZJI_PLANE, planeNum, fp);
    }

    for (current = *root; current && current->len; current = current->next)
    {
        if (current == *root)
        {
            chunk_write(ZJT_JBIG_BIH, 0, current->len, fp);
            fwrite(current->data, 1, current->len, fp);
        }
        else
        {
            len = current->len;
            next = current->next;
            if (!next || !next->len)
                pad_len = EXTRA_PAD + PADTO * ((len + PADTO - 1) / PADTO) - len;
            else
                pad_len = 0;
            chunk_write(ZJT_JBIG_BID, 0, len + pad_len, fp);
            fwrite(current->data, 1, len, fp);
            for (i = 0; i < pad_len; i++)
                putc(0, fp);
        }
    }

    free_chain(*root);

    /* END_JBIG */
    switch (model)
    {
    case MODEL_2300DL:
    case MODEL_HP1020:
        chunk_write(ZJT_END_JBIG, 0, 0, fp);
        break;
    case MODEL_HP_PRO:
    case MODEL_HP_PRO_CP:
    case MODEL_HP_M1132:
        chunk_write(ZJT_END_JBIG, 0, 0, fp);
        break;
    }

    /* END_PLANE */
    if (planeNum)
    {
        if (model == MODEL_HP_PRO_CP)
        {
            int nitems = 1;
            chunk_write_rsvd(ZJT_END_PLANE, nitems * 12,
                             nitems, nitems * sizeof(ZJ_ITEM_UINT32), fp);
            item_uint32_write(ZJI_PLANE, planeNum, fp);
        }
        else
        {
            chunk_write(ZJT_END_PLANE, 0, 0, fp);
        }
    }
}

static void
start_doc(FILE *fp, int model, int density, int economode, const char *title)
{
    char header[4] = "JZJZ";
    int nitems;
    int size;
    time_t now;
    struct tm *tmp;
    char datetime[14 + 1];

    /* hp-m1132-arm64: HP's PJL for the M1132 (CRLF, explicit ZJS) */
    if (model == MODEL_HP_M1132)
    {
        char name[64];
        size_t n = 0;

        for (; title && *title && n < sizeof(name) - 1; title++)
            if (*title >= 0x20 && *title < 0x7f && *title != '"')
                name[n++] = *title;
        name[n] = '\0';

        fprintf(fp, "\033%%-12345X@PJL USTATUS JOB=ON\r\n");
        fprintf(fp, "@PJL USTATUS PAGE=ON\r\n");
        fprintf(fp, "@PJL USTATUS DEVICE=ON\r\n");
        fprintf(fp, "@PJL INFO STATUS\r\n");
        /* Always open the job so the EOJ in end_doc has a matching JOB */
        if (n)
            fprintf(fp, "@PJL JOB NAME=%s\r\n", name);
        else
            fprintf(fp, "@PJL JOB\r\n");
        fprintf(fp, "@PJL SET DENSITY = %d\r\n", density);
        fprintf(fp, "@PJL SET JAMRECOVERY = OFF\r\n");
        fprintf(fp, "@PJL ENTER LANGUAGE=ZJS\r\n");
    }

    /* PJL preamble for HP models */
    if (model == MODEL_HP1020 || model == MODEL_HP_PRO ||
        model == MODEL_HP_PRO_CP)
    {
        now = time(NULL);
        tmp = localtime(&now);
        strftime(datetime, sizeof(datetime), "%Y%m%d%H%M%S", tmp);

        fprintf(fp, "\033%%-12345X@PJL JOB\n");
        fprintf(fp, "@PJL SET JAMRECOVERY=OFF\n");
        fprintf(fp, "@PJL SET DENSITY=%d\n", density);
        fprintf(fp, "@PJL SET ECONOMODE=%s\n", economode ? "ON" : "OFF");
        fprintf(fp, "@PJL SET RET=MEDIUM\n");
        fprintf(fp, "@PJL INFO STATUS\n");
        fprintf(fp, "@PJL USTATUS DEVICE = ON\n");
        fprintf(fp, "@PJL USTATUS JOB = ON\n");
        fprintf(fp, "@PJL USTATUS PAGE = ON\n");
        fprintf(fp, "@PJL USTATUS TIMED = 30\n");
        fprintf(fp, "@PJL SET JOBATTR=\"JobAttr4=%s\"", datetime);
        fputc(0, fp);
        fprintf(fp, "\033%%-12345X");
    }

    fwrite(header, 1, sizeof(header), fp);

    /* START_DOC */
    nitems = 1; /* ZJI_DMCOLLATE always */
    switch (model)
    {
    case MODEL_2300DL:
        nitems += 3; /* DMDUPLEX, PAGECOUNT, QUANTITY */
        break;
    case MODEL_HP1020:
    case MODEL_HP_PRO_CP:
        nitems += 2; /* DMDUPLEX, PAGECOUNT */
        break;
    case MODEL_HP_PRO:
        nitems += 1; /* DMDUPLEX (simplex only) */
        break;
    }
    size = nitems * sizeof(ZJ_ITEM_UINT32);

    switch (model)
    {
    case MODEL_2300DL:
        chunk_write(ZJT_START_DOC, nitems, size, fp);
        break;
    case MODEL_HP1020:
        chunk_write_rsvd(ZJT_START_DOC, 0x24, nitems, size, fp);
        break;
    case MODEL_HP_PRO:
    case MODEL_HP_PRO_CP:
    case MODEL_HP_M1132:
        chunk_write_rsvd(ZJT_START_DOC, nitems * 0x0c, nitems, size, fp);
        break;
    }

    item_uint32_write(ZJI_DMCOLLATE, 0, fp);

    switch (model)
    {
    case MODEL_2300DL:
    case MODEL_HP1020:
    case MODEL_HP_PRO_CP:
        item_uint32_write(ZJI_DMDUPLEX, 1, fp); /* off */
        item_uint32_write(ZJI_PAGECOUNT, 0, fp);
        break;
    case MODEL_HP_PRO:
        item_uint32_write(ZJI_DMDUPLEX, 1, fp); /* off */
        break;
    }

    if (model == MODEL_2300DL)
        item_uint32_write(ZJI_QUANTITY, 1, fp);
}

static void
end_doc(FILE *fp, int model)
{
    chunk_write(ZJT_END_DOC, 0, 0, fp);

    if (model == MODEL_HP1020 || model == MODEL_HP_PRO ||
        model == MODEL_HP_PRO_CP)
    {
        fprintf(fp, "\033%%-12345X@PJL EOJ\n");
        fprintf(fp, "\033%%-12345X");
    }
    else if (model == MODEL_HP_M1132)
    {
        fprintf(fp, "\033%%-12345X@PJL EOJ\r\n");
        fprintf(fp, "\033%%-12345X");
    }
}

static void
start_page(FILE *fp, int model, int resX, int resY, int bpp,
           unsigned long rasterW, unsigned long rasterH,
           int realWidth,
           int paperCode, int mediaCode, int sourceCode,
           int copies, int economode)
{
    int nitems = 12;

    /* hp-m1132-arm64: same items and order as HP's filter */
    if (model == MODEL_HP_M1132)
    {
        nitems = 13;
        chunk_write_rsvd(ZJT_START_PAGE, nitems * 12, nitems,
                         nitems * sizeof(ZJ_ITEM_UINT32), fp);
        item_uint32_write(ZJI_DMPAPER, paperCode, fp);
        item_uint32_write(ZJI_DMCOPIES, copies, fp);
        item_uint32_write(ZJI_DMDEFAULTSOURCE, sourceCode, fp);
        item_uint32_write(ZJI_DMMEDIATYPE, mediaCode, fp);
        item_uint32_write(ZJI_NBIE, 1, fp);
        item_uint32_write(ZJI_RESOLUTION_X, resX, fp);
        item_uint32_write(ZJI_RESOLUTION_Y, resY, fp);
        item_uint32_write(ZJI_RASTER_X, realWidth, fp);
        item_uint32_write(ZJI_RASTER_Y, rasterH, fp);
        item_uint32_write(ZJI_VIDEO_BPP, bpp, fp);
        item_uint32_write(ZJI_VIDEO_X, realWidth / bpp, fp);
        item_uint32_write(ZJI_VIDEO_Y, rasterH, fp);
        item_uint32_write(ZJI_ECONOMODE, economode, fp);
        return;
    }

    switch (model)
    {
    case MODEL_2300DL:
        nitems += 4 + 1; /* custom x/y, minolta page#, economode */
        chunk_write(ZJT_START_PAGE, nitems,
                    nitems * sizeof(ZJ_ITEM_UINT32), fp);
        break;
    case MODEL_HP1020:
        nitems += 1; /* economode */
        chunk_write_rsvd(ZJT_START_PAGE, 0x9c, nitems,
                         nitems * sizeof(ZJ_ITEM_UINT32), fp);
        break;
    case MODEL_HP_PRO:
        nitems += 1 + 1; /* RET + economode (if set) */
        if (!economode) nitems -= 1;
        chunk_write_rsvd(ZJT_START_PAGE, nitems * 12, nitems,
                         nitems * sizeof(ZJ_ITEM_UINT32), fp);
        break;
    case MODEL_HP_PRO_CP:
        nitems += 1; /* PLANE */
        if (economode) nitems += 1;
        chunk_write_rsvd(ZJT_START_PAGE, nitems * 12, nitems,
                         nitems * sizeof(ZJ_ITEM_UINT32), fp);
        break;
    }

    if (model == MODEL_HP_PRO_CP)
        item_uint32_write(ZJI_PLANE, 1, fp); /* nbie=1 for mono */

    if (model == MODEL_2300DL || model == MODEL_HP1020 || economode)
        item_uint32_write(ZJI_ECONOMODE, economode, fp);

    item_uint32_write(ZJI_VIDEO_X, realWidth / bpp, fp);
    item_uint32_write(ZJI_VIDEO_Y, rasterH, fp);
    item_uint32_write(ZJI_VIDEO_BPP, bpp, fp);

    if (model == MODEL_HP_PRO_CP)
        item_uint32_write(ZJI_RASTER_X, rasterW, fp);
    else
        item_uint32_write(ZJI_RASTER_X, realWidth, fp);

    item_uint32_write(ZJI_RASTER_Y, rasterH, fp);
    item_uint32_write(ZJI_NBIE, 1, fp); /* mono */
    item_uint32_write(ZJI_RESOLUTION_X, resX, fp);
    item_uint32_write(ZJI_RESOLUTION_Y, resY, fp);

    if (model == MODEL_HP_PRO)
        item_uint32_write(ZJI_RET, 1, fp);

    item_uint32_write(ZJI_DMDEFAULTSOURCE, sourceCode, fp);
    item_uint32_write(ZJI_DMCOPIES, copies, fp);
    item_uint32_write(ZJI_DMPAPER, paperCode, fp);
    item_uint32_write(ZJI_DMMEDIATYPE, mediaCode, fp);

    if (model == MODEL_2300DL)
    {
        item_uint32_write(ZJI_MINOLTA_CUSTOM_X, 0, fp);
        item_uint32_write(ZJI_MINOLTA_CUSTOM_Y, 0, fp);
        static int pageno = 0;
        item_uint32_write(ZJI_MINOLTA_PAGE_NUMBER, ++pageno, fp);
    }
}

static void
end_page(FILE *fp, int model)
{
    switch (model)
    {
    case MODEL_2300DL:
    case MODEL_HP1020:
    case MODEL_HP_PRO:
    case MODEL_HP_M1132:
        chunk_write(ZJT_END_PAGE, 0, 0, fp);
        break;
    case MODEL_HP_PRO_CP:
        chunk_write(ZJT_END_PAGE, 0, 0, fp);
        break;
    }
}

/* ---------- Mapping functions ---------- */

static int
map_paper_code(const char *name)
{
    if (!name || !name[0])
        return DMPAPER_LETTER;

    if (strcmp(name, "Letter") == 0)       return DMPAPER_LETTER;
    if (strcmp(name, "Legal") == 0)        return DMPAPER_LEGAL;
    if (strcmp(name, "Executive") == 0)    return DMPAPER_EXECUTIVE;
    if (strcmp(name, "A4") == 0)           return DMPAPER_A4;
    if (strcmp(name, "A5") == 0)           return DMPAPER_A5;
    if (strcmp(name, "A6") == 0)           return 70;
    if (strcmp(name, "B5") == 0)           return DMPAPER_B5;
    if (strcmp(name, "Env10") == 0)        return DMPAPER_ENV_10;
    if (strcmp(name, "EnvDL") == 0)        return DMPAPER_ENV_DL;
    if (strcmp(name, "EnvC5") == 0)        return DMPAPER_ENV_C5;
    if (strcmp(name, "EnvB5") == 0)        return DMPAPER_ENV_B5;
    if (strcmp(name, "EnvMonarch") == 0)   return DMPAPER_ENV_MONARCH;
    if (strcmp(name, "Postcard") == 0)     return DMPAPER_JAPANESE_POSTCARD;
    if (strcmp(name, "FanFoldGermanLegal") == 0) return DMPAPER_FANFOLD_LGL_GERMAN;

    return DMPAPER_LETTER;
}

static int
map_media_code(const char *media, unsigned cups_media_type, int model)
{
    /* hp-m1132-arm64: the PPD sets cupsMediaType to HP's own codes */
    if (model >= MODEL_HP1020 && cups_media_type > 0)
        return cups_media_type;

    if (!media || !media[0])
        return DMMEDIA_STANDARD;

    if (strcmp(media, "Plain") == 0)         return DMMEDIA_STANDARD;
    if (strcmp(media, "Transparency") == 0)  return DMMEDIA_TRANSPARENCY;
    if (strcmp(media, "Glossy") == 0)        return DMMEDIA_GLOSSY;

    /* Extended codes for HP models (z1/z2/z3) */
    if (model >= MODEL_HP1020)
    {
        if (strcmp(media, "Envelope") == 0)      return 267;
        if (strcmp(media, "Letterhead") == 0)    return 513;
        if (strcmp(media, "ThickStock") == 0)    return 261;
        if (strcmp(media, "Cardstock") == 0)     return 261;
        if (strcmp(media, "Labels") == 0)        return 265;
        if (strcmp(media, "Rough") == 0)         return 263;
        if (strcmp(media, "MidWeight") == 0)     return 282;
        if (strcmp(media, "Heavy") == 0)         return 262;
        if (strcmp(media, "ExtraHeavy") == 0)    return 283;
        if (strcmp(media, "Light") == 0)         return 258;
        if (strcmp(media, "Vellum") == 0)        return 273;
        if (strcmp(media, "Color") == 0)         return 512;
        if (strcmp(media, "Preprinted") == 0)    return 514;
        if (strcmp(media, "Prepunched") == 0)    return 515;
        if (strcmp(media, "Recycled") == 0)      return 516;
    }
    else
    {
        /* Model 0 (2300DL/Minolta) codes */
        if (strcmp(media, "Envelope") == 0)      return 0x101;
        if (strcmp(media, "Letterhead") == 0)    return 0x103;
        if (strcmp(media, "ThickStock") == 0)    return 0x105;
        if (strcmp(media, "Labels") == 0)        return 0x107;
        if (strcmp(media, "Postcard") == 0)      return 0x106;
    }

    return DMMEDIA_STANDARD;
}

static int
map_source_code(unsigned media_position)
{
    switch (media_position)
    {
    case 1:  return DMBIN_UPPER;
    case 2:  return DMBIN_LOWER;
    case 4:  return DMBIN_MANUAL;
    default: return DMBIN_AUTO;
    }
}

/* ---------- CUPS options parsing ---------- */

static int
is_true(const char *val)
{
    return val && (!strcasecmp(val, "true") || !strcasecmp(val, "on") ||
                   !strcasecmp(val, "yes") || !strcmp(val, "1"));
}

static void
set_density(const char *val, int *density)
{
    int d = val ? atoi(val) : 0;
    if (d >= 1 && d <= 5)
        *density = d;
}

/* hp-m1132-arm64: start from the PPD defaults (CUPS only puts explicitly
 * set job options in argv[5]), then apply the job options on top. */
static void
parse_options(const char *options, int *density, int *economode, int *model,
              int *collate)
{
    cups_option_t *opts = NULL;
    int num_opts;
    const char *val;
    ppd_file_t *ppd;
    ppd_choice_t *choice;

    num_opts = cupsParseOptions(options ? options : "", 0, &opts);

    if ((val = getenv("PPD")) != NULL && (ppd = ppdOpenFile(val)) != NULL)
    {
        ppdMarkDefaults(ppd);
        cupsMarkOptions(ppd, num_opts, opts);

        if (ppd->model_number >= MODEL_2300DL &&
            ppd->model_number <= MODEL_HP_M1132)
            *model = ppd->model_number;
        if ((choice = ppdFindMarkedChoice(ppd, "Density")) != NULL)
            set_density(choice->choice, density);
        if ((choice = ppdFindMarkedChoice(ppd, "EconoMode")) != NULL)
            *economode = is_true(choice->choice);
        if ((choice = ppdFindMarkedChoice(ppd, "Collate")) != NULL)
            *collate = is_true(choice->choice);

        ppdClose(ppd);
    }

    /* Job options, also used directly when running without a PPD */
    set_density(cupsGetOption("Density", num_opts, opts), density);

    if ((val = cupsGetOption("EconoMode", num_opts, opts)) != NULL)
        *economode = is_true(val);

    val = cupsGetOption("Quality", num_opts, opts);
    if (val && strcmp(val, "draft") == 0)
        *economode = 1;

    if ((val = cupsGetOption("Collate", num_opts, opts)) != NULL)
        *collate = is_true(val);
    if ((val = cupsGetOption("multiple-document-handling", num_opts, opts)) != NULL)
        *collate = strcmp(val, "separate-documents-uncollated-copies") != 0;

    val = cupsGetOption("Model", num_opts, opts);
    if (val)
    {
        int m = atoi(val);
        if (m >= 0 && m <= MODEL_HP_M1132)
            *model = m;
    }

    cupsFreeOptions(num_opts, opts);
}

/* ---------- Signal handling ---------- */

static volatile int Canceled = 0;

static void
cancel_handler(int sig)
{
    (void)sig;
    Canceled = 1;
}

/* ---------- Main ---------- */

int
main(int argc, char *argv[])
{
    cups_raster_t       *ras;
    cups_page_header2_t header;
    int                 fd;
    int                 page = 0;
    int                 failed = 0;
    int                 density = 3;
    int                 economode = 0;
    int                 model = DEFAULT_MODEL;
    int                 collate = 0;
    int                 job_copies;
    FILE                *pfp = stdout;  /* where pages go */
    char                *docbuf = NULL; /* collated copies: pages in memory */
    size_t              docsize = 0;
    int                 doc_copies = 1;

    if (argc < 6 || argc > 7)
    {
        fprintf(stderr, "Usage: %s job user title copies options [file]\n",
                argv[0]);
        return 1;
    }

    signal(SIGTERM, cancel_handler);
    signal(SIGPIPE, SIG_IGN);

    parse_options(argv[5], &density, &economode, &model, &collate);
    job_copies = atoi(argv[4]);

    if (argc == 7)
    {
        fd = open(argv[6], O_RDONLY);
        if (fd < 0)
        {
            fprintf(stderr, "ERROR: rastertozjs: cannot open %s\n", argv[6]);
            return 1;
        }
    }
    else
    {
        fd = 0;
    }

    ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    if (!ras)
    {
        fprintf(stderr, "ERROR: rastertozjs: cannot open raster stream\n");
        if (fd != 0) close(fd);
        return 1;
    }

    start_doc(stdout, model, density, economode, argv[3]);

    while (!Canceled && cupsRasterReadHeader2(ras, &header))
    {
        unsigned int    cupsW, cupsH, cupsBpl;
        int             resX, resY, bpp;
        int             jbigW, realWidth;
        int             bpl, bpl16;
        unsigned char   *buf;
        unsigned char   *bitmaps[1];
        struct jbg_enc_state se;
        BIE_CHAIN       *chain = NULL;
        int             paperCode, mediaCode, sourceCode;
        unsigned int    y;
        int             invert;
        int             copies;

        page++;

        cupsW   = header.cupsWidth;
        cupsH   = header.cupsHeight;
        cupsBpl = header.cupsBytesPerLine;
        resX    = header.HWResolution[0];
        resY    = header.HWResolution[1];
        /* hp-m1132-arm64: with cupsManualCopies False the count may only
         * be in argv[4]; never multiply the two. */
        copies  = header.NumCopies > 1 ? (int)header.NumCopies
                : job_copies > 1 ? job_copies : 1;

        /* The printer only makes uncollated copies (1,1,2,2), so for
         * collated copies keep the pages in memory and repeat them. */
        if (page == 1 && collate && copies > 1)
        {
            pfp = open_memstream(&docbuf, &docsize);
            if (pfp)
                doc_copies = copies;
            else
            {
                fprintf(stderr, "ERROR: rastertozjs: cannot buffer collated copies\n");
                pfp = stdout;
            }
        }

        fprintf(stderr, "DEBUG: rastertozjs: page %d, %ux%u pixels, %dx%d dpi, model %d\n",
                page, cupsW, cupsH, resX, resY, model);

        if (header.cupsBitsPerPixel != 1)
        {
            fprintf(stderr, "ERROR: rastertozjs: expected 1bpp, got %d\n",
                    header.cupsBitsPerPixel);
            break;
        }

        invert = (header.cupsColorSpace == CUPS_CSPACE_W);

        bpp = resX / 600;
        if (bpp < 1) bpp = 1;

        paperCode  = map_paper_code(header.cupsPageSizeName);
        mediaCode  = map_media_code(header.MediaType, header.cupsMediaType, model);
        sourceCode = map_source_code(header.MediaPosition);

        realWidth = cupsW;
        jbigW = (cupsW + 127) & ~127;
        bpl   = (jbigW + 7) / 8;
        bpl16 = (bpl + 15) & ~15;

        buf = calloc(bpl16, cupsH);
        if (!buf)
        {
            fprintf(stderr, "ERROR: rastertozjs: cannot allocate page buffer\n");
            break;
        }

        for (y = 0; y < cupsH && !Canceled; y++)
        {
            unsigned int n = cupsRasterReadPixels(ras,
                                                  buf + (size_t)y * bpl16,
                                                  cupsBpl);
            if (n == 0) break;

            if (invert)
            {
                unsigned int i;
                unsigned char *row = buf + (size_t)y * bpl16;
                for (i = 0; i < cupsBpl; i++)
                    row[i] ^= 0xFF;
            }
        }

        if (Canceled)
        {
            free(buf);
            break;
        }

        *bitmaps = buf;
        jbg_enc_init(&se, jbigW, cupsH, 1, bitmaps, output_jbig, &chain);
        jbg_enc_options(&se, JbgOptions[0], JbgOptions[1],
                        JbgOptions[2], JbgOptions[3], JbgOptions[4]);
        jbg_enc_out(&se);
        jbg_enc_free(&se);

        /* Check before START_PAGE so a bad page never reaches the printer */
        if (!chain_valid(chain))
        {
            fprintf(stderr, "ERROR: rastertozjs: invalid JBIG data on page %d\n",
                    page);
            free_chain(chain);
            free(buf);
            failed = 1;
            break;
        }

        start_page(pfp, model, 600, resY, bpp,
                   jbigW, cupsH, realWidth,
                   paperCode, mediaCode, sourceCode,
                   pfp != stdout ? 1 : copies, economode);
        /* hp-m1132-arm64: no START_PLANE/END_PLANE on the M1132 */
        write_plane(model == MODEL_HP_M1132 ? 0 : 4, &chain, pfp, model);
        end_page(pfp, model);

        free(buf);

        fprintf(stderr, "PAGE: %d %d\n", page,
                pfp != stdout ? doc_copies : copies);
    }

    if (pfp != stdout)
    {
        int c;

        fclose(pfp);
        fprintf(stderr, "DEBUG: rastertozjs: %d collated copies of %d page(s)\n",
                doc_copies, page);
        for (c = 0; c < doc_copies && !Canceled; c++)
            fwrite(docbuf, 1, docsize, stdout);
        free(docbuf);
    }

    end_doc(stdout, model);

    cupsRasterClose(ras);
    if (fd != 0) close(fd);

    return (Canceled || failed) ? 1 : 0;
}
