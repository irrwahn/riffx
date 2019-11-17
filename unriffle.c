/*
 * Copyright 2019 Urban Wallasch <irrwahn35@freenet.de>
 *
 * Licensed under the terms of the 0BSD ("Zero-clause BSD") license.
 * See LICENSE file for details.
 */

/*
 * Sequentially dump the chunks contained in a RIFF/RIFX file to stdout.
 *
 * A tiny subset of chunk types that may be present in some audio files
 * is given a special treatment to make them more readable to mere humans.
 * This may help to get an idea about the kind of data contained.
 *
 */

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static struct {
    FILE *dump_fp;
    FILE *log_fp;
    int endianess;
} cfg = {
    NULL,
    NULL,
    0,
};

typedef uint8_t fcc_t[4];

#define FOURCC_IS(p_,q_) (!memcmp((const void *)(p_),(const void *)(q_),4))

typedef
    struct RIFF_chunk_t {
        fcc_t fcc;
        uint32_t csize;
        uint8_t data[];
    }
    RIFF_chunk_t;

static size_t filesize = 0;
static void *fbuf;

static inline size_t boff(const void *p, const void *b) {
    return (size_t)((const uint8_t *)p - (const uint8_t *)b);
}

/* Little/Big Endian to native uint32 conversion: */
static inline uint32_t get_ui32(const void *p) {
    const uint8_t *b = p;
    if (!cfg.endianess)     /* Little Endian byte order (RIFF) */
        return b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24;
    /* Big Endian byte order (RIFX) */
    return b[3] | b[2] << 8 | b[1] << 16 | b[0] << 24;
}

/* Little/Big Endian to native uint16 conversion: */
static inline uint16_t get_ui16(const void *p) {
    const uint8_t *b = p;
    if (!cfg.endianess)     /* Little Endian byte order (RIFF) */
        return b[0] | b[1] << 8;
    /* Big Endian byte order (RIFX) */
    return b[1] | b[0] << 8;
}

/*
 * dump helper:
 */
#define LOG(...)  (fprintf(cfg.log_fp,__VA_ARGS__))
#define DIE(...)  do{LOG(__VA_ARGS__);exit(EXIT_FAILURE);}while(0)
#define DMP(...)  (fprintf(cfg.dump_fp,__VA_ARGS__))

static const char *hdig = "0123456789abcdef";

static inline void xdump(const void *s, size_t n) {
    const unsigned char *p = s;
    char buf[68];
    char *b = buf;
    char *a = buf + 51;
    size_t i = 0;

    memset(buf, ' ', sizeof buf);
    while (i < n) {
        *b++ = hdig[p[i] / 16];
        *b++ = hdig[p[i] % 16];
        *a++ = isgraph(p[i]) ? p[i] : '.';
        ++i;
        if (i % 16 == 0) {
            *a++ = '\0';
            fprintf(cfg.dump_fp, "[%8zu] %14zu: %s\n", boff(p+i-16, fbuf), i - 16, buf);
            memset(buf, ' ', sizeof buf);
            b = buf;
            a = buf + 51;
        }
        else {
            if (i % 8 == 0)
                *b++ = ' ';
            *b++ = ' ';
        }
    }
    if (b != buf) {
        *a++ = '\0';
        fprintf(cfg.dump_fp, "[%8zu] %14zu: %s\n", boff(p+i-(i%16), fbuf), (i - 1) / 16 * 16, buf);
    }
}

static inline void dump4cc(const char *s, fcc_t fcc) {
    fcc_t f;
    for (size_t i = 0; i < sizeof f; i++)
        f[i] = isprint((unsigned char)fcc[i]) ? fcc[i] : '?';
    DMP("[%8zu] %14s: %4.4s\n", boff(fcc, fbuf), s, f);
}

static inline void dump4ccEnd(const void *p, fcc_t fcc) {
    fcc_t f;
    for (size_t i = 0; i < sizeof f; i++)
        f[i] = isprint((unsigned char)fcc[i]) ? fcc[i] : '?';
    DMP("[%8zu]      [%4.4s end]\n", boff(p, fbuf), f);
}

static inline void dumpU16(const char *s, const void *u) {
    DMP("[%8zu] %14s: %"PRIu16"\n", boff(u, fbuf), s, get_ui16(u));
}

