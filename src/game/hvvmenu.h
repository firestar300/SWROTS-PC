#pragma once

#include <string>
#include <vector>

namespace swrots::game {

// Heroes vs Villains in the game's own main menu (Multi Player > Heroes vs Villains): the entry opens the port's
// screens in the game's style (the rules, the line-up, each place picked on the Versus character select), then the
// match starts (hvv.h). At every boot, after the image is loaded.
void InstallHvvMenu();

} // namespace swrots::game
