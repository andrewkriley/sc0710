#!/usr/bin/env python3
"""Fault injection against the real PCM open function with reference stubs."""
from pathlib import Path
import subprocess
import tempfile
import unittest


class PcmOpenTests(unittest.TestCase):
    def test_reference_cleanup(self):
        source = (Path(__file__).resolve().parents[1] / 'lib/sc0710-4port.c').read_text()
        start = source.index('static int fp_pcm_open(')
        function = source[start:source.index('\nstatic int fp_pcm_close(', start)]
        shim = r'''
#include <assert.h>
#define SNDRV_PCM_HW_PARAM_PERIODS 1
struct v4l2_device { int unused; };
struct sc0710_dev { struct v4l2_device v4l2_dev; };
struct sc0710_4port { struct sc0710_dev *dev; };
struct fp_port { struct sc0710_4port *fp; };
struct runtime { int hw; };
struct snd_pcm_substream { struct runtime *runtime; struct fp_port *p; };
static int fp_pcm_hw, hw_error, constraint_error, users, refs;
static struct fp_port *snd_pcm_substream_chip(struct snd_pcm_substream *s) { return s->p; }
static int fp_hw_get(struct sc0710_4port *fp) { if (hw_error) return hw_error; users++; return 0; }
static void fp_hw_put(struct sc0710_4port *fp) { users--; }
static void v4l2_device_get(struct v4l2_device *d) { refs++; }
static void v4l2_device_put(struct v4l2_device *d) { refs--; }
static int snd_pcm_hw_constraint_integer(struct runtime *r, int p) { return constraint_error; }
'''
        cases = r'''
int main(void) {
    struct sc0710_dev dev = {0};
    struct sc0710_4port fp = { .dev = &dev };
    struct fp_port port = { .fp = &fp };
    struct runtime rt = {0};
    struct snd_pcm_substream sub = { .runtime = &rt, .p = &port };
    hw_error = -19;
    assert(fp_pcm_open(&sub) == -19 && users == 0 && refs == 0);
    hw_error = 0; constraint_error = -12;
    assert(fp_pcm_open(&sub) == -12 && users == 0 && refs == 0);
    constraint_error = 0;
    assert(fp_pcm_open(&sub) == 0 && users == 1 && refs == 1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='sc0710-pcm-') as tmp:
            path = Path(tmp) / 'test.c'
            exe = Path(tmp) / 'test'
            path.write_text(shim + function + cases)
            subprocess.run(['cc', '-std=c11', str(path), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
