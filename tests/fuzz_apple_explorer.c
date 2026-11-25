#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "../src/aex_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *data;
    size_t         size;
} fuzz_mem_t;

static int fuzz_read_cb(void *ctx, uint64_t off, size_t len, void *buf) {
    const fuzz_mem_t *m = (const fuzz_mem_t *)ctx;
    if (off > m->size || len > m->size - (size_t)off) return -EIO;
    memcpy(buf, m->data + (size_t)off, len);
    return 0;
}

static int walk_cb(void *ctx, const char *name, uint64_t ino, aex_ftype_t ftype) {
    aex_t *fs = (aex_t *)ctx;
    (void)name;
    aex_attr_t a;
    if (aex_getattr(fs, ino, &a) == 0 && ftype == AEX_FT_REG && a.size > 0) {
        uint8_t tmp[256];
        size_t got = 0;
        (void)aex_read(fs, ino, 0, sizeof(tmp), tmp, &got);
    }
    return 0;
}

static void fuzz_one_input(const uint8_t *data, size_t size) {
    fuzz_mem_t fm = { data, size };
    aex_stream_t *st = NULL;
    if (aex_stream_open_io(fuzz_read_cb, &fm, size, &st) == 0) {
        aex_partition_desc_t parts[8];
        size_t pcnt = 0;
        (void)aex_list_partitions(st, parts, 8U, &pcnt);
        aex_stream_close(st);
    }

    aex_t *fs = NULL;
    if (aex_open_io(fuzz_read_cb, &fm, size, NULL, &fs) == 0) {
        uint64_t root = aex_root_ino(fs);
        (void)aex_readdir(fs, root, walk_cb, fs);
        aex_close(fs);
    }
}

static uint64_t xorshift64(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

int main(int argc, char **argv) {
    int iterations = (argc > 1) ? atoi(argv[1]) : 2500;
    const size_t seed_sz = 65536U;
    uint8_t *seed_hfs = (uint8_t *)calloc(1, seed_sz);
    uint8_t *seed_apfs = (uint8_t *)calloc(1, seed_sz);
    uint8_t *mut = (uint8_t *)malloc(seed_sz);

    /* Construct HFS+ and APFS headers as mutation seeds */
    seed_hfs[1024] = 0x48; seed_hfs[1025] = 0x2B;
    seed_hfs[1024 + 42] = 0x10; /* 4096 */
    memcpy(seed_apfs + 32, "NXSB", 4);
    seed_apfs[36] = 0x00; seed_apfs[37] = 0x10; /* 4096 */

    uint64_t rng = 0xA11CE5EED1234567ULL;
    for (int i = 0; i < iterations; i++) {
        const uint8_t *base = (i & 1) ? seed_hfs : seed_apfs;
        size_t cur_sz = seed_sz;
        if ((i % 5) == 0) {
            cur_sz = (size_t)(xorshift64(&rng) % seed_sz);
        }
        if (cur_sz > 0) {
            memcpy(mut, base, cur_sz);
            int flips = (int)(1 + (xorshift64(&rng) % 24));
            for (int f = 0; f < flips; f++) {
                size_t pos = (size_t)(xorshift64(&rng) % cur_sz);
                mut[pos] ^= (uint8_t)(xorshift64(&rng) & 0xFFU);
            }
        }
        fuzz_one_input(mut, cur_sz);
    }

    free(seed_hfs);
    free(seed_apfs);
    free(mut);
    printf("apple-explorer sanitizer fuzzing PASSED (%d corrupted-image mutations).\n", iterations);
    return 0;
}
