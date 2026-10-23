// Heroes vs Villains on the developer map (see hvv.h).

#include "game/hvv.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <sstream>

#include "core/log.h"
#include "game/characters.h"
#include "game/coop.h"
#include "game/devmap.h"
#include "game/freecam.h"
#include "game/game.h"
#include "input/controls.h"

namespace swrots::game {

namespace {

constexpr int kSide = 4;            // fighters a side
constexpr int kFighters = 2 * kSide;
constexpr float kLineGap = 300.0f;  // between fighters in a line
constexpr float kLinesApart = 800.0f;
constexpr float kPi = 3.14159265f;
constexpr ULONGLONG kWakeAfter = 500;    // ms after the player is in the level: the placed fighters join
// The intro (ms): a wide shot of both lines, a close-up of each fighter playing its Versus intro, the camera back
// behind the player; "FIGHT!" shows a while after.
constexpr ULONGLONG kWideShot = 3200, kFighterShot = 1800, kToPlayer = 1400, kFightBanner = 1500;
constexpr float kCloseUpDistance = 150.0f, kCloseUpHeight = 55.0f, kChest = 45.0f;
constexpr int kDefaultLives = 35;        // a side's
constexpr int kDefaultHealth = 500;      // every fighter's (a Jedi has 1000 of its own: long fights)
constexpr ULONGLONG kRespawnAfter = 4000; // ms after a fighter fell (its death plays out first)
constexpr ULONGLONG kShieldFor = 2000;   // ms a fighter that came back cannot be hurt
constexpr ULONGLONG kPlayerDown = 3000;  // ms the player's fall plays (shielded) before it comes back
// Where a fighter comes back: on the floor, near the fight but away from everyone, facing the fight.
constexpr float kFloorX0 = 1500.0f, kFloorX1 = 24000.0f, kFloorZ0 = 2700.0f, kFloorZ1 = 13500.0f;
constexpr float kFloorMargin = 600.0f;
constexpr float kRespawnReach = 2500.0f; // from the fight's middle, at most
constexpr float kRespawnClear = 900.0f;  // from every fighter, at least (when it can)
constexpr int kRespawnTries = 32;

constexpr const char* kDefaultLineup[kFighters] = { "IObiwan", "IYoda", "IMace", "ICinDrallig",
    "IVader", "IDooku", "IGrievous", "ISerra" };

// The game's character fields and calls the match uses.
constexpr uint32_t kActivate = 0xB4;           // object vfunc, thiscall (): a placed character joins the level
constexpr uint32_t kPlace = 0x1F4;             // object vfunc, thiscall (const float m[16])
constexpr uint32_t kObjectById = 0x000A31F0;   // cdecl (instance id) -> object or null
constexpr uint32_t kCharacterHealth = 0x130;
constexpr uint32_t kCharacterMaxHealth = 0x134;
constexpr uint32_t kCharacterDead = 0x12C;     // byte: its death played out
constexpr uint32_t kCharacterTransform = 0x150; // rows right, up, forward, position
constexpr uint32_t kCharacterInvincible = 0x5D6; // byte, "Invincible?"

struct Fighter {
    std::string className;
    uint32_t placedId = 0;   // the placed object's id (none for the player)
    uint8_t* object = nullptr;
    uint32_t instance = 0;   // its instance id (+4), to tell it is still there
    ULONGLONG fellAt = 0;    // when it fell (0: fighting)
    ULONGLONG shieldUntil = 0;
    bool out = false;        // fell with its side out of lives
};

bool g_Active = false;
Fighter g_Fighters[kFighters];
int g_Player = 0;
int g_StartLives = kDefaultLives;
int g_Health = kDefaultHealth;
int g_Lives[2] = {};
int g_Winner = -1;
ULONGLONG g_InLevelAt = 0;
bool g_Woken = false;
bool g_PlayerFell = false; // a fatal hit on the player, kept alive: its fall plays, then it comes back
ULONGLONG g_PlayerDownAt = 0;
bool g_IntroWanted = true;
enum class Phase { Waiting, Intro, Fight } g_Phase = Phase::Waiting;
ULONGLONG g_IntroAt = 0, g_FightAt = 0;
int g_ShotPlayed = -1;      // the last fighter whose close-up started (its intro played)
float g_LastShot[16] = {};  // the intro camera's last placement, for the way back to the game's

// The fighters' names on screen.
struct DisplayName {
    const char* className;
    const char* name;
};
constexpr DisplayName kNames[] = { { "IAnakin", "ANAKIN SKYWALKER" }, { "IObiwan", "OBI-WAN KENOBI" },
    { "IOldObiwan", "OBI-WAN KENOBI" }, { "IYoda", "YODA" }, { "IMace", "MACE WINDU" },
    { "ICinDrallig", "CIN DRALLIG" }, { "IVader", "DARTH VADER" }, { "IDooku", "COUNT DOOKU" },
    { "IGrievous", "GENERAL GRIEVOUS" }, { "ISerra", "SERRA KETO" }, { "IPalpatine", "DARTH SIDIOUS" } };

std::string DisplayNameOf(const std::string& className)
{
    for (const DisplayName& n : kNames)
        if (_stricmp(n.className, className.c_str()) == 0)
            return n.name;
    std::string name = className.size() > 1 && className[0] == 'I' ? className.substr(1) : className;
    for (char& c : name)
        c = char(toupper(static_cast<unsigned char>(c)));
    return name;
}

// The player's own choice of character, kept while a match plays its fighter, given back after.
struct PlayerChoice {
    bool saved = false;
    std::string className, variant, mesh, skin;
} g_SavedChoice;

void SavePlayerChoice()
{
    if (g_SavedChoice.saved)
        return;
    g_SavedChoice.saved = true;
    g_SavedChoice.className = PlayerClass() ? PlayerClass() : "";
    g_SavedChoice.variant = PlayerVariantChoice();
    g_SavedChoice.mesh = PlayerMesh();
    g_SavedChoice.skin = PlayerSkin();
}

void RestorePlayerChoice()
{
    if (!g_SavedChoice.saved)
        return;
    SetPlayerClass(nullptr);
    if (!g_SavedChoice.className.empty()) {
        SetPlayerClass(g_SavedChoice.className.c_str(), false);
        SetPlayerVariant(g_SavedChoice.variant);
        SetPlayerMesh(g_SavedChoice.mesh);
        SetPlayerSkin(g_SavedChoice.skin);
    }
    g_SavedChoice = {};
}

int SideOf(int slot) { return slot / kSide; }

// A place in the line-up: the heroes' line faces the villains' across the middle of the floor.
void LinePlace(int slot, float& x, float& z, float& yaw)
{
    const int k = slot % kSide;
    x = kDevMapMiddleX + (k - (kSide - 1) / 2.0f) * kLineGap;
    z = kDevMapMiddleZ + (SideOf(slot) == 0 ? -kLinesApart / 2 : kLinesApart / 2);
    yaw = SideOf(slot) == 0 ? 0.0f : kPi; // a character faces +z at yaw 0
}

void Transform(float x, float z, float yaw, float (&m)[16])
{
    const float c = std::cos(yaw), s = std::sin(yaw);
    const float rows[16] = { c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, x, kDevMapFloorY + 60.0f, z, 1 };
    std::copy(std::begin(rows), std::end(rows), m);
}

std::vector<std::string> Split(const std::string& text, char separator)
{
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string part;
    while (std::getline(in, part, separator))
        out.push_back(part);
    return out;
}

uint8_t* ObjectById(uint32_t id)
{
    return reinterpret_cast<uint8_t*(__cdecl*)(uint32_t)>(uintptr_t(kObjectById))(id);
}

template <typename T>
T& Field(uint8_t* object, uint32_t offset) { return *reinterpret_cast<T*>(object + offset); }

void Track(Fighter& f, uint8_t* object)
{
    f.object = object;
    f.instance = object ? Field<uint32_t>(object, 4) : 0;
    f.fellAt = 0;
}

void FullHealth(uint8_t* character)
{
    Field<float>(character, kCharacterMaxHealth) = float(g_Health);
    Field<float>(character, kCharacterHealth) = float(g_Health);
}

// Still in the level and standing.
bool Standing(Fighter& f)
{
    if (!f.object || ObjectById(f.instance) != f.object) {
        f.object = nullptr;
        return false;
    }
    return Field<float>(f.object, kCharacterHealth) > 0 && !Field<uint8_t>(f.object, kCharacterDead);
}

const float* Position(const uint8_t* character)
{
    return reinterpret_cast<const float*>(character + kCharacterTransform + 48);
}

// Somewhere to come back: near the fight's middle, as far from every standing fighter as the tries find (at
// least kRespawnClear when one does), facing the middle.
void SafePlace(int except, float (&m)[16])
{
    float cx = 0, cz = 0;
    int n = 0;
    std::vector<std::pair<float, float>> others;
    for (int i = 0; i < kFighters; ++i) {
        Fighter& f = g_Fighters[i];
        if (i == except || !Standing(f))
            continue;
        const float* p = Position(f.object);
        others.push_back({ p[0], p[2] });
        cx += p[0];
        cz += p[2];
        ++n;
    }
    if (n) {
        cx /= n;
        cz /= n;
    } else {
        cx = kDevMapMiddleX;
        cz = kDevMapMiddleZ;
    }
    float bestX = cx, bestZ = cz, bestClear = -1;
    for (int t = 0; t < kRespawnTries; ++t) {
        const float angle = float(std::rand()) / RAND_MAX * 2 * kPi;
        const float reach = kRespawnClear + float(std::rand()) / RAND_MAX * (kRespawnReach - kRespawnClear);
        const float x = std::clamp(cx + std::sin(angle) * reach, kFloorX0 + kFloorMargin, kFloorX1 - kFloorMargin);
        const float z = std::clamp(cz + std::cos(angle) * reach, kFloorZ0 + kFloorMargin, kFloorZ1 - kFloorMargin);
        float clear = 1e9f;
        for (const auto& o : others)
            clear = std::min(clear, std::hypot(x - o.first, z - o.second));
        if (clear > bestClear) {
            bestClear = clear;
            bestX = x;
            bestZ = z;
        }
        if (clear >= kRespawnClear && t >= kRespawnTries / 4)
            break;
    }
    Transform(bestX, bestZ, std::atan2(cx - bestX, cz - bestZ), m);
}

void Shield(Fighter& f, ULONGLONG now)
{
    Field<uint8_t>(f.object, kCharacterInvincible) = 1;
    f.shieldUntil = now + kShieldFor;
}

// A fatal hit on the player: while its side has lives it comes back (the game must not see it die: that ends
// the mission); with none left it falls, and the game's own death screen follows.
bool PlayerFatalHit(uint8_t* character)
{
    if (!g_Active || !g_Woken || g_Winner >= 0 || character != PlayerObject())
        return false;
    if (g_Lives[SideOf(g_Player)] <= 0)
        return false;
    if (!g_PlayerFell) {
        g_PlayerFell = true;
        g_PlayerDownAt = GetTickCount64();
        Field<uint8_t>(character, kCharacterInvincible) = 1; // no more hits while it is down
    }
    return true;
}

// --- The intro ---------------------------------------------------------------------------------------------------

uint8_t* GameOptions()
{
    auto* services = *reinterpret_cast<uint8_t**>(uintptr_t(kEngineServices));
    auto* core = services ? *reinterpret_cast<uint8_t**>(services + 0x38) : nullptr;
    return core ? *reinterpret_cast<uint8_t**>(core + 8) : nullptr;
}

// Everyone holds still (the game's aiDisabled; the player's controls held back), or acts again.
void HoldEveryone(bool hold)
{
    if (uint8_t* options = GameOptions())
        options[kOptionsAiDisabled] = hold ? 1 : 0;
    input::HoldPlayerInput(hold);
}

// A character plays its Versus intro: its sequence named <...>_Intro, found in its list ([[body +0x438] +0x70] +4:
// +8 count, +0xC items, each named at +0xC) and started as a reaction is (vfunc +0x410 (hit zone or null,
// sequence)). Yoda has none.
void PlayIntro(uint8_t* character)
{
    uint8_t* body = Field<uint8_t*>(character, 0x438);
    uint8_t* script = body ? Field<uint8_t*>(body, 0x70) : nullptr;
    uint8_t* list = script ? Field<uint8_t*>(script, 4) : nullptr;
    if (!list)
        return;
    const int count = Field<int>(list, 8);
    uint8_t** items = Field<uint8_t**>(list, 0xC);
    for (int i = 0; i < count; ++i) {
        const char* name = items[i] ? Field<const char*>(items[i], 0xC) : nullptr;
        const size_t length = name ? strlen(name) : 0;
        if (length > 6 && _stricmp(name + length - 6, "_Intro") == 0) {
            reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, void*, uint8_t*)>(
                (*reinterpret_cast<void* const* const*>(character))[0x410 / 4])(character, nullptr, nullptr, items[i]);
            return;
        }
    }
}

// A camera placement (rows right, up, forward, position) at `eye` looking at `target`, up +Y.
void LookAt(const float eye[3], const float target[3], float m[16])
{
    float f[3] = { target[0] - eye[0], target[1] - eye[1], target[2] - eye[2] };
    const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (float& v : f)
        v /= fl > 0 ? fl : 1;
    float r[3] = { f[2], 0, -f[0] }; // up x forward
    const float rl = std::sqrt(r[0] * r[0] + r[2] * r[2]);
    for (float& v : r)
        v /= rl > 0 ? rl : 1;
    const float u[3] = { f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0] };
    const float rows[16] = { r[0], r[1], r[2], 0, u[0], u[1], u[2], 0, f[0], f[1], f[2], 0, eye[0], eye[1], eye[2], 1 };
    std::copy(std::begin(rows), std::end(rows), m);
}

