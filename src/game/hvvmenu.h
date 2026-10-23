#pragma once

#include <string>
#include <vector>

namespace swrots::game {

// Heroes vs Villains in the game's own main menu (Multi Player > Heroes vs Villains): the entry opens the Versus
// character select in Heroes vs Villains mode, where the line-up is picked (the port's panel over the screen shows
// it, with the map, lives and health), then the match starts (hvv.h). At every boot, after the image is loaded.
void InstallHvvMenu();

// For the overlay: the line-up being picked on the select screen.
struct HvvPickState {
    bool active = false;             // the select screen is in Heroes vs Villains mode
    std::vector<std::string> picks;  // classes so far: heroes first, then villains (the first is the player's)
    std::string highlighted;         // the class under the cursor
    std::string map;                 // the map, as shown
    bool versus = false;             // the select screen is the game's own Versus (not Heroes vs Villains)
    std::string arena;               // Versus: the arena chosen with LB/RB (empty: the game's own choice)
    int perSide = 4;                 // fighters a side (the line-up's length is twice that)
    int lives = 0, health = 0;
    bool intro = true;
};
HvvPickState GetHvvPickState();

} // namespace swrots::game
