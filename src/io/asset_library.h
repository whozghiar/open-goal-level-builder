#pragma once

// The library: the folder ogle-extract writes to, one subfolder per game (jak1/, jak2/, jak3/).
//
//   <game>/library.json                           the game, its version, the levels extracted
//   <game>/<level>/decor/<level>-background.glb   a level: its decor, collision and actors
//   <game>/<level>/models/<model>-lodN.glb        the models of its actors
//   <game>/<level>/level.json                     what the folder holds (dgo, actors, models)
//   <game>/common/models/                         the models every level uses
//   <game>/prefabs/                               the user's prefabs made from that game's levels
//
// An AssetLibrary is the folder of one game. Scanning only lists file names; geometry is read
// when a level is opened or a thumbnail is drawn.

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "scene/scene.h"

namespace ogle {

struct GameFolder {
  std::string game;  // "jak2"
  std::string name;  // "Jak II: Renegade" (from its library.json)
  std::string path;  // absolute, '/' separated
  size_t levels = 0;
};
// The extracted games of a library folder, in game order. A folder that is itself a game's
// (it holds library.json) is taken as the game folder of its parent.
std::vector<GameFolder> list_games(const std::string& library_root);
// The library folder of a path the user chose: the parent of a game folder, else the path itself.
std::string library_root_of(const std::string& folder);
// The library folder when none holds an extracted game: library/ in the editor's own tree (the
// source tree it was built from, else the folder of the executable).
std::string default_library_root();

struct LevelEntry {
  std::string name;        // "atoll" (the folder)
  std::string level;       // the game's name of the level (level.json), "nest" for "nest-nestt"
  std::string dgo;         // "ATO.DGO" (level.json)
  std::string background;  // <level>/decor/<level>-background.glb
  int actors = 0;          // level.json
  int collision_triangles = -1;  // level.json, -1 when it does not say
  json actor_types;        // level.json: per actor type, how many and the data of the first one
  int64_t mtime = 0;       // date of the background file
  bool listed = true;      // shown in the Levels panel (see App::set_library)
};

struct ModelAsset {
  std::string path;
  std::string group;  // the level folder it is in ("common" for the shared models)
  std::string name;   // file name without "-lodN"
  int lod = 0;
};

class AssetLibrary {
 public:
  bool scan(const std::string& game_folder, std::string* error);
  void clear();
  bool empty() const { return m_levels.empty(); }
  const std::string& root() const { return m_root; }
  const std::string& game() const { return m_game; }  // "jak2", from library.json
  const std::vector<LevelEntry>& levels() const { return m_levels; }
  // decides which levels the Levels panel lists
  void set_listed(const std::function<bool(const LevelEntry&)>& listed);
  const LevelEntry* level(const std::string& name) const;
  const LevelEntry* level_of(const std::string& background_path) const;
  std::string prefab_folder() const { return m_root.empty() ? std::string() : m_root + "/prefabs"; }

  const std::vector<ModelAsset>& models() const { return m_models; }  // by name, group, lod
  // lowest lod of a model name, the copy of `prefer_group` first
  const ModelAsset* model_named(const std::string& name, const std::string& prefer_group) const;
  // The model drawn for an actor type, from file names only:
  //   1. a model named like the type (vin-door-ctyinda),
  //   2. the only model whose name starts with "<etype>-" (crate -> crate-krimson),
  //   3. the only model whose name ends with "-<etype>" (skill -> collectables-skill),
  // looking in `prefer_group` first, then in the whole game.
  const ModelAsset* model_for_etype(const std::string& etype, const std::string& prefer_group) const;

 private:
  std::string m_root;
  std::string m_game;
  std::vector<LevelEntry> m_levels;
  std::vector<ModelAsset> m_models;  // sorted by (name, group, lod)
};

// ------------------------------------------------------------------------------------------------
// loading (thread-safe: a worker fills these, the main thread merges them into a document)
// ------------------------------------------------------------------------------------------------

// A mesh with its own textures and materials (primitive materials index `materials`, material
// textures index `textures`).
struct LoadedMesh {
  std::shared_ptr<Mesh> mesh;
  std::vector<Texture> textures;
  std::vector<Material> materials;
};

// Every render node of the file baked into one mesh, in the file's space (a model in its bind
// pose, a prefab around its pivot).
bool load_model_asset(const std::string& path, LoadedMesh* out, std::string* error);

// Copies a mesh into a document: textures (deduplicated by name and content) and materials are
// appended, primitive materials remapped. The returned mesh belongs to the document.
std::shared_ptr<Mesh> merge_mesh(Scene& doc, const Mesh& mesh, const std::vector<Texture>& textures,
                                 const std::vector<Material>& materials);

// "collectables-skill-lod0" -> "collectables-skill", lod in *lod (0 without suffix)
std::string strip_lod(const std::string& name, int* lod = nullptr);

}  // namespace ogle
