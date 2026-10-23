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

namespace swrots::game {

namespace {

constexpr int kSide = 4;            // fighters a side
constexpr int kFighters = 2 * kSide;
constexpr float kLineGap = 300.0f;  // between fighters in a line
constexpr float kLinesApart = 800.0f;
constexpr float kPi = 3.14159265f;
constexpr ULONGLONG kWakeAfter = 1500;   // ms after the player is in the level: the placed fighters join
constexpr int kDefaultLives = 35;        // a side's
constexpr int kDefaultHealth = 500;      // every fighter's (a Jedi has 1000 of its own: long fights)
constexpr ULONGLONG kRespawnAfter = 4000; // ms after a fighter fell (its death plays out first)
constexpr ULONGLONG kShieldFor = 2000;   // ms a fighter that came back cannot be hurt
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
bool g_PlayerFell = false; // a fatal hit on the player, kept alive: it comes back on the next frame

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
    g_PlayerFell = true;
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

bool StartHvv(const std::vector<std::string>& lineup, int player, int lives, int health, std::string& error)
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
        " hvvhealth:" + std::to_string(health > 0 ? health : kDefaultHealth));
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
        LOG_INFO("HvV: the fight is on");
        return;
    }

    // The player came back from a fatal hit: a life of its side, a safe place, full health, a moment's shield.
    Fighter& me = g_Fighters[g_Player];
    if (g_PlayerFell) {
        g_PlayerFell = false;
        if (me.object != player)
            Track(me, player);
        --g_Lives[SideOf(g_Player)];
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