float Smooth(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

ULONGLONG IntroLength() { return kWideShot + kFighters * kFighterShot + kToPlayer; }

// The intro's camera at `t` ms; false past the close-ups (the way back blends towards the game's own).
bool IntroShot(ULONGLONG t, float m[16])
{
    const float floorY = kDevMapFloorY;
    if (t < kWideShot) { // a crane down the two lines, from their end
        const float a = Smooth(float(t) / kWideShot);
        const float eye[3] = { kDevMapMiddleX + 1500 - 600 * a, floorY + 520 - 360 * a, kDevMapMiddleZ - 250 + 250 * a };
        const float target[3] = { kDevMapMiddleX, floorY + 40, kDevMapMiddleZ };
        LookAt(eye, target, m);
        return true;
    }
    t -= kWideShot;
    if (t >= kFighters * kFighterShot)
        return false;
    const Fighter& f = g_Fighters[t / kFighterShot];
    if (!f.object)
        return false;
    // in front of the fighter, a little to its side, drifting across
    const float a = float(t % kFighterShot) / kFighterShot;
    const float* r = reinterpret_cast<const float*>(f.object + kCharacterTransform);
    const float* fw = r + 8;
    const float* p = r + 12;
    const float side = (a - 0.5f) * 60.0f + 25.0f;
    const float eye[3] = { p[0] + fw[0] * kCloseUpDistance + r[0] * side, p[1] + kCloseUpHeight,
        p[2] + fw[2] * kCloseUpDistance + r[2] * side };
    const float target[3] = { p[0], p[1] + kChest, p[2] };
    LookAt(eye, target, m);
    return true;
}

// The camera during the intro and on the way back to the game's.
bool HvvCamera(float m[16], float)
{
    if (!g_Active || g_Phase != Phase::Intro)
        return false;
    const ULONGLONG t = GetTickCount64() - g_IntroAt;
    if (IntroShot(t, m)) {
        std::copy(m, m + 16, g_LastShot);
        return true;
    }
    const ULONGLONG back = kWideShot + kFighters * kFighterShot;
    if (t < back)
        return false;
    // back to the game's own camera: positions and aims blended
    const float a = Smooth(float(t - back) / kToPlayer);
    float eye[3], target[3];
    for (int k = 0; k < 3; ++k) {
        eye[k] = g_LastShot[12 + k] + (m[12 + k] - g_LastShot[12 + k]) * a;
        const float from = g_LastShot[12 + k] + g_LastShot[8 + k] * 200, to = m[12 + k] + m[8 + k] * 200;
        target[k] = from + (to - from) * a;
    }
    LookAt(eye, target, m);
    return true;
}

void CheckWinner()
{
    if (g_Winner >= 0)
        return;
    for (int side = 0; side < 2; ++side) {
        if (g_Lives[side] > 0)
            continue;
        bool anyone = false;
        for (int i = side * kSide; i < (side + 1) * kSide; ++i)
            anyone |= Standing(g_Fighters[i]) || (g_Fighters[i].fellAt && !g_Fighters[i].out);
        if (!anyone) {
            g_Winner = 1 - side;
            LOG_INFO("HvV: the %s win", g_Winner == 0 ? "heroes" : "villains");
            return;
        }
    }
}

} // namespace

