/*
 * Amiga Forever's RP9 packages: a ZIP file with rp9-manifest.xml (the
 * model, Kickstart, memory, and the disks in their order: <floppy
 * priority="1">game1.adf</floppy> ...) beside the disk images.
 *
 * Read from the file as needed: the central directory for the members,
 * stored or deflated ones (the board's ROM inflater, zlib on the host).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "amiga_internal.h"
#include "rp9.h"

#ifdef ESP_PLATFORM
#include "miniz.h"
#else
#include <zlib.h>
#endif

#define MAX_DIR         (64 * 1024)

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

typedef struct {
    uint32_t local;                 /* the local header's offset */
    uint32_t packed, size;
    uint16_t method;
} member_t;

/* Find a member by name (case not minded) */
static bool find(FILE *f, const char *name, member_t *m)
{
    uint8_t tail[22 + 256];
    if (fseek(f, 0, SEEK_END) != 0) {
        return false;
    }
    long len = ftell(f);
    long back = len < (long)sizeof(tail) ? len : (long)sizeof(tail);
    fseek(f, len - back, SEEK_SET);
    if (fread(tail, 1, back, f) != (size_t)back) {
        return false;
    }
    long eocd = -1;
    for (long i = back - 22; i >= 0; i--) {
        if (le32(tail + i) == 0x06054B50) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        return false;
    }
    uint32_t dir_size = le32(tail + eocd + 12), dir_at = le32(tail + eocd + 16);
    if (dir_size > MAX_DIR) {
        return false;
    }
    uint8_t *dir = malloc(dir_size);
    if (!dir) {
        return false;
    }
    bool found = false;
    fseek(f, dir_at, SEEK_SET);
    if (fread(dir, 1, dir_size, f) == dir_size) {
        size_t nlen = strlen(name);
        for (uint32_t p = 0; p + 46 <= dir_size && le32(dir + p) == 0x02014B50;) {
            uint16_t n = le16(dir + p + 28), extra = le16(dir + p + 30), comment = le16(dir + p + 32);
            if (n == nlen && p + 46 + n <= dir_size && strncasecmp((const char *)dir + p + 46, name, n) == 0) {
                m->method = le16(dir + p + 10);
                m->packed = le32(dir + p + 20);
                m->size = le32(dir + p + 24);
                m->local = le32(dir + p + 42);
                found = true;
                break;
            }
            p += 46 + n + extra + comment;
        }
    }
    free(dir);
    return found;
}

static bool inflate_raw(const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t out_len)
{
#ifdef ESP_PLATFORM
    /* its ~10 KB of state on the heap: inserts run on small stacks (the web server's) */
    tinfl_decompressor *d = malloc(sizeof(tinfl_decompressor));
    if (!d) {
        return false;
    }
    tinfl_init(d);
    size_t in_bytes = in_len, out_bytes = out_len;
    tinfl_status st = tinfl_decompress(d, in, &in_bytes, out, out, &out_bytes, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(d);
    return st == TINFL_STATUS_DONE && out_bytes == out_len;
#else
    z_stream z = { 0 };
    if (inflateInit2(&z, -15) != Z_OK) {
        return false;
    }
    z.next_in = (Bytef *)in;
    z.avail_in = in_len;
    z.next_out = out;
    z.avail_out = out_len;
    int r = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    return r == Z_STREAM_END && z.total_out == out_len;
#endif
}

/* A member's contents into dst (max bytes); its size in *size */
bool rp9_member(const char *path, const char *name, uint8_t *dst, uint32_t max, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    member_t m;
    bool ok = false;
    uint8_t local[30];
    if (find(f, name, &m) && m.size <= max && fseek(f, m.local, SEEK_SET) == 0 &&
        fread(local, 1, 30, f) == 30 && le32(local) == 0x04034B50) {
        fseek(f, m.local + 30 + le16(local + 26) + le16(local + 28), SEEK_SET);
        if (m.method == 0) {
            ok = m.size == m.packed && fread(dst, 1, m.size, f) == m.size;
        } else if (m.method == 8) {
            uint8_t *packed = ami_malloc(m.packed ? m.packed : 1);
            if (packed) {
                ok = fread(packed, 1, m.packed, f) == m.packed && inflate_raw(packed, m.packed, dst, m.size);
                free(packed);
            }
        }
        *size = m.size;
    }
    fclose(f);
    return ok;
}

/* The text of an element (<tag ...>text</tag>) after *from; false at the end */
static bool element(const char *xml, const char *tag, const char **from, char *text, size_t len, int *priority)
{
    char open[32];
    snprintf(open, sizeof(open), "<%s", tag);
    const char *p = *from;
    while ((p = strstr(p, open)) != NULL) {
        char c = p[strlen(open)];
        if (c == '>' || c == ' ') {
            break;
        }
        p++;
    }
    if (!p) {
        return false;
    }
    const char *end = strchr(p, '>');
    if (!end) {
        return false;
    }
    if (priority) {
        const char *pr = strstr(p, "priority=\"");
        *priority = pr && pr < end ? atoi(pr + 10) : 0;
    }
    const char *close = strstr(end, "</");
    if (!close) {
        return false;
    }
    size_t n = (size_t)(close - end - 1);
    n = n < len - 1 ? n : len - 1;
    memcpy(text, end + 1, n);
    text[n] = 0;
    *from = close;
    return true;
}

bool rp9_read(const char *path, rp9_t *r)
{
    memset(r, 0, sizeof(*r));
    char *xml = malloc(32 * 1024);
    if (!xml) {
        return false;
    }
    uint32_t size = 0;
    if (!rp9_member(path, "rp9-manifest.xml", (uint8_t *)xml, 32 * 1024 - 1, &size)) {
        free(xml);
        return false;
    }
    xml[size] = 0;
    const char *p = xml;
    element(xml, "title", &p, r->title, sizeof(r->title), NULL);
    p = xml;
    element(xml, "system", &p, r->system, sizeof(r->system), NULL);
    char rom[16] = "";
    p = xml;
    if (element(xml, "systemrom", &p, rom, sizeof(rom), NULL)) {
        r->rom = atoi(rom);
    }
    /* the floppies by priority */
    int prio[RP9_DISKS];
    p = xml;
    char name[RP9_NAME];
    int pr;
    while (r->disks < RP9_DISKS && element(xml, "floppy", &p, name, sizeof(name), &pr)) {
        int i = r->disks++;
        while (i > 0 && prio[i - 1] > pr) {
            prio[i] = prio[i - 1];
            memcpy(r->disk[i], r->disk[i - 1], RP9_NAME);
            i--;
        }
        prio[i] = pr;
        snprintf(r->disk[i], RP9_NAME, "%s", name);
    }
    p = xml;
    r->harddrive = element(xml, "harddrive", &p, name, sizeof(name), NULL);
    r->turbo = strstr(xml, "<compatibility>turbo-floppy</compatibility>") != NULL;
    free(xml);
    return true;
}
