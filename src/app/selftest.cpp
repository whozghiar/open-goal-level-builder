// --selftest: exercises the editor without a window. Writes a small library in ogle-extract's
// layout, lists and opens its level (decor instances, locked terrain, actor with its model,
// collision), copies and pastes elements, makes and places a prefab, round-trips a project, opens
// a level with game data (creatures left out, water models, companion and layer levels, the story
// view, the catalog of objects and decor parts), checks the remote control of the MCP server, the
// translations and the game data shipped with the editor.

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>
#include <set>
#include <thread>

#include "app/app.h"
#include "app/remote.h"
#include "core/i18n.h"
#include "core/log.h"
#include "io/asset_library.h"
#include "io/catalog.h"
#include "io/game_data.h"
#include "io/gltf_io.h"
#include "io/library_index.h"
#include "io/prefabs.h"
#include "scene/story.h"

namespace ogle {

namespace {

namespace fs = std::filesystem;

int g_failures = 0;

void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
  if (!ok) g_failures++;
}

// a box of size 2, from y = 0 to 2
std::shared_ptr<Mesh> make_box(const std::string& name, int material) {
  auto mesh = std::make_shared<Mesh>();
  mesh->name = name;
  Primitive p;
  for (int i = 0; i < 8; i++) p.pos.push_back({(i & 1) ? 1.f : -1.f, (i & 2) ? 2.f : 0.f, (i & 4) ? 1.f : -1.f});
  const uint32_t faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
  for (auto& f : faces) p.idx.insert(p.idx.end(), {f[0], f[1], f[2], f[0], f[2], f[3]});
  p.material = material;
  p.compute_normals();
  mesh->prims.push_back(std::move(p));
  return mesh;
}

std::shared_ptr<Mesh> make_floor(int material) {
  auto mesh = std::make_shared<Mesh>();
  mesh->name = "floor";
  Primitive p;
  p.pos = {{-50, 0, -50}, {50, 0, -50}, {50, 0, 50}, {-50, 0, 50}};
  p.uv = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  p.idx = {0, 2, 1, 0, 3, 2};
  p.material = material;
  p.compute_normals();
  mesh->prims.push_back(std::move(p));
  return mesh;
}

// Copies a .glb with its JSON chunk edited (the binary chunk is kept as is).
bool rewrite_glb_json(const fs::path& in, const fs::path& out, const std::function<void(json&)>& edit) {
  std::ifstream f(in, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (bytes.size() < 20) return false;
  uint32_t json_len;
  std::memcpy(&json_len, &bytes[12], 4);
  json j = json::parse(std::string(bytes.begin() + 20, bytes.begin() + 20 + json_len));
  edit(j);
  std::string text = j.dump();
  while (text.size() % 4) text += ' ';
  std::vector<uint8_t> rest(bytes.begin() + 20 + json_len, bytes.end());
  uint32_t total = (uint32_t)(12 + 8 + text.size() + rest.size());
  uint32_t header[5] = {0x46546C67u, 2, total, (uint32_t)text.size(), 0x4E4F534Au};
  std::ofstream o(out, std::ios::binary);
  o.write((const char*)header, sizeof(header));
  o.write(text.data(), (std::streamsize)text.size());
  o.write((const char*)rest.data(), (std::streamsize)rest.size());
  return (bool)o;
}

void write_text(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

// the printf arguments of a text: "%zu objects, %s" -> "zs"
std::string format_args(const std::string& s) {
  static const std::regex spec("%[-+ #0]*[0-9]*(\\.[0-9]+)?(hh|h|ll|l|z|j|t|L)?([diouxXeEfgGcsp%])");
  std::string out;
  for (auto it = std::sregex_iterator(s.begin(), s.end(), spec); it != std::sregex_iterator(); ++it) {
    const std::string conv = (*it)[3].str();
    if (conv != "%") out += (*it)[2].str() + conv;
  }
  return out;
}

json read_json_file(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return json::parse(text, nullptr, false);
}

// A level written like ogle-extract writes one: a background with the terrain (tfrag), a decor
// prototype with two instances (TIE), collision and an actor; the model of its actor type; the
// models every level shares.
bool write_library(const fs::path& root) {
  std::string err;
  const fs::path game = root / "jak2";
  write_text(game / "library.json", R"({"game": "jak2", "name": "Jak II", "levels": ["LEV.DGO"]})");
  write_text(game / "lev" / "level.json", R"({"level": "lev", "dgo": "LEV.DGO", "actors": 1})");
  fs::create_directories(game / "_disc" / "DGO");
  write_text(game / "_disc" / "DGO" / "stray.glb", "not a level");

  Scene bg;
  Material stone;
  stone.name = "stone";
  const int mat = bg.add_material(stone);
  Node tfrag{};
  tfrag.name = "lev-tfrag-0";
  const uint32_t tfrag_id = bg.add(tfrag);
  Node floor{};
  floor.name = "floor-0";
  floor.kind = NodeKind::Render;
  floor.parent = tfrag_id;
  floor.mesh = make_floor(mat);
  bg.add(floor);
  Node tie{};
  tie.name = "lev-tie-0";
  const uint32_t tie_id = bg.add(tie);
  Node proto{};
  proto.name = "rock.mb";
  proto.parent = tie_id;
  const uint32_t proto_id = bg.add(proto);
  auto rock = make_box("rock.mb", mat);
  for (int i = 0; i < 2; i++) {
    Node inst{};
    inst.name = "rock.mb-" + std::to_string(i);
    inst.kind = NodeKind::Render;
    inst.parent = proto_id;
    inst.mesh = rock;
    inst.local.t = {i * 10.f, 0, 0};
    bg.add(inst);
  }
  Node col{};
  col.name = "lev-collision";
  col.kind = NodeKind::Collision;
  col.mesh = make_floor(-1);
  bg.add(col);
  const fs::path project = root / "bg-project.glb";
  ExportOptions eo;
  if (!export_glb(bg, project.string(), eo, &err)) return false;
  // as a rip has them: no editor extras, collision marked for the builder, an actor node
  fs::create_directories(game / "lev" / "decor");
  bool ok = rewrite_glb_json(project, game / "lev" / "decor" / "lev-background.glb", [](json& j) {
    for (auto& n : j["nodes"]) {
      const std::string kind = n["extras"].value("og_kind", std::string());
      n["extras"] = json::object();
      if (kind == "collision") n["extras"] = {{"set_collision", 1}, {"set_invisible", 1}, {"collide_pat", 4660}};
    }
    j["nodes"].push_back({{"name", "crate-9"},
                          {"translation", {1.0, 2.0, 3.0}},
                          {"extras",
                           {{"kind", "actor"},
                            {"etype", "crate"},
                            {"aid", 123},
                            {"game_task", 0},
                            {"bsphere", {1.0, 2.0, 3.0, 5.0}},
                            {"lump", {{"name", "crate-9"}}}}}});
    j["scenes"][0]["nodes"].push_back((int)j["nodes"].size() - 1);
    j["asset"]["extras"] = {{"opengoal", {{"game", "jak2"}, {"level", "lev"}}}};
  });
  // the models: one for the actor, one shared by every level
  for (auto [folder, name] : {std::pair<const char*, const char*>{"lev", "crate-krimson-lod0"},
                             {"lev", "crate-krimson-lod1"},
                             {"common", "collectables-skill-lod0"}}) {
    Scene m;
    Node n{};
    n.name = name;
    n.kind = NodeKind::Render;
    n.mesh = make_box(name, -1);
    m.add(n);
    fs::create_directories(game / folder / "models");
    ok &= export_glb(m, (game / folder / "models" / (std::string(name) + ".glb")).string(), eo, &err);
  }
  return ok;
}

// A level as ogle-extract writes one, with the decor of `bg` and rip actors (glTF nodes with
// their extras).
bool write_rip_level(const fs::path& game, const std::string& name, const Scene& bg, const json& actors,
                     int collision_triangles, const json& node_extras = json::object(),
                     const json& actor_types = json::object()) {
  std::string err;
  const fs::path tmp = game / (name + "-project.glb");
  ExportOptions eo;
  if (!export_glb(bg, tmp.string(), eo, &err)) return false;
  fs::create_directories(game / name / "decor");
  const bool ok = rewrite_glb_json(tmp, game / name / "decor" / (name + "-background.glb"), [&](json& j) {
    for (auto& n : j["nodes"]) n["extras"] = node_extras.value(n.value("name", std::string()), json::object());
    for (const auto& a : actors) {
      j["nodes"].push_back(a);
      j["scenes"][0]["nodes"].push_back((int)j["nodes"].size() - 1);
    }
    j["asset"]["extras"] = {{"opengoal", {{"game", "jak2"}, {"level", name}}}};
  });
  fs::remove(tmp);
  write_text(game / name / "level.json", json{{"level", name},
                                              {"dgo", "X.DGO"},
                                              {"actors", actors.size()},
                                              {"actor_types", actor_types},
                                              {"collision_triangles", collision_triangles}}
                                             .dump());
  return ok;
}

json rip_actor(const std::string& name, const std::string& etype, const json& lump, Vec3 at = {}) {
  json l = lump;
  l["name"] = name;
  return {{"name", name},
          {"translation", {at.x, at.y, at.z}},
          {"extras", {{"kind", "actor"}, {"etype", etype}, {"aid", 1}, {"game_task", 0}, {"lump", l}}}};
}

// A game with a story: the level "town" (a rock the mission breaks into rubble, a crate there
// until the mission ends, a statue broken by it, a creature, a logic actor, a cutscene character,
// a water pool), the hut the game shows with it, the layer of the mission's actors.
const char* kStoryGameData = R"({
 "format": 1, "game": "jak2",
 "actors": {"roles": {"grunt": "creature", "part-spawner": "logic"}, "creature_suffixes": ["-highres"],
            "effects": ["part-spawner"],
            "water_types": ["water-anim"], "water_looks": ["water-anim-test-pool"],
            "objects": {"crate": "crate", "statue-broken": "breakable", "no-model-thing": "other"}},
 "oceans": {"*ocean-map-test*": {"corner": [-96, -1.5, -96], "cell": 96, "cells": ["11", "01"], "color": [0.1, 0.2, 0.3]}},
 "levels": {
  "town": {"taskname": "town", "base_mask": 1, "ocean": "*ocean-map-test*", "layer": false, "traffic": false, "companions": ["hut"]},
  "hut": {"taskname": "town", "base_mask": 1, "ocean": null, "layer": false, "traffic": false, "companions": []},
  "lmission": {"taskname": "lmission", "base_mask": 0, "ocean": null, "layer": true, "traffic": false, "companions": []}
 },
 "story": {
  "reset_node": "start",
  "mask_bits": {"task0": 0, "task1": 1, "done": 8, "special": 12, "primary0": 13, "ctywide": 14, "never": 15},
  "nodes": [
   {"name": "none", "task": "", "level": ""},
   {"name": "start", "task": "intro", "level": "", "closed": true, "borrow": [["town", 0, null, null]]},
   {"name": "town-mission-introduction", "task": "town-mission", "level": "town", "parents": [1],
    "borrow": [["town", 0, "lmission", "display"]]},
   {"name": "town-mission-resolution", "task": "town-mission", "level": "town", "mask_op": "abs", "mask": 2,
    "close_task": true, "parents": [2], "borrow": [["town", 0, "lmission", "display"]]},
   {"name": "end", "task": "end", "level": "", "parents": [3]}
  ],
  "prototypes": {"town": [{"names": ["rock"], "when": ["not", ["complete", "town-mission"]]},
                          {"names": ["rubble"], "when": ["closed", "town-mission-resolution"]}]},
  "types": {"statue-broken": ["closed", "town-mission-resolution"]},
  "ocean_heights": {}
 }
})";

bool write_story_library(const fs::path& root) {
  const fs::path game = root / "jak2";
  write_text(game / "library.json", R"({"game": "jak2", "name": "Jak II", "levels": ["X.DGO"]})");
  bool ok = true;
  auto decor = [](const std::string& level, std::vector<std::string> protos) {
    Scene bg;
    Node tfrag{};
    tfrag.name = level + "-tfrag-0";
    const uint32_t t = bg.add(tfrag);
    Node floor{};
    floor.name = level + "-floor";
    floor.kind = NodeKind::Render;
    floor.parent = t;
    floor.mesh = make_floor(-1);
    bg.add(floor);
    // a piece of the shell standing apart (the extractor splits the tfrag): a small wall
    Node wall{};
    wall.name = level + "-wall";
    wall.kind = NodeKind::Render;
    wall.parent = t;
    wall.mesh = make_box(level + "-wall", -1);
    for (auto& v : wall.mesh->prims[0].pos) v = v + Vec3{20, 0, 10};  // in the level's space, as ripped
    bg.add(wall);
    Node tie{};
    tie.name = level + "-tie-0";
    const uint32_t tie_id = bg.add(tie);
    for (const auto& name : protos) {
      Node proto{};
      proto.name = name + ".mb";
      proto.parent = tie_id;
      const uint32_t id = bg.add(proto);
      Node inst{};
      inst.name = name + ".mb-0";
      inst.kind = NodeKind::Render;
      inst.parent = id;
      inst.mesh = make_box(name, -1);
      bg.add(inst);
    }
    return bg;
  };
  const json town_actors = json::array({
      rip_actor("crate-1", "crate", {{"kill-mask", 4294905854.0}}),  // 0xffff0ffe: while the mask is task0
      rip_actor("grunt-2", "grunt", json::object()),
      rip_actor("part-spawner-3", "part-spawner", {{"art-name", "group-town-torch"}}),
      rip_actor("jak-highres-4", "jak-highres", {{"kill-mask", 32768}}),
      rip_actor("pool-5", "water-anim", {{"look", 0}}),
      rip_actor("statue-broken-6", "statue-broken", json::object()),
  });
  const json terrain = {{"town-floor", {{"og_terrain", true}}}, {"town-wall", {{"og_terrain", false}}}};
  const json town_types = {{"crate", {{"count", 1}, {"lump", {{"name", "crate-1"}, {"eco-info", {13, 10}},
                                                            {"kill-mask", 4294905854.0}, {"next-actor", "x"}}}}}};
  ok &= write_rip_level(game, "town", decor("town", {"rock", "rubble"}), town_actors, 2, terrain, town_types);
  ok &= write_rip_level(game, "hut", decor("hut", {}), json::array(), 2);
  ok &= write_rip_level(game, "lmission", Scene(), json::array({rip_actor("lcrate-1", "crate", json::object())}), 0);
  // the models: the pool's look, the crate
  std::string err;
  fs::create_directories(game / "town" / "models");
  ExportOptions eo;
  for (const char* model : {"water-anim-test-pool-lod0", "crate-test-lod0"}) {
    Scene m;
    Node n{};
    n.name = model;
    n.kind = NodeKind::Render;
    n.mesh = make_box(n.name, -1);
    m.add(n);
    ok &= export_glb(m, (game / "town" / "models" / (std::string(model) + ".glb")).string(), eo, &err);
  }
  write_text(root / "game-data.json", kStoryGameData);
  return ok;
}

}  // namespace

