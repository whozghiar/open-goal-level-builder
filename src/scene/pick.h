#pragma once

// Ray picking against the scene: meshes (BVH per primitive, local space) and actor markers.

#include <limits>

#include "scene/scene.h"

namespace ogle {

struct PickFilter {
  bool render = true;
  bool actors = true;
  bool collision = false;
  bool include_locked = false;
  const std::vector<uint32_t>* exclude_list = nullptr;  // these nodes and their descendants
};

struct PickHit {
  uint32_t node = 0;
  float t = std::numeric_limits<float>::max();
  Vec3 point;
  Vec3 normal{0, 1, 0};
  int prim = -1;
  uint32_t tri = 0;
  bool hit() const { return node != 0; }
};

PickHit pick(Scene& scene, const Ray& ray, const PickFilter& filter);

}  // namespace ogle
