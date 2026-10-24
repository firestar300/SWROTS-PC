// Commander Cody's fighting kit (see cody.h).
//
// A clone under player control has one rule, the rifle butt on fast attack (the clone's rules 0x35AE70, player part
// 0x35B2A4); its shooting is its AI's (CloneTrooper.cpp: a layer over what it does, a bolt at its target). The cut
// playable Cody (ICommanderCody, whose script survives, rules 0x360D00) had his own: a shot on fast attack and a
// 10-bolt burst on heavy attack for 250 of his meter. Here, in the clone's script's turn (its StateManager, called for
// every clone every frame; the input functions read the script's character's own controller):
//   - heavy attack (Y) held: the shooting layer and a bolt every kShotIntervalMs, as co-op's clone;
//   - critical (B): a burst of kBurstBolts for kBurstCost of the meter (Force +0xA40, which the clone has; refilled
//     here, kMeterPerSecond);
//   - block (LT) with the stick left or right, in the rules' turn (a decision frame): a roll, the clone's roll out of
//     cover (CTroop_Nav_RollOut_L/R) as sequences of the port's (optional in the class, so not loaded in a duel: the
//     port's are required on duel maps, as the duelists' intros are), instead of a block, which a clone has not;
//   - push (RT): two clones of his for kHelperLifeMs, kSummonWaitMs between calls; he signals them in (his intro).
// The computer's Cody shoots as any clone; he calls his clones when he can.

#include "game/cody.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

#include "core/log.h"
#include "core/patch.h"
#include "game/characters.h"
#include "game/classsetup.h"
#include "game/customchars.h"
#include "game/devmap.h"

