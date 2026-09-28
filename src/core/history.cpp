#include "core/history.h"

#include <set>
#include <unordered_map>

namespace ogle {

void History::push(HistoryEntry e) {
  m_entries.resize(m_cursor);
  m_entries.push_back(std::move(e));
  if (m_entries.size() > kMax) m_entries.erase(m_entries.begin());
  m_cursor = m_entries.size();
  m_revision++;
}

void History::undo() {
  if (!can_undo()) return;
  m_cursor--;
  if (m_entries[m_cursor].undo) m_entries[m_cursor].undo();
  m_revision++;
}

void History::redo() {
  if (!can_redo()) return;
  if (m_entries[m_cursor].redo) m_entries[m_cursor].redo();
  m_cursor++;
  m_revision++;
}

void History::clear() {
  m_entries.clear();
  m_cursor = 0;
  m_revision++;
}

std::string History::undo_label() const { return can_undo() ? m_entries[m_cursor - 1].label : ""; }
std::string History::redo_label() const { return can_redo() ? m_entries[m_cursor].label : ""; }

namespace {

// Replace the given nodes by their recorded state (or remove them), then restore the order.
void apply_states(Scene& scene, const std::map<uint32_t, std::optional<Node>>& states,
                  const std::vector<uint32_t>& order) {
  std::unordered_map<uint32_t, Node> by_id;
  for (auto& n : scene.nodes) by_id.emplace(n.id, std::move(n));
  for (const auto& [id, st] : states) {
    if (st) by_id[id] = *st;
    else by_id.erase(id);
  }
  scene.nodes.clear();
  for (uint32_t id : order) {
    auto it = by_id.find(id);
    if (it != by_id.end()) {
      scene.nodes.push_back(std::move(it->second));
      by_id.erase(it);
    }
  }
  for (auto& [id, n] : by_id) scene.nodes.push_back(std::move(n));
  for (const auto& n : scene.nodes) scene.next_id = std::max(scene.next_id, n.id + 1);
}

}  // namespace

NodeChange::NodeChange(Scene& scene) : m_scene(scene) {
  m_order_before.reserve(scene.nodes.size());
  for (const auto& n : scene.nodes) m_order_before.push_back(n.id);
}

void NodeChange::save(uint32_t id) {
  if (m_before.count(id)) return;
  const Node* n = m_scene.find(id);
  m_before[id] = n ? std::optional<Node>(*n) : std::nullopt;
}

void NodeChange::save_subtree(uint32_t id) {
  save(id);
  for (auto d : m_scene.descendants(id)) save(d);
}

HistoryEntry NodeChange::commit(const std::string& label, std::function<void()> after_apply) {
  std::set<uint32_t> existed(m_order_before.begin(), m_order_before.end());
  std::map<uint32_t, std::optional<Node>> before = m_before;
  std::map<uint32_t, std::optional<Node>> after;
  // new nodes
  for (const auto& n : m_scene.nodes) {
    if (!existed.count(n.id)) before[n.id] = std::nullopt;
  }
  for (const auto& [id, st] : before) {
    const Node* n = m_scene.find(id);
    after[id] = n ? std::optional<Node>(*n) : std::nullopt;
  }
  std::vector<uint32_t> order_after;
  for (const auto& n : m_scene.nodes) order_after.push_back(n.id);
  std::vector<uint32_t> order_before = m_order_before;
  Scene* scene = &m_scene;
  HistoryEntry e;
  e.label = label;
  e.undo = [scene, before, order_before, after_apply]() {
    apply_states(*scene, before, order_before);
    if (after_apply) after_apply();
  };
  e.redo = [scene, after, order_after, after_apply]() {
    apply_states(*scene, after, order_after);
    if (after_apply) after_apply();
  };
  return e;
}

}  // namespace ogle
