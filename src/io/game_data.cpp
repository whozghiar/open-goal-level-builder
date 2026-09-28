#include "io/game_data.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>

#include "json.hpp"

namespace ogle {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

fs::path to_path(const std::string& s) { return fs::path(std::u8string((const char8_t*)s.c_str())); }
std::string from_path(const fs::path& p) {
  auto u = p.generic_u8string();
  return std::string(u.begin(), u.end());
}

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string str(const json& j) { return j.is_string() ? j.get<std::string>() : std::string(); }

// j[key] without a copy, null when missing (iterating it gives nothing)
const json& at(const json& j, const char* key) {
  static const json null;
  if (!j.is_object()) return null;
  auto it = j.find(key);
  return it != j.end() ? *it : null;
}

StoryBorrow read_borrow(const json& b, const std::string& host, int slot) {
  StoryBorrow out;
  out.host = host;
  out.slot = slot;
  out.level = str(b[0]);
  out.special = str(b[1]) == "special";
  return out;
}

}  // namespace

std::string game_data_path(const std::string& game) {
  std::error_code ec;
  const char* base = SDL_GetBasePath();
  const fs::path next_to_exe = to_path(base ? base : "") / "data" / game / "game-data.json";
  if (fs::is_regular_file(next_to_exe, ec)) return from_path(next_to_exe);
#ifdef OGLE_PROJECT_DIR
  const fs::path in_tree = to_path(OGLE_PROJECT_DIR) / "data" / game / "game-data.json";
  if (fs::is_regular_file(in_tree, ec)) return from_path(in_tree);
#endif
  return from_path(next_to_exe);
}

