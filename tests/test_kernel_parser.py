#!/usr/bin/env python3
"""Compile the real fp_feed/fp_find_sync with a userspace sink and sanitizers.

No module loading or capture hardware is required. Run with Python 3 and a C
compiler (CC defaults to cc). The caller always supplies full DMA chunks, as
fp_service does; arbitrary small feed sizes are outside this API's contract.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class KernelParserTests(unittest.TestCase):
    def test_dma_chunk_boundaries(self):
        source = (ROOT / 'lib/sc0710-4port.c').read_text()
        parser = source[source.index('static long fp_find_sync('):
                        source.index('/* Partial frames in flight')]
        sync = source[source.index('static const u8 fp_sync[12]'):
                      source.index('/* ----------------------------------------------------------- */',
                                   source.index('static const u8 fp_sync[12]'))]
        defines = '\n'.join(line for line in source.splitlines()
                            if line.startswith(('#define FP_LINE ', '#define FP_HDR ',
                                                '#define FP_CHUNK ')))
        shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
#define BUILD_BUG_ON(x) _Static_assert(!(x), #x)
#define printk_ratelimited(...) ((void)0)
static unsigned int fourport_debug;
struct sc0710_4port {
    u8 carry[FP_LINE], last_hdr[FP_HDR];
    size_t carry_len;
    bool synced, locked_once;
    unsigned long long resyncs;
    unsigned int received, next;
};
static void fp_line(struct sc0710_4port *fp, const u8 *hdr, const u8 *pix)
{
    unsigned int id;
    memcpy(&id, hdr + 12, sizeof(id));
    assert(id == fp->next++);
    for (size_t i = 0; i < FP_LINE - FP_HDR; i++)
        assert(pix[i] == (u8)(id % 251));
    fp->received++;
}
'''
        main = r'''
static void run(size_t offset, bool gap)
{
    size_t len = FP_CHUNK * 3;
    u8 *data = malloc(len);
    assert(data);
    memset(data, 0x55, len);
    unsigned int count = 0;
    size_t pos = offset;
    while (pos + FP_LINE <= len) {
        memcpy(data + pos, fp_sync, sizeof(fp_sync));
        memcpy(data + pos + 12, &count, sizeof(count));
        memset(data + pos + FP_HDR, count % 251, FP_LINE - FP_HDR);
        pos += FP_LINE;
        count++;
        if (gap && count == 100) pos += 23;
    }
    struct sc0710_4port fp = {0};
    for (pos = 0; pos < len; pos += FP_CHUNK) {
        fp_feed(&fp, data + pos, FP_CHUNK);
        assert(fp.carry_len < FP_LINE);
    }
    assert(fp.received == count);
    free(data);
}
int main(void)
{
    run(0, false);
    /* Every possible split within the 16-byte header. */
    for (size_t split = 1; split < FP_HDR; split++)
        run(FP_LINE - split, false);
    run(137, false); /* payload straddles chunk boundary */
    run(0, true); /* garbage between otherwise intact lines */
    puts("PASS: 18 real-kernel-parser scenarios (ASan/UBSan)");
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='sc0710-parser-') as tmp:
            cfile = Path(tmp) / 'parser.c'
            exe = Path(tmp) / 'parser'
            cfile.write_text(defines + shim + sync + parser + main)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-g', '-O1',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            str(cfile), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
