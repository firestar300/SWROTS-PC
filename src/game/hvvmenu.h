#pragma once

#include <string>
#include <vector>

namespace swrots::game {

// Heroes vs Villains in the game's own main menu (Multi Player > Heroes vs Villains): the entry opens the port's
// screens in the game's style (the rules, the line-up, each place picked on the Versus character select), then the
// match starts (hvv.h). At every boot, after the image is loaded.
void InstallHvvMenu();

// For the overlay: the Versus select screen's arena choice.
struct HvvPickState {
    bool versus = false;             // the select screen is the game's own Versus
    std::string arena;               // the arena chosen with LB/RB (empty: the game's own choice)
};
HvvPickState GetHvvPickState();

} // namespace swrots::game
