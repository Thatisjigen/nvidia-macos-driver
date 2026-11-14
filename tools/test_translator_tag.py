#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 kevineduardo
"""The shader cache's build tag does not change when the translator is signed again.

The tag was the SHA-256 of the whole libnvmtl_translate.dylib, signature included. `codesign --deep` over the bundle
(after one library in it was replaced) signed the translator again: every process swept its shader cache, and at the
next start WindowServer waited a minute on 450 translations, the watchdog killed it and the screen stayed black for two
minutes. plugin/nvmtl_translator_tag.h hashes the translator's LC_UUID and its segments without __LINKEDIT and without
the load commands instead.

This builds a small dylib and signs copies of it in three ways. The control is the old tag (the whole file's digest):
it must differ between the copies, or the test would prove nothing. The new tag must be the same for all of them, and
must change when the code or the data changes. A file that is not a thin 64-bit Mach-O is hashed whole.

usage: test_translator_tag.py [repo root]"""
import hashlib, pathlib, platform, shutil, struct, subprocess, sys, tempfile

root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[1])
fails = 0


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + (f' ({detail})' if detail else ''))
    fails += 0 if ok else 1


load = (root / 'plugin/NVMTLLibraryLoad.m').read_text()
i = load.find('static void nvmtl_translator_cache_tag(')
fn = load[i:load.index('\n}\n', i) + 3] if i >= 0 else ''
check('the plugin takes the tag from nvmtl_translator_tag.h', 'nvmtl_translator_tag_of_image(' in fn and 'CC_SHA256_Update' not in fn,
      'the whole file is still hashed in the plugin' if 'CC_SHA256_Update' in fn else '')

if platform.system() != 'Darwin' or not shutil.which('clang') or not shutil.which('codesign'):
    print('skip the Mach-O checks: needs macOS with clang and codesign')
    sys.exit(1 if fails else 0)

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include "nvmtl_translator_tag.h"
int main(int c, char **v) {
    for (int i = 1; i < c; i++) {
        FILE *f = fopen(v[i], "rb"); if (!f) return 2;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        unsigned char *b = malloc(n ? n : 1); if (fread(b, 1, n, f) != (size_t)n) return 3; fclose(f);
        char tag[80]; int how = nvmtl_translator_tag_of_image(b, (size_t)n, tag, sizeof tag);
        printf("%d %s\n", how, tag); free(b);
    }
    char small[40]; printf("%d short\n", nvmtl_translator_tag_of_image((const unsigned char *)"x", 1, small, sizeof small));
    return 0;
}
'''


def run(*a, **k):
    return subprocess.run(a, capture_output=True, text=True, **k)


with tempfile.TemporaryDirectory() as d:
    d = pathlib.Path(d)
    (d / 'h.c').write_text(HARNESS)
    r = run('clang', '-O1', '-Wall', '-Werror', '-I', str(root / 'plugin'), '-o', str(d / 'h'), str(d / 'h.c'))
    check('the header compiles on its own, without warnings', r.returncode == 0, r.stderr.strip()[-300:])
    (d / 'a.c').write_text('int table[4] = {1, 2, 3, 4};\nint lower(int x) { return x * 3 + table[x & 3]; }\n')
    (d / 'b.c').write_text('int table[4] = {1, 2, 3, 4};\nint lower(int x) { return x * 5 + table[x & 3]; }\n')   # other code
    (d / 'c.c').write_text('int table[4] = {1, 2, 3, 9};\nint lower(int x) { return x * 3 + table[x & 3]; }\n')   # other data
    built = True
    for n in 'abc':
        r = run('clang', '-arch', 'x86_64', '-dynamiclib', '-O1', '-o', str(d / f'{n}.dylib'), str(d / f'{n}.c'))
        built = built and r.returncode == 0
    check('the sample dylibs build', built)
    if fails == 0:
        plain = d / 'a.dylib'
        v1, v2, v3 = d / 'v1' / 'lib.dylib', d / 'v2' / 'lib.dylib', d / 'v3' / 'lib.dylib'
        for v in (v1, v2, v3):
            v.parent.mkdir(); shutil.copy(plain, v)
        run('codesign', '--remove-signature', str(v1))                                   # unsigned
        run('codesign', '-f', '-s', '-', str(v2))                                        # ad hoc, identifier from the name
        run('codesign', '-f', '-s', '-', '-i', 'another.identifier.of.a.different.length', str(v3))
        files = [plain, v1, v2, v3]
        whole = [hashlib.sha256(f.read_bytes()).hexdigest() for f in files]
        check('control: the old tag (the whole file) differs between signings of one build', len(set(whole[1:])) == 3,
              ' '.join(h[:8] for h in whole))
        out = run(str(d / 'h'), *map(str, files + [d / 'b.dylib', d / 'c.dylib', d / 'a.c'])).stdout.split('\n')
        how = [l.split()[0] for l in out if l.strip()]; tag = [l.split()[1] for l in out if l.strip()]
        check('one build signed in three ways, and unsigned, has one tag', len(set(tag[0:4])) == 1 and how[0:4] == ['2'] * 4,
              ' '.join(t[7:15] for t in tag[0:4]))
        check('the tag is the content digest, in the old form (sha256-<64 hex>)',
              all(t.startswith('sha256-') and len(t) == 71 for t in tag[0:6]) and tag[0][7:] not in whole)
        check('other code is another tag', tag[4] != tag[0] and how[4] == '2')
        check('other data is another tag', tag[5] != tag[0] and tag[5] != tag[4] and how[5] == '2')
        check('a file that is not a Mach-O is hashed whole',
              how[6] == '1' and tag[6] == 'sha256-' + hashlib.sha256((d / 'a.c').read_bytes()).hexdigest())
        check('a tag buffer too short for the tag is refused', out[7].split()[0] == '0' if len(out) > 7 else False)
        # a load command that points outside the file must not be read: the image is hashed whole instead
        b = bytearray(plain.read_bytes()); ncmds = struct.unpack_from('<I', b, 16)[0]; at = 32
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<II', b, at)
            if cmd == 0x19 and b[at + 8:at + 14] == b'__TEXT':
                struct.pack_into('<Q', b, at + 48, 1 << 40)   # filesize far past the end
                break
            at += size
        (d / 'bad.dylib').write_bytes(b)
        o = run(str(d / 'h'), str(d / 'bad.dylib')).stdout.split('\n')[0].split()
        check('a segment that runs past the file is not read (hashed whole)',
              o[0] == '1' and o[1] == 'sha256-' + hashlib.sha256(bytes(b)).hexdigest())

print('all passed' if not fails else f'{fails} failed')
sys.exit(1 if fails else 0)
