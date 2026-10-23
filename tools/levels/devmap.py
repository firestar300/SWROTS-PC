#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""devmap.py <in u107 .slp> <out .slp> [steps]: the developer test level, made from the Grievous survival cave
(levels\\u107_uta_grievoussurvival.slp, the PAK's copy). Steps (default all but paths):
  space   the cave sector's box (x 3840..8040, y -3116..120, z 3881..7560) is stretched to SPACE below: every
          coordinate equal to one of the box's limits is moved (world BSP faces and planes, the sector's mesh,
          centre, radius and bounds, its portals, the scene bounds and the octree)
  floor   the floor brush (collision) covers FLOOR; the wall brushes are lifted out of the way
  objects only the objects a level needs are kept (see KEEP); the static manager keeps the cave and sky entries
  statics the static manager keeps the cave's entry (its model is then the port's flat floor) and the sky's, the
          floor lit by ambient light
  spawn   the player's start (IPointSpawnPlayer) in the middle of the floor, or at x:z given after the steps
  lighting the point lights that light the characters moved over the floor, the characters' ambient raised
  paths   the path nodes (AI navigation) removed: the AI chases and fights without them on open ground
"""
import math
import struct
import sys

sys.path.insert(0, __import__('os').path.dirname(__file__))
sys.path.insert(0, __import__('os').path.join(__import__('os').path.dirname(__file__), '..', 'meshes'))
import charmsh
import slp

OLD = {'x': (3840.0, 8040.0), 'y': (-3115.924, 120.0), 'z': (3881.164, 7560.0)}
SPACE = {'x': (1500.0, 24000.0), 'y': (-3115.924, 1100.0), 'z': (2700.0, 13500.0)}
FLOOR = {'x': (1500.0, 24000.0), 'z': (2700.0, 13500.0)}  # the whole sector box; top stays at y -2557.5
KEEP = {b'IStaticManager', b'ILightManager', b'IBrushManager', b'ITrivialCharacterPhysicsManager',
        b'ILensFlareSlayerManager', b'ISectorSkybox', b'ISectorInfo', b'ILevelProperties', b'IMapIndexer',
        b'IPointSpawnPlayer', b'ILightStatic', b'IInvisibleBrush'}
AXES = 'xyz'


def remap(v, axis):
    for old, new in zip(OLD[axis], SPACE[axis]):
        if abs(v - old) < 0.01:
            return new
    return v


def remap_point(b):
    x, y, z = struct.unpack('<3f', b)
    return struct.pack('<3f', remap(x, 'x'), remap(y, 'y'), remap(z, 'z'))


def remap_plane(b):
    nx, ny, nz, d = struct.unpack('<4f', b)
    n = (nx, ny, nz)
    for k, axis in enumerate(AXES):
        if abs(abs(n[k]) - 1.0) < 1e-4:
            s = 1.0 if n[k] > 0 else -1.0
            d = s * remap(s * d, axis)
    return struct.pack('<4f', nx, ny, nz, d)


def remap_hedron(b):
    b = bytearray(b)
    for i in range(b[1]):
        o = 4 + 12 * i
        b[o:o + 12] = remap_point(bytes(b[o:o + 12]))
    return bytes(b)


def remap_tree(t):
    t['polyhedra']['items'] = [remap_hedron(b) for b in t['polyhedra']['items']]
    t['planes']['items'] = [remap_plane(p) for p in t['planes']['items']]


def stretch_space(f):
    remap_tree(f['world'])
    sc = f['scene']
    sec = sc['sectors']['items'][0]
    m = sec['mesh']
    m['bmin'] = remap_point(m['bmin'])
    m['bmax'] = remap_point(m['bmax'])
    lo, hi = struct.unpack('<3f', m['bmin']), struct.unpack('<3f', m['bmax'])
    centre = [(a + b) / 2 for a, b in zip(lo, hi)]
    radius = math.sqrt(sum(((b - a) / 2) ** 2 for a, b in zip(lo, hi)))
    m['centre'] = struct.pack('<3f', *centre)
    m['radius'] = radius
    for g in m['body']['groups']['items']:
        r = g['render']
        v = bytearray(r['vertices'])
        for i in range(r['count']):
            o = i * r['stride']
            v[o:o + 12] = remap_point(bytes(v[o:o + 12]))
        r['vertices'] = bytes(v)
    for p in sec['portals']['items']:
        p['points']['items'] = [remap_point(q) for q in p['points']['items']]
        nx, ny, nz, d = struct.unpack('<4f', remap_plane(p['normal'] + struct.pack('<f', p['d'])))
        p['d'] = d
    if OPEN_SKY:
        # the sky is drawn through the sector's portals; the cave had them only on its open sides (west, south,
        # top): portals on the other three sides too, and the sector's own wall faces made empty
        (x0, y0, z0), (x1, y1, z1) = lo, hi
        proto = sec['portals']['items'][0]

        def pt(x, y, z):
            return struct.pack('<3f', x, y, z)

        def add(points, normal, d):
            p = dict(proto)
            p['points'] = {'cap': 3, 'items': points}
            p['normal'] = struct.pack('<3f', *normal)
            p['d'] = d
            sec['portals']['items'].append(p)
        add([pt(x1, y1, z0), pt(x1, y1, z1), pt(x1, y0, z1)], (-1.0, 0.0, 0.0), -x1)
        add([pt(x1, y0, z0), pt(x1, y1, z0), pt(x1, y0, z1)], (-1.0, 0.0, 0.0), -x1)
        add([pt(x0, y1, z1), pt(x0, y0, z1), pt(x1, y0, z1)], (0.0, 0.0, -1.0), -z1)
        add([pt(x1, y1, z1), pt(x0, y1, z1), pt(x1, y0, z1)], (0.0, 0.0, -1.0), -z1)
        sec['portals']['cap'] = max(sec['portals']['cap'], len(sec['portals']['items']))
        for g in m['body']['groups']['items']:
            r = g['render']
            v = bytearray(r['vertices'])
            for i in range(r['count']):
                v[i * r['stride']:i * r['stride'] + 12] = pt(x0, y0, z0)
            r['vertices'] = bytes(v)
    b = sc['bounds']
    b['points'] = [b['points'][0], remap_point(b['points'][1]), remap_point(b['points'][2])]
    o = sc['octrees'][0]
    d = bytearray(o['data'])
    d[52:64] = remap_point(bytes(d[52:64]))
    d[64:76] = remap_point(bytes(d[64:76]))
    struct.pack_into('<3f', d, 76, *centre)
    struct.pack_into('<f', d, 88, radius)
    struct.pack_into('<f', d, 4, max(b - a for a, b in zip(lo, hi)))
    o['data'] = bytes(d)
    print('space: sector box %s .. %s' % (['%.0f' % x for x in lo], ['%.0f' % x for x in hi]))


def set_floor(f):
    brushes = [t for t in f['trees2']['items'] if t]

    def top(t):
        return max(v[1] for b in t['polyhedra']['items'] for v in slp.hedron_verts(b))
    floor = min(brushes, key=top)  # the floor: the brush with the lowest top
    for t in brushes:
        items = []
        for b in t['polyhedra']['items']:
            b = bytearray(b)
            for i in range(b[1]):
                o = 4 + 12 * i
                x, y, z = struct.unpack_from('<3f', b, o)
                if t is floor:
                    cx, cz = 6296.0, 6282.0
                    x = FLOOR['x'][1] if x > cx else FLOOR['x'][0]
                    z = FLOOR['z'][1] if z > cz else FLOOR['z'][0]
                else:
                    y += 100000.0
                struct.pack_into('<3f', b, o, x, y, z)
            items.append(bytes(b))
        t['polyhedra']['items'] = items
        planes = []
        for p in t['planes']['items']:
            nx, ny, nz, d = struct.unpack('<4f', p)
            if t is floor:
                if abs(nx) > 0.5:
                    d = nx * (FLOOR['x'][1] if nx > 0 else FLOOR['x'][0])
                elif abs(nz) > 0.5:
                    d = nz * (FLOOR['z'][1] if nz > 0 else FLOOR['z'][0])
            else:
                d += ny * 100000.0
            planes.append(struct.pack('<4f', nx, ny, nz, d))
        t['planes']['items'] = planes
    print('floor: brush at %#x covers x %s z %s; %d wall brushes lifted' % (floor['at'], FLOOR['x'], FLOOR['z'],
                                                                           len(brushes) - 1))


def keep_objects(f):
    items = f['objects']['items']
    kept = [o for o in items if o['cls'] in KEEP or (o['cls'] == b'IStatic' and
                                                      slp.get(o, b'Skybox Render Priority') is not None)]
    print('objects: %d of %d kept: %s' % (len(kept), len(items), sorted(set(o['cls'].decode() for o in kept))))
    f['objects']['items'] = kept


FLOOR_Y = -2557.5
FLOOR_VERTICES = 45 * 22  # devfloor.py's default grid
AMBIENT = (0.8, 0.8, 0.8)


def statics(f):
    """The static manager keeps the floor (the cave's entry, its mesh replaced by the generated floor) and the sky.
    The floor entry: placed at the floor's height, lit by ambient light only; its baked light (the scene's static
    instance of the same light handle) one white entry per floor vertex."""
    mgr = [o for o in f['objects']['items'] if o['cls'] == b'IStaticManager'][0]
    t = slp.tree(slp.get(mgr, b'MGR_StrippedStaticsList'))
    kept = []
    floor_handle = None
    for g in t[1]:
        d = dict(g)
        mesh = struct.unpack('<I', d[b'MGR_ManagedMeshID'])[0]
        if mesh == 6:  # m07_cave -> the floor
            g = [(k, struct.pack('<12f', 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, FLOOR_Y, 0) if k == b'MGR_WorldPosition' else
                  struct.pack('<3f', *AMBIENT) if k == b'MGR_AmbientLight' else
                  struct.pack('<I', 1) if k == b'MGR_Ambientlightingonly' else v) for k, v in g]
            floor_handle = struct.unpack('<I', d[b'MGR___lighthandle'])[0]
            kept.append(g)
        elif mesh == 7:  # the sky dome
            kept.append(g)
    slp.set_prop(mgr, b'MGR_StrippedStaticsList', slp.tree_bytes(('array', kept)))
    for s in f['instances']['items']:
        if s['id'] == floor_handle:
            s['lighting'] = b'\xff\xff\xff\xff' * FLOOR_VERTICES
            s['count1'] = FLOOR_VERTICES
    print('statics: %d of %d managed statics kept (floor light handle %#x)' % (len(kept), len(t[1]), floor_handle))


def spawn(f, x=None, z=None):
    """The player's start (IPointSpawnPlayer's transform) in the middle of the floor."""
    x = (FLOOR['x'][0] + FLOOR['x'][1]) / 2 if x is None else x
    z = (FLOOR['z'][0] + FLOOR['z'][1]) / 2 if z is None else z
    sp = [o for o in f['objects']['items'] if o['cls'] == b'IPointSpawnPlayer'][0]
    m = bytearray(slp.get(sp, b'Transform'))
    struct.pack_into('<3f', m, 36, x, FLOOR_Y + 60.0, z)
    slp.set_prop(sp, b'Transform', bytes(m))
    print('spawn: player start at %.0f %.0f %.0f' % (x, FLOOR_Y + 60.0, z))
    if SKY_REFERENCE:
        si = [o for o in f['objects']['items'] if o['cls'] == b'ISectorInfo'][0]
        m = bytearray(slp.get(si, b'Transform'))
        struct.pack_into('<f', m, 36, x)
        struct.pack_into('<f', m, 44, z)
        slp.set_prop(si, b'Transform', bytes(m))
        print('spawn: sector info moved to %.0f %.0f' % (x, z))


SKY_REFERENCE = True
OPEN_SKY = True


def paths(f):
    """The path system (TPathSystem, the file's last block: u32 size, then a 0x24-byte header of u16 offsets and
    counts: +0x10 link count, +0x12 node count; links; nodes of 20 bytes: x, y, z, u16 first link, u16 links, u32).
    The cave's 6 nodes sit in the cave; the AI does not need them on open ground: both counts are set to 0."""
    e = bytearray(f['end'])
    print('paths: %d links, %d nodes -> none' % struct.unpack_from('<HH', e, 4 + 0x10))
    struct.pack_into('<HH', e, 4 + 0x10, 0, 0)
    f['end'] = bytes(e)


