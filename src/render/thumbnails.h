#pragma once

// Thumbnails of the panels: prefabs (and models), whole levels and decor parts of a level (the
// mesh of one of its nodes), drawn offscreen (3/4 view)
// and cached as PNG files in <preferences>/thumbs, so each is drawn once. get() returns 0 until
// the thumbnail is ready: the geometry is loaded by the AssetStore's worker threads and drawn by
// update() on the main thread.

#include <string>
#include <unordered_map>

#include "io/asset_store.h"
#include "render/renderer.h"

namespace ogle {

struct ThumbRequest {
  enum class Kind { Model = 0, Level, Part } kind = Kind::Model;
  std::string path;   // .glb: a model or prefab, or a level's background
  std::string node;   // Part: the node of the level whose mesh is drawn
  int64_t stamp = 0;  // file date: a changed file gets a new thumbnail
  std::string key() const;
};

class Thumbnails {
 public:
  explicit Thumbnails(AssetStore& store) : m_store(store) {}
  bool init(std::string* error);  // needs the GL context
  void shutdown();
  unsigned get(const ThumbRequest& req);  // GL texture, 0 while it is not ready
  bool failed(const ThumbRequest& req) const;
  // Main thread, once per frame, before the viewport is drawn (the bound framebuffer is kept).
  void update();
  size_t pending() const;  // thumbnails asked for recently and not ready yet
  static constexpr int kSize = 128;

 private:
  struct Item {
    ThumbRequest req;
    unsigned tex = 0;
    uint64_t last_use = 0;
    bool disk_checked = false;
    bool failed = false;
  };
  bool render(Item& item);
  unsigned upload(const uint8_t* rgba, int w, int h);
  std::string cache_file(const std::string& key) const;

  AssetStore& m_store;
  Renderer m_renderer;
  std::unordered_map<std::string, Item> m_items;
  unsigned m_fbo = 0, m_color = 0, m_depth = 0;
  uint64_t m_frame = 0;
};

}  // namespace ogle
