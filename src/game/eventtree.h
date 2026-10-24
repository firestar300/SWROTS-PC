#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace swrots::game {

// A character's event tree (meshes\chars\common\s_<class>.xml, compiled .xbl_xml in the PAKs, resource type 31): the
// sounds it loads and what plays when its sequences or animations play (docs/modding/file-formats.md#event-trees).
//   file  = u32 0x18, u32 entry count, entries
//   str   = u32 length, bytes
//   entry = str kind, then
//     CharEvent_LoadSound: str sound id, str file
//     CharEvent_Sequence:  str name, u32 is-animation, u32 event count, events
//   event = str kind, u32 frame, u32 enter, u32 exit, then the kind's fields
//     (CharEvent_PlaySound: str sound id, u8 loops, f32 volume, f32 range; CharEvent_StopSound: str sound id; ...)
// An entry's key: a LoadSound's sound id; a Sequence's "seq:<name>" (looked up by the playing sequence's name) or
// "anim:<name>" (by its animation's). The engine compares names case-insensitively.

// Appends the `donor` entries `want(kind, key)` selects and `base` lacks (same kind and key), byte for byte, and fixes
// the count. False, `base` untouched, when either tree does not parse.
bool AppendEventEntries(std::vector<uint8_t>& base, const std::vector<uint8_t>& donor,
    const std::function<bool(const std::string& kind, const std::string& key)>& want);

// The entries of Vader's tree (s_vader) that make his Force powers' sounds on a character of the Jedi Brute's class,
// which plays the same Force sequences and animations: push, grip, saber throw, heal, lightning, and the sounds they
// load. Not his walk, run, attacks or reactions.
bool VaderForceEntry(const std::string& kind, const std::string& key);

} // namespace swrots::game
