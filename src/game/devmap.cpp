// The developer test map in the game: Grievous Survival's level file and the cave's model are replaced as the
// game reads them from the level's PAK (resource patches), when this start of the game asks for the dev map.

#include "game/devmap.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <vector>

#include "core/log.h"
#include "game/characters.h"
#include "game/freecam.h"
#include "game/custommaps.h"
#include "game/resources.h"
#include "kernel/kernel.h"

namespace swrots::game {

namespace {

constexpr const char* kLevelArgument = "map:u107_uta_grievoussurvival";
constexpr const char* kDevMapArgument = "devmap:1";
// A Versus duel on the dev map: the game's Versus starts its Utapau arena (a duel level; its PAK brings the duel's
// fighters, sounds, cameras and HUD art), whose level file is served as the dev map built for a duel.
constexpr const char* kDuelArena = "levels\\u107_uta_grievous_01.slp";
constexpr const char* kDuelArgument = "map:u107_uta_grievous_01";

bool g_Active = false;
DevMapLayout g_Layout;
bool g_HasLayout = false;
const CustomMap* g_Map = nullptr; // a map of a mod's own, from `custommap:<id>`
std::string g_MusicServed;        // the disc's music file its track is served for
bool g_Duel = false;              // a Versus duel on the dev map (`versus:1`)
std::string g_Arguments; // this start's launch arguments, or else its Default_Xbox.cfg

void BuildLevel(std::vector<uint8_t>& data)
{
    if (!g_Active)
        return;
    std::string report;
    DevMapLayout layout = g_Layout;
    if (g_Map)
        ApplyCustomMap(*g_Map, layout);
    if (BuildDevMapLevel(data, report, g_HasLayout || g_Map ? &layout : nullptr))
        LOG_INFO("Dev map: level built (%zu bytes): %s", data.size(), report.c_str());
    else
        LOG_WARN("Dev map: the level file is not the one expected (%s); Grievous Survival loads as it is",
            report.c_str());
}

// The duel arena's level file: the dev map (from Grievous Survival's level file, read from its PAK) built as a duel.
void BuildDuel(std::vector<uint8_t>& data)
{
    if (!g_Active || !g_Duel)
        return;
    std::vector<uint8_t> level;
    if (!ReadDiscResource(kDevMapSource, level)) {
        LOG_WARN("Dev map: Grievous Survival's level file is not on the disc; the arena loads as it is");
        return;
    }
    DevMapLayout layout = g_Layout;
    layout.versus = true;
    layout.duelArena = data; // its duel camera
    if (g_Map)
        ApplyCustomMap(*g_Map, layout);
    std::string report;
    if (!BuildDevMapLevel(level, report, &layout)) {
        LOG_WARN("Dev map: the duel could not be built (%s); the arena loads as it is", report.c_str());
        return;
    }
    LOG_INFO("Dev map: the duel's arena built (%zu bytes): %s", level.size(), report.c_str());
    data.swap(level);
}

// The duel's camera on a map with walls: the arena's rail was laid out for its open floor; where a wall stands
// between the camera and the two fighters, the camera comes nearer them along that line, to the last place that sees
// them (the map's collision boxes).
bool DuelCamera(float m[16], float)
{
    uint8_t* player = PlayerObject();
    if (!g_Map || !player)
        return false;
    auto position = [](uint8_t* c) { return reinterpret_cast<const float*>(c + 0x150 + 48); }; // transform's row 4
    const float* p = position(player);
    float focus[3] = { p[0], p[1], p[2] };
    float nearest = 1e30f;
    for (uint8_t* c : LevelCharacters()) { // the opponent: the nearest other character standing
        if (c == player || *reinterpret_cast<const float*>(c + 0x130) <= 0)
            continue;
        const float* q = position(c);
        const float d = std::hypot(q[0] - p[0], q[2] - p[2]);
        if (d < nearest) {
            nearest = d;
            focus[0] = (p[0] + q[0]) / 2;
            focus[2] = (p[2] + q[2]) / 2;
        }
    }
    focus[1] += 45;
    const float eye[3] = { m[12], m[13], m[14] };
    if (CustomMapLineOfSight(*g_Map, focus, eye))
        return false;
    float clear = 0;
    for (int i = 1; i <= 24; ++i) {
        const float t = float(i) / 24, at[3] = { focus[0] + (eye[0] - focus[0]) * t, focus[1] + (eye[1] - focus[1]) * t,
            focus[2] + (eye[2] - focus[2]) * t };
        if (!CustomMapLineOfSight(*g_Map, focus, at))
            break;
        clear = t;
    }
    const float t = std::max(0.25f, clear * 0.9f);
    for (int k = 0; k < 3; ++k)
        m[12 + k] = focus[k] + (eye[k] - focus[k]) * t;
    return true;
}

void BuildFloor(std::vector<uint8_t>& data)
{
    if (g_Active)
        data = DevMapFloorModel();
}

std::string ReadText(const std::wstring& path)
{
    std::string text;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return text;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart < (1 << 20)) {
        text.resize(size_t(size.QuadPart));
        DWORD read = 0;
        ReadFile(file, text.data(), DWORD(text.size()), &read, nullptr);
        text.resize(read);
    }
    CloseHandle(file);
    return text;
}

} // namespace

