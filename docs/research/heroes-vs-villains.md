# Heroes vs Villains

A team mode (4 against 4, as in Battlefront II's Heroes vs Villains). A first version (a prototype) is in the
port: on the [developer map](levels.md#the-developer-map), started from the debug console with `hvv`. Addresses
are for the supported retail XBE.

## Playing it

```
hvv                                     the default line-up: Obi-Wan, Yoda, Mace Windu, Cin Drallig
                                        against Vader, Dooku, Grievous, Serra; you play Obi-Wan
hvv IAnakin IObiwan IYoda IMace IVader IDooku IGrievous ISerra player 4 lives 10 health 800
                                        eight classes (heroes, then villains), your place in the line-up
                                        (0-7; 4 is Vader here), each side's lives (35) and every fighter's
                                        health (500)
```

The game restarts into the match, as a level change does: the fighters stand in two lines facing each other in
the middle of the floor, then the fight is on. Each side has its lives: a fighter that falls comes back after its
death has played out (about 4 s), somewhere near the fight but away from everyone, facing it, and cannot be hurt for
2 s. When you fall you come back at once the same way. A side out of lives keeps fighting with whoever is still
standing; when none is left, the other side wins. Your side out of lives and you falling ends the match the game's
way (its death screen, with Restart).

A scoreboard over the picture shows each side's lives and a mark for each fighter, lit while it stands (yours
ringed), and the winner at the end. `restart` (or Restart in the pause menu) plays the same match again.

The launch arguments of a match (`kernel::LaunchGame`, see [starting a level](levels.md#starting-a-level)):
`map:u107_uta_grievoussurvival devmap:1 hvv:<8 classes> hvvplayer:<0-7> hvvlives:<n> hvvhealth:<n>`. They also work
in `mods\Default_Xbox.cfg`.

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
  the health change, 0x151500, and the instant kill, 0x152D30, offer them to the mode first), it keeps a little
  health and is moved to a safe place with full health on the next frame.
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

- The intro: the fighters in their lines, each one's intro, a camera pass (each fighter's Versus intro has a
  camera file, `cinematics\introcamera\<name>_Intro_Cam.cin`).
- The player's HUD health bar reads against another maximum than the mode's health (it shows half full).
- Coming back for the player shows no fall: it is moved at once.
- A camera behind the player (Battlefront style), and a shared camera when two or more play on one screen.
- A match panel in the debug menu (line-up, lives, health); later the game's own menus.
- More than two players: the port's input takes four controllers; the game's player slots and HUD are made for
  two.
