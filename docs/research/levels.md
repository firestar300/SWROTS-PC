# Levels

How a level is built, from the level file (`levels\<level>.slp`) and the level's other resources, as far as it
is known, toward maps of the port's own (a developer test map first). Worked out on the bonus mission
*Grievous Survival* (`U107_UTA_GrievousSurvival`, a small cave on Utapau), compared with `M08` (Palpatine's
office) and `M09A` (the Jedi Temple). Research in progress.

## The level file (.slp)

A level file starts with `u32 4`, the tag `3DF!`, the version (`0x000A0042` on the retail levels) and the path of
the developers' source file (`C:\SLAYER\EP3\Assets\engine\Levels\U107_UTA_GrievousSurvival.SLS`). Then:

| Part | Contents |
|---|---|
| Sector volumes | one BSP tree per sector, in the sector's own coordinates: the spaces the level is made of |
| **Brushes** | BSP trees in world coordinates, each made of convex volumes and the planes that bound them (normal, distance): **the level's collision** |
| Static mesh instances | a mesh path and its baked lighting (4 bytes per vertex of the mesh) |
| World BSP | the partition of the world into sectors |
| Sector records | `Sect_000`, `Sect_001` with their transforms; portals between them |
| Objects | the placed objects, each a game class with its properties |
| Path nodes | a graph of points at floor height: most likely the AI's navigation |

Objects in Grievous Survival: the cave's pieces (`IStatic`), breakable stalactites (`IBreakable`), the characters
(clone troopers, battle droids), trigger zones (`ITriggerMulti`), scripted sequences and sequencers, cutscenes,
camera controls, sounds, particle effects, screen shakes, tips and bacta tanks, and the managers of statics,
brushes, lights and lens flares.

## What makes a level playable

- **The ground and walls are the brushes, not the models.** In Grievous Survival the floor is one brush whose top is
  at y = -2557.5, exactly where the player stands, and the cave's walls are five more. The cave model only draws:
  lowered by 300 units, the characters still stood on the old floor, in mid-air.
- **The playable space is bounded by the sectors.** Past them a character falls; a bigger map needs bigger sector
  volumes as well as a bigger floor brush.
- **Lighting is baked** into the level file for each vertex of each static model; a model whose vertices change no
  longer matches its lighting (it draws dark).
- **The sky** is a model too (the skybox's dome and clouds).
- **The models and textures** come from the level's PAK (the cave's pieces are `meshes\props\m07\cave\`).

## Toward a developer map

Proven in game (test code, not in the port):

- Widening the floor brush and lifting the wall brushes out of the way, as the level file is read: the player walks
  past where the cave's wall was, at the same floor height.
- A flat version of the cave's model (one large square for its floor): an open floor under the sky; it draws black,
  its lighting no longer matching.

A level file in `mods\levels\` does not load yet (it faults at 0x2120B), so a changed level is served as the game
reads it from the level's PAK.

Open: the objects' layout (to remove the level's enemies, waves and props and find the player's start), the path
nodes (AI navigation on a new floor), a floor model of the port's own with lighting, enlarged sectors, and serving a
whole level file of the port's own.
