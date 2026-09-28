#include "io/asset_store.h"

#include <algorithm>
#include <chrono>

#include "core/log.h"
#include "io/gltf_io.h"

namespace ogle {

namespace {
constexpr size_t kMaxRunning = 3;      // loads at once
constexpr size_t kMaxLevels = 1;       // of which levels (hundreds of MB each)
constexpr uint64_t kStaleFrames = 30;  // queued loads nobody asked for since are dropped
}  // namespace

AssetStore::~AssetStore() {
  // futures of std::async wait for their thread in their destructor
  m_entries.clear();
  m_orphans.clear();
}

std::shared_ptr<void> AssetStore::get(const std::string& key, int kind, bool urgent,
                                      std::function<std::shared_ptr<void>()> work) {
  auto it = m_entries.find(key);
  if (it == m_entries.end()) {
    Entry e;
    e.kind = kind;
    e.work = std::move(work);
    it = m_entries.emplace(key, std::move(e)).first;
  }
  Entry& e = it->second;
  e.last_use = m_clock;
  e.urgent |= urgent;
  return e.state == State::Ready ? e.result : nullptr;
}

bool AssetStore::failed(const std::string& key) const {
  auto it = m_entries.find(key);
  return it != m_entries.end() && it->second.state == State::Failed;
}

std::shared_ptr<LoadedMesh> AssetStore::model(const std::string& path, bool urgent) {
  auto r = get("m:" + path, 0, urgent, [path]() -> std::shared_ptr<void> {
    auto out = std::make_shared<LoadedMesh>();
    std::string err;
    if (!load_model_asset(path, out.get(), &err)) {
      LOG_WARN("%s: %s", path.c_str(), err.c_str());
      return nullptr;
    }
    return out;
  });
  return std::static_pointer_cast<LoadedMesh>(r);
}

std::shared_ptr<Scene> AssetStore::level(const std::string& path, bool urgent) {
  auto r = get("l:" + path, 1, urgent, [path]() -> std::shared_ptr<void> {
    auto out = std::make_shared<Scene>();
    ImportOptions o;
    ImportResult res;
    std::string err;
    if (!import_gltf(*out, path, o, &res, &err)) {
      LOG_WARN("%s: %s", path.c_str(), err.c_str());
      return nullptr;
    }
    return out;
  });
  return std::static_pointer_cast<Scene>(r);
}

void AssetStore::update() {
  using namespace std::chrono_literals;
  m_clock++;
  size_t running = 0, running_levels = 0;
  for (auto it = m_entries.begin(); it != m_entries.end();) {
    Entry& e = it->second;
    if (e.state == State::Running) {
      if (e.future.wait_for(0s) == std::future_status::ready) {
        e.result = e.future.get();
        e.state = e.result ? State::Ready : State::Failed;
        e.work = nullptr;
      } else {
        running++;
        if (e.kind == 1) running_levels++;
      }
    } else if (e.state == State::Queued && e.last_use + kStaleFrames < m_clock && !e.urgent) {
      it = m_entries.erase(it);  // scrolled away before it started
      continue;
    }
    ++it;
  }
  m_orphans.erase(std::remove_if(m_orphans.begin(), m_orphans.end(),
                                 [](auto& f) { return f.wait_for(0s) == std::future_status::ready; }),
                  m_orphans.end());
  // urgent loads first (placement), then the most recently asked (visible thumbnails)
  while (running < kMaxRunning) {
    Entry* best = nullptr;
    for (auto& [k, e] : m_entries) {
      if (e.state != State::Queued || (e.kind == 1 && running_levels >= kMaxLevels)) continue;
      if (!best || (e.urgent && !best->urgent) || (e.urgent == best->urgent && e.last_use > best->last_use))
        best = &e;
    }
    if (!best) break;
    best->state = State::Running;
    best->future = std::async(std::launch::async, best->work);
    running++;
    if (best->kind == 1) running_levels++;
  }
  evict(0, 48);
  evict(1, 2);
}

void AssetStore::evict(int kind, size_t keep) {
  std::vector<std::pair<uint64_t, std::string>> ready;
  for (auto& [k, e] : m_entries)
    if (e.kind == kind && e.state == State::Ready) ready.push_back({e.last_use, k});
  if (ready.size() <= keep) return;
  std::sort(ready.begin(), ready.end());
  for (size_t i = 0; i + keep < ready.size(); i++) m_entries.erase(ready[i].second);
}

size_t AssetStore::pending() const {
  size_t n = 0;
  for (auto& [k, e] : m_entries)
    if (e.state == State::Queued || e.state == State::Running) n++;
  return n;
}

void AssetStore::clear() {
  for (auto& [k, e] : m_entries)
    if (e.state == State::Running) m_orphans.push_back(std::move(e.future));
  m_entries.clear();
}

}  // namespace ogle
