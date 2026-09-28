#include "render/offscreen.h"

#include <cstring>

#include "glad/glad.h"
#include "stb_image_write.h"

namespace ogle {

OffscreenTarget::~OffscreenTarget() {
  if (fbo) glDeleteFramebuffers(1, &fbo);
  if (color) glDeleteTextures(1, &color);
  if (depth) glDeleteRenderbuffers(1, &depth);
}

bool OffscreenTarget::create(int w, int h) {
  width = w;
  height = h;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glGenTextures(1, &color);
  glBindTexture(GL_TEXTURE_2D, color);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
  glGenRenderbuffers(1, &depth);
  glBindRenderbuffer(GL_RENDERBUFFER, depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
  return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

std::vector<uint8_t> OffscreenTarget::read_rgba() const {
  GLint previous = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFinish();
  std::vector<uint8_t> pixels((size_t)width * height * 4), flipped(pixels.size());
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous);
  for (int y = 0; y < height; y++)
    std::memcpy(&flipped[(size_t)y * width * 4], &pixels[(size_t)(height - 1 - y) * width * 4], (size_t)width * 4);
  for (size_t i = 3; i < flipped.size(); i += 4) flipped[i] = 255;
  return flipped;
}

namespace {

// the picture is opaque: written without its alpha (a third smaller)
std::vector<uint8_t> rgb_of(const std::vector<uint8_t>& rgba) {
  std::vector<uint8_t> rgb(rgba.size() / 4 * 3);
  for (size_t i = 0, j = 0; i + 3 < rgba.size(); i += 4, j += 3) {
    rgb[j] = rgba[i];
    rgb[j + 1] = rgba[i + 1];
    rgb[j + 2] = rgba[i + 2];
  }
  return rgb;
}

}  // namespace

bool OffscreenTarget::save_png(const std::string& path) const {
  const auto rgb = rgb_of(read_rgba());
  return stbi_write_png(path.c_str(), width, height, 3, rgb.data(), width * 3) != 0;
}

std::vector<uint8_t> OffscreenTarget::png() const {
  const auto rgb = rgb_of(read_rgba());
  std::vector<uint8_t> out;
  stbi_write_png_to_func(
      [](void* ctx, void* data, int size) {
        auto* v = (std::vector<uint8_t>*)ctx;
        v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
      },
      &out, width, height, 3, rgb.data(), width * 3);
  return out;
}

}  // namespace ogle
