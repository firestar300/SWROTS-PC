// Commander Cody's fighting kit (see cody.h).
//
// A clone under player control has one rule, the rifle butt on fast attack (the clone's rules 0x35AE70, player part
// 0x35B2A4); its shooting is its AI's (CloneTrooper.cpp: a layer over what it does, a bolt at its target). The cut
// playable Cody (ICommanderCody, whose script survives, rules 0x360D00) had his own: a shot on fast attack and a burst
// on heavy attack for 250 of his meter. Here his meter (Force +0xA40, which the clone has; refilled here) pays for all
// he does beyond the rifle butt, for the player's Cody and the computer's alike:
//   - heavy attack (Y) held: the shooting layer and a bolt every kShotIntervalMs (in the clone's
//     script's turn: its StateManager, called for every clone every frame, the input functions reading the script's
//     character's own controller);
//   - block (LT): a roll aside (left or right with the stick), the clone's roll out of cover (CTroop_Nav_RollOut_L/R)
//     as sequences of the port's (optional in the class, so not loaded in a duel: the port's are required on duel maps,
//     as the duelists' intros are), instead of a block, which a clone has not;
//   - push (RT): two clones of his rappelling down beside him (the clone's own CTroop_Rappel_Loop: rope, slide, landing)
//     for kHelperLifeMs, one squad at a time; he signals them in and calls for them ("Bring up the reinforcements!");
//   - critical (B): a bombardment: shells (the game's IExplosion) falling round his opponent, hurting none of his;
//     he signals the strike ("Concentrate fire on the target!").
// What the player's input asks for is made outside the scripts (CodyFrame).

#include "game/cody.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

#include "core/log.h"
#include "input/controls.h"
#include "game/characters.h"
#include "game/classsetup.h"
#include "game/customchars.h"
#include "game/devmap.h"
#include "game/freecam.h"
#include "game/hvv.h"
#include "game/meshedit.h"
#include "game/resources.h"