int run_selftest() {
  std::printf("open-goal-level-editor selftest\n");
  const fs::path tmp = fs::temp_directory_path() / "ogle-selftest";
  std::error_code ec;
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp);
  std::string err;

  // math
  {
    Quat q = Quat::from_euler_deg({10, 35, -20});
    Vec3 e = q.to_euler_deg();
    check(std::fabs(e.x - 10) < 0.01f && std::fabs(e.y - 35) < 0.01f && std::fabs(e.z + 20) < 0.01f,
          "euler <-> quaternion");
    Transform t{{1, 2, 3}, q, {2, 3, 4}};
    Transform back = Transform::from_matrix(t.matrix());
    check(length(back.t - t.t) < 1e-4f && length(back.s - t.s) < 1e-4f &&
              std::fabs(std::fabs(back.r.w) - std::fabs(q.w)) < 1e-4f,
          "TRS -> matrix -> TRS");
    Mat4 m = t.matrix();
    Mat4 id = m * m.inverse();
    check(std::fabs(id.at(0, 0) - 1) < 1e-4f && std::fabs(id.at(1, 3)) < 1e-4f, "matrix inverse");
  }

  // the library
  const fs::path lib_root = tmp / "library";
  check(write_library(lib_root), "library written in ogle-extract's layout");
  auto games = list_games(lib_root.string());
  check(games.size() == 1 && games[0].game == "jak2" && games[0].name == "Jak II" && games[0].levels == 1,
        "library: its games (library.json)");
  check(fs::path(library_root_of((lib_root / "jak2").string())) == fs::absolute(lib_root),
        "library: a game folder chosen means its parent");
  AssetLibrary library;
  bool scanned = !games.empty() && library.scan(games[0].path, &err);
  const LevelEntry* level = library.level("lev");
  check(scanned && library.levels().size() == 1 && level && level->dgo == "LEV.DGO" && level->actors == 1 &&
            library.game() == "jak2",
        "library: one level with its level.json, _disc skipped");
  auto model = [&](const char* etype) {
    const ModelAsset* m = library.model_for_etype(etype, "lev");
    return m ? m->name + ":" + std::to_string(m->lod) : std::string();
  };
  check(model("crate") == "crate-krimson:0" && model("skill") == "collectables-skill:0" && model("babak").empty(),
        "actor models: by name start and end, lowest lod first");

  // the level
  Scene scene;
  bool opened = level && open_level(scene, library, nullptr, *level, &err);
  const Node* actor = nullptr;
  std::vector<uint32_t> instances;
  uint32_t floor = 0, collision = 0;
  for (const auto& n : scene.nodes) {
    if (n.kind == NodeKind::Actor) actor = &n;
    if (n.kind == NodeKind::Collision) collision = n.id;
    if (n.name == "floor-0") floor = n.id;
    if (n.name.rfind("rock.mb-", 0) == 0) instances.push_back(n.id);
  }
  check(opened && scene.settings.level == "lev" && scene.settings.game == Game::Jak2, "level opened: name and game");
  check(floor && scene.find(floor)->locked && instances.size() == 2 && !scene.find(instances[0])->locked &&
            scene.find(instances[0])->mesh == scene.find(instances[1])->mesh,
        "level: terrain locked, decor instances selectable and sharing their mesh");
  bool actor_model = false;
  if (actor)
    for (auto c : scene.children(actor->id))
      actor_model |= scene.find(c)->extras.value("og_actor_visual", std::string()) == "crate-krimson";
  check(actor && actor->extras.value("etype", std::string()) == "crate" && actor->extras.value("aid", 0) == 123 &&
            actor_model && length(actor->local.t - Vec3{1, 2, 3}) < 1e-5f,
        "level: actor with its data, position and model");
  check(collision && !scene.find(collision)->mesh->prims.empty() &&
            scene.find(collision)->mesh->prims[0].tri_pat == std::vector<uint32_t>(2, 4660u),
        "level: collision kept with its surface");

  // copy and paste
  if (instances.size() == 2 && actor) {
    NodeChange change(scene);
    const size_t before = scene.nodes.size();
    auto pasted = paste_nodes(scene, copy_nodes(scene, {instances[0], actor->id}));
    const Node* copy = pasted.size() == 2 ? scene.find(pasted[0]) : nullptr;
    const Node* actor_copy = pasted.size() == 2 ? scene.find(pasted[1]) : nullptr;
    const Node* original = scene.find(instances[0]);
    check(copy && copy->mesh == original->mesh && copy->parent == original->parent && copy->name != original->name &&
              length(scene.world(*copy).col3(3) - scene.world(*original).col3(3)) < 1e-5f,
          "paste: a copy in place, sharing the mesh, under the same parent");
    check(actor_copy && actor_copy->kind == NodeKind::Actor && !actor_copy->extras.contains("aid") &&
              scene.children(actor_copy->id).size() == 1,
          "paste: an actor copy with its model and without the original's id");
    HistoryEntry e = change.commit("paste");
    e.undo();
    check(scene.nodes.size() == before, "paste undone");
    e.redo();
    check(scene.nodes.size() == before + 3, "paste redone (the instance, the actor and its model)");
  }

  // prefabs
  const fs::path prefabs = tmp / "prefabs";
  const std::string prefab_path = new_prefab_path(prefabs.string(), "Rocks & crate");
  check(fs::path(prefab_path).filename() == "Rocks_crate.glb", "prefab: a file name from its name");
  bool saved = instances.size() == 2 && save_prefab(scene, instances, prefab_path, &err);
  check(saved && fs::path(new_prefab_path(prefabs.string(), "Rocks & crate")).filename() == "Rocks_crate-2.glb",
        "prefab saved, the next one of that name gets another file");
  auto listed = list_prefabs(prefabs.string());
  check(listed.size() == 1 && listed[0].name == "Rocks_crate", "prefabs listed");
  Scene prefab;
  bool loaded = saved && load_prefab(prefab_path, &prefab, &err);
  AABB pb = prefab.bounds();
  check(loaded && prefab.nodes.size() == 2 && pb.valid() && std::fabs(pb.lo.y) < 1e-4f &&
            std::fabs(pb.center().x) < 1e-4f && std::fabs(pb.center().z) < 1e-4f,
        "prefab: its elements around a pivot at the bottom center");
  if (loaded) {
    std::map<const Mesh*, std::shared_ptr<Mesh>> merged;
    const size_t before = scene.nodes.size();
    auto a = place_prefab(scene, prefab, Mat4::translate({100, 5, 0}), &merged);
    auto b = place_prefab(scene, prefab, Mat4::translate({200, 5, 0}), &merged);
    AABB placed;
    for (auto id : a) placed.add(scene.world_bounds(*scene.find(id)));
    check(a.size() == 2 && b.size() == 2 && scene.nodes.size() == before + 4 &&
              scene.find(a[0])->mesh == scene.find(b[0])->mesh && std::fabs(placed.lo.y - 5) < 1e-4f &&
              std::fabs(placed.center().x - 100) < 1e-3f,
          "prefab placed twice: on the point given, meshes shared");
  }
  {
    // an actor of a prefab placed twice: two actors of a level cannot share a name
    Scene with_actor, doc;
    Node crate{};
    crate.name = "crate-1";
    crate.kind = NodeKind::Actor;
    crate.extras = json{{"etype", "crate"}, {"lump", {{"name", "crate-1"}}}};
    with_actor.add(crate);
    std::map<const Mesh*, std::shared_ptr<Mesh>> merged;
    const auto first = place_prefab(doc, with_actor, Mat4::identity(), &merged);
    const auto second = place_prefab(doc, with_actor, Mat4::translate({4, 0, 0}), &merged);
    const Node* x = first.empty() ? nullptr : doc.find(first[0]);
    const Node* y = second.empty() ? nullptr : doc.find(second[0]);
    check(x && y && x->name == "crate-1" && y->name == "crate-2" &&
              y->extras["lump"].value("name", std::string()) == "crate-2",
          "prefab: its actors named like the game names them, in their data too");
  }

  // project round trip
  {
    Material glow;
    glow.name = "glow";
    glow.alpha = AlphaMode::Blend;
    glow.blend = BlendMode::Add;
    glow.cutoff = 0.15f;  // the alpha test of the PS2, kept by blended surfaces too
    Node n{};
    n.name = "glow";
    n.kind = NodeKind::Render;
    n.mesh = make_box("glow", scene.add_material(glow));
    n.local.s = {3.f, 1.f, 0.5f};  // stretched along x, flattened along z
    scene.add(n);
    const std::string project = (tmp / "project.glb").string();
    Scene back;
    bool ok = save_project(scene, project, &err) && open_project(back, project, &err);
    size_t kinds[(int)NodeKind::Count] = {}, kinds_back[(int)NodeKind::Count] = {};
    for (const auto& x : scene.nodes) kinds[(int)x.kind]++;
    for (const auto& x : back.nodes) kinds_back[(int)x.kind]++;
    check(ok && std::equal(std::begin(kinds), std::end(kinds), std::begin(kinds_back)) &&
              back.settings.level == "lev" && back.settings.game == Game::Jak2,
          "project: saved and opened again, same nodes and level");
    std::set<const Mesh*> rocks;
    bool locked = false, pats = false, blend = false, aid = false, stretched = false;
    for (const auto& x : back.nodes) {
      // the level's instances and the pasted copy (the prefab placements have their own mesh)
      if (x.name.rfind("rock.mb", 0) == 0 && x.kind == NodeKind::Render && x.parent) rocks.insert(x.mesh.get());
      if (x.name == "floor-0") locked = x.locked;
      if (x.kind == NodeKind::Collision) pats = x.mesh->prims[0].tri_pat == std::vector<uint32_t>(2, 4660u);
      if (x.kind == NodeKind::Actor && x.extras.value("aid", 0) == 123) aid = true;
      if (x.name == "glow") {
        const Material& m = back.materials[x.mesh->prims[0].material];
        blend = m.blend == BlendMode::Add && std::fabs(m.cutoff - 0.15f) < 1e-6f;
        const AABB b = back.world_bounds(x);
        stretched = length(x.local.s - Vec3{3.f, 1.f, 0.5f}) < 1e-4f && std::fabs(b.hi.x - b.lo.x - 6.f) < 1e-3f &&
                    std::fabs(b.hi.z - b.lo.z - 1.f) < 1e-3f;
      }
    }
    check(stretched, "project: the scale of a stretched element kept (and its bounds)");
    check(rocks.size() == 1 && locked && pats && aid && blend,
          "project: instancing, locks, collision surfaces, actor data, additive material and its alpha test kept");
    Scene empty, empty_back;
    const std::string empty_path = (tmp / "empty.glb").string();
    check(save_project(empty, empty_path, &err) && open_project(empty_back, empty_path, &err) &&
              empty_back.nodes.empty(),
          "project: an empty document saved and opened again");
  }

  // a level with game data: creatures and logic left out, water models, companion and layer
  // levels, the story view
  try {
    const fs::path story_root = tmp / "story-library";
    check(write_story_library(story_root), "story: a library with a story written");
    GameData data;
    const bool read = data.load((story_root / "game-data.json").string(), &err);
    check(read && data.role("grunt") == ActorRole::Creature && data.role("part-spawner") == ActorRole::Logic &&
              data.role("jak-highres") == ActorRole::Creature && data.role("crate") == ActorRole::Object &&
              data.water_model(0) == "water-anim-test-pool" && data.step_count() == 5,
          "game data: roles, water looks, story nodes");
    AssetLibrary story_lib;
    const bool scanned_story = story_lib.scan((story_root / "jak2").string(), &err);
    std::string listed_levels;
    for (const auto& l : story_lib.levels())
      if (level_listed(l, &data)) listed_levels += l.name + " ";
    check(scanned_story && listed_levels == "hut town ", "levels listed: the layer of a mission is not");
    Scene town;
    const LevelEntry* town_entry = story_lib.level("town");
    const bool town_opened = town_entry && open_level(town, story_lib, &data, *town_entry, &err);
    auto node_named = [&](const std::string& name) -> Node* {
      for (auto& x : town.nodes)
        if (x.name == name) return &x;
      return nullptr;
    };
    std::string actors;
    for (const auto& x : town.nodes)
      if (x.kind == NodeKind::Actor) actors += x.name + " ";
    check(town_opened && actors.find("grunt") == std::string::npos && actors.find("part-spawner") == std::string::npos &&
              actors.find("jak-highres") == std::string::npos && node_named("crate-1") && node_named("statue-broken-6"),
          "level with game data: creatures, logic actors and cutscene characters left out");
    bool pool_model = false;
    if (Node* pool = node_named("pool-5"))
      for (auto c : town.children(pool->id))
        pool_model |= town.find(c)->extras.value("og_actor_visual", std::string()) == "water-anim-test-pool";
    check(pool_model, "water actor: the model of its look");
    Node* hut = nullptr;
    Node* layer = nullptr;
    for (auto& x : town.nodes) {
      if (x.extras.value("og_companion", std::string()) == "hut") hut = &x;
      if (x.extras.value("og_layer", std::string()) == "lmission") layer = &x;
    }
    check(hut && layer && !town.children(hut->id).empty() && node_named("lcrate-1"),
          "level with game data: its companion and the layer of its mission under their groups");
    // the story: step 1 (the mission under way), step 4 (the mission over)
    auto shown = [&](const char* name) {
      Node* x = node_named(name);
      return x && town.effectively_visible(*x);
    };
    StoryView view;
    view.step = 1;
    apply_story(town, &data, view);
    const bool start = shown("crate-1") && !shown("statue-broken-6") && shown("rock.mb") && !shown("rubble.mb") &&
                       shown("lcrate-1") && shown("hut");
    view.step = 4;
    apply_story(town, &data, view);
    const bool after = !shown("crate-1") && shown("statue-broken-6") && !shown("rock.mb") && shown("rubble.mb") &&
                       !shown("lcrate-1") && shown("hut");
    view.enabled = false;
    apply_story(town, &data, view);
    const bool all = shown("crate-1") && shown("statue-broken-6") && shown("rock.mb") && shown("rubble.mb") &&
                     shown("lcrate-1");
    check(start && after && all,
          "story: kill masks, type and prototype rules, borrowed layer at two steps; everything when off");
    check(story_changes(town, data) == std::vector<int>{4}, "story: the steps where the level changes");
    check(data.ocean("*ocean-map-test*") && data.ocean("*ocean-map-test*")->cells == std::vector<uint8_t>{1, 1, 0, 1} &&
              std::fabs(data.ocean_height("town", 1) + 1.5f) < 1e-5f,
          "game data: the ocean of a level, its cells and height");

    // the shell split in pieces: the terrain locked, the others free, around their own origin
    Node* floor_piece = node_named("town-floor");
    Node* wall_piece = node_named("town-wall");
    check(floor_piece && floor_piece->locked && wall_piece && !wall_piece->locked &&
              length(wall_piece->local.t - Vec3{20, 0, 10}) < 1e-4f &&
              std::fabs(wall_piece->mesh->bounds().center().x) < 1e-4f && std::fabs(wall_piece->mesh->bounds().lo.y) < 1e-4f,
          "level pieces: the terrain locked, a piece of the shell selectable, its origin at its base");

    // the catalog: the objects with a model, the data of an actor of the game without its level's
    const auto objects = build_catalog(story_lib, &data);
    const CatalogObject* crate = nullptr;
    bool water = false, no_model = false;
    for (const auto& o : objects) {
      if (o.etype == "crate") crate = &o;
      if (o.family == "water" && o.model == "water-anim-test-pool" && o.lump.value("look", -1) == 0) water = true;
      if (o.etype == "no-model-thing") no_model = true;
    }
    check(crate && crate->model == "crate-test" && crate->family == "crate" && crate->count == 1 &&
              crate->lump.contains("eco-info") && !crate->lump.contains("kill-mask") &&
              !crate->lump.contains("next-actor") && !crate->lump.contains("name") && water && !no_model,
          "catalog: objects with their model, family and data (without their level's), water by look");
    Node placed = crate ? make_catalog_actor(*crate, "crate-7") : Node{};
    check(placed.kind == NodeKind::Actor && placed.extras.value("etype", std::string()) == "crate" &&
              placed.extras["lump"].value("name", std::string()) == "crate-7",
          "catalog: a new actor of an object, named in its data");

    // the decor parts of a level file: its prototypes and the pieces of its shell
    Scene town_file;
    ImportOptions io;
    ImportResult ir;
    const bool read_town = import_gltf(town_file, town_entry->background, io, &ir, &err);
    const auto parts = list_parts(town_file);
    std::string part_names;
    for (const auto& p : parts)
      part_names += p.name + (p.kind == CatalogPart::Kind::Piece ? "(piece) " : " ");
    std::shared_ptr<Mesh> wall_mesh;
    for (const auto& p : parts)
      if (p.kind == CatalogPart::Kind::Piece) wall_mesh = part_mesh(town_file, p);
    check(read_town && part_names == "rock.mb rubble.mb town-wall(piece) " && wall_mesh &&
              std::fabs(wall_mesh->bounds().lo.y) < 1e-4f && std::fabs(wall_mesh->bounds().center().z) < 1e-4f,
          "decor parts: prototypes and pieces (not the terrain), each around its base");

    // the index of the library: the same parts without loading the level, the art-names of its
    // actors; the catalog with it: a particle effect per group, the decor of every level
    LevelIndex town_index;
    bool same_parts = index_level(town_entry->background, &town_index, &err) && town_index.parts.size() == parts.size();
    for (size_t i = 0; same_parts && i < parts.size(); i++) {
      const CatalogPart &a = town_index.parts[i], &b = parts[i];
      same_parts = a.name == b.name && a.mesh_node == b.mesh_node && a.kind == b.kind && a.triangles == b.triangles &&
                   a.copies == b.copies;
    }
    check(same_parts && town_index.art_names["part-spawner"]["group-town-torch"] == 1,
          "library index: the decor parts of a level without loading it, the art-names of its actors");
    LibraryIndex lib_index;
    lib_index.start(story_lib, (tmp / "index").string());
    for (int i = 0; i < 500 && !lib_index.ready(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto with_index = build_catalog(story_lib, &data, &lib_index);
    const CatalogObject* torch = nullptr;
    for (const auto& o : with_index)
      if (o.family == "effect") torch = &o;
    const auto every_part = all_decor(lib_index, story_lib);
    size_t town_parts = 0;
    for (const auto& e : every_part) town_parts += e.level == "town";
    check(lib_index.ready() && torch && torch->name == "town-torch" && torch->etype == "part-spawner" &&
              torch->lump.value("art-name", std::string()) == "group-town-torch" && torch->count == 1 &&
              town_parts == parts.size(),
          "catalog: a particle effect per particle group of the levels, the decor of every level");

    // a copied actor is named like the others of its type, in its data too
    if (Node* c = node_named("crate-1")) {
      auto copies = paste_nodes(town, copy_nodes(town, {c->id}));
      const Node* copy = copies.empty() ? nullptr : town.find(copies[0]);
      check(copy && copy->name == "crate-2" && copy->extras["lump"].value("name", std::string()) == "crate-2",
            "paste: an actor copy named <type>-<number>, in its data too");
    }
  } catch (const std::exception& e) {
    std::printf("  exception: %s\n", e.what());
    check(false, "story: no exception");
  }

  // the scale gizmo, driven like the mouse would: an axis stretches along it, the center scales
  // the three, snap by steps of 0.1
  {
    Camera cam;
    cam.pos = {0, 3, 12};
    cam.look_at({0, 0, 0});
    Renderer r;  // not initialized (no GL): only its view, for project()
    r.set_view(cam, 0, 0, 800, 600);
    Gizmo g;
    g.op = GizmoOp::Scale;
    const float size = length(cam.pos) * std::tan(radians(cam.fov_deg) * 0.5f) * 0.24f;  // the gizmo's
    auto at = [&](const Vec3& p) {
      Vec2 v;
      r.project(p, &v);
      return v;
    };
    auto step = [&](const Vec2& mouse, bool pressed, bool down) {
      GizmoInput in;
      in.mouse = mouse;
      in.viewport_size = {800, 600};
      in.mouse_in_viewport = true;
      in.pressed = pressed;
      in.down = down;
      in.released = !down;
      return g.update(cam, r, in, Mat4::identity(), nullptr);
    };
    // the X axis grabbed at 60% of its length and pulled to 160%: twice as long
    const GizmoResult a = step(at({size * 0.6f, 0, 0}), true, true);
    const GizmoResult b = step(at({size * 1.6f, 0, 0}), false, true);
    const GizmoResult c = step(at({size * 1.6f, 0, 0}), false, false);
    const bool axis = a.started && b.dragging && std::fabs(b.scale.x - 2.f) < 0.01f && b.scale.y == 1.f &&
                      b.scale.z == 1.f && c.finished;
    // the center, dragged 50 px right and 50 px up: e times bigger
    const Vec2 o = at({0, 0, 0});
    const GizmoResult d = step(o, true, true);
    const GizmoResult e = step(Vec2{o.x + 50, o.y - 50}, false, true);
    step(Vec2{o.x + 50, o.y - 50}, false, false);
    const bool center = d.started && std::fabs(e.scale.x - std::exp(1.f)) < 1e-3f && e.scale.y == e.scale.x &&
                        e.scale.z == e.scale.x;
    // snapped: 1.77 becomes 1.8
    g.snap.enabled = true;
    step(at({size * 0.6f, 0, 0}), true, true);
    const GizmoResult f = step(at({size * 1.37f, 0, 0}), false, true);
    step(at({size * 1.37f, 0, 0}), false, false);
    check(axis && center && std::fabs(f.scale.x - 1.8f) < 1e-4f,
          "scale gizmo: an axis stretches along it, the center scales the three, snap by 0.1");
  }

  // the remote control of the MCP server: a request and its answer, one line each
  {
    RemoteServer server;
    RemoteClient client;
    std::string err_remote;
    const int port = 47899;
    bool round_trip = false;
    if (server.start(port, &err_remote) && client.connect(port, &err_remote)) {
      std::thread answer([&] {
        for (int i = 0; i < 200; i++) {
          auto requests = server.take();
          if (!requests.empty()) {
            const RemoteRequest& r = requests[0];
            server.reply(r.client, json{{"id", r.id}, {"ok", true}, {"result", {{"tool", r.tool}, {"x", r.args.value("x", 0)}}}});
            return;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      });
      json reply;
      round_trip = client.call(json{{"id", 7}, {"tool", "echo"}, {"args", {{"x", 42}}}}, &reply, 5000, &err_remote) &&
                   reply.value("id", 0) == 7 && reply["result"].value("tool", std::string()) == "echo" &&
                   reply["result"].value("x", 0) == 42;
      answer.join();
    }
    const uint8_t bytes[] = {'M', 'a', 'n'};
    check(round_trip && base64_encode(bytes, 3) == "TWFu" && base64_encode(bytes, 2) == "TWE=",
          "remote control: a request and its answer over the local connection");
  }

  // the game data shipped with the editor
  {
    GameData jak2;
    const bool read = jak2.load(game_data_path("jak2"), &err);
    const StoryCond* statue = read ? jak2.type_rule("ctypal-baron-statue-broken") : nullptr;
    const LevelInfo* ruins = read ? jak2.level("ruins") : nullptr;
    bool statue_late = false;
    if (statue) {
      int closed_at = 0;
      for (int i = 0; i < jak2.step_count(); i++)
        if (jak2.node(i).name == "canyon-insert-items-resolution") closed_at = i + 1;
      statue_late = closed_at > 1 && !jak2.eval(*statue, jak2.moment(closed_at - 1)) &&
                    jak2.eval(*statue, jak2.moment(closed_at));
    }
    check(read && jak2.role("mech") == ActorRole::Creature && jak2.role("crimson-guard-level") == ActorRole::Creature &&
              jak2.role("part-spawner") == ActorRole::Logic && jak2.role("crate") == ActorRole::Object &&
              jak2.is_water("dark-eco-pool") && statue_late && ruins && !ruins->ocean.empty() &&
              ruins->companions == std::vector<std::string>{"sagehut"},
          "game data of Jak II: roles, water, the broken statue of the palace, Dead Town's ocean and hut");
  }

  // translations: every language has every key of English, with the same printf arguments
  {
    const char* base = SDL_GetBasePath();
    const fs::path dir = fs::path(base ? base : "") / "lang";
    const json en = read_json_file(dir / "en.json");
    check(en.is_object() && en.size() > 50, "translations: lang/en.json next to the executable");
    size_t languages = 0;
    std::string problems;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      if (e.path().extension() != ".json" || e.path().stem() == "en") continue;
      languages++;
      const json other = read_json_file(e.path());
      for (const auto& [k, v] : en.items()) {
        if (!other.contains(k)) problems += e.path().stem().string() + ": no " + k + "\n";
        else if (format_args(v.get<std::string>()) != format_args(other[k].get<std::string>()))
          problems += e.path().stem().string() + ": other arguments in " + k + "\n";
      }
      for (const auto& [k, v] : other.items())
        if (!en.contains(k)) problems += e.path().stem().string() + ": unknown key " + k + "\n";
    }
    if (!problems.empty()) std::printf("%s", problems.c_str());
    check(languages >= 1 && problems.empty(), "translations: same keys and arguments as English");
    i18n_init(dir.string(), "fr");
    const bool french = std::string(tr("menu.file")) == "Fichier";
    i18n_select("en");
    check(french && std::string(tr("menu.file")) == "File" && std::string(tr("no.such.key")) == "no.such.key",
          "translations: switching language, a missing key shows itself");
  }

  fs::remove_all(tmp, ec);
  std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures, g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}

}  // namespace ogle
