# How it works

SWROTS-PC runs the game's own x86 code natively, inside a normal Windows process. Nothing is
emulated or recompiled: the Xbox had an x86 CPU, so its machine code runs as-is. What does not exist
on a PC is replaced by native code: the Xbox kernel, and the Xbox libraries the game links statically
(Direct3D 8, DirectSound, the controller library).

This page is an overview for the curious and for contributors. Addresses refer to the supported
executable (`default.xbe`, MD5 `6460ef37862de3af364a0563a0a18c4e`).

## Why 32-bit

The game is 32-bit machine code, and a 64-bit process cannot execute it. The port is therefore a
32-bit program. It needs 64-bit Windows, which gives 32-bit programs a 4 GB address space: the Xbox's
"physical" memory is placed at `0x80000000`, which 32-bit Windows reserves for its kernel.

## Startup

1. `swrots.exe` (`src/loader/`) is linked at address `0x10000` with a large empty section, so the
   game's fixed address range (`0x10000`-`0x96E000`) belongs to it before Windows puts anything
   there. It also reserves 128 MiB at `0x80000000` for the Xbox's contiguous memory, then loads
   `core.dll` and hands over. Its own code is overwritten by the game.
2. `core.dll` (`src/core/main.cpp`) reads settings and shows the launcher (`src/launcher/`), which
   installs the game files from the player's disc image (`src/core/install.cpp`) and starts the game in
   a process of its own (`swrots.exe --from-launcher`), so nothing the launcher loaded is in the game's
   address space. That process checks the
   executable's MD5, and copies the game's sections to their addresses. Windows still needs this
   process's PE headers, so they are moved aside and the game's header area points to them.
3. It patches the game where it touches Xbox-only hardware (below), creates the window, and starts
   the game's entry point on a thread set up like an Xbox thread.

## The Xbox kernel (`src/kernel/`)

The game imports about 130 kernel functions. Each is implemented on Windows (`KERNEL_EXPORT`):
files, events, threads, timers, memory, time. Unimplemented imports trap with their name.

- **Threads.** Each game thread gets Xbox per-thread state: a thread structure the game can read, a
  processor control region and thread-local storage laid out as the Xbox does. The few instructions
  that read this through the `FS` segment are patched to calls that fetch our copy.
- **One CPU core.** The Xbox had one core, and the game relies on that: its threads share data
  without the locking a multi-core machine needs. All threads that run game code share one core.
- **IRQL.** Raising the interrupt level to dispatch level is how Xbox code locks out other threads;
  here it takes a process-wide lock.
- **Files.** Xbox paths (`D:\` is the disc, `E:\` the save drive, `Z:\` a cache drive) map to
  `GameData\`, `saves\` and `cache\`. `mods\` overrides any disc file (see [modding](modding/README.md)).
- **Memory.** Contiguous memory is a simple allocator over the reserved region; everything the game
  allocates is tracked so a reboot can release it.

## Rebooting (`src/kernel/reboot.cpp`)

The game restarts itself to change levels, restart a mission or start a new game, as it did on the
Xbox: it "launches" its own executable again, passing a page of launch data. The port does this
inside the same process, so the window stays:

1. Every game thread is stopped. Threads waiting in a kernel function end there (all waits also wait
   for a reboot event). Threads running game code are redirected to exit; they hold no locks of ours.
2. Everything the old instance created is released: sound voices, the graphics device, open handles,
   timers, thread state, drive links, all its memory.
3. The game's sections are copied in again, the patches reinstalled, and the game started with the
   launch data.

If a thread does not stop within a few seconds, the port starts a fresh process instead.

## Xbox libraries (`src/xapi/`, `src/d3d/`, `src/audio/`, `src/input/`)

The game links the Xbox libraries statically, so their code sits inside the executable. A table of
their functions and addresses (`src/game/sdk_symbols.inc`) lets the port replace each one it
implements with a jump to native code (`SDK_REPLACE`). Everything else in those libraries is patched
to trap loudly, except pure computation that is safe to run as-is (`SDK_PASSTHROUGH`).

- **Graphics** (`src/d3d/`): the Xbox's Direct3D 8 on Direct3D 9Ex. The game writes Xbox resource
  headers into its own memory; the port creates matching host textures and buffers, translates the
  Xbox's vertex programs and register combiners (pixel shaders) into HLSL, and draws. The Xbox back
  buffer is an off-screen texture, scaled into the window at the chosen aspect ratio and resolution.
- **Audio** (`src/audio/`): DirectSound buffers and streams on XAudio2, including Xbox ADPCM,
  3D positioning and the game's WMA music, whose decoder runs from the executable.
- **Input** (`src/input/`): the controller library on XInput and HID. Port 0 is the keyboard and
  mouse plus the first controller.
- **Timing**: the executable's clock assumed a 733 MHz CPU; it reads the host clock instead. Vertical
  blanks are a fixed 60 Hz clock.

## The game's own systems (`src/game/`, `src/debug/`)

- `game.h`: facts about the executable (addresses, structure offsets), each with a comment.
- `resources.cpp`, `pak.cpp`: resource loading. Level archives (`.pak`) are read as one stream; the
  port decodes loose replacements instead of the packed copies and keeps the stream in step, loads
  what a level's archive lacks from the other archives on the disc (including animations and manual
  entries), and declares resources a level never listed, so characters from other levels can be
  used. See [how loading works](modding/how-loading-works.md).
- `versus.cpp`: versus mode: which characters the select screen offers (`duelist`), and duel cameras
  for characters that have none.
- `roster.cpp`: versus fighters beyond the game's nine (Yoda): their select-screen cells (pictures
  added to the menu as it loads), names, locks, Random, the duel's per-slot tables, and a player 2
  look (darkened textures) for a fighter against itself. One table
  row per fighter.
- `aliases.cpp`: sequences added to a character class's animation alias list (which of its own
  animations it plays for a sequence it inherits): Yoda's blocks against duelists, which otherwise
  play Anakin's and leave him floating.
- `characters.cpp`: what the player is (`player`, the Characters tab): its class, costume, texture
  set and body, from the game's per-class costume lists; class names the game knows; HUD portraits
  of characters a level did not expect. A change restarts the running mission
  (`kernel::RestartMission`, an in-process reboot with the launch data the game started with). See
  [characters](research/characters.md).
- `devoptions.cpp`: the engine's developer variables (`vars_xbox.cfg`), its hidden debug displays.
- `fixes.cpp`: guards against bugs in the game's own code that the developer tools (or content a
  level was not built with) can trigger, e.g. in rebuilding a body's animation binding for another
  class.
- `freecam.cpp`: the free camera: the master camera's placement replaced by a flown one, the
  renderer's view and field of view kept free of the game's camera effects, player 1's input held
  back from the game meanwhile (`input.cpp`). Nothing changes outside a flight.
- `src/debug/`: the debug menu (Dear ImGui: the Console, Characters and Game tabs) and the
  bridge to the game's own console, whose window and output the retail build left out. See
  [debug menu](debug-menu.md).

## The window (`src/core/window.cpp`)

A top-level window with the game's own icon (decoded from the executable at runtime; the port
ships no game artwork) and a child window the game draws into. The mouse is captured only once you
click into the game. The process is per-monitor DPI aware, so the picture is never stretched by
Windows.

## Engine background

The game runs on The Collective's *Slayer* engine, also used for Indiana Jones and the Emperor's
Tomb (2003) and Marc Ecko's Getting Up. Notes on what those and the PS2 version of this game teach us
are in [research](README.md#research).
