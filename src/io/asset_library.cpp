#include "io/asset_library.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_map>

#include "core/log.h"
#include "io/gltf_io.h"

namespace ogle {

namespace fs = std::filesystem;

namespace {

fs::path to_path(const std::string& s) { return fs::path(std::u8string((const char8_t*)s.c_str())); }
std::string from_path(const fs::path& p) {
  auto u = p.generic_u8string();
  return std::string(u.begin(), u.end());
}

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

json read_json(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return json();
  std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return json::parse(text, nullptr, false);
}

uint64_t fnv(const std::vector<uint8_t>& data) {
  uint64_t h = 1469598103934665603ull;
  for (uint8_t b : data) h = (h ^ b) * 1099511628211ull;
  return h;
}

}  // namespace

std::string strip_lod(const std::string& name, int* lod) {
  auto p = name.rfind("-lod");
  if (p != std::string::npos && p + 4 < name.size()) {
    bool digits = true;
    for (size_t i = p + 4; i < name.size(); i++) digits &= std::isdigit((unsigned char)name[i]) != 0;
    if (digits) {
      if (lod) *lod = std::stoi(name.substr(p + 4));
      return name.substr(0, p);
    }
  }
  if (lod) *lod = 0;
  return name;
}

// ------------------------------------------------------------------------------------------------
// games of a library folder
// ------------------------------------------------------------------------------------------------

std::string library_root_of(const std::string& folder) {
  std::error_code ec;
  const fs::path p = fs::absolute(to_path(folder), ec);
  return from_path(fs::exists(p / "library.json", ec) ? p.parent_path() : p);
}

std::string default_library_root() {
  std::error_code ec;
#ifdef OGLE_PROJECT_DIR
  const fs::path project = to_path(OGLE_PROJECT_DIR);
  if (fs::exists(project / "CMakeLists.txt", ec) && fs::is_directory(project / "src", ec))
    return from_path(project / "library");
#endif
  const char* base = SDL_GetBasePath();
  return from_path(to_path(base ? base : "") / "library");
}

std::vector<GameFolder> list_games(const std::string& library_root) {
  std::vector<GameFolder> out;
  if (library_root.empty()) return out;
  const fs::path root = to_path(library_root_of(library_root));
  for (const char* game : {"jak1", "jak2", "jak3"}) {
    const json lib = read_json(root / game / "library.json");
    if (!lib.is_object()) continue;
    GameFolder g;
    g.game = game;
    g.name = lib.value("name", std::string(game));
    g.path = from_path(root / game);
    g.levels = lib.value("levels", json::array()).size();
    out.push_back(std::move(g));
  }
  return out;
}

// ------------------------------------------------------------------------------------------------
// the folder of one game
// ------------------------------------------------------------------------------------------------

void AssetLibrary::clear() {
  m_root.clear();
  m_game.clear();
  m_levels.clear();
  m_models.clear();
}

bool AssetLibrary::scan(const std::string& game_folder, std::string* error) {
  clear();
  std::error_code ec;
  const fs::path base = fs::absolute(to_path(game_folder), ec);
  if (!fs::is_directory(base, ec)) {
    if (error) *error = "folder not found: " + game_folder;
    return false;
  }
  m_root = from_path(base);
  const json lib = read_json(base / "library.json");
  m_game = lib.is_object() ? lib.value("game", std::string()) : std::string();
  for (const auto& dir : fs::directory_iterator(base, ec)) {
    if (!dir.is_directory(ec)) continue;
    const std::string name = from_path(dir.path().filename());
    if (name == "prefabs" || name == "_disc" || name == "_work") continue;
    // the models of the folder
    for (const auto& f : fs::directory_iterator(dir.path() / "models", ec)) {
      if (!f.is_regular_file(ec) || f.path().extension() != ".glb") continue;
      ModelAsset m;
      m.path = from_path(f.path());
      m.group = name;
      m.name = strip_lod(from_path(f.path().stem()), &m.lod);
      m_models.push_back(std::move(m));
    }
    // a level: its decor
    const fs::path background = dir.path() / "decor" / (name + "-background.glb");
    if (!fs::is_regular_file(background, ec)) continue;
    LevelEntry level;
    level.name = name;
    level.background = from_path(background);
    level.mtime = (int64_t)fs::last_write_time(background, ec).time_since_epoch().count();
    const json info = read_json(dir.path() / "level.json");
    if (info.is_object()) {
      level.level = info.value("level", std::string());
      level.dgo = info.value("dgo", std::string());
      level.actors = info.value("actors", 0);
      level.collision_triangles = info.value("collision_triangles", -1);
      if (info.contains("actor_types")) level.actor_types = info["actor_types"];
    }
    m_levels.push_back(std::move(level));
  }
  std::sort(m_levels.begin(), m_levels.end(), [](const LevelEntry& a, const LevelEntry& b) { return a.name < b.name; });
  std::sort(m_models.begin(), m_models.end(), [](const ModelAsset& a, const ModelAsset& b) {
    if (a.name != b.name) return a.name < b.name;
    if (a.group != b.group) return a.group < b.group;
    return a.lod < b.lod;
  });
  LOG_INFO("library %s: %zu levels, %zu models", m_root.c_str(), m_levels.size(), m_models.size());
  return true;
}

void AssetLibrary::set_listed(const std::function<bool(const LevelEntry&)>& listed) {
  for (auto& l : m_levels) l.listed = listed(l);
}

const LevelEntry* AssetLibrary::level(const std::string& name) const {
  for (const auto& l : m_levels)
    if (l.name == name) return &l;
  return nullptr;
}

const LevelEntry* AssetLibrary::level_of(const std::string& background_path) const {
  std::string p = background_path;
  for (auto& c : p)
    if (c == '\\') c = '/';
  for (const auto& l : m_levels)
    if (l.background == p) return &l;
  return nullptr;
}

const ModelAsset* AssetLibrary::model_named(const std::string& name, const std::string& prefer_group) const {
  const ModelAsset* any = nullptr;
  auto it = std::lower_bound(m_models.begin(), m_models.end(), name,
                             [](const ModelAsset& m, const std::string& n) { return m.name < n; });
  for (; it != m_models.end() && it->name == name; ++it) {
    if (it->group == prefer_group) return &*it;  // lowest lod first
    if (!any) any = &*it;
  }
  return any;
}

const ModelAsset* AssetLibrary::model_for_etype(const std::string& etype, const std::string& prefer_group) const {
  if (etype.empty()) return nullptr;
  if (const ModelAsset* m = model_named(etype, prefer_group)) return m;
  // the only model name matching, in the preferred folder, then anywhere
  auto unique = [&](auto&& match, bool only_group) -> const ModelAsset* {
    std::string found;
    for (const auto& m : m_models) {
      if ((only_group && m.group != prefer_group) || !match(m.name)) continue;
      if (!found.empty() && found != m.name) return nullptr;  // ambiguous
      found = m.name;
    }
    return found.empty() ? nullptr : model_named(found, prefer_group);
  };
  auto prefix = [&](const std::string& n) { return n.rfind(etype + "-", 0) == 0; };
  auto suffix = [&](const std::string& n) { return ends_with(n, "-" + etype); };
  for (bool only_group : {true, false}) {
    if (only_group && prefer_group.empty()) continue;
    if (const ModelAsset* m = unique(prefix, only_group)) return m;
    if (const ModelAsset* m = unique(suffix, only_group)) return m;
  }
  return nullptr;
}

// ------------------------------------------------------------------------------------------------
// loading
// ------------------------------------------------------------------------------------------------

bool load_model_asset(const std::string& path, LoadedMesh* out, std::string* error) {
  Scene tmp;
  ImportOptions o;
  ImportResult r;
  if (!import_gltf(tmp, path, o, &r, error)) return false;
  auto mesh = std::make_shared<Mesh>();
  mesh->name = strip_lod(from_path(to_path(path).stem()));
  for (const auto& n : tmp.nodes) {
    if (n.kind != NodeKind::Render || !n.mesh) continue;
    Mat4 w = tmp.world(n);
    Mat4 nm = w.inverse().transposed();
    for (auto p : n.mesh->prims) {
      for (auto& v : p.pos) v = w.point(v);
      for (auto& v : p.nrm) v = normalize(nm.dir(v));
      p.tri_pat.clear();
      mesh->prims.push_back(std::move(p));
    }
  }
  if (mesh->prims.empty()) {
    if (error) *error = "no geometry in " + path;
    return false;
  }
  out->mesh = mesh;
  out->textures = std::move(tmp.textures);
  out->materials = std::move(tmp.materials);
  return true;
}

std::shared_ptr<Mesh> merge_mesh(Scene& doc, const Mesh& mesh, const std::vector<Texture>& textures,
                                 const std::vector<Material>& materials) {
  std::unordered_map<int, int> tex_map, mat_map;
  auto texture = [&](int ti) -> int {
    if (ti < 0 || ti >= (int)textures.size()) return -1;
    auto it = tex_map.find(ti);
    if (it != tex_map.end()) return it->second;
    const Texture& t = textures[ti];
    const uint64_t h = fnv(t.rgba);
    int found = -1;
    for (int i = 0; i < (int)doc.textures.size() && found < 0; i++) {
      const Texture& d = doc.textures[i];
      if (d.name == t.name && d.width == t.width && d.height == t.height && fnv(d.rgba) == h) found = i;
    }
    if (found < 0) found = doc.add_texture(t);
    tex_map[ti] = found;
    return found;
  };
  auto material = [&](int mi) -> int {
    if (mi < 0 || mi >= (int)materials.size()) return -1;
    auto it = mat_map.find(mi);
    if (it != mat_map.end()) return it->second;
    Material m = materials[mi];
    m.texture = texture(m.texture);
    int found = -1;
    for (int i = 0; i < (int)doc.materials.size() && found < 0; i++) {
      const Material& d = doc.materials[i];
      if (d.name == m.name && d.texture == m.texture && d.alpha == m.alpha && d.blend == m.blend &&
          d.cutoff == m.cutoff && d.double_sided == m.double_sided && d.base_color.x == m.base_color.x &&
          d.base_color.y == m.base_color.y && d.base_color.z == m.base_color.z &&
          d.base_color.w == m.base_color.w)
        found = i;
    }
    if (found < 0) found = doc.add_material(std::move(m));
    mat_map[mi] = found;
    return found;
  };
  auto out = std::make_shared<Mesh>(mesh);
  for (auto& p : out->prims) p.material = material(p.material);
  out->touch();
  return out;
}

}  // namespace ogle
