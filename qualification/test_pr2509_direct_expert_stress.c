/* Independent native Windows stress fixture for PR #2509.
 * Inert CPU/NTFS testing only, never attaches to the resident GLM engine.
 * Verifies 16 concurrent 64KiB positional reads on one DIRECT handle,
 * exact bytes, 64-bit-offset mechanics, handle lifetime and EOF.
 */
#include <stdio.h>
#ifndef _WIN32
int main(void) { puts("SKIP: Windows-only"); return 2; }
#else
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include "../compat.h"
#define BLOCK 65536u
#define BLOCKS 256u
#define LANES 16
#define ROUNDS 128
#define TMP "test_direct_expert_stress.tmp"
static const uint8_t *expected;
static volatile LONG failures;
static void fill(uint8_t *p) {
    for (size_t i = 0; i < BLOCK * BLOCKS; ++i) {
        uint32_t n = (uint32_t)i ^ (uint32_t)(i >> 7);
        n = (n ^ (n >> 16)) * 0x7feb352du;
        p[i] = (uint8_t)(n >> 13);
    }
}
typedef struct { int fd; unsigned lane; } Lane;
static DWORD WINAPI reader(void *arg) {
    Lane *a = (Lane *)arg;
    void *p = NULL;
    if (posix_memalign(&p, 4096, BLOCK)) {
        InterlockedIncrement(&failures);
        return 1;
    }
    for (unsigned j = 0; j < ROUNDS; ++j) {
        unsigned b = (j * 37u + a->lane * 53u) % BLOCKS;
        off_t off = (off_t)b * BLOCK;
        ssize_t n = pread(a->fd, p, BLOCK, off);
        if (n != (ssize_t)BLOCK || memcmp(p, expected + off, BLOCK) != 0) {
            fprintf(stderr, "lane=%u round=%u block=%u got=%lld\n",
                    a->lane, j, b, (long long)n);
            InterlockedIncrement(&failures);
            break;
        }
    }
    compat_aligned_free(p);
    return 0;
}
int main(void) {
    uint8_t *pattern = malloc(BLOCK * BLOCKS);
    if (!pattern) return 3;
    fill(pattern);
    FILE *f = fopen(TMP, "wb");
    if (!f) return 4;
    if (fwrite(pattern, 1, BLOCK * BLOCKS, f) != BLOCK * BLOCKS) return 5;
    if (fclose(f) != 0) return 6;
    expected = pattern;
    int fd = compat_open_direct(TMP);
    if (fd < 0) return 7;
    HANDLE handles[LANES] = {0};
    Lane lanes[LANES];
    unsigned launched = 0;
    for (unsigned k = 0; k < LANES; ++k) {
        lanes[k] = (Lane){fd, k};
        handles[k] = CreateThread(NULL, 0, reader, &lanes[k], 0, NULL);
        if (!handles[k]) {
            InterlockedIncrement(&failures);
            break;
        }
        launched++;
    }
    for (unsigned k = 0; k < launched; ++k) {
        if (WaitForSingleObject(handles[k], INFINITE) != WAIT_OBJECT_0)
            InterlockedIncrement(&failures);
        CloseHandle(handles[k]);
    }
    void *buf = NULL;
    if (posix_memalign(&buf, 4096, BLOCK)) return 8;
    if (pread(fd, buf, BLOCK, (off_t)BLOCK * BLOCKS) != 0) {
        fprintf(stderr, "FAIL: aligned EOF did not return 0\n");
        InterlockedIncrement(&failures);
    }
    if (compat_fsize(fd) != (off_t)BLOCK * BLOCKS) {
        fprintf(stderr, "FAIL: direct descriptor fsize\n");
        InterlockedIncrement(&failures);
    }
    close(fd);
    fd = compat_open_direct(TMP);
    if (fd < 0 || pread(fd, buf, BLOCK, 0) != BLOCK ||
        memcmp(buf, pattern, BLOCK) != 0) {
        fprintf(stderr, "FAIL: reopen/read lifecycle\n");
        InterlockedIncrement(&failures);
    }
    if (fd >= 0) close(fd);
    compat_aligned_free(buf);
    remove(TMP);
    free(pattern);
    if (failures != 0) {
        fprintf(stderr, "FAIL: %ld direct-read errors\n", failures);
        return 1;
    }
    printf("PASS: %u exact 64KiB positional reads across %u concurrent workers (128 MiB total), EOF, fsize, reopen\n",
           LANES * ROUNDS, LANES);
    return 0;
}
#endif
