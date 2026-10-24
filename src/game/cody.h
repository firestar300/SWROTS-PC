#pragma once

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

// Whether saber cuts leave this character's limbs on (Cody).
bool KeepsLimbs(const uint8_t* character);

// Cody's clones now (to mark them), with their master.
struct CodyHelper {
    uint8_t* object;
    uint8_t* master;
};
std::vector<CodyHelper> CodyHelpers();

} // namespace swrots::game
