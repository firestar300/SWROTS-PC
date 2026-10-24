#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace swrots::game {

// Commander Cody's fighting kit (the built-in fighter ICody, customchars.cpp: a clone trooper), so a blaster can stand
// against a Jedi: he shoots while heavy attack (Y) is held (as co-op's clone), fires a burst on B for a part of his
// meter, rolls aside on block (LT) with the stick left or right instead of blocking, and calls two clones of his
// (RT; marked, for a while, with a wait between) - the computer's Cody calls them too. His limbs stay on.

// Every start of the game image.
void InstallCody();

// Each frame, outside the scripts (the reinforcements come and go here).
void CodyFrame();

// From the clone script's StateManager (coop.cpp's hook), every clone every frame, in its script's turn.
void CloneKitFrame(uint8_t* script, uint8_t* instance, uint8_t* character);

// A health change of a character as it lands (coop.cpp's health hook): none for a Cody in the middle of a roll.
float CodyDamage(uint8_t* character, float change);

// Whether saber cuts leave this character's limbs on (Cody).
bool KeepsLimbs(const uint8_t* character);

// A blast of the game's (IExplosion: an effect, a sound, a growing damage sphere) where `at` is, hurting the enemies of
// `owner` only (not its side in Heroes vs Villains, not its helpers): Cody's shells, Force Storm's strikes. Its first
// effect the level has is shown; false when none is (the blast still hurts).
struct BlastLook {
    const char* const* effects = nullptr; // effect keys (IDS_FX_...), the first the level has
    size_t effectCount = 0;
    const char* sound = "exp_canister_small";
    float radius = 150;
    float push = 200;
    const char* damageType = nullptr;     // r_environment's damage type ("Electrified"), null: an explosion's
    float damage = 0;                     // with a damage type
};
bool FireBlast(uint8_t* owner, const float at[3], const BlastLook& look);
// Who fired a blast (FireBlast's `owner`), or null when `object` is none of them.
uint8_t* BlastOwner(const uint8_t* object);
// The kit fighter a blaster bolt (or anything holding a shooter's script instance) counts for: Cody, the droid
// commander, or the master of one of their reinforcements; null when none.
uint8_t* ShooterOf(const uint8_t* object);
// The master of one of their reinforcements, or null.
uint8_t* HelperMasterOf(const uint8_t* character);

// Cody's clones now (to mark them), with their master.
struct CodyHelper {
    uint8_t* object;
    uint8_t* master;
    bool enemy; // against the player (its Cody the computer's)
    const char* label; // its mark ("CODY'S CLONE")
};
std::vector<CodyHelper> CodyHelpers(); // only those still in the level

} // namespace swrots::game
