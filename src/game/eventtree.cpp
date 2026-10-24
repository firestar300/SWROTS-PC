// Character event trees (see eventtree.h). The grammar is the engine's own readers': LoadSound 0x14CBD0, Sequence
// 0x14D4B0, an event's common part 0x14CC70, PlaySound 0x14CD00, StopSound 0x14CD90; a Sequence entry goes to the
// event processor's table by sequence name (+0x18) or by animation name (+0x1CC) (0x14CC00), both looked up when a
// sequence starts (0x14A440).

#include "game/eventtree.h"

#include <set>

namespace swrots::game {

namespace {

struct Cursor {
    const std::vector<uint8_t>& data;
    size_t at = 0;
    bool ok = true;

    bool Need(size_t n)
    {
        if (!ok || at + n > data.size() || at + n < at)
            ok = false;
        return ok;
    }
    uint32_t U32()
    {
        if (!Need(4))
            return 0;
        const uint32_t v = uint32_t(data[at]) | uint32_t(data[at + 1]) << 8 | uint32_t(data[at + 2]) << 16 |
            uint32_t(data[at + 3]) << 24;
        at += 4;
        return v;
    }
    void Skip(size_t n)
    {
        if (Need(n))
            at += n;
    }
    std::string Str()
    {
        const uint32_t n = U32();
        if (!ok || n > 0x10000 || !Need(n)) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(data.data() + at), n);
        at += n;
        return s;
    }
};

// An event's fields after its frame, enter and exit: 's' a string, '4' four bytes, '1' one byte. PlaySound and
// StopSound are the only kinds the disc's trees use; the others are from the engine's readers.
struct EventLayout {
    const char* kind;
    const char* fields;
};
constexpr EventLayout kEventFields[] = {
    { "CharEvent_PlaySound", "s144" },
    { "CharEvent_StopSound", "s" },
    { "CharEvent_Footstep", "ssss44" },
    { "CharEvent_StopFootstep", "s" },
    { "CharEvent_ExecuteScript", "4s" },
    { "CharEvent_ItemCollision", "4444444" },
    { "CharEvent_ChangeVelocity", "444444" },
    { "CharEvent_ChangeHealth", "4" },
    { "CharEvent_ChangePower", "4" },
    { "CharEvent_SetAnimSpeed", "4" },
    { "CharEvent_AICounterOffer", "s4" },
};

bool SkipEvent(Cursor& c)
{
    const std::string kind = c.Str();
    if (!c.ok)
        return false;
    const char* fields = nullptr;
    for (const EventLayout& l : kEventFields)
        if (kind == l.kind)
            fields = l.fields;
    if (!fields) // a kind of unknown layout: refused, not guessed
        return false;
    c.Skip(12);
    for (const char* f = fields; *f && c.ok; ++f) {
        if (*f == 's')
            c.Str();
        else
            c.Skip(*f == '4' ? 4 : 1);
    }
    return c.ok;
}

struct Entry {
    size_t begin = 0, end = 0;
    std::string kind, key;
};

bool ReadEntry(Cursor& c, Entry& e)
{
    e.begin = c.at;
    e.kind = c.Str();
    if (!c.ok)
        return false;
    if (e.kind == "CharEvent_LoadSound") {
        e.key = c.Str();
        c.Str();
    } else if (e.kind == "CharEvent_Sequence") {
        const std::string name = c.Str();
        const uint32_t isAnimation = c.U32();
        const uint32_t count = c.U32();
        if (!c.ok || count > 0x10000)
            return false;
        e.key = (isAnimation ? "anim:" : "seq:") + name;
        for (uint32_t i = 0; i < count; ++i)
            if (!SkipEvent(c))
                return false;
    } else {
        return false;
    }
    e.end = c.at;
    return c.ok;
}

bool ReadTree(const std::vector<uint8_t>& data, std::vector<Entry>& out)
{
    Cursor c{ data };
    if (c.U32() != 0x18)
        return false;
    const uint32_t count = c.U32();
    if (!c.ok || count > 0x100000)
        return false;
    out.clear();
    for (uint32_t i = 0; i < count; ++i) {
        Entry e;
        if (!ReadEntry(c, e))
            return false;
        out.push_back(std::move(e));
    }
    return c.at == data.size();
}

std::string Lower(std::string s)
{
    for (char& ch : s)
        if (ch >= 'A' && ch <= 'Z')
            ch = char(ch - 'A' + 'a');
    return s;
}

} // namespace

