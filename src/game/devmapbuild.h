#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace swrots::game {

// The developer test map (docs/research/levels.md): an empty, lit, flat floor under the sky, made from the bonus
// mission Grievous Survival's level file (levels\u107_uta_grievoussurvival.slp) as the game reads it from its PAK.
// No game code: the level file is read and edited as bytes (the format: tools/levels/slp.py, slpscene.py), with
// the same edits as tools/levels/devmap.py (whose default output it matches byte for byte).

// The level file the dev map is made from.
constexpr const char* kDevMapSource = "levels\\u107_uta_grievoussurvival.slp";
// The model the floor takes the place of (the cave's, whose static entry the dev map keeps).
constexpr const char* kDevMapFloorModel = "meshes\\props\\m07\\cave\\m07_cave.msh";

// Edits `level` (the whole level file) into the dev map. False (level unchanged) when it is not the file expected;
// `report` says what was done or why not.
bool BuildDevMapLevel(std::vector<uint8_t>& level, std::string& report);

// The dev map's floor model, written from scratch: a flat grid tiled with the cave's floor texture.
std::vector<uint8_t> DevMapFloorModel();

} // namespace swrots::game