void InstallHvv()
{
    g_Active = false;
    g_InLevelAt = 0;
    g_Woken = false;
    g_PlayerFell = false;
    g_Winner = -1;
    g_Phase = Phase::Waiting;
    g_IntroAt = g_FightAt = 0;
    g_ShotPlayed = -1;
    for (Fighter& f : g_Fighters)
        f = {};
    SetFatalHitGuard(nullptr);
    std::string lineup, value;
    if (!DevMapActive() || !BootArgument("hvv", lineup)) {
        RestorePlayerChoice();
        return;
    }
    const std::vector<std::string> classes = Split(lineup, ',');
    if (classes.size() != kFighters) {
        LOG_WARN("HvV: the line-up needs %d classes, not %zu ('%s')", kFighters, classes.size(), lineup.c_str());
        RestorePlayerChoice();
        return;
    }
    g_Player = BootArgument("hvvplayer", value) ? std::atoi(value.c_str()) : 0;
    if (g_Player < 0 || g_Player >= kFighters)
        g_Player = 0;
    g_StartLives = BootArgument("hvvlives", value) ? std::max(1, std::atoi(value.c_str())) : kDefaultLives;
    g_Lives[0] = g_Lives[1] = g_StartLives;
    g_Health = BootArgument("hvvhealth", value) ? std::max(1, std::atoi(value.c_str())) : kDefaultHealth;
    g_IntroWanted = !BootArgument("hvvintro", value) || value != "0";

    DevMapLayout layout;
    for (int i = 0; i < kFighters; ++i) {
        g_Fighters[i].className = classes[i];
        float x, z, yaw;
        LinePlace(i, x, z, yaw);
        if (i == g_Player) {
            layout.start = true;
            layout.startX = x;
            layout.startZ = z;
            layout.startYaw = yaw;
            continue;
        }
        g_Fighters[i].placedId = kDevMapFirstId + uint32_t(i);
        layout.characters.push_back({ classes[i], g_Fighters[i].placedId, x, z, yaw });
    }
    SetDevMapLayout(layout);
    SavePlayerChoice();
    SetPlayerClass(nullptr);
    SetPlayerClass(classes[g_Player].c_str(), false);
    SetFatalHitGuard(&PlayerFatalHit);
    SetCameraAdjuster(&HvvCamera); // in place of co-op's for this start
    g_Active = true;
    std::srand(unsigned(GetTickCount()));
    LOG_INFO("HvV: %s, %s, %s, %s against %s, %s, %s, %s; the player is %s; %d lives a side, %d health",
        classes[0].c_str(), classes[1].c_str(), classes[2].c_str(), classes[3].c_str(), classes[4].c_str(),
        classes[5].c_str(), classes[6].c_str(), classes[7].c_str(), classes[g_Player].c_str(), g_StartLives, g_Health);
}

