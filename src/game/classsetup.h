#pragma once

#include <cstdint>

namespace swrots::game {

// More sequence functions for a class's script. A class's sequences are functions of its script's vtable: the class
// setup driver (0x1F66A0) calls each in setup mode, where each declares itself (SequenceSetup 0x1F5B20: its handle,
// name, animation, ...) and the driver makes a record of it, until the class's end marker. Functions added here are
// called after the class's own, as if the class had them: a record each, the function called again whenever the
// sequence plays (thiscall (class object, a, b) -> bool, as the game's).
using ClassSequenceFn = bool(__fastcall*)(uint8_t* classObject, void*, uint32_t, uint32_t);

// Asked once per class setup with the class's short name (e.g. "JDBrute"): fills `fns` (up to `max`) with the
// functions to add, returns how many.
using ClassSequenceProvider = int (*)(const char* shortName, ClassSequenceFn* fns, int max);

// Every start of the game image; then providers may be added.
void InstallClassSetup();
void AddClassSequences(ClassSequenceProvider provider);

} // namespace swrots::game
