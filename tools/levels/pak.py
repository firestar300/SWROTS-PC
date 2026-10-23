#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Find a resource inside a level PAK and copy its data out (resources stored inline, such as level files).

    python tools/levels/pak.py find    <pak> <name part>          the entries whose name contains <name part>
    python tools/levels/pak.py extract <pak> <name part> <out>     the first such entry's data, to <out>

An entry's header (see docs/modding/file-formats.md, "PAK entry"): u32 flags, u32 offset of the header itself,
u32 size, u32 raw size, u32 manual, u32 name length, the name, then u32 flags, u32 segmented, u32 type, i32 segment,
i32 image offset, i32 image size, and the data. The PAK comes from the user's own copy of the game.
"""
import re
import struct
import sys


def entries(data, part):
    """(header offset, name, raw size, type, data offset) of each entry whose name contains `part`."""
    out = []
    for m in re.finditer(re.escape(part.lower()), data.lower()):
        # back up to a header whose offset field points at itself
        for back in range(0, 200):
            h = m.start() - back - 24
            if h < 0:
                break
            flags, off, size, raw, manual, nl = struct.unpack_from('<6I', data, h)
            if off == h and 0 < nl < 260 and h + 24 + nl > m.start():
                name = data[h + 24:h + 24 + nl].decode('latin1').rstrip('\0')
                p = h + 24 + nl
                flags2, seg, typ, segment, image_off, image_size = struct.unpack_from('<IIIiii', data, p)
                out.append((h, name, raw, typ, p + 24))
                break
    return out


def main():
    if len(sys.argv) >= 4 and sys.argv[1] == 'find':
        for h, name, raw, typ, at in entries(open(sys.argv[2], 'rb').read(), sys.argv[3].encode()):
            print('%#x %s: type %d, %d bytes at %#x' % (h, name, typ, raw, at))
    elif len(sys.argv) >= 5 and sys.argv[1] == 'extract':
        data = open(sys.argv[2], 'rb').read()
        found = entries(data, sys.argv[3].encode())
        if not found:
            sys.exit('no entry named like %s' % sys.argv[3])
        h, name, raw, typ, at = found[0]
        open(sys.argv[4], 'wb').write(data[at:at + raw])
        print('%s: %d bytes to %s' % (name, raw, sys.argv[4]))
    else:
        print(__doc__)


if __name__ == '__main__':
    main()
