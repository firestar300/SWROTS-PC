#pragma once

namespace swrots::game {

// Other duelists' moves for custom characters on Serra Keto's class (ISerra base; Lightning=, Grapples=IDooku,
// ForceStorm= in docs/modding/custom-characters.md): Force lightning instead of her stun, Count Dooku's grapple throws,
// and the Force Storm special. Serra herself keeps hers.

// Every start of the game image, after the custom characters are read.
void InstallRevan();

// Each frame, outside the scripts (the storm's strikes).
void RevanFrame();

} // namespace swrots::game