static inline void dumpU32(const char *s, const void *u) {
    DMP("[%8zu] %14s: %"PRIu32"\n", boff(u, fbuf), s, get_ui32(u));
}

static inline void dumpStr(const char *s, const void *u) {
    DMP("[%8zu] %14s: %s\n", boff(u, fbuf), s, (const char *)u);
}

static int rdump(void *p, size_t fsize) {
    RIFF_chunk_t *r = p;
    uint32_t sz;

    if (fsize < 8)
        return 0;
    sz = get_ui32(&r->csize);
    if (sz < 2)
        return 0;
    if (sz > fsize)
        return -1;
    DMP("\n");
    dump4cc("Chunk ID", r->fcc);
    dumpU32("Size", &r->csize);

    if (FOURCC_IS(&r->fcc, "RIFF") || FOURCC_IS(&r->fcc, "RIFX")) {
        dump4cc("RIFF Type", r->data);
        rdump(r->data + sizeof(fcc_t), sz);
        dump4ccEnd(r->data + sz, r->fcc);
        if (fsize > sz + 8 ) {
            DMP("\nExtra Bytes at end of file:\n");
            xdump(r->data + sz, fsize - (sz + 8));
        }
        return 0;
    }
    else if (FOURCC_IS(&r->fcc, "LIST")) {
        dump4cc("Form Type", r->data);
        rdump(r->data + sizeof(fcc_t), sz - sizeof(fcc_t));
    }
    else if (FOURCC_IS(&r->fcc, "labl") || FOURCC_IS(&r->fcc, "note")) {
        dumpU32("Cue Point ID", r->data);
        dumpStr("Label Text", r->data + 4);
        fsize -= 8;
        if (sz % 2)  /* Take care of padding. */
            ++sz;
    }
    else if (FOURCC_IS(&r->fcc, "cue ")) {
        uint32_t cn = get_ui32(r->data);
        dumpU32("# Cue points", &r->data);
        for (uint32_t i = 0; i < cn; ++i) {
            uint8_t *c;
            c = r->data + i * 24 + 4;
            dumpU32("Cue Point ID", c);
            dumpU32("Cue Position", c+4);
            dump4cc("Data Chunk ID", c + 8);
            dumpU32("Chunk Start", c+12);
            dumpU32("Block Start", c+16);
            dumpU32("Sample Offset", c+20);
        }
    }
    else if (FOURCC_IS(&r->fcc, "fmt ")) {
        dumpU16("Compression", r->data);
        dumpU16("Channels", r->data+2);
        dumpU32("Sample Rate", r->data+4);
        dumpU32("Avg. Bytes/s", r->data+8);
        dumpU16("Block align", r->data+12);
        dumpU16("Signif. bit/s", r->data+14 );
        if (sz > 16) {
            dumpU16("Xtra FMT bytes", r->data+16);
            xdump(r->data+18, sz-18);
        }
    }
    else if (sz <= fsize) {
        xdump(r->data, sz);
    }
    dump4ccEnd(r->data + sz, r->fcc);
    return rdump(r->data + sz, fsize - sz);
}

int main(int argc, char *argv[]) {
    FILE *ifp = stdin;
    RIFF_chunk_t *r;
    size_t nrd;

    cfg.dump_fp = stdout;
    cfg.log_fp = stderr;
    if (argc > 1) {
        ifp = fopen(argv[1], "r");
        if (!ifp)
            DIE("fopen %s: %s\n", argv[1], strerror(errno));
        fseek(ifp, 0, SEEK_END);
        filesize = ftell(ifp);
        rewind(ifp);
    }
    fbuf = malloc(filesize);
    if (!fbuf)
        DIE("malloc: %s\n", strerror(errno));
    nrd = fread(fbuf, 1, filesize, ifp);
    if (ferror(ifp) || nrd != filesize)
        DIE("read: %s\n", strerror(errno));
    r = fbuf;

    if (!FOURCC_IS(r->fcc, "RIFF")) {
        if (!FOURCC_IS(r->fcc, "RIFX"))
            DIE("%s is not a RIFF file!\n", argv[1]);
        cfg.endianess = 1;
    }

    DMP("         %14s: %s\n", "File name", argv[1]);
    DMP("         %14s: %zu\n", "File size", filesize);
    DMP("\nBYTE OFFSET         FIELD  VALUE\n");
    rdump(r, filesize);
    free(fbuf);
    exit(EXIT_SUCCESS);
}


