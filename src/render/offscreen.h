#pragma once

// An offscreen framebuffer (color and depth) to draw into and read back: the captures of the
// whole editor (--screenshot) and of its view (the MCP server's capture_view).

#include <cstdint>
#include <string>
#include <vector>

namespace ogle {

struct OffscreenTarget {
  unsigned fbo = 0, color = 0, depth = 0;
  int width = 0, height = 0;

  OffscreenTarget() = default;
  OffscreenTarget(const OffscreenTarget&) = delete;
  OffscreenTarget& operator=(const OffscreenTarget&) = delete;
  ~OffscreenTarget();

  bool create(int w, int h);  // needs the GL context; leaves the framebuffer bound
  std::vector<uint8_t> read_rgba() const;  // top row first, opaque
  bool save_png(const std::string& path) const;
  std::vector<uint8_t> png() const;  // the bytes of a PNG file
};

}  // namespace ogle
