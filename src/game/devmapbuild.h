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

// What a mode puts on the dev map. Characters placed in the level load with it (their classes' sound sets too,
// which characters created later do not get) and wait inactive until the port activates them. Positions are on
// the floor (x, z); `yaw` turns about the up axis (a character faces +z at 0, -z at pi).
struct DevMapCharacter {
    std::string className; // e.g. "IObiwan"
    uint32_t id;           // the object's id, unique in the level (see kDevMapFirstId)
    float x, z, yaw;
};
// A box of collision (a box brush): its centre (y from the floor), half extents and turn about the up axis.
struct DevMapBox {
    float cx, cy, cz, hx, hy, hz, yaw;
};
struct DevMapLayout {
    std::vector<DevMapCharacter> characters;
    bool start = false; // the player's start, else the middle of the floor
    float startX = 0, startZ = 0, startYaw = 0;
    // A map of a mod's own (custommaps.h): its model under meshes\ (folder and name) in the place of the floor's,
    // where it stands on the floor, its vertex count (its baked light), and its collision boxes.
    std::string modelFolder, modelName;
    uint32_t modelVertices = 0;
    float modelX = 0, modelY = 0, modelZ = 0, modelYaw = 0; // modelY: from the floor
    std::vector<DevMapBox> boxes;
    // Its light: the model's ambient (negative: the floor's), its baked light (one colour a vertex, 0xAARRGGBB; with
    // none, white), the characters' ambient (negative: the dev map's) and the point lights' intensity scale.
    float modelAmbient[3] = { -1, -1, -1 };
    std::vector<uint32_t> bakedLight;
    float characterAmbient[3] = { -1, -1, -1 };
    float lightScale = 1.0f;
    // Its music: a track of the game's sound table (e.g. "mus_m13_templebattle01_lp"), played from the start (one of
    // the level's music objects kept and pointed at it); empty: none.
    std::string music;
};
// The ids the dev map's own objects take (the level's run far below).
constexpr uint32_t kDevMapFirstId = 0x00300001;
constexpr uint32_t kDevMapFirstBoxId = 0x00380001; // the collision boxes' brush handles
// The floor's middle and its height (the characters stand there).
constexpr float kDevMapMiddleX = 12750.0f, kDevMapMiddleZ = 8100.0f, kDevMapFloorY = -2557.5f;

// Edits `level` (the whole level file) into the dev map, with `layout`'s characters and start when given. False
// (level unchanged) when it is not the file expected; `report` says what was done or why not.
bool BuildDevMapLevel(std::vector<uint8_t>& level, std::string& report, const DevMapLayout* layout = nullptr);

// The dev map's floor model, written from scratch: a flat grid tiled with the cave's floor texture.
std::vector<uint8_t> DevMapFloorModel();

} // namespace swrots::game