namespace swrots::game {

namespace {

constexpr uint32_t kScriptContext = 0x006964C0;          // the script instance running
constexpr uint32_t kScriptCharacterContext = 0x006943CC; // +0x68: the character it runs for
constexpr uint32_t kIsComputer = 0x00176E30;             // cdecl bool (): the script's character is the AI's
constexpr uint32_t kHeldFrames = 0x00177FF0;             // cdecl int (button, 0)
constexpr uint32_t kPressed = 0x00178070;                // cdecl bool (button, 1, 0, 2)
constexpr uint32_t kButtonHeavy = 0x006964DC, kButtonCritical = 0x006964EC; // the script's button ids (Y, B)
constexpr uint32_t kButtonBlock = 0x006964E4, kButtonPush = 0x006964E8;     // (LT, RT)
constexpr uint32_t kScriptHasTarget = 0x00176FF0;   // cdecl bool ()
constexpr uint32_t kScriptPlayLayer = 0x00173B40;   // cdecl (animation, ...): an animation layer
constexpr uint32_t kScriptStopLayer = 0x00173B80;   // cdecl (layer, blend)
constexpr uint32_t kScriptShotSpread = 0x00176AB0;  // cdecl bool (): no spread
constexpr uint32_t kCloneFireBolt = 0x003C0090;     // cdecl (instance, script, 0, spread, muzzle data, muzzle)
constexpr uint32_t kCloneNextMuzzle = 0x001760B0;   // cdecl (muzzle*, count)
constexpr uint32_t kScriptPlaySound = 0x0017AD20;   // cdecl (sound, near, 1, far, -1, 0)
constexpr uint32_t kCloneMuzzle = 0x3D8, kCloneMuzzleData = 0x3EC, kCloneFireSound = 0x3E4; // in the instance
constexpr char kCloneShootingLoop[] = "ctroop_atk_shooting_loop";
constexpr uint32_t kCharacterHealth = 0x130, kCharacterMeter = 0xA40, kCharacterMeterMost = 0xA44;
constexpr uint32_t kCharacterTransform = 0x150, kCharacterOpponent = 0x3B8, kCharacterScript = 0x434;
constexpr uint32_t kCharacterAI = 0x9FC;
constexpr uint32_t kAISetTarget = 0x00193D90;  // thiscall on the AI controller (target)
constexpr uint32_t kObjectById = 0x000A31F0;   // cdecl (instance id) -> object or null
constexpr uintptr_t kPlaySequence = 0x150E30;  // ICharacter, thiscall (sequence, 0, 0, 0, speed)

constexpr ULONGLONG kShotIntervalMs = 250;
constexpr int kBurstBolts = 8;
constexpr ULONGLONG kBurstIntervalMs = 70;
constexpr float kBurstCost = 250, kMeterPerSecond = 40;
constexpr ULONGLONG kSummonWaitMs = 30000, kHelperLifeMs = 25000;
constexpr float kHelperSide = 90;      // how far beside him they come in
constexpr float kComputerCallsWithin = 900; // the computer's Cody calls his clones with his opponent this near

template <typename T> T& At(uint8_t* p, uint32_t offset)
{
    return *reinterpret_cast<T*>(p + offset);
}

bool Live(uint8_t* object, uint32_t id)
{
    return object && reinterpret_cast<uint8_t*(__cdecl*)(uint32_t)>(uintptr_t(kObjectById))(id) == object;
}

bool IsCody(const uint8_t* character)
{
    const CustomCharacter* custom = character ? CustomCharacterOf(character) : nullptr;
    return custom && custom->builtIn && custom->className == "ICody";
}

struct Kit {
    uint32_t id = 0;
    bool shooting = false;
    ULONGLONG lastShot = 0, lastFrame = 0, nextBurst = 0, summonReady = 0;
    int burstLeft = 0;
    bool summon = false; // asked for, made in CodyFrame
};
std::unordered_map<uint8_t*, Kit> g_Kits;

struct Helper {
    uint8_t* object;
    uint32_t id;
    uint8_t* master;
    ULONGLONG until;
};
std::vector<Helper> g_Helpers;

uint32_t Button(uint32_t global)
{
    return *reinterpret_cast<const uint32_t*>(uintptr_t(global));
}

bool Pressed(uint32_t global)
{
    return reinterpret_cast<bool(__cdecl*)(uint32_t, int, int, int)>(uintptr_t(kPressed))(Button(global), 1, 0, 2);
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

// The opponent a character fights: its own (+0x3B8) when it is a standing character, else the nearest other one
// standing that is not one of the helpers.
uint8_t* OpponentOf(uint8_t* character)
{
    const auto standing = [](uint8_t* c) { return c && At<float>(c, kCharacterHealth) > 0; };
    const std::vector<uint8_t*> characters = LevelCharacters();
    uint8_t* own = At<uint8_t*>(character, kCharacterOpponent);
    if (own && own != character && standing(own) && std::find(characters.begin(), characters.end(), own) != characters.end())
        return own;
    const float* p = reinterpret_cast<const float*>(character + kCharacterTransform + 48);
    uint8_t* best = nullptr;
    float bestDistance = 1e30f;
    for (uint8_t* c : characters) {
        if (c == character || !standing(c) ||
            std::any_of(g_Helpers.begin(), g_Helpers.end(), [&](const Helper& h) { return h.object == c; }))
            continue;
        const float* q = reinterpret_cast<const float*>(c + kCharacterTransform + 48);
        const float d = std::hypot(q[0] - p[0], q[2] - p[2]);
        if (d < bestDistance) {
            bestDistance = d;
            best = c;
        }
    }
    return best;
}

// --- His rolls: sequences of the port's on the clone class ---
struct Roll {
    const char* name;
    const char* animation;
    uint16_t handle;
};
Roll g_Rolls[2] = { { "Cody_RollLeft", "CTroop_Nav_RollOut_L", 0 }, { "Cody_RollRight", "CTroop_Nav_RollOut_R", 0 } };
constexpr uint32_t kSetup = 0x006965D8;
const char kDuelMapTag[] = "DuelMap";

template <int K> bool __fastcall RollFn(uint8_t*, void*, uint32_t, uint32_t)
{
    auto* setup = reinterpret_cast<uint8_t*>(uintptr_t(kSetup));
    Roll& roll = g_Rolls[K];
    const bool declared = reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint16_t*, const char*, const char*, float, uint32_t)>(
        uintptr_t(0x001F5B20))(setup, nullptr, &roll.handle, roll.name, roll.animation, 1.0f, 0x0055FB60);
    if (declared) { // optional, required on duel maps (as Vader_Intro)
        *reinterpret_cast<uint8_t*>(uintptr_t(0x00696531)) = 1;
        *reinterpret_cast<const char**>(uintptr_t(0x006964B8)) = kDuelMapTag;
    }
    return reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, const char*, uint32_t)>(uintptr_t(0x001F6C00))(
        setup, nullptr, reinterpret_cast<const char*>(uintptr_t(0x0059C07C)), 0x7D00 + K);
}

int __cdecl RollSequences(const char* shortName, ClassSequenceFn* fns, int max)
{
    if (!BootIsMatch() || _stricmp(shortName, "CloneTrooper") != 0 || max < 2 || !FindCustomCharacter("ICody"))
        return 0;
    fns[0] = &RollFn<0>;
    fns[1] = &RollFn<1>;
    LOG_INFO("Cody: his rolls added to the clone trooper's moves");
    return 2;
}

void OfferRoll(bool condition, const Roll& roll)
{
    void* sequence = reinterpret_cast<void*(__cdecl*)(uint32_t, const char*, int)>(uintptr_t(0x001F53E0))(roll.handle,
        "cody.cpp", __LINE__);
    reinterpret_cast<void(__cdecl*)(bool, void*, int, int, int, float)>(uintptr_t(0x00175D60))(condition, sequence,
        3, 3, 1, 1.0f);
}

bool Direction(uint32_t test, uint8_t* instance) // 0x1F5480 left, 0x1F54C0 right on [instance + 0xFC]
{
    return reinterpret_cast<bool(__cdecl*)(uint32_t)>(uintptr_t(test))(At<uint32_t>(instance, 0xFC));
}

// The clone's rules, called from its StateManager on a decision frame (0x35AE5E: push esi; push ecx; call 0x35AE70).
constexpr uint32_t kCloneRulesCall = 0x0035AE5E;
constexpr uint32_t kCloneRules = 0x0035AE70;
bool g_RollLeftNext = true;

void __cdecl CloneRulesHook(uint8_t* instance, uint8_t* classObject)
{
    reinterpret_cast<void(__cdecl*)(uint8_t*, uint8_t*)>(uintptr_t(kCloneRules))(instance, classObject);
    __try {
        uint8_t* scripts = *reinterpret_cast<uint8_t**>(uintptr_t(kScriptCharacterContext));
        uint8_t* character = scripts ? At<uint8_t*>(scripts, 0x68) : nullptr;
        if (!IsCody(character) || !g_Rolls[0].handle || reinterpret_cast<bool(__cdecl*)()>(uintptr_t(kIsComputer))())
            return;
        if (!Pressed(kButtonBlock))
            return;
        const bool left = Direction(0x001F5480, instance), right = Direction(0x001F54C0, instance);
        const bool toLeft = left || (!right && g_RollLeftNext);
        if (!left && !right)
            g_RollLeftNext = !g_RollLeftNext;
        OfferRoll(true, g_Rolls[toLeft ? 0 : 1]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

} // namespace

void InstallCody()
{
    g_Kits.clear();
    g_Helpers.clear();
    g_Rolls[0].handle = g_Rolls[1].handle = 0;
    if (!FindCustomCharacter("ICody"))
        return;
    AddClassSequences(&RollSequences);
    uint8_t call[5] = { 0xE8 };
    const int32_t rel = int32_t(kCloneRules) - int32_t(kCloneRulesCall + 5);
    std::memcpy(call + 1, &rel, 4);
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kCloneRulesCall)), call, 5) == 0)
        PatchCall(kCloneRulesCall, reinterpret_cast<const void*>(&CloneRulesHook), 5);
    else
        LOG_WARN("Cody: the clone trooper's rules are not as expected; Cody does not roll");
}

