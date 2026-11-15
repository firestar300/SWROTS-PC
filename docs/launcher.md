# The launcher

`swrots.exe` opens the launcher first: a window in the game's colours where you install the game files
from your disc image, change the settings and the controls, and start the game. Once the game files are
installed, its headings are in the game's own font and it shows the game's emblem; both are read from your
game files (the port contains nothing of the game).

| Entry | What it does |
|---|---|
| **Install** / **Play** | Installs the game files while they are not there, then starts the game |
| **Settings** | Everything in [`settings.ini`](settings.md): display, graphics, frame rate, co-op, developer options |
| **Controls** | The keyboard and mouse bindings of [`controls.ini`](controls.md), the mouse sensitivity, the button prompts |
| **Installation** | The state of the game files, the game's folder and log, and **Uninstall** |
| **Quit** | Closes the launcher |

A controller or the keyboard (arrows, Enter, Tab) moves through the launcher as well as the mouse.

## Installing

Choose **Choose disc image** and pick your disc image of *Star Wars: Episode III - Revenge of the Sith*
for the Xbox (North American release): an `.iso` / `.xiso`, or a `.7z`, `.zip` or `.rar` containing one.
You can also drop the file onto the window. The image is checked before anything is copied, then the game
files are copied into `GameData\` (about 2.5 GB), with a progress bar; **Cancel** stops it and leaves
nothing behind.

When an image cannot be used, the launcher says why:

| Message | What it means |
|---|---|
| The file cannot be opened | The file is gone, or another program has it open |
| The archive cannot be read | Windows' `tar` could not read the `.7z` / `.zip` / `.rar` (damaged, incomplete or password-protected): unpack it yourself and choose the `.iso` |
| No disc image in the archive | The archive has no `.iso` in it |
| The archive could not be unpacked | The archive is damaged, or the drive is full |
| Not a disc image | The file is not a disc image the port can read |
| Not an Xbox disc | A disc image without an Xbox game partition: the PlayStation 2 version of the game, another kind of disc, or an incomplete copy of the Xbox disc |
| The disc image is damaged or incomplete | The Xbox file table cannot be read, or the image ends before its files do (a copy or download that stopped early) |
| No game on this disc | An Xbox disc without a `default.xbe` |
| Another game's disc | An Xbox game, but not this one (the message names it) |
| Unsupported release of the game | Revenge of the Sith, but not the North American release (the message names its region), or a modified copy of it |
| Not enough free space | The drive cannot hold the game files (the message says how much is needed and free) |
| The GameData folder is in the way | `GameData\` exists with something else in it: uninstall first, or move it |
| The game files could not be written / The disc image could not be read | A file could not be copied (the message names it) |

The details are also in `logs\swrots.log`.

## Settings and controls

Changes are saved as you make them, to `settings.ini` and `controls.ini`; the game reads them when it
starts. To change a key, click it and press the new key or mouse button (Escape cancels); right-click a
key to remove it. Each action can have two keys in the launcher (more by editing `controls.ini`).
Controllers keep their layout.

## Uninstalling

**Installation > Uninstall** deletes the game files copied from your disc image (`GameData\`), the cache,
the game's icon and its Start menu and desktop shortcuts, after asking. Your saved games, settings and
mods are kept unless you tick them; your disc image is never touched. `swrots.exe` and `core.dll` stay,
so the game can be installed again from the launcher. To remove the port altogether, delete its folder
afterwards.

## Starting without the launcher

- Tick **Start the game directly from now on** on the Play page (`[Launcher] Skip=1` in `settings.ini`).
  The launcher still opens while the game files are not installed.
- Hold **Shift** while the game starts, or run `swrots.exe --launcher`, to see the launcher again.
- `swrots.exe --play` starts the game at once, whatever the setting (for shortcuts and scripts). Without
  the game files it asks for the disc image with Windows' own dialogs, as earlier versions did.
- `swrots.exe --install <image>` opens the launcher and installs that image straight away.

The launcher and the game are two processes: the launcher starts the game (`swrots.exe --from-launcher`)
and closes. Both write `logs\swrots.log`.
