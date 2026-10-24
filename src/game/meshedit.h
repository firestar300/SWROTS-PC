#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace swrots::game {

// Shapes added to a character model (.msh, docs/modding/file-formats.md#character-meshes): plain boxes in a group of
// its body mesh, moving with one bone, painted by one texel of the group's texture.

struct MeshBox {
    float lo[3], hi[3]; // model space, bind pose (+y up, +z front, +x the character's right)
    float texel[2];     // the texel (in a textureSize-square texture) all its faces sample
};

// Adds axis-aligned boxes to group `material` (e.g. "headSG") of the mesh named "Base" of a character model (.msh, the
// whole file), skinned wholly to the group's first palette bone; the shadow mesh is left as it is. False (data unchanged)
// when the model's layout is not one it knows (stride 36, one triangle list in the group, ...). `report` says what was
// done or why not.
bool AddMeshBoxes(std::vector<uint8_t>& data, const char* material, const std::vector<MeshBox>& boxes, int textureSize,
    std::string& report);

} // namespace swrots::game