void CloneKitFrame(uint8_t* script, uint8_t* instance, uint8_t* character)
{
    if (!IsCody(character) || !instance)
        return;
    Kit& kit = g_Kits[character];
    const uint32_t id = At<uint32_t>(character, 4);
    if (kit.id != id) { // a new Cody where an old one was
        kit = Kit{};
        kit.id = id;
    }
    const ULONGLONG now = GetTickCount64();
    // the meter refills
    if (kit.lastFrame) {
        float& meter = At<float>(character, kCharacterMeter);
        const float most = At<float>(character, kCharacterMeterMost);
        meter = std::min(most, meter + kMeterPerSecond * float(now - kit.lastFrame) / 1000.0f);
    }
    kit.lastFrame = now;
    if (At<float>(character, kCharacterHealth) <= 0) {
        Layer(kit, false);
        kit.burstLeft = 0;
        return;
    }
    const bool computer = reinterpret_cast<bool(__cdecl*)()>(uintptr_t(kIsComputer))();
    if (computer) { // shoots as any clone; calls his clones when his opponent is near
        if (now >= kit.summonReady && !kit.summon) {
            uint8_t* opponent = OpponentOf(character);
            if (opponent) {
                const float* p = reinterpret_cast<const float*>(character + kCharacterTransform + 48);
                const float* q = reinterpret_cast<const float*>(opponent + kCharacterTransform + 48);
                if (std::hypot(q[0] - p[0], q[2] - p[2]) < kComputerCallsWithin)
                    kit.summon = true;
            }
        }
        return;
    }
    // his clones
    if (Pressed(kButtonPush) && now >= kit.summonReady && !kit.summon)
        kit.summon = true;
    // the burst
    float& meter = At<float>(character, kCharacterMeter);
    if (Pressed(kButtonCritical) && kit.burstLeft == 0 && meter >= kBurstCost) {
        meter -= kBurstCost;
        kit.burstLeft = kBurstBolts;
        kit.nextBurst = now + 50;
    }
    const bool held = reinterpret_cast<int(__cdecl*)(uint32_t, int)>(uintptr_t(kHeldFrames))(Button(kButtonHeavy), 0) > 0;
    if (kit.burstLeft > 0) {
        if (!kit.shooting)
            kit.nextBurst = std::max(kit.nextBurst, now + 50); // a few frames into the layer
        Layer(kit, true);
        if (now >= kit.nextBurst) {
            Fire(script, instance);
            --kit.burstLeft;
            kit.nextBurst = now + kBurstIntervalMs;
        }
        return;
    }
    if (!held) {
        Layer(kit, false);
        return;
    }
    if (!kit.shooting) {
        Layer(kit, true);
        kit.lastShot = now - kShotIntervalMs + 50;
    }
    if (now - kit.lastShot >= kShotIntervalMs) {
        kit.lastShot = now;
        Fire(script, instance);
    }
}

