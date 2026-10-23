# Custom maps

A mod can add a map of its own: a model with its collision, played on in [Heroes vs
Villains](../research/heroes-vs-villains.md) or walked in with the [developer map](../research/levels.md). The port
builds it on the developer map (Grievous Survival's level made an empty, lit floor as it loads): the map's model takes
the place of the floor's, its collision boxes are added to the level's brushes, and the floor stays under it all.

## Installing one

A custom map comes as a folder to drop into the game's `mods\` folder:

```
mods\maps\cantina\map.ini               the map
mods\meshes\maps\cantina\cantina.msh    its model
mods\meshes\maps\cantina\*.stx          its textures (named as the model names them)
```

The port reads `mods\maps\*\map.ini` at every start (the log says `Custom maps: cantina (MOS EISLEY CANTINA), ...`).
Then:

- **Heroes vs Villains**: on its select screen (Multi Player > Heroes vs Villains) LB / RB (the Xbox pad's White /
  Black) choose the map; in the console `hvv map cantina`.
- **Walking in it**: `devmap cantina` in the console (`devmap` alone: the floor).
- The launch argument `custommap:<map>` next to `devmap:1` (for example in `mods\Default_Xbox.cfg`).

## The map file

`mods\maps\<map>\map.ini`, in the map's own coordinates (the model's: y up, the game's units, about 1 unit to the
centimetre):

```ini
[Map]
Name=Mos Eisley Cantina          ; as shown
Model=maps\cantina\cantina       ; its model under meshes\, without .msh
Vertices=10234                   ; the model's vertex count (all its parts)
Floor=0                          ; the height (y) of its floor: it stands there on the dev map's floor
HeroesLine=0 -350                ; Heroes vs Villains: the middle of the heroes' line (x z)
VillainsLine=0 350               ; and of the villains'; the lines run across the line between them
LineGap=150                      ; between fighters in a line
Spawns=-400 0, 400 0, 0 -600     ; where fighters come back (x z, any number), else near the fight
Bounds=-900 -900 900 900         ; x0 z0 x1 z1: where fighters may come back

[Collision]
Box1=0 100 450 600 100 20 0      ; a box: its centre (x y z), half its size (x y z), its turn (degrees, about y)
Box2=...                         ; numbered from 1, without gaps
```

The map is turned and moved so that the middle between its two lines is the dev map's middle and the heroes face the
villains along +z: the intro's camera, the line-up and the end are then the same as on the floor.

## The model

A model in the game's mesh format ([file formats](file-formats.md)), static (no skeleton), with its textures as STX
files in its folder ([replacing textures](replacing-textures.md)), each with its own name inside (offset 0x40)
matching the name the model gives it. The level lights it with ambient light only (its baked light, one entry per
vertex, is white: hence `Vertices`).

## How it works

`src/game/custommaps.cpp` reads the maps; `src/game/devmapbuild.cpp` builds the level:

- **The model.** The dev map keeps one static mesh entry, the cave's, whose model is the floor's
  (`MGR_StrippedStaticsList`, its `MGR_ManagedMeshID` 6). The model an id names is the static manager's
  `MGR_ManagedMeshData` entry (u32 capacity, u32 count, then per mesh its name and its folder under `meshes\`): the
  port rewrites entry 6 to the map's (name and folder), places the static entry (`MGR_WorldPosition`) where the map
  stands, and gives the static instance as many baked light entries as the model has vertices.
- **The collision.** The level's collision is its brushes (the scene's second list of BSP trees, in world
  coordinates). A box brush is one convex hedron (8 vertices; its edges and faces only index them), 13 nodes (they
  index the planes) and 6 planes (a normal and a distance). Each collision box is a copy of the cave's first wall
  brush, each vertex moved to its corner of the box and each plane turned with it; the brush manager
  (`IBrushManager`) gets a handle per box (`__handledetails`: its id, its class and what it stops: the player,
  characters, projectiles, the camera, ...; a copy of the wall's) and its `__handlecount` grows.
- The loose model and the textures in its folder are declared to the level when the level asks for the model, as
  for a custom character's body.

## Not yet

- Collision other than boxes (a model's own shape: ramps, stairs); fighters stand on the dev map's floor.
- Versus on a custom map.
- The map's own lights and sky.
