#pragma once

// Loads library files on worker threads, so that the panels never freeze the window: models and
// prefabs (baked into one mesh) and whole levels (for their thumbnails). Results are kept in
// memory, the least recently used first to go.
//
//   auto m = store.model(path);   // nullptr while loading: ask again next frame
//   store.update();               // once per frame: starts queued loads, collects finished ones

#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "io/asset_library.h"

namespace ogle {

class AssetStore {
 public:
  ~AssetStore();
  // nullptr while loading or after a failure (logged once, see failed_*())
  std::shared_ptr<LoadedMesh> model(const std::string& path, bool urgent = false);
  std::shared_ptr<Scene> level(const std::string& background_path, bool urgent = false);
  bool failed_model(const std::string& path) const { return failed("m:" + path); }
  bool failed_level(const std::string& path) const { return failed("l:" + path); }
  void forget_model(const std::string& path) { m_entries.erase("m:" + path); }  // the file changed
  void update();
  size_t pending() const;  // queued or running loads
  void clear();            // forgets finished results (running loads finish and are dropped)

 private:
  enum class State { Queued, Running, Ready, Failed };
  struct Entry {
    State state = State::Queued;
    int kind = 0;  // 0 model, 1 level
    bool urgent = false;
    std::function<std::shared_ptr<void>()> work;
    std::future<std::shared_ptr<void>> future;
    std::shared_ptr<void> result;
    uint64_t last_use = 0;
  };
  std::shared_ptr<void> get(const std::string& key, int kind, bool urgent,
                            std::function<std::shared_ptr<void>()> work);
  bool failed(const std::string& key) const;
  void evict(int kind, size_t keep);

  std::map<std::string, Entry> m_entries;
  std::vector<std::future<std::shared_ptr<void>>> m_orphans;  // loads running when clear() ran
  uint64_t m_clock = 0;
};

}  // namespace ogle
