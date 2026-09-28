#pragma once

// Free-flying editor camera (yaw/pitch), Y up, meters.

#include "core/math.h"

namespace ogle {

struct Camera {
  Vec3 pos{0, 10, 30};
  float yaw = 0.f;    // radians, 0 looks down -Z
  float pitch = -0.3f;
  float fov_deg = 60.f;
  float znear = 0.1f;
  float zfar = 20000.f;
  float speed = 12.f;  // m/s

  Vec3 forward() const {
    return {-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
  }
  Vec3 right() const { return normalize(cross(forward(), Vec3{0, 1, 0})); }
  Vec3 up() const { return cross(right(), forward()); }
  Mat4 view() const { return Mat4::look_at(pos, pos + forward(), {0, 1, 0}); }
  Mat4 proj(float aspect) const {
    return Mat4::perspective(radians(fov_deg), aspect, znear, zfar);
  }
  void look_at(const Vec3& target) {
    Vec3 d = normalize(target - pos);
    pitch = std::asin(std::clamp(d.y, -0.999f, 0.999f));
    yaw = std::atan2(-d.x, -d.z);
  }
  // Ray through a pixel of a viewport of size w x h (pixels, origin top-left)
  Ray ray(float px, float py, float w, float h) const {
    float ndc_x = (px / w) * 2.f - 1.f;
    float ndc_y = 1.f - (py / h) * 2.f;
    float t = std::tan(radians(fov_deg) * 0.5f);
    float aspect = w / h;
    Vec3 d = normalize(forward() + right() * (ndc_x * t * aspect) + up() * (ndc_y * t));
    return {pos, d};
  }
  // Frames a box: keeps the orientation, backs off so the box fits.
  void frame(const AABB& box) {
    if (!box.valid()) return;
    float r = std::max(box.radius(), 0.5f);
    float dist = r / std::tan(radians(fov_deg) * 0.5f) * 1.1f;
    pos = box.center() - forward() * dist;
    speed = std::clamp(r * 0.15f, 2.f, 150.f);
  }
};

}  // namespace ogle
