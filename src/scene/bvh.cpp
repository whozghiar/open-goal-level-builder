#include "scene/bvh.h"

#include <algorithm>

namespace ogle {

namespace {
constexpr uint32_t kLeafSize = 4;
}

void Bvh::build(const std::vector<Vec3>& pos, const std::vector<uint32_t>& idx) {
  nodes.clear();
  tri_order.clear();
  const uint32_t ntri = (uint32_t)(idx.size() / 3);
  if (ntri == 0) return;
  std::vector<AABB> tri_box(ntri);
  std::vector<Vec3> centroid(ntri);
  for (uint32_t t = 0; t < ntri; t++) {
    const Vec3& a = pos[idx[t * 3]];
    const Vec3& b = pos[idx[t * 3 + 1]];
    const Vec3& c = pos[idx[t * 3 + 2]];
    tri_box[t].add(a);
    tri_box[t].add(b);
    tri_box[t].add(c);
    centroid[t] = (a + b + c) / 3.f;
  }
  tri_order.resize(ntri);
  for (uint32_t t = 0; t < ntri; t++) tri_order[t] = t;
  nodes.reserve(ntri * 2 / kLeafSize + 2);

  struct Task {
    uint32_t node, first, count;
  };
  nodes.push_back({});
  std::vector<Task> stack{{0, 0, ntri}};
  while (!stack.empty()) {
    Task task = stack.back();
    stack.pop_back();
    AABB box, cbox;
    for (uint32_t i = task.first; i < task.first + task.count; i++) {
      box.add(tri_box[tri_order[i]]);
      cbox.add(centroid[tri_order[i]]);
    }
    nodes[task.node].box = box;
    if (task.count <= kLeafSize) {
      nodes[task.node].first = task.first;
      nodes[task.node].count = task.count;
      continue;
    }
    Vec3 ext = cbox.size();
    int axis = ext.x > ext.y ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
    uint32_t mid = task.first + task.count / 2;
    std::nth_element(tri_order.begin() + task.first, tri_order.begin() + mid,
                     tri_order.begin() + task.first + task.count,
                     [&](uint32_t a, uint32_t b) { return centroid[a][axis] < centroid[b][axis]; });
    uint32_t left = (uint32_t)nodes.size();
    nodes.push_back({});
    nodes.push_back({});
    nodes[task.node].first = left;
    nodes[task.node].count = 0;
    stack.push_back({left, task.first, mid - task.first});
    stack.push_back({left + 1, mid, task.first + task.count - mid});
  }
}

bool Bvh::raycast(const Ray& ray, const std::vector<Vec3>& pos, const std::vector<uint32_t>& idx,
                  float tmax, float* t_out, uint32_t* tri_out) const {
  if (nodes.empty()) return false;
  float best = tmax;
  bool hit = false;
  uint32_t stack[128];
  int sp = 0;
  stack[sp++] = 0;
  while (sp > 0) {
    const Node& n = nodes[stack[--sp]];
    if (!ray_aabb(ray, n.box, best)) continue;
    if (n.count > 0) {
      for (uint32_t i = n.first; i < n.first + n.count; i++) {
        uint32_t tri = tri_order[i];
        float t = ray_triangle(ray, pos[idx[tri * 3]], pos[idx[tri * 3 + 1]], pos[idx[tri * 3 + 2]]);
        if (t > 0 && t < best) {
          best = t;
          *tri_out = tri;
          hit = true;
        }
      }
    } else if (sp + 2 <= 128) {
      stack[sp++] = n.first;
      stack[sp++] = n.first + 1;
    }
  }
  if (hit) *t_out = best;
  return hit;
}

void Bvh::query(const AABB& box, const std::vector<Vec3>& pos, const std::vector<uint32_t>& idx,
                std::vector<uint32_t>& out) const {
  if (nodes.empty()) return;
  std::vector<uint32_t> stack{0};
  while (!stack.empty()) {
    const Node& n = nodes[stack.back()];
    stack.pop_back();
    if (!n.box.overlaps(box)) continue;
    if (n.count > 0) {
      for (uint32_t i = n.first; i < n.first + n.count; i++) {
        uint32_t tri = tri_order[i];
        AABB tb;
        tb.add(pos[idx[tri * 3]]);
        tb.add(pos[idx[tri * 3 + 1]]);
        tb.add(pos[idx[tri * 3 + 2]]);
        if (tb.overlaps(box)) out.push_back(tri);
      }
    } else {
      stack.push_back(n.first);
      stack.push_back(n.first + 1);
    }
  }
}

}  // namespace ogle
