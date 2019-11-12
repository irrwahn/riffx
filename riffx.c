/*
 * Original code copyright (c) 2019, volpol.
 *
 * Changes 2019 by irrwahn:
 *  - renamed wsp.c to riffx.c
 *  - replaced mkdir_p with mkdirp
 *  - refactoring, reformatting
 *  - some minor changes and optimizations
 *
 * Find anything that looks remotely like a RIFF data stream and dump it
 * into separate files, named using extracted labels, if found.   Useful
 * e.g. for extracting audio streams from game files like Unreal pck.
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


/*
 * USE_BASENAME
 *  0: retain directory structure: a/b/foo.in -> output/a/b/foo/042.riff
 *  1: flat output directory:      a/b/foo.in -> output/001_foo_042.riff
 */
#define USE_BASENAME 0

/* dump filename suffix */
#define SUFFIX      ".riff"

/* RIFF marker */
#define RIFF        "RIFF"
#define RIFF_LEN    4

/* label marker */
#define LABL        "labl"
#define LABL_LEN    4
#define LABL_SKIP   8


/* mkdirp
 * Create a directory and missing parents.
 */
int mkdirp(const char *pathname, mode_t mode) {
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
 * Returns pointer to first occurrence of needle in haystack or NULL.
 * Uses the Boyer-Moore search algorithm.
 *   Cf. http://www-igm.univ-mlv.fr/~lecroq/string/node14.html
 */
static void *mem_mem(const void *haystack, size_t hlen,
                     const void *needle, size_t nlen) {
    size_t k;
    int skip[256];
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

static void dump(const char *prefix, int id, uint8_t *b, size_t len) {
    int fd;
    char of[strlen(prefix) + 255];
    char *lab = NULL;
    size_t ml = len;
    uint8_t *mp = b;

    do {
        mp = mem_mem(mp, ml, LABL, LABL_LEN);
        if (mp) {
            size_t ll;
            mp += LABL_LEN;
            ll = *(uint32_t *)mp;
            mp += LABL_SKIP;
            ml = len - (mp - b);
            // The label we want? 200 is a magic number, 7 isn't (7 > 4 + 2)
            if (ll <= 200 && ll >= 7)
                lab = (char *)mp;
        }
    } while (mp);

    if (lab)
        snprintf(of, sizeof of, "%s%s%s", prefix, lab, SUFFIX);
    else
        snprintf(of, sizeof of, "%s%06d%s", prefix, id, SUFFIX);

    //fprintf(stderr, "Dumping %lu bytes to %s\n", len, of);
    fd = creat(of, 0644);
    if (0 > fd){
        fprintf(stderr, "Failed to create %s: %s\n", of, strerror(errno));
    } else {
        write(fd, b, len);
        close(fd);
    }
}


int main(int argc, char *argv[]) {
    int i;
    const char *odir;
    struct stat st;
    const char *usemsg  = "Usage: %s infile ... [outdir]\n";

    if (argc < 2) {
        fprintf(stderr, usemsg, argv[0]);
        exit(EXIT_FAILURE);
    }

    if (0 != stat(argv[argc - 1], &st) || S_ISDIR(st.st_mode))
        odir = argv[--argc];
    else
        odir = "output";
    fprintf(stderr, "Using \"%s\" as output directory\n", odir);

    if (argc < 2) {
        fprintf(stderr, usemsg, argv[0]);
        exit(EXIT_FAILURE);
    }

    i = 0;
    if (0 != stat(odir, &st)) {
        fprintf(stderr, "Creating \"%s\"\n", odir);
        mkdirp(odir, 0755);
        i = stat(odir, &st);
    }

    if (0 != i || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "%s is not a valid output directory\n", odir);
        exit(EXIT_FAILURE);
    }

    for (i = 1; i < argc; i++) {
        int fd, id;
        off_t fsize, rsize;
        uint8_t *mfile, *riff, *next;
        char fpfx[PATH_MAX], tfn[PATH_MAX], *x;

        id = 0;
        fd = -1;

        if (0 == stat(argv[i], &st)) {
            if (S_ISREG(st.st_mode))
                fd = open(argv[i], O_RDONLY);
            else
                errno = ENOTSUP;
        }
        if (fd < 0){
            fprintf(stderr, "Failed to open %s: %s\n", argv[i], strerror(errno));
            continue;
        }

        fprintf(stderr, "Processing %s\n", argv[i]);
        strcpy(tfn, argv[i]);
        if ( NULL != (x = strrchr(tfn, '.')))
            *x = 0;
#if USE_BASENAME
        x = strrchr(tfn, '/');
        snprintf(fpfx, sizeof fpfx, "%s/%03d_%s_", odir, i, x ? x + 1 : tfn);
#else
        snprintf(fpfx, sizeof fpfx, "%s/%s/", odir, tfn);
        mkdirp(fpfx, 0755);
#endif
        fsize = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
        mfile = mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
        riff = mem_mem(mfile, fsize, RIFF, RIFF_LEN);
        while (NULL != riff) {
            fprintf(stderr, "\rEntry %d ", id);
            rsize = fsize - (riff - mfile);
            next = mem_mem(riff + RIFF_LEN, rsize - RIFF_LEN, RIFF, RIFF_LEN);
            dump(fpfx, id, riff, next ? next - riff : rsize);
            riff = next;
            ++id;
        }
        munmap(mfile, fsize);
        close(fd);
        fprintf(stderr, "\rDumped %d entries\n", id);
    }

    exit(EXIT_SUCCESS);
}
