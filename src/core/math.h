#pragma once

// Small self-contained 3D math library: vectors, quaternions, column-major 4x4 matrices (OpenGL
// convention), TRS transforms, bounding boxes and ray tests. Units are meters, like glTF.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace ogle {

constexpr float kPi = 3.14159265358979323846f;
inline float radians(float deg) { return deg * kPi / 180.f; }
inline float degrees(float rad) { return rad * 180.f / kPi; }

struct Vec2 {
  float x = 0, y = 0;
  constexpr Vec2() = default;
  constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}
  Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
  Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
  Vec2 operator*(float s) const { return {x * s, y * s}; }
  float length() const { return std::sqrt(x * x + y * y); }
};

struct Vec3 {
  float x = 0, y = 0, z = 0;
  constexpr Vec3() = default;
  constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
  Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
  Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
  Vec3 operator-() const { return {-x, -y, -z}; }
  Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
  Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
  Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
  float& operator[](int i) { return (&x)[i]; }
  float operator[](int i) const { return (&x)[i]; }
  bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
};
inline Vec3 operator*(float s, const Vec3& v) { return v * s; }
inline Vec3 mul(const Vec3& a, const Vec3& b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length2(const Vec3& v) { return dot(v, v); }
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(const Vec3& v) {
  float l = length(v);
  return l > 1e-12f ? v / l : Vec3{0, 0, 0};
}
inline Vec3 vmin(const Vec3& a, const Vec3& b) {
  return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}
inline Vec3 vmax(const Vec3& a, const Vec3& b) {
  return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}
inline Vec3 lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }

struct Vec4 {
  float x = 0, y = 0, z = 0, w = 0;
  constexpr Vec4() = default;
  constexpr Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
  Vec4(const Vec3& v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
  Vec3 xyz() const { return {x, y, z}; }
  float& operator[](int i) { return (&x)[i]; }
  float operator[](int i) const { return (&x)[i]; }
  bool operator==(const Vec4& o) const { return x == o.x && y == o.y && z == o.z && w == o.w; }
};

struct Mat4;

struct Quat {
  float x = 0, y = 0, z = 0, w = 1;
  constexpr Quat() = default;
  constexpr Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

  static Quat axis_angle(const Vec3& axis, float rad) {
    Vec3 a = normalize(axis);
    float s = std::sin(rad * 0.5f);
    return {a.x * s, a.y * s, a.z * s, std::cos(rad * 0.5f)};
  }
  Quat operator*(const Quat& b) const {
    return {w * b.x + x * b.w + y * b.z - z * b.y, w * b.y - x * b.z + y * b.w + z * b.x,
            w * b.z + x * b.y - y * b.x + z * b.w, w * b.w - x * b.x - y * b.y - z * b.z};
  }
  Quat conj() const { return {-x, -y, -z, w}; }
  Quat normalized() const {
    float l = std::sqrt(x * x + y * y + z * z + w * w);
    if (l < 1e-12f) return {};
    return {x / l, y / l, z / l, w / l};
  }
  Vec3 rotate(const Vec3& v) const {
    Vec3 u{x, y, z};
    Vec3 t = cross(u, v) * 2.f;
    return v + t * w + cross(u, t);
  }
  // Tait-Bryan angles in degrees, q = qz * qy * qx (X applied first).
  static Quat from_euler_deg(const Vec3& deg) {
    Quat qx = axis_angle({1, 0, 0}, radians(deg.x));
    Quat qy = axis_angle({0, 1, 0}, radians(deg.y));
    Quat qz = axis_angle({0, 0, 1}, radians(deg.z));
    return (qz * qy * qx).normalized();
  }
  Vec3 to_euler_deg() const {
    float sinr = 2.f * (w * x + y * z);
    float cosr = 1.f - 2.f * (x * x + y * y);
    float sinp = std::clamp(2.f * (w * y - z * x), -1.f, 1.f);
    float siny = 2.f * (w * z + x * y);
    float cosy = 1.f - 2.f * (y * y + z * z);
    return {degrees(std::atan2(sinr, cosr)), degrees(std::asin(sinp)),
            degrees(std::atan2(siny, cosy))};
  }
  static Quat from_matrix(const Mat4& m);  // rotation part, columns must be orthonormal
};

// Column-major: m[col * 4 + row].
struct Mat4 {
  float m[16];

