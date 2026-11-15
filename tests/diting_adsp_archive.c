/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>
#include "DitingAdspArchive.h"

static voidpf allocate(voidpf opaque, uInt items, uInt size)
{
    if (opaque || (size && items > (1u << 20) / size)) return NULL;
    return calloc(items, size);
}
static void release(voidpf opaque, voidpf address)
{
    (void)opaque; free(address);
}
int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) return 1;
    FILE *file = fopen(argv[1], "rb");
    if (!file || fseek(file, 0, SEEK_END)) return 2;
    long length = ftell(file);
    if (length <= 0 || length > DitingAdspArchiveLimit + DitingAdspArchiveHeader ||
        fseek(file, 0, SEEK_SET)) { fclose(file); return 2; }
    uint8_t *packed = malloc((size_t)length);
    if (!packed) { fclose(file); return 2; }
    size_t got = fread(packed, 1, (size_t)length, file); fclose(file);
    struct DitingAdspArchive entry;
    if (got != (size_t)length || !diting_adsp_archive_parse(packed, got, &entry)) {
        free(packed); return 3;
    }
    uint8_t *raw = malloc((size_t)entry.bytes);
    if (!raw) { free(packed); return 2; }
    int ok = diting_adsp_archive_inflate(&entry, raw, allocate, release,
                                       argc == 4 ? (voidpf)&entry : NULL);
    if (!ok) { free(raw); free(packed); return 4; }
    file = fopen(argv[2], "wb");
    if (!file) { free(raw); free(packed); return 2; }
    got = fwrite(raw, 1, (size_t)entry.bytes, file);
    int closed = fclose(file);
    ok = got == entry.bytes && !closed;
    free(raw); free(packed);
    return ok ? 0 : 2;
}
