# Replacing weapons

A character's lightsaber is a weapon asset of its own, under `meshes\weapons\<name>\`, separate from the
character's model. Replacing its files under `mods\` replaces the hilt everywhere that saber is used. Worked
example: Anakin's hilt, which the game models on his Episode II saber, replaced with his Episode III hilt
(`tools/weapons/anakin_ep3_hilt.py`).

## Which files a saber uses

Load a level or a versus match with the character, with `[Debug] LogResources=1` in `settings.ini`, and
look for `weapons` in `logs\swrots.log`:

```
Resource type 4:  d:\meshes\weapons\lsaberanakin\lsaberanakin.msh
Resource type 0:  d:\meshes\weapons\lsaberanakin\vosasaber.stx
Resource type 34: d:\meshes\weapons\lsaberanakin\lsaberanakin.gin
Resource type 33: d:\meshes\weapons\lsaberanakin\lsaberanakin.gat
```

| File | Contents |
|---|---|
| `.msh` | the hilt's mesh ([file formats](file-formats.md#static-weapon-meshes)) |
| `.stx` | its texture; the engine finds it by the name stored inside ([replacing textures](replacing-textures.md)) |
| `.gat` | attachment points along the hilt's axis: `ATT_saber_base` (y = 4.27 on `lsaberanakin`), `ATT_saber_middle`, `ATT_sabertip` |
| `.gin` | a bound (`bound_B01`) |

The blade is an effect drawn from the `.gat` points, not part of the mesh. A replacement hilt keeps them by
keeping its emitter at the original's height; the `.gat` and `.gin` files can stay as they are.

**Duelists have two sabers.** The duel arenas load both, and the costume picks one:

| Saber | Used by |
|---|---|
| `lsaberanakin` | Anakin in the story levels, and his hooded costume in versus |
| `lsaberanakinduel` | his default versus costume (`Anakin_Duel`) |
| `lsaberobi`, `lsaberobiduel` | Obi-Wan, the same way |
| `lsaberdooku`, `lsabermace`, `lsaberknight` | Dooku, Mace Windu, Grievous |

A hilt meant for every appearance goes in both of a character's folders. The two `lsaberanakin*` folders
take the same files, the mesh renamed.

In an Anakin against Anakin match, both players show the replaced hilt, and player 2's looks the same as
player 1's: the darker player 2 textures ([the versus
roster](../research/versus-roster.md#player-2-against-itself-sith-yoda)) do not reach the saber.

## Installing

```
mods\meshes\weapons\lsaberanakin\lsaberanakin.msh
mods\meshes\weapons\lsaberanakin\vosasaber.stx
mods\meshes\weapons\lsaberanakinduel\lsaberanakinduel.msh
mods\meshes\weapons\lsaberanakinduel\vosasaber.stx
```

`logs\swrots.log` shows `Mod override:` and `Loose resource:` for each file used. A file listed only as
`Resource type ...` came from the disc: the path under `mods\` does not match (a missing folder, or a name
with a hidden second extension such as `.msh.msh`).

A new mesh usually comes with a new texture layout, so the mesh and the texture go together.

## Tools

In `tools/weapons/`, for the files dumped from your own copy of the game ([dumping assets](dumping-assets.md)):

- `msh.py`: weapon meshes to and from OBJ (`to-obj`, `from-obj`), `info`, and a `selftest` that rebuilds a
  mesh byte for byte and round-trips it through OBJ. In Blender, import and export with the default axes;
  export with normals, UV coordinates and triangulated faces, as one object.
- `stx.py`: DXT1 `.stx` textures to and from PNG. Encoding keeps the template's header and texture name.
- `anakin_ep3_hilt.py`: builds the Episode III hilt and its texture from Anakin's original files and writes
  both of his saber folders under a mods folder:

  ```
  python tools/weapons/anakin_ep3_hilt.py lsaberanakin.msh vosasaber.stx <game folder>\mods
  ```

  The positions of the parts round the hilt are settings at the top of the script. Its proportions were
  measured from a third-party 1:1 printable model (credited in the script); no geometry from it is used.

They need Python 3 with Pillow and NumPy. Share mods made with them as the script and these steps, not as
the generated files: those keep the original files' headers and strings. Post them in
[Discussions](https://github.com/jedijosh920/SWROTS-PC/discussions) (*Show and tell*).

## Limits

- Only static weapon meshes are handled (`lsaberanakin` and `lsaberanakinduel` were tested). Two values in a
  mesh's tail are not understood and are copied from the template.
- `stx.py` handles DXT1 only (format `0x40`, unswizzled blocks, as on `vosasaber.stx`).
- Hilts in the pre-rendered movies are part of the video.
- Size, as found by modders so far (the exact causes are not confirmed yet):
  - keep a hilt close to the size of Anakin's original (3,614 vertices): hilts of 3,611 and 3,872 vertices
    work, while larger first versions crashed the level load (one of 14,236 vertices);
  - a mesh with more than **32,767 indices** crashed the mission on load.
- A loose texture for `lsaberknight` (Serra's and Grievous's saber) crashes the texture loader
  (`C0000005 at 0x21097B`, right after loading `lsaberknight.stx`), so hilts for it reuse the original texture
  for now.
- Some characters share a saber: Vader loads Anakin's (`lsaberanakin`), and Serra and Grievous share
  `lsaberknight`, so a replacement changes all of them.
