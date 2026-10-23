# Heroes vs Villains

A team mode (4 against 4, as in Battlefront II's Heroes vs Villains). A first version (a prototype) is in the
port: on the [developer map](levels.md#the-developer-map), started from the debug menu's [Heroes vs Villains
tab](../debug-menu.md#heroes-vs-villains-tab) or the console's `hvv`. Addresses are for the supported retail XBE.

## Playing it

**From the main menu**: Multi Player > **Heroes vs Villains** opens the Versus character select in Heroes vs Villains
mode, with a panel over the right half: the line-up so far, the rules and the map. **A** adds the fighter under the
cursor to the line-up (the first is you; then your three fellow heroes, then the four villains; Random picks any;
fighters the profile has not unlocked for Versus can be picked too), **B** takes the last back (with none, back to
the main menu), **Up/Down** set each side's lives (5 to 99), **X** the health (150 to 1000), **Y** the intro,
**LB/RB** (the Xbox pad's White/Black) the map: the developer map's floor or a [custom map](../modding/custom-maps.md).
The eighth pick starts the match.

**From the debug menu or the console:**

```
hvv                                     the default line-up: Obi-Wan, Yoda, Mace Windu, Serra Keto
                                        against Vader, Dooku, Grievous, Anakin; you play Obi-Wan
hvv IAnakin IObiwan IYoda IMace IVader IDooku IGrievous ISerra player 4 lives 10 health 400 intro off
                                        eight classes (heroes, then villains), your place in the line-up
                                        (0-7; 4 is Vader here), each side's lives (35), every fighter's
                                        health (250) and no intro
hvv map cantina                         on a custom map (mods\maps\cantina, see custom maps)
hvv board on|off                        the leaderboard pinned (as holding Tab shows it)
```

The game restarts into the match, as a level change does: the fighters stand in two lines facing each other in
the middle of the floor. **The intro** (about 7 s): for each side a 1.5 s shot of its line from the front, its
fighters playing their Versus intros, then a 1.5 s close shot along the line ("HEROES" / "VILLAINS" with the four
names); then the camera glides back behind you and "FIGHT!". Everyone holds still meanwhile (the game's
`aiDisabled`, your controls held).

At "FIGHT!" every fighter is set on the nearest enemy (and again whenever it has none: one that came back, or
whose enemy fell), so no one stands waiting. Each side has its lives: a fighter that falls comes back after its
death has played out (about 4 s), just outside the fight and facing it, and cannot be hurt for 2 s.

**When you fall** you really die, as in Battlefront: your body plays its death where it fell while the camera goes
from it to look at whoever defeated you (and follows them), then after 4.5 s you come back at a spawn, shielded
for 2 s. A side out of lives keeps fighting with whoever is still standing (when yours has none, the camera follows
a teammate still fighting); when none is left, the other side wins. The game never sees its player die (that would
end the mission with its Game Over screen): see [how it works](#how-it-works).

**Health**: every fighter has the game's 1000, and the damage it takes is scaled to the mode's health (250: four
times): the HUD's health bar is as long as the maximum (`max x constant`, 0x25D1A0), so a lower maximum would show
a stub.

**On screen** (drawn over the picture, in the game's own HUD font, `interfc\fonts\font_large_white`):

- the scoreboard: each side's lives and a mark for each fighter, lit while it stands (yours ringed);
- a marker over each of your teammates (a chevron above the head, smaller far away);
- the kill feed (top right): who defeated whom, or who fell by themselves (names are the characters', yours too,
  your lines marked);
- your notices: "DEFEATED <NAME>" with your killstreak and the points, "ASSIST <NAME>", "DEFEATED BY <NAME>";
- the leaderboard while **Tab** is held: defeats, assists, falls and score of everyone;
- the end: "HEROES WIN" / "VILLAINS WIN", then the top five come to a podium facing the camera, playing their
  intros, with a card each (FIRST to FIFTH, name, score); the game's own HUD is hidden meanwhile.

**Points**: the damage dealt to enemies (in the mode's health), 100 a defeat, 50 an assist, and 25 more for each
defeat in a streak (up to 8). A defeat is the last enemy that hit the fighter within 10 s; the others that hit it
within 15 s assisted. The attacker of a hit is the health change's last argument (0x151500): the character, its
script instance (+0x434), one of its weapons (+0x1080..+0x10A0, its sabers) or a projectile (`IScriptProjectile`)
that points back at it.

`restart` (or Restart in the pause menu) plays the same match again.

The launch arguments of a match (`kernel::LaunchGame`, see [starting a level](levels.md#starting-a-level)):
`map:u107_uta_grievoussurvival devmap:1 hvv:<8 classes> hvvplayer:<0-7> hvvlives:<n> hvvhealth:<n>` (and
`hvvintro:0`, and `custommap:<map>`). They also work in `mods\Default_Xbox.cfg`.

**On a custom map** the lines stand where the map's `HeroesLine` / `VillainsLine` say (the map turned so that they
face each other along +z, as on the floor), `LineGap` apart; fighters come back at the map's `Spawns` (the one
clearest of fighters and nearest the fight), else near the fight within its `Bounds`.

## How it works

- **The menu entry** (`src/game/hvvmenu.cpp`): the main menu (`interfc\front_end\xml\main_menu.xbl_xml`, asked
  for as `main_menu.xml`) has under Multi Player a submenu (`MainMenuSubMenu`, its row count the u32 before its
  first row, then 8 bytes of its own after its last) of `MainMenuSwitchScreenButton` rows: name, position, text id,
  down and up neighbours, the screen to switch to and a button id (Versus 2, Cooperative 3: 3 asks the profile
  whether the co-op missions are open). A copy of Cooperative's row becomes Heroes vs Villains: Versus's screen
  (`select_jedi`) and id, linked between Cooperative and Versus, the count raised. The rows' event handler (vtable
  0x5D5580 slot 4, 0x2CBEC0: event 0x23 "chosen" switches the screen through 0xD4070) tells which row was chosen by
  its item data's +4, the row's name key (the game's string hash, 0x222BA0).
- **The character select** (`JediList`, vtable 0x5D68B8) passes player 1's events to 0x2D51B0 (thiscall (event),
  called from 0x2D54B6): 0x1F/0x20 left/right (the cursor, a slot, +0x2C), 0x21/0x22 up/down, 0x23 A (+0x28 set:
  picked; then player 1 picks the opponent, +0x29), 0x26 B, 0x2B X, 0x2C Y. In Heroes vs Villains mode the port
  takes them first (the call patched). The panel is drawn while the screen is (its vtable slot 3, 0x2D56F0, thiscall
  (one argument), runs every frame).

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
  health. On the next frame the port's live player change (`ReplacePlayer`, here keeping the old body in the level)
  gives the controls, the camera and the level's references to a fresh fighter of its class, placed where it will
  come back, held, shielded and "Ignored By AI" (AI data +0x52). The old body is now just a character: the health
  change deals it a fatal hit naming its killer, and it dies as any fighter does (its death plays, no Game Over).
  The kill camera (the mode's camera adjuster) starts from the game's camera, moves a third of the way to the body
  and a little up (the side the game's camera was on is free of walls; behind the body from the killer, it went
  into them) and turns to the killer; 4.5 s after the fatal hit the fighter comes back.
- **Who hit whom**: the health change's last argument names the attacker (see Points). Some hits name no one
  (null): Force lightning's steady damage and some finishing moves. Those go to the nearest enemy within 450 units,
  one whose AI is set on the victim first (the player counting as one).
- **The intro and the end**: everyone is woken at once and holds still (`aiDisabled`, the options byte +0xC4; the player's input
  held back); the camera is the port's (a camera adjuster, as co-op's shared camera, in its place for the match):
  look-at shots computed from the fighters' transforms; each fighter's intro starts as its close-up does. The titles
  and the scoreboard are drawn by the debug menu's layer over the picture (also when the menu is closed). The font
  sheet is read from the shared PAK and made a texture in the default pool through a system-memory copy: the
  port's device takes no managed textures (D3D9Ex; `D3DERR_INVALIDCALL`), which left the overlay in the menu's
  own font until found. The markers are placed through the game camera's placement and field of view, which is
  **horizontal** (taken as vertical, points off the middle land 16:9 times too near it).
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
- Co-op's player 2 is not part of a match yet (with co-op on, they would join as a companion of their own).
- A camera behind the player (Battlefront style), and a shared camera when two or more play on one screen.
- Maps: the developer map's floor and [custom maps](../modding/custom-maps.md) (boxes for collision only).
- More than two players: the port's input takes four controllers; the game's player slots and HUD are made for
  two.
