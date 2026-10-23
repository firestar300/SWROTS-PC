#pragma once

#include <string>

namespace swrots::game {

// The developer test map (devmapbuild.h, docs/research/levels.md): Grievous Survival's level made an empty, lit,
// flat floor under the sky, built by the port as the level loads. It is wanted by the launch argument `devmap:1`,
// in the launch data (StartDevMap) or in Default_Xbox.cfg next to `map:u107_uta_grievoussurvival`.

// Each start of the game image: whether this start wants the dev map; registers the level's resource patches.
void InstallDevMap(const std::wstring& gameData, const std::wstring& mods);

// True when this start of the game loads the dev map in place of Grievous Survival.
bool DevMapActive();

// Starts the dev map (the game restarts into it, as a level change does). Any thread; returns at once.
void StartDevMap();

} // namespace swrots::game
