// Maps of mods' own (see custommaps.h).

#include "game/custommaps.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "core/log.h"

namespace swrots::game {

namespace {

constexpr float kPi = 3.14159265f;
// The dev map's floor (devmapbuild.cpp's space), for a map without bounds.
constexpr float kFloorBounds[4] = { 1500.0f, 2700.0f, 24000.0f, 13500.0f };

std::vector<CustomMap> g_Maps;

std::string Profile(const std::wstring& file, const wchar_t* section, const wchar_t* key)
{
    wchar_t value[2048] = {};
    GetPrivateProfileStringW(section, key, L"", value, 2048, file.c_str());
    std::string out;
    for (const wchar_t* c = value; *c; ++c)
        out += char(*c < 128 ? *c : '?');
    return out;
}

// The numbers in a value, whatever separates them (spaces, commas, semicolons).
std::vector<float> Numbers(const std::string& text)
{
    std::vector<float> out;
    const char* p = text.c_str();
    while (*p) {
        if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.') {
            char* end = nullptr;
            const float v = std::strtof(p, &end);
            if (end == p) {
                ++p;
                continue;
            }
            out.push_back(v);
            p = end;
        } else {
            ++p;
        }
    }
    return out;
}

std::string Narrow(const wchar_t* text)
{
    std::string out;
    for (const wchar_t* c = text; *c; ++c)
        out += char(*c < 128 ? *c : '?');
    return out;
}

bool ReadMap(const std::wstring& file, const std::string& id, CustomMap& m)
{
    m.id = id;
    m.name = Profile(file, L"Map", L"Name");
    if (m.name.empty())
        m.name = id;
    for (char& c : m.name)
        c = char(toupper(static_cast<unsigned char>(c)));
    // Model=maps\cantina\cantina: its folder (with the last backslash) and its name
    const std::string model = Profile(file, L"Map", L"Model");
    const size_t slash = model.find_last_of('\\');
    if (model.empty() || slash == std::string::npos || slash + 1 == model.size()) {
        LOG_WARN("Custom maps: %s needs Model (its folder and name under meshes, e.g. maps\\%s\\%s)", id.c_str(),
            id.c_str(), id.c_str());
        return false;
    }
    m.modelFolder = model.substr(0, slash + 1);
    m.modelName = model.substr(slash + 1);
    m.vertices = uint32_t(std::strtoul(Profile(file, L"Map", L"Vertices").c_str(), nullptr, 10));
    if (!m.vertices) {
        LOG_WARN("Custom maps: %s needs Vertices (its model's vertex count)", id.c_str());
        return false;
    }
    std::vector<float> v = Numbers(Profile(file, L"Map", L"Floor"));
    m.floor = v.empty() ? 0 : v[0];
    const std::vector<float> heroes = Numbers(Profile(file, L"Map", L"HeroesLine"));
    const std::vector<float> villains = Numbers(Profile(file, L"Map", L"VillainsLine"));
    if (heroes.size() >= 2 && villains.size() >= 2) {
        m.heroes[0] = heroes[0];
        m.heroes[1] = heroes[1];
        m.villains[0] = villains[0];
        m.villains[1] = villains[1];
    } else { // lines across the middle of the map's own space
        m.heroes[1] = -350;
        m.villains[1] = 350;
    }
    if (std::hypot(m.villains[0] - m.heroes[0], m.villains[1] - m.heroes[1]) < 1) {
        LOG_WARN("Custom maps: %s's lines are at one place", id.c_str());
        return false;
    }
    v = Numbers(Profile(file, L"Map", L"LineGap"));
    if (!v.empty() && v[0] > 0)
        m.lineGap = v[0];
    v = Numbers(Profile(file, L"Map", L"Spawns"));
    for (size_t i = 0; i + 1 < v.size(); i += 2)
        m.spawns.push_back({ v[i], v[i + 1] });
    v = Numbers(Profile(file, L"Map", L"Bounds"));
    if (v.size() >= 4) {
        m.hasBounds = true;
        m.bounds[0] = std::min(v[0], v[2]);
        m.bounds[1] = std::min(v[1], v[3]);
        m.bounds[2] = std::max(v[0], v[2]);
        m.bounds[3] = std::max(v[1], v[3]);
    }
    for (int i = 1;; ++i) {
        const std::wstring key = L"Box" + std::to_wstring(i);
        v = Numbers(Profile(file, L"Collision", key.c_str()));
        if (v.size() < 6)
            break;
        m.boxes.push_back({ v[0], v[1], v[2], v[3], v[4], v[5], v.size() > 6 ? v[6] * kPi / 180 : 0.0f });
    }
    // its place: the middle between the lines at the dev map's middle, the heroes' line facing +z
    m.middle[0] = (m.heroes[0] + m.villains[0]) / 2;
    m.middle[1] = (m.heroes[1] + m.villains[1]) / 2;
    const float dx = m.villains[0] - m.heroes[0], dz = m.villains[1] - m.heroes[1];
    m.yaw = std::atan2(-dx, dz);
    return true;
}

} // namespace