  Mat4() { *this = identity(); }
  static Mat4 identity() {
    Mat4 r(0);
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.f;
    return r;
  }
  explicit Mat4(float fill) {
    for (auto& v : m) v = fill;
  }
  float& at(int row, int col) { return m[col * 4 + row]; }
  float at(int row, int col) const { return m[col * 4 + row]; }
  Vec3 col3(int c) const { return {m[c * 4 + 0], m[c * 4 + 1], m[c * 4 + 2]}; }
  void set_col3(int c, const Vec3& v) {
    m[c * 4 + 0] = v.x;
    m[c * 4 + 1] = v.y;
    m[c * 4 + 2] = v.z;
  }

  static Mat4 translate(const Vec3& t) {
    Mat4 r = identity();
    r.m[12] = t.x;
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
  }
  static Mat4 scale(const Vec3& s) {
    Mat4 r = identity();
    r.m[0] = s.x;
    r.m[5] = s.y;
    r.m[10] = s.z;
    return r;
  }
  static Mat4 rotate(const Quat& q) {
    Mat4 r = identity();
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    r.m[0] = 1 - 2 * (yy + zz);
    r.m[1] = 2 * (xy + wz);
    r.m[2] = 2 * (xz - wy);
    r.m[4] = 2 * (xy - wz);
    r.m[5] = 1 - 2 * (xx + zz);
    r.m[6] = 2 * (yz + wx);
    r.m[8] = 2 * (xz + wy);
    r.m[9] = 2 * (yz - wx);
    r.m[10] = 1 - 2 * (xx + yy);
    return r;
  }
  static Mat4 perspective(float fovy_rad, float aspect, float znear, float zfar) {
    Mat4 r(0);
    float f = 1.f / std::tan(fovy_rad * 0.5f);
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = (zfar + znear) / (znear - zfar);
    r.m[11] = -1.f;
    r.m[14] = 2.f * zfar * znear / (znear - zfar);
    return r;
  }
  static Mat4 look_at(const Vec3& eye, const Vec3& target, const Vec3& up) {
    Vec3 f = normalize(target - eye);
    Vec3 s = normalize(cross(f, up));
    Vec3 u = cross(s, f);
    Mat4 r = identity();
    r.m[0] = s.x;
    r.m[4] = s.y;
    r.m[8] = s.z;
    r.m[1] = u.x;
    r.m[5] = u.y;
    r.m[9] = u.z;
    r.m[2] = -f.x;
    r.m[6] = -f.y;
    r.m[10] = -f.z;
    r.m[12] = -dot(s, eye);
    r.m[13] = -dot(u, eye);
    r.m[14] = dot(f, eye);
    return r;
  }

  Mat4 operator*(const Mat4& b) const {
    Mat4 r(0);
    for (int c = 0; c < 4; c++) {
      for (int rr = 0; rr < 4; rr++) {
        float s = 0;
        for (int k = 0; k < 4; k++) s += at(rr, k) * b.at(k, c);
        r.at(rr, c) = s;
      }
    }
    return r;
  }
  Vec4 operator*(const Vec4& v) const {
    return {m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12] * v.w,
            m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13] * v.w,
            m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] * v.w,
            m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15] * v.w};
  }
  Vec3 point(const Vec3& p) const {
    return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
            m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
            m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
  }
  Vec3 dir(const Vec3& d) const {
    return {m[0] * d.x + m[4] * d.y + m[8] * d.z, m[1] * d.x + m[5] * d.y + m[9] * d.z,
            m[2] * d.x + m[6] * d.y + m[10] * d.z};
  }
  Mat4 transposed() const {
    Mat4 r(0);
    for (int c = 0; c < 4; c++)
      for (int rr = 0; rr < 4; rr++) r.at(rr, c) = at(c, rr);
    return r;
  }
  float det3() const {
    return at(0, 0) * (at(1, 1) * at(2, 2) - at(1, 2) * at(2, 1)) -
           at(0, 1) * (at(1, 0) * at(2, 2) - at(1, 2) * at(2, 0)) +
           at(0, 2) * (at(1, 0) * at(2, 1) - at(1, 1) * at(2, 0));
  }
  Mat4 inverse() const;
};

