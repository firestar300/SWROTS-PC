#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read and write the game's character models (.msh, skinned, with bones), field by field as the game reads them.

    python tools/meshes/charmsh.py info     anakincloak.msh [--verts]   structure: meshes, groups, bones, materials
    python tools/meshes/charmsh.py selftest anakincloak.msh ...         read and write back, byte for byte

The input comes from the user's own copy of the game (see docs/modding/dumping-assets.md). The layout is in
docs/modding/file-formats.md ("Character meshes"); every field is read in the order the game's own reader reads it
(TMMSHFile, EMClasses.cpp in the game's code). Tested on the character models of the Jedi Temple and Mustafar
levels (versions 0x9001D to 0xA0042): all are rebuilt byte for byte.

As a library: load(path) -> a dict tree, save(tree) -> bytes; vertices(group) gives each vertex's position, bone
weights and palette indices.
"""
import struct
import sys


class Archive:
    """The game's archive: a bool is 4 bytes; TString = u32 length + characters; TArray = i32 capacity (kept as
    stored), i32 count, then the elements."""

    def __init__(self, data=None):
        self.loading = data is not None
        self.data = data
        self.pos = 0
        self.out = bytearray()
        self.version = 0

    def _io(self, fmt, value=None):
        if self.loading:
            v = struct.unpack_from(fmt, self.data, self.pos)
            self.pos += struct.calcsize(fmt)
            return v[0] if len(v) == 1 else list(v)
        self.out += struct.pack(fmt, value)
        return value

    def u32(self, v=None): return self._io('<I', v)
    def i32(self, v=None): return self._io('<i', v)
    def u16(self, v=None): return self._io('<H', v)
    def s16(self, v=None): return self._io('<h', v)
    def u8(self, v=None): return self._io('<B', v)
    def f32(self, v=None): return self._io('<f', v)

    def b32(self, v=None):  # a bool, stored as 4 bytes (the raw value is kept)
        return self._io('<I', v)

    def raw(self, n, v=None):
        if self.loading:
            b = bytes(self.data[self.pos:self.pos + n])
            if len(b) != n:
                raise EOFError('raw %d at %#x' % (n, self.pos))
            self.pos += n
            return b
        assert len(v) == n
        self.out += v
        return v

    def string(self, v=None):
        n = self.u32(None if self.loading else len(v))
        return self.raw(n, None if self.loading else v)


def tarray(ar, arr, elem):
    """TArray: i32 capacity (as stored), i32 count, then each element via elem(ar, value_or_None)."""
    if ar.loading:
        cap = ar.i32()
        n = ar.i32()
        return {'cap': cap, 'items': [elem(ar, None) for _ in range(n)]}
    ar.i32(arr['cap'])
    ar.i32(len(arr['items']))
    for it in arr['items']:
        elem(ar, it)
    return arr


def S(ar, obj, fields):
    """Serialises a dict's fields in order: fields = [(name, fn)], fn(ar, value_or_None) -> value."""
    o = {} if ar.loading else obj
    for name, fn in fields:
        v = fn(ar, None if ar.loading else o[name])
        if ar.loading:
            o[name] = v
    return o


def F(method, *args):
    return lambda ar, v: getattr(ar, method)(*args, v) if args else getattr(ar, method)(v)


# --- render data ---
def prim(ar, p):
    """A triangle list: its bone palette, one triangle strip, two bools, the vertices it uses (each list takes the
    next run of the group's vertices), its triangle count (signed 16-bit) and a u32 copied as it is."""
    return S(ar, p, [
        ('palette', lambda ar, v: tarray(ar, v, F('u16'))),
        ('indices', lambda ar, v: tarray(ar, v, F('u16'))),
        ('b22', F('b32')), ('b23', F('b32')), ('u18', F('u16')),
        ('s20', lambda ar, v: ar.s16(v) if ar.version >= 0x9001C else None),
        ('u1c', lambda ar, v: ar.u32(v) if ar.version > 0x90014 else None),
    ])


def render_data(ar, r):
    o = {} if ar.loading else r
    present = ar.b32(None if ar.loading else o['present'])
    o['present'] = present
    if not present:
        return o
    fmt = ar.raw(35, None if ar.loading else o['format'])
    o['format'] = fmt
    stride = struct.unpack('b', fmt[0x22:0x23])[0]
    n = ar.i32(None if ar.loading else o['count'])
    o['count'] = n
    o['stride'] = stride
    o['vertices'] = ar.raw(n * stride, None if ar.loading else o['vertices'])
    o['prims'] = tarray(ar, None if ar.loading else o['prims'], prim)
    return o


def layer(ar, l):
    return S(ar, l, [('u8', F('u8')), ('uv', lambda ar, v: [ar.s16(None if v is None else v[i]) for i in range(4)]),
                     ('texture', F('u32'))])


