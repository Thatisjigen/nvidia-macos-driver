#!/usr/bin/env python3
"""The conversion the copy path uses for l10r, compiled on its own from the kext's source."""
import pathlib, re, subprocess, sys, tempfile

source = (pathlib.Path(__file__).resolve().parent.parent / "kexts/NVRM/accel/nvrm-accel.cpp").read_text()
line = re.search(r"static inline unsigned nvIopL10rToX8\(unsigned v\) \{.*\}", source).group(0)
program = line + """
#include <stdio.h>
int main(void) {
    static const unsigned cases[][2] = {
        {0x3fffffffu, 0xffffffffu}, {0xffffffffu, 0xffffffffu}, {0x00000000u, 0xff000000u},
        {0x3ff00000u, 0xffff0000u}, {0x000ffc00u, 0xff00ff00u}, {0x000003ffu, 0xff0000ffu},
        {0x20080200u, 0xff808080u}, {0x00401004u, 0xff010101u}, {0x00100401u, 0xff000000u},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++)
        if (nvIopL10rToX8(cases[i][0]) != cases[i][1]) {
            printf("%08x gave %08x, wanted %08x\\n", cases[i][0], nvIopL10rToX8(cases[i][0]), cases[i][1]);
            return 1;
        }
    return 0;
}
"""
with tempfile.TemporaryDirectory() as d:
    c = pathlib.Path(d) / "t.c"; x = pathlib.Path(d) / "t"
    c.write_text(program)
    subprocess.run(["clang", "-o", str(x), str(c)], check=True)
    sys.exit(subprocess.run([str(x)]).returncode)
