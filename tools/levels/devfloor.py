#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""devfloor.py <out.msh> [x0 x1 z0 z1 cells_x cells_z texture]: the developer level's floor, a static mesh written
from scratch (no game data): one flat grid at local y 0 (the static's placement puts it at the floor's height),
one triangle strip, textured with a texture the level already has, tiled every 512 units."""
import math
import struct
import sys

sys.path.insert(0, __import__('os').path.dirname(__file__))
sys.path.insert(0, __import__('os').path.join(__import__('os').path.dirname(__file__), '..', 'meshes'))
import charmsh

IDENTITY = struct.pack('<12f', 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0)
# vertex format (35 bytes; charmsh.element_offsets): position at 0, normal (4 x u8) at 12, uv (2 x f32) at 16,
# colour (4 x u8, the static's baked light) at 24; stride 28. Bytes before 0x16 copied from a game static's.
FORMAT = bytes.fromhex('00010302cf020000ce02000204010400cc020000cb02000c1810ffffffffffffff041c')


def table(names):
    text = b''.join(n + b'\0' for n in names)
    offsets, at = [], 0
    for n in names:
        offsets.append(at)
        at += len(n) + 1
    return {'total': len(text), 'x': 0, 'used': len(text), 'offsets': {'cap': max(4, len(names)), 'items': offsets},
            'text': text}


def floor_mesh(x0, x1, z0, z1, nx, nz, texture, tile=512.0):
    verts = bytearray()
    for j in range(nz + 1):
        for i in range(nx + 1):
            x = x0 + (x1 - x0) * i / nx
            z = z0 + (z1 - z0) * j / nz
            verts += struct.pack('<3f', x, 0.0, z) + bytes([128, 255, 128, 0]) + struct.pack('<2f', x / tile, z / tile)
            verts += bytes([255, 255, 255, 255])
    idx = []
    for j in range(nz):
        row = []
        for i in range(nx + 1):
            row += [(j + 1) * (nx + 1) + i, j * (nx + 1) + i]
        if idx:
            idx += [idx[-1], row[0]]  # degenerate join
            if len(idx) % 2:
                idx.append(row[0])  # keep the winding
        idx += row
    tris = 2 * nx * nz
    prim = {'palette': {'cap': 0, 'items': []}, 'indices': {'cap': len(idx), 'items': idx}, 'b22': 1, 'b23': 0,
            'u18': (nx + 1) * (nz + 1), 's20': tris, 'u1c': tris * 3}
    render = {'present': 1, 'format': FORMAT, 'count': (nx + 1) * (nz + 1), 'stride': 28, 'vertices': bytes(verts),
              'prims': {'cap': 1, 'items': [prim]}}
    layers = [{'u8': 255, 'uv': [0, 0, 0, 0], 'texture': 1}] + [{'u8': 0, 'uv': [0, 0, 0, 0], 'texture': 0}] * 3
    group = {'flags': 0, 'colour': 0xFF000000, 'f8': 1.0, 'fc': 0.01, 'u72': 0,
             'material': {'shader': 0, 'layers': layers}, 'render': render}
    body = {'newFormat': 1, 'appearances': {'cap': 0, 'items': []}, 'groups': {'cap': 1, 'items': [group]},
            'textures': table([texture]), 'materials': table([b'DevFloorSG']), 'images': {'cap': 0, 'items': []},
            # one record (type 0, 1, 48 zero bytes): without it the static crashes the level's load (0x2078FD)
            'attachments': {'cap': 4, 'items': [{'type': 0, 'i': 1, 'm': bytes(48)}]}}
    half = ((x1 - x0) / 2, 0.0, (z1 - z0) / 2)
    mesh = {'name': b'Base'.ljust(32, b'\0'), 'centre': struct.pack('<3f', (x0 + x1) / 2, 0.0, (z0 + z1) / 2),
            'radius': math.sqrt(half[0] ** 2 + half[2] ** 2), 'b5e': 0, 'b5c': 0, 'b5d': 0, 'b5f': 1,
            'colour': 0xFF000000, 'i6c': tris, 'b67': 1, 'body': body,
            'bmin': struct.pack('<3f', x0, 0.0, z0), 'bmax': struct.pack('<3f', x1, 0.0, z1), 'b63': 0, 'b64': 0,
            'b65': 1}
    bone = {'name': b'root'.ljust(32, b'\0'), 'matrix': IDENTITY, 'parent': -1}
    model = {'name': b'Base', 'b5d': 1, 'meshes': {'cap': 1, 'items': [mesh]}, 'bones': {'cap': 1, 'items': [bone]},
             'matrix': IDENTITY, 'matrix2': IDENTITY, 'flags': {'cap': 0, 'items': []}}
    return {'magic': 0xF8C203A1, 'version': 0xA0042,
            'header': {'cap': 1, 'items': [{'name': b'Base', 'names': {'cap': 1, 'items': [b'Base']}}]},
            'models': {'cap': 1, 'items': [model]}, 'meshFlags': [[1]]}


if __name__ == '__main__':
    a = sys.argv
    x0, x1, z0, z1 = (float(v) for v in a[2:6]) if len(a) > 5 else (1500.0, 24000.0, 2700.0, 13500.0)
    nx, nz = (int(a[6]), int(a[7])) if len(a) > 7 else (44, 21)
    tex = a[8].encode() if len(a) > 8 else b'M07_CaveFloorTile512_02'
    m = floor_mesh(x0, x1, z0, z1, nx, nz, tex)
    data = charmsh.save(m)
    open(a[1], 'wb').write(data)
    charmsh.load(a[1])
    print('wrote %s: %d vertices, %d bytes' % (a[1], (nx + 1) * (nz + 1), len(data)))
