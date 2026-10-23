# Heroes vs Villains

Research for a team mode (4 against 4, as in Battlefront II's Heroes vs Villains) built on the game's Versus. Only
findings so far: nothing of the mode is in the port yet. Addresses are for the supported retail XBE.

## What was tried

With temporary test code (not committed): characters spawned beside the fighters, on two sides, then each
character's Versus intro played.

- **A bonus mission** (`u112_mus_lava`): Obi-Wan, Mace and Yoda beside the player's Anakin, against Vader, Dooku,
  Grievous and a Jedi Knight. The AI picks targets across the teams on its own and the fight goes on; memory stayed
  at 26 of 128 MB. The level's own enemies and its hints stay.
- **A Versus arena** (`u150_mus_theduel_02`, Mustafar): the same, with the Versus HUD, no level enemies and no
  hints; 23 of 128 MB. The port's `spawn` places a character in front of the player, and Versus has no story
  player, so the test used the first fighter instead (the game manager `[0x7EB964]` +0x2A4 holds slot 0's instance
  id).

## Findings

- **A Versus arena holds a fighter of every class** besides the two fighting: inactive, with no health, far away
  from the platform (the select screen's choices, waiting).
- **Versus player 2's fighter keeps the duel's own teams** (teams 0), so it does not join a side by itself.
- **Every Versus fighter class has an intro sequence**: `Anakin_Intro`, `Obi_Intro`, `Mace_Intro`, `Dooku_Intro`,
  `Vader_Intro`, `Grievous_Intro`, `Serra_Intro`, `Cin_Intro`, `OldObiwan_Intro` (Yoda has none in Versus; his bonus
  mission has a cane-drop intro). A character's sequence is found by name in its list (`[[body(+0x438)+0x70]+4]`:
  +8 count, +0xC items, each with its name at +0xC) and started from outside its script with character vfunc
  +0x410 (hit zone or null, sequence), as `Reaction_Handle` starts a reaction: all of them played at once.
- **Characters added after the level loaded have no sound set** (their saber clashes are silent): the game
  registers a class's sounds only while the level loads ([characters](characters.md)). A team mode should load its
  fighters with the level, as Versus loads its two.

## Open

- Loading eight fighters with the level (as Versus does for two), and their teams.
- The intro: the fighters in two lines, each one's intro, a camera pass (each fighter's Versus intro has a camera
  file, `cinematics\introcamera\<name>_Intro_Cam.cin`).
- Shared lives with respawns, a winner when a side has none left, and a counter on the HUD.
- A camera behind the player (Battlefront style), and a shared camera when two or more play on one screen.
- More than two players: the port's input takes four controllers; the game's player slots and HUD are made for
  two.