namespace swrots::game {

namespace {

constexpr uint32_t kIsComputer = 0x00176E30;             // cdecl bool (): the AI drives the script's character
constexpr uint32_t kScriptCharacterContext = 0x006943CC; // +0x68: the character whose script runs, +0x70 its state
constexpr uint32_t kHeldFrames = 0x00177FF0;             // cdecl int (button, 0), in a script's turn
constexpr uint32_t kPressed = 0x00178070;                // cdecl bool (button, 1, 0, 2)
constexpr uint32_t kButtonHeavy = 0x006964DC, kButtonCritical = 0x006964EC; // the script's button ids (Y, B)
constexpr uint32_t kButtonPush = 0x006964E8;     // (RT: F)
constexpr uint32_t kButtonDodge = 0x00696500;    // White (L1: Q; the Jedi's saber throw): LT is the lock-on's
constexpr uint32_t kSayEvent = 0x0017ACC0;               // cdecl (event, context, 1, 0, 0): the script character says
constexpr uint32_t kScriptHasTarget = 0x00176FF0;   // cdecl bool ()
constexpr uint32_t kScriptPlayLayer = 0x00173B40;   // cdecl (animation, ...): an animation layer
constexpr uint32_t kScriptStopLayer = 0x00173B80;   // cdecl (layer, blend)
constexpr uint32_t kScriptShotSpread = 0x00176AB0;  // cdecl bool (): no spread
constexpr uint32_t kCloneFireBolt = 0x003C0090;     // cdecl (instance, script, 0, spread, muzzle data, muzzle)
constexpr uint32_t kCloneNextMuzzle = 0x001760B0;   // cdecl (muzzle*, count)
constexpr uint32_t kScriptPlaySound = 0x0017AD20;   // cdecl (sound, near, 1, far, -1, 0)
constexpr uint32_t kCloneMuzzle = 0x3D8, kCloneMuzzleData = 0x3EC, kCloneFireSound = 0x3E4; // in the instance
constexpr char kCloneShootingLoop[] = "ctroop_atk_shooting_loop";
constexpr uint32_t kCharacterHealth = 0x130, kCharacterHealthMost = 0x134, kCharacterMeter = 0xA40, kCharacterMeterMost = 0xA44;
constexpr uint32_t kCharacterTransform = 0x150, kCharacterOpponent = 0x3B8, kCharacterScript = 0x434;
constexpr uint32_t kCharacterAI = 0x9FC;
constexpr uint32_t kAISetTarget = 0x00193D90;  // thiscall on the AI controller (target)
constexpr uint32_t kObjectById = 0x000A31F0;   // cdecl (instance id) -> object or null
constexpr uintptr_t kPlaySequence = 0x150E30;  // ICharacter, thiscall (sequence, 0, 0, 0, speed)
constexpr uintptr_t kFindSequence = 0x150D60;  // ICharacter, thiscall (name) -> its sequence record or null

// his meter
constexpr float kMeterPerSecond = 35;
constexpr float kSummonCost = 500, kBombardCost = 400;
// his dodge costs nothing (a Jedi's block costs no Force either), one a kDodgeWaitMs at most, and no hit lands on him
// while he rolls (kRollGuardMs: a roll in time beats a combo, as a block does)
constexpr ULONGLONG kDodgeWaitMs = 700, kRollGuardMs = 650;
constexpr ULONGLONG kShotIntervalMs = 400;
constexpr ULONGLONG kHelperLifeMs = 25000;
constexpr ULONGLONG kComputerDodgeWaitMs = 1200; // the computer's Cody: one look for a dodge in this long
// a duel gives its fighters 1000 health (its bar a fixed scale): a blaster against a saber takes hits a third harder
constexpr float kDamageTaken = 1.33f;
constexpr uint32_t kCharacterInvincible = 0x5D6; // byte, "Invincible?"
constexpr float kHelperSide = 90;            // how far beside him they come down
constexpr float kRappelHeight = 320;         // and from how high (indoor arenas have ceilings)
constexpr ULONGLONG kHelperEngageMs = 2500;  // they turn on his opponent once down
constexpr float kComputerCallsWithin = 900;  // the computer's Cody calls his specials with his opponent this near
constexpr ULONGLONG kArmAfterMs = 7000;      // his specials only once the duel's intro is over (he appears in it)
// the bombardment
constexpr int kShells = 4;
constexpr ULONGLONG kFirstShellMs = 900, kShellGapMs = 350;
constexpr float kShellSpread = 110;

template <typename T> T& At(uint8_t* p, uint32_t offset)
{
    return *reinterpret_cast<T*>(p + offset);
}

bool Live(uint8_t* object, uint32_t id)
{
    return object && reinterpret_cast<uint8_t*(__cdecl*)(uint32_t)>(uintptr_t(kObjectById))(id) == object;
}

const float* Position(uint8_t* character)
{
    return reinterpret_cast<const float*>(character + kCharacterTransform + 48);
}

bool IsCody(const uint8_t* character)
{
    const CustomCharacter* custom = character ? CustomCharacterOf(character) : nullptr;
    return custom && custom->builtIn && custom->className == "ICody";
}

struct Kit {
    uint32_t id = 0;
    bool shooting = false;
    bool lastBlock = false;
    ULONGLONG armedAt = 0; // his specials from then on (not in the duel's intro)
    // a player's Cody: he had a controller (+0x43C, -1 for none) once since he came. The game's own test (0x1545A0)
    // also counts a player's character as the computer's while its AI drives it (the duel's intro), so it is not asked
    bool player = false;
    ULONGLONG lastShot = 0, lastFrame = 0, lastDodge = 0, rollUntil = 0;
    float rollDir[2] = {};         // the roll asked for: where the stick pointed (world x, z), 0 for none
    bool guarded = false;          // made invincible for a roll (its own byte kept to give back)
    uint8_t ownInvincible = 0;
    uint8_t* foe = nullptr;        // his opponent of the round
    uint32_t foeId = 0;
    bool summon = false;  // asked for, made in CodyFrame
    bool bombard = false; // likewise
    int roll = 0;         // likewise: 1 left, 2 right, 3 the stick's side
};
std::unordered_map<uint8_t*, Kit> g_Kits;

struct Helper {
    uint8_t* object;
    uint32_t id;
    uint8_t* master;
    bool enemy;          // against the player (its master the computer's)
    ULONGLONG engageAt;  // when it turns on its master's opponent (once down)
    ULONGLONG until;
};
std::vector<Helper> g_Helpers;

struct Shell {
    uint8_t* cody;
    uint32_t codyId;
    uint8_t* target;
    ULONGLONG at;
};
std::vector<Shell> g_Shells;

uint32_t Button(uint32_t global)
{
    return *reinterpret_cast<const uint32_t*>(uintptr_t(global));
}

bool Pressed(uint32_t global)
{
    return reinterpret_cast<bool(__cdecl*)(uint32_t, int, int, int)>(uintptr_t(kPressed))(Button(global), 1, 0, 2);
}

bool Held(uint32_t global)
{
    return reinterpret_cast<int(__cdecl*)(uint32_t, int)>(uintptr_t(kHeldFrames))(Button(global), 0) > 0;
}

bool Direction(uint32_t test, uint8_t* instance) // 0x1F5480 left, 0x1F54C0 right on [instance + 0xFC]
{
    return reinterpret_cast<bool(__cdecl*)(uint32_t)>(uintptr_t(test))(At<uint32_t>(instance, 0xFC));
}

// A line of his, in his script's turn: the event asked as the game asks it, the line forced (customchars.h).
void Say(uint8_t* character, const char* event, const char* line)
{
    ForceNextLine(character, event, line);
    reinterpret_cast<int(__cdecl*)(const char*, const char*, int, int, int)>(uintptr_t(kSayEvent))(event, "", 1, 0, 0);
}

void Fire(uint8_t* script, uint8_t* instance)
{
    if (!reinterpret_cast<bool(__cdecl*)()>(uintptr_t(kScriptHasTarget))())
        return;
    const float spread = reinterpret_cast<bool(__cdecl*)()>(uintptr_t(kScriptShotSpread))() ? 0.0f : 10.0f;
    uint32_t spreadBits;
    std::memcpy(&spreadBits, &spread, 4);
    reinterpret_cast<void(__cdecl*)(uint8_t*, uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t)>(uintptr_t(kCloneFireBolt))(
        instance, script, 0, spreadBits, At<uint32_t>(instance, kCloneMuzzleData), At<uint32_t>(instance, kCloneMuzzle));
    reinterpret_cast<void(__cdecl*)(uint8_t*, int)>(uintptr_t(kCloneNextMuzzle))(instance + kCloneMuzzle, 4);
    reinterpret_cast<void(__cdecl*)(uint32_t, float, int, float, int, int)>(uintptr_t(kScriptPlaySound))(
        At<uint32_t>(instance, kCloneFireSound), 100.0f, 1, 800.0f, -1, 0);
}

void Layer(Kit& kit, bool on)
{
    if (on == kit.shooting)
        return;
    if (on)
        reinterpret_cast<void(__cdecl*)(const char*, int, int, float, int, int, int, int, int)>(uintptr_t(kScriptPlayLayer))(
            kCloneShootingLoop, 0, 0, 1.0f, 5, 2, 0, 1, 2);
    else
        reinterpret_cast<void(__cdecl*)(int, int)>(uintptr_t(kScriptStopLayer))(2, 5);
    kit.shooting = on;
}

bool IsHelper(const uint8_t* c)
{
    return std::any_of(g_Helpers.begin(), g_Helpers.end(), [&](const Helper& h) { return h.object == c; });
}

// The opponent a character fights: its own (+0x3B8) when it is a standing character, else the nearest other one
// standing that is not one of the helpers.
uint8_t* OpponentOf(uint8_t* character)
{
    const auto standing = [](uint8_t* c) { return c && At<float>(c, kCharacterHealth) > 0; };
    const std::vector<uint8_t*> characters = LevelCharacters();
    uint8_t* own = At<uint8_t*>(character, kCharacterOpponent);
    if (own && own != character && standing(own) && !IsHelper(own) &&
        std::find(characters.begin(), characters.end(), own) != characters.end())
        return own;
    const float* p = Position(character);
    uint8_t* best = nullptr;
    float bestDistance = 1e30f;
    for (uint8_t* c : characters) {
        if (c == character || !standing(c) || IsHelper(c))
            continue;
        if (HvvActive() && HvvSameSide(c, character))
            continue;
        const float* q = Position(c);
        const float d = std::hypot(q[0] - p[0], q[2] - p[2]);
        if (d < bestDistance) {
            bestDistance = d;
            best = c;
        }
    }
    return best;
}

// Turns a character to face another where it stands (its placement, vfunc +0x1F4: rows right, up, forward, position).
void FaceToward(uint8_t* character, uint8_t* other)
{
    if (!other)
        return;
    const float* p = Position(character);
    const float* q = Position(other);
    float f[3] = { q[0] - p[0], 0, q[2] - p[2] };
    const float length = std::hypot(f[0], f[2]);
    if (length < 1)
        return;
    f[0] /= length;
    f[2] /= length;
    const float m[16] = { f[2], 0, -f[0], 0, 0, 1, 0, 0, f[0], 0, f[2], 0, p[0], p[1], p[2], 1 };
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*, const float*)>(
        (*reinterpret_cast<void* const* const*>(character))[0x1F4 / 4])(character, nullptr, m);
}

// Plays one of a character's sequences by name, as the duel plays an intro; false without it.
bool PlaySequence(uint8_t* character, const char* name)
{
    uint8_t* record = reinterpret_cast<uint8_t*(__fastcall*)(uint8_t*, void*, const char*)>(kFindSequence)(character, nullptr, name);
    return record && reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint8_t*, float, int, int, float)>(kPlaySequence)(
        character, nullptr, record, 0.0f, 0, 0, 1.0f);
}

