# File formats

What is known and verified in the port. All integers are little-endian.

## PAK entry

A level PAK starts with tables (string table, directories, resource types), followed by the
stream of entries. Each entry starts with this header:

| Offset | Size | Field |
|---|---|---|
| 0x00 | 4 | flag (0/1) |
| 0x04 | 4 | offset: the file offset of this header itself |
| 0x08 | 4 | size of the whole entry (header + inline data) |
| 0x0C | 4 | raw size of the data (the record size the decoder uses) |
| 0x10 | 4 | manual file flag (0/1) |
| 0x14 | 4 | name length *n* |
| 0x18 | *n* | name: relative path, e.g. `meshes\chars\human.xbl_cap` |
| +0 | 4 | flag (0/1) |
| +4 | 4 | segmented (1: data is in a `_segNN.pak`, loaded when precaching) |
| +8 | 4 | type id (0 stx, 4 msh, 10 bnm, ...) |
| +12 | 4 | segment index (-1 if none) |
| +16 | 4 | memory-image offset (-1 if none) |
| +20 | 4 | memory-image size (-1 if none) |

Inline data follows the header. The self-referencing offset field makes entries easy to find by
scanning. Xbox-converted files carry an `xbl_` or `xb_` extension prefix in the PAK
(`human.xbl_cap`, `bonegroups.xbl_xml`, `ep3.xb_wml`), but the engine requests them by their
source name (`human.cap`, `bonegroups.xml`).

## STX texture

| Offset | Size | Field |
|---|---|---|
| 0x00 | 4 | magic `STX\0` |
| 0x0C / 0x10 | 4 + 4 | width, height |
| 0x34 | 4 | BIR block size (file size = 0x104 + this) |
| 0x3C | 1 | format: 0x40 DXT1, 0x52 DXT5, 0x56 BGRA |
| 0x40 | 32 | texture name, zero-padded; **the engine looks textures up by this name** |
| 0x104 | | `BIR\0` block; pixel data starts 0x80 into it |

The port's own texture code (`src/d3d/textures.cpp`) handles the Xbox formats at render time.

On `meshes\weapons\lsaberanakin\vosasaber.stx` (128x128, format 0x40) the DXT1 blocks are stored in
row order, not swizzled; `tools/weapons/stx.py` reads and writes this case. Format 0x52 is DXT5 in row order too
(checked on the HUD font `interfc\fonts\font_large_white.stx`, its glyphs in the alpha, and on character
textures, whose alpha is a gloss mask or, for hair, a cut-out); an earlier version of this table called it DXT3,
swizzled.

## MSH mesh

Texture references are zero-terminated strings relative to the mesh's folder, stored
consecutively, for example:

```
palpatine_grey\0..\..\SharedMaps\SpecularMap\0palpatine_grey\0..\..\SharedMaps\SpecularMap\0
```

Shader-group names follow (`palpatine_greyGeo_palpatineBodySG`). The engine requests
`<mesh folder>\<name>.stx` for each reference, but only if the level's PAK lists that texture.

### Static weapon meshes

The layout of a weapon mesh, from `meshes\weapons\lsaberanakin\lsaberanakin.msh`;
`tools/weapons/msh.py` rebuilds that file byte for byte from it, and the game loads meshes written
this way with other vertex and triangle counts (1084 and 838 against the original's 262 and 202).
Character meshes (skinned, with bones) are below.

| Offset | Size | Field |
|---|---|---|
| 0x064 | 16 | bounding sphere: centre x, y, z and radius (floats) |
| 0x088 | 4 | triangle count |
| 0x114 | 4 | vertex count |
| 0x118 | 36 each | vertices (below) |
| after the vertices | 16 | not understood |
| +16 | 4 + 4 | index count, twice |
| +24 | 2 each | indices (u16), one triangle strip |
| then | | the tail: the strings above, with more fields (below) |

A vertex: position (3 floats), normal (3 unsigned bytes, `b / 127.5 - 1`, and a pad byte), uv
(2 floats), a second uv set (8 bytes, zero on the weapons seen), colour (4 bytes).

The strip joins separate strips with repeated indices: triangles with a repeated index are skipped,
and every odd triangle has its winding reversed. The game is left-handed (z reversed against an
OBJ file).

In the tail: three u16 counts at +0x08 (vertices, triangles, 3 x triangles) and the bounding box at
+0xC2 (min x, y, z, max x, y, z, floats). Two u32 values in it (39 on `lsaberanakin`) are not
understood.

A weapon's other files: `.gat` names its attachment points (`ATT_saber_base`, `ATT_saber_middle`,
`ATT_sabertip` for a lightsaber; the blade starts at `ATT_saber_base`), `.gin` its bound
(`bound_B01`). See [replacing weapons](replacing-weapons.md).

### Character meshes

A character model (`meshes\chars\<folder>\<file>.msh`) is a serialised object tree, read field by field by
the game's own reader (`TMMSHFile`); `tools/meshes/charmsh.py` follows it and rebuilds every character model
tested byte for byte (`python tools/meshes/charmsh.py info <file>` prints the structure). Versions seen:
0x9001D to 0xA0042.

The archive's types: a bool is 4 bytes; a string is a u32 length and its characters; an array is an i32
capacity (stored as it is), an i32 count, then the elements.

