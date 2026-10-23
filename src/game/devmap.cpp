// The developer test map in the game: Grievous Survival's level file and the cave's model are replaced as the
// game reads them from the level's PAK (resource patches), when this start of the game asks for the dev map.

#include "game/devmap.h"

#include <windows.h>

#include <cctype>
#include <cstdint>
#include <sstream>
#include <vector>

#include "core/log.h"
#include "game/devmapbuild.h"
#include "game/resources.h"
#include "kernel/kernel.h"

namespace swrots::game {

namespace {

constexpr const char* kLevelArgument = "map:u107_uta_grievoussurvival";
constexpr const char* kDevMapArgument = "devmap:1";

bool g_Active = false;

void BuildLevel(std::vector<uint8_t>& data)
{
    if (!g_Active)
        return;
    std::string report;
    if (BuildDevMapLevel(data, report))
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

// True when `text` (launch arguments, or a Default_Xbox.cfg) holds the dev map's argument.
bool HasDevMapArgument(const std::string& text)
{
    std::istringstream words(text);
    std::string word;
    while (words >> word)
        if (_stricmp(word.c_str(), kDevMapArgument) == 0)
            return true;
    return false;
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
    const std::string& arguments = kernel::LaunchArguments();
    if (!arguments.empty()) {
        g_Active = HasDevMapArgument(arguments);
    } else {
        std::string config = ReadText(mods + L"\\Default_Xbox.cfg");
        if (config.empty())
            config = ReadText(gameData + L"\\Default_Xbox.cfg");
        g_Active = HasDevMapArgument(config);
    }
    RegisterResourcePatch(kDevMapSource, &BuildLevel);
    RegisterResourcePatch(kDevMapFloorModel, &BuildFloor);
    if (g_Active)
        LOG_INFO("Dev map: Grievous Survival loads as the developer map");
}

bool DevMapActive() { return g_Active; }

void StartDevMap()
{
    kernel::LaunchGame(std::string(kLevelArgument) + " " + kDevMapArgument);
}

} // namespace swrots::game