// --- His moves: sequences of the port's on the clone class (only in matches) ---
struct OwnMove {
    const char* name;
    const char* animation;
    float speed;
    uint16_t handle;
};
OwnMove g_Moves[] = {
    { "Cody_RollLeft", "CTroop_Nav_RollOut_L", 1.7f, 0 }, // the roll out of cover, quicker: a dodge
    { "Cody_RollRight", "CTroop_Nav_RollOut_R", 1.7f, 0 },
    { "Cody_SignalLeft", "ClnCom_Signal_Left", 1.0f, 0 }, // calling his clones in
};
constexpr int kMoveCount = int(std::size(g_Moves));
constexpr uint32_t kSetup = 0x006965D8;
const char kDuelMapTag[] = "DuelMap";

// As the clone's own roll (CTroop_Nav_RollOut_L, 0x3575B0): optional (and its flag 0x696537); at its first frame
// 0x38A140 (instance, class object, -1, -1), which hands the character back to its state machine when the move ends
// (without it the move played on and on).
template <int K> bool __fastcall MoveFn(uint8_t* self, void*, uint32_t, uint32_t)
{
    auto* setup = reinterpret_cast<uint8_t*>(uintptr_t(kSetup));
    OwnMove& move = g_Moves[K];
    const bool declared = reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint16_t*, const char*, const char*, float, uint32_t)>(
        uintptr_t(0x001F5B20))(setup, nullptr, &move.handle, move.name, move.animation, move.speed, 0x0055FB60);
    if (declared) { // optional, required on duel maps (as Vader_Intro)
        *reinterpret_cast<uint8_t*>(uintptr_t(0x00696531)) = 1;
        *reinterpret_cast<uint8_t*>(uintptr_t(0x00696537)) = 1;
        *reinterpret_cast<const char**>(uintptr_t(0x006964B8)) = kDuelMapTag;
    }
    if (reinterpret_cast<bool(__fastcall*)(uint8_t*, void*)>(uintptr_t(0x001F62D0))(setup, nullptr)) { // first frame
        uint8_t* instance = *reinterpret_cast<uint8_t**>(uintptr_t(0x006964C0));
        reinterpret_cast<void(__cdecl*)(uint8_t*, uint8_t*, int, int)>(uintptr_t(0x0038A140))(instance, self, -1, -1);
    }
    return reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, const char*, uint32_t)>(uintptr_t(0x001F6C00))(
        setup, nullptr, reinterpret_cast<const char*>(uintptr_t(0x0059C07C)), 0x7D00 + K);
}

int __cdecl MoveSequences(const char* shortName, ClassSequenceFn* fns, int max)
{
    if (!BootIsMatch() || _stricmp(shortName, "CloneTrooper") != 0 || max < kMoveCount || !FindCustomCharacter("ICody"))
        return 0;
    fns[0] = &MoveFn<0>;
    fns[1] = &MoveFn<1>;
    fns[2] = &MoveFn<2>;
    LOG_INFO("Cody: his rolls and signal added to the clone trooper's moves");
    return kMoveCount;
}

// --- His clones rappel down ---
// The clone's own CTroop_Rappel_Loop (handle +0xFF6): at its first frame a rope from where it starts to its left hand
// (the rope manager, a singleton [0x7F2EAC], draws it: without one, or one not active ([+0xC] & 0x40000), the rope's
// add faults), no gravity, a steady slide down; on the floor its Rappel_Out lets go and gives it back to its AI.
bool RopesReady()
{
    const uint8_t* ropes = *reinterpret_cast<uint8_t* const*>(uintptr_t(0x007F2EAC));
    return ropes && (*reinterpret_cast<const uint32_t*>(ropes + 0xC) & 0x40000) != 0;
}

