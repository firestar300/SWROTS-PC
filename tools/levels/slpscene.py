#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""slpscene.py: the scene's tail in a level file (after the world BSP, before the object section), read and written
with charmsh's archive (one function per structure, loading or saving):

  sectors       TArray of sector: u32 a, u32 b, u32 c, i32, i32, TString name, a mesh (the format of meshes: the
                sector's own drawn geometry, its centre/radius and bound box), portals (TArray), portals2 (TArray),
                u32 tail. A portal: TArray of points (12 bytes each), u16 a, u16 b, plane (normal, d), u32, u32.
  groups        TArray of {u32 a, u32 b, u32 c, i32, i32, TString name} (sector names again)
  bounds        u32 present, then 3 points (12 bytes each)
  octrees       u32 count, each: 28-byte header ('OCT\\0', u32 ?, u32 size, ...) + size bytes
  indices       u32 count, each: s16 index; when >= 0: u32 n, n x u16
  u32, u32, u32 (0x4C0B0: bool, u32)
"""
import struct
import sys

sys.path.insert(0, __import__('os').path.dirname(__file__))
sys.path.insert(0, __import__('os').path.join(__import__('os').path.dirname(__file__), '..', 'meshes'))
import charmsh
from charmsh import S, F, tarray


def point(ar, v):
    return ar.raw(12, v)


def portal(ar, p):
    return S(ar, p, [('points', lambda ar, v: tarray(ar, v, point)), ('a', F('u16')), ('b', F('u16')),
                     ('normal', F('raw', 12)), ('d', F('f32')), ('u1', F('u32')), ('u2', F('u32'))])


def sector(ar, s):
    return S(ar, s, [('a', F('u32')), ('b', F('u32')), ('c', F('u32')), ('d', F('i32')), ('e', F('i32')),
                     ('name', F('string')), ('mesh', charmsh.mesh),
                     ('portals', lambda ar, v: tarray(ar, v, portal)),
                     ('portals2', lambda ar, v: tarray(ar, v, portal)), ('tail', F('u32'))])


def group(ar, g):
    return S(ar, g, [('a', F('u32')), ('b', F('u32')), ('c', F('u32')), ('d', F('i32')), ('e', F('i32')),
                     ('name', F('string'))])


def octree(ar, o):
    if ar.loading:
        head = ar.raw(28)
        size = struct.unpack_from('<I', head, 8)[0]
        return {'head': head, 'data': ar.raw(size)}
    ar.raw(28, o['head'])
    ar.raw(len(o['data']), o['data'])
    return o


def index(ar, x):
    o = {} if ar.loading else x
    o['i'] = ar.s16(None if ar.loading else o['i'])
    if o['i'] >= 0:
        n = ar.u32(None if ar.loading else len(o['list']))
        if ar.loading:
            o['list'] = [ar.u16() for _ in range(n)]
        else:
            for v in o['list']:
                ar.u16(v)
    return o


def counted(elem):
    def io(ar, v):
        n = ar.u32(None if ar.loading else len(v))
        return [elem(ar, None if ar.loading else v[i]) for i in range(n)]
    return io


def bounds(ar, b):
    o = {} if ar.loading else b
    o['present'] = ar.u32(None if ar.loading else o['present'])
    if o['present']:
        o['points'] = [ar.raw(12, None if ar.loading else o['points'][i]) for i in range(3)]
    return o


def scene(ar, s):
    return S(ar, s, [('sectors', lambda ar, v: tarray(ar, v, sector)),
                     ('groups', lambda ar, v: tarray(ar, v, group)),
                     ('bounds', bounds), ('octrees', counted(octree)), ('indices', counted(index)),
                     ('u1', F('u32')), ('u2', F('u32')), ('u3', F('u32'))])


def load(data, version):
    ar = charmsh.Archive(data)
    ar.version = version
    s = scene(ar, None)
    if ar.pos != len(data):
        raise ValueError('scene tail: %d bytes left at %#x' % (len(data) - ar.pos, ar.pos))
    return s


def save(s, version):
    ar = charmsh.Archive()
    ar.version = version
    scene(ar, s)
    return bytes(ar.out)
