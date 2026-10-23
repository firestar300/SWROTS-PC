#pragma once

#include <windows.h>

struct IDirect3DDevice9;

namespace swrots::debug {

// The debug menu: a Dear ImGui overlay on the game window ([Debug] DebugMenu=1 in
// settings.ini; [Debug] MenuKey, default ~, toggles it) with the game's console and
// developer switches.
//
// Threads: window messages arrive on the process's main thread, the game renders
// on its own. Messages for the menu are queued and fed to ImGui on the game thread
// when it builds the menu's frame.

// `key`: the toggle key's name from settings.ini ("~", "F1", "Insert", a letter...).
void ConfigureMenu(bool enabled, const wchar_t* key);
bool MenuOpen();
// True when something is drawn over the game's picture with the menu closed (Heroes vs Villains' scoreboard):
// RenderMenu draws it, as part of the picture.
bool OverlayShown();

// Window thread, first thing in the game window's procedure. Returns true (with
// `result`) when the menu consumed the message.
bool MenuWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT* result);

// Game thread: the host device was created / is about to be released.
void MenuDeviceCreated(IDirect3DDevice9* device, HWND window);
void MenuDeviceReleasing();

// Game thread, inside a scene, with the window's back buffer as render target 0.
void RenderMenu();

// Game thread, after the frame was presented: runs queued console commands.
void MenuAfterFrame();

} // namespace swrots::debug
