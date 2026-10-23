# Custom characters

A mod can add a character of its own: a new character class (for example `ILuke`) that the game creates wherever
it creates characters by name, beside the game's own (nobody is replaced). It shows up in the debug menu's
Characters tab (play as it, spawn it), in the console (`player ILuke`, `spawn ILuke enemy`), in [Heroes vs
Villains](../research/heroes-vs-villains.md) (the main menu's select screen, the debug menu's tab, `hvv`), and a
level can place it as an object.

A custom character has its own **body** (model and textures), **costume name**, **saber colour**, **name** and
**side**. Its **moves, AI, sounds and voice** are those of an existing class, its *base* (for example `IAnakin`):
the game's characters' behaviour is compiled into the game and cannot be written anew, so a new character fights
like the base it names.

## Installing one

A custom character comes as a folder to drop into the game's `mods\` folder:

```
mods\characters\luke.ini                  the character
mods\meshes\chars\luke\luke.msh           its body
mods\meshes\chars\luke\luke_body.stx      its textures (named as the model names them)
mods\meshes\chars\luke\luke_head.stx
```

Nothing else is needed: the port reads `mods\characters\*.ini` at every start (the log says `Custom characters:
ILuke (LUKE SKYWALKER, from IAnakin)`), and the body's textures are loaded with it.

## The character file

`mods\characters\<name>.ini`:

```ini
[Character]
Class=ILuke              ; the new class's name (a class the game does not have)
Name=Luke Skywalker      ; as shown (Heroes vs Villains' titles, scoreboard, kill feed)
Title=Jedi Knight
Base=IAnakin             ; the class whose moves, AI and sounds it has
Costume=Luke             ; its costume's name
Mesh=luke\luke           ; its body, under meshes\chars, without .msh
Saber=0 255 0            ; its saber's colour, 0-255 each (optional; 0 255 0 is the game's own green)
Side=Hero                ; Hero or Villain: its side in Heroes vs Villains by default
Speaker=Luke             ; its name in subtitles (optional; the name's first word)
Code=                    ; whose lines it says, a dialogue speaker code such as AKN or OBW (optional; the base's)

[Voice]                  ; optional: its own lines for some dialogue events (see "Its voice" below)
BAT=luk001 luk002        ; battle cries
TNT=luk003 luk004        ; taunts
KIL=luk008 luk009        ; after a kill

[Subtitles]              ; optional: the texts of its own lines
LUK001=For the Rebellion!
```

## The body

A character body is a model in the game's character format ([file formats](file-formats.md#character-meshes)),
with the base class's skeleton (the same bone names; the human Jedi share one) so that the base's animations play on
it. The port builds its animation binding (`.ban`) the first time it is worn (`cache\disc`). Its textures are STX
files ([replacing textures](replacing-textures.md)), each with its own name inside (offset 0x40) matching the name
the model gives it, in the model's folder.

A body can be tried before it becomes a character: `player IAnakin mesh luke\luke` wears it as Anakin.

Textures made for another game's lighting can look washed out here: the game lights its characters flatly (no
per-pixel lighting or bump maps), so colours that another engine warms or deepens show as they are (Battlefront
II's Luke's hair is an olive grey there, golden in its own lighting); grade them for this game. Where a texture's
alpha cuts a shape out (hair), fill the transparent texels' colour with the nearby opaque colour: the game filters
the texture, and black behind a cut-out shows as a dark fringe.

## How it works

`src/game/customchars.cpp`. The game creates a character of a class through the class registry's lookup (0xE9B60,
`cdecl (name)` -> the class info), whose vtable slot 0x30 creates the character (from 0xE2350); a level's objects
are resolved the same way (0xE9C30). The port answers the custom name with a copy of the base's class info with
a vtable of its own whose create calls the base's, then:

- gives the character the custom **costume list** (+0x1E0: a list per class, set by the class's constructor; a
  0x2C-byte header, then 20-byte records `{name, mesh "folder\file", ?, code, ?}` up to a null name): the custom
  costume first, then the base's, so the character wears its own body by default;
