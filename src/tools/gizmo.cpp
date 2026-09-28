#include "tools/gizmo.h"

#include <cmath>

#include "imgui.h"

namespace ogle {

namespace {

float dist_point_segment(const Vec2& p, const Vec2& a, const Vec2& b) {
  Vec2 ab = b - a, ap = p - a;
  float len2 = ab.x * ab.x + ab.y * ab.y;
  float t = len2 > 1e-6f ? std::clamp((ap.x * ab.x + ap.y * ab.y) / len2, 0.f, 1.f) : 0.f;
  Vec2 c = a + ab * t;
  return (p - c).length();
}

bool point_in_quad(const Vec2& p, const Vec2 q[4]) {
  // convex quad, either winding
  int sign = 0;
  for (int i = 0; i < 4; i++) {
    Vec2 a = q[i], b = q[(i + 1) % 4];
    float c = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
    int s = c > 0 ? 1 : (c < 0 ? -1 : 0);
    if (s == 0) continue;
    if (sign == 0) sign = s;
    else if (s != sign) return false;
  }
  return true;
}

ImU32 axis_color(int i, bool hot) {
  if (hot) return IM_COL32(255, 220, 70, 255);
  switch (i) {
    case 0: return IM_COL32(232, 72, 72, 255);
    case 1: return IM_COL32(96, 206, 90, 255);
    case 2: return IM_COL32(80, 132, 245, 255);
    default: return IM_COL32(230, 230, 230, 255);
  }
}

Vec3 perpendicular(const Vec3& v) {
  Vec3 o = std::fabs(v.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
  return normalize(cross(v, o));
}

float snap_value(float v, float step, bool enabled) {
  if (!enabled || step <= 0) return v;
  return std::round(v / step) * step;
}

}  // namespace

bool Gizmo::ray_on_drag_plane(const Ray& ray, Vec3* p) const {
  float t;
  if (!ray_plane(ray, m_plane_point, m_plane_normal, &t) || t < 0) return false;
  *p = ray.at(t);
  return true;
}

int Gizmo::hit_test(const Camera& cam, const Renderer& r, const GizmoInput& in, const Mat4& f,
                    float size) const {
  Vec3 pivot = f.col3(3);
  Vec2 o;
  if (!r.project(pivot, &o)) return -1;
  const Vec2 m = in.mouse;
  if ((m - o).length() < 10.f && op != GizmoOp::Rotate) return 6;
  if (op == GizmoOp::Rotate) {
    int best = -1;
    float best_d = 9.f;
    for (int i = 0; i < 3; i++) {
      Vec3 a = f.col3(i);
      Vec3 u = perpendicular(a), v = cross(a, u);
      Vec2 prev;
      bool has_prev = false;
      for (int k = 0; k <= 64; k++) {
        float ang = (float)k / 64 * 2.f * kPi;
        Vec2 s;
        bool ok = r.project(pivot + (u * std::cos(ang) + v * std::sin(ang)) * size, &s);
        if (ok && has_prev) {
          float d = dist_point_segment(m, prev, s);
          if (d < best_d) {
            best_d = d;
            best = i;
          }
        }
        prev = s;
        has_prev = ok;
      }
    }
    return best;
  }
  if (op == GizmoOp::Translate) {
    for (int k = 0; k < 3; k++) {
      int i = (k + 1) % 3, j = (k + 2) % 3;
      Vec3 ai = f.col3(i), aj = f.col3(j);
      Vec2 q[4];
      bool ok = r.project(pivot + (ai * 0.18f + aj * 0.18f) * size, &q[0]) &&
                r.project(pivot + (ai * 0.42f + aj * 0.18f) * size, &q[1]) &&
                r.project(pivot + (ai * 0.42f + aj * 0.42f) * size, &q[2]) &&
                r.project(pivot + (ai * 0.18f + aj * 0.42f) * size, &q[3]);
      if (ok && point_in_quad(m, q)) return 3 + k;
    }
  }
  int best = -1;
  float best_d = 8.f;
  for (int i = 0; i < 3; i++) {
    Vec2 e;
    if (!r.project(pivot + f.col3(i) * size, &e)) continue;
    float d = dist_point_segment(m, o, e);
    if (d < best_d) {
      best_d = d;
      best = i;
    }
  }
  (void)cam;
  return best;
}

void Gizmo::draw(const Renderer& r, const GizmoInput& in, const Mat4& f, float size,
                 ImDrawList* dl) const {
  if (!dl) return;
  Vec3 pivot = f.col3(3);
  Vec2 o;
  if (!r.project(pivot, &o)) return;
  auto S = [&](const Vec2& v) { return ImVec2(in.viewport_pos.x + v.x, in.viewport_pos.y + v.y); };
  int hot = m_active >= 0 ? m_active : m_hot;
  if (op == GizmoOp::Scale) {
    // an axis with a square at its end for each direction, a square in the center for all
    for (int i = 0; i < 3; i++) {
      Vec2 e;
      if (!r.project(pivot + f.col3(i) * size, &e)) continue;
      ImU32 c = axis_color(i, hot == i);
      dl->AddLine(S(o), S(e), c, hot == i ? 4.f : 2.8f);
      dl->AddRectFilled(ImVec2(S(e).x - 5, S(e).y - 5), ImVec2(S(e).x + 5, S(e).y + 5), c);
    }
    ImU32 cc = hot == 6 ? IM_COL32(255, 220, 70, 255) : IM_COL32(235, 235, 235, 230);
    dl->AddRectFilled(ImVec2(S(o).x - 6, S(o).y - 6), ImVec2(S(o).x + 6, S(o).y + 6), cc);
    return;
  }
  if (op == GizmoOp::Rotate) {
    for (int i = 0; i < 3; i++) {
      Vec3 a = f.col3(i);
      Vec3 u = perpendicular(a), v = cross(a, u);
      std::vector<ImVec2> pts;
      for (int k = 0; k <= 64; k++) {
        float ang = (float)k / 64 * 2.f * kPi;
        Vec2 s;
        if (r.project(pivot + (u * std::cos(ang) + v * std::sin(ang)) * size, &s)) pts.push_back(S(s));
      }
      if (pts.size() > 1) dl->AddPolyline(pts.data(), (int)pts.size(), axis_color(i, hot == i), 0, hot == i ? 3.5f : 2.2f);
    }
    dl->AddCircleFilled(S(o), 3.f, IM_COL32(240, 240, 240, 220));
    return;
  }
  // plane handles
  for (int k = 0; k < 3; k++) {
    int i = (k + 1) % 3, j = (k + 2) % 3;
    Vec3 ai = f.col3(i), aj = f.col3(j);
    Vec2 q[4];
    if (r.project(pivot + (ai * 0.18f + aj * 0.18f) * size, &q[0]) &&
        r.project(pivot + (ai * 0.42f + aj * 0.18f) * size, &q[1]) &&
        r.project(pivot + (ai * 0.42f + aj * 0.42f) * size, &q[2]) &&
        r.project(pivot + (ai * 0.18f + aj * 0.42f) * size, &q[3])) {
      ImU32 c = hot == 3 + k ? IM_COL32(255, 220, 70, 150) : (axis_color(k, false) & 0x00ffffff) | 0x70000000;
      dl->AddQuadFilled(S(q[0]), S(q[1]), S(q[2]), S(q[3]), c);
    }
  }
  for (int i = 0; i < 3; i++) {
    Vec2 e;
    Vec3 a = f.col3(i);
    if (!r.project(pivot + a * size, &e)) continue;
    ImU32 c = axis_color(i, hot == i);
    dl->AddLine(S(o), S(e), c, hot == i ? 4.f : 2.8f);
    Vec2 d = e - o;
    float len = d.length();
    if (len > 1) {
      Vec2 dir = d * (1.f / len), nrm{-dir.y, dir.x};
      Vec2 base = e - dir * 13.f;
      dl->AddTriangleFilled(S(e + dir * 3.f), S(base + nrm * 6.f), S(base - nrm * 6.f), c);
    }
  }
  ImU32 cc = hot == 6 ? IM_COL32(255, 220, 70, 255) : IM_COL32(235, 235, 235, 230);
  dl->AddRect(ImVec2(S(o).x - 6, S(o).y - 6), ImVec2(S(o).x + 6, S(o).y + 6), cc, 0.f, 0, 2.f);
}

GizmoResult Gizmo::update(const Camera& cam, const Renderer& r, const GizmoInput& in,
                          const Mat4& frame, ImDrawList* dl) {
  GizmoResult res;
  res.delta = Mat4::identity();
  // orthonormal gizmo frame
  // scaling is along the selection's own axes
  const bool local = space == GizmoSpace::Local || op == GizmoOp::Scale;
  Mat4 f = Mat4::identity();
  if (local) {
    Vec3 x = normalize(frame.col3(0)), y = frame.col3(1), z;
    if (length2(x) == 0) x = {1, 0, 0};
    z = normalize(cross(x, y));
    if (length2(z) == 0) z = perpendicular(x);
    y = cross(z, x);
    f.set_col3(0, x);
    f.set_col3(1, y);
    f.set_col3(2, z);
  }
  f.set_col3(3, frame.col3(3));
  const Vec3 pivot = f.col3(3);
  float dist = std::max(length(pivot - cam.pos), 0.5f);
  float size = dist * std::tan(radians(cam.fov_deg) * 0.5f) * 0.24f;

  if (m_active < 0) {
    m_hot = in.mouse_in_viewport ? hit_test(cam, r, in, f, size) : -1;
    if (in.pressed && m_hot >= 0) {
      m_active = m_hot;
      m_start_frame = f;
      m_start_mouse = in.mouse;
      m_plane_point = pivot;
      Vec3 view = normalize(pivot - cam.pos);
      if (op == GizmoOp::Rotate) {
        m_plane_normal = f.col3(m_active);
      } else if (op == GizmoOp::Scale && m_active == 6) {
        m_plane_normal = -cam.forward();
      } else if (m_active < 3) {
        Vec3 a = f.col3(m_active);
        Vec3 n = cross(a, cross(view, a));
        m_plane_normal = length2(n) > 1e-8f ? normalize(n) : perpendicular(a);
      } else if (m_active < 6) {
        m_plane_normal = f.col3(m_active - 3);
      } else {
        m_plane_normal = -cam.forward();
      }
      Ray ray = cam.ray(in.mouse.x, in.mouse.y, in.viewport_size.x, in.viewport_size.y);
      if (!ray_on_drag_plane(ray, &m_start_hit)) {
        m_active = -1;
      } else {
        res.started = true;
      }
    }
  }

  if (m_active >= 0) {
    if (in.down) {
      Ray ray = cam.ray(in.mouse.x, in.mouse.y, in.viewport_size.x, in.viewport_size.y);
      Vec3 p;
      const Mat4& sf = m_start_frame;
      const Vec3 sp = sf.col3(3);
      res.dragging = true;
      if (ray_on_drag_plane(ray, &p)) {
        Vec3 d = p - m_start_hit;
        if (op == GizmoOp::Scale) {
          // along an axis: how far the handle went, in sizes of the gizmo; all axes: the mouse
          // going up (or right) grows, down shrinks
          float k = 1.f;
          if (m_active < 3) {
            k = 1.f + dot(d, sf.col3(m_active)) / size;
          } else {
            const Vec2 md = in.mouse - m_start_mouse;
            k = std::exp((md.x - md.y) * 0.01f);
          }
          if (snap.enabled && snap.scale > 0) k = std::round(k / snap.scale) * snap.scale;
          k = std::max(k, 0.01f);
          res.scale = m_active < 3 ? Vec3{m_active == 0 ? k : 1.f, m_active == 1 ? k : 1.f, m_active == 2 ? k : 1.f}
                                   : Vec3{k, k, k};
        } else if (op == GizmoOp::Translate) {
          Vec3 offset{0, 0, 0};
          if (m_active < 3) {
            Vec3 a = sf.col3(m_active);
            offset = a * snap_value(dot(d, a), snap.translate, snap.enabled);
          } else if (m_active < 6) {
            for (int i = 0; i < 3; i++) {
              if (i == m_active - 3) continue;
              Vec3 a = sf.col3(i);
              offset += a * snap_value(dot(d, a), snap.translate, snap.enabled);
            }
          } else {
            offset = {snap_value(d.x, snap.translate, snap.enabled),
                      snap_value(d.y, snap.translate, snap.enabled),
                      snap_value(d.z, snap.translate, snap.enabled)};
          }
          res.delta = Mat4::translate(offset);
        } else {
          Vec3 a = sf.col3(m_active);
          Vec3 v0 = normalize(m_start_hit - sp), v1 = normalize(p - sp);
          float ang = std::atan2(dot(cross(v0, v1), a), dot(v0, v1));
          ang = radians(snap_value(degrees(ang), snap.rotate_deg, snap.enabled));
          res.delta = Mat4::translate(sp) * Mat4::rotate(Quat::axis_angle(a, ang)) * Mat4::translate(-sp);
        }
      }
    }
    if (!in.down) {
      res.finished = true;
      res.dragging = false;
      m_active = -1;
    }
  }
  draw(r, in, f, size, dl);
  return res;
}

}  // namespace ogle
