// The developer test map in the game: Grievous Survival's level file and the cave's model are replaced as the
// game reads them from the level's PAK (resource patches), when this start of the game asks for the dev map.

#include "game/devmap.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <sstream>
#include <vector>

#include "core/log.h"
#include "game/custommaps.h"
#include "game/resources.h"
#include "kernel/kernel.h"

namespace swrots::game {

namespace {

constexpr const char* kLevelArgument = "map:u107_uta_grievoussurvival";
constexpr const char* kDevMapArgument = "devmap:1";

bool g_Active = false;
DevMapLayout g_Layout;
bool g_HasLayout = false;
const CustomMap* g_Map = nullptr; // a map of a mod's own, from `custommap:<id>`
std::string g_MusicServed;        // the disc's music file its track is served for
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
    RegisterResourcePatch(kDevMapFloorModel, &BuildFloor);
    if (g_Active)
        LOG_INFO("Dev map: Grievous Survival loads as the developer map%s%s", g_Map ? ", on " : "",
            g_Map ? g_Map->name.c_str() : "");
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

void StartDevMap(const std::string& more)
{
    kernel::LaunchGame(std::string(kLevelArgument) + " " + kDevMapArgument + (more.empty() ? "" : " " + more));
}

} // namespace swrots::game