- points its **saber colour** (+0xF20, an RGB the saber takes when the character equips it, in ICharacter::Init)
  at the custom colour.

The character is otherwise the base class (the game names it so: `characters` lists an ILuke as IAnakin), so its
sounds are the base's (and a character of a class placed in the level has its sound set: see
[characters](../research/characters.md)).

A model under `mods\` whose name no level has (a new body) is declared to the level when it is first asked for,
and so are the textures (`.stx`) in its folder: the model loader only asks for the textures the level declared.

## On the Versus select screen

With its select pictures in the pack, a custom character gets a Versus slot of its own (after Yoda's), its name
and title under it, and fights in the duel as itself (also in the main menu's Heroes vs Villains, which picks
fighters on the same screen):

```
mods\interfc\front_end\s_selduel_<costume>head.stx    the grid's cell, 64x64 DXT5 (a ring in its alpha, as the game's)
mods\interfc\front_end\s_selduel_<costume>bust.stx    the bust above the grid, 256x128 DXT5
```

(`<costume>` in lower case, e.g. `s_selduel_lukehead.stx`.) The menus use only pictures read with the shared PAK
(`res_null.pak`; a picture under `mods\` that no PAK has is refused: its pre-cache finds no image, 0x210B6B), so the
port serves the custom pictures in the place of a pair the disc has and never shows (the cut Grievous bodyguard's,
`s_selduel_gbghead` / `gbgbust`): **one custom character on the select screen so far**; others are left off it (the
log says so) and can still be played, spawned and placed.

The duel keeps a slot's fighter for the player whose chosen name equals the fighter's class name (0x27B7D0,
compared at 0x27B8CD and 0x27B983); a custom character's fighter is its base class, so the port compares the chosen
name with the slot's class instead.

## Its HUD portrait

```
mods\interfc\hud_b\hud_face_<costume>01.stx    64x64 DXT5, black where transparent (as the game's faces)
```

The HUD picks a face by the character's class (0x25E9xx, the face's name from the game's portrait table, the
lookup 0x27B740 called at 0x25EADD with the character in `esi`); for a custom character the port hands it the
custom face instead. As with the menus, a texture no PAK has decodes to nothing for the HUD, so the face is served
in the place of one the disc has and never shows (cut Commander Cody's, `hud_face_unlock_cody01`): one custom
character's portrait so far. The served pictures keep the replaced texture's own name inside (offset 0x40).

## Its voice

A character's voice lines are the dialogue engine's (`TDialogueEngine`), not its class's sound set (sabers,
footsteps): each level loads a dialogue database (its own `Dialogue\<mission>_Dialogue`, else `GEN_Dialogue`) of
entries per speaker code and event (BAT battle cry, TNT taunt, KIL kill, FSE first sight of an enemy, HIT hit or
saber lock, DIE death, and grunts), whose lines are in `Dialogue\*.rpx` archives (headerless Xbox ADPCM, mono,
22050 Hz) with their texts in the string tables. A character's speaker code is its costume record's code (+0xC),
taken by RegisterSpeaker (0x8D800) in its Init: a custom character's record has the base's code (or `Code=`), so it
says the base's lines; its name in subtitles (+0xA04, set at 0x15BC56 from `IDS_SPEAKER_<its list's name>`) is
its own (`Speaker=`).

With a `[Voice]` section it says its own lines for those events: the files `mods\characters\<name>\voice\<line>.xadpcm`
(headerless Xbox ADPCM, mono, 22050 Hz, as the game's), their texts in `[Subtitles]`. The port serves the generic
archive (`Dialogue\gen_dialogue.rpx`, mounted in every level) with the lines added (built in `cache\voice\` when
they change) and, when the dialogue engine starts one of the character's entries for such an event (StartEntry,
0x90330), gives the entry the character's line for that call: the voice, the subtitle and the line's length follow
it. Its other sounds (grunts) stay the base's. The lines take turns per event (a turn passes only when the
engine starts or queues one). Subtitles show with the game's subtitles on (the Settings menu, or `set subtitles true`).

## Not yet
- More than one custom character on the Versus select screen and with a HUD portrait (a way to load pictures
  that no PAK has).