void CallSquad(uint8_t* cody, bool enemy, ULONGLONG now)
{

    const bool rappel = RopesReady();
    int made = 0;
    for (int side = -1; side <= 1; side += 2) {
        float m[16];
        std::memcpy(m, cody + kCharacterTransform, sizeof m);
        for (int k = 0; k < 3; ++k) // beside him, a step back
            m[12 + k] += m[k] * kHelperSide * float(side) - m[8 + k] * 40.0f;
        if (rappel)
            m[13] += kRappelHeight;
        std::string error;
        uint8_t* clone = SpawnHelper("ICloneTrooper", std::string(), cody, enemy, m, error);
        if (!clone) {
            LOG_WARN("Cody: a clone of his could not come (%s)", error.c_str());
            continue;
        }
        const bool roped = rappel && PlaySequence(clone, "CTroop_Rappel_Loop");
        if (rappel && !roped)
            LOG_WARN("Cody: his clone has no rappel here; it drops in");
        g_Helpers.push_back({ clone, At<uint32_t>(clone, 4), cody, enemy, now + (roped ? kHelperEngageMs : 0), now + kHelperLifeMs });
        ++made;
    }
    LOG_INFO("Cody: %d of his clones called in%s", made, rappel ? ", rappelling" : "");

}

// --- The bombardment ---
// A shell is the game's IExplosion (class info 0x68E760, as a breakable or a rocket makes one): its properties at
// +0x1E0 (the effect key +0x1F0, the hit callback +0x200, the sound link +0x204, radii, lifetime, explode once), spawned
// (0xA2DE0) where it falls and set off (vfunc +0xA4): the effect, the sound, a damage sphere growing over its lifetime,
// which hands each object it reaches to the callback. The callback hurts his enemies only, modestly, through the
// game's own hit (0xA6720: a knock-down reaction from r_environment's EXPLOSION_* rows, the health change after it).
// The effects are each level's own: the first of these the level has; without one, the shell is heard, not seen.
constexpr uint32_t kExplosionClass = 0x0068E760;
constexpr const char* kShellEffects[] = { "IDS_FX_explosion_med", "IDS_FX_explosion_large06", "IDS_FX_explosion_barrel_small",
    "IDS_FX_explosion_large05", "IDS_FX_flak_explosion01", "IDS_FX_explosion_large", "IDS_FX_Homing_Rocket_Explosion",
    "IDS_FX_electric_explosion_small", "IDS_FX_flak_explosion03", "IDS_FX_flak_explosion02", "IDS_FX_dust_burst_explosion09" };
std::unordered_map<uint8_t*, uint8_t*> g_ShellOwners; // shell -> his Cody

bool IsCharacter(uint8_t* object)
{
    return reinterpret_cast<bool(__cdecl*)(uint8_t*)>(uintptr_t(0x00089D10))(object);
}

void __cdecl ShellHit(uint8_t* shell, uint8_t* target, void*)
{
    __try {
        const auto owner = g_ShellOwners.find(shell);
        uint8_t* cody = owner == g_ShellOwners.end() ? nullptr : owner->second;
        if (!target || !IsCharacter(target) || target == cody || IsHelper(target))
            return;
        if (cody && HvvActive() && HvvSameSide(target, cody))
            return;
        const float* centre = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*(__fastcall*)(uint8_t*, void*)>(
            uintptr_t(0x000F2280))(shell, nullptr) + 0x30);
        const float* p = Position(target);
        const float d = std::hypot(p[0] - centre[0], p[2] - centre[2]);
        float damage = d < 60 ? 60.0f : d < 110 ? 40.0f : 20.0f;
        const auto isA = reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint32_t)>(
            (*reinterpret_cast<void* const* const*>(target))[1]);
        if (isA(target, nullptr, 0x00249950) && At<uint8_t>(target, 0x1060)) // a Jedi blocking (as the canister)
            damage /= 2;
        const float from[4] = { centre[0], centre[1] + 720.0f, centre[2], 1.0f };
        reinterpret_cast<int(__cdecl*)(uint8_t*, uint8_t*, const char*, const float*, int, float, bool)>(uintptr_t(0x000A6720))(
            target, shell, d < 60 ? "EXPLOSION_MEDIUM" : "EXPLOSION_LIGHT", from, 0, damage, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The two kinds of text in the properties, as the canister sets them (0x24F1C0): the effect key (+0x10) is an interned
// name (made by 0x222FA0 cdecl (out, text) into a holder of the game's that keeps it, and copied in as it is); the sound
// link (+0x24) a text the constructor made, set with its assign (0x225A10).
void AssignText(uint8_t* field, const char* text)
{
    reinterpret_cast<void(__fastcall*)(void*, void*, const char*)>(uintptr_t(0x00225A10))(field, nullptr, text);
}

bool SetEffect(uint8_t* field)
{
    for (const char* key : kShellEffects) { // the effect manager's check (0x1B3B00): one this level has
        void* name = nullptr; // held, never released: the interned name lives on, as the canister's holder's does
        reinterpret_cast<void*(__cdecl*)(void*, const char*)>(uintptr_t(0x00222FA0))(&name, key);
        if (name && reinterpret_cast<bool(__cdecl*)(void*)>(uintptr_t(0x001B3B00))(&name)) {
            *reinterpret_cast<void**>(field) = name;
            return true;
        }
    }
    return false;
}

void FireShell(uint8_t* cody, uint8_t* target)
{
    const float* p = Position(target);
    const ULONGLONG r = GetTickCount64();
    const float angle = float(r % 628) / 100.0f, distance = float((r / 7) % 100) / 100.0f * kShellSpread;
    float m[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, p[0] + std::cos(angle) * distance, p[1], p[2] + std::sin(angle) * distance, 1 };
    uint8_t* shell = reinterpret_cast<uint8_t*(__cdecl*)(uint32_t)>(uintptr_t(0x000A2DB0))(kExplosionClass);
    if (!shell)
        return;
    uint8_t* props = shell + 0x1E0; // as its constructor left them (its texts made), the port's values set
    const bool seen = SetEffect(props + 0x10);
    *reinterpret_cast<void**>(props + 0x20) = reinterpret_cast<void*>(&ShellHit);
    AssignText(props + 0x24, "exp_canister_small");
    At<float>(props, 0x28) = 0;     // start radius
    At<float>(props, 0x2C) = 150;   // end radius
    At<float>(props, 0x30) = 200;   // push
    At<float>(props, 0x34) = 60;    // damage (ours: the callback's)
    At<float>(props, 0x38) = 20;
    At<float>(props, 0x3C) = 0.4f;  // lifetime
    props[0x40] = 1;                // explode once (removed at the end)
    g_ShellOwners[shell] = cody;
    if (!reinterpret_cast<bool(__cdecl*)(uint8_t*, void*, const float*, int)>(uintptr_t(0x000A2DE0))(shell, nullptr, m, 1))
        return;
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*)>((*reinterpret_cast<void* const* const*>(shell))[0xA4 / 4])(shell, nullptr);
    static bool noted = false;
    if (!noted) {
        noted = true;
        LOG_INFO("Cody: a shell falls (%s)", seen ? "with the level's explosion" : "the level has no explosion to show");
    }
}

