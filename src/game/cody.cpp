// Commander Cody's fighting kit (see cody.h).
//
// A clone under player control has one rule, the rifle butt on fast attack (the clone's rules 0x35AE70, player part
// 0x35B2A4); its shooting is its AI's (CloneTrooper.cpp: a layer over what it does, a bolt at its target). The cut
// playable Cody (ICommanderCody, whose script survives, rules 0x360D00) had his own: a shot on fast attack and a burst
// on heavy attack for 250 of his meter. Here his meter (Force +0xA40, which the clone has; refilled here) pays for all
// he does beyond the rifle butt, for the player's Cody and the computer's alike:
//   - heavy attack (Y) held: the shooting layer and a bolt every kShotIntervalMs, kShotCost each (in the clone's
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
#include "game/characters.h"
#include "game/classsetup.h"
#include "game/customchars.h"
#include "game/devmap.h"
#include "game/hvv.h"
#include "game/meshedit.h"
#include "game/resources.h"

namespace swrots::game {

namespace {

constexpr uint32_t kHeldFrames = 0x00177FF0;             // cdecl int (button, 0), in a script's turn
constexpr uint32_t kPressed = 0x00178070;                // cdecl bool (button, 1, 0, 2)
constexpr uint32_t kButtonHeavy = 0x006964DC, kButtonCritical = 0x006964EC; // the script's button ids (Y, B)
constexpr uint32_t kButtonBlock = 0x006964E4, kButtonPush = 0x006964E8;     // (LT, RT)
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
constexpr float kShotCost = 15, kSummonCost = 500, kBombardCost = 400, kDodgeCost = 150;
constexpr ULONGLONG kShotIntervalMs = 400;
constexpr ULONGLONG kHelperLifeMs = 25000;
constexpr ULONGLONG kComputerDodgeWaitMs = 2500; // the computer's Cody: one dodge in this long at most
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
    ULONGLONG lastShot = 0, lastFrame = 0, lastDodge = 0;
    bool summon = false;  // asked for, made in CodyFrame
    bool bombard = false; // likewise
    int roll = 0;         // likewise: 1 left, 2 right
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
    uint16_t handle;
};
OwnMove g_Moves[] = {
    { "Cody_RollLeft", "CTroop_Nav_RollOut_L", 0 },
    { "Cody_RollRight", "CTroop_Nav_RollOut_R", 0 },
    { "Cody_SignalLeft", "ClnCom_Signal_Left", 0 }, // calling his clones in
};
constexpr int kMoveCount = int(std::size(g_Moves));
constexpr uint32_t kSetup = 0x006965D8;
const char kDuelMapTag[] = "DuelMap";

template <int K> bool __fastcall MoveFn(uint8_t*, void*, uint32_t, uint32_t)
{
    auto* setup = reinterpret_cast<uint8_t*>(uintptr_t(kSetup));
    OwnMove& move = g_Moves[K];
    const bool declared = reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint16_t*, const char*, const char*, float, uint32_t)>(
        uintptr_t(0x001F5B20))(setup, nullptr, &move.handle, move.name, move.animation, 1.0f, 0x0055FB60);
    if (declared) { // optional, required on duel maps (as Vader_Intro)
        *reinterpret_cast<uint8_t*>(uintptr_t(0x00696531)) = 1;
        *reinterpret_cast<const char**>(uintptr_t(0x006964B8)) = kDuelMapTag;
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
    { { 3.9f, 67.0f, -0.4f }, { 5.0f, 69.4f, 1.5f }, { 30.5f, 109.5f } },   // the housing over the ear
    { { 4.6f, 69.2f, 0.25f }, { 4.95f, 75.6f, 0.65f }, { 24.5f, 97.5f } },  // the stalk
};

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
        kit.armedAt = now + kArmAfterMs;
    }
    if (At<int32_t>(character, 0x43C) >= 0)
        kit.player = true;
    // his health's most: a duel gives its fighters 1000 at each round, more than a blaster against a saber should have
    if (const CustomCharacter* custom = CustomCharacterOf(character); custom && custom->health > 0) {
        At<float>(character, kCharacterHealthMost) = custom->health;
        At<float>(character, kCharacterHealth) = std::min(At<float>(character, kCharacterHealth), custom->health);
    }
    float& meter = At<float>(character, kCharacterMeter);
    if (kit.lastFrame) // the meter refills
        meter = std::min(At<float>(character, kCharacterMeterMost), meter + kMeterPerSecond * float(now - kit.lastFrame) / 1000.0f);
    kit.lastFrame = now;
    if (At<float>(character, kCharacterHealth) <= 0 || now < kit.armedAt || (HvvActive() && GetHvvState().intro)) {
        Layer(kit, false);
        return;
    }
    const bool squadOut = std::any_of(g_Helpers.begin(), g_Helpers.end(), [&](const Helper& h) { return h.master == character; });
    if (!kit.player) { // shoots as any clone; his specials when his opponent is near
        uint8_t* opponent = OpponentOf(character);
        if (!opponent || kit.summon || kit.bombard)
            return;
        const float* p = Position(character);
        const float* q = Position(opponent);
        const float apart = std::hypot(q[0] - p[0], q[2] - p[2]);
        if (apart < kComputerCallsWithin && !squadOut && meter >= kSummonCost + kDodgeCost) {
            meter -= kSummonCost;
            kit.summon = true;
            Say(character, "TKE", "CLC013");
        } else if (apart > 250 && apart < kComputerCallsWithin && meter >= kBombardCost + kDodgeCost + 100) {
            meter -= kBombardCost;
            kit.bombard = true;
            Say(character, "TKE", "CMC915");
        }
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
    // the roll: on the press of block, to the stick's side (the left without one)
    const bool block = Held(kButtonBlock);
    if (block && !kit.lastBlock && !kit.roll && meter >= kDodgeCost) {
        meter -= kDodgeCost;
        kit.roll = Direction(0x001F54C0, instance) ? 2 : 1;
    }
    kit.lastBlock = block;
    // the blaster
    if (!Held(kButtonHeavy) || meter < kShotCost) {
        Layer(kit, false);
        return;
    }
    if (!kit.shooting) {
        Layer(kit, true);
        kit.lastShot = now - kShotIntervalMs + 120; // the first bolt a few frames into the layer
    }
    if (now - kit.lastShot >= kShotIntervalMs) {
        kit.lastShot = now;
        meter -= kShotCost;
        Fire(script, instance);
    }
}

