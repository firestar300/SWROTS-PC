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

#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>

#include "core/log.h"
#include "core/patch.h"
#include "game/game.h"

namespace swrots::game {

namespace {

constexpr uint32_t kClassLookup = 0x000E9B60;
constexpr uint8_t kClassLookupBytes[] = { 0xA1, 0xE8, 0xEB, 0x68, 0x00, 0x83, 0xEC, 0x0C }; // mov eax, [0x68EBE8]; sub esp, 0xC
constexpr uint32_t kCreateSlot = 0x30 / 4;
constexpr size_t kInfoCopy = 0x200;     // bytes of the base's class info copied (it is smaller; reads only)
constexpr size_t kVtableCopy = 96;      // slots of its vtable copied
constexpr uint32_t kCharacterVariants = 0x1E0;
constexpr uint32_t kCharacterSaberColor = 0xF20;

std::vector<CustomCharacter> g_Customs;

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
    *reinterpret_cast<const char**>(first + 12) = p.costumeCode.c_str();
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
        c.code = Profile(file, L"Code", L"CUS");
        const std::string saber = Profile(file, L"Saber");
        int r, g, b;
        if (std::sscanf(saber.c_str(), "%d %d %d", &r, &g, &b) == 3) {
            c.hasSaber = true;
            c.saber[0] = r / 255.0f;
            c.saber[1] = g / 255.0f;
            c.saber[2] = b / 255.0f;
        }
        c.side = _stricmp(Profile(file, L"Side", L"Hero").c_str(), "villain") == 0 ? 1 : 0;
        std::string source;
        for (const wchar_t* ch = found.cFileName; *ch; ++ch)
            source += char(*ch < 128 ? *ch : '?');
        c.source = source;
        LOG_INFO("Custom characters: %s (%s, from %s), from characters\\%s", c.className.c_str(), c.name.c_str(),
            c.base.c_str(), source.c_str());
        g_Customs.push_back(std::move(c));
    } while (FindNextFileW(find, &found));
    FindClose(find);
}

} // namespace

void InstallCustomCharacters(const std::wstring& mods)
{
    g_Proxies.clear(); // class infos are made anew at every start
    ReadCharacters(mods);
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
}

const std::vector<CustomCharacter>& CustomCharacters()
{
    return g_Customs;
}

const CustomCharacter* FindCustomCharacter(const char* className)
{
    for (const CustomCharacter& c : g_Customs)
        if (className && _stricmp(c.className.c_str(), className) == 0)
            return &c;
    return nullptr;
}

} // namespace swrots::game