bool GameData::load(const std::string& path, std::string* error) {
  *this = GameData();
  std::ifstream f(to_path(path), std::ios::binary);
  if (!f) {
    if (error) *error = "cannot read " + path;
    return false;
  }
  std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  const json j = json::parse(text, nullptr, false);
  if (!j.is_object() || j.value("format", 0) != 1) {
    if (error) *error = "not a game data file: " + path;
    return false;
  }
  m_game = j.value("game", std::string());

  // actors
  const json& actors = at(j, "actors");
  for (const auto& [type, role] : at(actors, "roles").items())
    m_roles[type] = str(role) == "creature" ? ActorRole::Creature : ActorRole::Logic;
  for (const auto& s : at(actors, "creature_suffixes")) m_creature_suffixes.push_back(str(s));
  for (const auto& t : at(actors, "water_types")) m_water_types[str(t)] = true;
  for (const auto& t : at(actors, "effects")) m_effects.insert(str(t));
  for (const auto& t : at(actors, "unlisted")) m_unlisted.insert(str(t));
  for (const auto& m : at(actors, "water_looks")) m_water_looks.push_back(str(m));
  for (const auto& [type, family] : at(actors, "objects").items()) m_objects[type] = str(family);
  for (const auto& [type, models] : at(actors, "models").items())
    for (const auto& m : models) m_type_models[type].push_back(str(m));
  for (const auto& [name, model] : at(actors, "skeletons").items()) m_skeletons[name] = str(model);

  // oceans
  for (const auto& [name, o] : at(j, "oceans").items()) {
    OceanMap map;
    const json& c = at(o, "corner");
    if (c.size() == 3) map.corner = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
    map.cell = o.value("cell", 96.f);
    const json& rows = at(o, "cells");
    map.size = (int)rows.size();
    map.cells.assign((size_t)map.size * map.size, 0);
    for (int r = 0; r < map.size; r++) {
      const std::string row = str(rows[r]);
      for (int col = 0; col < map.size && col < (int)row.size(); col++) map.cells[(size_t)r * map.size + col] = row[col] == '1';
    }
    const json& color = at(o, "color");
    if (color.size() == 3) map.color = {color[0].get<float>(), color[1].get<float>(), color[2].get<float>()};
    m_oceans[name] = std::move(map);
  }

  // levels
  for (const auto& [name, l] : at(j, "levels").items()) {
    LevelInfo info;
    info.taskname = l.value("taskname", name);
    info.base_mask = l.value("base_mask", 0u);
    info.ocean = str(at(l, "ocean"));
    if (l.contains("ocean_height") && l["ocean_height"].is_number()) info.ocean_height = l["ocean_height"].get<float>();
    info.layer = l.value("layer", false);
    info.traffic = l.value("traffic", false);
    for (const auto& c : at(l, "companions")) info.companions.push_back(str(c));
    const json& slots = at(l, "borrow");
    for (size_t i = 0; i < slots.size(); i++)
      if (slots[i].is_array() && slots[i].size() == 2) info.borrow.push_back(read_borrow(slots[i], name, (int)i));
    m_levels[name] = std::move(info);
  }

  // story
  const json& story = at(j, "story");
  if (story.is_object()) {
    const json& bits = at(story, "mask_bits");
    auto bit = [&](const char* name, uint32_t fallback) {
      return bits.contains(name) ? 1u << bits[name].get<int>() : fallback;
    };
    m_bit_special = bit("special", m_bit_special);
    m_bit_primary = bit("primary0", m_bit_primary);
    m_bit_ctywide = bit("ctywide", m_bit_ctywide);
    m_bit_never = bit("never", m_bit_never);
    const json& nodes = at(story, "nodes");
    std::unordered_map<std::string, int> task_index;
    for (size_t i = 0; i < nodes.size(); i++) m_node_index[nodes[i].value("name", std::string())] = (int)i;
    for (size_t i = 0; i < nodes.size(); i++) {
      const json& n = nodes[i];
      StoryNode node;
      node.name = n.value("name", std::string());
      node.level = n.value("level", std::string());
      const std::string task = n.value("task", std::string());
      if (!task.empty()) {
        auto it = task_index.find(task);
        if (it == task_index.end()) {
          it = task_index.emplace(task, (int)m_tasks.size()).first;
          m_tasks.push_back(task);
          m_task_closers.emplace_back();
        }
        node.task = it->second;
      }
      const std::string op = n.value("mask_op", std::string());
      node.mask_op = op == "abs" ? StoryNode::Abs : op == "set" ? StoryNode::Set : op == "clear" ? StoryNode::Clear : StoryNode::NoMask;
      node.mask = n.value("mask", 0u);
      node.close_task = n.value("close_task", false);
      node.closed = n.value("closed", false) || i == 0;
      for (const auto& p : at(n, "parents"))
        if (p.is_number_integer() && p.get<int>() >= 0 && p.get<int>() < (int)nodes.size()) node.parents.push_back(p.get<int>());
      for (const auto& b : at(n, "borrow")) {
        if (!b.is_array() || b.size() != 4) continue;
        StoryBorrow sb;
        sb.host = str(b[0]);
        sb.slot = b[1].is_number_integer() ? b[1].get<int>() : 0;
        sb.level = str(b[2]);
        sb.special = str(b[3]) == "special";
        node.borrow.push_back(std::move(sb));
      }
      if (node.close_task && node.task >= 0) m_task_closers[node.task].push_back((int)i);
      m_nodes.push_back(std::move(node));
    }
    const std::string reset = str(at(story, "reset_node"));
    if (m_node_index.count(reset)) m_reset_node = m_node_index[reset];

    // conditions: names -> indices
    std::function<StoryCond(const json&)> cond = [&](const json& e) -> StoryCond {
      StoryCond c;
      if (e.is_boolean()) {
        c.op = e.get<bool>() ? StoryCond::True : StoryCond::False;
        return c;
      }
      if (!e.is_array() || e.empty() || !e[0].is_string()) return c;
      const std::string op = e[0].get<std::string>();
      if ((op == "closed" || op == "open") && e.size() == 2) {
        auto it = m_node_index.find(str(e[1]));
        if (it == m_node_index.end()) return c;
        c.op = op == "closed" ? StoryCond::Closed : StoryCond::Open;
        c.ref = it->second;
      } else if (op == "complete" && e.size() == 2) {
        auto it = task_index.find(str(e[1]));
        if (it == task_index.end()) return c;
        c.op = StoryCond::Complete;
        c.ref = it->second;
      } else if (op == "not" && e.size() == 2) {
        c.op = StoryCond::Not;
        c.args.push_back(cond(e[1]));
      } else if ((op == "and" || op == "or") && e.size() > 1) {
        c.op = op == "and" ? StoryCond::And : StoryCond::Or;
        for (size_t i = 1; i < e.size(); i++) c.args.push_back(cond(e[i]));
      }
      return c;
    };
    for (const auto& [level, rules] : at(story, "prototypes").items()) {
      auto& list = m_prototypes[level];
      for (const auto& r : rules) {
        PrototypeRule rule;
        for (const auto& name : at(r, "names")) rule.names.push_back(str(name));
        rule.when = cond(at(r, "when"));
        list.push_back(std::move(rule));
      }
    }
    for (const auto& [type, when] : at(story, "types").items()) m_types[type] = cond(when);
    for (const auto& [level, rules] : at(story, "ocean_heights").items())
      for (const auto& r : rules) m_heights[level].push_back({cond(at(r, "when")), r.value("height", 0.f)});
  }
  m_loaded = true;
  return true;
}

