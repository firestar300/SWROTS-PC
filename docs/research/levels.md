# Levels

How a level is built, from its level file (`levels\<level>.slp`) and the level's other resources, and how a
developer test map was made from one: an empty, lit, flat floor under the sky, where characters spawned with the
debug console fight anywhere. Worked out on the bonus mission *Grievous Survival* (`U107_UTA_GrievousSurvival`, a
small cave on Utapau) and checked against `M08` (Palpatine's office) and `M09A` (the Jedi Temple), with the game's
own level reader traced read by read and in-game A/B tests. Addresses are for the supported retail XBE.

The developer map is part of the port (`devmap` in the debug console, or the debug menu's Game tab): the port
builds it from the level's own file as the level loads (`src/game/devmapbuild.cpp`). The tools in
[`tools/levels/`](../../tools/levels/README.md) read, show and edit level files for research, and make the same map.

## Where a level file lives

A level file is a resource of type 32 inside the level's PAK (`res_<level>.pak`), stored inline (not in the
memory-image block). Two cautions:

- A level file dumped with `DumpResources` is **not complete**: the dumper records only the bytes the game reads,
  and the reader skips (seeks over) properties it does not know. Take the PAK's own copy.
- A level file in `mods\levels\` does not load yet (it faults at 0x2120B). A changed level can be served as the game
  reads it from the PAK, through the port's resource patches (any size).

## The level file

A `TMeshWorld` file (the engine's `SceneMeshWorld.cpp`, read from 0x5D810), in the same archive as the models (see
[file formats](../modding/file-formats.md)): a string is a u32 length and its characters, an array an i32 capacity,
an i32 count and the elements, a bool 4 bytes.

1. **Header:** the tag `3DF!`, the version (`0x000A0042` on the retail levels), the developers' source path
   (`C:\SLAYER\EP3\Assets\engine\Levels\U107_UTA_GrievousSurvival.SLS`), two u32.
2. Two bools: a BSP world (none in the levels built from props) and a scene.
3. **The scene** (0x5C7E0):
   - an array of BSP trees in local coordinates, and a bool for each (their role is not known yet);
   - **the brushes**: an array of BSP trees in world coordinates, **the level's collision**. A tree (0x57840) holds
     convex volumes ("hedrons": faces, vertices, edges, planes), its nodes, its planes (normal and distance, with
     n·p = d) and some extra blocks;
   - **static instances** (0x5AC80): for each static model, an id (the owner's light handle), its mesh path, and
     either a reference (most statics of M08 and M09A) or its **baked lighting**, 4 bytes for each vertex of the
     mesh, with a few more fields;
   - **the world BSP**: the partition of space into the sectors' boxes;
   - **the sectors**: for each, a name (`Sect_000`), a mesh in the model format (the sector's own drawn faces) and
     its **portals** (three points each; a portal to the outside is marked 0xFFFE), then group records, the scene's
     bounds, octrees (blocks tagged `OCT`) and index lists.
4. **The objects:** a version, a u32 and the object count, then each object: its class name (`IStatic`, `IObiwan`),
   its size, its properties and 8 bytes. A property is a name, a size and its data: a group of properties, an array
   of groups, or a plain value (numbers, a colour, a string, a transform: a 3×3 rotation and a position). The game
   skips the properties it does not know, and an object can be removed with the count fixed.
5. **The path system** (`TPathSystem`, 0x984A0): a header of offsets and counts and 20-byte path nodes (a position
   and links); Grievous Survival has six, all in the cave.

The parts after the objects' layout in M08 and M09A (their sectors' meshes) use an older mesh form not parsed yet.

## What each part does

| Part | Does |
|---|---|
| **Brushes** | **The ground and the walls.** The cave's floor is one brush (a slab whose top is at y −2557.5, exactly where the characters stand), its walls five more. **The models only draw:** a cave model lowered by 300 units left the characters standing on the old floor, in mid-air. |
| **Sector box, portals, world BSP, octree** | **The playable space** and what is drawn. The cave's sector is a box; a character leaving the floor falls to its bottom and its sides stop it. **The sky is drawn only through the sector's portals**: the cave had them on three sides, and the fourth showed the plain clear colour. |
| `IStaticManager` | most of the props: for each, its mesh, placement, sector, light handle and an ambient-only light |
| static instance (by light handle) | a static's baked light, per vertex |
| `IStatic`, `IBreakable` | the other props (mesh, transform, light handle); the breakables have "stump" and "chunks" models for when they break |
| `ISectorSkybox`, the skybox's statics, the sky dome | **the sky** |
| `ILevelProperties` | ambient colours, far clip, bloom |
| `ILightManager` (`__lights`: position, range, colour, intensity) | **what lights the characters** |
| `IPointSpawnPlayer` (its transform) | **the player's start** |
| characters, `ICharacterManager` (waves: templates, spawn engines, spawn locations), triggers, sequences, sequencers, directors, cutscenes, camera controls, tips, sounds, effects, pickups | **the mission** |
| `ISectorInfo`, `ILightStatic`, `ILensFlareSlayerManager`, `IBrushManager`, `IInvisibleBrush`, `IMapIndexer`, `ITrivialCharacterPhysicsManager` | managers the level keeps |
| path system | path nodes for the AI. **On open ground the AI does not need them**: with none, spawned enemies still chase and fight anywhere |

A static model needs one attachment record (type 0, 1, 48 zero bytes); without it the level faults while loading
(0x2078FD).

## The developer map

Built from Grievous Survival by the test tools (with the PAK's copy of its level file):

| Step | Change |
|---|---|
| Space | the sector's box stretched to x 1500..24000, z 2700..13500 (22,500 × 10,800 units), with portals on all four sides (the sky everywhere); the world BSP, bounds and octree to match |
| Floor | one floor brush over the whole box; the wall brushes moved far above |
| Floor model | the cave's model replaced by a flat floor made from scratch by the port, tiled with the level's floor texture, lit by ambient light |
| Objects | 17 of the 131 kept: the managers, the lights, the sky, the sector's settings, the level's properties and the player's start; the cave's props, the breakables, every character, the waves, triggers, cutscenes, tips, sounds, effects and pickups removed |
| Light | the 11 point lights moved over the floor (so characters are lit), the ambient raised |
| Start | the player in the middle of the floor |
| Paths | kept (the 6 nodes of the old cave); with none, the AI chases and fights just the same |

The result loads without a fault: an open floor under the sky in every direction, nothing else, the player at the
middle. Characters spawned with the debug console (`spawn IObiwan enemy`, `spawn IVader ally`) fight the player and
each other, and chase the player across the floor, as far as its corners.

## Building it

**In the port** (`src/game/devmap.cpp`, `devmapbuild.cpp`): the launch argument `devmap:1` asks for the developer
map. The `devmap` command (and the debug menu's button) restarts the game with the launch data
`map:u107_uta_grievoussurvival devmap:1`, as the game's own level changes do (see [Starting a
level](#starting-a-level)); `restart` keeps it, since a restart reuses the launch data. The same two lines in
`mods\Default_Xbox.cfg` start the game in it. With the argument, two resource patches change what the level reads from
its PAK:

- `levels\u107_uta_grievoussurvival.slp`: walked as the game reads it, with the edits above made in place or spliced
  in (the floor's baked light, the new portals, the object section written again). Its result is byte for byte
  that of `tools/levels/makedevmap.py` (91,059 bytes from 202,692). A level file that is not the one expected (another
  version, another layout) is left as it is, with a warning in the log.
- `meshes\props\m07\cave\m07_cave.msh`: the flat floor, written from scratch.

Without the argument Grievous Survival is the game's own.

**With the tools:** `python tools/levels/makedevmap.py <GameData folder> <out.slp>` copies the level file out of
`res_u107_uta_grievoussurvival.pak` and edits it (`devmap.py`): the same map, as a file, for research.

## Starting a level

A level change is a reboot of the game into itself with **launch data** (`XLaunchNewImage`, 0x4A1789): a 4 KB page
holding its type (0, the title's own), the title's id (checked by `XGetLaunchInfo`, 0x4A186A), the image's path at +8
(`d:\default.xbe`) and, at +0x400, the title's data. This game's data is a line of launch arguments in
`Default_Xbox.cfg`'s form: at startup (0x6FA31) it is split at spaces and added to the arguments, so `map:<level>`
there starts that level. The game's own level changes write `map:`, `nextmap:`, `previousmap:` and more
(0x136080). The port starts a level the same way (`kernel::LaunchGame`, any arguments).

## Modes and missions from the game's own objects

A level holds no code of its own: its behaviour comes from the game's classes placed in it, with their
properties. With the objects readable and writable, a map can be given the game's own systems. Proven so far: the
player's start (`IPointSpawnPlayer`), the lights, the sky, the managers, and characters fighting on a floor of the
port's own (spawned with the debug console). Seen in Grievous Survival, not tried yet:

- **Characters placed in the level**, which load with it. Characters spawned later have no sound set (their clashes
  and hits are silent, [characters](characters.md)); placed ones should have theirs.
- **Waves:** `ICharacterManager` (templates, spawn engines, spawn locations), how the survival mission sends its
  clones.
- **Scripting:** trigger volumes (`ITriggerMulti`), scripted sequences (`ITriggerSequence`, sequencers,
  directors): "when this happens, do that", as the missions' objectives and events use them.
- **Cameras and cutscenes:** `ICameraControl` with its helper objects (fixed camera angles in an area) and
  `ICutscene`. A level without them, as the developer map, keeps the game's following camera everywhere.
- Sounds and music (`IAudio`), effects (`IParticleSynthEffect`), pickups (`ISmallBactaTank`), breakables, tips,
  screen shakes.

The game's scripts themselves (the classes' behaviour) are compiled into the game and cannot be written anew; what
the objects cannot do, the port's own code can.

## Open

- The roles of the scene's first BSP trees, of some lists and of the objects' last 8 bytes; the path system's full
  layout.
- The older sector mesh form of M08 and M09A.
- Other levels as arenas the same way (their brushes, sectors and objects), e.g. for [Heroes vs
  Villains](heroes-vs-villains.md).
