#pragma once
// Setup: fills GameData\ from the player's own disc image, and removes it again.
//
// Accepts an Xbox disc image (.iso: an XISO, or a full Redump-style image) or
// an archive containing one (.7z/.zip/.rar, unpacked with Windows' built-in
// tar.exe). The game files are extracted to GameData\, the executable is
// checked against the supported release, and the game's icon and optional
// shortcuts are created.
//
// The launcher (launcher/launcher.h) runs the installation on a worker thread and shows its
// progress and errors itself; without the launcher (--play, unattended runs) EnsureGameData
// asks for the image with Windows' own dialogs, as before.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace swrots {

// Why setup could not finish: what is wrong with the chosen file, or with the folder installed to.
enum class InstallError {
    None,
    Cancelled,
    ImageNotFound,       // the file does not exist or cannot be opened
    ArchiveUnreadable,   // tar could not list the archive
    ArchiveWithoutImage, // no .iso in the archive
    ArchiveUnpackFailed, // the image could not be unpacked from the archive
    NotADiscImage,       // not a disc image at all
    NotAnXboxImage,      // a disc image without an Xbox game partition (a PC or PlayStation 2 disc)
    DamagedImage,        // the Xbox file table cannot be read, or files lie past the end of the image
    NoGameExecutable,    // an Xbox disc without a default.xbe
    OtherGame,           // another Xbox game's disc
    OtherRelease,        // the right game, but another release of it (region, revision) or a modified copy
    NotEnoughSpace,      // the drive cannot hold the game files
    FolderInTheWay,      // GameData\ exists with something else in it
    WriteFailed,         // a game file could not be written
    ReadFailed,          // a game file could not be read from the image
};

// A short name for the error, for a heading over its message (InstallProgress::error).
const wchar_t* InstallErrorTitle(InstallError code);

// A running installation: the worker thread writes it, the window shows it.
struct InstallProgress {
    std::mutex lock;
    std::wstring status; // what is being done; under `lock` (SetStatus, Status)
    std::atomic<uint64_t> done = 0, total = 0; // bytes; total 0 while there is nothing to measure
    std::atomic<bool> cancel = false, finished = false;
    // Valid once finished.
    bool ok = false;
    InstallError code = InstallError::None;
    std::wstring error;

    void SetStatus(const std::wstring& s)
    {
        std::lock_guard<std::mutex> g(lock);
        status = s;
    }
    std::wstring Status()
    {
        std::lock_guard<std::mutex> g(lock);
        return status;
    }
    void Fail(InstallError why, const std::wstring& message)
    {
        code = why;
        error = message;
    }
};

// Whether `gameData` holds the game (a default.xbe; the game checks which one when it starts).
bool GameDataInstalled(const std::wstring& gameData);

// The whole installation of `image` (a disc image or an archive of one) into `gameData`: on the
// calling (worker) thread, until done, failed or cancelled (p.cancel). Leaves p.ok, p.code and
// p.error; nothing is left in `gameData` unless it succeeded. Does not set p.finished.
void InstallGameFiles(const std::wstring& exeDir, const std::wstring& gameData, const std::wstring& image,
    InstallProgress& p);

// Windows' file dialog for the disc image; empty when cancelled.
std::wstring ChooseDiscImage(HWND owner);

// The game's own icon (swrots.ico, from its executable) and the Start menu shortcut, and a desktop
// shortcut when asked.
void CreateGameShortcuts(const std::wstring& exeDir, const std::wstring& gameData, bool desktop);

// What uninstalling removes besides the game files.
struct UninstallOptions {
    bool saves = false;    // the saves folder (the saved games)
    bool settings = false; // settings.ini and controls.ini
    bool mods = false;     // the mods folder
};

// Removes the game files (GameData\, cache\, swrots.ico and the shortcuts), and what `options`
// add. swrots.exe and core.dll stay, so the game can be installed again. False, with `error`,
// when something could not be deleted.
bool UninstallGame(const std::wstring& exeDir, const UninstallOptions& options, std::wstring& error);

// Runs setup if `gameData` has no default.xbe, with Windows' own dialogs (no launcher). `imagePath`
// preselects the disc image (from --install); empty asks the player. Returns false if the player
// cancelled or setup failed (after telling them why).
bool EnsureGameData(const std::wstring& exeDir, const std::wstring& gameData, const std::wstring& imagePath);

} // namespace swrots
