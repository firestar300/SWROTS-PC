// Heroes vs Villains in the game's main menu (see hvvmenu.h).
//
// The main menu (interfc\front_end\xml\main_menu.xbl_xml, asked for as main_menu.xml) has under Multi Player a
// submenu (MainMenuSubMenu) of two rows, Versus and Cooperative: MainMenuSwitchScreenButton items, each a text id,
// its down and up neighbours and a screen to switch to (select_jedi, select_challenge). A third row is added, a copy
// of Cooperative, switching to the Versus character select; the port knows it was this row that was chosen from the
// button's event (vtable 0x5D5580 slot 4, 0x2CBEC0; event 0x23 "chosen" switches the screen through 0xD4070).

#include "game/hvvmenu.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>

#include "core/log.h"
#include "core/patch.h"
#include "game/custommaps.h"
#include "game/hvv.h"
#include "game/menus.h"
#include "game/resources.h"
#include "game/roster.h"

namespace swrots::game {

namespace {

constexpr const char* kMainMenu = "interfc\\front_end\\xml\\main_menu.xml";
constexpr const char* kRowName = "hvv";
constexpr const char* kRowText = "IDS_PORT_HVV";
constexpr float kRowStep = 24.0f; // the submenu's rows

constexpr uint32_t kSwitchButtonVtable = 0x005D5580;
constexpr uint32_t kEventSlot = 4;
constexpr uint32_t kSwitchButtonEvent = 0x002CBEC0;
constexpr int kChosen = 0x23;

bool g_HvvMode = false;
std::vector<std::string> g_Picks;

// The submenu's child count: the u32 right before its first child (the Versus row).
bool PatchMainMenu(std::vector<uint8_t>& data)
{
    size_t versus, versusEnd, coop, coopEnd, existing, existingEnd;
    if (!MenuRowBlock(data, "versus", versus, versusEnd) || !MenuRowBlock(data, "cooperative", coop, coopEnd) ||
        MenuRowBlock(data, kRowName, existing, existingEnd) || versus < 4) {
        LOG_WARN("HvV menu: the main menu is not as expected; no Heroes vs Villains entry");
        return false;
    }
    uint32_t children;
    std::memcpy(&children, data.data() + versus - 4, 4);
    if (children != 2) {
        LOG_WARN("HvV menu: the Multi Player submenu has %u rows, not 2; no Heroes vs Villains entry", children);
        return false;
    }
    // The submenu ends with 8 bytes of its own after its last row: Cooperative's row ends where its highlight bar
    // (as long as Versus's) does.
    const std::vector<uint8_t> item = MenuString("screenItem");
    const size_t versusBar = MenuFind(data, item, versus + 1, versusEnd);
    const size_t coopBar = MenuFind(data, item, coop + 1, coopEnd);
    if (versusBar == std::string::npos || coopBar == std::string::npos ||
        coopBar + (versusEnd - versusBar) > coopEnd) {
        LOG_WARN("HvV menu: the submenu's rows are not as expected; no Heroes vs Villains entry");
        return false;
    }
    coopEnd = coopBar + (versusEnd - versusBar);
    const size_t nameAt = MenuString("screenItem").size();
    // the new row: Cooperative's, one row lower, Versus's screen
    std::vector<uint8_t> row(data.begin() + coop, data.begin() + coopEnd);
    float x, y;
    MenuItemPosition(row, &x, &y, false);
    y += kRowStep;
    MenuItemPosition(row, &x, &y, true);
    size_t rowEnd = row.size();
    // after the text id: down (stays Versus: back to the top), then up (Cooperative)
    const std::vector<uint8_t> versusString = MenuString("versus");
    bool ok = MenuReplaceString(row, 0, rowEnd, "cooperative", kRowName) &&
              MenuReplaceString(row, nameAt + 8, rowEnd, "IDS_COOPERATIVE", kRowText);
    const size_t rowDown = ok ? MenuFind(row, versusString, nameAt + 8, rowEnd) : std::string::npos;
    ok = rowDown != std::string::npos &&
         MenuReplaceString(row, rowDown + versusString.size(), rowEnd, "versus", "cooperative") &&
         MenuReplaceString(row, nameAt + 8, rowEnd, "select_challenge", "select_jedi");
    // after the screen: the button's id (+0x84 at run time; Versus 2, Cooperative 3, which asks the profile
    // whether the co-op missions are open): Versus's
    const std::vector<uint8_t> screen = MenuString("select_jedi");
    const size_t screenAt = ok ? MenuFind(row, screen, nameAt + 8, rowEnd) : std::string::npos;
    if (!ok || screenAt == std::string::npos || screenAt + screen.size() + 4 > rowEnd) {
        LOG_WARN("HvV menu: the Cooperative row is not as expected; no Heroes vs Villains entry");
        return false;
    }
    const uint32_t versusId = 2;
    std::memcpy(row.data() + screenAt + screen.size(), &versusId, 4);
    // Cooperative's down and Versus's up go to the new row (the later edits first: they move nothing before them)
    size_t coopLinks = coopEnd;
    if (!MenuReplaceString(data, coop + nameAt + 12, coopLinks, "versus", kRowName)) {
        LOG_WARN("HvV menu: the Cooperative row's links are not as expected; no Heroes vs Villains entry");
        return false;
    }
    data.insert(data.begin() + coopLinks, row.begin(), row.end());
    size_t versusLinks = versusEnd;
    // Versus's links: down "cooperative", then up "cooperative": the second becomes the new row
    const std::vector<uint8_t> coopString = MenuString("cooperative");
    const size_t down = MenuFind(data, coopString, versus + nameAt + 8, versusLinks);
    if (down == std::string::npos ||
        !MenuReplaceString(data, down + coopString.size(), versusLinks, "cooperative", kRowName)) {
        LOG_WARN("HvV menu: the Versus row's links are not as expected; no Heroes vs Villains entry");
        return false;
    }
    ++children;
    std::memcpy(data.data() + versus - 4, &children, 4);
    LOG_INFO("HvV menu: the main menu has a Heroes vs Villains entry");
    return true;
}

void PatchMainMenuResource(std::vector<uint8_t>& data)
{
    std::vector<uint8_t> copy = data;
    if (PatchMainMenu(copy))
        data.swap(copy);
}

using EventFn = bool(__fastcall*)(uint8_t*, void*, int, int);

// The submenu's buttons: the row chosen (an item's data +4 is its name key) puts the select screen in Heroes vs
// Villains mode or takes it out.
bool __fastcall SwitchButtonEvent(uint8_t* self, void*, int event, int value)
{
    if (event == kChosen) {
        uint8_t* item = *reinterpret_cast<uint8_t**>(self + 8);
        const uint32_t key = item ? *reinterpret_cast<const uint32_t*>(item + 4) : 0;
        if (key == MenuKey(kRowName)) {
            g_HvvMode = true;
            g_Picks.clear();
            LOG_INFO("HvV menu: Heroes vs Villains chosen");
        } else if (key) {
            g_HvvMode = false;
        }
    }
    return reinterpret_cast<EventFn>(uintptr_t(kSwitchButtonEvent))(self, nullptr, event, value);
}

// --- The character select in Heroes vs Villains mode ---------------------------------------------------------------
// The select screen (JediList, vtable 0x5D68B8) passes player 1's events to 0x2D51B0 (thiscall (event), from
// 0x2D54B6): left/right (0x1F/0x20) move the cursor (+0x2C, a slot), A (0x23) picks (+0x28 set; then player 1 picks
// the opponent), B (0x26) takes the pick back. In Heroes vs Villains mode the cursor stays player 1's: A adds the
// fighter under it to the line-up (the first is the player's, heroes first, then villains), B takes the last back
// (with none, the screen's own B: back to the main menu), up/down set the lives, X the health, Y the intro,
// White/Black (LB/RB: 0x2A/0x29) the map. The eighth pick starts the match.
constexpr uint32_t kPlayer1EventCall = 0x002D54B6;
constexpr uint32_t kPlayer1Event = 0x002D51B0;
constexpr uint32_t kSelectVtable = 0x005D68B8;
constexpr uint32_t kSelectFrameSlot = 3;
constexpr uint32_t kSelectFrame = 0x002D56F0;
constexpr uint32_t kSelectSound = 0x2C; // vfunc, thiscall (sound id): 0x13 the pick's
constexpr int kLeft = 0x1F, kRight = 0x20, kUp = 0x21, kDown = 0x22, kB = 0x26;
constexpr int kX = 0x2B, kY = 0x2C;
constexpr int kWhite = 0x2A, kBlack = 0x29; // LB and RB on a PC pad (LT 0x28, RT 0x27)
constexpr size_t kLineup = 8;
constexpr int kLivesChoices[] = { 5, 10, 20, 35, 50, 75, 99 };
constexpr int kHealthChoices[] = { 150, 250, 400, 600, 1000 };

ULONGLONG g_SelectSeen = 0;
uint8_t* g_SelectScreen = nullptr;
int g_LivesChoice = 3, g_HealthChoice = 1;
bool g_Intro = true;
int g_MapChoice = 0; // 0 the dev map's floor, then the custom maps

// The chosen map's id (empty: the dev map's floor).
std::string MapChoice()
{
    const auto& maps = CustomMaps();
    return g_MapChoice > 0 && g_MapChoice <= int(maps.size()) ? maps[size_t(g_MapChoice - 1)].id : std::string();
}

using Player1EventFn = bool(__fastcall*)(uint8_t*, void*, int);
using FrameFn = void(__fastcall*)(uint8_t*, void*, uint32_t);

void Sound(uint8_t* screen, int id)
{
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*, int)>(
        (*reinterpret_cast<void* const* const*>(screen))[kSelectSound / 4])(screen, nullptr, id);
}

bool __fastcall Player1EventHook(uint8_t* screen, void*, int event)
{
    g_SelectScreen = screen;
    if (!g_HvvMode)
        return reinterpret_cast<Player1EventFn>(uintptr_t(kPlayer1Event))(screen, nullptr, event);
    if (event == kChosen) {
        const int slot = *reinterpret_cast<const int*>(screen + 0x2C);
        if (const char* className = SelectSlotClass(slot)) {
            g_Picks.push_back(className);
        } else { // Random: any fighter the match panel offers
            const std::vector<std::string> classes = HvvFighterClasses();
            if (!classes.empty())
                g_Picks.push_back(classes[size_t(std::rand()) % classes.size()]);
        }
        Sound(screen, 0x13);
        if (g_Picks.size() >= kLineup) {
            std::string error;
            if (!StartHvv(g_Picks, 0, kLivesChoices[g_LivesChoice], kHealthChoices[g_HealthChoice], g_Intro, MapChoice(), error)) {
                LOG_WARN("HvV menu: the match cannot start: %s", error.c_str());
                g_Picks.pop_back();
            } else {
                LOG_INFO("HvV menu: the match starts");
            }
        }
        return true;
    }
    if (event == kB && !g_Picks.empty()) {
        g_Picks.pop_back();
        Sound(screen, 0x13);
        return true;
    }
    if (event == kUp || event == kDown) {
        const int n = int(std::size(kLivesChoices));
        g_LivesChoice = (g_LivesChoice + (event == kUp ? 1 : n - 1)) % n;
        return true;
    }
    if (event == kX) {
        g_HealthChoice = (g_HealthChoice + 1) % int(std::size(kHealthChoices));
        return true;
    }
    if (event == kY) {
        g_Intro = !g_Intro;
        return true;
    }
    if (event == kWhite || event == kBlack) { // the map: the floor, then the custom maps
        const int n = int(CustomMaps().size()) + 1;
        g_MapChoice = (g_MapChoice + (event == kBlack ? 1 : n - 1)) % n;
        return true;
    }
    return reinterpret_cast<Player1EventFn>(uintptr_t(kPlayer1Event))(screen, nullptr, event);
}

void __fastcall SelectFrameHook(uint8_t* screen, void*, uint32_t arg)
{
    g_SelectScreen = screen;
    g_SelectSeen = GetTickCount64();
    reinterpret_cast<FrameFn>(uintptr_t(kSelectFrame))(screen, nullptr, arg);
}

} // namespace

