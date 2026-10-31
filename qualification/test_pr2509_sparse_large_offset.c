/* Independent native Windows test of high-offset aligned direct reads.
 * A sparse 4GiB+ file requires only ~64KiB physical allocation.
 * Operates solely on a unique local temporary file, never a model shard.
 * Source c/compat.h must be supplied from exact candidate or base.
 */
#include <stdio.h>
#ifndef _WIN32
int main(void) { puts("SKIP Windows only"); return 2; }
#else
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include <winioctl.h>
#include "../compat.h"

#define TEST_FILE "sparse_large_offset_direct_2509.tmp"
#define BLOCK 65536
#define HIGH ((int64_t)1 << 32)
#define POS (HIGH + BLOCK)
#define SIZE (POS + BLOCK)

static int fail(const char *reason) {
    fprintf(stderr, "FAIL sparse offset: %s, Win32=%lu\n",
            reason, (unsigned long)GetLastError());
    return 1;
}

static int make_sparse(const char *path, const unsigned char *pattern) {
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return fail("CREATE_NEW");
    DWORD ignored = 0;
    if (!DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0,
                         &ignored, NULL)) {
        CloseHandle(h);
        return fail("FSCTL_SET_SPARSE");
    }
    LARGE_INTEGER pos = {0};
    pos.QuadPart = POS;
    if (!SetFilePointerEx(h, pos, NULL, FILE_BEGIN)) {
        CloseHandle(h);
        return fail("seek 4GiB+");
    }
    DWORD wrote = 0;
    if (!WriteFile(h, pattern, BLOCK, &wrote, NULL) || wrote != BLOCK) {
        CloseHandle(h);
        return fail("write 64KiB marker");
    }
    LARGE_INTEGER len = {0};
    if (!GetFileSizeEx(h, &len) || len.QuadPart != SIZE) {
        CloseHandle(h);
        return fail("size 4GiB+");
    }
    if (!FlushFileBuffers(h)) {
        CloseHandle(h);
        return fail("flush");
    }
    if (!CloseHandle(h)) return fail("close writer");
    return 0;
}

int main(void) {
    if (sizeof(off_t) < 8) return fail("off_t not 64-bit");
    /* Isolated fixture path is not shared with other tests. */
    DeleteFileA(TEST_FILE);
    unsigned char *pattern = malloc(BLOCK);
    if (!pattern) return fail("alloc pattern");
    for (size_t i = 0; i < BLOCK; i++)
        pattern[i] = (unsigned char)((i * 37u + 19u) ^ (i >> 5));
    if (make_sparse(TEST_FILE, pattern)) return 2;

    int fd = compat_open_direct(TEST_FILE);
    if (fd < 0) return fail("open direct");
    if (compat_fsize(fd) != (off_t)SIZE) return fail("compat_fsize");
    void *aligned = NULL;
    if (posix_memalign(&aligned, 4096, BLOCK)) return fail("aligned alloc");
    for (int r = 0; r < 128; r++) {
        ssize_t n = pread(fd, aligned, BLOCK, (off_t)POS);
        if (n != BLOCK || memcmp(aligned, pattern, BLOCK) != 0)
            return fail("high-offset marker exactness");
        n = pread(fd, aligned, BLOCK, (off_t)(HIGH + (int64_t)BLOCK * 0));
        if (n != BLOCK) return fail("hole read length");
        for (size_t i = 0; i < BLOCK; i++)
            if (((unsigned char *)aligned)[i] != 0)
                return fail("sparse-hole data");
        n = pread(fd, aligned, BLOCK, (off_t)SIZE);
        if (n != 0) return fail("EOF must return 0");
    }
    compat_aligned_free(aligned);
    if (close(fd) != 0) return fail("close reader");
    if (!DeleteFileA(TEST_FILE)) return fail("DeleteFile");
    free(pattern);
    puts("PASS sparse 4GiB+ direct-read: 128 exact marker/hole/EOF rounds, fsize and close");
    return 0;
}
#endif
