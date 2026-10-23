#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""slpobj.py <file.slp> <class or name part> [array limit]: the matching objects' properties as a tree."""
import sys

sys.path.insert(0, __import__('os').path.dirname(__file__))
sys.path.insert(0, __import__('os').path.join(__import__('os').path.dirname(__file__), '..', 'meshes'))
import slp

f, r = slp.load(sys.argv[1])
want = sys.argv[2].encode()
limit = int(sys.argv[3]) if len(sys.argv) > 3 else 3
for o in f['objects']['items']:
    name = slp.get(o, b'Name') or b''
    if o['cls'] == want or (want in name):
        print('%#x %s %s (tail %s)' % (o['at'], o['cls'].decode(), name[4:].decode('latin1'), o['tail'].hex()))
        for p in o['props']:
            sub = slp.show_tree(slp.tree(p['data']), '    ', limit)
            if len(sub) == 1:
                print('  %s: %s' % (p['name'].decode('latin1'), sub[0].strip()))
            else:
                print('  %s:' % p['name'].decode('latin1'))
                print('\n'.join(sub))