bool HvvActive() { return g_Active; }

HvvState GetHvvState()
{
    HvvState s;
    s.active = g_Active;
    if (!g_Active)
        return s;
    s.started = g_Woken;
    const ULONGLONG now = GetTickCount64();
    if (g_Phase == Phase::Intro) {
        const ULONGLONG t = now - g_IntroAt;
        if (t < kWideShot) {
            s.caption = "HEROES VS VILLAINS";
            s.captionFade = float(t) / kWideShot;
        } else if (t < kWideShot + kFighters * kFighterShot) {
            const int k = int((t - kWideShot) / kFighterShot);
            s.caption = DisplayNameOf(g_Fighters[k].className);
            s.captionSide = SideOf(k);
            s.captionFade = float((t - kWideShot) % kFighterShot) / kFighterShot;
        }
    } else if (g_Phase == Phase::Fight && now - g_FightAt < kFightBanner) {
        s.caption = "FIGHT!";
        s.captionFade = float(now - g_FightAt) / kFightBanner;
    }
    s.winner = g_Winner;
    s.player = g_Player;
    s.startLives = g_StartLives;
    for (int side = 0; side < 2; ++side)
        s.lives[side] = g_Lives[side];
    for (int i = 0; i < kFighters; ++i) {
        s.fighters[i].className = g_Fighters[i].className;
        s.fighters[i].standing = g_Woken && Standing(g_Fighters[i]);
        s.fighters[i].out = g_Fighters[i].out;
        if (s.fighters[i].standing)
            ++s.standing[SideOf(i)];
    }
    return s;
}

