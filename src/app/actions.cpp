#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "app/app.h"
#include "core/i18n.h"
#include "core/log.h"
#include "core/prefs.h"
#include "io/gltf_io.h"
#include "scene/story.h"

namespace ogle {

namespace fs = std::filesystem;

namespace {

fs::path to_path(const std::string& s) { return fs::path(std::u8string((const char8_t*)s.c_str())); }
std::string from_path(const fs::path& p) {
  auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

std::string lower_ext(const std::string& path) {
  std::string e = to_path(path).extension().string();
  for (auto& c : e) c = (char)std::tolower((unsigned char)c);
  return e;
}

// The model of an actor (catalog.h: its look, its art-name, the models of its type...).
const ModelAsset* actor_model(const Node& actor, const AssetLibrary& library, const GameData* data,
                              const std::string& group) {
  auto lump = actor.extras.find("lump");
  return resolve_actor_model(library, data, actor.extras.value("etype", std::string()),
                             lump != actor.extras.end() ? *lump : json(), group);
}

// The game draws water and dark eco as a see-through layer lit by a reflection map, which is not
// drawn here: water becomes a bluish translucent surface, dark eco (a black layer) a dark purple
// one. Lava keeps its own look.
void style_water_mesh(Scene& scene, const Mesh& mesh, const std::string& model) {
  if (model.find("lava") != std::string::npos) return;
  const bool dark_eco = model.find("dark-eco") != std::string::npos;
  std::set<int> done;
  for (const auto& prim : mesh.prims) {
    if (prim.material < 0 || prim.material >= (int)scene.materials.size() || !done.insert(prim.material).second)
      continue;
    Material& m = scene.materials[prim.material];
    m.alpha = AlphaMode::Blend;
    if (dark_eco) {
      m.texture = -1;
      m.base_color = {0.2f, 0.07f, 0.3f, 0.88f};
    } else {
      m.base_color = {m.base_color.x * 0.55f, m.base_color.y * 0.8f, m.base_color.z, std::min(m.base_color.w, 0.75f)};
    }
  }
}

// Draws under each actor of scene.nodes[first..] the model the library has for it (from the
// folder `group` first). Returns the number of actors with one.
size_t attach_actor_models(Scene& scene, const AssetLibrary& library, const GameData* data, size_t first,
                           const std::string& group) {
  std::map<std::string, std::shared_ptr<Mesh>> meshes;  // model file -> mesh of the document
  std::vector<uint32_t> actors;
  for (size_t i = first; i < scene.nodes.size(); i++)
    if (scene.nodes[i].kind == NodeKind::Actor) actors.push_back(scene.nodes[i].id);
  size_t attached = 0;
  for (auto id : actors) {
    const ModelAsset* model = actor_model(*scene.find(id), library, data, group);
    if (!model) continue;
    auto it = meshes.find(model->path);
    if (it == meshes.end()) {
      LoadedMesh lm;
      std::string err;
      std::shared_ptr<Mesh> mesh;
      if (load_model_asset(model->path, &lm, &err)) mesh = merge_mesh(scene, *lm.mesh, lm.textures, lm.materials);
      else LOG_WARN("%s: %s", model->path.c_str(), err.c_str());
      const std::string etype = scene.find(id)->extras.value("etype", std::string());
      if (mesh && data && data->is_water(etype)) style_water_mesh(scene, *mesh, model->name);
      it = meshes.emplace(model->path, mesh).first;
    }
    if (!it->second) continue;
    Node v;
    v.parent = id;
    v.name = model->name;
    v.kind = NodeKind::Render;
    v.mesh = it->second;
    v.extras = json{{"og_actor_visual", model->name}};
    scene.add(std::move(v));
    attached++;
  }
  return attached;
}

// Removes the nodes of `doomed` and their descendants.
void remove_subtrees(Scene& scene, std::unordered_set<uint32_t> doomed) {
  if (doomed.empty()) return;
  // parents may come after their children: repeat until nothing is added
  for (bool grew = true; grew;) {
    grew = false;
    for (const auto& n : scene.nodes)
      if (n.parent && !doomed.count(n.id) && doomed.count(n.parent)) grew = doomed.insert(n.id).second || grew;
  }
  scene.nodes.erase(std::remove_if(scene.nodes.begin(), scene.nodes.end(),
                                   [&](const Node& n) { return doomed.count(n.id) != 0; }),
                    scene.nodes.end());
}

// What a level file brings, made ready for editing (scene.nodes[first..]): without its creatures
// and logic actors, the terrain locked, a model under each actor.
void prepare_level_nodes(Scene& scene, const AssetLibrary& library, const GameData* data, size_t first,
                         const std::string& group) {
  if (data) {
    std::unordered_set<uint32_t> doomed;
    for (size_t i = first; i < scene.nodes.size(); i++) {
      const Node& n = scene.nodes[i];
      if (n.kind == NodeKind::Actor && data->role(n.extras.value("etype", std::string())) != ActorRole::Object)
        doomed.insert(n.id);
    }
    remove_subtrees(scene, std::move(doomed));
  }
  // the terrain is locked: clicks go to what is on it. The extractor splits the level's shell
  // (tfrag) in pieces and marks the large ones og_terrain; older extractions have one mesh per
  // texture over the whole level, all terrain.
  std::unordered_map<uint32_t, size_t> index;
  for (size_t i = first; i < scene.nodes.size(); i++) index[scene.nodes[i].id] = i;
  for (size_t i = first; i < scene.nodes.size(); i++) {
    Node& n = scene.nodes[i];
    if (n.kind != NodeKind::Render) continue;
    auto terrain = n.extras.find("og_terrain");
    if (terrain != n.extras.end() && terrain->is_boolean()) {
      n.locked = terrain->get<bool>();
      // a piece is written in the level's space: its origin goes to the bottom center of its
      // geometry, so that it moves and turns around itself
      if (!n.locked && n.mesh && n.mesh.use_count() == 1 && length(n.local.t) == 0.f) {
        const AABB b = n.mesh->bounds();
        if (b.valid()) {
          const Vec3 origin{b.center().x, b.lo.y, b.center().z};
          for (auto& prim : n.mesh->prims)
            for (auto& p : prim.pos) p = p - origin;
          n.mesh->touch();
          n.local.t = origin;
        }
      }
      continue;
    }
    auto p = index.find(n.parent);
    if (p != index.end() && scene.nodes[p->second].name.find("-tfrag-") != std::string::npos) n.locked = true;
  }
  attach_actor_models(scene, library, data, first, group);
}

// Adds a level of the library under a group telling what it is to the level opened (a layer, a
// companion: see scene/story.h).
bool add_extra_level(Scene& scene, const AssetLibrary& library, const GameData* data, const LevelEntry& level,
                     const char* key) {
  Node group;
  group.name = level.name;
  group.kind = NodeKind::Group;
  group.extras = json{{key, level.name}};
  const uint32_t id = scene.add(std::move(group));
  const size_t first = scene.nodes.size();
  ImportOptions o;
  o.parent = id;
  ImportResult r;
  std::string err;
  if (!import_gltf(scene, level.background, o, &r, &err)) {
    LOG_WARN("%s: %s", level.background.c_str(), err.c_str());
    scene.remove({id});
    return false;
  }
  prepare_level_nodes(scene, library, data, first, level.name);
  // a layer of creatures only brings nothing
  bool content = false;
  for (size_t i = first; i < scene.nodes.size() && !content; i++)
    content = scene.nodes[i].kind == NodeKind::Actor || scene.nodes[i].kind == NodeKind::Render;
  if (!content) {
    scene.remove({id});
    return false;
  }
  return true;
}

std::string unique_name(const std::string& base, std::unordered_set<std::string>& used) {
  std::string name = base;
  for (int i = 2; used.count(name); i++) name = base + "-" + std::to_string(i);
  used.insert(name);
  return name;
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// document operations (no window needed)
// ------------------------------------------------------------------------------------------------

bool open_project(Scene& scene, const std::string& path, std::string* error) {
  const std::string ext = lower_ext(path);
  if (ext != ".glb" && ext != ".gltf") {
    if (error) *error = "not a .glb file";
    return false;
  }
  ImportOptions o;
  o.read_settings = true;
  ImportResult r;
  return import_gltf(scene, path, o, &r, error);
}

bool save_project(const Scene& scene, const std::string& path, std::string* error) {
  ExportOptions o;
  o.mode = ExportMode::Project;
  // write to a temporary file first: a crash while saving must not destroy the project
  const std::string tmp = path + ".tmp";
  if (!export_glb(scene, tmp, o, error)) return false;
  std::error_code ec;
  fs::rename(to_path(tmp), to_path(path), ec);
  if (ec) {
    fs::remove(to_path(path), ec);
    fs::rename(to_path(tmp), to_path(path), ec);
    if (ec) {
      if (error) *error = "cannot replace " + path;
      return false;
    }
  }
  return true;
}

bool open_level(Scene& scene, const AssetLibrary& library, const GameData* data, const LevelEntry& level,
                std::string* error) {
  ImportOptions o;
  o.game_from_file = true;
  ImportResult r;
  if (!import_gltf(scene, level.background, o, &r, error)) return false;
  // the game's name of the level (a copy found in another DGO has its folder named after both)
  scene.settings.level = level.level.empty() ? level.name : level.level;
  scene.settings.source_group = level.name;
  prepare_level_nodes(scene, library, data, 0, level.name);
  if (!data) return true;
  // the levels the game shows with it, and the layers the story lends it
  if (const LevelInfo* info = data->level(scene.settings.level))
    for (const auto& name : info->companions)
      if (const LevelEntry* c = library.level(name)) add_extra_level(scene, library, data, *c, "og_companion");
  for (const auto& name : data->layers_of(scene.settings.level))
    if (const LevelEntry* l = library.level(name)) add_extra_level(scene, library, data, *l, "og_layer");
  return true;
}

bool level_listed(const LevelEntry& level, const GameData* data) {
  if (!level.level.empty() && level.level != level.name) return false;
  if (level.collision_triangles == 0) return false;
  const LevelInfo* info = data ? data->level(level.name) : nullptr;
  return !(info && info->layer);
}

Clipboard copy_nodes(const Scene& scene, const std::vector<uint32_t>& roots) {
  Clipboard clip;
  for (auto root : roots) {
    const Node* n = scene.find(root);
    if (!n) continue;
    clip.roots.push_back(root);
    clip.root_worlds[root] = scene.world(*n);
    clip.nodes.push_back(*n);
    for (auto d : scene.descendants(root))
      if (const Node* dn = scene.find(d)) clip.nodes.push_back(*dn);
  }
  return clip;
}

std::vector<uint32_t> paste_nodes(Scene& scene, const Clipboard& clip) {
  std::unordered_map<uint32_t, uint32_t> ids;
  for (const auto& n : clip.nodes) ids[n.id] = scene.next_id++;
  std::unordered_set<std::string> names;
  for (const auto& n : scene.nodes) names.insert(n.name);
  std::vector<uint32_t> roots;
  for (const auto& src : clip.nodes) {
    Node n = src;
    n.id = ids[src.id];
    const bool is_root = clip.root_worlds.count(src.id) != 0;
    if (is_root) {
      // under its old parent when it is still in the document
      n.parent = scene.find(src.parent) ? src.parent : 0;
      n.name = unique_name(src.name, names);
    } else {
      n.parent = ids[src.parent];
    }
    if (n.kind == NodeKind::Actor) {
      // a copied actor is a new one: the game gives it its id, and names it like the others of
      // its type (<type>-<number>)
      n.extras.erase("aid");
      const std::string etype = n.extras.value("etype", std::string());
      if (is_root && !etype.empty()) {
        names.erase(n.name);
        std::string name;
        for (int i = 1; name.empty() || names.count(name); i++) name = etype + "-" + std::to_string(i);
        n.name = name;
        names.insert(name);
      }
      if (n.extras.contains("lump") && n.extras["lump"].is_object()) n.extras["lump"]["name"] = n.name;
    }
    const uint32_t id = scene.add(std::move(n));
    if (is_root) {
      scene.set_world(*scene.find(id), clip.root_worlds.at(src.id));
      roots.push_back(id);
    }
  }
  return roots;
}

// ------------------------------------------------------------------------------------------------
// library and document
// ------------------------------------------------------------------------------------------------

void App::set_library(const std::string& root, const std::string& game) {
  library_root = root.empty() ? std::string() : library_root_of(root);
  games = list_games(library_root);
  const GameFolder* chosen = nullptr;
  for (const auto& g : games)
    if (g.game == game) chosen = &g;
  if (!chosen && !games.empty()) chosen = &games.front();
  library.clear();
  store.clear();
  if (chosen) {
    std::string err;
    if (!library.scan(chosen->path, &err)) LOG_ERROR("%s", err.c_str());
  }
  auto data = game_data_for(library.game());
  library.set_listed([&](const LevelEntry& l) { return level_listed(l, data.get()); });
  catalog = build_catalog(library, data.get());
  catalog_models = list_catalog_models(library, data.get(), catalog);
  library_index.start(library, Prefs::dir() + "index", !Prefs::read_only);
  m_index_applied = false;
  all_parts.clear();
  m_all_parts_shown.clear();
  m_all_parts_filter = "";
  m_decor_level.clear();
  m_decor_scene.reset();
  m_decor_parts.clear();
  m_selected_level.clear();
  refresh_prefabs();
  Prefs& prefs = Prefs::get();
  prefs.library_root = library_root;
  prefs.library_game = chosen ? chosen->game : std::string();
  prefs.save();
}

void App::apply_library_index() {
  m_index_applied = true;
  auto data = game_data_for(library.game());
  catalog = build_catalog(library, data.get(), &library_index);
  catalog_models = list_catalog_models(library, data.get(), catalog);
  all_parts = all_decor(library_index, library);
  m_all_prototypes = m_all_pieces = 0;
  for (const auto& e : all_parts) (e.part.kind == CatalogPart::Kind::Prototype ? m_all_prototypes : m_all_pieces)++;
  m_all_parts_filter = "";
}

std::shared_ptr<const GameData> App::game_data_for(const std::string& game) {
  if (game.empty()) return nullptr;
  auto it = m_game_data.find(game);
  if (it != m_game_data.end()) return it->second;
  auto data = std::make_shared<GameData>();
  std::string err;
  std::shared_ptr<const GameData> loaded;
  if (data->load(game_data_path(game), &err)) loaded = data;
  else LOG_WARN(tr("log.game_data_missing"), game.c_str(), err.c_str());
  m_game_data[game] = loaded;
  return loaded;
}

const GameData* App::document_game_data() {
  if (!document_open()) return nullptr;
  return game_data_for(game_name(scene.settings.game)).get();
}

// ------------------------------------------------------------------------------------------------
// story view, ocean
// ------------------------------------------------------------------------------------------------

void App::story_document_changed() {
  const GameData* data = document_game_data();
  story_steps = data ? story_changes(scene, *data) : std::vector<int>{};
  if (data && data->has_story()) story.step = std::clamp(story.step, 1, data->step_count());
  apply_story_view();
  update_ocean();
}

void App::set_story_view(const StoryView& view) {
  story = view;
  if (const GameData* data = document_game_data(); data && data->has_story())
    story.step = std::clamp(story.step, 1, data->step_count());
  apply_story_view();
  update_ocean();
}

void App::apply_story_view() {
  apply_story(scene, document_game_data(), story);
  m_story_revision = history.revision();
  // what the story hides cannot stay selected
  for (auto it = selection.begin(); it != selection.end();) {
    const Node* n = scene.find(*it);
    if (n && scene.effectively_visible(*n)) ++it;
    else it = selection.erase(it);
  }
  if (!selection.count(primary)) primary = selection.empty() ? 0 : *selection.begin();
}

void App::update_ocean() {
  const GameData* data = document_game_data();
  const LevelInfo* info = data ? data->level(scene.settings.level) : nullptr;
  const OceanMap* map = info ? data->ocean(info->ocean) : nullptr;
  if (!map || map->size <= 0) {
    m_ocean.reset();
    m_ocean_map.clear();
    return;
  }
  m_ocean_height = data->ocean_height(scene.settings.level, story.enabled ? story.step : 1);
  if (m_ocean && m_ocean_map == info->ocean) return;
  m_ocean_map = info->ocean;
  // a quad per run of ocean cells along x, at height 0 (drawn at the ocean's height)
  auto mesh = std::make_shared<Mesh>();
  mesh->name = "ocean";
  Primitive p;
  const int size = map->size;
  for (int r = 0; r < size; r++) {
    for (int c = 0; c < size;) {
      if (!map->cells[(size_t)r * size + c]) {
        c++;
        continue;
      }
      const int c0 = c;
      while (c < size && map->cells[(size_t)r * size + c]) c++;
      const float x0 = map->corner.x + c0 * map->cell, x1 = map->corner.x + c * map->cell;
      const float z0 = map->corner.z + r * map->cell, z1 = z0 + map->cell;
      const uint32_t base = (uint32_t)p.pos.size();
      p.pos.insert(p.pos.end(), {{x0, 0, z0}, {x1, 0, z0}, {x1, 0, z1}, {x0, 0, z1}});
      p.nrm.insert(p.nrm.end(), 4, Vec3{0, 1, 0});
      p.idx.insert(p.idx.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
    }
  }
  p.material = 0;
  mesh->prims.push_back(std::move(p));
  m_ocean = mesh;
  Material m;
  m.name = "ocean";
  auto lift = [](float v) { return std::min(1.f, v * 1.6f + 0.08f); };
  m.base_color = {lift(map->color.x), lift(map->color.y), lift(map->color.z), 0.55f};
  m.alpha = AlphaMode::Blend;
  m_ocean_materials = {m};
}

void App::load_level(const LevelEntry& level) {
  if (m_level_future.valid()) {
    LOG_WARN("%s", tr("log.level_busy"));
    return;
  }
  stop_placement();
  m_level_name = level.name;
  AssetLibrary lib = library;  // the worker reads its own copy
  LevelEntry entry = level;
  auto data = game_data_for(lib.game());
  m_level_future = std::async(std::launch::async, [lib, entry, data]() -> std::unique_ptr<Scene> {
    auto s = std::make_unique<Scene>();
    s->settings.game = game_from_name(lib.game());
    std::string err;
    if (!open_level(*s, lib, data.get(), entry, &err)) {
      LOG_ERROR("%s: %s", entry.background.c_str(), err.c_str());
      return nullptr;
    }
    return s;
  });
  LOG_INFO(tr("log.level_opening"), level.name.c_str());
}

void App::open_project_file(const std::string& path) {
  Scene loaded;
  std::string err;
  if (!open_project(loaded, path, &err)) {
    LOG_ERROR("%s: %s", path.c_str(), err.c_str());
    return;
  }
  new_document();
  scene = std::move(loaded);
  history.clear();
  saved_revision = history.revision();
  project_path = path;
  Prefs::get().add_recent_project(path);
  Prefs::get().save();
  story_document_changed();
  frame_all();
  set_title();
  LOG_INFO(tr("log.project_opened"), path.c_str());
}

void App::save(bool save_as) {
  if (save_as || project_path.empty()) {
    open_dialog(DialogAction::SaveProjectAs);
    return;
  }
  std::string err;
  if (save_project(scene, project_path, &err)) {
    saved_revision = history.revision();
    Prefs::get().add_recent_project(project_path);
    Prefs::get().save();
    LOG_INFO(tr("log.project_saved"), project_path.c_str());
  } else {
    LOG_ERROR(tr("log.save_failed"), err.c_str());
  }
}

// ------------------------------------------------------------------------------------------------
// selection and editing
// ------------------------------------------------------------------------------------------------

void App::select_only(uint32_t id) {
  selection.clear();
  if (id) selection.insert(id);
  primary = id;
}

void App::toggle_select(uint32_t id) {
  if (!id) return;
  if (selection.count(id)) {
    selection.erase(id);
    if (primary == id) primary = selection.empty() ? 0 : *selection.begin();
  } else {
    selection.insert(id);
    primary = id;
  }
}

std::vector<uint32_t> App::selection_roots() const {
  std::vector<uint32_t> out;
  for (auto id : selection) {
    if (!scene.find(id)) continue;
    bool has_selected_ancestor = false;
    for (auto other : selection)
      if (other != id && scene.is_ancestor(other, id)) has_selected_ancestor = true;
    if (!has_selected_ancestor) out.push_back(id);
  }
  return out;
}

Mat4 App::selection_frame() const {
  auto roots = selection_roots();
  if (roots.empty()) return Mat4::identity();
  Vec3 p{0, 0, 0};
  for (auto id : roots) {
    // the node's origin, unless it is far from its geometry (meshes in world space have their
    // origin at 0,0,0): then the center of its box
    const Node* n = scene.find(id);
    Vec3 origin = scene.world(id).col3(3);
    AABB b = n ? scene.world_bounds(*n) : AABB{};
    for (auto d : scene.descendants(id))
      if (const Node* dn = scene.find(d)) b.add(scene.world_bounds(*dn));
    p += (b.valid() && !b.expanded(1.f).contains(origin)) ? b.center() : origin;
  }
  p = p / (float)roots.size();
  uint32_t ref = selection.count(primary) ? primary : roots.front();
  Transform t = Transform::from_matrix(scene.world(ref));
  return Mat4::translate(p) * Mat4::rotate(t.r);
}

PickHit App::pick_surface(const Vec2& mouse_vp) {
  PickFilter f;
  f.actors = false;
  f.include_locked = true;
  return pick(scene, camera.ray(mouse_vp.x, mouse_vp.y, m_vp_w, m_vp_h), f);
}

void App::delete_selection() {
  auto roots = selection_roots();
  if (roots.empty()) return;
  NodeChange c(scene);
  for (auto id : roots) c.save_subtree(id);
  scene.remove(roots);
  history.push(c.commit(strf(tr("undo.delete"), roots.size())));
  selection.clear();
  primary = 0;
}

void App::copy_selection() {
  auto roots = selection_roots();
  if (roots.empty()) return;
  clipboard = copy_nodes(scene, roots);
  LOG_INFO(tr("log.copied"), roots.size());
}

void App::paste() {
  if (clipboard.empty()) return;
  NodeChange c(scene);
  auto roots = paste_nodes(scene, clipboard);
  if (roots.empty()) return;
  history.push(c.commit(strf(tr("undo.paste"), roots.size())));
  selection = std::set<uint32_t>(roots.begin(), roots.end());
  primary = roots.front();
}

void App::duplicate_selection() {
  auto roots = selection_roots();
  if (roots.empty()) return;
  NodeChange c(scene);
  auto copies = paste_nodes(scene, copy_nodes(scene, roots));
  history.push(c.commit(strf(tr("undo.duplicate"), copies.size())));
  selection = std::set<uint32_t>(copies.begin(), copies.end());
  primary = copies.front();
}

// ------------------------------------------------------------------------------------------------
// prefabs
// ------------------------------------------------------------------------------------------------

std::string App::prefab_folder() const {
  return !m_prefab_dir_override.empty() ? m_prefab_dir_override : library.prefab_folder();
}

void App::refresh_prefabs() {
  const std::string folder = prefab_folder();
  prefabs = folder.empty() ? std::vector<PrefabInfo>{} : list_prefabs(folder);
}

void App::create_prefab(const std::string& name) {
  auto roots = selection_roots();
  if (roots.empty()) return;
  const std::string folder = prefab_folder();
  if (folder.empty()) {
    LOG_WARN("%s", tr("log.prefab_no_library"));
    return;
  }
  const std::string path = new_prefab_path(folder, name);
  std::string err;
  if (!save_prefab(scene, roots, path, &err)) {
    LOG_ERROR(tr("log.prefab_failed"), err.c_str());
    return;
  }
  refresh_prefabs();
  LOG_INFO(tr("log.prefab_saved"), from_path(to_path(path).stem()).c_str(), roots.size());
}

const Scene* App::prefab_scene(const std::string& path) {
  auto& s = m_prefab_scenes[path];
  if (!s) {
    auto loaded = std::make_unique<Scene>();
    std::string err;
    if (!load_prefab(path, loaded.get(), &err)) {
      LOG_ERROR("%s: %s", path.c_str(), err.c_str());
      m_prefab_scenes.erase(path);
      return nullptr;
    }
    s = std::move(loaded);
  }
  return s.get();
}

std::string Placement::key() const {
  switch (kind) {
    case Kind::Object:
    case Kind::Model: return "model:" + path;
    case Kind::Part: return "part:" + path + "#" + part.mesh_node;
    default: return path;
  }
}

void App::start_placement(const std::string& prefab_path, const std::string& name, bool once) {
  placement = Placement{};
  placement.path = prefab_path;
  placement.name = name;
  placement.once = once;
  tool = ToolMode::Place;
  store.model(prefab_path, true);  // the preview appears as soon as the file is read
}

void App::start_object_placement(const CatalogObject& object, bool once) {
  placement = Placement{};
  placement.kind = Placement::Kind::Object;
  placement.path = object.model_path;
  placement.name = object.name;
  placement.object = object;
  placement.once = once;
  tool = ToolMode::Place;
  store.model(object.model_path, true);
}

void App::start_model_placement(const CatalogModel& model, bool once) {
  placement = Placement{};
  placement.kind = Placement::Kind::Model;
  placement.path = model.path;
  placement.name = model.name;
  placement.once = once;
  tool = ToolMode::Place;
  store.model(model.path, true);
}

void App::start_part_placement(const std::string& level_background, const CatalogPart& part, bool once) {
  placement = Placement{};
  placement.kind = Placement::Kind::Part;
  placement.path = level_background;
  placement.name = part.name;
  placement.part = part;
  placement.once = once;
  tool = ToolMode::Place;
  store.level(level_background, true);
}

std::shared_ptr<Mesh> App::placement_mesh() {
  std::shared_ptr<Mesh>& mesh = m_prefab_previews[placement.key()];
  if (mesh) return mesh;
  switch (placement.kind) {
    case Placement::Kind::Prefab:
    case Placement::Kind::Object:
    case Placement::Kind::Model:
      if (placement.path.empty()) break;  // an object drawn without a model: a marker
      if (auto lm = store.model(placement.path, true)) {
        mesh = merge_mesh(scene, *lm->mesh, lm->textures, lm->materials);
        const GameData* data = game_data_for(library.game()).get();
        if (placement.kind == Placement::Kind::Object && data && data->is_water(placement.object.etype))
          style_water_mesh(scene, *mesh, placement.object.model);
      }
      break;
    case Placement::Kind::Part: {
      std::shared_ptr<Scene> level = m_decor_scene && library.level_of(placement.path) &&
                                             library.level_of(placement.path)->name == m_decor_level
                                         ? m_decor_scene
                                         : store.level(placement.path, true);
      if (!level) break;
      if (auto part = part_mesh(*level, placement.part)) mesh = merge_mesh(scene, *part, level->textures, level->materials);
      break;
    }
  }
  return mesh;
}

std::vector<uint32_t> App::place_at(const Mat4& m) {
  std::vector<uint32_t> roots;
  if (placement.kind == Placement::Kind::Prefab) {
    const Scene* prefab = prefab_scene(placement.path);
    if (!prefab) return roots;
    NodeChange c(scene);
    roots = place_prefab(scene, *prefab, m, &m_prefab_meshes[placement.path]);
    if (roots.empty()) return roots;
    history.push(c.commit(strf(tr("undo.place"), placement.name.c_str())));
  } else {
    std::shared_ptr<Mesh> mesh = placement_mesh();
    const bool markerless = placement.kind == Placement::Kind::Object && placement.path.empty();
    if (!mesh && !markerless) return roots;  // still loading
    std::unordered_set<std::string> names;
    for (const auto& n : scene.nodes) names.insert(n.name);
    NodeChange c(scene);
    if (placement.kind == Placement::Kind::Object) {
      // an actor named like the game names them: <type>-<number>
      std::string name;
      for (int i = 1; name.empty() || names.count(name); i++) name = placement.object.etype + "-" + std::to_string(i);
      Node actor = make_catalog_actor(placement.object, name);
      actor.local = Transform::from_matrix(m);
      const uint32_t id = scene.add(std::move(actor));
      if (mesh) {
        Node visual;
        visual.parent = id;
        visual.name = placement.object.model;
        visual.kind = NodeKind::Render;
        visual.mesh = mesh;
        visual.extras = json{{"og_actor_visual", placement.object.model}};
        scene.add(std::move(visual));
      }
      roots.push_back(id);
    } else if (placement.kind == Placement::Kind::Model) {
      Node n;
      n.name = unique_name(placement.name, names);
      n.kind = NodeKind::Render;
      n.mesh = mesh;
      n.local = Transform::from_matrix(m);
      roots.push_back(scene.add(std::move(n)));
    } else {
      std::string base = placement.part.name;
      if (base.size() > 3 && base.compare(base.size() - 3, 3, ".mb") == 0) base.resize(base.size() - 3);
      Node n;
      n.name = unique_name(base, names);
      n.kind = NodeKind::Render;
      n.mesh = mesh;
      n.local = Transform::from_matrix(m);
      roots.push_back(scene.add(std::move(n)));
    }
    history.push(c.commit(strf(tr("undo.place"), placement.name.c_str())));
  }
  selection = std::set<uint32_t>(roots.begin(), roots.end());
  primary = roots.front();
  return roots;
}

void App::new_level() {
  new_document();
  // a document without a level of the game: no story, no ocean
  scene.settings.level = "custom-level";
  story_document_changed();
  set_title();
  LOG_INFO("%s", tr("log.new_level"));
}

void App::stop_placement() {
  tool = ToolMode::Select;
  placement = Placement{};
  m_place_valid = false;
}

Mat4 App::placement_matrix(const PickHit& hit, const Ray& ray) const {
  Vec3 p;
  if (hit.hit()) {
    p = hit.point;
  } else {
    float t;
    if (ray_plane(ray, {0, 0, 0}, {0, 1, 0}, &t) && t > 0 && t < 2000.f) p = ray.at(t);
    else p = ray.at(15.f);
  }
  if (gizmo.snap.enabled && gizmo.snap.translate > 0) {
    const float s = gizmo.snap.translate;
    p.x = std::round(p.x / s) * s;
    p.z = std::round(p.z / s) * s;
  }
  return Mat4::translate(p) * Mat4::rotate(Quat::axis_angle({0, 1, 0}, placement.yaw));
}

// ------------------------------------------------------------------------------------------------
// native file dialogs (SDL3). The callback may run on another thread: results are queued.
// ------------------------------------------------------------------------------------------------

struct DialogRequest {
  App* app;
  DialogAction action;
};

void dialog_callback(void* userdata, const char* const* filelist, int /*filter*/) {
  auto* req = (DialogRequest*)userdata;
  if (filelist && filelist[0]) {
    std::lock_guard<std::mutex> lock(req->app->m_dialog_mutex);
    req->app->m_dialog_results.push_back({req->action, filelist[0]});
  }
  delete req;
}

void App::open_dialog(DialogAction action) {
  static const SDL_DialogFileFilter project_filter[] = {{"glTF (.glb)", "glb"}};
  static const SDL_DialogFileFilter iso_filter[] = {{"Disc image (.iso)", "iso"}};
  auto* req = new DialogRequest{this, action};
  switch (action) {
    case DialogAction::OpenProject:
      SDL_ShowOpenFileDialog(dialog_callback, req, m_window, project_filter, 1, nullptr, false);
      break;
    case DialogAction::SaveProjectAs:
      SDL_ShowSaveFileDialog(dialog_callback, req, m_window, project_filter, 1, nullptr);
      break;
    case DialogAction::ExtractSourceIso:
      SDL_ShowOpenFileDialog(dialog_callback, req, m_window, iso_filter, 1, nullptr, false);
      break;
    case DialogAction::ExtractSourceFolder:
    case DialogAction::ExtractOutput:
      SDL_ShowOpenFolderDialog(dialog_callback, req, m_window, nullptr, false);
      break;
  }
}

void App::process_dialog_results() {
  std::vector<DialogResult> results;
  {
    std::lock_guard<std::mutex> lock(m_dialog_mutex);
    results.swap(m_dialog_results);
  }
  for (auto& r : results) {
    switch (r.action) {
      case DialogAction::OpenProject:
        open_project_file(r.path);
        break;
      case DialogAction::SaveProjectAs: {
        std::string p = r.path;
        if (lower_ext(p) != ".glb") p += ".glb";
        project_path = p;
        save(false);
        break;
      }
      case DialogAction::ExtractSourceIso:
      case DialogAction::ExtractSourceFolder:
        extract.source = r.path;
        extract_detect(r.path);
        break;
      case DialogAction::ExtractOutput:
        extract.output = r.path;
        break;
    }
  }
}

void App::confirm_discard(std::function<void()> action) {
  if (history.revision() == saved_revision || !document_open()) {
    action();
    return;
  }
  m_pending_discard = std::move(action);
  m_show_discard = true;
}

void App::request_quit() {
  if (m_quit_confirmed) {
    m_running = false;
    return;
  }
  confirm_discard([this]() {
    m_quit_confirmed = true;
    m_running = false;
  });
}

void App::set_language(const std::string& code) {
  if (!i18n_select(code)) return;
  Prefs::get().language = code;
  Prefs::get().save();
}

// ------------------------------------------------------------------------------------------------
// backups: a snapshot a few seconds after the last change, 10 kept
// ------------------------------------------------------------------------------------------------

void App::autosave_tick() {
  uint64_t rev = history.revision();
  if (rev != m_last_seen_revision) {
    m_last_seen_revision = rev;
    m_last_change_time = m_time;
  }
  if (project_path.empty() || rev == m_backup_revision || rev == saved_revision) return;
  if (m_time - m_last_change_time < 5.0 || m_time - m_last_backup_time < 60.0) return;
  fs::path dir = to_path(project_path + ".backups");
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::time_t now = std::time(nullptr);
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
  std::string err;
  if (save_project(scene, from_path(dir / (std::string(stamp) + ".glb")), &err)) {
    m_backup_revision = rev;
    m_last_backup_time = m_time;
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(dir, ec))
      if (e.path().extension() == ".glb") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    while (files.size() > 10) {
      fs::remove(files.front(), ec);
      files.erase(files.begin());
    }
  }
}

}  // namespace ogle
