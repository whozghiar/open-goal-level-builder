#pragma once

// .glb / .gltf import (tinygltf) and .glb export (own writer).
//
// Export modes:
//   Project : the editor's document. Keeps the hierarchy, every node kind and the editor's extras
//             (og_kind, the surface of each collision triangle, actor data).
//   Prefab  : some nodes and their descendants, placed around a pivot (a prefab file).

#include <set>
#include <string>
#include <vector>

#include "scene/scene.h"

namespace ogle {

struct ImportOptions {
  uint32_t parent = 0;  // attach the imported roots under this node
  bool read_settings = false;  // take LevelSettings from the file's asset extras (projects)
  bool game_from_file = false; // take the game of a decompiler rip (asset extras "opengoal")
};

struct ImportResult {
  std::vector<uint32_t> roots;
  std::string generator;
  json asset_extras;  // the file's asset.extras (a rip's "opengoal": {"game", "level"})
  size_t nodes = 0;
  size_t triangles = 0;
};

bool import_gltf(Scene& scene, const std::string& path, const ImportOptions& opts,
                 ImportResult* result, std::string* error);

enum class ExportMode { Project, Prefab };

struct ExportOptions {
  ExportMode mode = ExportMode::Project;
  std::vector<uint32_t> only;  // Prefab mode: these nodes and their descendants
  Mat4 pivot = Mat4::identity();  // Prefab mode: the roots are written relative to this frame
};

bool export_glb(const Scene& scene, const std::string& path, const ExportOptions& opts,
                std::string* error);

// tinygltf::Value <-> json helpers are internal to gltf_io.cpp.

}  // namespace ogle