def lighting(f, characters=0.55):
    """ILevelProperties: characters (and other moving meshes) are lit by the level's ambient colours plus nearby
    lights; the cave's lights are far away now, so the ambient is raised."""
    lp = [o for o in f['objects']['items'] if o['cls'] == b'ILevelProperties'][0]
    for name in (b'Character ambient color', b'Dynamic-mesh ambient color'):
        slp.set_prop(lp, name, struct.pack('<3f', characters, characters, characters))
    print('lighting: character / dynamic-mesh ambient %.2f' % characters)
    # the light manager's point lights (what lights characters): moved with the start, from the cave to the middle
    # of the floor, their range made to reach the whole floor
    lm = [o for o in f['objects']['items'] if o['cls'] == b'ILightManager'][0]
    t = slp.tree(slp.get(lm, b'__lights'))
    dx = (FLOOR['x'][0] + FLOOR['x'][1]) / 2 - 6296.0
    dz = (FLOOR['z'][0] + FLOOR['z'][1]) / 2 - 6282.0
    lights = []
    for g in t[1]:
        g = [(k, struct.pack('<f', struct.unpack('<f', v)[0] + dx) if k == b'__x' else
              struct.pack('<f', struct.unpack('<f', v)[0] + dz) if k == b'__z' else
              struct.pack('<I', LIGHT_RANGE) if k == b'__rng' else v) for k, v in g]
        lights.append(g)
    slp.set_prop(lm, b'__lights', slp.tree_bytes(('array', lights)))
    print('lighting: %d lights moved by %.0f, %.0f, range %d' % (len(lights), dx, dz, LIGHT_RANGE))


LIGHT_RANGE = 12000


def main():
    src, dst = sys.argv[1:3]
    steps = sys.argv[3].split(',') if len(sys.argv) > 3 else ['space', 'floor', 'objects', 'statics', 'spawn', 'lighting']
    f, r = slp.load(src)
    if 'statics' in steps:
        statics(f)
    if 'spawn' in steps:
        at = [float(v) for v in sys.argv[4].split(':')] if len(sys.argv) > 4 else [None, None]
        spawn(f, *at)
    if 'lighting' in steps:
        lighting(f)
    if 'paths' in steps:
        paths(f)
    if 'space' in steps:
        stretch_space(f)
    if 'floor' in steps:
        set_floor(f)
    if 'objects' in steps:
        keep_objects(f)
    data = slp.save(f)
    open(dst, 'wb').write(data)
    slp.load(dst)
    print('wrote %s (%d bytes)' % (dst, len(data)))


if __name__ == '__main__':
    main()
