// Characters of mods' own (see customchars.h).
//
// A class info (what the class registry, 0xE9B60 cdecl (name), returns) is an object whose vtable slot 0x30 creates a
// character of the class (thiscall (), from 0xE2350); the class's constructor sets the character's costume list
// (+0x1E0, a static list per class: a 0x2C-byte header, then 20-byte records {name, mesh "folder\file", ?, code, ?}
// up to a null name; 0x29C000.. the constructors). A custom class's info is a copy of its base's with a vtable of its
// own whose create calls the base's, then gives the character the custom list (its costume first) and its saber
// colour (+0xF20, a pointer to an RGB the saber takes when the character equips it in ICharacter::Init).

#include "game/customchars.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>

#include "core/log.h"
#include "core/patch.h"
#include "game/game.h"
#include "game/menus.h"
#include "kernel/kernel.h"
#include "game/resources.h"

namespace swrots::game {

namespace {

constexpr uint32_t kClassLookup = 0x000E9B60;
constexpr uint8_t kClassLookupBytes[] = { 0xA1, 0xE8, 0xEB, 0x68, 0x00, 0x83, 0xEC, 0x0C }; // mov eax, [0x68EBE8]; sub esp, 0xC
constexpr uint32_t kCreateSlot = 0x30 / 4;
constexpr size_t kInfoCopy = 0x200;     // bytes of the base's class info copied (it is smaller; reads only)
constexpr size_t kVtableCopy = 96;      // slots of its vtable copied
constexpr uint32_t kCharacterSaberColor = 0xF20;

// A Jedi's sabers: the class's Init fills up to 4 weapon records (+0x1064, 0x20 bytes each: +0 double-bladed flag, +4
// the hilt mesh (a TString), +8 its colour) before the Jedi's weapon setup (0x2814C0) makes a saber of each record
// with a mesh (0x280480: the double-bladed class for a flagged one). The Jedi Brute's is double-bladed (Lsaberjdbrute,
// flag 1); Serra Keto's two records make her two sabers. At 0x2814D2 (lea ebx, [esi + 0x1068]; 6 bytes, replaced by a
// call) a custom character's Blades= changes the records first.
constexpr uint32_t kSaberSetup = 0x002814D2;
constexpr uint8_t kSaberSetupBytes[] = { 0x8D, 0x9E, 0x68, 0x10, 0x00, 0x00 };
constexpr uint32_t kWeaponRecords = 0x1064, kWeaponRecordSize = 0x20;
constexpr uint32_t kAssignString = 0x00225A10; // TString, thiscall (const char*)
constexpr char kSingleHilt[] = "Meshes\\Weapons\\LsaberKnight\\LsaberKnight";
constexpr char kDoubleHilt[] = "Meshes\\Weapons\\Lsaberjdbrute\\Lsaberjdbrute";

std::vector<CustomCharacter> g_Customs;
std::wstring g_Mods;

// A custom class's info, for this start's base info.
struct Proxy {
    const CustomCharacter* custom = nullptr;
    uint8_t* baseInfo = nullptr;
    std::unique_ptr<uint8_t[]> info;
    std::unique_ptr<uint32_t[]> vtable;
    std::vector<uint8_t> list;           // the costume list: the custom costume, then the base's
    std::string costumeName, costumeMesh, costumeCode;
    const uint8_t* baseList = nullptr;   // the base's list the copy was made from
};
std::vector<std::unique_ptr<Proxy>> g_Proxies;

using LookupFn = uint8_t*(__cdecl*)(const char*);
LookupFn g_OriginalLookup = nullptr;

Proxy* ProxyOf(const uint8_t* info)
{
    for (auto& p : g_Proxies)
        if (p->info.get() == info)
            return p.get();
    return nullptr;
}

// The custom list from the base's: header, the custom costume, the base's costumes, a null record.
void BuildList(Proxy& p, const uint8_t* baseList)
{
    p.baseList = baseList;
    int count = 0;
    while (baseList && count < 32 &&
        *reinterpret_cast<const char* const*>(baseList + kVariantRecords + count * kVariantRecordSize))
        ++count;
    p.list.assign(kVariantRecords + (count + 2) * kVariantRecordSize, 0);
    if (baseList) {
        std::memcpy(p.list.data(), baseList, kVariantRecords);
        std::memcpy(p.list.data() + kVariantRecords + kVariantRecordSize, baseList + kVariantRecords,
            size_t(count) * kVariantRecordSize);
    }
    uint8_t* first = p.list.data() + kVariantRecords;
    *reinterpret_cast<const char**>(first) = p.costumeName.c_str();
    *reinterpret_cast<const char**>(first + 4) = p.costumeMesh.c_str();
    // the record's code is its dialogue speaker (RegisterSpeaker 0x8D800 takes it: the lines the dialogue engine
    // picks for it, as the game's own codes AKN, OBW...): its own (Code=) or the base's first costume's
    if (p.custom->code.empty() && count > 0)
        p.costumeCode = *reinterpret_cast<const char* const*>(baseList + kVariantRecords + 12);
    *reinterpret_cast<const char**>(first + 12) = p.costumeCode.c_str();
}

// A subtitle's speaker name: the character's (+0xA04, the text of IDS_SPEAKER_<its costume list's name>), set by
// ICharacter::Init (0x15BC56: mov [esi+0xA04], edx, just after its string table lookup). A custom character's list
// keeps the base's name (the duel also finds the intro camera by it), so the store takes its own name instead: the
// text of IDS_SPEAKER_<its key>, which the port's string hook supplies. (Answering it from the getter, 0x151B80, at
// subtitle time, looked it up from there: the game hung.)
constexpr uint32_t kSpeakerNameStore = 0x0015BC56;
constexpr uint8_t kSpeakerNameStoreBytes[] = { 0x89, 0x96, 0x04, 0x0A, 0x00, 0x00 };
constexpr uint32_t kStringCrc = 0x00222BA0;  // cdecl (const char*): a string id's key
constexpr uint32_t kGameCore = 0x00645F7C;   // [+0x38] +0x94: the string tables; vfunc +4 thiscall (out, &key, 0)

// The game's text handles, once a start (cleared at every start: the game's string pool is made anew).
std::vector<std::pair<const CustomCharacter*, uint32_t>> g_SpeakerTexts;

uint32_t SpeakerText(const CustomCharacter& custom)
{
    auto& cache = g_SpeakerTexts;
    for (const auto& c : cache)
        if (c.first == &custom)
            return c.second;
    const std::string id = "IDS_SPEAKER_" + custom.speakerKey;
    uint32_t key = reinterpret_cast<uint32_t(__cdecl*)(const char*)>(uintptr_t(kStringCrc))(id.c_str());
    uint32_t text = 0;
    uint8_t* core = *reinterpret_cast<uint8_t**>(uintptr_t(kGameCore));
    uint8_t* tables = core ? *reinterpret_cast<uint8_t**>(*reinterpret_cast<uint8_t**>(core + 0x38) + 0x94) : nullptr;
    if (tables) {
        using Find = uint32_t*(__fastcall*)(uint8_t*, void*, uint32_t*, uint32_t*, int);
        reinterpret_cast<Find>((*reinterpret_cast<void* const* const*>(tables))[1])(tables, nullptr, &text, &key, 0);
        cache.push_back({ &custom, text });
    }
    return text;
}

// The dialogue engine's start of a line (StartEntry, thiscall (speaker, entry, 0, 0, 0), ret 0x14): its first six
// bytes (push ebp / mov ebp, esp / and esp, -16) moved to a stub.
constexpr uint32_t kStartEntry = 0x00090330;
constexpr uint8_t kStartEntryBytes[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };
using StartEntryFn = int(__fastcall*)(uint8_t*, void*, uint8_t*, int, int, int, int);
StartEntryFn g_StartEntry = nullptr;

// A custom character's own lines: the dialogue engine picks an entry as for its base (its speaker code); for an
// event the character has lines for, the entry's line id (+0x10, a pooled string, e.g. "AKN045") is one of its own
// for the call: the subtitle's text, the voice (the dialogue archive, served with the lines added: see
// BuildVoiceArchive) and the line's length all follow it. The entries: engine +0x118, 0x3C bytes each, +0x14 the
// entry's name, its first three letters the event ("BATAKN___005GEN").
constexpr uint32_t kDialogueEntries = 0x118;
constexpr uint32_t kDialogueEntrySize = 0x3C;
bool g_VoiceServed = false; // this start serves the archive with the custom lines

// The custom line ids as the game's pooled strings, and each event's next line: per start (a reboot makes the game's
// string pool anew).
std::vector<std::pair<std::string, const char*>> g_PooledLines;
std::vector<std::pair<const void*, size_t>> g_LineTurns;

const char* PooledString(const std::string& text)
{
    for (const auto& p : g_PooledLines)
        if (p.first == text)
            return p.second;
    uint8_t* core = *reinterpret_cast<uint8_t**>(uintptr_t(kGameCore));
    if (!core)
        return nullptr;
    const auto acquire = *reinterpret_cast<const char*(__cdecl**)(const char*)>(core + 0xE0);
    const char* pooled = acquire(text.c_str());
    if (pooled)
        g_PooledLines.push_back({ text, pooled });
    return pooled;
}

size_t& LineTurn(const void* lines)
{
    for (auto& t : g_LineTurns)
        if (t.first == lines)
            return t.second;
    g_LineTurns.push_back({ lines, 0 });
    return g_LineTurns.back().second;
}

// StartEntry returns 0 when it refuses the entry (another line is on) and the engine asks again every frame (a saber
// lock's HIT for seconds), else the started or queued line's id: the character's next line is taken only then.
int __fastcall StartEntryHook(uint8_t* engine, void*, uint8_t* speaker, int entry, int a, int b, int c)
{
    const CustomCharacter* custom = speaker && g_VoiceServed ? CustomCharacterOf(speaker) : nullptr;
    if (!custom || custom->voice.empty() || entry < 0)
        return g_StartEntry(engine, nullptr, speaker, entry, a, b, c);
    uint8_t* e = *reinterpret_cast<uint8_t**>(engine + kDialogueEntries) + size_t(entry) * kDialogueEntrySize;
    const char* name = *reinterpret_cast<const char**>(e + 0x14);
    for (const auto& event : custom->voice) {
        if (!name || _strnicmp(name, event.first.c_str(), 3) != 0 || event.second.empty())
            continue;
        size_t& turn = LineTurn(&event.second);
        const char* line = PooledString(event.second[turn % event.second.size()]);
        if (!line)
            break;
        const char*& id = *reinterpret_cast<const char**>(e + 0x10);
        const char* own = id;
        id = line;
        const int result = g_StartEntry(engine, nullptr, speaker, entry, a, b, c);
        id = own;
        if (result)
            ++turn;
        return result;
    }
    return g_StartEntry(engine, nullptr, speaker, entry, a, b, c);
}

// The custom lines' files and texts, all characters'.
struct VoiceLine {
    std::string id;       // upper case, e.g. "LUK003"
    std::wstring file;    // headerless Xbox ADPCM, mono, 22050 Hz (the game's dialogue lines' format)
    std::wstring text;    // its subtitle
};
std::vector<VoiceLine> g_VoiceLines;

uint32_t __cdecl SpeakerNameFor(uint8_t* character, uint32_t text)
{
    const CustomCharacter* custom = CustomCharacterOf(character);
    const uint32_t own = custom ? SpeakerText(*custom) : 0;
    return own ? own : text;
}

void AssignString(uint8_t* string, const char* text)
{
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*, const char*)>(uintptr_t(kAssignString))(string, nullptr, text);
}

void __cdecl ApplyBlades(uint8_t* character)
{
    const CustomCharacter* custom = CustomCharacterOf(character);
    if (!custom)
        return;
    uint8_t* first = character + kWeaponRecords;
    uint8_t* second = first + kWeaponRecordSize;
    // its colour as the records' own too: some moves (a special's power-up) put a saber back to its record's colour,
    // which the +0xF20 override does not cover (the Jedi Brute's is the green key colour)
    if (custom->hasSaber)
        for (int i = 0; i < 2; ++i)
            std::memcpy(first + i * kWeaponRecordSize + 8, custom->saber, sizeof custom->saber);
    // its own hilt (a single blade's: a double-bladed one keeps the double hilt the blades need)
    if (!custom->hilt.empty() && custom->blades != 2 && !first[0])
        AssignString(first + 4, custom->hilt.c_str());
    if (!custom->blades)
        return;
    const char* hilt = *reinterpret_cast<const char**>(first + 4);
    const bool hadDouble = first[0] != 0;
    if (custom->blades == 2) {
        first[0] = 1;
        if (!hadDouble)
            AssignString(first + 4, kDoubleHilt);
        AssignString(second + 4, "");
        return;
    }
    first[0] = 0;
    if (hadDouble || !hilt || !*hilt)
        AssignString(first + 4, kSingleHilt);
    if (custom->blades == 1) {
        AssignString(second + 4, "");
        return;
    }
    second[0] = 0; // dual: a second saber like the first, in the other hand
    AssignString(second + 4, *reinterpret_cast<const char**>(first + 4));
    std::memcpy(second + 8, first + 8, 12);
}

// The Jedi set-up (0x2814C0, thiscall () -> bool: the sabers made, then the Force set to a constant, +0xA40 and its
// most +0xA44; a class whose Init sets no Force of its own after it, as the Jedi Brute, has none): a custom
// character's Force and size after it. Its first instructions: push esi; mov esi, ecx; call 0x2670F0 (relative,
// re-made in the stub).
constexpr uint32_t kJediSetup = 0x002814C0;
constexpr uint8_t kJediSetupBytes[] = { 0x56, 0x8B, 0xF1, 0xE8 };
constexpr uint32_t kJediSetupCallee = 0x002670F0;
constexpr uint32_t kCharacterForce = 0xA40, kCharacterMaxForce = 0xA44, kObjectScale = 0x194;
using JediSetupFn = bool(__fastcall*)(uint8_t*, void*);
JediSetupFn g_OriginalJediSetup = nullptr;

bool __fastcall JediSetupHook(uint8_t* character, void*)
{
    const bool result = g_OriginalJediSetup(character, nullptr);
    if (const CustomCharacter* custom = CustomCharacterOf(character)) {
        if (custom->force >= 0) {
            *reinterpret_cast<float*>(character + kCharacterForce) = custom->force;
            *reinterpret_cast<float*>(character + kCharacterMaxForce) = custom->force;
        }
        if (custom->scale != 1.0f)
            *reinterpret_cast<float*>(character + kObjectScale) = custom->scale;
    }
    return result;
}

// Force controls for a base without them. Every Jedi script class has the Force sequences (the shared Jedi set-up:
// their handles at the same places in every class object, e.g. Force push's at +0x119E), but a class's player rules
// decide what the buttons do: each class's StateManager calls its own (cdecl (script instance, class object)); the
// Jedi Brute's (0x3D9D50, called at 0x3D9D38) has no Force rules. The port's run after it, as Vader's (0x48E5B0:
// inputs 0x48E5C1, gate 0x48EA70, which powers 0x48EB6E, rules 0x48ECB4-0x48F50D): a branch is offered with
// 0x175D60 (cdecl (condition, sequence, 3, 3, 1, 1.0f); the sequence from its handle by 0x1F53E0 (handle, file,
// line)); the second stance (state 0x32, 0x1741A0) has its own set (each handle +2).
constexpr uint32_t kBruteRulesCall = 0x003D9D38;
constexpr uint32_t kBruteRules = 0x003D9D50;
constexpr uint32_t kScriptCharacter = 0x006943CC; // [..] +0x68: the character whose script runs
using RulesFn = void(__cdecl*)(uint8_t*, uint8_t*);

bool Pressed(uint32_t button) // the button id's global; 0x178070 (id, 5, 0, 2)
{
    return reinterpret_cast<bool(__cdecl*)(uint32_t, int, int, int)>(uintptr_t(0x00178070))(
        *reinterpret_cast<const uint32_t*>(uintptr_t(button)), 5, 0, 2);
}

bool Direction(uint32_t test, uint8_t* inst) // 0x1F5440 back, 0x1F5480 left, 0x1F54C0 right on [inst+0xFC]
{
    return reinterpret_cast<bool(__cdecl*)(uint32_t)>(uintptr_t(test))(*reinterpret_cast<uint32_t*>(inst + 0xFC));
}

void Offer(bool condition, uint8_t* classObject, uint32_t handleAt)
{
    const uint16_t handle = *reinterpret_cast<uint16_t*>(classObject + handleAt);
    void* sequence = reinterpret_cast<void*(__cdecl*)(uint32_t, const char*, int)>(uintptr_t(0x001F53E0))(handle,
        "customchars.cpp", __LINE__);
    reinterpret_cast<void(__cdecl*)(bool, void*, int, int, int, float)>(uintptr_t(0x00175D60))(condition, sequence,
        3, 3, 1, 1.0f);
}

void ForceRules(uint8_t* inst, uint8_t* classObject, const CustomCharacter& custom)
{
    const auto call = [](uint32_t f) { return reinterpret_cast<bool(__cdecl*)()>(uintptr_t(f))(); };
    // what the Jedi set-up 0x400EE0 does and the Jedi Brute's Init leaves out: the powers' costs (lightning, throw,
    // push, grip), once; which powers (the Jedi constructor's defaults, or the character's)
    float* costs = reinterpret_cast<float*>(inst + 0x330);
    if (costs[0] == 0 && costs[3] == 0) {
        costs[0] = costs[1] = 100;
        costs[2] = 150;
        costs[3] = 250;
        costs[4] = 100;
        if (custom.forcePowers >= 0) {
            inst[0x2DC] = custom.forcePowers & 1 ? 1 : 0;
            inst[0x2DB] = custom.forcePowers & 2 ? 1 : 0;
            inst[0x2DF] = custom.forcePowers & 4 ? 1 : 0;
            inst[0x2DD] = custom.forcePowers & 8 ? 1 : 0;
            inst[0x2D9] = custom.forcePowers & 16 ? 1 : 0;
        }
    }
    if (call(0x00176E30)) // the computer's character: its Force is the AI's
        return;
    // the gate (nothing else chosen, ...), as Vader's
    if (inst[0x1DA] || call(0x002BD150) || !call(0x002BC160) || inst[0x2F2])
        return;
    const int taps = reinterpret_cast<int(__cdecl*)(uint32_t, int, int, int, int, int)>(uintptr_t(0x001780F0))(
        *reinterpret_cast<const uint32_t*>(uintptr_t(0x006964E8)), 0, 0, 10, 1, 0);
    const bool push = taps >= 1, lightning = Pressed(0x006964F4), saberThrow = Pressed(0x00696500);
    const bool heal = Pressed(0x00696514) && Pressed(0x00696518);
    const bool canPush = inst[0x2DC] != 0, canGrip = inst[0x2DB] != 0, canLightning = inst[0x2DD] != 0;
    const bool canThrow = inst[0x2DF] && !inst[0x2F9], canHeal = inst[0x2D9] != 0;
    if (canGrip) // the grip's target (jedi_functions: 0x426190 (inst, class object, 0x1E0, 24, 350))
        reinterpret_cast<void(__cdecl*)(uint8_t*, uint8_t*, int, float, float)>(uintptr_t(0x00426190))(inst,
            classObject, 0x1E0, 24.0f, 350.0f);
    const uint32_t alt = reinterpret_cast<bool(__cdecl*)(int)>(uintptr_t(0x001741A0))(0x32) ? 2 : 0;
    const bool back = Direction(0x001F5440, inst), left = Direction(0x001F5480, inst), right = Direction(0x001F54C0, inst);
    // each power: back, left, right, then any way
    const auto power = [&](bool trigger, uint32_t backAt, uint32_t leftAt, uint32_t rightAt, uint32_t plainAt) {
        Offer(trigger && back, classObject, backAt + alt);
        Offer(trigger && left, classObject, leftAt + alt);
        Offer(trigger && right, classObject, rightAt + alt);
        Offer(trigger, classObject, plainAt + alt);
    };
    if (canPush)
        power(push, 0x11AA, 0x11A2, 0x11A6, 0x119E);
    if (canHeal)
        Offer(heal, classObject, 0x1248 + alt);
    if (canLightning)
        power(lightning, 0x11FC, 0x11F4, 0x11F8, 0x11F0);
    if (canThrow)
        power(saberThrow, 0x122A, 0x1222, 0x1226, 0x121E);
    if (canGrip) // offered only: the grip comes from holding the push (the push sequence's own logic)
        Offer(false, classObject, 0x11B6 + alt);
}

// The Jedi Brute's script, starting (its Init 0x3DA3E0), sets its character's Force to none (0x177AE0 (0, 0), cdecl
// (force, most) on the script's character [[0x6943CC]+0x68], called at 0x3DA43C): a custom character's Force= instead.
constexpr uint32_t kBruteSetForceCall = 0x003DA43C;
constexpr uint32_t kSetForce = 0x00177AE0;

// Its health, the same way: the Jedi Brute's script sets 220 (0x177AC0 (health): the script's character's vfunc
// +0x380), called at 0x3DA433.
constexpr uint32_t kBruteSetHealthCall = 0x003DA433;
constexpr uint32_t kSetHealth = 0x00177AC0;

void __cdecl BruteSetHealthHook(float health)
{
    __try {
        uint8_t* scripts = *reinterpret_cast<uint8_t**>(uintptr_t(kScriptCharacter));
        uint8_t* character = scripts ? *reinterpret_cast<uint8_t**>(scripts + 0x68) : nullptr;
        const CustomCharacter* custom = character ? CustomCharacterOf(character) : nullptr;
        if (custom && custom->health > 0)
            health = custom->health;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    reinterpret_cast<void(__cdecl*)(float)>(uintptr_t(kSetHealth))(health);
}

void __cdecl BruteSetForceHook(float force, float most)
{
    __try {
        uint8_t* scripts = *reinterpret_cast<uint8_t**>(uintptr_t(kScriptCharacter));
        uint8_t* character = scripts ? *reinterpret_cast<uint8_t**>(scripts + 0x68) : nullptr;
        const CustomCharacter* custom = character ? CustomCharacterOf(character) : nullptr;
        if (custom && custom->force >= 0)
            force = most = custom->force;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    reinterpret_cast<void(__cdecl*)(float, float)>(uintptr_t(kSetForce))(force, most);
}

void __cdecl BruteRulesHook(uint8_t* inst, uint8_t* classObject)
{
    reinterpret_cast<RulesFn>(uintptr_t(kBruteRules))(inst, classObject);
    __try {
        uint8_t* scripts = *reinterpret_cast<uint8_t**>(uintptr_t(kScriptCharacter));
        uint8_t* character = scripts ? *reinterpret_cast<uint8_t**>(scripts + 0x68) : nullptr;
        const CustomCharacter* custom = character ? CustomCharacterOf(character) : nullptr;
        if (custom && custom->forceControls && inst && classObject)
            ForceRules(inst, classObject, *custom);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

__declspec(naked) void SaberSetupHook()
{
    __asm {
        pushad
        push esi
        call ApplyBlades
        add esp, 4
        popad
        lea ebx, [esi + 0x1068]
        ret
    }
}

// At 0x15BC56 (6 bytes, replaced by a call): esi the character, edx its name's text.
__declspec(naked) void SpeakerNameStore()
{
    __asm {
        pushad
        push edx
        push esi
        call SpeakerNameFor
        add esp, 8
        mov [esp + 20], eax // the saved edx
        popad
        mov [esi + 0xA04], edx
        ret
    }
}

// The custom characters made this start (to find a saber's owner; a removed one no longer has a custom list).
std::vector<uint8_t*> g_Made;

// A saber's colour is set through its class's SetColor (vtable +0x604: 0x286180 for the single-bladed classes,
// 0x2864F0 for the double-bladed one), by the equip, the power-up switch and some moves (the Jedi Brute's specials
// light its saber in its own green): a custom character's sabers keep its colour.
constexpr uint32_t kSaberVtables[] = { 0x005B1FF0, 0x005B2620, 0x005B2C90, 0x005B32F8, 0x005B3958 };
constexpr uint32_t kSaberSetColorSlot = 0x604 / 4;
constexpr uint32_t kCharacterWeapons = 0x1080; // 4 slots, 0x20 apart
using SetColorFn = void(__fastcall*)(uint8_t*, void*, const float*);
SetColorFn g_SetColorOriginals[std::size(kSaberVtables)] = {};

const float* OwnColour(uint8_t* weapon)
{
    __try {
        for (uint8_t* c : g_Made) {
            const CustomCharacter* custom = CustomCharacterOf(c);
            if (!custom || !custom->hasSaber)
                continue;
            for (int i = 0; i < 4; ++i)
                if (*reinterpret_cast<uint8_t**>(c + kCharacterWeapons + i * 0x20) == weapon)
                    return custom->saber;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

void __fastcall SaberSetColorHook(uint8_t* weapon, void*, const float* rgb)
{
    const uint32_t vtable = *reinterpret_cast<uint32_t*>(weapon);
    SetColorFn original = nullptr;
    for (size_t i = 0; i < std::size(kSaberVtables); ++i)
        if (kSaberVtables[i] == vtable)
            original = g_SetColorOriginals[i];
    if (!original)
        return;
    if (const float* own = OwnColour(weapon))
        rgb = own;
    original(weapon, nullptr, rgb);
}

uint8_t* __fastcall CreateCustom(uint8_t* info, void*)
{
    Proxy* p = ProxyOf(info);
    if (!p)
        return nullptr;
    using CreateFn = uint8_t*(__fastcall*)(uint8_t*, void*);
    const auto create = reinterpret_cast<CreateFn>((*reinterpret_cast<void* const* const*>(p->baseInfo))[kCreateSlot]);
    uint8_t* character = create(p->baseInfo, nullptr);
    if (!character)
        return nullptr;
    const uint8_t* baseList = *reinterpret_cast<const uint8_t* const*>(character + kCharacterVariants);
    if (p->list.empty() || baseList != p->baseList)
        BuildList(*p, baseList);
    *reinterpret_cast<uint8_t**>(character + kCharacterVariants) = p->list.data();
    if (p->custom->hasSaber)
        *reinterpret_cast<const float**>(character + kCharacterSaberColor) = p->custom->saber;
    g_Made.push_back(character);
    return character;
}

bool CopyInfo(uint8_t* info, uint32_t* vtable, const uint8_t* baseInfo)
{
    __try {
        std::memcpy(info, baseInfo, kInfoCopy);
        std::memcpy(vtable, *reinterpret_cast<void* const*>(baseInfo), kVtableCopy * 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint8_t* __cdecl LookupHook(const char* name)
{
    const CustomCharacter* custom = name ? FindCustomCharacter(name) : nullptr;
    if (!custom)
        return g_OriginalLookup(name);
    uint8_t* baseInfo = g_OriginalLookup(custom->base.c_str());
    if (!baseInfo)
        return nullptr;
    for (auto& p : g_Proxies)
        if (p->custom == custom && p->baseInfo == baseInfo)
            return p->info.get();
    // A copy of the base's info and vtable; the create is the custom one.
    auto p = std::make_unique<Proxy>();
    p->custom = custom;
    p->baseInfo = baseInfo;
    p->info.reset(new uint8_t[kInfoCopy]);
    p->vtable.reset(new uint32_t[kVtableCopy]);
    if (!CopyInfo(p->info.get(), p->vtable.get(), baseInfo))
        return baseInfo;
    p->vtable[kCreateSlot] = uint32_t(reinterpret_cast<uintptr_t>(&CreateCustom));
    *reinterpret_cast<uint32_t**>(p->info.get()) = p->vtable.get();
    p->costumeName = custom->costume;
    p->costumeMesh = custom->mesh;
    p->costumeCode = custom->code;
    LOG_INFO("Custom characters: %s is a %s with %s's body", custom->className.c_str(), custom->base.c_str(),
        custom->mesh.c_str());
    g_Proxies.push_back(std::move(p));
    return g_Proxies.back()->info.get();
}

std::string ProfileIn(const std::wstring& file, const wchar_t* section, const wchar_t* key)
{
    wchar_t value[1024] = {};
    GetPrivateProfileStringW(section, key, L"", value, 1024, file.c_str());
    std::string out;
    for (const wchar_t* c = value; *c; ++c)
        out += char(*c < 128 ? *c : '?');
    return out;
}

std::string Profile(const std::wstring& file, const wchar_t* key, const wchar_t* fallback = L"")
{
    wchar_t value[256] = {};
    GetPrivateProfileStringW(L"Character", key, fallback, value, 256, file.c_str());
    std::string out;
    for (const wchar_t* c = value; *c; ++c)
        out += char(*c < 128 ? *c : '?');
    return out;
}

std::string Upper(std::string text)
{
    for (char& c : text)
        c = char(toupper(static_cast<unsigned char>(c)));
    return text;
}

void ReadCharacters(const std::wstring& mods)
{
    g_Customs.clear();
    g_VoiceLines.clear();
    WIN32_FIND_DATAW found;
    const std::wstring folder = mods + L"\\characters\\";
    HANDLE find = FindFirstFileW((folder + L"*.ini").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        const std::wstring file = folder + found.cFileName;
        CustomCharacter c;
        c.className = Profile(file, L"Class");
        c.base = Profile(file, L"Base");
        c.mesh = Profile(file, L"Mesh");
        if (c.className.empty() || c.base.empty() || c.mesh.empty() || _stricmp(c.className.c_str(), c.base.c_str()) == 0) {
            LOG_WARN("Custom characters: %ls needs Class, Base and Mesh", found.cFileName);
            continue;
        }
        c.name = Upper(Profile(file, L"Name", L""));
        if (c.name.empty())
            c.name = Upper(c.className.size() > 1 && c.className[0] == 'I' ? c.className.substr(1) : c.className);
        c.title = Upper(Profile(file, L"Title"));
        c.costume = Profile(file, L"Costume");
        if (c.costume.empty())
            c.costume = c.className.size() > 1 && c.className[0] == 'I' ? c.className.substr(1) : c.className;
        c.code = Profile(file, L"Code");
        c.speaker = Profile(file, L"Speaker");
        if (c.speaker.empty()) { // the name's first word, as the game's own ("Anakin", "Obi-Wan")
            const std::string name = Profile(file, L"Name");
            c.speaker = name.substr(0, name.find(' '));
            if (c.speaker.empty())
                c.speaker = c.className.size() > 1 && c.className[0] == 'I' ? c.className.substr(1) : c.className;
        }
        c.speakerKey = "PORT_" + Upper(c.className);
        SetMenuText(("IDS_SPEAKER_" + c.speakerKey).c_str(), std::wstring(c.speaker.begin(), c.speaker.end()));
        const std::string saber = Profile(file, L"Saber");
        int r, g, b;
        if (std::sscanf(saber.c_str(), "%d %d %d", &r, &g, &b) == 3) {
            c.hasSaber = true;
            c.saber[0] = r / 255.0f;
            c.saber[1] = g / 255.0f;
            c.saber[2] = b / 255.0f;
        }
        c.side = _stricmp(Profile(file, L"Side", L"Hero").c_str(), "villain") == 0 ? 1 : 0;
        c.scale = std::clamp(float(atof(Profile(file, L"Scale", L"1").c_str())), 0.5f, 2.0f);
        const std::string force = Profile(file, L"Force");
        c.force = force.empty() ? -1.0f : std::clamp(float(atof(force.c_str())), 0.0f, 10000.0f);
        const std::string health = Profile(file, L"Health");
        c.health = health.empty() ? -1.0f : std::clamp(float(atof(health.c_str())), 1.0f, 100000.0f);
        c.forceControls = !Profile(file, L"ForceFrom").empty();
        {
            const std::string powers = Upper(Profile(file, L"ForcePowers"));
            if (!powers.empty()) {
                c.forcePowers = 0;
                static const struct { const char* name; int bit; } kPowers[] = { { "PUSH", 1 }, { "GRIP", 2 },
                    { "GRASP", 2 }, { "SABERTHROW", 4 }, { "LIGHTNING", 8 }, { "HEAL", 16 } };
                for (const auto& p : kPowers)
                    if (powers.find(p.name) != std::string::npos)
                        c.forcePowers |= p.bit;
            }
        }
        c.hilt = Profile(file, L"Hilt");
        if (!c.hilt.empty() && _strnicmp(c.hilt.c_str(), "Meshes", 6) != 0) // "lsaberluke\lsaberluke" alone too
            c.hilt = "Meshes\\Weapons\\" + c.hilt;
        const std::string blades = Profile(file, L"Blades");
        c.blades = _stricmp(blades.c_str(), "single") == 0 ? 1 : _stricmp(blades.c_str(), "double") == 0 ? 2
            : _stricmp(blades.c_str(), "dual") == 0 ? 3 : 0;
        if (!blades.empty() && !c.blades)
            LOG_WARN("Custom characters: %ls: Blades=%s is none of single, double, dual", found.cFileName, blades.c_str());
        std::string costume = c.costume;
        for (char& ch : costume)
            ch = char(tolower(static_cast<unsigned char>(ch)));
        const std::string face = "interfc\\hud_b\\hud_face_" + costume + "01";
        g_Mods = mods; // (ModFileExists)
        if (ModFileExists(face + ".stx")) { // loaded from its file by the HUD (characters.cpp)
            c.face = face;
            c.hudFace = face;
        }
        std::string source;
        for (const wchar_t* ch = found.cFileName; *ch; ++ch)
            source += char(*ch < 128 ? *ch : '?');
        c.source = source;
        // its voice: [Voice] EVENT=line line ..., the lines in characters\<name>\voice\<line>.xadpcm, their
        // texts in [Subtitles] LINE=text
        {
            const std::wstring stem = file.substr(0, file.size() - 4);
            wchar_t keys[4096] = {};
            GetPrivateProfileStringW(L"Voice", nullptr, L"", keys, 4096, file.c_str());
            for (const wchar_t* k = keys; *k; k += wcslen(k) + 1) {
                std::string event;
                for (const wchar_t* ch = k; *ch; ++ch)
                    event += char(toupper(*ch < 128 ? *ch : '?'));
                std::vector<std::string> ids;
                std::istringstream words(ProfileIn(file, L"Voice", k));
                std::string id;
                while (words >> id) {
                    for (char& ch : id)
                        ch = char(toupper(static_cast<unsigned char>(ch)));
                    std::string lower = id;
                    for (char& ch : lower)
                        ch = char(tolower(static_cast<unsigned char>(ch)));
                    const std::wstring path = stem + L"\\voice\\" + std::wstring(lower.begin(), lower.end()) + L".xadpcm";
                    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                        LOG_WARN("Custom characters: %s's line %s has no file", c.className.c_str(), id.c_str());
                        continue;
                    }
                    ids.push_back(id);
                    bool known = false;
                    for (const VoiceLine& v : g_VoiceLines)
                        known |= v.id == id;
                    if (!known) {
                        const std::string text = ProfileIn(file, L"Subtitles", std::wstring(id.begin(), id.end()).c_str());
                        g_VoiceLines.push_back({ id, path, std::wstring(text.begin(), text.end()) });
                    }
                }
                if (!ids.empty())
                    c.voice.push_back({ event, ids });
            }
        }
        LOG_INFO("Custom characters: %s (%s, from %s), from characters\\%s", c.className.c_str(), c.name.c_str(),
            c.base.c_str(), source.c_str());
        g_Customs.push_back(std::move(c));
    } while (FindNextFileW(find, &found));
    FindClose(find);
}

bool ReadWhole(const std::wstring& path, std::vector<uint8_t>& data)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart < (256 << 20);
    if (ok) {
        data.resize(size_t(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(file, data.data(), DWORD(data.size()), &read, nullptr) && read == data.size();
    }
    CloseHandle(file);
    return ok;
}

// The game's generic dialogue archive (Dialogue\gen_dialogue.rpx, mounted in every level) with the custom lines
// added, served in its place for this start. The archive: u32 1, u32 count, 0x18 bytes, then `count` 0x2C-byte
// records {name[0x20] (lower case, sorted), u32 offset, u32 size, u32 (the same in all)}, free room up to the first
// line's data, the lines (2048-aligned). The new records go in their sorted places (the offsets are absolute: no
// line moves), their lines at the end. Rebuilt in cache\voice\ when its inputs change.
void BuildVoiceArchive(const std::wstring& gameData, const std::wstring& cache)
{
    static const std::string kArchive = "\\dialogue\\gen_dialogue.rpx";
    kernel::SetDiscFileOverride(kArchive, L"");
    g_VoiceServed = false;
    if (g_VoiceLines.empty())
        return;
    const std::wstring source = gameData + L"\\Dialogue\\gen_dialogue.rpx";
    const std::wstring folder = cache + L"\\voice", target = folder + L"\\gen_dialogue.rpx";
    // its inputs: the archive's size and every line's file and size
    std::string stamp;
    WIN32_FILE_ATTRIBUTE_DATA info;
    auto describe = [&](const std::wstring& path) {
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
            stamp += std::to_string(info.nFileSizeLow) + ":" + std::to_string(info.ftLastWriteTime.dwLowDateTime) + ";";
    };
    describe(source);
    for (const VoiceLine& v : g_VoiceLines) {
        stamp += v.id + "=";
        describe(v.file);
    }
    std::vector<uint8_t> old;
    if (!(ReadWhole(folder + L"\\stamp.txt", old) && std::string(old.begin(), old.end()) == stamp &&
            GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES)) {
        std::vector<uint8_t> archive;
        if (!ReadWhole(source, archive) || archive.size() < 0x20) {
            LOG_WARN("Custom characters: the dialogue archive is missing; custom voices are not heard");
            return;
        }
        uint32_t count;
        std::memcpy(&count, archive.data() + 4, 4);
        struct Record {
            std::string name;
            uint32_t offset, size, tag;
        };
        std::vector<Record> records;
        uint32_t firstData = uint32_t(archive.size());
        for (uint32_t i = 0; i < count && 0x20 + (i + 1) * 0x2C <= archive.size(); ++i) {
            const uint8_t* r = archive.data() + 0x20 + i * 0x2C;
            Record record{ std::string(reinterpret_cast<const char*>(r), strnlen(reinterpret_cast<const char*>(r), 0x20)) };
            std::memcpy(&record.offset, r + 0x20, 4);
            std::memcpy(&record.size, r + 0x24, 4);
            std::memcpy(&record.tag, r + 0x28, 4);
            firstData = std::min(firstData, record.offset);
            records.push_back(record);
        }
        size_t added = 0;
        for (const VoiceLine& v : g_VoiceLines) {
            std::string name = v.id;
            for (char& ch : name)
                ch = char(tolower(static_cast<unsigned char>(ch)));
            std::vector<uint8_t> line;
            if (name.size() >= 0x20 || !ReadWhole(v.file, line) || line.empty())
                continue;
            if (std::any_of(records.begin(), records.end(), [&](const Record& r) { return _stricmp(r.name.c_str(), name.c_str()) == 0; }))
                continue;
            archive.resize((archive.size() + 2047) & ~size_t(2047), 0);
            records.push_back({ name, uint32_t(archive.size()), uint32_t(line.size()), records.empty() ? 0 : records[0].tag });
            archive.insert(archive.end(), line.begin(), line.end());
            ++added;
        }
        if (0x20 + records.size() * 0x2C > firstData) {
            LOG_WARN("Custom characters: the dialogue archive has no room for %zu more lines", added);
            return;
        }
        archive.resize((archive.size() + 2047) & ~size_t(2047), 0);
        std::sort(records.begin(), records.end(), [](const Record& a, const Record& b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
        const uint32_t total = uint32_t(records.size());
        std::memcpy(archive.data() + 4, &total, 4);
        for (size_t i = 0; i < records.size(); ++i) {
            uint8_t* r = archive.data() + 0x20 + i * 0x2C;
            std::memset(r, 0, 0x20);
            std::memcpy(r, records[i].name.data(), records[i].name.size());
            std::memcpy(r + 0x20, &records[i].offset, 4);
            std::memcpy(r + 0x24, &records[i].size, 4);
            std::memcpy(r + 0x28, &records[i].tag, 4);
        }
        CreateDirectoryW(folder.c_str(), nullptr);
        auto write = [](const std::wstring& path, const void* data, size_t size) {
            HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (f == INVALID_HANDLE_VALUE)
                return false;
            DWORD written = 0;
            const bool ok = WriteFile(f, data, DWORD(size), &written, nullptr) && written == size;
            CloseHandle(f);
            return ok;
        };
        if (!write(target, archive.data(), archive.size()) || !write(folder + L"\\stamp.txt", stamp.data(), stamp.size())) {
            LOG_WARN("Custom characters: the dialogue archive could not be written to the cache");
            return;
        }
        LOG_INFO("Custom characters: the dialogue archive with %zu custom lines built", added);
    }
    kernel::SetDiscFileOverride(kArchive, target);
    g_VoiceServed = true;
    for (const VoiceLine& v : g_VoiceLines) // the subtitles' texts (the dialogue looks a line's id up)
        SetMenuText(v.id.c_str(), v.text);
}

} // namespace

void InstallCustomCharacters(const std::wstring& mods, const std::wstring& gameData, const std::wstring& cache)
{
    g_Proxies.clear(); // class infos are made anew at every start
    g_PooledLines.clear();
    g_LineTurns.clear();
    g_SpeakerTexts.clear();
    g_Mods = mods;
    ReadCharacters(mods);
    BuildVoiceArchive(gameData, cache);
    if (g_Customs.empty())
        return;
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kClassLookup)), kClassLookupBytes, sizeof(kClassLookupBytes)) != 0) {
        LOG_WARN("Custom characters: the class lookup is not as expected; no custom characters");
        g_Customs.clear();
        return;
    }
    // The stub lives in the port's memory and survives reboots; the jump is patched into every new image.
    static uint8_t* stub = [] {
        uint8_t* s = AllocStub(sizeof(kClassLookupBytes) + 5);
        std::memcpy(s, kClassLookupBytes, sizeof(kClassLookupBytes));
        s[sizeof(kClassLookupBytes)] = 0xE9;
        const int32_t rel = int32_t(kClassLookup + sizeof(kClassLookupBytes)) -
            int32_t(uintptr_t(s) + sizeof(kClassLookupBytes) + 5);
        std::memcpy(s + sizeof(kClassLookupBytes) + 1, &rel, 4);
        return s;
    }();
    g_OriginalLookup = reinterpret_cast<LookupFn>(stub);
    PatchJump(kClassLookup, reinterpret_cast<const void*>(&LookupHook));
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kStartEntry)), kStartEntryBytes, sizeof kStartEntryBytes) == 0) {
        static uint8_t* startEntryStub = [] { // once: the port's memory outlives the game's reboots
            uint8_t* stub = AllocStub(sizeof kStartEntryBytes + 5);
            std::memcpy(stub, kStartEntryBytes, sizeof kStartEntryBytes);
            stub[sizeof kStartEntryBytes] = 0xE9;
            const int32_t rel = int32_t(kStartEntry + sizeof kStartEntryBytes) -
                int32_t(uintptr_t(stub) + sizeof kStartEntryBytes + 5);
            std::memcpy(stub + sizeof kStartEntryBytes + 1, &rel, 4);
            return stub;
        }();
        g_StartEntry = reinterpret_cast<StartEntryFn>(startEntryStub);
        PatchJump(kStartEntry, reinterpret_cast<const void*>(&StartEntryHook));
    }
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kSaberSetup)), kSaberSetupBytes, sizeof kSaberSetupBytes) == 0)
        PatchCall(kSaberSetup, reinterpret_cast<const void*>(&SaberSetupHook), sizeof kSaberSetupBytes);
    g_Made.clear();
    for (size_t i = 0; i < std::size(kSaberVtables); ++i) {
        uint32_t* slot = reinterpret_cast<uint32_t*>(uintptr_t(kSaberVtables[i])) + kSaberSetColorSlot;
        const uint32_t hook = uint32_t(reinterpret_cast<uintptr_t>(&SaberSetColorHook));
        if (*slot == hook)
            continue;
        g_SetColorOriginals[i] = reinterpret_cast<SetColorFn>(uintptr_t(*slot));
        PatchBytes(uintptr_t(slot), &hook, 4);
    }
    {
        uint8_t call[5] = { 0xE8 };
        const int32_t rel = int32_t(kSetHealth) - int32_t(kBruteSetHealthCall + 5);
        std::memcpy(call + 1, &rel, 4);
        if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kBruteSetHealthCall)), call, 5) == 0)
            PatchCall(kBruteSetHealthCall, reinterpret_cast<const void*>(&BruteSetHealthHook), 5);
    }
    {
        uint8_t call[5] = { 0xE8 };
        const int32_t rel = int32_t(kSetForce) - int32_t(kBruteSetForceCall + 5);
        std::memcpy(call + 1, &rel, 4);
        if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kBruteSetForceCall)), call, 5) == 0)
            PatchCall(kBruteSetForceCall, reinterpret_cast<const void*>(&BruteSetForceHook), 5);
    }
    {
        uint8_t call[5] = { 0xE8 };
        const int32_t rel = int32_t(kBruteRules) - int32_t(kBruteRulesCall + 5);
        std::memcpy(call + 1, &rel, 4);
        if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kBruteRulesCall)), call, 5) == 0)
            PatchCall(kBruteRulesCall, reinterpret_cast<const void*>(&BruteRulesHook), 5);
    }
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kJediSetup)), kJediSetupBytes, sizeof kJediSetupBytes) == 0) {
        static uint8_t* jediStub = [] { // once: the port's memory outlives the game's restarts
            uint8_t* s = AllocStub(13);
            s[0] = 0x56; // push esi
            s[1] = 0x8B; // mov esi, ecx
            s[2] = 0xF1;
            s[3] = 0xE8; // call 0x2670F0
            const int32_t call = int32_t(kJediSetupCallee) - int32_t(uintptr_t(s) + 8);
            std::memcpy(s + 4, &call, 4);
            s[8] = 0xE9; // jmp back after the call
            const int32_t back = int32_t(kJediSetup + 8) - int32_t(uintptr_t(s) + 13);
            std::memcpy(s + 9, &back, 4);
            return s;
        }();
        g_OriginalJediSetup = reinterpret_cast<JediSetupFn>(jediStub);
        PatchJump(kJediSetup, reinterpret_cast<const void*>(&JediSetupHook));
    }
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kSpeakerNameStore)), kSpeakerNameStoreBytes,
            sizeof kSpeakerNameStoreBytes) == 0)
        PatchCall(kSpeakerNameStore, reinterpret_cast<const void*>(&SpeakerNameStore), sizeof kSpeakerNameStoreBytes);
    else
        LOG_WARN("Custom characters: the subtitles' speaker name is not as expected; custom characters keep the base's");
}