// --- His helmet ---
// Cody's helmet (his select bust, his portrait) is the Phase II trooper's with a rangefinder: a stalk rising past the dome
// from a housing over his right ear. His body is the 212th's (meshes\chars\clonetrooper\hordetrooper.msh) with the two
// added to the helmet's group (headSG, skinned to the head), made at load under a name of his own, so the other clones
// keep theirs; the engine builds its animation binding as for any new body (the same skeleton). Sizes in model units,
// the texel each part takes its colour from in the 128-texel body texture (the visor's dark grey, the dome's white).
constexpr const char* kBodySource = "meshes\\chars\\clonetrooper\\hordetrooper.msh";
constexpr const char* kBody = "meshes\\chars\\clonetrooper\\hordetrooper_cody.msh";
const std::vector<MeshBox> kRangefinder = {
    // the helmet's front is at z 6 to 7.5 under the brow (y 70 to 71), its sides at x 4
    { { -3.2f, 70.5f, 4.6f }, { 3.2f, 71.1f, 7.3f }, { 6.0f, 92.0f } },     // the orange brim over the visor (the
                                                                          // painted brow's texel), out past the front
    { { 3.55f, 71.0f, 1.2f }, { 4.25f, 72.4f, 2.2f }, { 24.5f, 97.5f } },   // the rangefinder's hinge on his right
    { { 3.95f, 72.4f, 0.9f }, { 4.2f, 75.6f, 3.0f }, { 24.5f, 97.5f } },    // its viewplate, flipped up: a thin grey
                                                                          // plate standing over the dome's side
};

// His markings: the 212th's helmet (one 128x128 DXT1 texture for the whole body, the helmet unwrapped once and mirrored
// on both sides, u 0 the front's middle) gets an orange brow over the visor wrapping round the dome, and an orange
// panel by each end of the visor's top: in the 212th's own orange (its dome ring's), shaded by the plate's own
// brightness (its scratches kept), with a soft edge. Only the blocks they touch are decoded, painted and encoded
// again. Served as the texture set "_cody" of his costume (customchars.cpp), so the clones keep theirs.
constexpr const char* kHelmetSource = "meshes\\chars\\clonetrooper\\hordetrooper.stx";
constexpr const char* kHelmetTexture = "meshes\\chars\\clonetrooper\\hordetrooper_cody.stx";
constexpr const char kHelmetName[] = "hordeTrooper_cody";
constexpr size_t kStxPixels = 0x184, kStxNameAt = 0x40;
constexpr int kTextureSize = 128;

struct Point {
    float x, y;
};
const std::vector<std::vector<Point>> kMarkings = { // polygons in texels
    { { 0, 89 }, { 16, 89.5f }, { 16, 94 }, { 14, 99.5f }, { 0, 99.5f } },                                  // brow
    { { 16, 89.5f }, { 26, 90 }, { 33, 91 }, { 35, 92.5f }, { 32, 94 }, { 16, 94 } },                       // its wing
    { { 13, 99.5f }, { 16, 94 }, { 22, 94 }, { 22, 100 }, { 20, 106 }, { 17, 111 }, { 14, 110 }, { 13, 103 } }, // cheek
};

bool Inside(const std::vector<Point>& p, float x, float y)
{
    bool in = false;
    for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++)
        if ((p[i].y > y) != (p[j].y > y) && x < (p[j].x - p[i].x) * (y - p[i].y) / (p[j].y - p[i].y) + p[i].x)
            in = !in;
    return in;
}

void Colour565(uint16_t c, float rgb[3])
{
    rgb[0] = float((c >> 11) & 31) * 255.0f / 31.0f;
    rgb[1] = float((c >> 5) & 63) * 255.0f / 63.0f;
    rgb[2] = float(c & 31) * 255.0f / 31.0f;
}

uint16_t To565(const float rgb[3])
{
    const auto q = [](float v, int bits) { return std::clamp(int(v * float((1 << bits) - 1) / 255.0f + 0.5f), 0, (1 << bits) - 1); };
    return uint16_t(q(rgb[0], 5) << 11 | q(rgb[1], 6) << 5 | q(rgb[2], 5));
}

void DecodeBlock(const uint8_t* block, float texels[16][3])
{
    const uint16_t c0 = uint16_t(block[0] | block[1] << 8), c1 = uint16_t(block[2] | block[3] << 8);
    float palette[4][3];
    Colour565(c0, palette[0]);
    Colour565(c1, palette[1]);
    for (int k = 0; k < 3; ++k) {
        palette[2][k] = c0 > c1 ? (2 * palette[0][k] + palette[1][k]) / 3 : (palette[0][k] + palette[1][k]) / 2;
        palette[3][k] = c0 > c1 ? (palette[0][k] + 2 * palette[1][k]) / 3 : 0;
    }
    for (int i = 0; i < 16; ++i)
        std::memcpy(texels[i], palette[(block[4 + i / 4] >> (2 * (i % 4))) & 3], sizeof texels[i]);
}

