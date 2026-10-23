#pragma once

#include <string>

namespace swrots::game {

// The on-screen button prompts for the keyboard and mouse (docs/controls.md): while the player's last input was the
// keyboard or the mouse (input::KeyboardPrompts), every Xbox button the game shows (menu footers, tips, the HUD's
// prompts, the pause screen's moves) is drawn as the key bound to it in controls.ini instead: key caps in the game's
// two icon looks, labelled with the game's own font, made at run time. With a controller, the game's own again.
// At every boot; `cache`: where the made pictures are kept.
void InstallPrompts(const std::wstring& cache);

} // namespace swrots::game
