/*
 * Original code copyright (c) 2019, volpol.
 *
 * Changes 2019 by irrwahn:
 *  - renamed wsp.c to riffx.c
 *  - replaced mkdir_p with mkdirp
 *  - refactoring, reformatting
 *  - some minor changes and optimizations
 *
 * Traverse input file(s) and dump anything that looks remotely like a
 * RIFF data stream into separate output files, named using extracted
 * labels, if applicable.  Useful e.g. for extracting audio streams from
 * game files like Borderlands2 *.pck.
 *
 * NOTE: The extracted raw RIFF streams most likely will need some form
 * of post-processing to be useful.  E.g. for the Audiokinetic Wwise
 * RIFF/RIFX sound format you should:
 *
 *   1. Run each dumped file through the ww2ogg converter.
 *      [ See https://github.com/hcs64/ww2ogg ]
 *
 *   2. Fix up the resulting Ogg vorbis file with revorb.
 *      [ See https://github.com/jonboydell/revorb-nix ]
 *
 * Porting:
 *
 *  - Replace open/mmap/creat/write with functions from stdio (fopen,
 *    fread, fwrite) while taking extra care of splitting on buffer
 *    boundaries.
 *
 *  - Port mkdirp.
 *
 */


#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>


#define LOG(...)    fprintf(stderr, __VA_ARGS__)


/* mkdirp
 * Create a directory and missing parents.
 */
static inline int mkdirp(const char *pathname, mode_t mode) {
    if (!pathname || !*pathname) {
        errno = ENOENT;
        return -1;
    }

    int err = 0;
    char *p;
    char path[strlen(pathname) + 1];
    struct stat sb;

    if (stat(pathname, &sb) == 0 && S_ISDIR(sb.st_mode))
        return 0;

    mode |= S_IRWXU;
    strcpy(path, pathname);
    p = path + 1;
    do {
        p = strchr(p, '/');
        if (p)
            *p = '\0';
        if (stat(path, &sb) != 0 || !S_ISDIR(sb.st_mode))
            err = mkdir(path, mode);
        if (p)
            *p++ = '/';
    } while (!err && p && *p);

    return err;
}


/* mem_mem
 * Locate needle of length nlen in haystack of length hlen.
 * Returns a pointer to the first occurrence of needle in haystack, or
 * haystack for a needle of zero length, or NULL if needle was not found
 * in haystack.
 *
 * Uses the Boyer-Moore-Horspool search algorithm, see
 * https://en.wikipedia.org/wiki/Boyer–Moore–Horspool_algorithm
 *
 * For our tiny needles and non-pathologic haystacks this borders on
 * overkill, but meh.
 */
static inline void *mem_mem(const void *haystack, size_t hlen,
                     const void *needle, size_t nlen) {
    size_t k, skip[256];
    const uint8_t *hst = (const uint8_t *)haystack;
    const uint8_t *ndl = (const uint8_t *)needle;

    if (nlen == 0)
        return (void *)haystack;

    /* Set up the finite state machine we use. */
    for (k = 0; k < 256; ++k)
        skip[k] = nlen;
    for (k = 0; k < nlen - 1; ++k)
        skip[ndl[k]] = nlen - k - 1;

    /* Do the search. */
    for (k = nlen - 1; k < hlen; k += skip[hst[k]]) {
        int i, j;
        for (j = nlen - 1, i = k; j >= 0 && hst[i] == ndl[j]; j--)
            i--;
        if (j == -1)
            return (void *)(hst + i + 1);
    }
    return NULL;
}


/*
 * use_basename:
 * 0: retain directory structure:   a/b/foo.in -> output/a/b/foo/042.riff
 * 1: create flat output directory: a/b/foo.in -> output/001_foo_042.riff
 *
 * use_label:
 * 0: create output filename from stream index number only
 * 1: scan for label in stream and use it for output filename
 *
 * guess_length:
 * 0: read the stream length from the RIFF header size field
 * 1: assume the stream ends at the beginning of the next (or EOF)
 */