| Part | Contents |
|---|---|
| file | u32 magic `F8C203A1`, u32 version, a header (array of {name, array of names}: `Base` lists the model's meshes, e.g. `Base`, `head`, `Shadow_Xbox`), an array of models, then one bool per mesh |
| model | name, bool, array of meshes, array of bones, two 48-byte matrices, an array of bools |
| bone | name (32 bytes), a 48-byte matrix (4 x 3 floats), parent (s16, -1 for the root) |
| mesh | name (32 bytes), centre (3 floats), radius, 4 bools, colour, the mesh's triangle total (i32), bool, the **body**, bounding box (min, max), then a few bools (and an optional extra block) |
| body | bool, an empty legacy array, an array of **groups**, the texture names and the material names (two string tables), an empty array of embedded textures, an array of 60-byte attachments |
| group | u32 flags, colour, 2 floats, u8, the material (u32 shader, 4 layers of {u8, 4 x s16, u32 texture index}), then the render data |
| render data | bool, the vertex format (35 bytes), i32 vertex count, the vertices, an array of triangle lists |
| triangle list | bone palette (array of u16 bone numbers), indices (array of u16, one triangle strip), 2 bools, u16 vertices used, s16 triangle count, u32 |

A string table: u32 total size, u32, u32 bytes used, an array of u32 offsets, then the text (the names,
zero-terminated). Group *n* takes material name *n*; textures are referred to by 1-based index into the
texture table.

**Vertices.** The vertex format's bytes 0x16 to 0x20 hold the offsets of 11 elements (0xFF when absent), and
byte 0x22 the vertex size: 36 bytes for most groups (position 3 floats, normal 4 bytes, uv 2 floats, bone
weights 4 bytes adding up to 255, bone indices 4 bytes, 4 more bytes), 44 with a tangent (normal-mapped
groups), 16 for the shadow mesh (position and normal only). A bone index is the bone's place in the triangle
list's palette **times 3** (a bone takes three shader constants).

**Triangle lists.** Each list takes the next run of the group's vertices and has its own palette: up to
21 bones were seen in one list. The strip joins separate strips with repeated indices, as in the weapon
meshes.

**Pieces.** A group is drawn with its material, and the game can turn a material off for one character:
that is how dismemberment hides a limb. The clone trooper's arms are groups of their own (`Shoulder_LSG`,
`Elbow_RSG`), laid over a body that stays closed underneath; the temple Jedi Knight's model has a left arm
(`Shoulder_LSG`) and a neck (`neckSG`) piece too, made for a dismemberment the game never shipped. The
heroes' models are split by look instead (Anakin's: pants, leather, cloth, cloak, head), and Anakin's bare
left hand is a group named `AnakinHeadSG`.

**Limits.** A list's triangle count is a signed 16-bit number, so a list can hold at most 32,767 triangles;
indices are 16-bit, so a group addresses at most 65,535 vertices. Weapons have a lower limit in practice (see
[replacing weapons](replacing-weapons.md#limits)).

## Character skin table

In the game executable (`.data`): one list per character class, each a 0x2C-byte header and then
20-byte records up to one with a null name.

```c
struct SkinList   { const char* className; uint32_t numbers[4]; const char* textureSets[6]; };
struct SkinRecord { const char* name; const char* meshPath; uint32_t zero; const char* code; uint32_t zero2; };
```

Examples: the `Anakin` list has `{ "Anakin", "Anakin\\Anakin", 0, "AKN", 0 }`,
`{ "Anakin_Duel", "AnakinDuel\\AnakinDuel", ... }`; the `CloneTrooper` list has the texture sets
`"_var01"`, `"_var02"` and `{ "hordeTrooper", "clonetrooper\\hordeTrooper", 0, "CLT", 0 }`;
`{ "Sidious", "sidious\\sidious", ... }` and `{ "Luke", "luke\\luke", ... }` name models that are
not on the disc. The debug console's `variants <class>` prints a class's list. The details, texture
sets included, are in [characters](../research/characters.md).

Forty classes have lists, with about 75 skins between them, including characters used in only a
few levels (Serra, Cin Drallig) and cut ones (Emperor, Luke). Level files (`levels\<level>.slp`) place characters as class instances
(`IVader`, `IOldObiwan`, `IStormtrooper`) with properties such as `Name`, `Transform`, `Player?`
and `Enemies`.

## Event trees

A character's sounds are its event trees: `meshes/chars/common/s_<class>.xml` (compiled `.xbl_xml` in the PAKs,
resource type 31), named by its definition (`meshes/chars/common/<class>`, type 38). A tree lists the sounds the
character loads and what plays when its sequences or animations play. All 30 trees on the disc follow this layout:

```c
file  = u32 0x18, u32 entryCount, entry[entryCount]
str   = u32 length, bytes                         // no terminator
entry = str kind, then
        "CharEvent_LoadSound": str soundId, str file
        "CharEvent_Sequence":  str name, u32 isAnimation, u32 eventCount, event[eventCount]
event = str kind, u32 frame, u32 enter, u32 exit, then
        "CharEvent_PlaySound": str soundId, u8 loops, f32 volume, f32 range
        "CharEvent_StopSound": str soundId
```

A `CharEvent_Sequence` entry with `isAnimation` 0 fires when a sequence of that name plays, with 1 when any sequence
playing that animation does (both are looked up when a sequence starts; names compare case-insensitively). `enter`
events fire as it starts, `exit` events as it ends, others at their frame. The engine knows more event kinds
(footsteps, scripts, item collision, velocity, health and power changes, animation speed); the disc's trees use
only these two.
