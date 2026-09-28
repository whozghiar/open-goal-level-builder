#include "io/prefabs.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

#include "io/asset_library.h"
#include "io/gltf_io.h"

namespace ogle {

namespace fs = std::filesystem;

namespace {

fs::path to_path(const std::string& s) { return fs::path(std::u8string((const char8_t*)s.c_str())); }
std::string from_path(const fs::path& p) {
  auto u = p.generic_u8string();
  return std::string(u.begin(), u.end());
}

}  // namespace

std::vector<PrefabInfo> list_prefabs(const std::string& folder) {
  std::vector<PrefabInfo> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(to_path(folder), ec)) {
    if (!e.is_regular_file(ec) || e.path().extension() != ".glb") continue;
    PrefabInfo p;
    p.name = from_path(e.path().stem());
    p.path = from_path(e.path());
    p.mtime = (int64_t)e.last_write_time(ec).time_since_epoch().count();
    out.push_back(std::move(p));
  }
  std::sort(out.begin(), out.end(), [](const PrefabInfo& a, const PrefabInfo& b) { return a.name < b.name; });
  return out;
}

std::string new_prefab_path(const std::string& folder, const std::string& name) {
  std::string base;
  for (char c : name) {
    const bool keep = std::isalnum((unsigned char)c) || c == '-' || c == '_' || (unsigned char)c >= 0x80;
    if (keep) base += c;
    else if (!base.empty() && base.back() != '_') base += '_';
  }
  while (!base.empty() && base.back() == '_') base.pop_back();
  if (base.empty()) base = "prefab";
  std::error_code ec;
  std::string path = folder + "/" + base + ".glb";
  for (int i = 2; fs::exists(to_path(path), ec); i++) path = folder + "/" + base + "-" + std::to_string(i) + ".glb";
  return path;
}

bool save_prefab(const Scene& scene, const std::vector<uint32_t>& roots, const std::string& path,
                 std::string* error) {
  if (roots.empty()) {
    if (error) *error = "nothing selected";
    return false;
  }
  // the pivot: bottom center of everything the prefab holds
  AABB b;
  for (auto id : roots) {
    if (const Node* n = scene.find(id)) b.add(scene.world_bounds(*n));
    for (auto d : scene.descendants(id))
      if (const Node* n = scene.find(d)) b.add(scene.world_bounds(*n));
  }
  ExportOptions o;
  o.mode = ExportMode::Prefab;
  o.only = roots;
  if (b.valid()) o.pivot = Mat4::translate({b.center().x, b.lo.y, b.center().z});
  std::error_code ec;
  fs::create_directories(to_path(path).parent_path(), ec);
  return export_glb(scene, path, o, error);
}

bool load_prefab(const std::string& path, Scene* out, std::string* error) {
  ImportOptions o;
  ImportResult r;
  return import_gltf(*out, path, o, &r, error);
}

std::vector<uint32_t> place_prefab(Scene& doc, const Scene& prefab, const Mat4& m,
                                   std::map<const Mesh*, std::shared_ptr<Mesh>>* merged) {
  std::unordered_map<uint32_t, uint32_t> ids;
  for (const auto& n : prefab.nodes) ids[n.id] = doc.next_id++;
  std::unordered_set<std::string> names;
  for (const auto& n : doc.nodes) names.insert(n.name);
  auto free_name = [&](const std::string& base, bool numbered) {
    std::string name = numbered ? base + "-1" : base;
    for (int i = 2; names.count(name); i++) name = base + "-" + std::to_string(i);
    names.insert(name);
    return name;
  };
  std::vector<uint32_t> roots;
  for (const auto& src : prefab.nodes) {
    Node n = src;
    n.id = ids[src.id];
    auto p = ids.find(src.parent);
    n.parent = src.parent && p != ids.end() ? p->second : 0;
    if (!n.parent) {
      n.local = Transform::from_matrix(m * src.local.matrix());
      roots.push_back(n.id);
    }
    // two actors of a level cannot share a name (the game finds them by it): a placed actor is
    // named like the game names them, <type>-<number>; the other roots apart from the document's
    const std::string etype = n.kind == NodeKind::Actor ? n.extras.value("etype", std::string()) : std::string();
    if (!etype.empty()) {
      n.name = free_name(etype, true);
      if (n.extras.contains("lump") && n.extras["lump"].is_object()) n.extras["lump"]["name"] = n.name;
    } else if (!n.parent) {
      n.name = free_name(n.name, false);
    }
    if (src.mesh) {
      auto& shared = (*merged)[src.mesh.get()];
      if (!shared) shared = merge_mesh(doc, *src.mesh, prefab.textures, prefab.materials);
      n.mesh = shared;
    }
    // a placed actor is a new one: the game gives it its id
    if (n.kind == NodeKind::Actor) n.extras.erase("aid");
    doc.add(std::move(n));
  }
  return roots;
}

}  // namespace ogle
