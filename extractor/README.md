# ogle-extract

`ogle-extract` reads **your own copy** of a Jak and Daxter game (Jak 1, 2 or 3: an `.iso` image or
the folder of a disc already extracted) and writes the `.glb` files the editor loads. The editor
runs it itself (**File > Extract game assets...**), but it also works alone:

```text
ogle-extract <game.iso | disc folder> <output folder> [--levels ATO,CIB] [--textures]
ogle-extract --detect <game.iso | folder>   # game, version and serial (json)
ogle-extract --list <game.iso | folder>     # the levels of the disc (json)
```

| Option | Effect |
|---|---|
| `--levels ATO,CIB` | extracts these DGO files only (without it: every level of the game) |
| `--textures` | also writes the textures of each level as `.png` files |
| `--data <folder>` | the games' tables (by default `ogle-extract-data/` next to the executable) |
| `--work <folder>` | where the files read from an `.iso` are copied (by default `<output>/_disc/`, deleted at the end) |

An `.iso` is read in place: `--detect` and `--list` read its file table, and an extraction copies
only the files it reads (the `DGO` and `CGO` folders, about 400 MB of a 4 GB disc).

The extractor reads nothing but the disc and its tables, and writes nothing outside the output
folder: the decompiler code it is built from looks for its files below a "project" folder, which
is always the output folder (the folder of the executable for `--detect` and `--list`), never a
jak-project checkout.

## What is written

```text
<output>/<game>/library.json                            the game, its version, the levels extracted
<output>/<game>/common/models/*.glb                     models every level uses (collectables, Jak...)
<output>/<game>/<level>/decor/<level>-background.glb    decor (one node per instance), collision, actors
<output>/<game>/<level>/models/<model>-lodN.glb         the models of the level (crates, doors, enemies...)
<output>/<game>/<level>/textures/*.png                  the textures of the level (--textures)
<output>/<game>/<level>/level.json                      what the folder holds (DGO, actors, models)
```

In the decor, the level's shell (tfrag: ground, walls, floors, bridges) is split in pieces: the
triangles their vertices connect, vertices at the same place counting as one, so that a building
or a bridge is a node of its own. The pieces 60 m across or more are the terrain
(`extras.og_terrain: true`): the editor locks them. The vertex colors are, for each vertex, the
brightest of the level's 8 palettes: the game mixes them each frame, and in Jak II and Jak 3 they
are layers of light (palette 0 alone leaves the insides of buildings black).

`level.json` also lists the actors by type (`actor_types`: how many, the data of the first one
and the art-names of all, `art_names`), the templates of the editor's catalog.

A level goes to the folder of its name. When a second DGO holds the same level (Jak II's
`NESTT.DGO`, a copy of `nest`), it goes to `<level>-<dgo>` (`nest-nestt`).

Nothing of the games is in this repository: everything written comes from the user's disc.

## Messages for the editor

The lines starting with `OGLE_` are read by the editor, which translates their codes:

| Line | Meaning |
|---|---|
| `OGLE_GAME {json}` | the game of the disc: `game`, `version`, `name`, `serial`, `iso` |
| `OGLE_LEVELS [json]` | the answer to `--list`: `[{"dgo": "ATO.DGO", "names": ["atoll"]}, ...]` |
| `OGLE_STEP <code> [numbers]` | the current step: `iso <done> <total>`, `read`, `textures`, `levels <count>` |
| `OGLE_PROGRESS <done> <total> <name>` | a level (or `common`) is written |
| `OGLE_DONE <folder>` | the end: the folder of the game |
| `OGLE_ERROR <code> [detail]` | the failure: `not_found`, `unreadable`, `not_iso`, `not_jak`, `no_executable`, `unknown_disc`, `tables_missing`, `tables_unreadable`, `no_levels`, `cannot_create`, `iso_read`, `extraction` |

## Where the code comes from

`opengoal/` holds the data readers of the decompiler of [OpenGOAL](https://github.com/open-goal/jak-project)
(ISC license, see `opengoal/LICENSE`), copied with their paths from commit `a42c07199`. Only the
files `CMakeLists.txt` compiles and the headers they include are kept: no code analysis, no GOAL
compiler, no game runtime, no REPL.

Changes from jak-project:

- `decompiler/level_extractor/fr3_to_gltf.*`: the decor can hold the collision (the exact
  surfaces) and the actors, and each decor instance (TIE, shrub) is a glTF node sharing the mesh of
  its prototype. Each model keeps only its own vertices: the original code wrote the whole vertex
  buffer of the level in every model file (once more per face blend shape), 9.8 GB instead of
  2.6 GB for all of Jak II. The PS2's additive and subtractive materials (glows, searchlight beams),
  which glTF cannot describe, keep their blend in `extras.og_blend` (`add`, `add_alpha`,
  `subtract`, `half`) instead of turning opaque.
  The level's shell is split in pieces and its vertex colors are the brightest palette (see
  above); the original code wrote one mesh per texture over the whole level and the first palette.
- `decompiler/level_extractor/extract_level.*`: the layout above, the common models, progress,
  levels extracted in parallel (as many as the processor has cores), the actors by type in
  `level.json`; no `.fr3` files for the game.
- `decompiler/config.h`: the options `rip_level_entities`, `rip_output_root`, `rip_textures_png`,
  `on_level_done`, `rip_level_folder`.
- `decompiler/ObjectFile/ObjectFileDB.*`: without the file patches (xdelta3) and the extractors of
  the game's texts, counts and subtitles.
- `common/goos/Reader.*`: without the REPL.
- `decompiler/level_extractor/extract_anim.*`: without the dependency on `goalc`.
- `common/util/FileUtil.cpp`: the project folder is always given; no search for a jak-project
  checkout from the executable's path (the original code took a sibling `jak-project` folder for
  it when the path contained that name).
- `stubs.cpp` (outside `opengoal/`): the only symbol of the code analysis the link needs.

`data/<game>/` holds the tables of jak-project the reading needs: type definitions
(`all-types.gc`), the object file names of Jak 1 (`all_objs*.json`), the files of the disc and its
levels (`inputs.json`), the textures some levels miss (`missing_textures.json`), and the level
names of each DGO (`levels.json`, from `level-info.gc`).
