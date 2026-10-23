#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""slp.py: the game's level files (levels\\<level>.slp, the engine's TMeshWorld file), read as the game's own reader
(SceneMeshWorld.cpp, 0x5D810) reads them, and written back byte for byte.

Parsed: the header; the scene's sector volumes (BSP trees), a flag per tree, the brushes (BSP trees in world
coordinates: the level's collision), the static mesh instances (mesh path + baked per-vertex lighting), the world
BSP tree; the scene's tail (sectors, portals, bounds, octrees, index lists) through slpscene.py (the Grievous
Survival level; the older sector meshes of M08 and M09A are kept as bytes); the objects (class, properties,
trailer; tree() shows a property's data as the editor's tree); the path system (kept as bytes). The object
section is found by its first class name (find_objects).

    python tools/levels/slp.py map <file.slp>          what has been parsed, with offsets
    python tools/levels/slp.py selftest <file.slp>...  load + save gives the same bytes
"""
import struct
import sys

sys.path.insert(0, __import__('os').path.dirname(__file__))
sys.path.insert(0, __import__('os').path.join(__import__('os').path.dirname(__file__), '..', 'meshes'))


class R:
    def __init__(self, data):
        self.d = data
        self.p = 0
        self.version = 0

    def u8(self):
        v = self.d[self.p]
        self.p += 1
        return v

    def u16(self):
        v = struct.unpack_from('<H', self.d, self.p)[0]
        self.p += 2
        return v

    def u32(self):
        v = struct.unpack_from('<I', self.d, self.p)[0]
        self.p += 4
        return v

    def i32(self):
        v = struct.unpack_from('<i', self.d, self.p)[0]
        self.p += 4
        return v

    def f32(self):
        v = struct.unpack_from('<f', self.d, self.p)[0]
        self.p += 4
        return v

    def raw(self, n):
        if n < 0 or self.p + n > len(self.d):
            raise ValueError('raw %d at %#x' % (n, self.p))
        v = self.d[self.p:self.p + n]
        self.p += n
        return v

    def string(self):
        return self.raw(self.u32())


class W:
    def __init__(self):
        self.out = bytearray()

    def u8(self, v):
        self.out.append(v)

    def u16(self, v):
        self.out += struct.pack('<H', v)

    def u32(self, v):
        self.out += struct.pack('<I', v)

    def i32(self, v):
        self.out += struct.pack('<i', v)

    def raw(self, v):
        self.out += v

    def string(self, v):
        self.u32(len(v))
        self.out += v


def tarray(r, elem, limit=1000000):
    cap = r.i32()
    n = r.i32()
    if n < 0 or n > limit:
        raise ValueError('array count %d at %#x' % (n, r.p - 4))
    return {'cap': cap, 'items': [elem(r) for _ in range(n)]}


def put_tarray(w, a, elem):
    w.i32(a['cap'])
    w.i32(len(a['items']))
    for v in a['items']:
        elem(w, v)


def blob(r):
    """A size-prefixed block (the size counts itself)."""
    size = r.u32()
    return r.raw(size - 4)


def put_blob(w, b):
    w.u32(len(b) + 4)
    w.raw(b)


def bsp_extra(r):
    """0x56CF0 (current versions): bool; counts a and b; a x 10 bytes; b x 4 bytes; c; c x 32 bytes."""
    present = r.u32()
    if not present:
        return None
    a = r.u32()
    b = r.u32()
    ra = r.raw(a * 10)
    rb = r.raw(b * 4)
    c = r.u32()
    rc = r.raw(c * 0x20)
    return {'a': a, 'b': b, 'c': c, 'ra': ra, 'rb': rb, 'rc': rc}


def put_bsp_extra(w, e):
    if e is None:
        w.u32(0)
        return
    w.u32(1)
    w.u32(len(e['ra']) // 10)
    w.u32(len(e['rb']) // 4)
    w.raw(e['ra'])
    w.raw(e['rb'])
    w.u32(len(e['rc']) // 0x20)
    w.raw(e['rc'])


def bsp_tree(r):
    """0x57840: byte present; hedrons (convex polyhedra, size-prefixed); nodes (8 bytes each); planes (16 bytes);
    0x56CF0; blobs. A hedron: u8 faces, vertices, edges, planes; vertices (3 x f32); edges, faces (indices)."""
    start = r.p
    if not r.u8():
        return None
    t = {'at': start, 'polyAt': []}

    def hedron(r):
        t['polyAt'].append(r.p + 4)
        return blob(r)
    t['polyhedra'] = tarray(r, hedron)
    n = r.u32()
    t['nodes'] = r.raw(n * 8)
    t['planesAt'] = r.p + 8  # 16 bytes each: normal (3 x f32), d (n . p = d on the plane)
    t['planes'] = tarray(r, lambda r: r.raw(16))
    t['extra'] = bsp_extra(r)
    t['blobs'] = tarray(r, blob)
    t['end'] = r.p
    return t


def put_bsp_tree(w, t):
    if t is None:
        w.u8(0)
        return
    w.u8(1)
    put_tarray(w, t['polyhedra'], put_blob)
    w.u32(len(t['nodes']) // 8)
    w.raw(t['nodes'])
    put_tarray(w, t['planes'], W.raw)
    put_bsp_extra(w, t['extra'])
    put_tarray(w, t['blobs'], put_blob)


def instance(r):
    """0x5AC80: a static mesh instance. A record whose u16 is not -1 ends there (its data is elsewhere: m08/m09a
    keep most statics so, the game's flag 0x6778AC set); else: u32 sector?, bool, u32 colour, vertex counts (x2),
    baked lighting (4 bytes per vertex of the mesh), u16 array (sectors?), u32 pair."""
    s = {'at': r.p, 'id': r.u32(), 'mesh': r.string(), 'ref': r.u16()}
    if s['ref'] != 0xFFFF:
        return s
    s['a'] = r.u32()
    s['flag'] = r.u32()
    s['colour'] = r.u32()
    s['count1'] = r.u32()
    n = r.u32()
    s['lighting'] = r.raw(n * 4)
    s['ucap'] = r.u32()
    s['shorts'] = r.raw(r.u32() * 2)
    s['tail'] = (r.u32(), r.u32())
    return s


def put_instance(w, s):
    w.u32(s['id'])
    w.string(s['mesh'])
    w.u16(s['ref'])
    if s['ref'] != 0xFFFF:
        return
    w.u32(s['a'])
    w.u32(s['flag'])
    w.u32(s['colour'])
    w.u32(s['count1'])
    w.u32(len(s['lighting']) // 4)
    w.raw(s['lighting'])
    w.u32(s['ucap'])
    w.u32(len(s['shorts']) // 2)
    w.raw(s['shorts'])
    w.u32(s['tail'][0])
    w.u32(s['tail'][1])


def header(r):
    return {'tag': r.string(), 'version': r.u32(), 'path': r.string(), 'c': r.u32(), 'b': r.u32()}


def put_header(w, h):
    w.string(h['tag'])
    w.u32(h['version'])
    w.string(h['path'])
    w.u32(h['c'])
    w.u32(h['b'])


def load(path):
    r = R(open(path, 'rb').read())
    f = {'header': header(r)}
    r.version = f['header']['version']
    f['hasWorld'] = r.u32()   # +0x10: the BSP mesh world (none in levels built of props)
    if f['hasWorld']:
        raise NotImplementedError('BSP mesh world (0x587C0)')
    f['hasScene'] = r.u32()   # +0x14: the scene (0x5C7E0)
    f['sceneAt'] = r.p
    f['trees'] = tarray(r, bsp_tree)          # +0x00 (0x59750): the sectors' volumes (sector-local)
    f['flags'] = tarray(r, lambda r: r.u32())  # +0x0C (0x28570): a bool per sector
    f['trees2'] = tarray(r, bsp_tree)         # +0x18 (0x59750): the brushes = the collision (world coordinates)
    f['instances'] = tarray(r, instance)      # +0x24 (0x5B190): static mesh instances
    f['world'] = bsp_tree(r)                  # +0xA0 (0x57840): the world BSP (sector partition)
    f['restAt'] = r.p
    at = find_objects(r.d, r.p)
    f['sceneTail'] = r.raw(at - r.p)          # sectors, portals, octrees (slpscene.py)
    try:
        import slpscene
        f['scene'] = slpscene.load(f['sceneTail'], f['header']['version'])
    except Exception:  # m08/m09a sectors: a mesh body form slpscene does not know yet
        f['scene'] = None
    f['objectsAt'] = r.p
    f['objects'] = objects(r)
    f['endAt'] = r.p
    f['end'] = r.raw(len(r.d) - r.p)          # the path-node block (0x984A0): u32 size + data
    return f, r


def find_objects(d, start):
    """The object section: u32 version (the file's), u32 next free id?, u32 object count, then the first object's
    class name (a string starting with I)."""
    ver = d[4 + struct.unpack_from('<I', d, 0)[0]:][:4]
    p = d.find(ver, start)
    while p >= 0:
        n = struct.unpack_from('<I', d, p + 12)[0]
        if 1 < n < 64 and d[p + 16:p + 17] == b'I' and all(32 <= c < 127 for c in d[p + 16:p + 16 + n]):
            return p
        p = d.find(ver, p + 1)
    raise ValueError('no object section')


def prop(r):
    """A property: name, u32 size (counting itself), data (its type's; a group's is u32 count + child properties)."""
    at = r.p
    name = r.string()
    size = r.u32()
    return {'at': at, 'name': name, 'data': r.raw(size - 4)}


def put_prop(w, p):
    w.string(p['name'])
    w.u32(len(p['data']) + 4)
    w.raw(p['data'])


def obj(r):
    """An object: class name, u32 size (counting itself: to the object's end), u32 property count, properties, then
    what follows them up to the size (8 bytes: two u32, 0 in almost every object)."""
    o = {'at': r.p, 'cls': r.string()}
    start = r.p
    size = r.u32()
    o['props'] = [prop(r) for _ in range(r.u32())]
    o['tail'] = r.raw(start + size - r.p)
    return o


def put_obj(w, o):
    w.string(o['cls'])
    body = W()
    body.u32(len(o['props']))
    for p in o['props']:
        put_prop(body, p)
    body.raw(o['tail'])
    w.u32(len(body.out) + 4)
    w.raw(body.out)


def objects(r):
    s = {'version': r.u32(), 'nextId': r.u32()}
    n = r.u32()
    s['items'] = [obj(r) for _ in range(n)]
    return s


def put_objects(w, s):
    w.u32(s['version'])
    w.u32(s['nextId'])
    w.u32(len(s['items']))
    for o in s['items']:
        put_obj(w, o)


def _props_at(d, p, n):
    """n properties from p, or None when the bytes are not properties; returns (list, end)."""
    out = []
    for _ in range(n):
        if p + 4 > len(d):
            return None
        ln = struct.unpack_from('<I', d, p)[0]
        if not 0 < ln < 80 or p + 8 + ln > len(d) or not all(32 <= c < 127 for c in d[p + 4:p + 4 + ln]):
            return None
        size = struct.unpack_from('<I', d, p + 4 + ln)[0]
        if size < 4 or p + 4 + ln + size > len(d):
            return None
        out.append((d[p + 4:p + 4 + ln], d[p + 8 + ln:p + 4 + ln + size]))
        p += 4 + ln + size
    return out, p


def tree(data):
    """A property's data as the editor's property tree, where it looks like one: a group (u32 count + properties)
    -> ('group', [(name, tree)]); an array of groups (u32 count, each u32 count + properties) -> ('array', [...]);
    else the bytes."""
    if len(data) >= 4:
        n = struct.unpack_from('<I', data, 0)[0]
        if 0 < n < 10000:
            g = _props_at(data, 4, n)
            if g and g[1] == len(data):
                return ('group', [(k, tree(v)) for k, v in g[0]])
            p, items = 4, []
            for _ in range(n):
                if p + 4 > len(data):
                    break
                m = struct.unpack_from('<I', data, p)[0]
                g = _props_at(data, p + 4, m) if 0 < m < 10000 else None
                if not g:
                    break
                items.append([(k, tree(v)) for k, v in g[0]])
                p = g[1]
            if len(items) == n and p == len(data):
                return ('array', items)
    return data


def tree_bytes(t):
    """The data of a tree() result."""
    if isinstance(t, (bytes, bytearray)):
        return bytes(t)
    kind, items = t
    w = W()

    def group(props):
        w.u32(len(props))
        for k, v in props:
            data = tree_bytes(v)
            w.string(k)
            w.u32(len(data) + 4)
            w.raw(data)
    if kind == 'group':
        group(items)
    else:
        w.u32(len(items))
        for g in items:
            group(g)
    return bytes(w.out)


def set_prop(o, name, data):
    for p in o['props']:
        if p['name'] == name:
            p['data'] = data
            return
    raise KeyError(name)


def show_tree(t, indent='  ', limit=None):
    if isinstance(t, bytes):
        txt = t[4:].decode('latin1') if len(t) > 4 and struct.unpack_from('<I', t, 0)[0] == len(t) - 4 else None
        if txt is not None and all(32 <= ord(c) < 127 for c in txt):
            return [indent + repr(txt)]
        return [indent + (t.hex() if len(t) <= 48 else t[:48].hex() + '.. (%d bytes)' % len(t))]
    kind, items = t
    lines = []
    if kind == 'group':
        for k, v in items:
            sub = show_tree(v, indent + '  ', limit)
            if len(sub) == 1 and isinstance(v, bytes):
                lines.append('%s%s: %s' % (indent, k.decode('latin1'), sub[0].strip()))
            else:
                lines.append('%s%s:' % (indent, k.decode('latin1')))
                lines += sub
    elif kind == 'array':
        for i, g in enumerate(items[:limit] if limit else items):
            lines.append('%s[%d]' % (indent, i))
            lines += show_tree(('group', g), indent + '  ', limit)
        if limit and len(items) > limit:
            lines.append('%s... %d more' % (indent, len(items) - limit))
    return lines


def get(o, name):
    for p in o['props']:
        if p['name'] == name:
            return p['data']
    return None


def save(f):
    w = W()
    put_header(w, f['header'])
    w.u32(f['hasWorld'])
    w.u32(f['hasScene'])
    put_tarray(w, f['trees'], put_bsp_tree)
    put_tarray(w, f['flags'], W.u32)
    put_tarray(w, f['trees2'], put_bsp_tree)
    put_tarray(w, f['instances'], put_instance)
    put_bsp_tree(w, f['world'])
    if f.get('scene'):
        import slpscene
        w.raw(slpscene.save(f['scene'], f['header']['version']))
    else:
        w.raw(f['sceneTail'])
    put_objects(w, f['objects'])
    w.raw(f['end'])
    return bytes(w.out)


def hedron_verts(b):
    return [struct.unpack_from('<3f', b, 4 + 12 * i) for i in range(b[1])]


def show(path):
    f, r = load(path)
    h = f['header']
    print('%s: version %#x path %s, scene at %#x' % (path, h['version'], h['path'].decode('latin1'), f['sceneAt']))
    for name in ('trees', 'trees2'):
        for i, t in enumerate(f[name]['items']):
            if not t:
                continue
            vs = [v for b in t['polyhedra']['items'] for v in hedron_verts(b)]
            box = [(min(v[k] for v in vs), max(v[k] for v in vs)) for k in range(3)]
            print('  %s %d at %#x: %d hedrons, %d nodes, %d planes; x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f' % (
                'sector' if name == 'trees' else 'brush', i, t['at'], len(t['polyhedra']['items']),
                len(t['nodes']) // 8, len(t['planes']['items']), *box[0], *box[1], *box[2]))
    full = [s for s in f['instances']['items'] if s['ref'] == 0xFFFF]
    print('  %d static instances (%d with lighting here)' % (len(f['instances']['items']), len(full)))
    for s in full[:5]:
        print('    %#x %s, %d vertices lit' % (s['id'], s['mesh'].decode('latin1'), len(s['lighting']) // 4))
    print('  world tree: %d hedrons; scene tail %#x..%#x; %d objects at %#x; end block %d bytes' % (
        len(f['world']['polyhedra']['items']) if f['world'] else 0, f['restAt'], f['objectsAt'],
        len(f['objects']['items']), f['objectsAt'], len(f['end'])))
    for o in f['objects']['items']:
        name = get(o, b'Name')
        print('    %#x %s %s: %s' % (o['at'], o['cls'].decode(), name[4:].decode('latin1') if name else '',
                                     ', '.join(p['name'].decode('latin1') for p in o['props'])))


def selftest(paths):
    ok = True
    for p in paths:
        data = open(p, 'rb').read()
        try:
            f, _ = load(p)
            same = save(f) == data
            print('%s %s (%d bytes, %d objects)' % ('OK  ' if same else 'DIFF', p, len(data), len(f['objects']['items'])))
        except Exception as e:  # report and go on
            print('FAIL %s: %s' % (p, e))
            same = False
        ok &= same
    return ok


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == 'map':
        show(sys.argv[2])
    elif len(sys.argv) >= 3 and sys.argv[1] == 'selftest':
        sys.exit(0 if selftest(sys.argv[2:]) else 1)
    else:
        print(__doc__)
