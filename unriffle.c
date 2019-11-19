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

static inline uint8_t get_ui8(const void *p) {
    return *(const uint8_t *)p;
}

/*
 * dump helper:
 */
#define LOG(...)    (fprintf(cfg.log_fp,__VA_ARGS__))
#define DIE(...)    do{LOG(__VA_ARGS__);exit(EXIT_FAILURE);}while(0)
#define DMP(...)    (fprintf(cfg.dump_fp,__VA_ARGS__))
#define DMPO(p_,b_) DMP("[%8zu] ",(size_t)((const uint8_t *)p_-(const uint8_t *)b_));

static inline void xdump(const void *s, size_t n, const void *basep) {
    static const char *hdig = "0123456789abcdef";
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
            DMPO(p+i-16, basep);
            DMP("%14zu: %s\n", i - 16, buf);
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
        DMPO(p+i-(i%16), basep);
        DMP("%14zu: %s\n", (i - 1) / 16 * 16, buf);
    }
}

static inline void dump4cc(const char *s, fcc_t fcc, const void *basep) {
    fcc_t f;
    for (size_t i = 0; i < sizeof f; i++)
        f[i] = isprint((unsigned char)fcc[i]) ? fcc[i] : '?';
    DMPO(fcc, basep);
    DMP("%14s: '%4.4s'\n", s, f);
}

static inline void dump4ccEnd(const void *p, fcc_t fcc, const void *basep) {
    fcc_t f;
    for (size_t i = 0; i < sizeof f; i++)
        f[i] = isprint((unsigned char)fcc[i]) ? fcc[i] : '?';
    DMPO(p, basep);
    DMP("   ['%4.4s' end]\n", f);
}

static inline void dumpU8(const char *s, const void *u, const void *basep) {
    DMPO(u, basep);
    DMP("%14s: %"PRIu8"\n", s, get_ui8(u));
}

static inline void dumpU16(const char *s, const void *u, const void *basep) {
    DMPO(u, basep);
    DMP("%14s: %"PRIu16"\n", s, get_ui16(u));
}

static inline void dumpU32(const char *s, const void *u, const void *basep) {
    DMPO(u, basep);
    DMP("%14s: %"PRIu32"\n", s, get_ui32(u));
}

static inline void dumpStr(const char *s, const void *u, const void *basep) {
    DMPO(u, basep);
    DMP("%14s: %s\n", s, (const char *)u);
}

static int rdump(void *p, size_t fsize, const void *basep) {
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
    dump4cc("Chunk ID", r->fcc, basep);
    dumpU32("Size", &r->csize, basep);

    if (FOURCC_IS(&r->fcc, "RIFF") || FOURCC_IS(&r->fcc, "RIFX")) {
        dump4cc("RIFF Type", r->data, basep);
        rdump(r->data + sizeof(fcc_t), sz, basep);
        dump4ccEnd(r->data + sz, r->fcc, basep);
        if (fsize > sz + 8 ) {
            DMP("\nExtra Bytes at end of file:\n");
            xdump(r->data + sz, fsize - (sz + 8), basep);
        }
        return 0;
    }
    else if (FOURCC_IS(&r->fcc, "LIST")) {
        dump4cc("Form Type", r->data, basep);
        rdump(r->data + sizeof(fcc_t), sz - sizeof(fcc_t), basep);
    }
    else if (FOURCC_IS(&r->fcc, "labl") || FOURCC_IS(&r->fcc, "note")) {
        dumpU32("Cue Point ID", r->data, basep);
        dumpStr("Label Text", r->data + 4, basep);
        fsize -= 8;
    }
    else if (FOURCC_IS(&r->fcc, "cue ")) {
        uint32_t cn = get_ui32(r->data);
        dumpU32("# Cue points", &r->data, basep);
        for (uint32_t i = 0; i < cn; ++i) {
            uint8_t *c;
            c = r->data + i * 24 + 4;
            dumpU32("Cue Point ID", c, basep);
            dumpU32("Cue Position", c+4, basep);
            dump4cc("Data Chunk ID", c + 8, basep);
            dumpU32("Chunk Start", c+12, basep);
            dumpU32("Block Start", c+16, basep);
            dumpU32("Sample Offset", c+20, basep);
        }
    }
    else if (FOURCC_IS(&r->fcc, "fmt ")) {
        dumpU16("Compression", r->data, basep);
        dumpU16("Channels", r->data+2, basep);
        dumpU32("Sample Rate", r->data+4, basep);
        dumpU32("Avg. Bytes/s", r->data+8, basep);
        dumpU16("Block align", r->data+12, basep);
        dumpU16("Signif. bit/s", r->data+14, basep);
        if (sz > 16) {
            dumpU16("Xtra FMT bytes", r->data+16, basep);
            xdump(r->data+18, sz-18, basep);
        }
    }
    else if (sz <= fsize) {
        xdump(r->data, sz, basep);
    }
    dump4ccEnd(r->data + sz, r->fcc, basep);
    /* Take care of chunk padding: */
    if (sz % 2) {
        dumpU8("Padding Byte", r->data + sz, basep);
        ++sz;
    }
    return rdump(r->data + sz, fsize - sz, basep);
}

int main(int argc, char *argv[]) {
    FILE *ifp = stdin;
    size_t filesize = 0;
    void *fbuf;
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

    DMP("File name: %s\n", argv[1]);
    DMP("File size: %zu\n", filesize);
    DMP("\nBYTE OFFSET         FIELD  VALUE\n");
    rdump(r, filesize, fbuf);
    free(fbuf);
    exit(EXIT_SUCCESS);
}