// A plain encoder: the block's darkest and brightest texels as the ends, each texel the nearest of the four.
void EncodeBlock(const float texels[16][3], uint8_t* block)
{
    const auto luma = [](const float* c) { return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]; };
    int lo = 0, hi = 0;
    for (int i = 1; i < 16; ++i) {
        lo = luma(texels[i]) < luma(texels[lo]) ? i : lo;
        hi = luma(texels[i]) > luma(texels[hi]) ? i : hi;
    }
    uint16_t c0 = To565(texels[hi]), c1 = To565(texels[lo]);
    if (c0 < c1)
        std::swap(c0, c1);
    float palette[4][3];
    Colour565(c0, palette[0]);
    Colour565(c1, palette[1]);
    for (int k = 0; k < 3; ++k) {
        palette[2][k] = (2 * palette[0][k] + palette[1][k]) / 3;
        palette[3][k] = (palette[0][k] + 2 * palette[1][k]) / 3;
    }
    block[0] = uint8_t(c0);
    block[1] = uint8_t(c0 >> 8);
    block[2] = uint8_t(c1);
    block[3] = uint8_t(c1 >> 8);
    for (int row = 0; row < 4; ++row) {
        uint8_t bits = 0;
        for (int col = 0; col < 4; ++col) {
            const float* t = texels[row * 4 + col];
            int best = 0;
            float bestDistance = 1e30f;
            for (int p = 0; p < (c0 == c1 ? 1 : 4); ++p) {
                float d = 0;
                for (int k = 0; k < 3; ++k)
                    d += (t[k] - palette[p][k]) * (t[k] - palette[p][k]);
                if (d < bestDistance) {
                    bestDistance = d;
                    best = p;
                }
            }
            bits = uint8_t(bits | best << (2 * col));
        }
        block[4 + row] = bits;
    }
}

bool PaintHelmet(std::vector<uint8_t>& data)
{
    constexpr int blocks = kTextureSize / 4;
    if (data.size() < kStxPixels + size_t(blocks * blocks * 8) || std::memcmp(data.data(), "STX", 4) != 0 || data[8] != 3)
        return false;
    std::vector<float> image(size_t(kTextureSize * kTextureSize * 3));
    const auto at = [&](int x, int y) { return &image[size_t((y * kTextureSize + x) * 3)]; };
    for (int b = 0; b < blocks * blocks; ++b) {
        float texels[16][3];
        DecodeBlock(&data[kStxPixels + size_t(b) * 8], texels);
        for (int i = 0; i < 16; ++i)
            std::memcpy(at((b % blocks) * 4 + i % 4, (b / blocks) * 4 + i / 4), texels[i], sizeof texels[i]);
    }
    const auto luma = [](const float* c) { return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]; };
    float orange[3] = {}, white = 0;
    int oranges = 0, whites = 0;
    for (int y = 79; y <= 84; ++y) // the dome's ring
        for (int x = 8; x <= 36; ++x) {
            const float* c = at(x, y);
            if (c[0] > 170 && c[1] > 95 && c[1] < 190 && c[2] < 110) {
                for (int k = 0; k < 3; ++k)
                    orange[k] += c[k];
                ++oranges;
            }
        }
    for (int y = 87; y <= 93; ++y) // the dome's plate
        for (int x = 16; x <= 36; ++x, ++whites)
            white += luma(at(x, y));
    if (!oranges || !whites)
        return false;
    for (float& o : orange)
        o /= float(oranges);
    white /= float(whites);
    std::vector<uint8_t> mark(size_t(kTextureSize * kTextureSize), 0); // 2 painted, 1 its edge
    for (int y = 0; y < kTextureSize; ++y)
        for (int x = 0; x < kTextureSize; ++x)
            for (const auto& p : kMarkings)
                if (Inside(p, float(x) + 0.5f, float(y) + 0.5f))
                    mark[size_t(y * kTextureSize + x)] = 2;
    const auto painted = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < kTextureSize && y < kTextureSize && mark[size_t(y * kTextureSize + x)] == 2;
    };
    for (int y = 0; y < kTextureSize; ++y)
        for (int x = 0; x < kTextureSize; ++x)
            if (!mark[size_t(y * kTextureSize + x)] && (painted(x - 1, y) || painted(x + 1, y) || painted(x, y - 1) || painted(x, y + 1)))
                mark[size_t(y * kTextureSize + x)] = 1;
    std::vector<bool> touched(size_t(blocks * blocks), false);
    for (int y = 0; y < kTextureSize; ++y)
        for (int x = 0; x < kTextureSize; ++x) {
            const uint8_t m = mark[size_t(y * kTextureSize + x)];
            if (!m)
                continue;
            float* c = at(x, y);
            const float shade = std::clamp(luma(c) / white, 0.0f, 1.15f);
            for (int k = 0; k < 3; ++k) {
                const float paint = std::min(255.0f, orange[k] * shade);
                c[k] = m == 2 ? paint : (c[k] + paint) / 2;
            }
            touched[size_t((y / 4) * blocks + x / 4)] = true;
        }
    for (int b = 0; b < blocks * blocks; ++b) {
        if (!touched[size_t(b)])
            continue;
        float texels[16][3];
        for (int i = 0; i < 16; ++i)
            std::memcpy(texels[i], at((b % blocks) * 4 + i % 4, (b / blocks) * 4 + i / 4), sizeof texels[i]);
        EncodeBlock(texels, &data[kStxPixels + size_t(b) * 8]);
    }
    std::memset(&data[kStxNameAt], 0, 32);
    std::memcpy(&data[kStxNameAt], kHelmetName, sizeof kHelmetName - 1);
    return true;
}

bool HelmetGenerator(const std::string& lowerName, std::vector<uint8_t>& data)
{
    return lowerName == kHelmetTexture && ReadDiscResource(kHelmetSource, data) && PaintHelmet(data);
}

bool BodyGenerator(const std::string& lowerName, std::vector<uint8_t>& data)
{
    if (lowerName != kBody || !ReadDiscResource(kBodySource, data))
        return false;
    std::string report;
    const bool made = AddMeshBoxes(data, "headSG", kRangefinder, 128, report);
    if (made)
        LOG_INFO("Cody: his helmet's rangefinder made (%s)", report.c_str());
    else
        LOG_WARN("Cody: his helmet could not be made (%s)", report.c_str());
    return made;
}

