#pragma once

// Prefabs: elements of a level saved together (a building and its props, a group of crates...)
// to be put again in any level. Each prefab is a .glb in the game's prefab folder
// (<library>/<game>/prefabs/): the elements with their hierarchy, around a pivot at the bottom
// center of their bounds, so that a prefab sits on the surface it is placed on.

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "scene/scene.h"

namespace ogle {

struct PrefabInfo {
  std::string name;  // file name without .glb
  std::string path;
  int64_t mtime = 0;
};
std::vector<PrefabInfo> list_prefabs(const std::string& folder);

// A file name for a prefab name ("Pipes & crates" -> "Pipes_crates.glb"), not taken in `folder`.
std::string new_prefab_path(const std::string& folder, const std::string& name);

// Writes `roots` (and their descendants) to a prefab file.
bool save_prefab(const Scene& scene, const std::vector<uint32_t>& roots, const std::string& path,
                 std::string* error);
bool load_prefab(const std::string& path, Scene* out, std::string* error);

// Puts a prefab in a document at `m`: new nodes, the roots at the top of the document, named apart
// from the document's (its actors like the game names them: <type>-<number>, in their data too).
// Meshes are merged into the document once (`merged`, per prefab mesh) and shared by later
// placements. Returns the new roots.
std::vector<uint32_t> place_prefab(Scene& doc, const Scene& prefab, const Mat4& m,
                                   std::map<const Mesh*, std::shared_ptr<Mesh>>* merged);

}  // namespace ogle