const std::vector<CustomCharacter>& CustomCharacters()
{
    return g_Customs;
}

const CustomCharacter* CustomCharacterOf(const uint8_t* character)
{
    if (!character)
        return nullptr;
    const uint8_t* list = *reinterpret_cast<const uint8_t* const*>(character + kCharacterVariants);
    for (auto& p : g_Proxies)
        if (!p->list.empty() && p->list.data() == list)
            return p->custom;
    return nullptr;
}

bool ModFileExists(const std::string& relative)
{
    if (g_Mods.empty())
        return false;
    const std::wstring path = g_Mods + L"\\" + std::wstring(relative.begin(), relative.end());
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool ReadModFile(const std::string& relative, std::vector<uint8_t>& data)
{
    if (g_Mods.empty())
        return false;
    const std::wstring path = g_Mods + L"\\" + std::wstring(relative.begin(), relative.end());
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart < (16 << 20);
    if (ok) {
        data.resize(size_t(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(file, data.data(), DWORD(data.size()), &read, nullptr) && read == data.size();
    }
    CloseHandle(file);
    return ok;
}

const CustomCharacter* FindCustomCharacter(const char* className)
{
    for (const CustomCharacter& c : g_Customs)
        if (className && _stricmp(c.className.c_str(), className) == 0)
            return &c;
    return nullptr;
}

} // namespace swrots::game
