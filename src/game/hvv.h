#pragma once

#include <string>
#include <vector>

namespace swrots::game {

// Heroes vs Villains (docs/research/heroes-vs-villains.md): four heroes against four villains on the developer map.
// The fighters are placed in the level (so they load with it, with their sounds) and wait until the match starts;
// the player is one of them. Each side has a number of lives: a fighter that falls comes back after its death has
// played out, somewhere safe near the fight, while its side has lives left; a side with none left and no one
// standing loses. Asked for by the launch arguments `hvv:<8 classes>` (heroes, then villains), `hvvplayer:<0-7>`
// (the player's place in the line-up, 0 by default), `hvvlives:<n>` and `hvvhealth:<n>`, next to the dev map's.

// Each start of the game image, after InstallDevMap: lays out the dev map for a match when this start asks for one.
void InstallHvv();

// True when this start of the game is a match.
bool HvvActive();

// The match, for the scoreboard and the debug menu.
struct HvvState {
    bool active = false;
    bool started = false; // the fight is on
    int winner = -1;      // 0 the heroes, 1 the villains; -1 none yet
    int player = 0;       // the player's place in the line-up
    int startLives = 0;
    int lives[2] = {};    // left, heroes and villains
    int standing[2] = {}; // fighters standing
    struct Fighter {
        std::string className;
        bool standing = false;
        bool out = false; // fell with no lives left
    } fighters[8];
};
HvvState GetHvvState();

// The default line-up: heroes, then villains.
std::vector<std::string> HvvDefaultLineup();

// Starts a match (the game restarts into it). `lineup`: 8 classes, heroes then villains; `player`: the player's
// place in it; `lives`: each side's; `health`: every fighter's (0: the defaults, 35 and 500). False with `error` set
// for a line-up the game cannot play.
bool StartHvv(const std::vector<std::string>& lineup, int player, int lives, int health, std::string& error);

// Once a frame on the game thread: the match's progress.
void HvvFrame();

} // namespace swrots::game
