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


/*
 * dump helper:
 */

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
            fprintf(cfg.dump_fp, "%14zu: %s\n", i - 16, buf);
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
        fprintf(cfg.dump_fp, "%14zu: %s\n", (i - 1) / 16 * 16, buf);
    }
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

#define LOG(...)  (fprintf(cfg.log_fp,__VA_ARGS__))
#define DIE(...)  do{LOG(__VA_ARGS__);exit(EXIT_FAILURE);}while(0)
#define DMP(...)  (fprintf(cfg.dump_fp,__VA_ARGS__))

#define FOURCC_IS(p_,q_) (!memcmp((const void *)(p_),(const void *)(q_),4))

typedef uint8_t fcc_t[4];

typedef
    struct RIFF_chunk_t {
        fcc_t fcc;
        uint32_t csize;
        uint8_t data[];
    }
    RIFF_chunk_t;

static inline void dump4cc(const char *s, fcc_t fcc) {
    fcc_t f;
    for (size_t i = 0; i < sizeof f; i++)
        f[i] = isprint((unsigned char)fcc[i]) ? fcc[i] : '?';
    DMP("[4] %14s: ", s);  DMP("%4.4s\n", f);
}

static inline void dump4ccEnd(const char *s, fcc_t fcc) {
    fcc_t f;
    for (size_t i = 0; i < sizeof f; i++)
        f[i] = isprint((unsigned char)fcc[i]) ? fcc[i] : '?';
    DMP("    %14s: [%4.4s end]\n", s, f);
}

static inline void dumpU16(const char *s, const void *u) {
    DMP("[2] %14s: ", s);  DMP("%"PRIu16"\n", get_ui16(u));
}

static inline void dumpU32(const char *s, const void *u) {
    DMP("[4] %14s: ", s);  DMP("%"PRIu32"\n", get_ui32(u));
}

static inline void dumpStr(const char *s, const void *u) {
    DMP("%14s: ", s);  DMP("%s\n", (const char *)u);
}

static int rdump(void *p, size_t fsize) {
    RIFF_chunk_t *r = p;
    uint32_t sz = get_ui32(&r->csize);

    if (!fsize || !sz) {
        return 0;
    }
    DMP("\n");
    dump4cc("Chunk ID", r->fcc);
    dumpU32("Size", &r->csize);
    if (FOURCC_IS(&r->fcc, "RIFF") || FOURCC_IS(&r->fcc, "RIFX")) {
        dump4cc("RIFF Type", r->data);
        rdump(r->data + sizeof(fcc_t), fsize - sizeof(fcc_t));
    }
    else if (FOURCC_IS(&r->fcc, "LIST")) {
        dump4cc("Form Type", r->data);
        rdump(r->data + sizeof(fcc_t), sz - sizeof(fcc_t));
    }
    else if (FOURCC_IS(&r->fcc, "labl")) {
        dumpU32("Label ID", r->data);
        dumpStr("Label Text", r->data + 4);
        sz += 8;
    }
    else if (FOURCC_IS(&r->fcc, "cue ")) {
        uint32_t cn = get_ui32(r->data);
        dumpU32("# Cue points", &r->data);
        for (uint32_t i = 0; i < cn; ++i) {
            uint8_t *c;
            c = r->data + i * 24 + 4;
            dumpU32("Cue ID", c);
            dumpU32("Cue Position", c+4);
            dump4cc("Data Chunk ID", c + 8);
            dumpU32("Chunk Start", c+12);
            dumpU32("Block Start", c+16);
            dumpU32("Sample Offset", c+20);
        }
    }
    else if (FOURCC_IS(&r->fcc, "fmt ")) {
        dumpU16("Compression", r->data);
        dumpU16("# Channels", r->data+2);
        dumpU32("Sample Rate", r->data+4);
        dumpU32("Avg. Bytes/s", r->data+8);
        dumpU16("Block align", r->data+12);
        dumpU16("Signif. bit/s", r->data+14 );
        dumpU16("Xtra FMT bytes", r->data+16);
        xdump(r->data+18, sz-18);
    }
    else if (sz <= fsize) {
        xdump(r->data, sz);
    }
    else {
        /* Garbage beyond expected EOF: */
        xdump(r->data, fsize);
        return -1;
    }
    dump4ccEnd("==============", r->fcc);
    return rdump(r->data + sz, fsize - sz);
}

int main(int argc, char *argv[]) {
    FILE *ifp = stdin;
    size_t fsize = 0;
    void *buf;
    RIFF_chunk_t *r;
    size_t nrd;

    cfg.dump_fp = stdout;
    cfg.log_fp = stderr;
    if (argc > 1) {
        ifp = fopen(argv[1], "r");
        if (!ifp)
            DIE("fopen %s: %s\n", argv[1], strerror(errno));
        fseek(ifp, 0, SEEK_END);
        fsize = ftell(ifp);
        rewind(ifp);
    }
    buf = malloc(fsize);
    if (!buf)
        DIE("malloc: %s\n", strerror(errno));
    nrd = fread(buf, 1, fsize, ifp);
    if (ferror(ifp) || nrd != fsize)
        DIE("read: %s\n", strerror(errno));
    r = buf;

    if (!FOURCC_IS(r->fcc, "RIFF")) {
        if (!FOURCC_IS(r->fcc, "RIFX"))
            DIE("%s is not a RIFF file!\n", argv[1]);
        cfg.endianess = 1;
    }

    DMP("File name: %s\n", argv[1]);
    DMP("File size: %zu\n", fsize);
    rdump(r, fsize);
    free(buf);
    exit(EXIT_SUCCESS);
}


