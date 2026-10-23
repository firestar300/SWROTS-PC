#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace swrots::game {

// Characters of mods' own (docs/modding/custom-characters.md): a file under mods\characters\ (<name>.ini) makes a new
// character class, e.g. ILuke, that the game creates wherever it creates characters by class name (a level's
// objects, Versus, the port's player choice and spawns, Heroes vs Villains). Its moves, sounds and AI are another
// class's (its base, e.g. IAnakin); its body, costume, saber colour, name and side its own. The class lookup
// (0xE9B60) answers the new name with a copy of the base's class info whose create gives the character the custom
// costume list (its own costume first, then the base's).

struct CustomCharacter {
    std::string className;   // e.g. "ILuke"
    std::string name;        // as shown, e.g. "LUKE SKYWALKER"
    std::string title;       // e.g. "JEDI KNIGHT"
    std::string base;        // the class whose moves it has, e.g. "IAnakin"
    std::string costume;     // its costume's name, e.g. "Luke"
    std::string mesh;        // its body under meshes\chars ("folder\file"), e.g. "luke\luke"
    std::string code;        // three letters, e.g. "LKE"
    bool hasSaber = false;
    float saber[3] = {};     // its saber's colour (0-1), when it has one of its own
    int side = 0;            // 0 hero, 1 villain (Heroes vs Villains' default side)
    std::string source;      // the file it came from
};

// Each start of the game image, before the classes are first looked up: reads mods\characters\*.ini and hooks the
// class lookup.
void InstallCustomCharacters(const std::wstring& mods);

// The custom characters (this start's), and one by class name (case-insensitive), or null.
const std::vector<CustomCharacter>& CustomCharacters();
const CustomCharacter* FindCustomCharacter(const char* className);

// True when the mods folder has this file (relative, e.g. a custom character's pictures), from the start of the
// game on (before the resource hooks are installed).
bool ModFileExists(const std::string& relative);
// The file's contents; false when there is none.
bool ReadModFile(const std::string& relative, std::vector<uint8_t>& data);

} // namespace swrots::game