void CodyFrame()
{
    if (g_Kits.empty() && g_Helpers.empty())
        return;
    const ULONGLONG now = GetTickCount64();
    // his clones' time is up, or his
    for (auto it = g_Helpers.begin(); it != g_Helpers.end();) {
        const auto kit = g_Kits.find(it->master);
        const bool masterGone = kit == g_Kits.end() || !Live(it->master, kit->second.id) ||
            At<float>(it->master, kCharacterHealth) <= 0;
        if (!Live(it->object, it->id)) {
            it = g_Helpers.erase(it);
            continue;
        }
        if (now >= it->until || masterGone) {
            RemoveSpawnedCharacter(it->object);
            it = g_Helpers.erase(it);
            continue;
        }
        ++it;
    }
    for (auto it = g_Kits.begin(); it != g_Kits.end();) {
        uint8_t* cody = it->first;
        Kit& kit = it->second;
        if (!Live(cody, kit.id)) {
            it = g_Kits.erase(it);
            continue;
        }
        if (kit.summon) {
            kit.summon = false;
            kit.summonReady = now + kSummonWaitMs;
            uint8_t* opponent = OpponentOf(cody);
            int made = 0;
            for (int side = -1; side <= 1; side += 2) {
                float m[16];
                std::memcpy(m, cody + kCharacterTransform, sizeof m);
                for (int k = 0; k < 3; ++k) // beside him, a step back
                    m[12 + k] += m[k] * kHelperSide * float(side) - m[8 + k] * 40.0f;
                std::string error;
                uint8_t* clone = SpawnHelper("ICloneTrooper", std::string(), cody, m, error);
                if (!clone) {
                    LOG_WARN("Cody: a clone of his could not come (%s)", error.c_str());
                    continue;
                }
                if (opponent)
                    if (uint8_t* brain = At<uint8_t*>(clone, kCharacterAI))
                        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, uint8_t*)>(uintptr_t(kAISetTarget))(
                            brain, nullptr, opponent);
                g_Helpers.push_back({ clone, At<uint32_t>(clone, 4), cody, now + kHelperLifeMs });
                ++made;
            }
            LOG_INFO("Cody: %d of his clones called in", made);
            // he signals them in: his intro (the attack signal), as the duel plays it
            uint8_t* script = At<uint8_t*>(cody, kCharacterScript);
            uint8_t* list = script ? At<uint8_t*>(script, 4) : nullptr;
            const int count = list ? At<int>(list, 8) : 0;
            uint8_t** items = list ? At<uint8_t**>(list, 0xC) : nullptr;
            for (int i = 0; i < count; ++i) {
                const char* name = items[i] ? At<const char*>(items[i], 0xC) : nullptr;
                if (name && _stricmp(name, "Cody_Intro") == 0) {
                    reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint8_t*, float, int, int, float)>(kPlaySequence)(
                        cody, nullptr, items[i], 0.0f, 0, 0, 1.0f);
                    break;
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
        out.push_back({ h.object, h.master });
    return out;
}

} // namespace swrots::game
