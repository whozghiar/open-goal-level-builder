// The editor's side of its MCP server (see remote.h, mcp.cpp): the tools an assistant calls,
// carried out on the main thread between frames, like the user's actions (shown as they happen,
// undoable). Positions are in meters, Y up, like the editor's view.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <unordered_set>

#include "app/app.h"
#include "core/i18n.h"
#include "core/log.h"
#include "core/prefs.h"
#include "glad/glad.h"
#include "render/offscreen.h"

namespace ogle {

namespace fs = std::filesystem;

namespace {

json ok(json result = json::object()) { return {{"ok", true}, {"result", std::move(result)}}; }
json fail(const std::string& error) { return {{"ok", false}, {"error", error}}; }

float round3(float v) { return std::round(v * 1000.f) / 1000.f; }
json vec3_json(const Vec3& v) { return json::array({round3(v.x), round3(v.y), round3(v.z)}); }

bool vec3_arg(const json& args, const char* key, Vec3* out) {
  auto it = args.find(key);
  if (it == args.end() || !it->is_array() || it->size() != 3) return false;
  for (int i = 0; i < 3; i++)
    if (!(*it)[i].is_number()) return false;
  *out = {(*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>()};
  return true;
}

std::vector<uint32_t> ids_arg(const json& args, const char* key = "ids") {
  std::vector<uint32_t> out;
  auto it = args.find(key);
  if (it == args.end()) return out;
  if (it->is_number_integer()) out.push_back(it->get<uint32_t>());
  if (it->is_array())
    for (const auto& v : *it)
      if (v.is_number_integer()) out.push_back(v.get<uint32_t>());
  return out;
}

std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

const char* kind_word(NodeKind k) { return kind_name(k); }

}  // namespace

// ------------------------------------------------------------------------------------------------
// server
// ------------------------------------------------------------------------------------------------

void App::remote_start(int port) {
  if (port <= 0) return;
  std::string err;
  if (m_remote.start(port, &err)) LOG_INFO(tr("log.remote_ready"), port);
  else LOG_WARN(tr("log.remote_failed"), err.c_str());
}

void App::remote_poll() {
  // requests that wait for something (a level loading...): answered when it is there
  for (size_t i = 0; i < m_remote_waits.size();) {
    if (m_remote_waits[i]()) m_remote_waits.erase(m_remote_waits.begin() + (long)i);
    else i++;
  }
  for (auto& r : m_remote.take()) {
    const int client = r.client;
    const json id = r.id;
    const std::string tool = r.tool;
    RemoteReply reply = [this, client, id](json answer) {
      answer["id"] = id;
      m_remote.reply(client, answer);
    };
    try {
      remote_dispatch(tool, r.args, reply);
    } catch (const std::exception& e) {
      reply(fail(std::string("error in ") + tool + ": " + e.what()));
    }
  }
}

// ------------------------------------------------------------------------------------------------
// helpers
// ------------------------------------------------------------------------------------------------

json App::remote_node_json(const Node& n) {
  json j = {{"id", n.id}, {"name", n.name}, {"kind", kind_word(n.kind)}};
  if (n.kind == NodeKind::Actor) j["etype"] = n.extras.value("etype", std::string());
  j["position"] = vec3_json(scene.world(n).col3(3));
  AABB b = scene.world_bounds(n);
  for (auto d : scene.descendants(n.id))
    if (const Node* dn = scene.find(d)) b.add(scene.world_bounds(*dn));
  if (b.valid()) j["size"] = vec3_json(b.size());
  if (n.locked) j["locked"] = true;
  if (!scene.effectively_visible(n)) j["hidden"] = true;
  return j;
}

bool App::remote_unsaved(const json& args) const {
  return history.revision() != saved_revision && document_open() && !args.value("discard_changes", false);
}

std::string App::capture_view_png(int w, int h) {
  OffscreenTarget target;
  if (!target.create(w, h)) return {};
  GLint viewport[4];
  glGetIntegerv(GL_VIEWPORT, viewport);
  glViewport(0, 0, w, h);
  glClearColor(0.07f, 0.08f, 0.09f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  scene.begin_frame();
  renderer.begin(camera, 0, 0, w, h, rs, (float)w, (float)h);
  renderer.draw_scene(scene, rs, selection, 0);
  if (rs.show_ocean && m_ocean && document_open())
    renderer.draw_mesh(*m_ocean, Mat4::translate({0, m_ocean_height, 0}), m_ocean_materials, {}, rs);
  const std::vector<uint8_t> png = target.png();
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
  return base64_encode(png.data(), png.size());
}

json App::remote_status() {
  json games = json::array();
  for (const auto& g : this->games) games.push_back({{"game", g.game}, {"name", g.name}, {"levels", g.levels}});
  size_t actors = 0;
  for (const auto& n : scene.nodes) actors += n.kind == NodeKind::Actor;
  json j = {{"library", library_root},
            {"games", games},
            {"game", library.game()},
            {"document",
             {{"open", document_open()},
              {"level", scene.settings.level},
              {"game", game_name(scene.settings.game)},
              {"project", project_path},
              {"elements", scene.nodes.size()},
              {"actors", actors},
              {"unsaved", history.revision() != saved_revision && document_open()},
              {"loading", m_level_future.valid()}}},
            {"camera",
             {{"position", vec3_json(camera.pos)},
              {"yaw_deg", round3(camera.yaw * 57.29578f)},
              {"pitch_deg", round3(camera.pitch * 57.29578f)},
              {"forward", vec3_json(camera.forward())}}},
            {"selection", std::vector<uint32_t>(selection.begin(), selection.end())},
            {"can_undo", history.can_undo()},
            {"can_redo", history.can_redo()}};
  if (const GameData* data = document_game_data(); data && data->has_story()) {
    json s = {{"enabled", story.enabled}, {"step", story.step}, {"steps", data->step_count()},
              {"changes_of_this_level", story_steps}};
    if (story.step > 1) s["after_node"] = data->node(story.step - 1).name;
    j["story"] = s;
  }
  if (document_open()) {
    const AABB b = scene.focus_bounds();
    if (b.valid()) j["document"]["bounds"] = {{"min", vec3_json(b.lo)}, {"max", vec3_json(b.hi)}};
  }
  return j;
}

// ------------------------------------------------------------------------------------------------
// the tools
// ------------------------------------------------------------------------------------------------

void App::remote_dispatch(const std::string& tool, const json& args, RemoteReply reply) {
  if (tool == "get_status") {
    reply(ok(remote_status()));

  } else if (tool == "list_levels") {
    const std::string f = lower(args.value("filter", std::string()));
    json levels = json::array();
    for (const auto& l : library.levels())
      if (l.listed && (f.empty() || l.name.find(f) != std::string::npos || lower(l.dgo).find(f) != std::string::npos))
        levels.push_back({{"name", l.name}, {"dgo", l.dgo}});
    reply(ok({{"game", library.game()}, {"levels", levels}}));

  } else if (tool == "open_level") {
    const std::string name = args.value("name", std::string());
    const LevelEntry* level = library.level(name);
    if (!level) return reply(fail("no level '" + name + "' in the library (see list_levels)"));
    if (m_level_future.valid()) return reply(fail("a level is already loading"));
    if (remote_unsaved(args)) return reply(fail("the document has unsaved changes: save_project first, or discard_changes: true"));
    load_level(*level);
    m_remote_waits.push_back([this, reply, name]() {
      if (m_level_future.valid()) return false;
      if (scene.settings.source_group != name) reply(fail("the level " + name + " could not be opened (see the editor's log)"));
      else reply(ok(remote_status()));
      return true;
    });

  } else if (tool == "new_level") {
    if (remote_unsaved(args)) return reply(fail("the document has unsaved changes: save_project first, or discard_changes: true"));
    new_level();
    reply(ok(remote_status()));

  } else if ((tool == "list_catalog" || tool == "list_models") && !m_index_applied) {
    // the whole catalog: once the editor has read the levels (their variants and effects)
    m_remote_waits.push_back([this, tool, args, reply]() {
      if (!m_index_applied) return false;
      remote_dispatch(tool, args, reply);
      return true;
    });

  } else if (tool == "list_catalog") {
    const std::string family = args.value("family", std::string());
    const std::string f = lower(args.value("filter", std::string()));
    const int offset = std::max(0, args.value("offset", 0));
    const int limit = std::clamp(args.value("limit", 100), 1, 1000);
    json objects = json::array();
    int matching = 0;
    for (const auto& o : catalog) {
      if (!family.empty() && o.family != family) continue;
      if (!f.empty() && o.name.find(f) == std::string::npos && o.etype.find(f) == std::string::npos) continue;
      if (matching++ < offset || (int)objects.size() >= limit) continue;
      json j = {{"name", o.name}, {"etype", o.etype}, {"family", o.family}};
      if (!o.model.empty()) j["model"] = o.model;
      else j["drawn_without_model"] = true;
      if (o.count) j["in_game"] = o.count;
      objects.push_back(j);
    }
    json families = json::array();
    for (const auto& o : catalog)
      if (families.empty() || families.back() != o.family) families.push_back(o.family);
    reply(ok({{"total", matching}, {"offset", offset}, {"objects", objects}, {"families", families}}));

  } else if (tool == "list_models") {
    const std::string f = lower(args.value("filter", std::string()));
    const int offset = std::max(0, args.value("offset", 0));
    const int limit = std::clamp(args.value("limit", 100), 1, 1000);
    json models = json::array();
    int matching = 0;
    for (const auto& m : catalog_models) {
      if (!f.empty() && m.name.find(f) == std::string::npos && m.group.find(f) == std::string::npos) continue;
      if (matching++ < offset || (int)models.size() >= limit) continue;
      models.push_back({{"name", m.name}, {"from", m.group}});
    }
    reply(ok({{"total", matching}, {"offset", offset}, {"models", models}}));

  } else if (tool == "list_decor") {
    const std::string name = args.value("level", std::string());
    const std::string f = lower(args.value("filter", std::string()));
    const int offset = std::max(0, args.value("offset", 0));
    const int limit = std::clamp(args.value("limit", 100), 1, 1000);
    if (name.empty() || name == "all") {
      // the decor of every level: once the editor has read them
      m_remote_waits.push_back([this, reply, f, offset, limit]() {
        if (!m_index_applied) return false;
        json parts = json::array();
        int matching = 0;
        for (const auto& e : all_parts) {
          if (!f.empty() && e.key.find(f) == std::string::npos) continue;
          if (matching++ < offset || (int)parts.size() >= limit) continue;
          json j = {{"name", e.part.name},
                    {"level", e.level},
                    {"kind", e.part.kind == CatalogPart::Kind::Prototype ? "prototype" : "piece"},
                    {"triangles", e.part.triangles}};
          if (e.levels > 1) j["in_levels"] = e.levels;
          parts.push_back(j);
        }
        reply(ok({{"total", matching}, {"offset", offset}, {"parts", parts}}));
        return true;
      });
      return;
    }
    const LevelEntry* level = library.level(name);
    if (!level) return reply(fail("no level '" + name + "' in the library (see list_levels)"));
    const std::string background = level->background;
    m_remote_waits.push_back([this, reply, background, f, offset, limit, name]() {
      auto scene_ptr = store.level(background, true);
      if (!scene_ptr) {
        if (!store.failed_level(background)) return false;
        reply(fail("the decor of " + name + " cannot be read"));
        return true;
      }
      json parts = json::array();
      int matching = 0;
      for (const auto& p : list_parts(*scene_ptr)) {
        if (!f.empty() && lower(p.name).find(f) == std::string::npos) continue;
        if (matching++ < offset || (int)parts.size() >= limit) continue;
        json j = {{"name", p.name},
                  {"kind", p.kind == CatalogPart::Kind::Prototype ? "prototype" : "piece"},
                  {"triangles", p.triangles}};
        if (p.kind == CatalogPart::Kind::Prototype) j["copies_in_level"] = p.copies;
        parts.push_back(j);
      }
      reply(ok({{"level", name}, {"total", matching}, {"offset", offset}, {"parts", parts}}));
      return true;
    });

  } else if (tool == "list_prefabs") {
    refresh_prefabs();
    json list = json::array();
    for (const auto& p : prefabs) list.push_back(p.name);
    reply(ok({{"prefabs", list}}));

  } else if (tool == "place") {
    remote_place(args, reply);

  } else if (tool == "find_nodes") {
    const std::string name = lower(args.value("name", std::string()));
    const std::string etype = args.value("etype", std::string());
    const std::string kind = args.value("kind", std::string());
    const bool include_locked = args.value("include_locked", false);
    Vec3 near;
    const bool has_near = vec3_arg(args, "near", &near);
    const float radius = args.value("radius", 20.f);
    const size_t limit = (size_t)std::clamp(args.value("limit", 50), 1, 1000);
    const SceneCache& cache = scene.cache();
    std::vector<std::pair<float, const Node*>> found;
    for (size_t i = 0; i < scene.nodes.size(); i++) {
      const Node& n = scene.nodes[i];
      if (n.extras.contains("og_actor_visual")) continue;  // an actor's model: the actor stands for it
      if (n.kind == NodeKind::Collision) continue;
      if (n.locked && !include_locked) continue;
      if (!kind.empty() && kind != kind_word(n.kind)) continue;
      if (!name.empty() && lower(n.name).find(name) == std::string::npos) continue;
      if (!etype.empty() && (n.kind != NodeKind::Actor || n.extras.value("etype", std::string()) != etype)) continue;
      if (n.kind == NodeKind::Group && name.empty() && etype.empty() && kind.empty()) continue;
      float d = 0;
      if (has_near) {
        AABB b = n.mesh ? cache.bounds[i] : AABB{};
        const Vec3 p = b.valid() ? b.center() : cache.worlds[i].col3(3);
        d = length(p - near);
        if (d > radius) continue;
      }
      found.push_back({d, &n});
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    json list = json::array();
    for (size_t k = 0; k < found.size() && k < limit; k++) list.push_back(remote_node_json(*found[k].second));
    reply(ok({{"total", found.size()}, {"nodes", list}}));

  } else if (tool == "get_node") {
    const Node* n = scene.find(args.value("id", 0u));
    if (!n) return reply(fail("no element with that id"));
    json j = remote_node_json(*n);
    j["parent"] = n->parent;
    j["local"] = {{"position", vec3_json(n->local.t)},
                  {"rotation_deg", vec3_json(n->local.r.to_euler_deg())},
                  {"scale", vec3_json(n->local.s)}};
    if (n->mesh) j["mesh"] = {{"name", n->mesh->name}, {"triangles", n->mesh->tri_count()}};
    if (n->kind == NodeKind::Actor) {
      j["game_data"] = n->extras;
      for (auto c : scene.children(n->id))
        if (const Node* cn = scene.find(c); cn && cn->extras.contains("og_actor_visual")) j["model"] = cn->name;
    }
    json children = json::array();
    for (auto c : scene.children(n->id)) children.push_back(c);
    if (!children.empty()) j["children"] = children;
    reply(ok(j));

  } else if (tool == "transform_nodes") {
    const auto ids = ids_arg(args);
    Vec3 position, translate, rotation;
    const bool set_position = vec3_arg(args, "position", &position);
    const bool add_offset = vec3_arg(args, "translate", &translate);
    const bool set_rotation = vec3_arg(args, "rotation_deg", &rotation);
    const float turn = args.value("rotate_y_deg", 0.f);
    // scale: [x, y, z] or one number for the three; scale_by multiplies the scale they have
    auto scale_arg = [&](const char* key, Vec3* out) {
      auto it = args.find(key);
      if (it != args.end() && it->is_number()) {
        const float k = it->get<float>();
        *out = {k, k, k};
        return true;
      }
      return vec3_arg(args, key, out);
    };
    Vec3 scale, scale_by;
    const bool set_scale = scale_arg("scale", &scale);
    const bool mul_scale = scale_arg("scale_by", &scale_by);
    if (ids.empty()) return reply(fail("ids: the elements to move"));
    NodeChange c(scene);
    json moved = json::array();
    for (auto id : ids) {
      Node* n = scene.find(id);
      if (!n) continue;
      c.save(id);
      if (set_rotation) n->local.r = Quat::from_euler_deg(rotation);
      Mat4 w = scene.world(*n);
      if (turn != 0.f) {
        const Vec3 p = w.col3(3);
        w = Mat4::translate(p) * Mat4::rotate(Quat::axis_angle({0, 1, 0}, radians(turn))) * Mat4::translate(-p) * w;
      }
      if (set_position) w = Mat4::translate(position - w.col3(3)) * w;
      if (add_offset) w = Mat4::translate(translate) * w;
      scene.set_world(*n, w);
      if (set_scale) n->local.s = scale;
      if (mul_scale) n->local.s = {n->local.s.x * scale_by.x, n->local.s.y * scale_by.y, n->local.s.z * scale_by.z};
      n->local.s = {std::max(n->local.s.x, 0.01f), std::max(n->local.s.y, 0.01f), std::max(n->local.s.z, 0.01f)};
      moved.push_back(remote_node_json(*n));
    }
    history.push(c.commit(tr(set_scale || mul_scale ? "undo.scale"
                             : set_rotation || turn != 0.f ? "undo.rotate"
                                                           : "undo.move")));
    reply(ok({{"nodes", moved}}));

  } else if (tool == "delete_nodes") {
    auto ids = ids_arg(args);
    ids.erase(std::remove_if(ids.begin(), ids.end(), [&](uint32_t id) { return !scene.find(id); }), ids.end());
    if (ids.empty()) return reply(fail("ids: the elements to delete"));
    NodeChange c(scene);
    for (auto id : ids) c.save_subtree(id);
    scene.remove(ids);
    history.push(c.commit(strf(tr("undo.delete"), ids.size())));
    for (auto id : ids) selection.erase(id);
    reply(ok({{"deleted", ids.size()}}));

  } else if (tool == "duplicate_nodes") {
    auto ids = ids_arg(args);
    ids.erase(std::remove_if(ids.begin(), ids.end(), [&](uint32_t id) { return !scene.find(id); }), ids.end());
    if (ids.empty()) return reply(fail("ids: the elements to copy"));
    Vec3 offset{0, 0, 0};
    vec3_arg(args, "offset", &offset);
    NodeChange c(scene);
    auto copies = paste_nodes(scene, copy_nodes(scene, ids));
    for (auto id : copies)
      if (Node* n = scene.find(id)) scene.set_world(*n, Mat4::translate(offset) * scene.world(*n));
    history.push(c.commit(strf(tr("undo.duplicate"), copies.size())));
    json list = json::array();
    for (auto id : copies)
      if (const Node* n = scene.find(id)) list.push_back(remote_node_json(*n));
    reply(ok({{"nodes", list}}));

  } else if (tool == "raycast") {
    Vec3 origin, direction{0, -1, 0};
    if (!vec3_arg(args, "origin", &origin)) return reply(fail("origin: [x, y, z]"));
    vec3_arg(args, "direction", &direction);
    if (length(direction) < 1e-6f) return reply(fail("direction: a vector"));
    PickFilter f;
    f.actors = args.value("actors", false);
    f.include_locked = true;
    const PickHit hit = pick(scene, Ray{origin, normalize(direction)}, f);
    if (!hit.hit()) return reply(ok({{"hit", false}}));
    json j = {{"hit", true}, {"point", vec3_json(hit.point)}, {"normal", vec3_json(hit.normal)}, {"distance", round3(hit.t)}};
    if (const Node* n = scene.find(hit.node)) j["node"] = remote_node_json(*n);
    reply(ok(j));

  } else if (tool == "set_camera") {
    Vec3 position, target;
    if (args.contains("frame")) {
      const json& f = args["frame"];
      if (f.is_string() && f.get<std::string>() == "all") {
        frame_all();
      } else {
        auto ids = f.is_array() ? ids_arg(args, "frame") : std::vector<uint32_t>(selection.begin(), selection.end());
        AABB b;
        for (auto id : ids)
          if (const Node* n = scene.find(id)) {
            b.add(scene.world_bounds(*n));
            for (auto d : scene.descendants(id))
              if (const Node* dn = scene.find(d)) b.add(scene.world_bounds(*dn));
          }
        if (b.valid()) camera.frame(b.expanded(2.f));
      }
    }
    if (vec3_arg(args, "position", &position)) camera.pos = position;
    if (vec3_arg(args, "look_at", &target)) camera.look_at(target);
    if (args.contains("yaw_deg") && args["yaw_deg"].is_number()) camera.yaw = radians(args["yaw_deg"].get<float>());
    if (args.contains("pitch_deg") && args["pitch_deg"].is_number())
      camera.pitch = std::clamp(radians(args["pitch_deg"].get<float>()), -1.55f, 1.55f);
    reply(ok(remote_status()["camera"]));

  } else if (tool == "capture_view") {
    const int w = std::clamp(args.value("width", 960), 64, 2048);
    const int h = std::clamp(args.value("height", 600), 64, 2048);
    const std::string png = capture_view_png(w, h);
    if (png.empty()) return reply(fail("the view could not be captured"));
    json answer = ok({{"width", w}, {"height", h}, {"camera", remote_status()["camera"]}});
    answer["image"] = png;
    reply(answer);

  } else if (tool == "select") {
    selection.clear();
    for (auto id : ids_arg(args))
      if (scene.find(id)) selection.insert(id);
    primary = selection.empty() ? 0 : *selection.begin();
    reply(ok({{"selection", std::vector<uint32_t>(selection.begin(), selection.end())}}));

  } else if (tool == "set_story") {
    const GameData* data = document_game_data();
    if (!data || !data->has_story()) return reply(fail("no story view for this document (Jak II and Jak 3 levels only)"));
    StoryView v = story;
    if (args.contains("enabled") && args["enabled"].is_boolean()) v.enabled = args["enabled"].get<bool>();
    if (args.contains("step") && args["step"].is_number_integer()) v.step = args["step"].get<int>();
    if (args.contains("after_node") && args["after_node"].is_string()) {
      const std::string node = args["after_node"].get<std::string>();
      int found = -1;
      for (int i = 0; i < data->step_count(); i++)
        if (data->node(i).name == node) found = i;
      if (found < 0) return reply(fail("no task node '" + node + "'"));
      v.step = found + 1;
      v.enabled = true;
    }
    set_story_view(v);
    reply(ok(remote_status()["story"]));

  } else if (tool == "create_prefab") {
    const auto ids = ids_arg(args);
    const std::string name = args.value("name", std::string());
    if (ids.empty() || name.empty()) return reply(fail("ids and name"));
    selection.clear();
    for (auto id : ids)
      if (scene.find(id)) selection.insert(id);
    if (selection.empty()) return reply(fail("no element with those ids"));
    const size_t before = prefabs.size();
    create_prefab(name);
    if (prefabs.size() <= before) return reply(fail("the prefab could not be saved (see the editor's log)"));
    reply(ok({{"prefabs", [&] {
                 json l = json::array();
                 for (const auto& p : prefabs) l.push_back(p.name);
                 return l;
               }()}}));

  } else if (tool == "save_project") {
    std::string path = args.value("path", project_path);
    if (path.empty()) return reply(fail("path: where to save the project (a .glb file)"));
    if (fs::path(path).extension() != ".glb") path += ".glb";
    std::string err;
    if (!save_project(scene, path, &err)) return reply(fail("cannot save: " + err));
    project_path = path;
    saved_revision = history.revision();
    Prefs::get().add_recent_project(path);
    Prefs::get().save();
    set_title();
    LOG_INFO(tr("log.project_saved"), path.c_str());
    reply(ok({{"path", path}}));

  } else if (tool == "open_project") {
    const std::string path = args.value("path", std::string());
    if (path.empty()) return reply(fail("path: the project (.glb)"));
    if (remote_unsaved(args)) return reply(fail("the document has unsaved changes: save_project first, or discard_changes: true"));
    open_project_file(path);
    if (project_path != path) return reply(fail("cannot open " + path + " (see the editor's log)"));
    reply(ok(remote_status()));

  } else if (tool == "undo" || tool == "redo") {
    const bool can = tool == "undo" ? history.can_undo() : history.can_redo();
    const std::string label = tool == "undo" ? history.undo_label() : history.redo_label();
    if (can) {
      if (tool == "undo") history.undo();
      else history.redo();
    }
    reply(ok({{"done", can}, {"change", label}}));

  } else {
    reply(fail("unknown tool '" + tool + "'"));
  }
}

void App::remote_place(const json& args, RemoteReply reply) {
  const std::string kind = args.value("kind", std::string("object"));
  const std::string name = args.value("name", std::string());
  Vec3 position;
  if (!vec3_arg(args, "position", &position)) return reply(fail("position: [x, y, z] in meters"));
  if (!document_open()) return reply(fail("no document: open_level or new_level first"));
  const float yaw = radians(args.value("yaw_deg", 0.f));
  const bool on_ground = args.value("on_ground", false);

  // what to place
  if (kind == "model") {
    const CatalogModel* found = nullptr;
    for (const auto& m : catalog_models)
      if (m.name == name) found = &m;
    if (!found) return reply(fail("no model '" + name + "' in the catalog (see list_models)"));
    start_model_placement(*found, false);
  } else if (kind == "object") {
    const CatalogObject* found = nullptr;
    for (const auto& o : catalog)
      if (o.name == name) found = &o;
    if (!found)
      for (const auto& o : catalog)
        if (o.etype == name && !found) found = &o;
    if (!found) return reply(fail("no object '" + name + "' in the catalog (see list_catalog)"));
    start_object_placement(*found, false);
  } else if (kind == "prefab") {
    refresh_prefabs();
    const PrefabInfo* found = nullptr;
    for (const auto& p : prefabs)
      if (p.name == name) found = &p;
    if (!found) return reply(fail("no prefab '" + name + "' (see list_prefabs)"));
    start_placement(found->path, found->name, false);
  } else if (kind == "decor") {
    std::string level_name = args.value("level", std::string());
    if (level_name.empty()) {
      // the first level with a part of that name
      if (!m_index_applied) {
        m_remote_waits.push_back([this, args, reply]() {
          if (!m_index_applied) return false;
          remote_place(args, reply);
          return true;
        });
        return;
      }
      for (const auto& e : all_parts)
        if (e.part.name == name || e.part.name == name + ".mb") {
          level_name = e.level;
          break;
        }
      if (level_name.empty()) return reply(fail("no decor part '" + name + "' in the levels (see list_decor)"));
    }
    const LevelEntry* level = library.level(level_name);
    if (!level) return reply(fail("level: the level the decor part comes from (see list_decor)"));
    auto source = store.level(level->background, true);
    if (!source) {
      if (store.failed_level(level->background)) return reply(fail("the decor of that level cannot be read"));
      // loading: try again next frame
      m_remote_waits.push_back([this, args, reply, level_name]() {
        const LevelEntry* level = library.level(level_name);
        if (level && !store.level(level->background, true) && !store.failed_level(level->background)) return false;
        remote_place(args, reply);
        return true;
      });
      return;
    }
    const CatalogPart* found = nullptr;
    const auto parts = list_parts(*source);
    for (const auto& p : parts)
      if (p.name == name || p.name == name + ".mb") found = &p;
    if (!found) return reply(fail("no decor part '" + name + "' in " + level->name + " (see list_decor)"));
    start_part_placement(level->background, *found, false);
  } else {
    return reply(fail("kind: object, model, decor or prefab"));
  }
  placement.yaw = yaw;

  // where: on the surface below, when asked
  Vec3 p = position;
  if (on_ground) {
    PickFilter f;
    f.actors = false;
    f.include_locked = true;
    const PickHit hit = pick(scene, Ray{position + Vec3{0, 1.f, 0}, {0, -1, 0}}, f);
    if (hit.hit()) p = hit.point;
  }
  const Mat4 m = Mat4::translate(p) * Mat4::rotate(Quat::axis_angle({0, 1, 0}, yaw));
  // its model or its prefab may still be loading
  m_remote_waits.push_back([this, m, reply, kind]() {
    const bool markerless = placement.kind == Placement::Kind::Object && placement.path.empty();
    if (kind != "prefab" && !markerless && !placement_mesh()) {
      const bool failed = placement.kind == Placement::Kind::Part ? store.failed_level(placement.path)
                                                                  : store.failed_model(placement.path);
      if (!failed) return false;
      stop_placement();
      reply(fail("its model cannot be read"));
      return true;
    }
    const auto roots = place_at(m);
    stop_placement();
    if (roots.empty()) {
      reply(fail("nothing was placed"));
      return true;
    }
    json list = json::array();
    for (auto id : roots)
      if (const Node* n = scene.find(id)) list.push_back(remote_node_json(*n));
    reply(ok({{"nodes", list}}));
    return true;
  });
}

}  // namespace ogle
