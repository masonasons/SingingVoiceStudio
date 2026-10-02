#!/usr/bin/env python3
"""Turn a crash report's "svs.exe+0x819f85" lines into function names.

    python scripts/resolve_crash.py crash-20261002-174434.txt [build/SingingVoiceStudio.map]

The map is the linker's own record of where every function landed (the build
writes it beside each executable); it has to be the map of the very build that
crashed, which is why the release script keeps it. Lines for other modules --
Windows' own DLLs, DECtalk.dll -- are shown as they are.
"""
import bisect
import os
import re
import sys

SYMBOL = re.compile(r'^\s+0x([0-9a-f]{8,16})\s+(\S.*)$')
OFFSET = re.compile(r'(SingingVoiceStudio|svs)\.exe\+0x([0-9a-f]+)', re.I)
IMAGE_BASE = 0x140000000          # what GNU ld gives a 64-bit executable


def load(path):
    out = []
    with open(path, encoding='utf-8', errors='replace') as fh:
        for line in fh:
            m = SYMBOL.match(line)
            if m:
                name = m.group(2).strip()
                if not name.startswith(('.', '*', '[')):
                    out.append((int(m.group(1), 16), name))
    out.sort()
    return out


def demangle(names):
    try:
        import subprocess
        res = subprocess.run(['c++filt'], input='\n'.join(names), capture_output=True,
                             text=True, timeout=20)
        if res.returncode == 0:
            return res.stdout.splitlines()
    except (OSError, ValueError):
        pass
    return names


def main():
    report = sys.argv[1]
    here = os.path.dirname(os.path.abspath(__file__))
    with open(report, encoding='utf-8', errors='replace') as fh:
        text = fh.read()
    exe = (OFFSET.search(text) or [None, 'SingingVoiceStudio'])[1]
    default = os.path.join(here, '..', 'build', exe + '.map')
    symbols = load(sys.argv[2] if len(sys.argv) > 2 else default)
    addrs = [a for a, _n in symbols]
    found = {}
    for m in OFFSET.finditer(text):
        at = IMAGE_BASE + int(m.group(2), 16)
        i = bisect.bisect_right(addrs, at) - 1
        if i >= 0:
            found[m.group(0)] = (symbols[i][1], at - symbols[i][0])
    names = demangle([n for n, _o in found.values()])
    pretty = dict(zip(found, names))
    for line in text.splitlines():
        m = OFFSET.search(line)
        if m and m.group(0) in found:
            line += '   %s +0x%x' % (pretty[m.group(0)], found[m.group(0)][1])
        print(line)


if __name__ == '__main__':
    main()
