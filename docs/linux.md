# Linux and handheld compatibility

SWROTS-PC is a Windows program, but runs on Linux through Proton (Steam's compatibility layer). These
steps were performed, reviewed, and submitted by community member chatgipity. Hardware tested includes
a ROG Ally X handheld, a Lenovo Yoga Slim 7i laptop, and an AMD Ryzen 7 5700X3D desktop. Each device was
running Bazzite 44, a Fedora-based Linux OS, and tested with Proton Experimental on each release from
SWROTS-PC v0.2.1 to v0.3.1. Another user reported playing on a Steam Deck LCD (SteamOS) using Proton GE 10-24.

You need your own copy of the game as an `.iso` ([installing](install.md)). Use the `.iso` itself rather
than an archive: unpacking archives relies on a Windows tool that Proton may not have.

## Setting up

1. **Add it to Steam.** Download and extract the release `.zip`, right-click `swrots.exe` and choose
   **Add to Steam** (if on a Steam Deck, this will be from Desktop Mode).
2. **Choose Proton.** In Steam, right-click the new `swrots.exe` entry: **Properties → Compatibility →
   Force the use of a specific Steam Play compatibility tool**, and pick **Proton Experimental** (or
   Proton GE).
3. **Create its Proton folder.** Launch it once. When its [launcher](launcher.md) opens, close it
   (**Quit**). Steam has now made the game's own Windows folder (its *prefix*).
4. **Find the prefix.** A non-Steam game gets a number of its own (its app ID). The Protontricks app lists
   it next to `swrots.exe`; the prefix is then:

   ```
   /home/<you>/.steam/steam/steamapps/compatdata/<app ID>/pfx/dosdevices/c:/
   ```

5. **Move the game into it.** There, create `Games/SWROTS/`, and copy `swrots.exe`, `core.dll` and your
   `.iso` into it.
6. **Point Steam at it.** In the entry's **Properties**, set:
   - **Target**: the full path of that `swrots.exe`, in quotation marks:
     `"/home/<you>/.steam/steam/steamapps/compatdata/<app ID>/pfx/dosdevices/c:/Games/SWROTS/swrots.exe"`
   - **Start In**: the folder it is in, without quotation marks:
     `/home/<you>/.steam/steam/steamapps/compatdata/<app ID>/pfx/dosdevices/c:/Games/SWROTS/`
7. **Install.** Launch it. In the launcher, choose **Choose disc image** and pick your `.iso` in the file
   dialog. The game files are copied; then choose **Play**.

Settings and controls are the `.ini` files in that folder ([settings](settings.md), [controls](controls.md)).

## Updating

Extract the new release and replace `swrots.exe` and `core.dll` in `c:/Games/SWROTS/` inside the prefix.
The game files, saves, settings and mods stay.

## Reporting a problem

Include `logs/swrots.log` from that folder. Since v0.3.1 it records the Proton (Wine) version, the GPU,
the sound output and the frame rate, so please say which Proton you chose too.