inline Mat4 Mat4::inverse() const {
  const float* a = m;
  float inv[16];
  inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] +
           a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
  inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] -
           a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
  inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] +
           a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
  inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] -
            a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
  inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] -
           a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
  inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] +
           a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
  inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] -
           a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
  inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] +
            a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
  inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] +
           a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
  inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] -
           a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
  inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] +
            a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
  inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] -
            a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
  inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] -
           a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
  inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] +
           a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
  inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] -
            a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
  inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] +
            a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
  float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
  Mat4 r(0);
  if (std::fabs(det) < 1e-20f) return Mat4::identity();
  det = 1.f / det;
  for (int i = 0; i < 16; i++) r.m[i] = inv[i] * det;
  return r;
}

inline Quat Quat::from_matrix(const Mat4& a) {
  float m00 = a.at(0, 0), m11 = a.at(1, 1), m22 = a.at(2, 2);
  float tr = m00 + m11 + m22;
  Quat q;
  if (tr > 0) {
    float s = std::sqrt(tr + 1.f) * 2.f;
    q.w = 0.25f * s;
    q.x = (a.at(2, 1) - a.at(1, 2)) / s;
    q.y = (a.at(0, 2) - a.at(2, 0)) / s;
    q.z = (a.at(1, 0) - a.at(0, 1)) / s;
  } else if (m00 > m11 && m00 > m22) {
    float s = std::sqrt(1.f + m00 - m11 - m22) * 2.f;
    q.w = (a.at(2, 1) - a.at(1, 2)) / s;
    q.x = 0.25f * s;
    q.y = (a.at(0, 1) + a.at(1, 0)) / s;
    q.z = (a.at(0, 2) + a.at(2, 0)) / s;
  } else if (m11 > m22) {
    float s = std::sqrt(1.f + m11 - m00 - m22) * 2.f;
    q.w = (a.at(0, 2) - a.at(2, 0)) / s;
    q.x = (a.at(0, 1) + a.at(1, 0)) / s;
    q.y = 0.25f * s;
    q.z = (a.at(1, 2) + a.at(2, 1)) / s;
  } else {
    float s = std::sqrt(1.f + m22 - m00 - m11) * 2.f;
    q.w = (a.at(1, 0) - a.at(0, 1)) / s;
    q.x = (a.at(0, 2) + a.at(2, 0)) / s;
    q.y = (a.at(1, 2) + a.at(2, 1)) / s;
    q.z = 0.25f * s;
  }
  return q.normalized();
}

// Translation, rotation, scale: matrix = T * R * S.
struct Transform {
  Vec3 t{0, 0, 0};
  Quat r{};
  Vec3 s{1, 1, 1};

  Mat4 matrix() const { return Mat4::translate(t) * Mat4::rotate(r) * Mat4::scale(s); }
  static Transform from_matrix(const Mat4& m) {
    Transform out;
    out.t = m.col3(3);
    Vec3 c0 = m.col3(0), c1 = m.col3(1), c2 = m.col3(2);
    out.s = {length(c0), length(c1), length(c2)};
    if (m.det3() < 0) out.s.x = -out.s.x;
    Mat4 rot = Mat4::identity();
    rot.set_col3(0, out.s.x != 0 ? c0 / out.s.x : Vec3{1, 0, 0});
    rot.set_col3(1, out.s.y != 0 ? c1 / out.s.y : Vec3{0, 1, 0});
    rot.set_col3(2, out.s.z != 0 ? c2 / out.s.z : Vec3{0, 0, 1});
    out.r = Quat::from_matrix(rot);
    return out;
  }
  bool operator==(const Transform& o) const {
    return t == o.t && r.x == o.r.x && r.y == o.r.y && r.z == o.r.z && r.w == o.r.w && s == o.s;
  }
};