void InstallHvvMenu()
{
    g_HvvMode = false;
    g_Picks.clear();
    g_SelectSeen = 0;
    g_SelectScreen = nullptr;
    SetMenuText(kRowText, L"Heroes vs Villains");
    RegisterResourcePatch(kMainMenu, &PatchMainMenuResource);
    const uint32_t slot = kSwitchButtonVtable + kEventSlot * 4;
    const uint32_t frameSlot = kSelectVtable + kSelectFrameSlot * 4;
    uint8_t call[5] = { 0xE8 };
    const int32_t rel = int32_t(kPlayer1Event) - int32_t(kPlayer1EventCall + 5);
    std::memcpy(call + 1, &rel, 4);
    if (*reinterpret_cast<const uint32_t*>(uintptr_t(slot)) != kSwitchButtonEvent ||
        *reinterpret_cast<const uint32_t*>(uintptr_t(frameSlot)) != kSelectFrame ||
        std::memcmp(reinterpret_cast<const void*>(uintptr_t(kPlayer1EventCall)), call, 5) != 0) {
        LOG_WARN("HvV menu: the menus' code is not as expected; no Heroes vs Villains entry");
        return;
    }
    const uint32_t hook = uint32_t(reinterpret_cast<uintptr_t>(&SwitchButtonEvent));
    PatchBytes(slot, &hook, 4);
    const uint32_t frame = uint32_t(reinterpret_cast<uintptr_t>(&SelectFrameHook));
    PatchBytes(frameSlot, &frame, 4);
    PatchCall(kPlayer1EventCall, reinterpret_cast<const void*>(&Player1EventHook), 5);
}

HvvPickState GetHvvPickState()
{
    HvvPickState s;
    s.active = g_HvvMode && g_SelectScreen && GetTickCount64() - g_SelectSeen < 300;
    if (!s.active)
        return s;
    s.picks = g_Picks;
    if (const char* c = SelectSlotClass(*reinterpret_cast<const int*>(g_SelectScreen + 0x2C)))
        s.highlighted = c;
    const std::string map = MapChoice();
    s.map = map.empty() ? "DEVELOPER MAP" : FindCustomMap(map)->name;
    s.lives = kLivesChoices[g_LivesChoice];
    s.health = kHealthChoices[g_HealthChoice];
    s.intro = g_Intro;
    return s;
}

} // namespace swrots::game
