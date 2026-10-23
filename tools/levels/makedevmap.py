#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the developer test map from the user's own game files.

    python tools/levels/makedevmap.py <GameData folder> <out.slp>

Copies the Grievous Survival level file out of its PAK and edits it into the developer map (devmap.py): an empty,
lit, flat floor under the sky. Research: see docs/research/levels.md for what it changes and how to play it.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import devmap
import pak

LEVEL = 'u107_uta_grievoussurvival'


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    pak_path = os.path.join(sys.argv[1], 'pak', 'res_%s.pak' % LEVEL)
    data = open(pak_path, 'rb').read()
    found = [e for e in pak.entries(data, (LEVEL + '.slp').encode()) if e[3] == 32]
    if not found:
        sys.exit('%s: no level file in %s' % (LEVEL, pak_path))
    h, name, raw, typ, at = found[0]
    original = sys.argv[2] + '.original'
    open(original, 'wb').write(data[at:at + raw])
    sys.argv = [sys.argv[0], original, sys.argv[2]]
    devmap.main()
    os.remove(original)


if __name__ == '__main__':
    main()
