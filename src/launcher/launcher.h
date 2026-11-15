#pragma once
// The launcher: the window swrots.exe opens before the game.
//
// It installs the game files from the player's disc image (core/install.h) and says what is
// wrong with an image it cannot use, edits settings.ini and controls.ini, uninstalls the game
// files again, and starts the game. It looks like the game: the port's overlay colours, and,
// once the game files are installed, the game's own HUD font and title emblem, read from the
// player's files (nothing of the game is part of the port).
//
// The launcher runs in the process swrots.exe starts first; the game itself then runs in a
// process of its own (`swrots.exe --from-launcher`), started as before: nothing the launcher
// loaded (Direct3D, the file dialog's shell extensions) is in the game's address space.
// docs/launcher.md describes it for players.

#include <string>

namespace swrots {

enum class LauncherResult {
    Quit,        // the player closed the launcher
    Play,        // the player chose to play: start the game (StartGameProcess)
    Unavailable, // the launcher's window could not be made (no Direct3D 9): carry on without it
};

// Shows the launcher until the player plays or quits. `installImage` (from --install) is installed
// straight away when the game files are not there yet.
LauncherResult RunLauncher(const std::wstring& exeDir, const std::wstring& installImage);

// Starts the game in a process of its own, with this process's arguments (without the
// launcher's own), and closes this process's log for it to continue. False when the process
// could not be started.
bool StartGameProcess(const std::wstring& exeDir);

} // namespace swrots
