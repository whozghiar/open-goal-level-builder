#include "scene/pick.h"

#include <algorithm>

#include "scene/bvh.h"

namespace ogle {

namespace {

bool excluded(const Scene& scene, const PickFilter& f, uint32_t id) {
  if (!f.exclude_list) return false;
  for (auto e : *f.exclude_list)
    if (id == e || scene.is_ancestor(e, id)) return true;
  return false;
}

}  // namespace

PickHit pick(Scene& scene, const Ray& ray, const PickFilter& f) {
  PickHit best;
  const SceneCache& cache = scene.cache();
  for (size_t i = 0; i < scene.nodes.size(); i++) {
    auto& n = scene.nodes[i];
    if (!cache.visible[i]) continue;
    if (n.locked && !f.include_locked) continue;
    const bool allowed = (n.kind == NodeKind::Render && f.render) || (n.kind == NodeKind::Collision && f.collision) ||
                         (n.kind == NodeKind::Actor && f.actors);
    if (!allowed) continue;
    const Mat4& w = cache.worlds[i];
    // cheap rejection first: most nodes of a level are far from the ray
    if (n.mesh && !ray_aabb(ray, cache.bounds[i].expanded(0.01f), best.t)) continue;
    if (excluded(scene, f, n.id)) continue;
    if (n.kind == NodeKind::Actor) {
      float t = ray_sphere(ray, w.col3(3), kMarkerRadius * 1.3f);
      if (t > 0 && t < best.t) {
        best.node = n.id;
        best.t = t;
        best.point = ray.at(t);
        best.normal = {0, 1, 0};
        best.prim = -1;
      }
      continue;
    }
    if (!n.mesh) continue;
    Ray local = transform_ray(w.inverse(), ray);
    for (size_t p = 0; p < n.mesh->prims.size(); p++) {
      const auto& prim = n.mesh->prims[p];
      const Bvh& bvh = n.mesh->bvh(p);
      float t;
      uint32_t tri;
      // the see-through surfaces of the terrain (water, glass) do not hide what is behind them
      if (n.locked && prim.material >= 0 && prim.material < (int)scene.materials.size() &&
          scene.materials[prim.material].alpha == AlphaMode::Blend)
        continue;
      if (bvh.raycast(local, prim.pos, prim.idx, best.t, &t, &tri)) {
        Vec3 a = w.point(prim.pos[prim.idx[tri * 3]]);
        Vec3 b = w.point(prim.pos[prim.idx[tri * 3 + 1]]);
        Vec3 c = w.point(prim.pos[prim.idx[tri * 3 + 2]]);
        Vec3 nrm = normalize(cross(b - a, c - a));
        if (dot(nrm, ray.d) > 0) nrm = -nrm;
        best.node = n.id;
        best.t = t;
        best.point = ray.at(t);
        best.normal = nrm;
        best.prim = (int)p;
        best.tri = tri;
      }
    }
  }
  return best;
}

}  // namespace ogle
