/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomMdt.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Bytes {unsigned char *p; size_t n;};
static int read_image(void *context, uint64_t off, void *to, size_t length)
{
    struct Bytes *b = context;
    if (off > b->n || length > b->n - off) return -1;
    memcpy(to, b->p + off, length); return 0;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *f = fopen(argv[1], "rb"); assert(f);
    assert(!fseek(f, 0, SEEK_END)); long size = ftell(f);
    assert(size > 0 && size <= 64 * 1024 * 1024); rewind(f);
    struct Bytes b = {malloc((size_t)size), (size_t)size}; assert(b.p);
    assert(fread(b.p, 1, b.n, f) == b.n); fclose(f);
    QcomMdtImage image;
    assert(!qcom_mdt_parse(&image, b.n, read_image, &b));
    assert(qcom_mdt_span(&image) <= 0x3100000);
    for (unsigned i = 0; i < image.count; ++i) {
        if (!image.segment[i].load) continue;
        uint64_t offset = 0;
        assert(!qcom_mdt_place(&image, i, 0x9fd00000, 0x3100000, &offset));
        assert(offset + image.segment[i].memory_size <= 0x3100000);
    }
    uint64_t n = qcom_mdt_metadata_size(&image); assert(n > 0 && n <= 65536);
    unsigned char *metadata = malloc(n); assert(metadata);
    assert(!qcom_mdt_metadata(&image, read_image, &b, metadata, n));
    assert(!memcmp(metadata, b.p, image.header_size));
    const QcomMdtSegment *hash = &image.segment[image.hash];
    assert(!memcmp(metadata + image.header_size, b.p + hash->file_offset, hash->file_size));
    free(metadata); free(b.p); puts("PASS native MDT placement and PAS metadata");
    return 0;
}