void InstallCustomMaps(const std::wstring& mods)
{
    g_Maps.clear();
    WIN32_FIND_DATAW found;
    const std::wstring folder = mods + L"\\maps\\";
    HANDLE find = FindFirstFileW((folder + L"*").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == L'.')
            continue;
        const std::wstring file = folder + found.cFileName + L"\\map.ini";
        if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;
        CustomMap m;
        if (!ReadMap(file, Narrow(found.cFileName), m))
            continue;
        LOG_INFO("Custom maps: %s (%s), model meshes\\%s%s, %zu collision boxes, %zu spawns", m.id.c_str(),
            m.name.c_str(), m.modelFolder.c_str(), m.modelName.c_str(), m.boxes.size(), m.spawns.size());
        g_Maps.push_back(std::move(m));
    } while (FindNextFileW(find, &found));
    FindClose(find);
}

const std::vector<CustomMap>& CustomMaps() { return g_Maps; }

const CustomMap* FindCustomMap(const std::string& id)
{
    for (const CustomMap& m : g_Maps)
        if (_stricmp(m.id.c_str(), id.c_str()) == 0)
            return &m;
    return nullptr;
}

void CustomMapToWorld(const CustomMap& map, float x, float z, float& worldX, float& worldZ)
{
    // turned as a placed object's rows turn it (right c 0 -s, forward s 0 c)
    const float c = std::cos(map.yaw), s = std::sin(map.yaw);
    const float lx = x - map.middle[0], lz = z - map.middle[1];
    worldX = kDevMapMiddleX + c * lx + s * lz;
    worldZ = kDevMapMiddleZ - s * lx + c * lz;
}

float CustomMapYaw(const CustomMap& map) { return map.yaw; }

void CustomMapBounds(const CustomMap& map, float (&bounds)[4])
{
    if (!map.hasBounds) {
        std::copy(std::begin(kFloorBounds), std::end(kFloorBounds), bounds);
        return;
    }
    bounds[0] = bounds[1] = 1e30f;
    bounds[2] = bounds[3] = -1e30f;
    for (int i = 0; i < 4; ++i) {
        float x, z;
        CustomMapToWorld(map, map.bounds[i & 1 ? 2 : 0], map.bounds[i & 2 ? 3 : 1], x, z);
        bounds[0] = std::min(bounds[0], x);
        bounds[1] = std::min(bounds[1], z);
        bounds[2] = std::max(bounds[2], x);
        bounds[3] = std::max(bounds[3], z);
    }
}

void ApplyCustomMap(const CustomMap& map, DevMapLayout& layout)
{
    layout.modelFolder = map.modelFolder;
    layout.modelName = map.modelName;
    layout.modelVertices = map.vertices;
    CustomMapToWorld(map, 0, 0, layout.modelX, layout.modelZ);
    layout.modelY = -map.floor;
    layout.modelYaw = map.yaw;
    layout.boxes.clear();
    for (const DevMapBox& b : map.boxes) {
        DevMapBox w = b;
        CustomMapToWorld(map, b.cx, b.cz, w.cx, w.cz);
        w.cy = b.cy - map.floor;
        w.yaw = b.yaw + map.yaw;
        layout.boxes.push_back(w);
    }
}

} // namespace swrots::game
