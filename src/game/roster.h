#pragma once

namespace swrots::game {

// The versus roster beyond the game's nine fighters: extra select-screen slots (from 10; 9 is the
// game's Random) for characters on the disc that were never versus fighters. At every boot.
void InstallRoster();

// Copies the game's nine fighter classes (kDuelistClasses) into the roster's table, which the duel
// reads by slot; after every change to the game's table (the duelist command).
void SyncRosterClasses();

// The duelist whose duel cameras the extra fighter in `slot` uses (e.g. "Anakin", for
// cinematics\introcamera\Anakin_Intro_Cam.cin), or null for a slot that is not an extra fighter's.
const char* ExtraFighterCameras(int slot);
// The character class of a select-screen slot (0-8 the game's fighters, from 10 the extra ones), or null (Random,
// or no such slot).
const char* SelectSlotClass(int slot);

} // namespace swrots::game