def material(ar, m):
    return S(ar, m, [('shader', F('u32')), ('layers', lambda ar, v: [layer(ar, None if v is None else v[i]) for i in range(4)])])


def group(ar, g):
    if ar.version < 0x90011:
        raise NotImplementedError('group format before 0x90011')
    return S(ar, g, [('flags', F('u32')), ('colour', F('u32')), ('f8', F('f32')), ('fc', F('f32')), ('u72', F('u8')),
                     ('material', material), ('render', render_data)])


def string_table(ar, t):
    o = {} if ar.loading else t
    for k in ('total', 'x', 'used'):
        v = ar.u32(None if ar.loading else o[k])
        o[k] = v
    o['offsets'] = tarray(ar, None if ar.loading else o['offsets'], F('u32'))
    o['text'] = ar.raw(o['used'], None if ar.loading else o['text'])
    return o


def table_names(t):
    out = []
    for off in t['offsets']['items']:
        out.append(t['text'][off:t['text'].index(b'\0', off)].decode('latin1'))
    return out


def embedded(ar, e):
    raise NotImplementedError('embedded STX images')


def attachment(ar, a):
    return S(ar, a, [('type', F('u32')), ('i', F('i32')), ('m', F('raw', 48))])


def body(ar, b):
    o = {} if ar.loading else b
    if ar.version < 0x52033:
        raise NotImplementedError('mesh body before 0x52033')
    o['newFormat'] = ar.b32(None if ar.loading else o['newFormat'])
    if not o['newFormat']:
        raise NotImplementedError('legacy mesh body')
    o['appearances'] = tarray(ar, None if ar.loading else o['appearances'], lambda ar, v: (_ for _ in ()).throw(
        NotImplementedError('legacy appearances')))
    if ar.version < 0x90003:
        raise NotImplementedError('mesh body before 0x90003')
    o['groups'] = tarray(ar, None if ar.loading else o['groups'], group)
    o['textures'] = string_table(ar, None if ar.loading else o['textures'])
    o['materials'] = string_table(ar, None if ar.loading else o['materials']) if ar.version >= 0x90008 else None
    o['images'] = tarray(ar, None if ar.loading else o['images'], embedded)
    o['attachments'] = tarray(ar, None if ar.loading else o['attachments'], attachment)
    return o


def mesh(ar, m):
    o = {} if ar.loading else m
    if ar.version < 0x80006:
        raise NotImplementedError('mesh before 0x80006')

    def io(name, fn):
        o[name] = fn(ar, None if ar.loading else o[name])

    io('name', F('raw', 32))
    io('centre', F('raw', 12))
    io('radius', F('f32'))
    if ar.version < 0x5203C:
        raise NotImplementedError('mesh before 0x5203C')
    io('b5e', F('b32'))
    io('b5c', F('b32'))
    io('b5d', F('b32'))
    io('b5f', F('b32'))
    io('colour', F('u32'))
    io('i6c', F('i32'))  # the mesh's triangle total
    io('b67', F('b32'))
    io('body', body)
    io('bmin', F('raw', 12))
    io('bmax', F('raw', 12))
    io('b63', F('b32'))
    if ar.version >= 0x52049 and o['b63']:
        io('pairs', lambda ar, v: tarray(ar, v, lambda ar, p: [ar.u32(None if p is None else p[0]),
                                                               ar.u32(None if p is None else p[1])]))
        io('x120', F('raw', 12))
        io('x12c', F('raw', 12))
    io('b64', F('b32'))
    if ar.version >= 0x90007:
        io('b65', F('b32'))
    return o


def bone(ar, b):
    return S(ar, b, [('name', F('raw', 32)), ('matrix', F('raw', 48)), ('parent', F('s16'))])


def model(ar, md):
    o = {} if ar.loading else md

    def io(name, fn):
        o[name] = fn(ar, None if ar.loading else o[name])

    io('name', F('string'))
    io('b5d', F('b32'))
    io('meshes', lambda ar, v: tarray(ar, v, mesh))
    if ar.version < 0x48001:
        raise NotImplementedError('legacy bones')
    io('bones', lambda ar, v: tarray(ar, v, bone))
    io('matrix', F('raw', 48))
    if ar.version >= 0x9000F:
        io('matrix2', F('raw', 48))
    if ar.version >= 0x52030:
        io('flags', lambda ar, v: tarray(ar, v, F('b32')))
    return o


