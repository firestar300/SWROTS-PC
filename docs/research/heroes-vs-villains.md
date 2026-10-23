# Heroes vs Villains

A team mode (4 against 4, as in Battlefront II's Heroes vs Villains). A first version (a prototype) is in the
port: on the [developer map](levels.md#the-developer-map), started from the debug menu's [Heroes vs Villains
tab](../debug-menu.md#heroes-vs-villains-tab) or the console's `hvv`. Addresses are for the supported retail XBE.

## Playing it

```
hvv                                     the default line-up: Obi-Wan, Yoda, Mace Windu, Cin Drallig
                                        against Vader, Dooku, Grievous, Serra; you play Obi-Wan
hvv IAnakin IObiwan IYoda IMace IVader IDooku IGrievous ISerra player 4 lives 10 health 800 intro off
                                        eight classes (heroes, then villains), your place in the line-up
                                        (0-7; 4 is Vader here), each side's lives (35), every fighter's
                                        health (500) and no intro
```

The game restarts into the match, as a level change does: the fighters stand in two lines facing each other in
the middle of the floor. **The intro** (about 19 s): a crane shot down both lines ("HEROES VS VILLAINS"), then a
close-up of each fighter playing its Versus intro, its name below in its side's colour, then the camera glides back
behind you and "FIGHT!". Everyone holds still meanwhile (the game's `aiDisabled`, your controls held).

Each side has its lives: a fighter that falls comes back after its death has played out (about 4 s), somewhere near
the fight but away from everyone, facing it, and cannot be hurt for 2 s. When you fall, your fall plays out (3 s,
nothing can hurt you), then a fresh fighter of your class takes over at such a place. A side out of lives keeps
fighting with whoever is still standing; when none is left, the other side wins. Your side out of lives and you
falling ends the match the game's way (its death screen, with Restart).

A scoreboard over the picture shows each side's lives and a mark for each fighter, lit while it stands (yours
ringed), and the winner at the end. `restart` (or Restart in the pause menu) plays the same match again.

The launch arguments of a match (`kernel::LaunchGame`, see [starting a level](levels.md#starting-a-level)):
`map:u107_uta_grievoussurvival devmap:1 hvv:<8 classes> hvvplayer:<0-7> hvvlives:<n> hvvhealth:<n>` (and
`hvvintro:0`). They also work in `mods\Default_Xbox.cfg`.

## How it works

- **The fighters are placed in the level** (`src/game/hvv.cpp`, `devmapbuild.cpp`): the dev map's level file gets a
  character object for each AI fighter (class, id, name, transform: the same few properties as the level's own
  characters) and the player's start at the player's place. The player's class is the port's player choice for that
  start (given back afterwards).
- **Placed characters load with the level, whatever their class**: classes the level never had (Vader, Dooku, Mace
  Windu, Yoda and Anakin in Grievous Survival) are loaded from the other levels' files by the port's resource
  loading, as spawned ones are.
- **They have their sounds.** The game registers a class's sound set only while the level loads (see
  [characters](characters.md)); placed characters are there then, so each class gets its own (saber clashes and
  hits are heard). A character of such a class created later, as a fighter coming back, takes the same set.
- **They wait inactive** (not drawn, no health, no AI) until something wakes them, as the Versus arenas' spare
  fighters do. The port wakes them 1.5 s after the player is in the level with the object's vfunc +0xB4
  (thiscall, no arguments; the same call the game makes on a character it has just created).
- **Sides**: each fighter is put on the player's side or against it, with the port's own team bits and its AI's
  "Target Player" (as `spawn ... ally|enemy` does); the AI then picks targets across the sides by itself.
- **Coming back**: a fallen AI fighter is replaced by a new one of its class (`SpawnCharacterAt`) at a safe place.
  The player must not die (the game ends the mission when it does): its fatal hits are caught (the co-op hooks on
  the health change, 0x151500, and the instant kill, 0x152D30, offer them to the mode first) and it keeps a little
  health. Moving it at once is not enough: a killing blow is often a move both fighters play (a finisher), the
  victim's ending on the ground, and a character kept alive stays lying there. So its fall plays out (3 s,
  invincible), then the port's live player change (`ReplacePlayer`) gives the controls to a fresh fighter of its
  class, which is moved to a safe place.
- **The intro**: everyone is woken at once and holds still (`aiDisabled`, the options byte +0xC4; the player's input
  held back); the camera is the port's (a camera adjuster, as co-op's shared camera, in its place for the match):
  look-at shots computed from the fighters' transforms; each fighter's intro starts as its close-up does. The titles
  and the scoreboard are drawn by the debug menu's layer over the picture (also when the menu is closed).
- **Facing**: a character's transform is rows right, up, forward and position; at yaw 0 (identity) it faces +z.

## What was tried before

With temporary test code: characters spawned beside the fighters, on two sides, then each character's Versus intro
played.

- **A bonus mission** (`u112_mus_lava`): Obi-Wan, Mace and Yoda beside the player's Anakin, against Vader, Dooku,
  Grievous and a Jedi Knight. The AI picks targets across the teams on its own and the fight goes on; memory stayed
  at 26 of 128 MB. The level's own enemies and its hints stay.
- **A Versus arena** (`u150_mus_theduel_02`, Mustafar): the same, with the Versus HUD, no level enemies and no
  hints; 23 of 128 MB. A Versus arena holds a fighter of every class besides the two fighting: inactive, with no
  health, far away from the platform (the select screen's choices, waiting). Versus player 2's fighter keeps the
  duel's own teams (teams 0), so it does not join a side by itself.
- **Every Versus fighter class has an intro sequence**: `Anakin_Intro`, `Obi_Intro`, `Mace_Intro`, `Dooku_Intro`,
  `Vader_Intro`, `Grievous_Intro`, `Serra_Intro`, `Cin_Intro`, `OldObiwan_Intro` (Yoda has none in Versus; his bonus
  mission has a cane-drop intro). A character's sequence is found by name in its list (`[[body(+0x438)+0x70]+4]`:
  +8 count, +0xC items, each with its name at +0xC) and started from outside its script with character vfunc
  +0x410 (hit zone or null, sequence), as `Reaction_Handle` starts a reaction: all of them played at once.

## Open

- The intro's camera is the port's own: each fighter's Versus intro has a camera file
  (`cinematics\introcamera\<name>_Intro_Cam.cin`) the game plays in Versus.
- The player's HUD health bar reads against another maximum than the mode's health (with 300 it shows less than a
  third full). The live player change already clears some of the HUD's state; its bar's maximum is not found yet.
- Co-op's player 2 is not part of a match yet (with co-op on, they would join as a companion of their own).
- The titles use the debug menu's pixel font.
- A camera behind the player (Battlefront style), and a shared camera when two or more play on one screen.
- A match panel in the debug menu (line-up, lives, health); later the game's own menus.
- More than two players: the port's input takes four controllers; the game's player slots and HUD are made for
  two.