// Whether the script's character may start something now: its current sequence lets it branch (the sequence's
// record +0x30 bit 1, as the decision-frame test 0x173D70 reads it, without that test's own bookkeeping). Not while
// gripped, knocked down, hit, rolling or falling.
bool CanAct()
{
    uint8_t* scripts = *reinterpret_cast<uint8_t**>(uintptr_t(kScriptCharacterContext));
    uint8_t* state = scripts ? At<uint8_t*>(scripts, 0x70) : nullptr;
    if (!state)
        return false;
    uint8_t* record = reinterpret_cast<uint8_t*(__fastcall*)(uint8_t*, void*)>(
        (*reinterpret_cast<void* const* const*>(state))[1])(state, nullptr);
    return record && (At<uint32_t>(record, 0x30) >> 1 & 1) != 0;
}

} // namespace

void InstallCody()
{
    g_Kits.clear();
    g_Helpers.clear();
    g_Shells.clear();
    g_ShellOwners.clear();
    for (OwnMove& move : g_Moves)
        move.handle = 0;
    if (!FindCustomCharacter("ICody"))
        return;
    AddClassSequences(&MoveSequences);
    RegisterResourceGenerator(&BodyGenerator);
    RegisterResourceGenerator(&HelmetGenerator);
}

void CloneKitFrame(uint8_t* script, uint8_t* instance, uint8_t* character)
{
    if (!IsCody(character) || !instance)
        return;
    Kit& kit = g_Kits[character];
    const uint32_t id = At<uint32_t>(character, 4);
    const ULONGLONG now = GetTickCount64();
    if (kit.id != id) { // a new Cody where an old one was
        kit = Kit{};
        kit.id = id;
        kit.armedAt = HvvActive() ? now : now + kArmAfterMs; // (the computer's: not in the duel's intro)
    }
    if (At<int32_t>(character, 0x43C) >= 0)
        kit.player = true;
    float& meter = At<float>(character, kCharacterMeter);
    if (kit.lastFrame) // the meter refills
        meter = std::min(At<float>(character, kCharacterMeterMost), meter + kMeterPerSecond * float(now - kit.lastFrame) / 1000.0f);
    kit.lastFrame = now;
    if (At<float>(character, kCharacterHealth) <= 0 || (HvvActive() && GetHvvState().intro)) {
        Layer(kit, false);
        return;
    }
    // a player's Cody acts once the game stops driving him (the duel's intro); the computer's after its intro
    if (kit.player ? reinterpret_cast<bool(__cdecl*)()>(uintptr_t(kIsComputer))() : now < kit.armedAt) {
        Layer(kit, false);
        return;
    }
    // nothing new while he is gripped, down, hit, rolling or falling
    const bool free = CanAct();
    const bool squadOut = std::any_of(g_Helpers.begin(), g_Helpers.end(), [&](const Helper& h) { return h.master == character; });
    if (!kit.player) { // shoots as any clone; his specials when his opponent is near
        uint8_t* opponent = OpponentOf(character);
        if (!opponent || !free || kit.summon || kit.bombard)
            return;
        const float* p = Position(character);
        const float* q = Position(opponent);
        const float apart = std::hypot(q[0] - p[0], q[2] - p[2]);
        if (apart < kComputerCallsWithin && !squadOut && meter >= kSummonCost) {
            meter -= kSummonCost;
            kit.summon = true;
            Say(character, "TKE", "CLC013");
        } else if (apart > 250 && apart < kComputerCallsWithin && meter >= kBombardCost + 100) {
            meter -= kBombardCost;
            kit.bombard = true;
            Say(character, "TKE", "CMC915");
        }
        return;
    }
    if (!free) {
        Layer(kit, false);
        kit.lastBlock = Held(kButtonDodge);
        return;
    }
    // his clones (one squad at a time), the bombardment
    if (Pressed(kButtonPush) && !squadOut && !kit.summon && meter >= kSummonCost) {
        meter -= kSummonCost;
        kit.summon = true;
        Say(character, "TKE", "CLC013"); // "Bring up the reinforcements!"
    }
    if (Pressed(kButtonCritical) && !kit.bombard && meter >= kBombardCost) {
        meter -= kBombardCost;
        kit.bombard = true;
        Say(character, "TKE", "CMC915"); // "Concentrate fire on the target!"
    }
    // the roll: on the press, where the stick points on the screen (its port's stick, by the camera's right and
    // forward as drawn), the side nearer it once he has turned to his opponent (CodyFrame)
    const bool dodge = Held(kButtonDodge);
    if (dodge && !kit.lastBlock && !kit.roll && now - kit.lastDodge >= kDodgeWaitMs) {
        kit.lastDodge = now;
        kit.roll = 3; // a side to be chosen
        kit.rollDir[0] = kit.rollDir[1] = 0;
        float x = 0, y = 0, view[16], fov = 0;
        if (input::LeftStick(At<int32_t>(character, 0x43C), x, y) && GameRenderedView(view, fov) && std::hypot(x, y) > 0.3f) {
            kit.rollDir[0] = x * view[0] + y * view[2]; // the camera's right (the view's first column) and forward
            kit.rollDir[1] = x * view[8] + y * view[10];
        }
    }
    kit.lastBlock = dodge;
    // the blaster
    if (!Held(kButtonHeavy)) {
        Layer(kit, false);
        return;
    }
    if (!kit.shooting) {
        Layer(kit, true);
        kit.lastShot = now - kShotIntervalMs + 120; // the first bolt a few frames into the layer
    }
    if (now - kit.lastShot >= kShotIntervalMs) {
        kit.lastShot = now;
        Fire(script, instance);
    }
}