def msh(ar, f):
    o = {} if ar.loading else f
    o['magic'] = ar.u32(None if ar.loading else o['magic'])
    if o['magic'] != 0xF8C203A1:
        raise ValueError('not a mesh')
    o['version'] = ar.u32(None if ar.loading else o['version'])
    ar.version = o['version']
    o['header'] = tarray(ar, None if ar.loading else o['header'],
                         lambda ar, h: S(ar, h, [('name', F('string')),
                                                 ('names', lambda ar, v: tarray(ar, v, F('string')))]))
    o['models'] = tarray(ar, None if ar.loading else o['models'], model)
    if ar.version >= 0x5202F:
        if ar.loading:
            o['meshFlags'] = [[ar.b32() for _ in md['meshes']['items']] for md in o['models']['items']]
        else:
            for flags in o['meshFlags']:
                for fl in flags:
                    ar.b32(fl)
    return o


def load(path):
    data = open(path, 'rb').read()
    ar = Archive(data)
    m = msh(ar, None)
    if ar.pos != len(data):
        raise ValueError('%s: %d bytes left after %#x' % (path, len(data) - ar.pos, ar.pos))
    return m


def save(m):
    ar = Archive()
    ar.version = m['version']
    msh(ar, m)
    return bytes(ar.out)


def cname(b):
    return b.split(b'\0')[0].decode('latin1')


def element_offsets(fmt):
    """The vertex format's element offsets (35 bytes; bytes 0x16..0x20 are 11 slots, 0xFF absent; 0x21 the element
    count, 0x22 the stride; bytes before 0x16 are partly uninitialised). Slots: 0 position, 1 normal (4 x u8), 3 uv,
    4 tangent (stride 44), 6 bone weights (4 x u8), 7 bone indices (4 x u8, palette index x 3), 10 colour/unused."""
    return [None if b == 0xFF else b for b in fmt[0x16:0x21]]


def vertices(g):
    """(position, weights[4], palette indices[4]) per vertex; the indices already divided by 3."""
    r = g['render']
    off = element_offsets(r['format'])
    out = []
    v = r['vertices']
    for i in range(r['count']):
        o = i * r['stride']
        pos = struct.unpack_from('<3f', v, o + (off[0] or 0))
        w = list(v[o + off[6]:o + off[6] + 4]) if off[6] is not None else [255, 0, 0, 0]
        idx = [x // 3 for x in v[o + off[7]:o + off[7] + 4]] if off[7] is not None else [0, 0, 0, 0]
        out.append((pos, w, idx))
    return out


def info(path, show_verts=False):
    m = load(path)
    print('%s: version %#x' % (path, m['version']))
    for h in m['header']['items']:
        print('  header %s: %s' % (h['name'].decode(), [n.decode() for n in h['names']['items']]))
    for md in m['models']['items']:
        bones = [cname(b['name']) for b in md['bones']['items']]
        print('  model %s: %d meshes, %d bones' % (md['name'].decode(), len(md['meshes']['items']), len(bones)))
        print('    bones: %s' % ', '.join('%d:%s(%d)' % (i, n, b['parent']) for i, (n, b) in enumerate(zip(bones, md['bones']['items']))))
        for me in md['meshes']['items']:
            b = me['body']
            mats = table_names(b['materials']) if b['materials'] else []
            texs = table_names(b['textures'])
            print('    mesh %s: %d groups; textures %s; materials %s; attachments %d' % (
                cname(me['name']), len(b['groups']['items']), texs, mats, len(b['attachments']['items'])))
            for gi, g in enumerate(b['groups']['items']):
                r = g['render']
                if not r['present']:
                    print('      group %d: no render data' % gi)
                    continue
                tex = [l['texture'] for l in g['material']['layers']]
                print('      group %d (%s): flags %#x shader %d tex %s, %d vertices (stride %d), %d lists' % (
                    gi, mats[gi] if gi < len(mats) else '?', g['flags'], g['material']['shader'], tex, r['count'],
                    r['stride'], len(r['prims']['items'])))
                for p in r['prims']['items']:
                    pal = p['palette']['items']
                    print('        list: %d strip indices, palette %s' % (
                        len(p['indices']['items']), [bones[x] if x < len(bones) else x for x in pal]))
                if show_verts:
                    for vi, (pos, w, idx) in enumerate(vertices(g)[:8]):
                        print('        v%d %s w %s idx %s' % (vi, ['%.2f' % c for c in pos], w, idx))


def selftest(paths):
    ok = True
    for p in paths:
        data = open(p, 'rb').read()
        try:
            same = save(load(p)) == data
        except Exception as e:  # report and go on
            print('FAIL %s: %s' % (p, e))
            ok = False
            continue
        print('%s %s (%d bytes)' % ('OK  ' if same else 'DIFF', p, len(data)))
        ok &= same
    return ok


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == 'info':
        info(sys.argv[2], '--verts' in sys.argv)
    elif len(sys.argv) >= 3 and sys.argv[1] == 'selftest':
        sys.exit(0 if selftest(sys.argv[2:]) else 1)
    else:
        print(__doc__)