std::vector<std::string> HvvDefaultLineup()
{
    return std::vector<std::string>(std::begin(kDefaultLineup), std::end(kDefaultLineup));
}

std::vector<std::string> HvvFighterClasses()
{
    std::vector<std::string> out;
    for (const DisplayName& n : kNames)
        if (const char* name = RegisteredClassName(n.className); name && ClassHasBody(name))
            out.push_back(name);
    return out;
}

bool StartHvv(const std::vector<std::string>& lineup, int player, int lives, int health, bool intro, std::string& error)
{
    if (lineup.size() != kFighters) {
        error = "a match needs 8 fighters";
        return false;
    }
    std::string text;
    for (const std::string& c : lineup) {
        const char* name = RegisteredClassName(c.c_str());
        if (!name) {
            error = c + ": not a class the game knows";
            return false;
        }
        if (!ClassHasBody(name)) {
            error = std::string(name) + " was cut from the game";
            return false;
        }
        text += (text.empty() ? "" : ",") + std::string(name);
    }
    if (player < 0 || player >= kFighters)
        player = 0;
    StartDevMap("hvv:" + text + " hvvplayer:" + std::to_string(player) +
        " hvvlives:" + std::to_string(lives > 0 ? lives : kDefaultLives) +
        " hvvhealth:" + std::to_string(health > 0 ? health : kDefaultHealth) + (intro ? "" : " hvvintro:0"));
    return true;
}