bool AppendEventEntries(std::vector<uint8_t>& base, const std::vector<uint8_t>& donor,
    const std::function<bool(const std::string& kind, const std::string& key)>& want)
{
    std::vector<Entry> baseEntries, donorEntries;
    if (!ReadTree(base, baseEntries) || !ReadTree(donor, donorEntries))
        return false;
    std::set<std::string> have;
    for (const Entry& e : baseEntries)
        have.insert(e.kind + '\n' + Lower(e.key));
    std::vector<uint8_t> extra;
    uint32_t added = 0;
    for (const Entry& e : donorEntries) {
        if (!want(e.kind, e.key) || !have.insert(e.kind + '\n' + Lower(e.key)).second)
            continue;
        extra.insert(extra.end(), donor.begin() + std::ptrdiff_t(e.begin), donor.begin() + std::ptrdiff_t(e.end));
        ++added;
    }
    const uint32_t count = uint32_t(baseEntries.size()) + added;
    base.insert(base.end(), extra.begin(), extra.end());
    for (int i = 0; i < 4; ++i)
        base[4 + i] = uint8_t(count >> (8 * i));
    return true;
}

// Chosen from s_vader against s_jdbrute and both classes' sequence definitions (their Force sequences play the same
// animations): Vader's Force sequences that are no victim's reaction, the animations only those play, and the sounds
// they play that s_jdbrute does not load. Left out: what a Jedi Brute plays itself (RunF, WalkF, Strafe*, Death,
// falls, hits), attacks, saber locks, jumps and the Force run.
bool VaderForceEntry(const std::string& kind, const std::string& key)
{
    static const char* const kLoads[] = {
        "fs_anakin_mtlh_stp", "wep_ls_swing_quick_d", "wep_ls_swing_fast_d", "force_lightning_lp", "force_push",
        "for_force_throw", "for_force_draw", "for_force_heal", "lfe_loop", "lfe_front_wave",
    };
    static const char* const kSequences[] = {
        "seq:forcelightning_loop", "seq:forcelightningb", "seq:forcelightningl", "seq:forcelightningr",
        "seq:forcelightningb_alt", "seq:forcelightningl_alt", "seq:forcelightningr_alt", "seq:forcegrasp_lightning",
        "anim:anakin_frc_saberthrow_in", "anim:anakin_frc_saberthrow_out", "anim:anakin_frc_push",
        "anim:anakin_frc_pushr", "anim:anakin_frc_pushl", "anim:anakin_frc_pushb", "anim:anakin_frc_push_alt",
        "anim:anakin_frc_pushl_alt", "anim:anakin_frc_pushr_alt", "anim:anakin_frc_pushb_alt",
        "anim:anakin_frc_grab_in", "anim:anakin_frc_grab_in_alt", "anim:anakin_nav_altstalkf_frcgrab",
        "anim:anakin_frc_grab_throwf", "anim:anakin_frc_grab_throwb", "anim:anakin_frc_grab_throwl",
        "anim:anakin_frc_grab_throwr", "anim:anakin_frc_pushcustom_l", "anim:anakin_frc_pushcustom_r",
        "anim:anakin_frc_pushcustom_altl", "anim:anakin_frc_pushcustom_altr", "seq:forceheal_loop",
        "anim:anakin_frc_heal_out", "seq:forcegrasp_strafei", "seq:forcegrasp_strafel", "seq:forcegrasp_strafer",
        "seq:forcegrasp_strafef", "seq:forcegrasp_strafeb",
    };
    const std::string k = Lower(key);
    if (kind == "CharEvent_LoadSound") {
        for (const char* s : kLoads)
            if (k == s)
                return true;
    } else if (kind == "CharEvent_Sequence") {
        for (const char* s : kSequences)
            if (k == s)
                return true;
    }
    return false;
}

} // namespace swrots::game