void InstallDevMap(const std::wstring& gameData, const std::wstring& mods)
{
    // the launch data decides when there is any (a level change, StartDevMap); else the configuration the game
    // starts with (mods\ overrides the disc's)
    g_Arguments = kernel::LaunchArguments();
    if (g_Arguments.empty()) {
        g_Arguments = ReadText(mods + L"\\Default_Xbox.cfg");
        if (g_Arguments.empty())
            g_Arguments = ReadText(gameData + L"\\Default_Xbox.cfg");
    }
    std::string value;
    g_Active = BootArgument("devmap", value) && value == "1";
    g_Layout = {};
    g_HasLayout = false;
    if (g_MusicServed.size()) // the last start's map's track: the disc's own again
        kernel::SetDiscFileOverride(g_MusicServed, L"");
    g_MusicServed.clear();
    g_Map = nullptr;
    g_Duel = g_Active && BootArgument("versus", value) && value == "1";
    if (g_Active && BootArgument("custommap", value)) {
        g_Map = FindCustomMap(value);
        if (g_Map && !g_Map->music.empty()) { // its track in the slot's place, for this start only
            g_MusicServed = "\\audio\\xbox\\" + g_Map->music + ".hwx";
            kernel::SetDiscFileOverride(g_MusicServed, g_Map->musicFile);
        }
        if (!g_Map)
            LOG_WARN("Dev map: there is no map %s under mods\\maps (the dev map's floor instead)", value.c_str());
    }
    RegisterResourcePatch(kDevMapSource, &BuildLevel);
    RegisterResourcePatch(kDuelArena, &BuildDuel);
    if (g_Duel && g_Map)
        SetCameraAdjuster(&DuelCamera);
    RegisterResourcePatch(kDevMapFloorModel, &BuildFloor);
    if (g_Active)
        LOG_INFO("Dev map: %s loads as the developer map%s%s", g_Duel ? "a Versus duel" : "Grievous Survival",
            g_Map ? ", on " : "", g_Map ? g_Map->name.c_str() : "");
}

bool DevMapActive() { return g_Active; }

const CustomMap* DevMapCustomMap() { return g_Active ? g_Map : nullptr; }

bool BootArgument(const char* name, std::string& value)
{
    std::istringstream words(g_Arguments);
    std::string word;
    const size_t length = strlen(name);
    while (words >> word) {
        if (word.size() > length && word[length] == ':' && _strnicmp(word.c_str(), name, length) == 0) {
            value = word.substr(length + 1);
            return true;
        }
    }
    return false;
}

void SetDevMapLayout(const DevMapLayout& layout)
{
    g_Layout = layout;
    g_HasLayout = true;
}

void StartDuel(const std::string& first, const std::string& second, const std::string& map, int rounds,
    const std::string& more)
{
    kernel::LaunchGame(std::string(kDuelArgument) + " " + kDevMapArgument + " versus:1" +
        (map.empty() ? "" : " custommap:" + map) + " v_versusMode:1 v_c1:" + first + " v_c2:" + second +
        " v_p1:0 v_p2:-1 v_roundcount:" + std::to_string(rounds > 0 ? rounds : 3) + more);
}

void StartDevMap(const std::string& more)
{
    kernel::LaunchGame(std::string(kLevelArgument) + " " + kDevMapArgument + (more.empty() ? "" : " " + more));
}

} // namespace swrots::game
