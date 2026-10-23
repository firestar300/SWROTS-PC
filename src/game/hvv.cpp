// Heroes vs Villains on the developer map (see hvv.h).

#include "game/hvv.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <sstream>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "game/characters.h"
#include "game/coop.h"
#include "game/customchars.h"
#include "game/custommaps.h"
#include "game/devmap.h"
#include "game/freecam.h"
#include "game/game.h"
#include "input/controls.h"
#include "kernel/kernel.h"

namespace swrots::game {

namespace {

constexpr int kMaxSide = 8;         // fighters a side, at most (the default line-up has four)
constexpr int kMaxFighters = 2 * kMaxSide;
constexpr float kLineGap = 150.0f;  // between fighters in a line (the dev map's floor; a map has its own)
constexpr float kLinesApart = 700.0f;
constexpr float kPi = 3.14159265f;
constexpr ULONGLONG kWakeAfter = 500;     // ms after the player is in the level: the placed fighters join
constexpr int kDefaultLives = 35;         // a side's
constexpr int kDefaultHealth = 250;       // every fighter's (a Jedi has 1000 of its own: long duels, not a battle)
// Every fighter keeps a health of kHealthScale; the mode's health is made by scaling the damage taken (250: four
// times). The HUD's bar and its frame are as long as the maximum (x 1/1500, 0x25D1A0): a Jedi's own 1000 matches the
// Force bar under it (1000 too); a low maximum would show a stub.
constexpr float kHealthScale = 1000.0f;
constexpr ULONGLONG kRespawnAfter = 4000; // ms after a fighter fell (its death plays out first)
constexpr ULONGLONG kShieldFor = 2000;    // ms a fighter that came back cannot be hurt
// The player's death (as Battlefront's): its body dies where it fell while the camera goes from it to look at its
// killer; then it comes back.
constexpr ULONGLONG kPlayerDown = 4500;   // ms from the fatal hit to coming back
constexpr ULONGLONG kKillCamIn = 1000;    // ms: the camera from the game's to the kill camera
constexpr ULONGLONG kKillCamBack = 500;   // ms: back to the game's camera, behind the fighter that came back
constexpr ULONGLONG kEngageEvery = 1000;  // ms: fighters without an enemy to fight are given one
// The AI walks straight at its target (the dev map has no paths): a wall or the bar between them stops it. A fighter
// that stays within kStuckMove units for kStuckFor while its target is further than kStuckReach is brought near it.
constexpr ULONGLONG kStuckFor = 5000;
constexpr float kStuckMove = 40.0f, kStuckReach = 260.0f;
constexpr ULONGLONG kIdleFor = 12000; // beside its target, no blow either way this long: its target is given again
// Where a fighter comes back: on the floor, just outside the fight, facing it.
constexpr float kFloorX0 = 1500.0f, kFloorX1 = 24000.0f, kFloorZ0 = 2700.0f, kFloorZ1 = 13500.0f;
constexpr float kFloorMargin = 600.0f;
constexpr float kRespawnReach = 950.0f;  // from the fight's middle, at most
constexpr float kRespawnClear = 380.0f;  // from every fighter, at least (when it can)
constexpr int kRespawnTries = 32;
// The intro (ms): for each side a frontal shot of its line playing its Versus intros, then a close shot along the
// line; the camera back behind the player; "FIGHT!" shows a while after.
constexpr ULONGLONG kFrontShot = 2800, kCloseShot = 2200, kToPlayer = 900, kFightBanner = 1500;
constexpr ULONGLONG kSideShots = kFrontShot + kCloseShot;
constexpr ULONGLONG kShotsLength = 2 * kSideShots;
// Defeats, assists and points.
constexpr ULONGLONG kKillerWithin = 10000; // ms: the last enemy hit this recent made the defeat
constexpr float kLikelyReach = 450.0f;     // a hit naming no attacker: an enemy this near dealt it
constexpr ULONGLONG kAssistWithin = 15000; // ms: other enemies' hits this recent are assists
constexpr int kDefeatPoints = 100, kAssistPoints = 50, kStreakPoints = 25;
constexpr ULONGLONG kNoticeFor = 3500, kFeedFor = 7000;
constexpr size_t kFeedLines = 6;
// The end: the top five, lined up, after the winner is known for a moment; then back to the main menu.
constexpr ULONGLONG kEndAfter = 2500;
constexpr ULONGLONG kMenuAfter = 12000; // ms of the top five

constexpr const char* kDefaultLineup[] = { "IObiwan", "IYoda", "IMace", "ISerra",
    "IVader", "IDooku", "IGrievous", "IAnakin" };

// The game's character fields and calls the match uses.
constexpr uint32_t kActivate = 0xB4;           // object vfunc, thiscall (): a placed character joins the level
constexpr uint32_t kPlace = 0x1F4;             // object vfunc, thiscall (const float m[16])
constexpr uintptr_t kPlaySequence = 0x150E30;  // ICharacter, thiscall (sequence, 0, 0, 0, speed): what Versus' intro calls
constexpr uint32_t kObjectById = 0x000A31F0;   // cdecl (instance id) -> object or null
constexpr uint32_t kChangeHealth = 0x00151500; // character, thiscall (float change, a, attacker): co-op hooks it
constexpr uint32_t kCharacterHealth = 0x130;
constexpr uint32_t kCharacterMaxHealth = 0x134;
constexpr uint32_t kCharacterPower = 0xA40;    // Force power, and its maximum (0 for those without)
constexpr uint32_t kCharacterMaxPower = 0xA44;
constexpr uint32_t kCharacterDead = 0x12C;     // byte: its death played out
constexpr uint32_t kCharacterTransform = 0x150; // rows right, up, forward, position
constexpr uint32_t kCharacterScript = 0x434;   // its script instance (a hit can name it as the attacker)
constexpr uint32_t kCharacterInvincible = 0x5D6; // byte, "Invincible?"
constexpr uint32_t kCharacterAI = 0x9FC;       // its AI controller
constexpr uint32_t kAITarget = 0x41C;          // the controller's target
constexpr uint32_t kAISetTarget = 0x00193D90;  // thiscall on the controller (target)
// A hit's attacker (the health change's last argument) is the character, its script instance, one of its
// weapons (+0x1080.. +0x10A0: its sabers, ILightSaber) or a projectile of its (IScriptProjectile), which points
// back at it.
constexpr uint32_t kWeaponsFrom = 0x1080, kWeaponsTo = 0x10A4;
constexpr uint32_t kOwnerSearch = 0x300;

struct Hit {
    int by;
    ULONGLONG at;
};

struct Fighter {
    std::string className;
    uint32_t placedId = 0;   // the placed object's id (none for the player)
    uint8_t* object = nullptr;
    uint32_t instance = 0;   // its instance id (+4), to tell it is still there
    ULONGLONG fellAt = 0;    // when it fell (0: fighting)
    ULONGLONG shieldUntil = 0;
    bool out = false;        // fell with its side out of lives
    std::deque<Hit> hits;    // enemy hits taken, newest last
    int score = 0, defeats = 0, falls = 0, assists = 0, streak = 0;
    float damage = 0;
    float stillAt[3] = {};   // where it was when it last moved on (stuck: see Unstick)
    ULONGLONG stillSince = 0;
    ULONGLONG lastBlow = 0;  // when it last dealt or took an enemy's hit (idle beside its target: see Unstick)
};

bool g_Active = false;
Fighter g_Fighters[kMaxFighters];
int g_Count = 0;  // fighters in the match
int g_Heroes = 0; // of which heroes (the first ones; the villains after)
int g_Player = 0;
int g_StartLives = kDefaultLives;
int g_Health = kDefaultHealth;
int g_Lives[2] = {};
int g_Winner = -1;
ULONGLONG g_InLevelAt = 0, g_WinnerAt = 0, g_LastEngage = 0;
bool g_Woken = false;
bool g_PlayerFell = false; // a fatal hit on the player: its old body dies, then it comes back
ULONGLONG g_PlayerDownAt = 0;
int g_PlayerKiller = -1;      // who defeated the player last (the kill camera looks at it), or -1
bool g_Swapped = false;       // the controls went to a fresh fighter; the old body died
uint8_t* g_Corpse = nullptr;  // the player's old body
uint32_t g_CorpseId = 0;
float g_CorpseAt[3] = {};     // where it fell
float g_DeathView[16] = {};   // the game's camera when the player fell (the kill camera starts there)
bool g_DeathViewSet = false;
float g_KillView[16] = {};    // the kill camera's last placement (the way back starts there)
ULONGLONG g_BackAt = 0;       // when the player came back (the camera returns)
bool g_IntroWanted = true;
enum class Phase { Waiting, Intro, Fight, End } g_Phase = Phase::Waiting;
ULONGLONG g_IntroAt = 0, g_FightAt = 0, g_EndAt = 0;
bool g_MenuAsked = false;   // the way back to the menus is on its way
uint8_t* g_Podium[5] = {};  // the end's top five: fresh characters on the podium
bool g_PodiumIntros = false; // their intros are still to start
int g_ShotPlayed = -1;      // the last intro shot whose line played its intros
float g_LastShot[16] = {};  // the intro camera's last placement, for the way back to the game's
bool g_BoardPinned = false;
std::vector<int> g_Top;     // the end's top five
// the arena: the floor's, or a custom map's (its lines' gap and distance, where fighters can come back)
float g_LineGap = kLineGap, g_LinesApart = kLinesApart;
float g_Bounds[4] = { kFloorX0, kFloorZ0, kFloorX1, kFloorZ1 };
float g_Margin = kFloorMargin;
std::vector<std::pair<float, float>> g_Spawns; // a map's own places to come back (the dev map's coordinates)
std::string g_MapName;
int g_KillEye = -1; // the kill camera's place last chosen (kept while it sees)

struct FeedEntry {
    int killer, victim;
    ULONGLONG at;
};
std::deque<FeedEntry> g_Feed;
struct Notice {
    std::string text, sub;
    int points = 0, side = 0;
    ULONGLONG at = 0;
} g_Notice;

// The fighters' names on screen.
struct DisplayName {
    const char* className;
    const char* name;
};
constexpr DisplayName kNames[] = { { "IAnakin", "ANAKIN SKYWALKER" }, { "IObiwan", "OBI-WAN KENOBI" },
    { "IOldObiwan", "OBI-WAN KENOBI" }, { "IYoda", "YODA" }, { "IMace", "MACE WINDU" },
    { "ICinDrallig", "CIN DRALLIG" }, { "IVader", "DARTH VADER" }, { "IDooku", "COUNT DOOKU" },
    { "IGrievous", "GENERAL GRIEVOUS" }, { "ISerra", "SERRA KETO" }, { "IPalpatine", "DARTH SIDIOUS" } };

// Just above a fighter's head, from its feet (the humans' bodies are 73-75 units tall).
float MarkerHeight(const std::string& className)
{
    std::string base = className;
    if (const CustomCharacter* custom = FindCustomCharacter(className.c_str()))
        base = custom->base;
    if (_stricmp(base.c_str(), "IYoda") == 0)
        return 42.0f;
    if (_stricmp(base.c_str(), "IGrievous") == 0)
        return 102.0f;
    return 86.0f;
}

std::string DisplayNameOf(const std::string& className)
{
    if (const CustomCharacter* custom = FindCustomCharacter(className.c_str()))
        return custom->name;
    for (const DisplayName& n : kNames)
        if (_stricmp(n.className, className.c_str()) == 0)
            return n.name;
    std::string name = className.size() > 1 && className[0] == 'I' ? className.substr(1) : className;
    for (char& c : name)
        c = char(toupper(static_cast<unsigned char>(c)));
    return name;
}

// A fighter's name as shown: its character's, the player's too (its lines are marked as its own).
std::string ShownName(int i)
{
    return DisplayNameOf(g_Fighters[i].className);
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

int SideOf(int slot) { return slot < g_Heroes ? 0 : 1; }
int SideStart(int side) { return side == 0 ? 0 : g_Heroes; }
int SideEnd(int side) { return side == 0 ? g_Heroes : g_Count; }
int SideSize(int side) { return SideEnd(side) - SideStart(side); }

// A place in the line-up: the heroes' line faces the villains' across the middle of the floor.
void LinePlace(int slot, float& x, float& z, float& yaw)
{
    const int k = slot - SideStart(SideOf(slot));
    x = kDevMapMiddleX + (k - (SideSize(SideOf(slot)) - 1) / 2.0f) * g_LineGap;
    z = kDevMapMiddleZ + (SideOf(slot) == 0 ? -g_LinesApart / 2 : g_LinesApart / 2);
    yaw = SideOf(slot) == 0 ? 0.0f : kPi; // a character faces +z at yaw 0
}

// A placement on the floor facing `yaw`, `lift` above it (60 for one that may fall onto it: a spawn's; a fighter
// set down on the floor itself keeps playing what it plays, one that falls is stopped by its fall).
void Transform(float x, float z, float yaw, float (&m)[16], float lift = 60.0f)
{
    const float c = std::cos(yaw), s = std::sin(yaw);
    const float rows[16] = { c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, x, kDevMapFloorY + lift, z, 1 };
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

template <typename F>
F VirtualOf(uint8_t* object, uint32_t offset)
{
    return reinterpret_cast<F>((*reinterpret_cast<void* const* const*>(object))[offset / 4]);
}

void Track(Fighter& f, uint8_t* object)
{
    f.object = object;
    f.instance = object ? Field<uint32_t>(object, 4) : 0;
    f.fellAt = 0;
    f.hits.clear();
}

void FullHealth(uint8_t* character)
{
    Field<float>(character, kCharacterMaxHealth) = kHealthScale;
    Field<float>(character, kCharacterHealth) = kHealthScale;
}

// Still in the level (`placed`), and standing.
bool Placed(Fighter& f)
{
    if (!f.object || ObjectById(f.instance) != f.object) {
        f.object = nullptr;
        return false;
    }
    return true;
}

bool Standing(Fighter& f)
{
    return Placed(f) && Field<float>(f.object, kCharacterHealth) > 0 && !Field<uint8_t>(f.object, kCharacterDead);
}

const float* Position(const uint8_t* character)
{
    return reinterpret_cast<const float*>(character + kCharacterTransform + 48);
}

int FighterOf(const uint8_t* object)
{
    for (int i = 0; i < g_Count; ++i)
        if (object && g_Fighters[i].object == object)
            return i;
    return -1;
}

// The fighter behind a hit's attacker object (see kWeaponsFrom), or -1.
int AttackerOf(uint8_t* attacker)
{
    if (!attacker)
        return -1;
    for (int i = 0; i < g_Count; ++i) {
        uint8_t* f = g_Fighters[i].object;
        if (!f)
            continue;
        if (f == attacker || Field<uint8_t*>(f, kCharacterScript) == attacker)
            return i;
        for (uint32_t at = kWeaponsFrom; at < kWeaponsTo; at += 4)
            if (Field<uint8_t*>(f, at) == attacker)
                return i;
    }
    __try { // a projectile: its own pointer back to its owner
        for (uint32_t at = 4; at < kOwnerSearch; at += 4) {
            uint8_t* p = Field<uint8_t*>(attacker, at);
            for (int i = 0; i < g_Count; ++i)
                if (p && g_Fighters[i].object && (p == g_Fighters[i].object ||
                        p == Field<uint8_t*>(g_Fighters[i].object, kCharacterScript)))
                    return i;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return -1;
}

// Somewhere to come back: just outside the fight (near its middle, as far from every standing fighter as the tries
// find, at least kRespawnClear when one does), facing the middle.
void SafePlace(int except, float (&m)[16])
{
    float cx = 0, cz = 0;
    int n = 0;
    std::vector<std::pair<float, float>> others;
    for (int i = 0; i < g_Count; ++i) {
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
    float bestX = cx, bestZ = cz, bestScore = -1e9f;
    const int tries = g_Spawns.empty() ? kRespawnTries : int(g_Spawns.size());
    for (int t = 0; t < tries; ++t) {
        float x, z;
        if (!g_Spawns.empty()) { // a map's own places
            x = g_Spawns[t].first;
            z = g_Spawns[t].second;
        } else {
            const float angle = float(std::rand()) / RAND_MAX * 2 * kPi;
            const float reach = kRespawnClear + float(std::rand()) / RAND_MAX * (kRespawnReach - kRespawnClear);
            x = std::clamp(cx + std::sin(angle) * reach, g_Bounds[0] + g_Margin, g_Bounds[2] - g_Margin);
            z = std::clamp(cz + std::cos(angle) * reach, g_Bounds[1] + g_Margin, g_Bounds[3] - g_Margin);
        }
        float clear = 1e9f;
        for (const auto& o : others)
            clear = std::min(clear, std::hypot(x - o.first, z - o.second));
        // clear enough, then as near the middle as can be
        const float score = std::min(clear, kRespawnClear) * 4 - std::hypot(x - cx, z - cz);
        if (score > bestScore) {
            bestScore = score;
            bestX = x;
            bestZ = z;
        }
    }
    Transform(bestX, bestZ, std::atan2(cx - bestX, cz - bestZ), m);
}

void Shield(Fighter& f, ULONGLONG now)
{
    Field<uint8_t>(f.object, kCharacterInvincible) = 1;
    f.shieldUntil = now + kShieldFor;
}

// --- Fighting ----------------------------------------------------------------------------------------------------

// A fighter of the AI's that has no enemy to fight (none, or one that fell) is set on the nearest standing enemy,
// as the AI's own target choice would once provoked: a fighter that came back, or one that stood waiting.
void Engage(int i)
{
    Fighter& f = g_Fighters[i];
    if (i == g_Player || !Standing(f))
        return;
    uint8_t* brain = Field<uint8_t*>(f.object, kCharacterAI);
    if (!brain)
        return;
    uint8_t* target = Field<uint8_t*>(brain, kAITarget);
    const int current = FighterOf(target);
    if (current >= 0 && SideOf(current) != SideOf(i) && Standing(g_Fighters[current]))
        return;
    const float* p = Position(f.object);
    int best = -1;
    float bestDistance = 1e9f;
    for (int k = 0; k < g_Count; ++k) {
        if (SideOf(k) == SideOf(i) || !Standing(g_Fighters[k]) ||
            (k == g_Player && (g_PlayerFell || g_Fighters[k].out))) // the player's fresh fighter, waiting
            continue;
        const float* q = Position(g_Fighters[k].object);
        const float d = std::hypot(p[0] - q[0], p[2] - q[2]);
        if (d < bestDistance) {
            bestDistance = d;
            best = k;
        }
    }
    if (best >= 0)
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, uint8_t*)>(uintptr_t(kAISetTarget))(
            brain, nullptr, g_Fighters[best].object);
}

bool LineOfSight(const float a[3], const float b[3]); // below, with the cameras

// A fighter of the AI's that cannot reach its target (see kStuckFor): moved to a place near the target that sees it
// and is clear of the map's boxes.
void Unstick(int i, ULONGLONG now)
{
    Fighter& f = g_Fighters[i];
    if (i == g_Player || !Standing(f) || !DevMapCustomMap())
        return;
    const float* p = Position(f.object);
    if (!f.stillSince || std::hypot(p[0] - f.stillAt[0], p[2] - f.stillAt[2]) > kStuckMove) {
        std::memcpy(f.stillAt, p, sizeof f.stillAt);
        f.stillSince = now;
        return;
    }
    if (now - f.stillSince < kStuckFor)
        return;
    uint8_t* brain = Field<uint8_t*>(f.object, kCharacterAI);
    const int target = brain ? FighterOf(Field<uint8_t*>(brain, kAITarget)) : -1;
    if (target < 0 || !Standing(g_Fighters[target]))
        return;
    const float* t = Position(g_Fighters[target].object);
    auto setTarget = [&] {
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, uint8_t*)>(uintptr_t(kAISetTarget))(
            brain, nullptr, g_Fighters[target].object);
    };
    if (std::hypot(p[0] - t[0], p[2] - t[2]) < kStuckReach) {
        // near its target and not fighting: out of its reach (on a platform the AI cannot climb), brought beside it
        const ULONGLONG last = std::max(f.lastBlow, g_Fighters[target].lastBlow);
        if (now - std::max(last, f.stillSince) < kIdleFor)
            return;
        f.lastBlow = now;
    }
    f.stillSince = now;
    // a place around the target (its own side first, then all round, nearer to further) that sees the target and is
    // clear (nothing from the floor up to above a head there); else the map's spawn nearest the target that sees it
    const float head[3] = { t[0], t[1] + 40, t[2] };
    auto clear = [&](float x, float z) {
        const float low[3] = { x, t[1] + 5, z }, high[3] = { x, t[1] + 110, z }, chest[3] = { x, t[1] + 40, z };
        return LineOfSight(low, high) && LineOfSight(head, chest);
    };
    float bestX = 0, bestZ = 0;
    bool found = false;
    for (float radius : { 90.0f, 150.0f, 220.0f, 300.0f }) {
        for (int k = 0; k < 12 && !found; ++k) {
            const float angle = std::atan2(p[0] - t[0], p[2] - t[2]) + float(k) * kPi / 6;
            const float x = t[0] + std::sin(angle) * radius, z = t[2] + std::cos(angle) * radius;
            if (clear(x, z)) {
                bestX = x;
                bestZ = z;
                found = true;
            }
        }
        if (found)
            break;
    }
    float nearest = 1e9f;
    for (size_t k = 0; !found && k <= g_Spawns.size(); ++k) {
        if (k == g_Spawns.size()) {
            found = nearest < 1e9f;
            break;
        }
        const float d = std::hypot(g_Spawns[k].first - t[0], g_Spawns[k].second - t[2]);
        if (d < nearest && clear(g_Spawns[k].first, g_Spawns[k].second)) {
            nearest = d;
            bestX = g_Spawns[k].first;
            bestZ = g_Spawns[k].second;
        }
    }
    if (!found) {
        LOG_INFO("HvV: %s cannot reach %s, and no place near it is clear", ShownName(i).c_str(), ShownName(target).c_str());
        return;
    }
    float m[16];
    Transform(bestX, bestZ, std::atan2(t[0] - bestX, t[2] - bestZ), m);
    VirtualOf<void(__fastcall*)(uint8_t*, void*, const float*)>(f.object, kPlace)(f.object, nullptr, m);
    setTarget(); // its walk was cut short: set on the target anew
    LOG_INFO("HvV: %s could not reach %s; brought nearer", ShownName(i).c_str(), ShownName(target).c_str());
}

void EngageAll()
{
    for (int i = 0; i < g_Count; ++i)
        Engage(i);
}

void NoticeFor(const std::string& text, const std::string& sub, int points, int side, ULONGLONG now)
{
    g_Notice = { text, sub, points, side, now };
}

// A fighter fell: who defeated it (the last enemy that hit it, lately), who assisted (other enemies that hit it),
// the points, the streaks, the feed and the player's notices.
void Defeated(int victim, ULONGLONG now)
{
    Fighter& v = g_Fighters[victim];
    int killer = -1;
    for (auto it = v.hits.rbegin(); it != v.hits.rend(); ++it) {
        if (now - it->at <= kKillerWithin) {
            killer = it->by;
            break;
        }
    }
    std::vector<int> assists;
    for (const Hit& h : v.hits)
        if (h.by != killer && now - h.at <= kAssistWithin &&
            std::find(assists.begin(), assists.end(), h.by) == assists.end())
            assists.push_back(h.by);
    v.hits.clear();
    ++v.falls;
    v.streak = 0;
    if (killer >= 0) {
        Fighter& k = g_Fighters[killer];
        ++k.defeats;
        ++k.streak;
        const int points = kDefeatPoints + kStreakPoints * std::min(k.streak - 1, 8);
        k.score += points;
        if (killer == g_Player)
            NoticeFor("DEFEATED " + DisplayNameOf(v.className),
                k.streak >= 2 ? std::to_string(k.streak) + " KILLSTREAK" : "", points, SideOf(victim), now);
    }
    for (int a : assists) {
        ++g_Fighters[a].assists;
        g_Fighters[a].score += kAssistPoints;
        if (a == g_Player && killer != g_Player)
            NoticeFor("ASSIST " + DisplayNameOf(v.className), "", kAssistPoints, SideOf(victim), now);
    }
    if (victim == g_Player)
        g_PlayerKiller = killer;
    if (victim == g_Player)
        NoticeFor(killer >= 0 ? "DEFEATED BY " + DisplayNameOf(g_Fighters[killer].className) : "YOU FELL", "", 0,
            killer >= 0 ? SideOf(killer) : SideOf(victim), now);
    g_Feed.push_front({ killer, victim, now });
    while (g_Feed.size() > kFeedLines)
        g_Feed.pop_back();
    LOG_INFO("HvV: %s %s%s", ShownName(victim).c_str(), killer >= 0 ? "defeated by " : "fell",
        killer >= 0 ? ShownName(killer).c_str() : "");
}

// The enemy most likely behind a hit that names no attacker (Force lightning's steady damage, some finishing moves:
// the health change's attacker is null): the nearest standing enemy within reach, one whose AI is set on the
// victim first (the player, which has no AI, counts as one); -1 when none is near.
int LikelyAttacker(int victim)
{
    Fighter& v = g_Fighters[victim];
    if (!Placed(v))
        return -1;
    const float* at = Position(v.object);
    int best = -1;
    float bestScore = 1e9f;
    for (int i = 0; i < g_Count; ++i) {
        Fighter& f = g_Fighters[i];
        if (SideOf(i) == SideOf(victim) || !Standing(f))
            continue;
        const float* p = Position(f.object);
        const float d = std::hypot(p[0] - at[0], p[2] - at[2]);
        if (d > kLikelyReach)
            continue;
        uint8_t* brain = i == g_Player ? nullptr : Field<uint8_t*>(f.object, kCharacterAI);
        const bool onIt = i == g_Player || (brain && Field<uint8_t*>(brain, kAITarget) == v.object);
        const float score = d - (onIt ? kLikelyReach : 0);
        if (score < bestScore) {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

// Every hit on a fighter: the enemy that dealt it scores its damage and is remembered for the defeat.
void Damaged(uint8_t* character, float change, uint32_t, uint8_t* attacker)
{
    if (!g_Active || g_Phase != Phase::Fight || change >= 0)
        return;
    const int victim = FighterOf(character);
    int by = AttackerOf(attacker);
    if (victim >= 0 && by < 0 && !attacker) // a hit that names no one (Force lightning, some finishing moves)
        by = LikelyAttacker(victim);
    if (victim < 0 || by < 0 || SideOf(by) == SideOf(victim))
        return;
    Fighter& v = g_Fighters[victim];
    Fighter& k = g_Fighters[by];
    k.damage += -change * float(g_Health) / kHealthScale; // in the mode's health
    k.score = int(k.damage) + k.defeats * kDefeatPoints + k.assists * kAssistPoints; // damage counts as it comes
    v.hits.push_back({ by, GetTickCount64() });
    v.lastBlow = k.lastBlow = GetTickCount64();
    while (v.hits.size() > 32)
        v.hits.pop_front();
}

// Damage to a fighter, scaled to the mode's health (see kHealthScale).
float ScaleDamage(uint8_t* character, float change)
{
    if (!g_Active || change >= 0 || FighterOf(character) < 0)
        return change;
    return change * kHealthScale / float(g_Health);
}

// A fatal hit on the player: the game must never see it die (that ends the mission with its Game Over screen).
// While its side has lives it comes back; with none left it stays down, out, until the match ends.
bool PlayerFatalHit(uint8_t* character)
{
    if (!g_Active || !g_Woken || character != PlayerObject())
        return false;
    if (g_Winner >= 0 || g_Fighters[g_Player].out)
        return true;
    if (!g_PlayerFell) {
        g_PlayerFell = true;
        g_PlayerDownAt = GetTickCount64();
        g_Swapped = false;
        g_KillEye = -1;
        float fov = 0; // the camera as the hit lands (the kill camera starts there)
        g_DeathViewSet = GameCameraPlacement(g_DeathView, fov);
        g_BackAt = 0;
        std::memcpy(g_CorpseAt, Position(character), sizeof g_CorpseAt);
        Field<uint8_t>(character, kCharacterInvincible) = 1; // no more hits until its body is let die
        Defeated(g_Player, g_PlayerDownAt);
    }
    return true;
}

void CheckWinner(ULONGLONG now)
{
    if (g_Winner >= 0)
        return;
    for (int side = 0; side < 2; ++side) {
        if (g_Lives[side] > 0)
            continue;
        bool anyone = false;
        for (int i = SideStart(side); i < SideEnd(side); ++i)
            anyone |= (Standing(g_Fighters[i]) && !g_Fighters[i].out) || (g_Fighters[i].fellAt && !g_Fighters[i].out) ||
                (i == g_Player && g_PlayerFell);
        if (!anyone) {
            g_Winner = 1 - side;
            g_WinnerAt = now;
            LOG_INFO("HvV: the %s win", g_Winner == 0 ? "heroes" : "villains");
            return;
        }
    }
}

// --- Other fighters' slow motion and camera ------------------------------------------------------------------------
// The game was made for duels with the player in them: a move's slow motion and the saber lock's camera are global.
// In a match they reach the player only from moves it is part of (settings.ini [HvV] OthersSlowMotion=1: all).
//
// - Slow motion: the moves' scripts call 0x176BF0 (cdecl (float scale, int rampIn, int hold, int rampOut); it
//   skips when the script effects are off, byte 0x6943D0, else jumps to 0xA20E0, which sets the time manager's ramp:
//   the scale multiplies every frame's time). A slowing (scale < 1) holds for 9999 frames until the move's script
//   restores 1; there is no stack, so a restore from anyone ends anyone's.
// - The saber lock's camera ("combateffect", 0x2BB250, cdecl, 9 arguments, the first: on) frames the player and
//   the instigator's target; the shared saber-lock sequences call it from six places.
// - The instigator is the running script's character ([[0x6943CC] +0x68]); a kill move, grab or lock links the two
//   fighters ([0x6943CC] +0x23C: their instance ids at +0xC and +0x10).
constexpr uint32_t kScriptSlowMotion = 0x00176BF0;
constexpr uint8_t kScriptSlowMotionBytes[] = { 0xE8, 0xEB, 0x57, 0xFF, 0xFF, 0x80, 0x38, 0x00, 0x75, 0x05 };
constexpr uint32_t kSetSlowMotion = 0x000A20E0;    // cdecl (scale, rampIn, hold, rampOut)
constexpr uint32_t kCancelSlowMotion = 0x000A2110; // cdecl (): back to 1
constexpr uint32_t kScriptEffectsOff = 0x006943D0;
constexpr uint32_t kScriptContext = 0x006943CC;
constexpr uint32_t kCombatCamera = 0x002BB250;
constexpr uint32_t kCombatCameraCalls[] = { 0x00408B52, 0x00408BAE, 0x00408DE1, 0x00408E27, 0x0040907B, 0x004090C1 };

uint8_t* g_SlowOwner = nullptr;       // whose slowing is running (its restore must get through)
uint8_t* g_LockCameraOwner = nullptr; // whose lock camera is on

// The running script's character and whether the player is part of its move: the character itself, linked to it
// (a kill move, grab or lock), or its AI's target.
bool PlayerInMove(uint8_t** instigator)
{
    uint8_t* player = PlayerObject();
    *instigator = nullptr;
    if (!player)
        return true;
    __try {
        uint8_t* context = *reinterpret_cast<uint8_t**>(uintptr_t(kScriptContext));
        uint8_t* c = context ? *reinterpret_cast<uint8_t**>(context + 0x68) : nullptr;
        *instigator = c;
        if (!c || c == player)
            return true;
        const uint32_t playerId = *reinterpret_cast<uint32_t*>(player + 4);
        if (uint8_t* link = *reinterpret_cast<uint8_t**>(context + 0x23C))
            if (*reinterpret_cast<uint32_t*>(link + 0xC) == playerId || *reinterpret_cast<uint32_t*>(link + 0x10) == playerId)
                return true;
        uint8_t* brain = *reinterpret_cast<uint8_t**>(c + kCharacterAI);
        return brain && *reinterpret_cast<uint8_t**>(brain + kAITarget) == player;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

// Whether an effect (on: begun; off: ended) passes, keeping its owner so that its end gets through.
bool EffectPasses(bool on, uint8_t*& owner)
{
    if (!g_Active || GetSettings().hvvOthersSlowMotion)
        return true;
    uint8_t* by = nullptr;
    const bool mine = PlayerInMove(&by);
    if (on) {
        if (!mine)
            return false;
        owner = by;
        return true;
    }
    if (by == owner) {
        owner = nullptr;
        return true;
    }
    return mine;
}

void __cdecl ScriptSlowMotion(float scale, int rampIn, int hold, int rampOut)
{
    if (*reinterpret_cast<volatile uint8_t*>(uintptr_t(kScriptEffectsOff)))
        return;
    if (!EffectPasses(scale < 1.0f, g_SlowOwner))
        return;
    reinterpret_cast<void(__cdecl*)(float, int, int, int)>(uintptr_t(kSetSlowMotion))(scale, rampIn, hold, rampOut);
}

void __cdecl LockCamera(uint32_t on, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7,
    uint32_t a8, uint32_t a9)
{
    if (!EffectPasses((on & 0xFF) != 0, g_LockCameraOwner))
        return;
    reinterpret_cast<void(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
        uint32_t)>(uintptr_t(kCombatCamera))(on, a2, a3, a4, a5, a6, a7, a8, a9);
}

// Each start of a match (the game image is fresh): the hooks, the code checked first.
void InstallEffectHooks()
{
    g_SlowOwner = g_LockCameraOwner = nullptr;
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kScriptSlowMotion)), kScriptSlowMotionBytes,
            sizeof kScriptSlowMotionBytes) == 0)
        PatchJump(kScriptSlowMotion, reinterpret_cast<const void*>(&ScriptSlowMotion));
    else
        LOG_WARN("HvV: the slow-motion call is not as expected; other fighters' slow motion reaches the player");
    int hooked = 0;
    for (uint32_t site : kCombatCameraCalls) {
        const uint8_t* at = reinterpret_cast<const uint8_t*>(uintptr_t(site));
        int32_t rel;
        std::memcpy(&rel, at + 1, 4);
        if (at[0] != 0xE8 || site + 5 + uint32_t(rel) != kCombatCamera)
            continue;
        PatchCall(site, reinterpret_cast<const void*>(&LockCamera), 5);
        ++hooked;
    }
    if (hooked != int(std::size(kCombatCameraCalls)))
        LOG_WARN("HvV: %d of %zu saber-lock camera calls hooked", hooked, std::size(kCombatCameraCalls));
}

// A slowing whose owner fell or went (its restore will never come): time runs again.
void CheckSlowMotion()
{
    if (!g_SlowOwner)
        return;
    const int i = FighterOf(g_SlowOwner);
    if (i >= 0 && Standing(g_Fighters[i]))
        return;
    g_SlowOwner = nullptr;
    reinterpret_cast<void(__cdecl*)()>(uintptr_t(kCancelSlowMotion))();
}

// --- The profile across the match ---------------------------------------------------------------------------------
// A level change is a reboot; the game keeps the player's profile across one in `Z:\persist.tmp` (cache\persist.tmp):
// saved before it reboots (0x24D300, thiscall on the profile [0x7E8F24], which skips the write without a profile's
// data at +0x3C) and read by the next start only when its arguments say v_sessionCreated:1, which also lands the
// menus on the main menu (0x2FAFA0) instead of the title. A match started from the menus brings them along, with the
// controller and memory unit in use (the session settings [[0x66F7A4] +4]: +0x84, +0x80), and goes back to the main
// menu the same way.
constexpr uint32_t kProfile = 0x007E8F24;
constexpr uint32_t kSaveProfile = 0x0024D300;
constexpr uint32_t kSessionSettings = 0x0066F7A4;

// Saves the profile for the next start; false without one (a start from Default_Xbox.cfg, before the menus).
bool SaveProfile(int& controller, int& memoryUnit)
{
    __try {
        uint8_t* profile = *reinterpret_cast<uint8_t**>(uintptr_t(kProfile));
        if (!profile || !*reinterpret_cast<void**>(profile + 0x3C))
            return false;
        if (!reinterpret_cast<bool(__fastcall*)(uint8_t*, void*)>(uintptr_t(kSaveProfile))(profile, nullptr))
            return false;
        uint8_t* settings = *reinterpret_cast<uint8_t**>(*reinterpret_cast<uintptr_t*>(uintptr_t(kSessionSettings)) + 4);
        controller = settings ? *reinterpret_cast<int*>(settings + 0x84) : 0;
        memoryUnit = settings ? *reinterpret_cast<int*>(settings + 0x80) : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The arguments that carry the profile into the next start (empty without one).
std::string SessionArguments()
{
    int controller = 0, memoryUnit = 0;
    if (!SaveProfile(controller, memoryUnit))
        return {};
    return " v_sessionCreated:1 playlicensemovie:0 title:0 controller:" + std::to_string(controller) +
        " memoryunit:" + std::to_string(memoryUnit);
}

// After the end: back to the main menu with the profile, for a match started from the menus; one started from
// Default_Xbox.cfg (whose map: the game would read again) plays again.
void BackToMenu()
{
    std::string value;
    const std::string session = BootArgument("v_sessionCreated", value) && value == "1" ? SessionArguments() : "";
    if (session.empty()) {
        LOG_INFO("HvV: the match again (it was not started from the menus)");
        kernel::RestartMission();
        return;
    }
    LOG_INFO("HvV: back to the main menu");
    kernel::LaunchGame("map: v_shellBetweenLevels:0 v_versusMode:0 v_bonusMissionMode:0 "
        "v_challengeMissionMode:0 v_checkpointIndex:-1" + session);
}

// --- The intro and the end ---------------------------------------------------------------------------------------

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

// A character plays its Versus intro the way the duel starts one (0x27B550): its sequence named <...>_Intro, found
// in its script's list ([char +0x434] +4: +8 count, +0xC items, each named at +0xC; the duel's lookup 0x150D60),
// started with ICharacter's play (0x150E30, the Jedi's vfunc +0x314; its +0x410 is a reaction handler that
// ignored the intro). Yoda has none: one of his "flavour" idles instead.
void PlayIntro(uint8_t* character)
{
    uint8_t* script = Field<uint8_t*>(character, kCharacterScript);
    uint8_t* list = script ? Field<uint8_t*>(script, 4) : nullptr;
    if (!list)
        return;
    const int count = Field<int>(list, 8);
    uint8_t** items = Field<uint8_t**>(list, 0xC);
    uint8_t* flavour = nullptr; // for one without an intro (Yoda): its first "flavour" idle (Idle_Flavor_v1...)
    for (int i = 0; i < count; ++i) {
        const char* name = items[i] ? Field<const char*>(items[i], 0xC) : nullptr;
        const size_t length = name ? strlen(name) : 0;
        if (length > 6 && _stricmp(name + length - 6, "_Intro") == 0) {
            flavour = items[i];
            break;
        }
        if (!flavour && length >= 11 && _strnicmp(name, "Idle_Flavor", 11) == 0)
            flavour = items[i];
    }
    if (flavour)
        reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint8_t*, float, int, int, float)>(kPlaySequence)(
            character, nullptr, flavour, 0.0f, 0, 0, 1.0f);
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

ULONGLONG IntroLength() { return kShotsLength + kToPlayer; }

// The intro's shot `t` ms in: 0 the heroes' line from the front, 1 along it, 2 and 3 the villains'; -1 past them.
// `a`: how far through the shot (0 to 1).
int IntroShotAt(ULONGLONG t, float* a = nullptr)
{
    if (t >= kShotsLength)
        return -1;
    const ULONGLONG in = t % kSideShots;
    const bool close = in >= kFrontShot;
    if (a)
        *a = close ? float(in - kFrontShot) / kCloseShot : float(in) / kFrontShot;
    return int(t / kSideShots) * 2 + (close ? 1 : 0);
}

// The intro's camera `t` ms in; false past its shots (the way back blends towards the game's own).
bool IntroShot(ULONGLONG t, float m[16])
{
    float along = 0;
    const int shot = IntroShotAt(t, &along);
    if (shot < 0)
        return false;
    const int side = shot / 2;
    const float a = Smooth(along);
    const float floorY = kDevMapFloorY;
    const float lineZ = kDevMapMiddleZ + (side == 0 ? -g_LinesApart / 2 : g_LinesApart / 2);
    const float facing = side == 0 ? 1.0f : -1.0f; // the line faces +z (heroes) or -z
    const float x = kDevMapMiddleX;
    if (shot % 2 == 0) { // from the front, near, a little low, panning along the line (each fighter's intro in turn)
        const float half = (SideSize(side) - 1) / 2.0f * g_LineGap;
        const float along = (side == 0 ? -1.0f : 1.0f) * half * (0.75f - 1.5f * a);
        const float eye[3] = { x + along * 0.8f, floorY + 68, lineZ + facing * 230 };
        const float target[3] = { x + along, floorY + 52, lineZ };
        LookAt(eye, target, m);
    } else { // close, from one end of the line, along it at chest height
        const float end = (side == 0 ? 1.0f : -1.0f) * ((SideSize(side) - 1) / 2.0f * g_LineGap + 110);
        const float eye[3] = { x + end - (side == 0 ? 35 : -35) * a, floorY + 72, lineZ + facing * (85 + 15 * a) };
        const float target[3] = { x - end * 0.5f, floorY + 58, lineZ - facing * 10 };
        LookAt(eye, target, m);
    }
    return true;
}

// Between two camera placements (`a` 0 to 1): positions and aims blended. `out` may be `to`.
void Blend(const float from[16], const float to[16], float a, float out[16])
{
    float eye[3], target[3];
    for (int k = 0; k < 3; ++k) {
        eye[k] = from[12 + k] + (to[12 + k] - from[12 + k]) * a;
        const float f = from[12 + k] + from[8 + k] * 200, t = to[12 + k] + to[8 + k] * 200;
        target[k] = f + (t - f) * a;
    }
    LookAt(eye, target, out);
}

// The end: the top five lined up facing the camera, first in the middle (as Battlefront's end screen); the top three
// up front, fourth and fifth well behind them and to the sides (smaller in the picture).
constexpr float kPodium[5][2] = { { 0, 0 }, { -95, 30 }, { 95, 30 }, { -170, 110 }, { 170, 110 } }; // x, back
constexpr float kPodiumZ = 0; // relative to the floor's middle

// The podium stands just in front of where the heroes' line stood, its back row on the line (open floor on any map:
// behind it can be a wall), facing the villains' side, where the camera is.
float PodiumZ() { return kDevMapMiddleZ + kPodiumZ - g_LinesApart / 2 + kPodium[4][1]; }

void PodiumPlace(int rank, float (&m)[16])
{
    // facing +z (the camera): its left is -x on screen's right, so the podium's x is turned over (second on the left);
    // on the floor itself: a fall would stop the intro each plays there
    Transform(kDevMapMiddleX - kPodium[rank][0], PodiumZ() - kPodium[rank][1], 0, m, 1.0f);
}

void EndCamera(float m[16])
{
    const float back = std::min(290.0f, g_LinesApart - kPodium[4][1] - 30); // short of the villains' line
    const float a = Smooth(float(GetTickCount64() - g_EndAt) / 4000.0f);
    const float eye[3] = { kDevMapMiddleX, kDevMapFloorY + 75 - 10 * a, PodiumZ() + back - 40 * a };
    const float target[3] = { kDevMapMiddleX, kDevMapFloorY + 55, PodiumZ() - 30 };
    LookAt(eye, target, m);
}

// Whether the line from `a` to `b` passes no collision box of the map (always on the floor, which has none).
bool LineOfSight(const float a[3], const float b[3])
{
    const CustomMap* map = DevMapCustomMap();
    return !map || CustomMapLineOfSight(*map, a, b);
}

// The kill camera (as Battlefront's): from the side of the player's body the game's camera was on when it fell
// (a place the camera could be: no wall), nearer the body and a little higher, turned to look at its killer (the
// nearest enemy when no one was credited; the body when none is near). While the player is out of lives it follows
// a teammate still fighting, from the side it is on.
int NearestEnemy(const float at[3], float within)
{
    int best = -1;
    for (int i = 0; i < g_Count; ++i) {
        if (SideOf(i) == SideOf(g_Player) || !Standing(g_Fighters[i]))
            continue;
        const float* p = Position(g_Fighters[i].object);
        const float d = std::hypot(p[0] - at[0], p[2] - at[2]);
        if (d < within) {
            within = d;
            best = i;
        }
    }
    return best;
}

void KillView(float m[16])
{
    if (g_Corpse && ObjectById(g_CorpseId) == g_Corpse)
        std::memcpy(g_CorpseAt, Position(g_Corpse), sizeof g_CorpseAt);
    float target[3] = { g_CorpseAt[0], g_CorpseAt[1] + 20, g_CorpseAt[2] };
    float eye[3];
    if (g_Fighters[g_Player].out && !g_PlayerFell) { // out: a teammate's fight
        const float* from = g_KillView + 12;
        for (int i = SideStart(SideOf(g_Player)); i < SideEnd(SideOf(g_Player)); ++i) {
            if (i == g_Player || !Standing(g_Fighters[i]) || g_Fighters[i].out)
                continue;
            const float* p = Position(g_Fighters[i].object);
            const float dx = from[0] - p[0], dz = from[2] - p[2], d = std::max(1.0f, std::hypot(dx, dz));
            const float want[3] = { p[0] + dx / d * 220, p[1] + 95, p[2] + dz / d * 220 };
            for (int k = 0; k < 3; ++k)
                eye[k] = from[k] + (want[k] - from[k]) * 0.05f; // eased, frame by frame
            const float look[3] = { p[0], p[1] + 45, p[2] };
            LookAt(eye, look, m);
            return;
        }
        LookAt(from, target, m);
        return;
    }
    int look = g_PlayerKiller >= 0 && Standing(g_Fighters[g_PlayerKiller]) ? g_PlayerKiller : -1;
    if (look < 0)
        look = NearestEnemy(g_CorpseAt, 900);
    if (look >= 0) {
        const float* k = Position(g_Fighters[look].object);
        target[0] = k[0];
        target[1] = k[1] + 50;
        target[2] = k[2];
    }
    // the places tried: the first (on the game camera's side), then around the body, then above it; the one last
    // chosen is kept while it still sees both the body and the target (no jumping between places)
    const float body[3] = { g_CorpseAt[0], g_CorpseAt[1] + 30, g_CorpseAt[2] };
    auto place = [&](int i, float (&out)[3]) {
        if (i == 0) {
            for (int k = 0; k < 3; ++k)
                out[k] = g_DeathView[12 + k] + (g_CorpseAt[k] - g_DeathView[12 + k]) * 0.35f;
            out[1] = std::max(out[1], g_CorpseAt[1] + 70) + 25;
        } else if (i <= 8) {
            const float angle = float(i - 1) * kPi / 4;
            out[0] = g_CorpseAt[0] + std::sin(angle) * 150;
            out[1] = g_CorpseAt[1] + 95;
            out[2] = g_CorpseAt[2] + std::cos(angle) * 150;
        } else {
            out[0] = g_CorpseAt[0] + 20;
            out[1] = g_CorpseAt[1] + 140;
            out[2] = g_CorpseAt[2] - 20;
        }
    };
    auto sees = [&](const float (&from)[3]) { return LineOfSight(body, from) && LineOfSight(from, target); };
    if (g_KillEye >= 0) {
        place(g_KillEye, eye);
        if (!sees(eye))
            g_KillEye = -1;
    }
    for (int i = 0; g_KillEye < 0 && i <= 9; ++i) {
        place(i, eye);
        if (sees(eye) || i == 9)
            g_KillEye = i;
    }
    place(g_KillEye, eye);
    LookAt(eye, target, m);
}

// The camera during the intro, on the way back to the game's, at the player's death and at the end.
bool HvvCamera(float m[16], float)
{
    if (!g_Active)
        return false;
    if (g_Phase == Phase::End) {
        EndCamera(m);
        return true;
    }
    if (g_Phase == Phase::Fight) {
        const ULONGLONG now = GetTickCount64();
        if (g_PlayerFell || g_Fighters[g_Player].out) {
            if (!g_DeathViewSet) {
                std::copy(m, m + 16, g_DeathView);
                g_DeathViewSet = true;
            }
            float view[16];
            KillView(view);
            const float a = g_PlayerFell ? Smooth(float(now - g_PlayerDownAt) / kKillCamIn) : 1.0f;
            Blend(g_DeathView, view, a, m);
            std::copy(m, m + 16, g_KillView);
            return true;
        }
        if (g_BackAt && now - g_BackAt < kKillCamBack) { // back to the game's camera
            Blend(g_KillView, m, Smooth(float(now - g_BackAt) / kKillCamBack), m);
            return true;
        }
        return false;
    }
    if (g_Phase != Phase::Intro)
        return false;
    const ULONGLONG t = GetTickCount64() - g_IntroAt;
    if (IntroShot(t, m)) {
        std::copy(m, m + 16, g_LastShot);
        return true;
    }
    const ULONGLONG back = kShotsLength;
    Blend(g_LastShot, m, Smooth(float(t - back) / kToPlayer), m); // back to the game's own camera
    return true;
}

std::vector<int> Ranking()
{
    std::vector<int> order(g_Count);
    for (int i = 0; i < g_Count; ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(),
        [](int a, int b) { return g_Fighters[a].score > g_Fighters[b].score; });
    return order;
}

// The end begins: everyone holds still, the top five come to the podium (one that is down comes back there), the
// others go far away, and the five play their intros.
// The end: every character of the match (fighters, the player, the fallen) goes out of sight, and the top five
// stand on the podium as fresh characters of their classes: the fighters themselves would bring their fight along
// (lying from a blow, turned their own way, mid-move), and a character placed where it falls stops its intro.
void StartEnd(ULONGLONG now)
{
    g_Phase = Phase::End;
    g_EndAt = now;
    HoldEveryone(true);
    std::vector<int> order = Ranking();
    g_Top.assign(order.begin(), order.begin() + std::min(5, g_Count));
    int away = 0;
    for (uint8_t* c : LevelCharacters()) {
        float m[16];
        Transform(kDevMapMiddleX + 5000 + 250 * float(away % 20), kDevMapMiddleZ - 4000 - 250 * float(away / 20), 0, m);
        VirtualOf<void(__fastcall*)(uint8_t*, void*, const float*)>(c, kPlace)(c, nullptr, m);
        Field<uint8_t>(c, kCharacterInvincible) = 1;
        ++away;
    }
    for (size_t rank = 0; rank < 5; ++rank) {
        g_Podium[rank] = nullptr;
        if (rank >= g_Top.size())
            continue;
        float m[16];
        PodiumPlace(int(rank), m);
        std::string error;
        uint8_t* c = SpawnCharacterAt(g_Fighters[g_Top[rank]].className.c_str(), "", "", "", SpawnSide::Default, m, error);
        if (!c) {
            LOG_WARN("HvV: %s is not on the podium (%s)", ShownName(g_Top[rank]).c_str(), error.c_str());
            continue;
        }
        FullHealth(c);
        Field<uint8_t>(c, kCharacterInvincible) = 1;
        g_Podium[rank] = c;
    }
    g_PodiumIntros = true; // a moment after they stand (see HvvFrame)
    LOG_INFO("HvV: the end; first %s with %d", ShownName(g_Top[0]).c_str(), g_Fighters[g_Top[0]].score);
}

} // namespace

void InstallHvv()
{
    g_Active = false;
    g_InLevelAt = g_WinnerAt = g_LastEngage = 0;
    g_Woken = false;
    g_PlayerFell = false;
    g_Winner = -1;
    g_Phase = Phase::Waiting;
    g_IntroAt = g_FightAt = g_EndAt = 0;
    g_MenuAsked = false;
    g_ShotPlayed = -1;
    g_Feed.clear();
    g_Notice = {};
    g_Top.clear();
    for (Fighter& f : g_Fighters)
        f = {};
    SetFatalHitGuard(nullptr);
    SetDamageObserver(nullptr);
    SetDamageFilter(nullptr);
    std::string lineup, value;
    if (!DevMapActive() || !BootArgument("hvv", lineup)) {
        RestorePlayerChoice();
        return;
    }
    const std::vector<std::string> classes = Split(lineup, ',');
    if (classes.size() < 2 || classes.size() > size_t(kMaxFighters)) {
        LOG_WARN("HvV: the line-up needs 2 to %d classes, not %zu ('%s')", kMaxFighters, classes.size(), lineup.c_str());
        RestorePlayerChoice();
        return;
    }
    g_Count = int(classes.size());
    g_Heroes = BootArgument("hvvheroes", value) ? std::atoi(value.c_str()) : g_Count / 2;
    g_Heroes = std::clamp(g_Heroes, std::max(1, g_Count - kMaxSide), std::min(kMaxSide, g_Count - 1));
    g_Player = BootArgument("hvvplayer", value) ? std::atoi(value.c_str()) : 0;
    if (g_Player < 0 || g_Player >= g_Count)
        g_Player = 0;
    g_StartLives = BootArgument("hvvlives", value) ? std::max(1, std::atoi(value.c_str())) : kDefaultLives;
    g_Lives[0] = g_Lives[1] = g_StartLives;
    g_Health = BootArgument("hvvhealth", value) ? std::max(1, std::atoi(value.c_str())) : kDefaultHealth;
    g_IntroWanted = !BootArgument("hvvintro", value) || value != "0";
    g_LineGap = kLineGap;
    g_LinesApart = kLinesApart;
    const float floorBounds[4] = { kFloorX0, kFloorZ0, kFloorX1, kFloorZ1 };
    std::copy(std::begin(floorBounds), std::end(floorBounds), g_Bounds);
    g_Margin = kFloorMargin;
    g_Spawns.clear();
    g_MapName.clear();
    if (const CustomMap* map = DevMapCustomMap()) { // its lines (turned to face along z), its spawns and bounds
        g_LineGap = map->lineGap;
        g_LinesApart = std::hypot(map->villains[0] - map->heroes[0], map->villains[1] - map->heroes[1]);
        CustomMapBounds(*map, g_Bounds);
        g_Margin = map->hasBounds ? 0.0f : kFloorMargin;
        for (const auto& s : map->spawns) {
            float x, z;
            CustomMapToWorld(*map, s.first, s.second, x, z);
            g_Spawns.push_back({ x, z });
        }
        g_MapName = map->name;
    }

    DevMapLayout layout;
    for (int i = 0; i < g_Count; ++i) {
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
    SetDamageObserver(&Damaged);
    SetDamageFilter(&ScaleDamage);
    SetCameraAdjuster(&HvvCamera); // in place of co-op's for this start
    InstallEffectHooks();
    g_Active = true;
    std::srand(unsigned(GetTickCount()));
    std::string heroes, villains;
    for (int i = 0; i < g_Count; ++i)
        (i < g_Heroes ? heroes : villains) += (i == 0 || i == g_Heroes ? "" : ", ") + classes[i];
    LOG_INFO("HvV: %s against %s; the player is %s; %d lives a side, %d health; on %s", heroes.c_str(),
        villains.c_str(), classes[g_Player].c_str(), g_StartLives, g_Health,
        g_MapName.empty() ? "the dev map" : g_MapName.c_str());
}

bool HvvActive() { return g_Active; }

// The game's pause: the time manager (TManager_Time, [[0x68D944] +0x5C]) has a frozen state on its stack (+0x14 set,
// +0x38 its entries: only the pause pushes one, 0xA7A30; see fixes.cpp).
bool GamePaused()
{
    __try {
        uint8_t* services = *reinterpret_cast<uint8_t**>(uintptr_t(0x0068D944));
        uint8_t* time = services ? *reinterpret_cast<uint8_t**>(services + 0x5C) : nullptr;
        return time && time[0x14] && *reinterpret_cast<uint32_t*>(time + 0x38) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool HvvLineOfSight(const float a[3], const float b[3]) { return !g_Active || LineOfSight(a, b); }

HvvState GetHvvState()
{
    HvvState s;
    s.active = g_Active;
    if (!g_Active)
        return s;
    const ULONGLONG now = GetTickCount64();
    s.started = g_Woken;
    s.intro = g_Active && g_Phase == Phase::Intro;
    s.fighting = g_Phase == Phase::Fight;
    s.winner = g_Winner;
    s.player = g_Player;
    s.startLives = g_StartLives;
    s.heroes = g_Heroes;
    s.paused = GamePaused();
    s.playerDown = g_PlayerFell || (g_Player < g_Count && g_Fighters[g_Player].out);
    s.board = g_BoardPinned;
    s.fighters.resize(size_t(g_Count));
    for (int side = 0; side < 2; ++side)
        s.lives[side] = g_Lives[side];
    for (int i = 0; i < g_Count; ++i) {
        Fighter& f = g_Fighters[i];
        auto& o = s.fighters[i];
        o.className = f.className;
        o.name = ShownName(i);
        o.side = SideOf(i);
        o.isPlayer = i == g_Player;
        o.standing = g_Woken && Standing(f) && !f.out;
        o.out = f.out;
        o.placed = g_Woken && Placed(f);
        if (o.placed) {
            const float* p = Position(f.object);
            o.head[0] = p[0];
            o.head[1] = p[1] + MarkerHeight(f.className);
            o.head[2] = p[2];
            std::copy(p, p + 3, o.feet);
            o.health = std::clamp(Field<float>(f.object, kCharacterHealth) / kHealthScale, 0.0f, 1.0f);
            const float maxPower = Field<float>(f.object, kCharacterMaxPower);
            o.force = maxPower > 0 ? std::clamp(Field<float>(f.object, kCharacterPower) / maxPower, 0.0f, 1.0f) : -1.0f;
        }
        o.score = f.score;
        o.defeats = f.defeats;
        o.falls = f.falls;
        o.assists = f.assists;
        o.damage = int(f.damage);
        o.streak = f.streak;
        if (o.standing)
            ++s.standing[SideOf(i)];
    }
    for (const FeedEntry& e : g_Feed) {
        if (now - e.at > kFeedFor)
            continue;
        HvvState::FeedLine line;
        line.killer = e.killer >= 0 ? ShownName(e.killer) : "";
        line.killerSide = e.killer >= 0 ? SideOf(e.killer) : -1;
        line.victim = ShownName(e.victim);
        line.victimSide = SideOf(e.victim);
        line.mine = e.killer == g_Player || e.victim == g_Player;
        line.age = float(now - e.at) / 1000.0f;
        s.feed.push_back(line);
    }
    if (g_Notice.at && now - g_Notice.at < kNoticeFor) {
        s.notice = g_Notice.text;
        s.noticeSub = g_Notice.sub;
        s.noticePoints = g_Notice.points;
        s.noticeSide = g_Notice.side;
        s.noticeAge = float(now - g_Notice.at) / 1000.0f;
    }
    if (g_Phase == Phase::Intro) {
        const ULONGLONG t = now - g_IntroAt;
        if (t < kShotsLength) {
            const int side = int(t / kSideShots);
            s.caption = side == 0 ? "HEROES" : "VILLAINS";
            for (int i = SideStart(side); i < SideEnd(side); ++i)
                s.subcaption += (s.subcaption.empty() ? "" : "   ") + DisplayNameOf(g_Fighters[i].className);
            s.captionSide = side;
            s.captionFade = float(t % kSideShots) / kSideShots;
        }
    } else if (g_Phase == Phase::Fight && g_FightAt && now - g_FightAt < kFightBanner) {
        s.caption = "FIGHT!";
        s.captionFade = float(now - g_FightAt) / kFightBanner;
    }
    if (g_Phase == Phase::End) {
        s.ending = true;
        s.endingAge = float(now - g_EndAt) / 1000.0f;
        s.top = g_Top;
        for (size_t rank = 0; rank < g_Top.size() && rank < 5; ++rank) { // where the podium's characters stand
            auto& o = s.fighters[size_t(g_Top[rank])];
            o.placed = g_Podium[rank] != nullptr;
            if (g_Podium[rank])
                std::copy(Position(g_Podium[rank]), Position(g_Podium[rank]) + 3, o.feet);
        }
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
    for (const CustomCharacter& custom : CustomCharacters())
        out.push_back(custom.className);
    return out;
}

std::string HvvDisplayName(const std::string& className)
{
    return DisplayNameOf(className);
}

std::string ProfileSessionArguments()
{
    return SessionArguments();
}

bool EndHvvNow()
{
    if (!g_Active || g_Phase != Phase::Fight || g_Winner >= 0)
        return false;
    int score[2] = {};
    for (int i = 0; i < g_Count; ++i)
        score[SideOf(i)] += g_Fighters[i].score;
    g_Winner = g_Lives[0] != g_Lives[1] ? (g_Lives[0] > g_Lives[1] ? 0 : 1) : (score[0] >= score[1] ? 0 : 1);
    g_WinnerAt = GetTickCount64();
    LOG_INFO("HvV: ended from the console; the %s win", g_Winner == 0 ? "heroes" : "villains");
    return true;
}

void SetHvvBoardPinned(bool pinned)
{
    g_BoardPinned = pinned;
}

bool StartHvv(const std::vector<std::string>& lineup, int heroes, int player, int lives, int health, bool intro,
    const std::string& map, std::string& error)
{
    if (!map.empty() && !FindCustomMap(map)) {
        error = "no map " + map + " under mods/maps";
        return false;
    }
    if (lineup.size() < 2 || lineup.size() > size_t(kMaxFighters)) {
        error = "a match needs 2 to " + std::to_string(kMaxFighters) + " fighters";
        return false;
    }
    const int count = int(lineup.size());
    if (heroes <= 0)
        heroes = count / 2;
    if (heroes >= count || heroes > kMaxSide || count - heroes > kMaxSide) {
        error = "each side needs 1 to " + std::to_string(kMaxSide) + " fighters";
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
    if (player < 0 || player >= count)
        player = 0;
    StartDevMap("hvv:" + text + " hvvheroes:" + std::to_string(heroes) + " hvvplayer:" + std::to_string(player) +
        " hvvlives:" + std::to_string(lives > 0 ? lives : kDefaultLives) +
        " hvvhealth:" + std::to_string(health > 0 ? health : kDefaultHealth) + (intro ? "" : " hvvintro:0") +
        (map.empty() ? "" : " custommap:" + map) + SessionArguments());
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
        for (int i = 0; i < g_Count; ++i) {
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
            if (Field<float>(placed, kCharacterHealth) <= 0) {
                VirtualOf<void(__fastcall*)(uint8_t*, void*)>(placed, kActivate)(placed, nullptr);
                SettleActivated(placed);
            }
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
            EngageAll();
            LOG_INFO("HvV: the fight is on");
        }
        return;
    }
    if (g_Phase == Phase::Intro) {
        if (input::TakeHeldPress()) // skipped (A or Start), as the game's Versus intros
            g_IntroAt = now - IntroLength();
        const ULONGLONG t = now - g_IntroAt;
        if (t < kShotsLength) {
            const int shot = IntroShotAt(t);
            if (shot % 2 == 0 && shot != g_ShotPlayed) { // a line's first shot: its fighters play their intros
                g_ShotPlayed = shot;
                for (int i = SideStart(shot / 2); i < SideEnd(shot / 2); ++i)
                    if (Standing(g_Fighters[i]))
                        PlayIntro(g_Fighters[i].object);
            }
        }
        if (t < IntroLength())
            return;
        HoldEveryone(false);
        g_Phase = Phase::Fight;
        g_FightAt = now;
        g_LastEngage = now;
        EngageAll();
        LOG_INFO("HvV: the fight is on");
    }
    if (g_Phase == Phase::End) {
        if (g_PodiumIntros && now - g_EndAt >= 200) { // they stand: their intros
            g_PodiumIntros = false;
            for (uint8_t* c : g_Podium)
                if (c)
                    PlayIntro(c);
        }
        if (!g_MenuAsked && now - g_EndAt >= kMenuAfter) {
            g_MenuAsked = true;
            BackToMenu();
        }
        return;
    }
    CheckSlowMotion();
    if (g_Winner >= 0 && now - g_WinnerAt >= kEndAfter && !g_PlayerFell) {
        StartEnd(now);
        return;
    }

    // The player's fatal hit (kept from the game, whose player must never die: that ends the mission): a fresh
    // fighter of its class takes over the controls at once (the live player change, which removes the old body),
    // held, shielded, out of the AI's sight, where it will come back; where the old body fell, a double of it (an
    // ordinary character) is dealt the death by its killer and dies there (its death plays, as any fighter's). The
    // kill camera meanwhile; then the fighter comes back, a life of its side gone. (Keeping the player's own body to
    // die froze the game once: an object list of its went round in a circle, 0x1F3DB0.)
    Fighter& me = g_Fighters[g_Player];
    if (g_PlayerFell && !g_Swapped) {
        g_Swapped = true;
        g_Corpse = nullptr;
        float fell[16];
        std::memcpy(fell, player + kCharacterTransform, sizeof fell);
        // the old body out of sight first: its removal can take a moment, and it played its own fall beside the
        // double's
        float away[16];
        Transform(kDevMapMiddleX + 9000, kDevMapMiddleZ + 4500, 0, away);
        VirtualOf<void(__fastcall*)(uint8_t*, void*, const float*)>(player, kPlace)(player, nullptr, away);
        std::string error;
        if (ReplacePlayer(error)) {
            player = PlayerObject();
            Track(me, player);
            // it waits far from the fight (out of the kill camera's picture and the AI's way) until it comes back
            float m[16];
            Transform(kDevMapMiddleX - 9000, kDevMapMiddleZ + 4500, 0, m);
            VirtualOf<void(__fastcall*)(uint8_t*, void*, const float*)>(player, kPlace)(player, nullptr, m);
            FullHealth(player);
            Field<uint8_t>(player, kCharacterInvincible) = 1;
            SetIgnoredByAI(player, true);
            input::HoldPlayerInput(true);
            if (uint8_t* body = SpawnCharacterAt(me.className.c_str(), "", "", "", SpawnSide::Default, fell, error)) {
                g_Corpse = body;
                g_CorpseId = Field<uint32_t>(body, 4);
                uint8_t* by = g_PlayerKiller >= 0 && Placed(g_Fighters[g_PlayerKiller])
                    ? g_Fighters[g_PlayerKiller].object : nullptr;
                reinterpret_cast<void(__fastcall*)(uint8_t*, void*, float, uint32_t, uint32_t)>(uintptr_t(kChangeHealth))(
                    body, nullptr, -10 * kHealthScale, 0, uint32_t(reinterpret_cast<uintptr_t>(by)));
            } else {
                LOG_WARN("HvV: no body to fall (%s)", error.c_str());
            }
        } else {
            LOG_WARN("HvV: a fresh fighter could not take over (%s); you get up where you are", error.c_str());
        }
    }
    if (g_PlayerFell && now - g_PlayerDownAt >= kPlayerDown) {
        g_PlayerFell = false;
        Field<uint8_t>(player, kCharacterInvincible) = 0;
        if (g_Lives[SideOf(g_Player)] <= 0) { // out: held, out of the way, until the end
            me.out = true;
            float m[16];
            Transform(g_Bounds[0] + 100, g_Bounds[1] + 100, 0, m);
            VirtualOf<void(__fastcall*)(uint8_t*, void*, const float*)>(player, kPlace)(player, nullptr, m);
            Field<uint8_t>(player, kCharacterInvincible) = 1;
            LOG_INFO("HvV: you are out");
        } else {
            --g_Lives[SideOf(g_Player)];
            float m[16];
            SafePlace(g_Player, m);
            VirtualOf<void(__fastcall*)(uint8_t*, void*, const float*)>(player, kPlace)(player, nullptr, m);
            FullHealth(player);
            Shield(me, now);
            SetIgnoredByAI(player, false);
            input::HoldPlayerInput(false);
            g_BackAt = now;
            LOG_INFO("HvV: you came back (%d lives left)", g_Lives[SideOf(g_Player)]);
        }
    }

    for (int i = 0; i < g_Count; ++i) {
        Fighter& f = g_Fighters[i];
        if (f.shieldUntil && now >= f.shieldUntil) {
            f.shieldUntil = 0;
            if (Placed(f))
                Field<uint8_t>(f.object, kCharacterInvincible) = 0;
        }
        if (i == g_Player || f.out || g_Winner >= 0)
            continue;
        if (!f.fellAt) {
            if (!Standing(f)) {
                f.fellAt = now;
                Defeated(i, now);
            }
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
        Engage(i);
        LOG_INFO("HvV: %s came back (%d lives left)", f.className.c_str(), g_Lives[side]);
    }
    if (now - g_LastEngage >= kEngageEvery) {
        g_LastEngage = now;
        EngageAll();
        for (int i = 0; i < g_Count; ++i)
            Unstick(i, now);
    }
    CheckWinner(now);
}

} // namespace swrots::game