// ------------------------------------------------------------------------------------------------
// actors, levels
// ------------------------------------------------------------------------------------------------

ActorRole GameData::role(const std::string& etype) const {
  auto it = m_roles.find(etype);
  if (it != m_roles.end()) return it->second;
  for (const auto& s : m_creature_suffixes)
    if (!s.empty() && ends_with(etype, s)) return ActorRole::Creature;
  return ActorRole::Object;
}

bool GameData::is_water(const std::string& etype) const { return m_water_types.count(etype) != 0; }

std::vector<std::string> GameData::water_types() const {
  std::vector<std::string> out;
  for (const auto& [t, yes] : m_water_types) out.push_back(t);
  std::sort(out.begin(), out.end());
  return out;
}

const std::vector<std::string>* GameData::type_models(const std::string& etype) const {
  auto it = m_type_models.find(etype);
  return it == m_type_models.end() ? nullptr : &it->second;
}

std::string GameData::skeleton_model(const std::string& name) const {
  auto it = m_skeletons.find(name);
  return it == m_skeletons.end() ? std::string() : it->second;
}

std::string GameData::skeleton_of_model(const std::string& model) const {
  std::string best;
  for (const auto& [name, m] : m_skeletons)
    if (m == model && (best.empty() || name < best)) best = name;
  return best;
}

std::string GameData::object_family(const std::string& etype) const {
  auto it = m_objects.find(etype);
  return it != m_objects.end() && !it->second.empty() ? it->second : std::string("other");
}

const std::string& GameData::water_model(int look) const {
  static const std::string none;
  return look >= 0 && look < (int)m_water_looks.size() ? m_water_looks[look] : none;
}

const LevelInfo* GameData::level(const std::string& name) const {
  auto it = m_levels.find(name);
  return it == m_levels.end() ? nullptr : &it->second;
}

const OceanMap* GameData::ocean(const std::string& name) const {
  auto it = m_oceans.find(name);
  return it == m_oceans.end() ? nullptr : &it->second;
}

// ------------------------------------------------------------------------------------------------
// story
// ------------------------------------------------------------------------------------------------

StoryMoment GameData::moment(int step) const {
  StoryMoment m;
  const int n = (int)m_nodes.size();
  m.step = std::clamp(step, 1, std::max(n, 1));
  m.closed.assign(n, 0);
  m.open.assign(n, 0);
  for (int i = 0; i < n; i++) m.closed[i] = m_nodes[i].closed || i < m.step;
  for (int i = 0; i < n; i++) {
    if (m.closed[i]) continue;
    bool ready = true;
    for (int p : m_nodes[i].parents) ready &= m.closed[p] != 0;
    m.open[i] = ready;
  }
  m.complete.assign(m_tasks.size(), 0);
  for (size_t t = 0; t < m_tasks.size(); t++)
    for (int i : m_task_closers[t]) m.complete[t] |= m.closed[i];
  return m;
}

