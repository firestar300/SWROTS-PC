# Heroes vs Villains

A team mode (as Battlefront II's Heroes vs Villains): 4 against 4 by default, any size from 1 against 1 to 8 against
8. On the [developer map](levels.md#the-developer-map) or a [custom map](../modding/custom-maps.md), started from the
main menu, the debug menu's [Heroes vs Villains tab](../debug-menu.md#heroes-vs-villains-tab) or the console's `hvv`.
Addresses are for the supported retail XBE.

## Playing it

**From the main menu**: Multi Player > **Heroes vs Villains** opens its screen (as the game's Settings screen): **Team
Size** (1v1 to 5v5), **Lives** (each side's, 5 to 99), **Health** (150 to 1000), **Intro**, **Map** (the developer
map's floor or a [custom map](../modding/custom-maps.md); left/right or A change a value), **Line-up** and **Start**.
Line-up lists the team's places, heroes then villains (Hero 1 is you); choosing one opens the Versus character select,
where A takes the fighter under the cursor for that place (Random picks any; fighters the profile has not unlocked for
Versus can be picked too) and B leaves it as it was. The choices are kept until the game is closed. When a match ends
(the top five, 12 s), the game goes back to the main menu, with your profile.

**From the debug menu or the console:**

```
hvv                                     the default line-up: Obi-Wan, Yoda, Mace Windu, Serra Keto
                                        against Vader, Dooku, Grievous, Anakin; you play Obi-Wan
hvv IAnakin IObiwan IYoda IMace IVader IDooku IGrievous ISerra player 4 lives 10 health 400 intro off
                                        eight classes (heroes, then villains), your place in the line-up
                                        (0-7; 4 is Vader here), each side's lives (35), every fighter's
                                        health (250) and no intro
hvv ILuke IVader                        one against one (any number of classes: heroes, then villains, half
                                        each)
hvv IObiwan IYoda IVader IDooku IGrievous heroes 2
                                        two against three (`heroes`: how many of the classes are heroes)
hvv map cantina                         on a custom map (mods\maps\cantina, see custom maps)
hvv board on|off                        the leaderboard pinned (as holding Tab shows it)
hvv end                                 the running match ends now (the side with more lives wins)
```

The game restarts into the match, as a level change does: the fighters stand in two lines facing each other in
the middle of the floor. **The intro** (about 9 s): for each side a 2.6 s pan along its line from the front,
near, its fighters playing their Versus intros (Yoda, who has none, one of his flavour idles), then a 1.6 s close
shot along the line ("HEROES" / "VILLAINS" with the names); then the camera glides back behind you and "FIGHT!". Everyone holds still meanwhile (the game's
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
times): the HUD's health bar and its frame are as long as the maximum (`max x 1/1500`, 0x25D1A0), so a lower
maximum would show a stub; 1000 matches the Force bar under it. While you are down the game's HUD is hidden.

**Slow motion and the saber lock's camera** are the game's own, made for duels with the player in them: they slow
the whole game and pull the camera. In a match they reach you only from moves you are part of (you do the move,
are its target or are locked in it); settings.ini `[HvV] OthersSlowMotion=1` lets every fighter's through.

**On screen** (drawn over the picture, in the game's own HUD font, `interfc\fonts\font_large_white`):

- the scoreboard: each side's lives and a mark for each fighter, lit while it stands (yours ringed);
- a marker over each of your teammates (a chevron above the head, smaller far away);
- over the fighters' heads (small, fading with distance): a mark of their side, their name, their health and Force
  bars; an enemy's only while in sight (settings.ini `[HvV] OverheadInfo`: 0 off, 1 allies only, 2 everyone);
- the kill feed (top right): who defeated whom, or who fell by themselves (names are the characters', yours too,
  your lines marked);
- your notices: "DEFEATED <NAME>" with your killstreak and the points, "ASSIST <NAME>", "DEFEATED BY <NAME>";
- the leaderboard while **Tab** is held: defeats, assists, falls and score of everyone;
- the end: "HEROES WIN" / "VILLAINS WIN", then the top five stand on a podium facing the camera (first in the
  middle, second and third beside it up front, fourth and fifth further back and outside), playing their intros, with
  a card under each (FIRST to FIFTH, name, score; the top three's cards bigger); the game's own HUD is hidden
  meanwhile;
- while the game is paused, none of the mode's screen is drawn (the Tab leaderboard neither).

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
- **Slow motion and the lock camera**: the moves' scripts slow the game through one call, 0x176BF0 (cdecl (float
  scale, int rampIn, int hold, int rampOut); it skips when the script effects are off, byte 0x6943D0, then jumps to
  0xA20E0, which sets the time manager's ramp; the scale multiplies every frame's time). A slowing holds 9999 frames
  until the move's script restores 1, and there is no stack: anyone's restore ends anyone's slowing. The saber lock's
  camera is "combateffect" (0x2BB250, cdecl, 9 arguments, the first: on), called from the six saber-lock sequence
  sites (0x408B52, 0x408BAE, 0x408DE1, 0x408E27, 0x40907B, 0x4090C1); it frames the player and the instigator's
  target. The instigator is the running script's character ([[0x6943CC] +0x68]); a kill move, grab or lock links two
  fighters ([0x6943CC] +0x23C, their ids at +0xC and +0x10). The port's hooks let an effect start only when the
  player is the instigator, linked to it or its AI's target, and let its end through for whoever started it; a
  slowing whose owner falls is ended (0xA2110). The pause's freeze is a separate stack (`src/game/fixes.cpp`).
- **Fighters that cannot reach their target**: the AI walks straight at its target (the dev map has no paths), so a
  wall, the bar or a platform it cannot step onto stops it. A fighter that stays put for 5 s with its target more
  than 260 units away, or stands near it for 12 s without a blow either way, is moved beside its target to a spot
  clear of the map's boxes (a ring around the target, else the map's spawn nearest it) and set on it again.
- **The player's death**: the fatal hit is kept from the game; on the next frame the old body is moved far out of
  sight (its removal can take a moment, and it played its own fall beside the double's: two deaths), the live player
  change gives the controls to a fresh fighter (the old body removed, as any live change does), placed where it will
  come back, held, shielded and "Ignored By AI" (AI data +0x52). Where the old body fell, a double of it (an ordinary
  spawned character) is dealt the death by the killer and dies there. The kill camera starts from the game's camera
  as the hit landed. Keeping the player's own body in the level to die
  froze the game once (an object list, next pointer +0x24, walked at 0x1F3DB0, went round in a circle).
- **Back to the main menu**: a level change is a reboot; the game keeps the profile across one in `Z:\persist.tmp`
  (`cache\persist.tmp`), saved by 0x24D300 (thiscall on the profile [0x7E8F24]; nothing without the profile's data at
  +0x3C) and read by the next start only with `v_sessionCreated:1`, which also lands the menus on the main menu
  (0x2FAFA0) instead of the title. A match started from the menus saves it and passes `v_sessionCreated:1
  playlicensemovie:0 title:0 controller:<n> memoryunit:<n>` (the session settings [[0x66F7A4] +4]: +0x84, +0x80);
  after the end it saves again and starts `map:` (empty: the menus) with them. A match started from
  Default_Xbox.cfg plays again instead (the file's `map:` would be read again). The port drops its own arguments
  (`devmap:`, `custommap:`, `hvv...`) from every start whose map is not the dev map's level: the game's level
  changes keep every argument but the map, so a Quit from a match would carry them on.
- **Who hit whom**: the health change's last argument names the attacker (see Points). Some hits name no one
  (null): Force lightning's steady damage and some finishing moves. Those go to the nearest enemy within 450 units,
  one whose AI is set on the victim first (the player counting as one).
- **The intro and the end**: everyone is woken at once and holds still (`aiDisabled`, the options byte +0xC4; the player's input
  held back); the camera is the port's (a camera adjuster, as co-op's shared camera, in its place for the match):
  look-at shots computed from the fighters' transforms; each line's intros start as its pan does. A character's
  intro is its sequence named `*_Intro`, started as Versus starts it (see below); one placed above the floor falls, and
  the fall stops the sequence, so the end's characters are set down on the floor itself. **The end** moves every
  character of the match out of sight (fighters, the player, the fallen) and spawns the top five afresh on the podium:
  the fighters themselves brought their fight along (lying from a blow, turned their own way, mid-move). The
  podium stands just in front of the heroes' line (open floor on any map; behind it can be a wall). The titles
  and the scoreboard are drawn by the debug menu's layer over the picture (also when the menu is closed). The font
  sheet is read from the shared PAK and made a texture in the default pool through a system-memory copy: the
  port's device takes no managed textures (D3D9Ex; `D3DERR_INVALIDCALL`), which left the overlay in the menu's
  own font until found. The markers are placed through the game camera's placement and field of view, which is
  **horizontal** (taken as vertical, points off the middle land 16:9 times too near it).
- **Facing**: a character's transform is rows right, up, forward and position; at yaw 0 (identity) it faces +z.
- **The menus** (`hvvmenu.cpp`, `menus.cpp`): the front end (the shell) lists its screens in `gameinfo\shelllist.txt`
  and registers its menu actions at its setup (VaderShell 0x2FA610, a list at +0x268 as the in-game HUD's); the port
  adds two screens there, generated from the front-end Settings screen (`settings.xbl_xml`: nine rows 24 apart from
  y 138, each with a veil behind it): `hvv_setup` (value rows made from the Subtitles row, an On/Off control whose On
  text, the ids pushed at 0x2D8EAB / 0x2D8EFF, the port points at its own text while the row draws; button rows made
  from the Codes row, SettingsScreenButton 0x2D93F0) and `hvv_lineup` (ten button rows). A row's item holds its
  neighbours as the keys of their names (+0x18 up, +0x1C down), which the line-up's rows set as they draw for the
  team size; the rows past the team are not drawn. Screens are changed through the shell's transition object
  (0x2D1B80 on [[0x7F637C] +0xDD0], the name a TString from 0x222FA0). The main menu's Heroes vs Villains row goes to
  `hvv_setup`; the character select, entered from a line-up row, takes A and B for that place (0x2D54B6) and goes
  back to the line-up.

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
  mission has a cane-drop intro). Versus starts one at 0x27B550: `<name>_Intro` (or `_Outro`) found by name in the
  character's script's list (0x150D60: `[[char+0x434]+4]`: +8 count, +0xC items, each with its name at +0xC) and
  played with ICharacter's 0x150E30 (thiscall: sequence, 0, 0, 0, speed 1.0; the Jedi's vfunc +0x314; it refuses
  while +0x1C1 is set). The port does the same. (Vfunc +0x410, tried first, is a reaction handler on the Jedi
  classes: the intro never showed.) The pause: the time manager `[[0x68D944]+0x5C]` has +0x14 set and entries at
  +0x38 only while the pause menu is up.

## Open

- The intro's camera is the port's own: each fighter's Versus intro has a camera file
  (`cinematics\introcamera\<name>_Intro_Cam.cin`) the game plays in Versus.
- Co-op's player 2 is not part of a match yet (with co-op on, they would join as a companion of their own).
- A camera behind the player (Battlefront style), and a shared camera when two or more play on one screen.
- Maps: the developer map's floor and [custom maps](../modding/custom-maps.md) (boxes for collision only; the AI has
  no paths, so fighters that cannot reach their target are moved beside it).
- More than two players: the port's input takes four controllers; the game's player slots and HUD are made for
  two.
