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
#include "game/characters.h"
#include "game/coop.h"
#include "game/customchars.h"
#include "game/devmap.h"
#include "game/freecam.h"
#include "game/game.h"
#include "input/controls.h"

namespace swrots::game {

namespace {

constexpr int kSide = 4;            // fighters a side
constexpr int kFighters = 2 * kSide;
constexpr float kLineGap = 150.0f;  // between fighters in a line
constexpr float kLinesApart = 700.0f;
constexpr float kPi = 3.14159265f;
constexpr ULONGLONG kWakeAfter = 500;     // ms after the player is in the level: the placed fighters join
constexpr int kDefaultLives = 35;         // a side's
constexpr int kDefaultHealth = 250;       // every fighter's (a Jedi has 1000 of its own: long duels, not a battle)
// Every fighter keeps a health of kHealthScale (the HUD's bar is as long as the maximum: a low one shows a stub);
// the mode's health is made by scaling the damage taken (250: four times).
constexpr float kHealthScale = 1000.0f;
constexpr ULONGLONG kRespawnAfter = 4000; // ms after a fighter fell (its death plays out first)
constexpr ULONGLONG kShieldFor = 2000;    // ms a fighter that came back cannot be hurt
constexpr ULONGLONG kPlayerDown = 3000;   // ms the player's fall plays (shielded) before it comes back
constexpr ULONGLONG kEngageEvery = 1000;  // ms: fighters without an enemy to fight are given one
// Where a fighter comes back: on the floor, just outside the fight, facing it.
constexpr float kFloorX0 = 1500.0f, kFloorX1 = 24000.0f, kFloorZ0 = 2700.0f, kFloorZ1 = 13500.0f;
constexpr float kFloorMargin = 600.0f;
constexpr float kRespawnReach = 950.0f;  // from the fight's middle, at most
constexpr float kRespawnClear = 380.0f;  // from every fighter, at least (when it can)
constexpr int kRespawnTries = 32;
// The intro (ms): for each side a frontal shot of its line playing its Versus intros, then a close shot along the
// line; the camera back behind the player; "FIGHT!" shows a while after.
constexpr ULONGLONG kLineShot = 3000, kToPlayer = 1400, kFightBanner = 1500;
constexpr int kIntroShots = 4;
// Defeats, assists and points.
constexpr ULONGLONG kKillerWithin = 10000; // ms: the last enemy hit this recent made the defeat
constexpr ULONGLONG kAssistWithin = 15000; // ms: other enemies' hits this recent are assists
constexpr int kDefeatPoints = 100, kAssistPoints = 50, kStreakPoints = 25;
constexpr ULONGLONG kNoticeFor = 3500, kFeedFor = 7000;
constexpr size_t kFeedLines = 6;
// The end: the top five, lined up, after the winner is known for a moment.
constexpr ULONGLONG kEndAfter = 2500;

constexpr const char* kDefaultLineup[kFighters] = { "IObiwan", "IYoda", "IMace", "ISerra",
    "IVader", "IDooku", "IGrievous", "IAnakin" };

// The game's character fields and calls the match uses.
constexpr uint32_t kActivate = 0xB4;           // object vfunc, thiscall (): a placed character joins the level
constexpr uint32_t kPlace = 0x1F4;             // object vfunc, thiscall (const float m[16])
constexpr uint32_t kPlaySequence = 0x410;      // character vfunc, thiscall (hit zone or null, sequence)
constexpr uint32_t kObjectById = 0x000A31F0;   // cdecl (instance id) -> object or null
constexpr uint32_t kCharacterHealth = 0x130;
constexpr uint32_t kCharacterMaxHealth = 0x134;
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
};

bool g_Active = false;
Fighter g_Fighters[kFighters];
int g_Player = 0;
int g_StartLives = kDefaultLives;
int g_Health = kDefaultHealth;
int g_Lives[2] = {};
int g_Winner = -1;
ULONGLONG g_InLevelAt = 0, g_WinnerAt = 0, g_LastEngage = 0;
bool g_Woken = false;
bool g_PlayerFell = false; // a fatal hit on the player, kept alive: its fall plays, then it comes back
ULONGLONG g_PlayerDownAt = 0;
bool g_IntroWanted = true;
enum class Phase { Waiting, Intro, Fight, End } g_Phase = Phase::Waiting;
ULONGLONG g_IntroAt = 0, g_FightAt = 0, g_EndAt = 0;
int g_ShotPlayed = -1;      // the last intro shot whose line played its intros
float g_LastShot[16] = {};  // the intro camera's last placement, for the way back to the game's
bool g_BoardPinned = false;
std::vector<int> g_Top;     // the end's top five

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

std::string ShownName(int i)
{
    return i == g_Player ? "YOU" : DisplayNameOf(g_Fighters[i].className);
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
    for (int i = 0; i < kFighters; ++i)
        if (object && g_Fighters[i].object == object)
            return i;
    return -1;
}

// The fighter behind a hit's attacker object (see kWeaponsFrom), or -1.
int AttackerOf(uint8_t* attacker)
{
    if (!attacker)
        return -1;
    for (int i = 0; i < kFighters; ++i) {
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
            for (int i = 0; i < kFighters; ++i)
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
    float bestX = cx, bestZ = cz, bestScore = -1e9f;
    for (int t = 0; t < kRespawnTries; ++t) {
        const float angle = float(std::rand()) / RAND_MAX * 2 * kPi;
        const float reach = kRespawnClear + float(std::rand()) / RAND_MAX * (kRespawnReach - kRespawnClear);
        const float x = std::clamp(cx + std::sin(angle) * reach, kFloorX0 + kFloorMargin, kFloorX1 - kFloorMargin);
        const float z = std::clamp(cz + std::cos(angle) * reach, kFloorZ0 + kFloorMargin, kFloorZ1 - kFloorMargin);
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
    for (int k = 0; k < kFighters; ++k) {
        if (SideOf(k) == SideOf(i) || !Standing(g_Fighters[k]))
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

void EngageAll()
{
    for (int i = 0; i < kFighters; ++i)
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
        NoticeFor(killer >= 0 ? "DEFEATED BY " + DisplayNameOf(g_Fighters[killer].className) : "YOU FELL", "", 0,
            killer >= 0 ? SideOf(killer) : SideOf(victim), now);
    g_Feed.push_front({ killer, victim, now });
    while (g_Feed.size() > kFeedLines)
        g_Feed.pop_back();
    LOG_INFO("HvV: %s %s%s", ShownName(victim).c_str(), killer >= 0 ? "defeated by " : "fell",
        killer >= 0 ? ShownName(killer).c_str() : "");
}

// Every hit on a fighter: the enemy that dealt it scores its damage and is remembered for the defeat.
void Damaged(uint8_t* character, float change, uint32_t, uint8_t* attacker)
{
    if (!g_Active || g_Phase != Phase::Fight || change >= 0)
        return;
    const int victim = FighterOf(character);
    const int by = AttackerOf(attacker);
    if (victim < 0 || by < 0 || SideOf(by) == SideOf(victim))
        return;
    Fighter& v = g_Fighters[victim];
    Fighter& k = g_Fighters[by];
    k.damage += -change * float(g_Health) / kHealthScale; // in the mode's health
    k.score = int(k.damage) + k.defeats * kDefeatPoints + k.assists * kAssistPoints; // damage counts as it comes
    v.hits.push_back({ by, GetTickCount64() });
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
        Field<uint8_t>(character, kCharacterInvincible) = 1; // no more hits while it is down
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
        for (int i = side * kSide; i < (side + 1) * kSide; ++i)
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
            VirtualOf<bool(__fastcall*)(uint8_t*, void*, void*, uint8_t*)>(character, kPlaySequence)(
                character, nullptr, nullptr, items[i]);
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

ULONGLONG IntroLength() { return kIntroShots * kLineShot + kToPlayer; }

// The intro's camera `t` ms in; false past its shots (the way back blends towards the game's own). Shots 0 and 1:
// the heroes' line from the front, then along it; 2 and 3 the villains'.
bool IntroShot(ULONGLONG t, float m[16])
{
    if (t >= kIntroShots * kLineShot)
        return false;
    const int shot = int(t / kLineShot), side = shot / 2;
    const float a = Smooth(float(t % kLineShot) / kLineShot);
    const float floorY = kDevMapFloorY;
    const float lineZ = kDevMapMiddleZ + (side == 0 ? -kLinesApart / 2 : kLinesApart / 2);
    const float facing = side == 0 ? 1.0f : -1.0f; // the line faces +z (heroes) or -z
    const float x = kDevMapMiddleX;
    if (shot % 2 == 0) { // from the front, a little low, drifting across and in
        const float eye[3] = { x - 60 + 120 * a, floorY + 80 - 10 * a, lineZ + facing * (440 - 60 * a) };
        const float target[3] = { x, floorY + 55, lineZ };
        LookAt(eye, target, m);
    } else { // close, from one end of the line, along it at chest height
        const float end = (side == 0 ? 1.0f : -1.0f) * (1.5f * kLineGap + 110);
        const float eye[3] = { x + end - (side == 0 ? 35 : -35) * a, floorY + 72, lineZ + facing * (85 + 15 * a) };
        const float target[3] = { x - end * 0.5f, floorY + 58, lineZ - facing * 10 };
        LookAt(eye, target, m);
    }
    return true;
}

// The end: the top five lined up facing the camera, first in the middle (as Battlefront's end screen).
constexpr float kPodium[5][2] = { { 0, 0 }, { -85, 35 }, { 85, 35 }, { -170, 70 }, { 170, 70 } }; // x, back
constexpr float kPodiumZ = 0; // relative to the floor's middle

void EndCamera(float m[16])
{
    const float a = Smooth(float(GetTickCount64() - g_EndAt) / 4000.0f);
    const float eye[3] = { kDevMapMiddleX, kDevMapFloorY + 75 - 10 * a, kDevMapMiddleZ + kPodiumZ - 330 + 40 * a };
    const float target[3] = { kDevMapMiddleX, kDevMapFloorY + 55, kDevMapMiddleZ + kPodiumZ + 30 };
    LookAt(eye, target, m);
}

// The camera during the intro, on the way back to the game's, and at the end.
bool HvvCamera(float m[16], float)
{
    if (!g_Active)
        return false;
    if (g_Phase == Phase::End) {
        EndCamera(m);
        return true;
    }
    if (g_Phase != Phase::Intro)
        return false;
    const ULONGLONG t = GetTickCount64() - g_IntroAt;
    if (IntroShot(t, m)) {
        std::copy(m, m + 16, g_LastShot);
        return true;
    }
    const ULONGLONG back = kIntroShots * kLineShot;
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

std::vector<int> Ranking()
{
    std::vector<int> order(kFighters);
    for (int i = 0; i < kFighters; ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(),
        [](int a, int b) { return g_Fighters[a].score > g_Fighters[b].score; });
    return order;
}

// The end begins: everyone holds still, the top five come to the podium (one that is down comes back there), the
// others go far away, and the five play their intros.
void StartEnd(ULONGLONG now)
{
    g_Phase = Phase::End;
    g_EndAt = now;
    HoldEveryone(true);
    std::vector<int> order = Ranking();
    g_Top.assign(order.begin(), order.begin() + 5);
    for (int i = 0; i < kFighters; ++i) {
        Fighter& f = g_Fighters[i];
        const auto place = std::find(g_Top.begin(), g_Top.end(), i);
        float m[16];
        if (place == g_Top.end()) {
            if (Placed(f)) {
                Transform(kDevMapMiddleX + 4000 + 300 * i, kDevMapMiddleZ - 4000, 0, m);
                VirtualOf<void(__fastcall*)(uint8_t*, void*, const float*)>(f.object, kPlace)(f.object, nullptr, m);
            }
            continue;
        }
        const int rank = int(place - g_Top.begin());
        Transform(kDevMapMiddleX + kPodium[rank][0], kDevMapMiddleZ + kPodiumZ + kPodium[rank][1], kPi, m);
        if (i == g_Player && (f.out || !Standing(f))) { // a fresh fighter takes over from the one lying down
            std::string error;
            if (ReplacePlayer(error))
                Track(f, PlayerObject());
            if (f.object)
                VirtualOf<void(__fastcall*)(uint8_t*, void*, const float*)>(f.object, kPlace)(f.object, nullptr, m);
        } else if (!Standing(f)) {
            std::string error;
            if (uint8_t* back = SpawnCharacterAt(f.className.c_str(), "", "", "", SpawnSide::Neutral, m, error))
                Track(f, back);
        } else if (f.object) {
            VirtualOf<void(__fastcall*)(uint8_t*, void*, const float*)>(f.object, kPlace)(f.object, nullptr, m);
        }
        if (f.object && Standing(f)) {
            FullHealth(f.object);
            Field<uint8_t>(f.object, kCharacterInvincible) = 1;
            PlayIntro(f.object);
        }
    }
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
    SetDamageObserver(&Damaged);
    SetDamageFilter(&ScaleDamage);
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
    const ULONGLONG now = GetTickCount64();
    s.started = g_Woken;
    s.fighting = g_Phase == Phase::Fight;
    s.winner = g_Winner;
    s.player = g_Player;
    s.startLives = g_StartLives;
    s.board = g_BoardPinned;
    for (int side = 0; side < 2; ++side)
        s.lives[side] = g_Lives[side];
    for (int i = 0; i < kFighters; ++i) {
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
            o.head[1] = p[1] + 95.0f;
            o.head[2] = p[2];
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
        if (t < kIntroShots * kLineShot) {
            const int side = int(t / kLineShot) / 2;
            s.caption = side == 0 ? "HEROES" : "VILLAINS";
            for (int i = side * kSide; i < (side + 1) * kSide; ++i)
                s.subcaption += (s.subcaption.empty() ? "" : "   ") + DisplayNameOf(g_Fighters[i].className);
            s.captionSide = side;
            s.captionFade = float(t % (2 * kLineShot)) / (2 * kLineShot);
        }
    } else if (g_Phase == Phase::Fight && g_FightAt && now - g_FightAt < kFightBanner) {
        s.caption = "FIGHT!";
        s.captionFade = float(now - g_FightAt) / kFightBanner;
    }
    if (g_Phase == Phase::End) {
        s.ending = true;
        s.endingAge = float(now - g_EndAt) / 1000.0f;
        s.top = g_Top;
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

void SetHvvBoardPinned(bool pinned)
{
    g_BoardPinned = pinned;
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
                VirtualOf<void(__fastcall*)(uint8_t*, void*)>(placed, kActivate)(placed, nullptr);
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
        const ULONGLONG t = now - g_IntroAt;
        if (t < kIntroShots * kLineShot) {
            const int shot = int(t / kLineShot);
            if (shot % 2 == 0 && shot != g_ShotPlayed) { // a line's first shot: its fighters play their intros
                g_ShotPlayed = shot;
                for (int i = (shot / 2) * kSide; i < (shot / 2 + 1) * kSide; ++i)
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
    if (g_Phase == Phase::End)
        return;
    if (g_Winner >= 0 && now - g_WinnerAt >= kEndAfter && !g_PlayerFell) {
        StartEnd(now);
        return;
    }

    // The player's fatal hit: its fall plays out (a killing blow is often a move both play, the victim's ending on
    // the ground), then a fresh fighter of its class takes over the controls (the live player change), at a safe
    // place, with full health and a moment's shield; a life of its side goes.
    Fighter& me = g_Fighters[g_Player];
    if (g_PlayerFell && now - g_PlayerDownAt >= kPlayerDown && g_Lives[SideOf(g_Player)] <= 0) {
        g_PlayerFell = false; // out: it stays down (invincible) until the end
        me.out = true;
        LOG_INFO("HvV: you are out");
    } else if (g_PlayerFell && now - g_PlayerDownAt >= kPlayerDown) {
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
        VirtualOf<void(__fastcall*)(uint8_t*, void*, const float*)>(player, kPlace)(player, nullptr, m);
        FullHealth(player);
        Shield(me, now);
        LOG_INFO("HvV: you came back (%d lives left)", g_Lives[SideOf(g_Player)]);
    }

    for (int i = 0; i < kFighters; ++i) {
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
    }
    CheckWinner(now);
}

} // namespace swrots::game
