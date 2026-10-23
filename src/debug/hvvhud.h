#pragma once

namespace swrots::debug {

// Heroes vs Villains' screen, over the game's picture (game/hvv.h): the scoreboard, the intro's titles, markers over
// the player's teammates, the kill feed, the player's notices ("DEFEATED <NAME>"), the leaderboard (Tab held) and the
// end's top five. In the game's own font (gamefont.h). Inside the overlay's ImGui frame, on the game thread.
void DrawHvvHud();

} // namespace swrots::debug
