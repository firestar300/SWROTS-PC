#pragma once

#include <string>
#include <vector>

namespace swrots::game {

// Heroes vs Villains (docs/research/heroes-vs-villains.md): four heroes against four villains on the developer map.
// The fighters are placed in the level (so they load with it, with their sounds) and wait until the match starts;
// the player is one of them. Each side has a number of lives: a fighter that falls comes back after its death has
// played out, near the fight, while its side has lives left; a side with none left and no one standing loses.
// Points for damage, defeats, assists and streaks; a kill feed; the top five at the end. Asked for by the launch
// arguments `hvv:<8 classes>` (heroes, then villains), `hvvplayer:<0-7>` (the player's place in the line-up, 0 by
// default), `hvvlives:<n>`, `hvvhealth:<n>` and `hvvintro:0` (no intro), next to the dev map's.

// Each start of the game image, after InstallDevMap: lays out the dev map for a match when this start asks for one.
void InstallHvv();

// True when this start of the game is a match.
bool HvvActive();

// The match, for the overlay (scoreboard, titles, markers, kill feed, leaderboard, end) and the debug menu.
struct HvvState {
    bool active = false;
    bool started = false;  // the fighters are there (the intro, the fight or the end)
    bool fighting = false; // the fight is on
    int winner = -1;       // 0 the heroes, 1 the villains; -1 none yet
    int player = 0;        // the player's place in the line-up
    int startLives = 0;
    int lives[2] = {};     // left, heroes and villains
    int standing[2] = {};  // fighters standing
    // the intro's titles, then "FIGHT!"
    std::string caption, subcaption;
    int captionSide = -1;  // the side a title is of
    float captionFade = 0; // 0 to 1 through the title's time
    struct Fighter {
        std::string className, name; // name: as shown ("YOU" for the player)
        int side = 0;
        bool standing = false, out = false, isPlayer = false;
        bool placed = false;      // in the level now (`head` valid)
        float head[3] = {};       // above its head, in the world
        int score = 0, defeats = 0, falls = 0, assists = 0, damage = 0, streak = 0;
    } fighters[8];
    struct FeedLine {
        std::string killer, victim; // killer empty: it fell by itself
        int killerSide = -1, victimSide = 0;
        bool mine = false;          // the player is in it
        float age = 0;              // seconds
    };
    std::vector<FeedLine> feed;     // newest first
    // the player's own notices: "DEFEATED <NAME>" with a streak, "ASSIST <NAME>", "DEFEATED BY <NAME>"
    std::string notice, noticeSub;
    int noticePoints = 0;
    int noticeSide = 0;
    float noticeAge = -1;           // seconds; negative: none
    bool board = false;             // the leaderboard is pinned (`hvv board`)
    bool ending = false;            // the end: the top five
    float endingAge = 0;
    std::vector<int> top;           // fighters, best first (up to 5)
};
HvvState GetHvvState();

// The default line-up: heroes, then villains.
std::vector<std::string> HvvDefaultLineup();

// Starts a match (the game restarts into it). `lineup`: 8 classes, heroes then villains; `player`: the player's
// place in it; `lives`: each side's; `health`: every fighter's (0: the defaults, 35 and 250); `intro`: the lines
// shown first. False with `error` set for a line-up the game cannot play.
bool StartHvv(const std::vector<std::string>& lineup, int player, int lives, int health, bool intro,
    std::string& error);

// Fighters for the match panel: the Versus fighters and the other saber duelists (registered classes only).
std::vector<std::string> HvvFighterClasses();
// A fighter's name as shown (e.g. "OBI-WAN KENOBI").
std::string HvvDisplayName(const std::string& className);

// Pins the leaderboard (as holding Tab shows it), or unpins it.
void SetHvvBoardPinned(bool pinned);

// Once a frame on the game thread: the match's progress.
void HvvFrame();

} // namespace swrots::game