bool GameData::eval(const StoryCond& c, const StoryMoment& m) const {
  switch (c.op) {
    case StoryCond::False: return false;
    case StoryCond::True: return true;
    case StoryCond::Closed: return c.ref < (int)m.closed.size() && m.closed[c.ref];
    case StoryCond::Open: return c.ref < (int)m.open.size() && m.open[c.ref];
    case StoryCond::Complete: return c.ref < (int)m.complete.size() && m.complete[c.ref];
    case StoryCond::Not: return c.args.empty() || !eval(c.args[0], m);
    case StoryCond::And:
      for (const auto& a : c.args)
        if (!eval(a, m)) return false;
      return true;
    case StoryCond::Or:
      for (const auto& a : c.args)
        if (eval(a, m)) return true;
      return false;
  }
  return true;
}

uint32_t GameData::task_mask(const std::string& level, const std::string& host, const StoryMoment& m) const {
  const LevelInfo* info = this->level(level);
  const LevelInfo* host_info = this->level(host);
  // the story bits: task0..task7, done (level-method-22 / set-proto-vis!)
  uint32_t mask = info ? info->base_mask & 0x1ffu : 0;
  if (info) {
    for (size_t i = 0; i < m_nodes.size(); i++) {
      const StoryNode& n = m_nodes[i];
      if (!m.closed[i] || n.mask_op == StoryNode::NoMask || n.level != info->taskname) continue;
      if (n.mask_op == StoryNode::Abs) mask = n.mask;
      else if (n.mask_op == StoryNode::Set) mask |= n.mask;
      else mask &= ~n.mask;
    }
  }
  // the settings' bits: "never" always, "ctywide" out of the city (its traffic clears it),
  // "primary0" when no level shown is a primary one
  mask |= m_bit_never;
  if (!host_info || !host_info->traffic) mask |= m_bit_ctywide;
  if (!host_info || !(host_info->base_mask & m_bit_primary)) mask |= m_bit_primary;
  return mask;
}

bool GameData::actor_alive(uint32_t kill_mask, uint32_t task_mask, bool special) const {
  // the movie bits (16 and up) only matter during cutscenes
  const bool killed = (kill_mask & 0xffffu & task_mask) != 0;
  if (special) return (kill_mask & m_bit_special) && !killed;
  return !killed;
}

std::map<std::string, bool> GameData::borrowed(const std::string& host, const StoryMoment& m) const {
  std::map<int, StoryBorrow> slots;
  auto apply = [&](const std::vector<StoryBorrow>& list) {
    for (const auto& b : list)
      if (b.host == host) slots[b.slot] = b;
  };
  if (const LevelInfo* info = level(host)) apply(info->borrow);
  if (m_reset_node >= 0) apply(m_nodes[m_reset_node].borrow);
  for (size_t i = 0; i < m_nodes.size(); i++)
    if (m.open[i]) apply(m_nodes[i].borrow);
  std::map<std::string, bool> out;
  for (const auto& [slot, b] : slots)
    if (!b.level.empty()) out[b.level] = b.special;
  return out;
}

std::vector<std::string> GameData::layers_of(const std::string& host) const {
  std::set<std::string> out;
  if (const LevelInfo* info = level(host))
    for (const auto& b : info->borrow)
      if (!b.level.empty()) out.insert(b.level);
  for (const auto& n : m_nodes)
    for (const auto& b : n.borrow)
      if (b.host == host && !b.level.empty()) out.insert(b.level);
  return {out.begin(), out.end()};
}

const std::vector<PrototypeRule>* GameData::prototype_rules(const std::string& level) const {
  auto it = m_prototypes.find(level);
  return it == m_prototypes.end() ? nullptr : &it->second;
}

const StoryCond* GameData::type_rule(const std::string& etype) const {
  auto it = m_types.find(etype);
  return it == m_types.end() ? nullptr : &it->second;
}

float GameData::ocean_height(const std::string& level, int step) const {
  const LevelInfo* info = this->level(level);
  float height = 0.f;
  if (info) {
    if (info->ocean_height) height = *info->ocean_height;
    else if (const OceanMap* map = ocean(info->ocean)) height = map->corner.y;
  }
  // the story sets it (the sewer drained): the rule that held last, at that step or before
  auto it = m_heights.find(level);
  if (it != m_heights.end() && has_story()) {
    for (int s = std::clamp(step, 1, step_count()); s >= 1; s--) {
      const StoryMoment m = moment(s);
      for (const auto& r : it->second)
        if (eval(r.when, m)) return r.height;
    }
  }
  return height;
}

}  // namespace ogle
