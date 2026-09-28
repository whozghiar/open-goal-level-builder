#pragma once

// Bounding volume hierarchy over the triangles of one primitive, in the primitive's local space.
// Used for picking (ray casts) and for area queries (collision brush, snapping).

#include <cstdint>
#include <vector>

#include "core/math.h"

namespace ogle {

struct Bvh {
  struct Node {
    AABB box;
    uint32_t first = 0;  // leaf: first index into tri_order; inner: index of left child
    uint32_t count = 0;  // leaf: number of triangles; inner: 0 (right child = left + 1)
  };
  std::vector<Node> nodes;
  std::vector<uint32_t> tri_order;

  void build(const std::vector<Vec3>& pos, const std::vector<uint32_t>& idx);
  bool empty() const { return nodes.empty(); }

  // nearest hit with t < tmax; returns the triangle index in *tri
  bool raycast(const Ray& ray, const std::vector<Vec3>& pos, const std::vector<uint32_t>& idx,
               float tmax, float* t, uint32_t* tri) const;
  // triangles whose bounds overlap the box
  void query(const AABB& box, const std::vector<Vec3>& pos, const std::vector<uint32_t>& idx,
             std::vector<uint32_t>& out) const;
};

}  // namespace ogle