void HvvFrame()
{
    if (!g_Active)
        return;
    uint8_t* player = PlayerObject();
    if (!player)
        return;
    const ULONGLONG now = GetTickCount64();
    if (!g_InLevelAt)
        g_InLevelAt = now;
    if (!g_Woken) {
        if (now - g_InLevelAt < kWakeAfter)
            return;
        g_Woken = true;
        Track(g_Fighters[g_Player], player);
        FullHealth(player);
        for (int i = 0; i < kFighters; ++i) {
            Fighter& f = g_Fighters[i];
            if (i == g_Player)
                continue;
            uint8_t* placed = nullptr;
            for (uint8_t* c : LevelCharacters())
                if (Field<uint32_t>(c, 4) == f.placedId)
                    placed = c;
            if (!placed) {
                LOG_WARN("HvV: %s (id 0x%X) is not in the level", f.className.c_str(), f.placedId);
                continue;
            }
            Track(f, placed);
            if (Field<float>(placed, kCharacterHealth) <= 0)
                reinterpret_cast<void(__fastcall*)(uint8_t*, void*)>(
                    (*reinterpret_cast<void* const* const*>(placed))[kActivate / 4])(placed, nullptr);
            SetCharacterSide(placed, SideOf(i) == SideOf(g_Player) ? SpawnSide::Ally : SpawnSide::Enemy);
            FullHealth(placed);
        }
        if (g_IntroWanted) {
            g_Phase = Phase::Intro;
            g_IntroAt = now;
            HoldEveryone(true);
            LOG_INFO("HvV: the intro");
        } else {
            g_Phase = Phase::Fight;
            g_FightAt = now;
            LOG_INFO("HvV: the fight is on");
        }
        return;
    }
    if (g_Phase == Phase::Intro) {
        const ULONGLONG t = now - g_IntroAt;
        if (t >= kWideShot && t < kWideShot + kFighters * kFighterShot) {
            const int shot = int((t - kWideShot) / kFighterShot);
            if (shot != g_ShotPlayed) {
                g_ShotPlayed = shot;
                if (g_Fighters[shot].object && Standing(g_Fighters[shot]))
                    PlayIntro(g_Fighters[shot].object);
            }
        }
        if (t < IntroLength())
            return;
        HoldEveryone(false);
        g_Phase = Phase::Fight;
        g_FightAt = now;
        LOG_INFO("HvV: the fight is on");
    }

    // The player's fatal hit: its fall plays out (a killing blow is often a move both play, the victim's ending on
    // the ground), then a fresh fighter of its class takes over the controls (the live player change), at a safe
    // place, with full health and a moment's shield; a life of its side goes.
    Fighter& me = g_Fighters[g_Player];
    if (g_PlayerFell && now - g_PlayerDownAt >= kPlayerDown) {
        g_PlayerFell = false;
        --g_Lives[SideOf(g_Player)];
        std::string error;
        if (ReplacePlayer(error))
            player = PlayerObject();
        else
            LOG_WARN("HvV: a fresh fighter could not take over (%s); you get up where you are", error.c_str());
        Track(me, player);
        float m[16];
        SafePlace(g_Player, m);
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, const float*)>(
            (*reinterpret_cast<void* const* const*>(player))[kPlace / 4])(player, nullptr, m);
        FullHealth(player);
        Shield(me, now);
        LOG_INFO("HvV: you came back (%d lives left)", g_Lives[SideOf(g_Player)]);
    }

    for (int i = 0; i < kFighters; ++i) {
        Fighter& f = g_Fighters[i];
        if (f.shieldUntil && now >= f.shieldUntil) {
            f.shieldUntil = 0;
            if (f.object && ObjectById(f.instance) == f.object)
                Field<uint8_t>(f.object, kCharacterInvincible) = 0;
        }
        if (i == g_Player || f.out || g_Winner >= 0)
            continue;
        if (!f.fellAt) {
            if (!Standing(f))
                f.fellAt = now;
            continue;
        }
        if (now - f.fellAt < kRespawnAfter)
            continue;
        // Its death has played out: a new one of its class comes back, while its side has lives.
        const int side = SideOf(i);
        if (g_Lives[side] <= 0) {
            f.out = true;
            f.fellAt = 0;
            LOG_INFO("HvV: %s is out", f.className.c_str());
            continue;
        }
        float m[16];
        SafePlace(i, m);
        std::string error;
        uint8_t* back = SpawnCharacterAt(f.className.c_str(), "", "", "",
            side == SideOf(g_Player) ? SpawnSide::Ally : SpawnSide::Enemy, m, error);
        if (!back) {
            LOG_WARN("HvV: %s could not come back: %s", f.className.c_str(), error.c_str());
            f.out = true;
            continue;
        }
        --g_Lives[side];
        Track(f, back);
        FullHealth(back);
        Shield(f, now);
        LOG_INFO("HvV: %s came back (%d lives left)", f.className.c_str(), g_Lives[side]);
    }
    CheckWinner();
}

} // namespace swrots::game