static struct {
    int use_basename;
    int use_label;
    int guess_length;
    int verbose;
    int endianess; /* No cmd line option for this, we figure it out. */
} cfg = {
    0,
    0,
    0,
    0,
    0,
};

static inline void usage(const char *argv0) {
    LOG("Usage: %s [-b] infile ... [outdir]\n"
        "  -b : create flat output directory\n"
        "  -l : use extracted labels in filenames (unreliable!)\n"
        "  -g : ignore size fields, guess stream length (imprecise!)\n"
        "  -v : be more verbose\n"
        , argv0);
    exit(EXIT_FAILURE);
}

static inline int config(int argc, char *argv[]) {
    int opt;

    while ((opt = getopt(argc, argv, "+:bglv")) != -1) {
        switch (opt) {
        case 'b':
           cfg.use_basename = 1;
           break;
        case 'g':
           cfg.guess_length = 1;
           break;
        case 'l':
           cfg.use_label = 1;
           break;
        case 'v':
           cfg.verbose = 1;
           break;
        default: /* '?' || ':' */
           usage(argv[0]);
           break;
        }
    }
    return optind;
}

static inline uint32_t get_ui32(const void *p) {
    const uint8_t *b = p;
    if (!cfg.endianess)     /* Little Endian byte order (RIFF) */
        return b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24;
    /* Big Endian byte order (RIFX) */
    return b[3] | b[2] << 8 | b[1] << 16 | b[0] << 24;
}

/*
 * Try to find a suitable "labl" chunk.
 * We should really parse the RIFF structure.  Instead, we are satisfied
 * with the first null-terminated label string with length > 0.
 */
static inline const char *labl(const void *p, size_t len) {
    static char lab[201] = "";
    const uint8_t *b;
    size_t l, ll;

    *lab = '\0';
    b = p;
    l = len;
    while (l > 8 && NULL != (b = mem_mem(b, l, "labl", 4))) {
        b += 4;    /* skip 'labl' */
        l = len - (b - (uint8_t *)p);
        ll = get_ui32(b);
        if (ll > l - 4)
            ll = l - 4;
        /* The label we want? 200 is a magic number, 6 isn't (ID + 1 + '\0').
         * We want it null terminated and start with a printable character! */
        if (ll <= 200 && ll >= 6 && isprint(b[8]) && b[ll+4-1] == '\0') {
            strcpy(lab, (const char *)(b + 8)); /* skip label size and ID */
            break;
        }
    }
    /* Sanitize label */
    for (char *p = lab; *p; ++p) {
        if (!isprint((unsigned char)*p) || strchr("/\\ ", *p))
            *p = '_';
    }
    return lab;
}

/*
 * Dump RIFF stream.
 * Write a data blob of length len starting at b to a file whose name is
 * constructed from prefix, an optional label, a numeric id and a suffix.
 */
static inline int dump(const char *prefix, size_t id, const void *b, size_t len) {
    int fd;
    const char *suffix[] = {"riff", "rifx"};  /* dump filename suffix */
    char of[strlen(prefix) + 255];
    const char *lab;

    /* Construct file name from prefix and label or id: */
    lab = cfg.use_label ? labl(b, len) : "";
    snprintf(of, sizeof of, "%s%s%s%06zu.%s",
                    prefix, lab, *lab?"_":"", id, suffix[cfg.endianess]);
    if (cfg.verbose)
        LOG(": %8zu -> %s\n", len, of);
    /* Caveat: This will overwrite any existing file with the same name! */
    fd = creat(of, 0644);
    if (0 > fd){
        LOG("Failed to create %s: %s\n", of, strerror(errno));
        return -1;
    } else {
        write(fd, b, len);
        close(fd);
    }
    return 0;
}

/*
 * Traverse file fd and dump anything that looks like a RIFF stream.
 */