void CodyFrame()
{
    if (g_Kits.empty() && g_Helpers.empty() && g_Shells.empty())
        return;
    const ULONGLONG now = GetTickCount64();
    // his clones: down, they go for his opponent; their time up (or his), they fall as clones do (the character's
    // instant kill, vfunc +0xC8: its death, the game takes the body away; removing the object at once, while the
    // level's lists held it, left a hole in one: 0x16B3C7)
    for (auto it = g_Helpers.begin(); it != g_Helpers.end();) {
        const auto kit = g_Kits.find(it->master);
        const bool masterGone = kit == g_Kits.end() || !Live(it->master, kit->second.id) ||
            At<float>(it->master, kCharacterHealth) <= 0;
        if (!Live(it->object, it->id)) {
            it = g_Helpers.erase(it);
            continue;
        }
        if (now >= it->until || masterGone) {
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
                LOG_INFO("Cody: rolls %s%s", kit.roll == 1 ? "left" : "right",
                    PlaySequence(cody, kit.roll == 1 ? "Cody_RollLeft" : "Cody_RollRight") ? "" : " (refused)");
            kit.roll = 0;
        }
        // the computer's Cody rolls aside now and then when his opponent comes close
        if (standing && !kit.player && now - kit.lastDodge > kComputerDodgeWaitMs) {
            float& meter = At<float>(cody, kCharacterMeter);
            uint8_t* opponent = OpponentOf(cody);
            if (opponent && meter >= kDodgeCost) {
                const float* p = Position(cody);
                const float* q = Position(opponent);
                kit.lastDodge = now; // looked once a wait: a roll a third of the time
                if (std::hypot(q[0] - p[0], q[2] - p[2]) < 170.0f && (now / 7) % 3 == 0 &&
                    PlaySequence(cody, (now / 13) % 2 ? "Cody_RollLeft" : "Cody_RollRight")) {
                    meter -= kDodgeCost;
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

} // namespace swrots::game
