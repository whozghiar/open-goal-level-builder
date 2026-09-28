#include "scene/story.h"

#include <unordered_map>

namespace ogle {

namespace {

// What the story decides in a document, collected once per evaluation.
struct Item {
  enum Kind : uint8_t { Actor, Prototype, Layer, LayerDecor };
  size_t index = 0;  // in Scene::nodes
  Kind kind = Actor;
  int level = 0;     // in Items::levels
  uint32_t kill_mask = 0;
  std::vector<const StoryCond*> rules;  // all must hold
};

struct Items {
  std::vector<std::string> levels;  // [0]: the level opened
  std::vector<uint8_t> is_layer;
  std::vector<Item> items;
};

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string extra_string(const Node& n, const char* key) {
  auto it = n.extras.find(key);
  return it != n.extras.end() && it->is_string() ? it->get<std::string>() : std::string();
}

uint32_t kill_mask_of(const Node& n) {
  auto lump = n.extras.find("lump");
  if (lump == n.extras.end() || !lump->is_object()) return 0;
  auto km = lump->find("kill-mask");
  if (km == lump->end() || !km->is_number()) return 0;
  return (uint32_t)(int64_t)km->get<double>();
}

Items collect(const Scene& scene, const GameData& data) {
  Items out;
  out.levels.push_back(scene.settings.level);
  out.is_layer.push_back(0);
  std::unordered_map<std::string, int> level_ids;
  auto level_id = [&](const std::string& name, bool layer) {
    auto it = level_ids.find(name);
    if (it != level_ids.end()) return it->second;
    const int id = (int)out.levels.size();
    out.levels.push_back(name);
    out.is_layer.push_back(layer);
    level_ids[name] = id;
    return id;
  };
  const size_t n = scene.nodes.size();
  std::unordered_map<uint32_t, size_t> index;
  index.reserve(n);
  for (size_t i = 0; i < n; i++) index[scene.nodes[i].id] = i;
  // the level of each node: a group of an extra level, else the level opened
  std::vector<int> level_of(n, -1);
  std::vector<size_t> chain;
  for (size_t i = 0; i < n; i++) {
    chain.clear();
    size_t cur = i;
    int found = -1;
    while (true) {
      if (level_of[cur] >= 0) {
        found = level_of[cur];
        break;
      }
      chain.push_back(cur);
      const Node& node = scene.nodes[cur];
      const std::string layer = extra_string(node, "og_layer");
      const std::string companion = extra_string(node, "og_companion");
      if (!layer.empty() || !companion.empty()) {
        found = layer.empty() ? level_id(companion, false) : level_id(layer, true);
        break;
      }
      auto p = node.parent ? index.find(node.parent) : index.end();
      if (p == index.end() || chain.size() > 1000) {
        found = 0;
        break;
      }
      cur = p->second;
    }
    for (size_t c : chain) level_of[c] = found;
  }

  for (size_t i = 0; i < n; i++) {
    const Node& node = scene.nodes[i];
    const int level = level_of[i];
    const bool in_layer = out.is_layer[level] != 0;
    if (!extra_string(node, "og_layer").empty()) {
      out.items.push_back({i, Item::Layer, level, 0, {}});
      continue;
    }
    auto pit = node.parent ? index.find(node.parent) : index.end();
    const Node* parent = pit != index.end() ? &scene.nodes[pit->second] : nullptr;
    if (in_layer && parent && !extra_string(*parent, "og_layer").empty() && !ends_with(node.name, "-actors")) {
      out.items.push_back({i, Item::LayerDecor, level, 0, {}});
      continue;
    }
    if (node.kind == NodeKind::Actor) {
      Item it{i, Item::Actor, level, kill_mask_of(node), {}};
      if (const StoryCond* rule = data.type_rule(extra_string(node, "etype"))) it.rules.push_back(rule);
      if (it.kill_mask || !it.rules.empty() || in_layer) out.items.push_back(std::move(it));
      continue;
    }
    // a decor prototype: a node under the level's tie or shrub trees
    if (parent && (parent->name.find("-tie-") != std::string::npos || parent->name.find("-shrub-") != std::string::npos)) {
      const auto* rules = data.prototype_rules(out.levels[level]);
      if (!rules) continue;
      std::string name = node.name;
      if (ends_with(name, ".mb")) name.resize(name.size() - 3);
      Item it{i, Item::Prototype, level, 0, {}};
      for (const auto& r : *rules)
        for (const auto& proto : r.names)
          if (proto == name) it.rules.push_back(&r.when);
      if (!it.rules.empty()) out.items.push_back(std::move(it));
    }
  }
  return out;
}

// hidden[k]: whether items.items[k] is hidden at that moment
void evaluate(const Items& items, const GameData& data, const StoryMoment& m, std::vector<uint8_t>& hidden) {
  const std::string& host = items.levels[0];
  const auto borrowed = data.borrowed(host, m);
  std::vector<uint32_t> masks(items.levels.size());
  for (size_t l = 0; l < items.levels.size(); l++) masks[l] = data.task_mask(items.levels[l], host, m);
  hidden.assign(items.items.size(), 0);
  for (size_t k = 0; k < items.items.size(); k++) {
    const Item& it = items.items[k];
    const std::string& level = items.levels[it.level];
    auto b = items.is_layer[it.level] ? borrowed.find(level) : borrowed.end();
    const bool special = b != borrowed.end() && b->second;
    bool hide = false;
    switch (it.kind) {
      case Item::Layer: hide = b == borrowed.end(); break;
      case Item::LayerDecor: hide = special; break;  // a 'special level draws no decor
      case Item::Actor: hide = !data.actor_alive(it.kill_mask, masks[it.level], special); break;
      case Item::Prototype: break;
    }
    for (const StoryCond* r : it.rules) hide |= !data.eval(*r, m);
    hidden[k] = hide;
  }
}

}  // namespace

void apply_story(Scene& scene, const GameData* data, const StoryView& view) {
  for (auto& n : scene.nodes) n.story_hidden = false;
  scene.begin_frame();
  if (!data || !data->has_story() || !view.enabled || scene.nodes.empty()) return;
  const Items items = collect(scene, *data);
  std::vector<uint8_t> hidden;
  evaluate(items, *data, data->moment(view.step), hidden);
  for (size_t k = 0; k < items.items.size(); k++) scene.nodes[items.items[k].index].story_hidden = hidden[k] != 0;
}

std::vector<int> story_changes(const Scene& scene, const GameData& data) {
  std::vector<int> out;
  if (!data.has_story() || scene.nodes.empty()) return out;
  const Items items = collect(scene, data);
  if (items.items.empty()) return out;
  std::vector<uint8_t> prev, cur;
  for (int s = 1; s <= data.step_count(); s++) {
    evaluate(items, data, data.moment(s), cur);
    if (s > 1 && cur != prev) out.push_back(s);
    prev.swap(cur);
  }
  return out;
}

}  // namespace ogle
