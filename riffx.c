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
 * game files like Unreal *.pck.
 *
 * NOTE: The extracted raw RIFF streams most likely will need some form
 * of post-processing to be useful.  E.g. for the Audiokinetic Wwise
 * RIFF/RIFX sound format (Unreal Engine 3, et al.) you should:
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


/* dump filename suffix */
#define SUFFIX      ".riff"


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

static inline uint32_t get_ui32(const void *p) {
    const uint8_t *b = p;
    return b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24;
}


/*
 * Try to find a suitable "labl" chunk.
 * We should really parse the RIFF structure.  Instead, we are satisfied
 * with the first null-terminated label string with length > 0.
 */
static inline const char *labl(const void *p, size_t len) {
    const uint8_t *b;
    size_t l, ll;

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
        if (ll <= 200 && ll >= 6 && isprint(b[8]) && b[ll+4-1] == '\0')
            return (const char *)(b + 8); /* skip label size and ID */
    }
    return "";
}

/*
 * Dump RIFF stream.
 * Write a data blob of length len starting at b to a file whose name is
 * constructed from prefix and an extracted label, or, in absence of a
 * label, the provided numeric id.
 */
static inline int dump(const char *prefix, size_t id, const void *b, size_t len) {
    int fd;
    char of[strlen(prefix) + 255];
    const char *lab;

    /* Construct file name from prefix and label or id: */
    lab = labl(b, len);
    snprintf(of, sizeof of, "%s%06zu%s%s%s", prefix, id, *lab?"_":"", lab, SUFFIX);
    //LOG(" -> %s\n", of);
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
    riff = mfile;
    remsize = fsize;
    while (remsize > 8 && NULL != (riff = mem_mem(riff, remsize, "RIFF", 4))) {
        remsize = fsize - (riff - mfile);
        rsize = get_ui32(riff + 4) + 8; /* + 'RIFF' + uint32 */
        if ((off_t)rsize > remsize)
            rsize = remsize;
        LOG("\rEntry %4zu: %8zu ", id, rsize);
        dump(pfx, id, riff, rsize);
        ++id;
        /* Skip 'RIFF'; skipping rsize would be correct, but the size info
         * isn't always sane, so we rather risk getting false positives. */
        riff += 4;
        remsize -= 4;
    }
    munmap((void *)mfile, fsize);
    return id;
}

int main(int argc, char *argv[]) {
    int i, argidx = 1;
    int use_basename = 0;
    const char *odir;
    struct stat st;
    const char *usemsg = "Usage: %s [-b] infile ... [outdir]\n"
                         "  -b : create flat output hierarchy\n";

    if (argc - argidx < 1) {
        LOG(usemsg, argv[0]);
        exit(EXIT_FAILURE);
    }

    /* Check for -b (use basename) option:
     * 0: retain directory structure:   a/b/foo.in -> output/a/b/foo/042.riff
     * 1: create flat output directory: a/b/foo.in -> output/001_foo_042.riff
     */
    if (0 == strcmp(argv[argidx], "-b")) {
        use_basename = 1;
        ++argidx;
    }
    if (argc - argidx < 1) {
        LOG(usemsg, argv[0]);
        exit(EXIT_FAILURE);
    }

    /* If the last argument does not designate an existing file, we
     * attempt to interpret it as the name of the output directory: */
    if (0 != stat(argv[argc - 1], &st) || S_ISDIR(st.st_mode)) {
        odir = argv[--argc];
        if (argc - argidx < 1) {
            LOG(usemsg, argv[0]);
            exit(EXIT_FAILURE);
        }
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
    for (i = argidx; i < argc; i++) {
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
            LOG("Failed to open %s: %s\n", argv[i], strerror(errno));
            continue;
        }

        LOG("Processing %s\n", argv[i]);
        strcpy(tfn, argv[i]);
        if ( NULL != (x = strrchr(tfn, '.')))
            *x = 0;
        if (use_basename) {
            x = strrchr(tfn, '/');
            snprintf(fpfx, sizeof fpfx, "%s/%03d_%s_", odir, i, x ? x + 1 : tfn);
        }
        else {
            snprintf(fpfx, sizeof fpfx, "%s/%s/", odir, tfn);
            mkdirp(fpfx, 0755);
        }
        LOG("Dumping to %s*\n", fpfx);
        cnt = extract(fd, fpfx);
        close(fd);
        LOG("\rDumped %d entries      \n", cnt);
    }

    exit(EXIT_SUCCESS);
}
