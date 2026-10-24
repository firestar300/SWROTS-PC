// Shapes added to character models (see meshedit.h). The model is read field by field as the game's own reader reads it
// (TMMSHFile, EMClasses.cpp; tools/meshes/charmsh.py reads and writes the same layout byte for byte); only the body of
// the mesh named "Base", its triangle total and its bounding box are written back, everything else is copied as it is.

#include "game/meshedit.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace swrots::game {

namespace {

// The game's mesh archive: a bool is 4 bytes, a TString a u32 length and its characters, a TArray an i32 capacity
// (kept as stored), an i32 count and the elements.
constexpr uint32_t kMagic = 0xF8C203A1;
constexpr uint32_t kOldestVersion = 0x00090011; // the group layout below
constexpr int kGroupHead = 73; // u32 flags, u32 colour, f32, f32, u8, material: u32 shader, 4 x {u8, 4 x s16, u32 texture}
constexpr int kFormatSize = 35; // vertex format: element offsets at 0x16 (11 slots, 0xFF absent), stride at 0x22
constexpr int kSlotPosition = 0, kSlotNormal = 1, kSlotUv = 3, kSlotWeights = 6, kSlotBones = 7;
// A mesh record: name (32), centre (12), radius, 4 bools, colour, then the mesh's triangle total.
constexpr size_t kTriangleTotalAt = 32 + 12 + 4 + 16 + 4;

// The vertex layout the boxes are written in (the character models' usual one, stride 36): position f32 x 3 at 0,
// normal u8 x 4 at 0xC, uv f32 x 2 at 0x10, bone weights u8 x 4 at 0x18, bone indices u8 x 4 at 0x1C (palette slot x 3,
// a bone being 3 shader constants), 4 bytes at 0x20.
constexpr int kStride = 36;
constexpr int kNormalAt = 0x0C, kUvAt = 0x10, kWeightsAt = 0x18, kBonesAt = 0x1C;

struct Reader {
    const uint8_t* p;
    size_t n;
    size_t pos = 0;
    bool ok = true;
    template <typename T> T Get()
    {
        T v{};
        if (pos + sizeof(T) > n) {
            ok = false;
            return v;
        }
        std::memcpy(&v, p + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    void Skip(size_t k)
    {
        if (k > n || pos + k > n)
            ok = false;
        else
            pos += k;
    }
    std::vector<uint8_t> Raw(size_t k)
    {
        if (k > n || pos + k > n) {
            ok = false;
            return {};
        }
        std::vector<uint8_t> v(p + pos, p + pos + k);
        pos += k;
        return v;
    }
    void SkipString() { Skip(Get<uint32_t>()); }
};

struct Writer {
    std::vector<uint8_t> out;
    template <typename T> void Put(T v)
    {
        const auto* b = reinterpret_cast<const uint8_t*>(&v);
        out.insert(out.end(), b, b + sizeof(T));
    }
    void Raw(const void* d, size_t k)
    {
        const auto* b = static_cast<const uint8_t*>(d);
        out.insert(out.end(), b, b + k);
    }
};

// A triangle list: its bone palette, one triangle strip, two bools, the vertices it uses (each list takes the next run
// of the group's vertices), its triangle count (degenerate ones not counted) and a u32 kept as it is.
struct Prim {
    int32_t paletteCap = 0;
    std::vector<uint16_t> palette;
    int32_t indexCap = 0;
    std::vector<uint16_t> indices;
    uint32_t b22 = 0, b23 = 0;
    uint16_t vertexCount = 0;
    int16_t triangleCount = 0;
    uint32_t u1c = 0;
};

struct Group {
    std::array<uint8_t, kGroupHead> head{};
    uint32_t present = 0;
    std::array<uint8_t, kFormatSize> format{};
    int stride = 0;
    std::vector<uint8_t> vertices;
    int32_t primCap = 0;
    std::vector<Prim> prims;
    int Offset(int slot) const { return format[0x16 + slot] == 0xFF ? -1 : format[0x16 + slot]; }
    int Count() const { return stride ? int(vertices.size() / stride) : 0; }
};

// A string table (texture and material names): sizes, offsets into the text, the text.
struct Table {
    uint32_t total = 0, x = 0, used = 0;
    int32_t cap = 0;
    std::vector<uint32_t> offsets;
    std::vector<uint8_t> text;
    std::string Name(size_t i) const
    {
        if (i >= offsets.size() || offsets[i] >= text.size())
            return {};
        const char* s = reinterpret_cast<const char*>(text.data() + offsets[i]);
        return std::string(s, strnlen(s, text.size() - offsets[i]));
    }
};

struct Body {
    uint32_t newFormat = 0;
    int32_t appearanceCap = 0;
    int32_t groupCap = 0;
    std::vector<Group> groups;
    Table textures, materials;
    int32_t imageCap = 0;
    int32_t attachmentCap = 0;
    int32_t attachmentCount = 0;
    std::vector<uint8_t> attachments; // 56 bytes each
};

struct MeshRecord {
    std::string name;
    size_t begin = 0, bodyBegin = 0, bodyEnd = 0; // the body is followed by the bounding box (min, max)
    Body body;
};

uint32_t g_Version = 0;

bool ReadTable(Reader& r, Table& t)
{
    t.total = r.Get<uint32_t>();
    t.x = r.Get<uint32_t>();
    t.used = r.Get<uint32_t>();
    t.cap = r.Get<int32_t>();
    const int32_t count = r.Get<int32_t>();
    if (!r.ok || count < 0 || count > 4096)
        return false;
    t.offsets.resize(count);
    for (auto& o : t.offsets)
        o = r.Get<uint32_t>();
    t.text = r.Raw(t.used);
    return r.ok;
}

void WriteTable(Writer& w, const Table& t)
{
    w.Put(t.total);
    w.Put(t.x);
    w.Put(t.used);
    w.Put(t.cap);
    w.Put(int32_t(t.offsets.size()));
    for (uint32_t o : t.offsets)
        w.Put(o);
    w.Raw(t.text.data(), t.text.size());
}

bool ReadPrim(Reader& r, Prim& p)
{
    p.paletteCap = r.Get<int32_t>();
    int32_t count = r.Get<int32_t>();
    if (!r.ok || count < 0 || count > 256)
        return false;
    p.palette.resize(count);
    for (auto& b : p.palette)
        b = r.Get<uint16_t>();
    p.indexCap = r.Get<int32_t>();
    count = r.Get<int32_t>();
    if (!r.ok || count < 0 || count > 1000000)
        return false;
    p.indices.resize(count);
    for (auto& i : p.indices)
        i = r.Get<uint16_t>();
    p.b22 = r.Get<uint32_t>();
    p.b23 = r.Get<uint32_t>();
    p.vertexCount = r.Get<uint16_t>();
    if (g_Version >= 0x9001C)
        p.triangleCount = r.Get<int16_t>();
    if (g_Version > 0x90014)
        p.u1c = r.Get<uint32_t>();
    return r.ok;
}

void WritePrim(Writer& w, const Prim& p)
{
    w.Put(p.paletteCap);
    w.Put(int32_t(p.palette.size()));
    for (uint16_t b : p.palette)
        w.Put(b);
    w.Put(p.indexCap);
    w.Put(int32_t(p.indices.size()));
    for (uint16_t i : p.indices)
        w.Put(i);
    w.Put(p.b22);
    w.Put(p.b23);
    w.Put(p.vertexCount);
    if (g_Version >= 0x9001C)
        w.Put(p.triangleCount);
    if (g_Version > 0x90014)
        w.Put(p.u1c);
}

bool ReadGroup(Reader& r, Group& g)
{
    auto head = r.Raw(kGroupHead);
    g.present = r.Get<uint32_t>();
    if (!r.ok)
        return false;
    std::copy(head.begin(), head.end(), g.head.begin());
    if (!g.present)
        return true;
    auto format = r.Raw(kFormatSize);
    if (!r.ok)
        return false;
    std::copy(format.begin(), format.end(), g.format.begin());
    g.stride = int8_t(g.format[0x22]);
    const int32_t count = r.Get<int32_t>();
    if (!r.ok || g.stride <= 0 || count < 0 || count > 65536)
        return false;
    g.vertices = r.Raw(size_t(count) * g.stride);
    g.primCap = r.Get<int32_t>();
    const int32_t prims = r.Get<int32_t>();
    if (!r.ok || prims < 0 || prims > 256)
        return false;
    g.prims.resize(prims);
    for (auto& p : g.prims)
        if (!ReadPrim(r, p))
            return false;
    return true;
}

void WriteGroup(Writer& w, const Group& g)
{
    w.Raw(g.head.data(), g.head.size());
    w.Put(g.present);
    if (!g.present)
        return;
    w.Raw(g.format.data(), g.format.size());
    w.Put(int32_t(g.Count()));
    w.Raw(g.vertices.data(), g.vertices.size());
    w.Put(g.primCap);
    w.Put(int32_t(g.prims.size()));
    for (const Prim& p : g.prims)
        WritePrim(w, p);
}

bool ReadBody(Reader& r, Body& b)
{
    b.newFormat = r.Get<uint32_t>();
    b.appearanceCap = r.Get<int32_t>();
    if (!r.ok || !b.newFormat || r.Get<int32_t>() != 0) // no legacy appearances
        return false;
    b.groupCap = r.Get<int32_t>();
    const int32_t groups = r.Get<int32_t>();
    if (!r.ok || groups < 0 || groups > 256)
        return false;
    b.groups.resize(groups);
    for (auto& g : b.groups)
        if (!ReadGroup(r, g))
            return false;
    if (!ReadTable(r, b.textures) || !ReadTable(r, b.materials))
        return false;
    b.imageCap = r.Get<int32_t>();
    if (r.Get<int32_t>() != 0) // no embedded images
        return false;
    b.attachmentCap = r.Get<int32_t>();
    b.attachmentCount = r.Get<int32_t>();
    if (!r.ok || b.attachmentCount < 0 || b.attachmentCount > 256)
        return false;
    b.attachments = r.Raw(size_t(b.attachmentCount) * 56);
    return r.ok && b.materials.offsets.size() == b.groups.size();
}

void WriteBody(Writer& w, const Body& b)
{
    w.Put(b.newFormat);
    w.Put(b.appearanceCap);
    w.Put(int32_t(0));
    w.Put(b.groupCap);
    w.Put(int32_t(b.groups.size()));
    for (const Group& g : b.groups)
        WriteGroup(w, g);
    WriteTable(w, b.textures);
    WriteTable(w, b.materials);
    w.Put(b.imageCap);
    w.Put(int32_t(0));
    w.Put(b.attachmentCap);
    w.Put(b.attachmentCount);
    w.Raw(b.attachments.data(), b.attachments.size());
}

bool ReadMesh(Reader& r, MeshRecord& m)
{
    m.begin = r.pos;
    auto name = r.Raw(32);
    if (!r.ok)
        return false;
    m.name.assign(reinterpret_cast<const char*>(name.data()), strnlen(reinterpret_cast<const char*>(name.data()), 32));
    r.Skip(12 + 4 + 4 * 4 + 4 + 4 + 4); // centre, radius, 4 bools, colour, triangle total, bool
    m.bodyBegin = r.pos;
    if (!r.ok || !ReadBody(r, m.body))
        return false;
    m.bodyEnd = r.pos;
    r.Skip(24); // bounding box
    const uint32_t extra = r.Get<uint32_t>();
    if (g_Version >= 0x52049 && extra) {
        r.Get<int32_t>();
        const int32_t pairs = r.Get<int32_t>();
        if (!r.ok || pairs < 0 || pairs > 100000)
            return false;
        r.Skip(size_t(pairs) * 8 + 24);
    }
    r.Skip(4);
    if (g_Version >= 0x90007)
        r.Skip(4);
    return r.ok;
}

// A model: name, bool, its meshes, bones (name, matrix, parent), two matrices, per-bone flags.
bool ReadModel(Reader& r, std::vector<MeshRecord>& meshes)
{
    r.SkipString();
    r.Skip(4);
    r.Get<int32_t>();
    const int32_t count = r.Get<int32_t>();
    if (!r.ok || count < 0 || count > 64)
        return false;
    meshes.resize(count);
    for (auto& m : meshes)
        if (!ReadMesh(r, m))
            return false;
    r.Get<int32_t>();
    const int32_t bones = r.Get<int32_t>();
    if (!r.ok || bones < 0 || bones > 512)
        return false;
    r.Skip(size_t(bones) * (32 + 48 + 2));
    r.Skip(48);
    if (g_Version >= 0x9000F)
        r.Skip(48);
    if (g_Version >= 0x52030) {
        r.Get<int32_t>();
        const int32_t flags = r.Get<int32_t>();
        if (!r.ok || flags < 0 || flags > 4096)
            return false;
        r.Skip(size_t(flags) * 4);
    }
    return r.ok;
}

bool SameName(const std::string& a, const char* b)
{
    if (a.size() != std::strlen(b))
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

// One box: its 8 corners (index = 4x + 2y + z, 0 = lo, 1 = hi), each with the normal pointing from the box's centre
// through the corner (the faces do not get their own vertices; flat enough for small parts), and 12 triangles. The
// game's winding: for a triangle (a, b, c), cross(b - a, c - a) points INTO the surface.
struct BoxGeometry {
    std::array<std::array<double, 3>, 8> corners;
    std::array<std::array<double, 3>, 8> normals;
    std::array<std::array<int, 3>, 12> triangles;
};

BoxGeometry MakeBox(const MeshBox& box)
{
    BoxGeometry g{};
    for (int i = 0; i < 8; ++i) {
        g.corners[i][0] = double((i & 4) ? box.hi[0] : box.lo[0]);
        g.corners[i][1] = double((i & 2) ? box.hi[1] : box.lo[1]);
        g.corners[i][2] = double((i & 1) ? box.hi[2] : box.lo[2]);
    }
    struct Quad {
        int n[3];
        int c[4];
    };
    constexpr Quad quads[6] = {
        { { -1, 0, 0 }, { 0, 1, 3, 2 } }, { { 1, 0, 0 }, { 4, 6, 7, 5 } },
        { { 0, -1, 0 }, { 0, 4, 5, 1 } }, { { 0, 1, 0 }, { 2, 3, 7, 6 } },
        { { 0, 0, -1 }, { 0, 2, 6, 4 } }, { { 0, 0, 1 }, { 1, 5, 7, 3 } },
    };
    int t = 0;
    for (const Quad& q : quads) {
        const int pairs[2][3] = { { q.c[0], q.c[1], q.c[2] }, { q.c[0], q.c[2], q.c[3] } };
        for (const auto& tri : pairs) {
            const auto& a = g.corners[tri[0]];
            const auto& b = g.corners[tri[1]];
            const auto& c = g.corners[tri[2]];
            const double u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
            const double v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
            const double cross[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
            const double outward = cross[0] * q.n[0] + cross[1] * q.n[1] + cross[2] * q.n[2];
            g.triangles[t++] = outward > 0 ? std::array<int, 3>{ tri[0], tri[2], tri[1] }
                                           : std::array<int, 3>{ tri[0], tri[1], tri[2] };
        }
    }
    double centre[3] = { 0, 0, 0 };
    for (const auto& p : g.corners)
        for (int k = 0; k < 3; ++k)
            centre[k] += p[k];
    for (double& c : centre)
        c /= 8;
    for (int i = 0; i < 8; ++i) {
        double d[3];
        for (int k = 0; k < 3; ++k)
            d[k] = g.corners[i][k] - centre[k];
        const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int k = 0; k < 3; ++k)
            g.normals[i][k] = d[k] / len;
    }
    return g;
}

// A normal component as the vertex stores it: -1..1 to 0..255, rounded to nearest (ties to even).
uint8_t NormalByte(double v)
{
    return uint8_t(int(std::nearbyint((v + 1.0) * 127.5)) & 255);
}

// Appends triangles to a strip: each after the first joined by repeating the strip's last index and its own first
// (once more when needed so it starts at an even place, keeping its winding); the joins make only triangles of no size.
void AppendToStrip(std::vector<uint16_t>& strip, const std::array<uint16_t, 3>& t)
{
    if (!strip.empty()) {
        strip.push_back(strip.back());
        strip.push_back(t[0]);
        if (strip.size() % 2 == 1)
            strip.push_back(t[0]);
    }
    strip.insert(strip.end(), t.begin(), t.end());
}

} // namespace

bool AddMeshBoxes(std::vector<uint8_t>& data, const char* material, const std::vector<MeshBox>& boxes, int textureSize,
    std::string& report)
{
    if (!material || boxes.empty() || textureSize <= 0) {
        report = "nothing to add";
        return false;
    }
    for (const MeshBox& b : boxes)
        for (int k = 0; k < 3; ++k)
            if (!(b.lo[k] < b.hi[k])) {
                report = "a box without size";
                return false;
            }

    Reader r{ data.data(), data.size() };
    if (r.Get<uint32_t>() != kMagic) {
        report = "not a model";
        return false;
    }
    g_Version = r.Get<uint32_t>();
    if (g_Version < kOldestVersion) {
        report = "an older model format";
        return false;
    }
    r.Get<int32_t>();
    const int32_t headers = r.Get<int32_t>();
    if (!r.ok || headers < 0 || headers > 64) {
        report = "an unknown layout";
        return false;
    }
    for (int32_t i = 0; r.ok && i < headers; ++i) {
        r.SkipString();
        r.Get<int32_t>();
        const int32_t names = r.Get<int32_t>();
        if (names < 0 || names > 64)
            r.ok = false;
        for (int32_t k = 0; r.ok && k < names; ++k)
            r.SkipString();
    }
    r.Get<int32_t>();
    const int32_t models = r.Get<int32_t>();
    if (!r.ok || models < 1 || models > 8) {
        report = "an unknown layout";
        return false;
    }
    std::vector<std::vector<MeshRecord>> records(models);
    for (auto& meshes : records)
        if (!ReadModel(r, meshes)) {
            report = "an unknown layout";
            return false;
        }
    // Then one bool per mesh of every model; the file ends there.
    for (const auto& meshes : records)
        r.Skip(meshes.size() * 4);
    if (!r.ok || r.pos != data.size()) {
        report = "an unknown layout";
        return false;
    }

    // The body mesh of the (first) model; the shadow mesh is left as it is.
    MeshRecord* base = nullptr;
    for (auto& m : records[0])
        if (m.name == "Base") {
            base = &m;
            break;
        }
    if (!base) {
        report = "no mesh named Base";
        return false;
    }
    Body& body = base->body;
    Group* group = nullptr;
    for (size_t i = 0; i < body.groups.size(); ++i)
        if (SameName(body.materials.Name(i), material)) {
            group = &body.groups[i];
            break;
        }
    if (!group) {
        report = std::string("no group ") + material;
        return false;
    }
    if (!group->present || group->stride != kStride || group->Offset(kSlotPosition) != 0 ||
        group->Offset(kSlotNormal) != kNormalAt || group->Offset(kSlotUv) != kUvAt ||
        group->Offset(kSlotWeights) != kWeightsAt || group->Offset(kSlotBones) != kBonesAt) {
        report = "a vertex layout it does not know";
        return false;
    }
    if (group->prims.size() != 1 || group->prims[0].palette.empty()) {
        report = "not one triangle list with bones";
        return false;
    }
    Prim& prim = group->prims[0];
    const size_t vertexTotal = size_t(group->Count()) + boxes.size() * 8;
    const int addedTriangles = int(boxes.size()) * 12;
    int32_t triangleTotal;
    std::memcpy(&triangleTotal, data.data() + base->begin + kTriangleTotalAt, 4);
    if (vertexTotal > 0xFFFF || prim.triangleCount + addedTriangles > 0x7FFF) {
        report = "too many vertices or triangles";
        return false;
    }

    float bounds[2][3]; // min, max
    std::memcpy(bounds, data.data() + base->bodyEnd, sizeof(bounds));
    for (const MeshBox& box : boxes) {
        const BoxGeometry g = MakeBox(box);
        const uint16_t first = uint16_t(group->Count());
        const float uv[2] = { float(double(box.texel[0]) / textureSize), float(double(box.texel[1]) / textureSize) };
        for (int i = 0; i < 8; ++i) {
            uint8_t v[kStride] = {};
            const float pos[3] = { float(g.corners[i][0]), float(g.corners[i][1]), float(g.corners[i][2]) };
            std::memcpy(v, pos, 12);
            for (int k = 0; k < 3; ++k)
                v[kNormalAt + k] = NormalByte(g.normals[i][k]);
            std::memcpy(v + kUvAt, uv, 8);
            v[kWeightsAt] = 255; // wholly the palette's first bone (indices 0)
            group->vertices.insert(group->vertices.end(), v, v + kStride);
        }
        for (const auto& t : g.triangles)
            AppendToStrip(prim.indices,
                { uint16_t(first + t[0]), uint16_t(first + t[1]), uint16_t(first + t[2]) });
        for (int k = 0; k < 3; ++k) {
            bounds[0][k] = std::min(bounds[0][k], box.lo[k]);
            bounds[1][k] = std::max(bounds[1][k], box.hi[k]);
        }
    }
    prim.indexCap = int32_t(prim.indices.size());
    prim.vertexCount = uint16_t(group->Count());
    prim.triangleCount = int16_t(prim.triangleCount + addedTriangles);
    triangleTotal += addedTriangles;

    Writer w;
    w.Raw(data.data(), base->bodyBegin);
    std::memcpy(w.out.data() + base->begin + kTriangleTotalAt, &triangleTotal, 4);
    WriteBody(w, body);
    w.Raw(bounds, sizeof(bounds));
    w.Raw(data.data() + base->bodyEnd + sizeof(bounds), data.size() - base->bodyEnd - sizeof(bounds));

    char line[160];
    std::snprintf(line, sizeof(line), "%d boxes added to %s of Base (%d vertices, %d triangles)", int(boxes.size()),
        material, int(boxes.size()) * 8, addedTriangles);
    report = line;
    data = std::move(w.out);
    return true;
}

} // namespace swrots::game