namespace {

// A roll: facing his opponent, aside, out of reach of any hit while it lasts (the character's "Invincible?" byte,
// +0x5D6, as Heroes vs Villains' respawns; given back after).
void Roll(uint8_t* cody, Kit& kit, int side, ULONGLONG now)
{
    FaceToward(cody, OpponentOf(cody));
    if (side == 3) { // the stick's side: by his right as he now stands (his placement's first row)
        const float* right = reinterpret_cast<const float*>(cody + kCharacterTransform);
        const float along = kit.rollDir[0] * right[0] + kit.rollDir[1] * right[2];
        side = along < 0 ? 1 : 2;
    }
    if (!PlaySequence(cody, side == 1 ? "Cody_RollLeft" : "Cody_RollRight"))
        return;
    LOG_INFO("Cody: rolls %s", side == 1 ? "left" : "right");
    if (!kit.guarded) {
        kit.ownInvincible = At<uint8_t>(cody, kCharacterInvincible);
        kit.guarded = true;
    }
    At<uint8_t>(cody, kCharacterInvincible) = 1;
    kit.rollUntil = now + kRollGuardMs;
}

// The round is over when his opponent of the round fell (a duel brings both back for the next).
bool RoundOver(Kit& kit)
{
    return kit.foe && (!Live(kit.foe, kit.foeId) || At<float>(kit.foe, kCharacterHealth) <= 0);
}

} // namespace

void CodyFrame()
{
    if (g_Kits.empty() && g_Helpers.empty() && g_Shells.empty())
        return;
    const ULONGLONG now = GetTickCount64();
    // his clones: down, they go for his opponent; their time up, his or the round's, they fall as clones do (the
    // character's instant kill, vfunc +0xC8: its death, the game takes the body away; removing the object at once,
    // while the level's lists held it, left a hole in one: 0x16B3C7)
    for (auto it = g_Helpers.begin(); it != g_Helpers.end();) {
        const auto kit = g_Kits.find(it->master);
        const bool masterGone = kit == g_Kits.end() || !Live(it->master, kit->second.id) ||
            At<float>(it->master, kCharacterHealth) <= 0 || RoundOver(kit->second);
        if (!Live(it->object, it->id)) {
            it = g_Helpers.erase(it);
            continue;
        }
        if (now >= it->until || masterGone) {
            LOG_INFO("Cody: a clone of his falls (%s)", now >= it->until ? "his time" : "the round or Cody over");
            if (At<float>(it->object, kCharacterHealth) > 0)
                reinterpret_cast<void(__fastcall*)(uint8_t*, void*, uint32_t)>(
                    (*reinterpret_cast<void* const* const*>(it->object))[0xC8 / 4])(it->object, nullptr, 0);
            it = g_Helpers.erase(it);
            continue;
        }
        if (it->engageAt && now >= it->engageAt) {
            it->engageAt = 0;
            if (uint8_t* opponent = OpponentOf(it->master))
                if (uint8_t* brain = At<uint8_t*>(it->object, kCharacterAI))
                    reinterpret_cast<void(__fastcall*)(uint8_t*, void*, uint8_t*)>(uintptr_t(kAISetTarget))(brain, nullptr, opponent);
        }
        ++it;
    }
    // the shells falling
    for (auto it = g_Shells.begin(); it != g_Shells.end();) {
        if (now < it->at) {
            ++it;
            continue;
        }
        if (Live(it->cody, it->codyId) && it->target && At<float>(it->target, kCharacterHealth) > 0)
            FireShell(it->cody, it->target);
        it = g_Shells.erase(it);
    }
    for (auto it = g_Kits.begin(); it != g_Kits.end();) {
        uint8_t* cody = it->first;
        Kit& kit = it->second;
        if (!Live(cody, kit.id)) {
            it = g_Kits.erase(it);
            continue;
        }
        const bool standing = At<float>(cody, kCharacterHealth) > 0;
        // his opponent of the round (to know its end), once one stands
        if (standing && (!kit.foe || RoundOver(kit)))
            if (uint8_t* foe = OpponentOf(cody); foe && foe != kit.foe) {
                kit.foe = foe;
                kit.foeId = At<uint32_t>(foe, 4);
            }
        // the roll's guard over
        if (kit.guarded && now >= kit.rollUntil) {
            At<uint8_t>(cody, kCharacterInvincible) = kit.ownInvincible;
            kit.guarded = false;
        }
        if (kit.summon) {
            kit.summon = false;
            if (standing) {
                PlaySequence(cody, "Cody_SignalLeft"); // he waves them in
                CallSquad(cody, !kit.player, now);
            }
        }
        if (kit.bombard) {
            kit.bombard = false;
            if (uint8_t* target = standing ? OpponentOf(cody) : nullptr) {
                PlaySequence(cody, "Cody_Intro"); // the strike signal
                for (int i = 0; i < kShells; ++i)
                    g_Shells.push_back({ cody, kit.id, target, now + kFirstShellMs + ULONGLONG(i) * kShellGapMs });
                LOG_INFO("Cody: bombardment on %p", target);
            }
        }
        if (kit.roll) {
            if (standing)
                Roll(cody, kit, kit.roll, now);
            kit.roll = 0;
        }
        // the computer's Cody rolls away from his opponent's saber when it comes close, as a Jedi blocks
        if (standing && !kit.player && now >= kit.armedAt && now - kit.lastDodge > kComputerDodgeWaitMs) {
            if (uint8_t* opponent = OpponentOf(cody)) {
                const float* p = Position(cody);
                const float* q = Position(opponent);
                kit.lastDodge = now; // looked once a wait: a roll half the time
                if (std::hypot(q[0] - p[0], q[2] - p[2]) < 200.0f && (now / 7) % 2 == 0) {
                    Roll(cody, kit, (now / 13) % 2 ? 1 : 2, now);
                    LOG_INFO("Cody: the computer's Cody rolls aside");
                }
            }
        }
        ++it;
    }
}

bool KeepsLimbs(const uint8_t* character)
{
    return IsCody(character);
}

std::vector<CodyHelper> CodyHelpers()
{
    std::vector<CodyHelper> out;
    for (const Helper& h : g_Helpers)
        out.push_back({ h.object, h.master, h.enemy });
    return out;
}

float CodyDamage(uint8_t* character, float change)
{
    const auto kit = g_Kits.find(character);
    if (change >= 0 || kit == g_Kits.end())
        return change;
    if (GetTickCount64() < kit->second.rollUntil)
        return 0; // rolling: out of the way
    return change * kDamageTaken;
}

} // namespace swrots::game
