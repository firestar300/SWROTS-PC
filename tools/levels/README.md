# Level tools

Research tools for the game's level files (`levels\<level>.slp`), described in
[docs/research/levels.md](../../docs/research/levels.md). They read the user's own copy of the game and write
nothing of it back to the game's folders. Python 3; `slpscene.py`, `devfloor.py` and `devmap.py` also use
`tools/meshes/charmsh.py` (the model format).

| Tool | Does |
|---|---|
| `pak.py find <pak> <name>` / `extract <pak> <name> <out>` | finds a resource stored inline in a level PAK, and copies its data out (take level files from the PAK: a dumped one is incomplete) |
| `slp.py map <file.slp>` | prints what the reader parsed, with offsets (header, brushes, static instances, objects) |
| `slp.py selftest <file.slp>...` | reads and writes back each file, checking the bytes are the same (Grievous Survival, M08 and M09A pass) |
| `slpobj.py <file.slp> <class or name part>` | prints the matching objects' properties as a tree (e.g. `IPointSpawnPlayer`, `ILightManager`) |
| `devmap.py <in.slp> <out.slp> [steps]` | turns the Grievous Survival level into the developer test map (steps in its notes) |
| `makedevmap.py <GameData folder> <out.slp>` | both at once: the level file out of its PAK, then `devmap.py` |
| `devfloor.py <out.msh>` | writes the developer map's flat floor model from scratch (the port makes the same floor itself) |

As a library, `slp.load(path)` returns the parsed file and `slp.save(file)` its bytes; `slp.get`, `slp.set_prop`,
`slp.tree` and `slp.tree_bytes` read and change an object's properties.