int extract(int fd, const char *pfx) {
    const char *RIF_[] = {"RIFF", "RIFX"};
    size_t id, rsize;
    off_t fsize, remsize;
    const uint8_t *riff, *mfile;

    fsize = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    mfile = mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mfile == MAP_FAILED) {
        LOG("mmap failed: %s\n", strerror(errno));
        return -1;
    }
    id = 0;
    /* Where there's no RIFF, there might be a RIFX ... */
    for (cfg.endianess = 0; cfg.endianess < 2; ++cfg.endianess )
        if (NULL != (riff = mem_mem(mfile, fsize, RIF_[cfg.endianess], 4)))
            break;
    if (!riff)  /* ... or nothing at all. */
        return 0;
    remsize = fsize - (riff - mfile);
    while (remsize > 8 && NULL != riff) {
        const uint8_t *next;
        next = mem_mem(riff + 4, remsize - 4, RIF_[cfg.endianess], 4);
        /* Read length info or guess stream length: */
        if (cfg.guess_length) {
            rsize = next ? next - riff : remsize;
        }
        else {
            rsize = get_ui32(riff + 4) + 8; /* size + 'RIFF' + uint32 */
            if ((off_t)rsize > remsize)
                rsize = remsize;
        }
        /* Dump RIFF stream: */
        LOG("%sEntry %5zu", cfg.verbose?"":"\r", id);
        dump(pfx, id, riff, rsize);
        /* Skip to next segment: */
        ++id;
        riff = next;
        remsize = riff ? fsize - (riff - mfile) : 0;
    }
    munmap((void *)mfile, fsize);
    return id;
}

int main(int argc, char *argv[]) {
    int i, total, argidx = 1;
    const char *odir;
    struct stat st;

    argidx = config(argc, argv);
    if (argc - argidx < 1)
        usage(argv[0]);

    /* If the last argument does not designate an existing file, we
     * attempt to interpret it as the name of the output directory: */
    if (0 != stat(argv[argc - 1], &st) || S_ISDIR(st.st_mode)) {
        odir = argv[--argc];
        if (argc - argidx < 1)
            usage(argv[0]);
    }
    else
        odir = "output";
    LOG("Using \"%s\" as output directory\n", odir);
    i = stat(odir, &st);
    if (0 != i) {
        LOG("Creating \"%s\"\n", odir);
        mkdirp(odir, 0755);
        i = stat(odir, &st);
    }
    if (0 != i || !S_ISDIR(st.st_mode)) {
        LOG("%s is not a valid output directory\n", odir);
        exit(EXIT_FAILURE);
    }

    /* Loop over remaining arguments as input files: */
    for (total = 0, i = argidx; i < argc; i++) {
        int fd, cnt;
        char fpfx[PATH_MAX], tfn[PATH_MAX], *x;

        fd = -1;
        if (0 == stat(argv[i], &st)) {
            if (S_ISREG(st.st_mode))
                fd = open(argv[i], O_RDONLY);
            else
                errno = ENOTSUP;
        }
        if (fd < 0){
            LOG("Skipping %s (failed to open: %s)\n", argv[i], strerror(errno));
            continue;
        }

        LOG("Processing %s\n", argv[i]);
        strcpy(tfn, argv[i]);
        if ( NULL != (x = strrchr(tfn, '.')))
            *x = 0;
        if (cfg.use_basename) {
            x = strrchr(tfn, '/');
            x = x ? x : tfn;
            snprintf(fpfx, sizeof fpfx, "%s/%03d_%s_", odir, i - argidx, x);
        }
        else {
            snprintf(fpfx, sizeof fpfx, "%s/%s/", odir, tfn);
            mkdirp(fpfx, 0755);
        }
        LOG("Dumping to %s...\n", fpfx);
        cnt = extract(fd, fpfx);
        close(fd);
        LOG("%sDumped %d entries      \n", cfg.verbose?"":"\r", cnt);
        total += cnt;
    }
    LOG("\rDumped a total of %d entries.\n", total);

    exit(EXIT_SUCCESS);
}
