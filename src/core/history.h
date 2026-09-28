#pragma once

// Undo/redo. Every change of the document goes through a HistoryEntry.
// NodeChange records the state of some nodes before a change and builds the entry afterwards.

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "scene/scene.h"

namespace ogle {

struct HistoryEntry {
  std::string label;
  std::function<void()> undo;
  std::function<void()> redo;
};

class History {
 public:
  void push(HistoryEntry e);
  bool can_undo() const { return m_cursor > 0; }
  bool can_redo() const { return m_cursor < m_entries.size(); }
  void undo();
  void redo();
  void clear();
  std::string undo_label() const;
  std::string redo_label() const;
  // changes every time the document changes (dirty tracking, autosave)
  uint64_t revision() const { return m_revision; }

 private:
  std::vector<HistoryEntry> m_entries;
  size_t m_cursor = 0;
  uint64_t m_revision = 0;
  static constexpr size_t kMax = 200;
};

// Snapshot-based node change:
//   NodeChange c(scene);
//   c.save(id);            // before modifying or deleting node `id`
//   ... modify the scene (nodes created after the constructor are detected) ...
//   history.push(c.commit("Move"));
class NodeChange {
 public:
  explicit NodeChange(Scene& scene);
  void save(uint32_t id);
  void save_subtree(uint32_t id);
  HistoryEntry commit(const std::string& label, std::function<void()> after_apply = {});

 private:
  Scene& m_scene;
  std::vector<uint32_t> m_order_before;
  std::map<uint32_t, std::optional<Node>> m_before;
};

}  // namespace ogle
