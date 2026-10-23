#pragma once

#include <string>

#include "game/devmapbuild.h"

namespace swrots::game {

// The developer test map (devmapbuild.h, docs/research/levels.md): Grievous Survival's level made an empty, lit,
// flat floor under the sky, built by the port as the level loads. It is wanted by the launch argument `devmap:1`,
// in the launch data (StartDevMap) or in Default_Xbox.cfg next to `map:u107_uta_grievoussurvival`.

// Each start of the game image: whether this start wants the dev map; registers the level's resource patches.
// Before the port's modes (which read their arguments with BootArgument and lay out the map).
void InstallDevMap(const std::wstring& gameData, const std::wstring& mods);

// True when this start of the game loads the dev map in place of Grievous Survival.
bool DevMapActive();

// A launch argument of this start of the game (`name:value`, from its launch data, else from Default_Xbox.cfg).
bool BootArgument(const char* name, std::string& value);

// What a mode puts on this start's dev map (before the level loads).
void SetDevMapLayout(const DevMapLayout& layout);

// Starts the dev map, with more launch arguments (e.g. a mode's), as a level change does: the game restarts into
// it. Any thread; returns at once.
void StartDevMap(const std::string& more = {});

} // namespace swrots::game