struct AABB {
  Vec3 lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
          std::numeric_limits<float>::max()};
  Vec3 hi{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
          -std::numeric_limits<float>::max()};

  bool valid() const { return lo.x <= hi.x && lo.y <= hi.y && lo.z <= hi.z; }
  void add(const Vec3& p) {
    lo = vmin(lo, p);
    hi = vmax(hi, p);
  }
  void add(const AABB& b) {
    if (!b.valid()) return;
    lo = vmin(lo, b.lo);
    hi = vmax(hi, b.hi);
  }
  Vec3 center() const { return (lo + hi) * 0.5f; }
  Vec3 size() const { return hi - lo; }
  float radius() const { return length(hi - lo) * 0.5f; }
  AABB expanded(float e) const {
    AABB r = *this;
    r.lo -= Vec3{e, e, e};
    r.hi += Vec3{e, e, e};
    return r;
  }
  bool overlaps(const AABB& o) const {
    return lo.x <= o.hi.x && hi.x >= o.lo.x && lo.y <= o.hi.y && hi.y >= o.lo.y &&
           lo.z <= o.hi.z && hi.z >= o.lo.z;
  }
  bool contains(const Vec3& p) const {
    return p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y && p.z >= lo.z && p.z <= hi.z;
  }
  // the box around the transformed box (center and half extents: one point transform)
  AABB transformed(const Mat4& m) const {
    AABB r;
    if (!valid()) return r;
    Vec3 c = (lo + hi) * 0.5f, e = (hi - lo) * 0.5f;
    Vec3 nc = m.point(c);
    Vec3 ne{std::fabs(m.at(0, 0)) * e.x + std::fabs(m.at(0, 1)) * e.y + std::fabs(m.at(0, 2)) * e.z,
            std::fabs(m.at(1, 0)) * e.x + std::fabs(m.at(1, 1)) * e.y + std::fabs(m.at(1, 2)) * e.z,
            std::fabs(m.at(2, 0)) * e.x + std::fabs(m.at(2, 1)) * e.y + std::fabs(m.at(2, 2)) * e.z};
    r.lo = nc - ne;
    r.hi = nc + ne;
    return r;
  }
};

struct Ray {
  Vec3 o;
  Vec3 d;  // not necessarily normalized when transformed into a local space
  Vec3 at(float t) const { return o + d * t; }
};

inline Ray transform_ray(const Mat4& m, const Ray& r) { return {m.point(r.o), m.dir(r.d)}; }

// Slab test. Returns true when the ray hits the box within [0, tmax].
inline bool ray_aabb(const Ray& r, const AABB& b, float tmax, float* tnear_out = nullptr) {
  float t0 = 0.f, t1 = tmax;
  for (int a = 0; a < 3; a++) {
    float inv = 1.f / r.d[a];
    float tn = (b.lo[a] - r.o[a]) * inv;
    float tf = (b.hi[a] - r.o[a]) * inv;
    if (tn > tf) std::swap(tn, tf);
    t0 = tn > t0 ? tn : t0;
    t1 = tf < t1 ? tf : t1;
    if (t0 > t1) return false;
  }
  if (tnear_out) *tnear_out = t0;
  return true;
}

// Moller-Trumbore. Returns the distance along the ray, or -1 when there is no hit.
inline float ray_triangle(const Ray& r, const Vec3& a, const Vec3& b, const Vec3& c) {
  Vec3 e1 = b - a, e2 = c - a;
  Vec3 p = cross(r.d, e2);
  float det = dot(e1, p);
  if (std::fabs(det) < 1e-12f) return -1.f;
  float inv = 1.f / det;
  Vec3 s = r.o - a;
  float u = dot(s, p) * inv;
  if (u < 0.f || u > 1.f) return -1.f;
  Vec3 q = cross(s, e1);
  float v = dot(r.d, q) * inv;
  if (v < 0.f || u + v > 1.f) return -1.f;
  float t = dot(e2, q) * inv;
  return t > 0.f ? t : -1.f;
}

inline bool ray_plane(const Ray& r, const Vec3& p, const Vec3& n, float* t) {
  float den = dot(n, r.d);
  if (std::fabs(den) < 1e-9f) return false;
  *t = dot(p - r.o, n) / den;
  return true;
}

inline float ray_sphere(const Ray& r, const Vec3& c, float radius) {
  Vec3 oc = r.o - c;
  float a = dot(r.d, r.d);
  float b = dot(oc, r.d);
  float cc = dot(oc, oc) - radius * radius;
  float disc = b * b - a * cc;
  if (disc < 0) return -1.f;
  float s = std::sqrt(disc);
  float t = (-b - s) / a;
  if (t < 0) t = (-b + s) / a;
  return t >= 0 ? t : -1.f;
}

inline uint32_t pack_rgba(float r, float g, float b, float a = 1.f) {
  auto c = [](float v) { return (uint32_t)std::clamp((int)std::lround(v * 255.f), 0, 255); };
  return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);
}

}  // namespace ogle
