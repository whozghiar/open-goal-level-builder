#include "render/renderer.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "core/log.h"
#include "glad/glad.h"

namespace ogle {

namespace {

// Meshes are drawn instanced: every node of a mesh is one instance (a decor prototype and its
// copies: one draw per primitive). The instances of the frame are in a texture buffer, 5 texels
// each (world matrix columns, selection tint); a draw reads them from u_instance_base on.
const char* kMeshVs = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_nrm;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in vec4 a_col;
uniform mat4 u_viewproj;
uniform samplerBuffer u_instances;
uniform int u_instance_base;
out vec3 v_nrm;
out vec2 v_uv;
out vec4 v_col;
out vec4 v_tint;
void main() {
  int base = (u_instance_base + gl_InstanceID) * 5;
  mat4 model = mat4(texelFetch(u_instances, base), texelFetch(u_instances, base + 1),
                    texelFetch(u_instances, base + 2), texelFetch(u_instances, base + 3));
  vec4 a_tint = texelFetch(u_instances, base + 4);
  // the normals of a stretched element: each axis divided by its scale squared (rotation * 1/scale)
  mat3 nm = mat3(model);
  for (int i = 0; i < 3; i++) nm[i] /= max(dot(nm[i], nm[i]), 1e-8);
  v_nrm = nm * a_nrm;
  v_uv = a_uv;
  v_col = a_col;
  v_tint = a_tint;
  gl_Position = u_viewproj * (model * vec4(a_pos, 1.0));
}
)";

const char* kMeshFs = R"(#version 330 core
in vec3 v_nrm;
in vec2 v_uv;
in vec4 v_col;
in vec4 v_tint;
uniform sampler2D u_tex;
uniform int u_has_tex;
uniform vec4 u_base;
uniform int u_alpha_mode;
uniform float u_cutoff;
uniform int u_use_vcol;
uniform float u_vcol_scale;
uniform int u_shade;
out vec4 o_color;
// lit colors above 0.8 rolled off towards 1 instead of cut: a bright texture under a bright light
// (the PS2 doubles vertex colors) keeps its detail instead of turning white
vec3 shoulder(vec3 x) {
  vec3 over = max(x - 0.8, 0.0);
  return min(x, vec3(0.8)) + 0.2 * (1.0 - exp(-over / 0.2));
}
void main() {
  vec4 c = u_base;
  if (u_has_tex == 1) c *= texture(u_tex, v_uv);
  if (u_use_vcol == 1) c.rgb = shoulder(c.rgb * clamp(v_col.rgb * u_vcol_scale, 0.0, 4.0));
  // alpha test, like the PS2: masked and blended materials (most of a level's) drop the texels
  // under their reference and write depth for the others
  if (u_alpha_mode != 0 && c.a < u_cutoff) discard;
  if (u_shade == 1) {
    vec3 n = normalize(v_nrm);
    if (!gl_FrontFacing) n = -n;
    float l = 0.45 + 0.55 * max(dot(n, normalize(vec3(0.35, 1.0, 0.25))), 0.0);
    c.rgb *= l;
  }
  c.rgb = mix(c.rgb, v_tint.rgb, v_tint.a);
  o_color = c;
}
)";

const char* kFlatVs = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_col;
uniform mat4 u_mvp;
out vec4 v_col;
void main() {
  v_col = a_col;
  gl_Position = u_mvp * vec4(a_pos, 1.0);
}
)";

const char* kFlatFs = R"(#version 330 core
in vec4 v_col;
uniform float u_alpha;
uniform vec4 u_override;
out vec4 o_color;
void main() {
  vec4 c = v_col;
  c.a *= u_alpha;
  c = mix(c, vec4(u_override.rgb, c.a), u_override.a);
  o_color = c;
}
)";

const char* kSolidVs = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
uniform mat4 u_mvp;
void main() { gl_Position = u_mvp * vec4(a_pos, 1.0); }
)";

const char* kSolidFs = R"(#version 330 core
uniform vec4 u_color;
out vec4 o_color;
void main() { o_color = u_color; }
)";

unsigned compile(GLenum type, const char* src, std::string* error) {
  unsigned s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  int ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetShaderInfoLog(s, sizeof(log), nullptr, log);
    if (error) *error = log;
    glDeleteShader(s);
    return 0;
  }
  return s;
}

unsigned link(const char* vs, const char* fs, std::string* error) {
  unsigned v = compile(GL_VERTEX_SHADER, vs, error);
  if (!v) return 0;
  unsigned f = compile(GL_FRAGMENT_SHADER, fs, error);
  if (!f) return 0;
  unsigned p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, f);
  glLinkProgram(p);
  glDeleteShader(v);
  glDeleteShader(f);
  int ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetProgramInfoLog(p, sizeof(log), nullptr, log);
    if (error) *error = log;
    return 0;
  }
  return p;
}

int loc(unsigned prog, const char* name) { return glGetUniformLocation(prog, name); }

struct GpuVertex {
  float pos[3];
  float nrm[3];
  float uv[2];
  float col[4];
};

uint32_t hash_color(const std::string& s, float alpha = 1.f) {
  uint32_t h = 2166136261u;
  for (char c : s) h = (h ^ (uint8_t)c) * 16777619u;
  float r = 0.45f + ((h >> 0) & 0xff) / 255.f * 0.5f;
  float g = 0.45f + ((h >> 8) & 0xff) / 255.f * 0.5f;
  float b = 0.45f + ((h >> 16) & 0xff) / 255.f * 0.5f;
  return pack_rgba(r, g, b, alpha);
}

constexpr uint32_t kSelected = 0xff1e9bff;   // orange (ABGR packed as RGBA little endian)
constexpr uint32_t kHovered = 0xffffe08a;

}  // namespace

bool Renderer::init(std::string* error) {
  m_mesh_prog = link(kMeshVs, kMeshFs, error);
  m_flat_prog = link(kFlatVs, kFlatFs, error);
  m_solid_prog = link(kSolidVs, kSolidFs, error);
  if (!m_mesh_prog || !m_flat_prog || !m_solid_prog) return false;
  // uniform locations, looked up once (a lookup is a string search in the driver)
  m_u.flat_alpha = loc(m_flat_prog, "u_alpha");
  m_u.flat_mvp = loc(m_flat_prog, "u_mvp");
  m_u.flat_override = loc(m_flat_prog, "u_override");
  m_u.mesh_alpha_mode = loc(m_mesh_prog, "u_alpha_mode");
  m_u.mesh_instances = loc(m_mesh_prog, "u_instances");
  m_u.mesh_instance_base = loc(m_mesh_prog, "u_instance_base");
  m_u.mesh_base = loc(m_mesh_prog, "u_base");
  m_u.mesh_cutoff = loc(m_mesh_prog, "u_cutoff");
  m_u.mesh_has_tex = loc(m_mesh_prog, "u_has_tex");
  m_u.mesh_shade = loc(m_mesh_prog, "u_shade");
  m_u.mesh_tex = loc(m_mesh_prog, "u_tex");
  m_u.mesh_use_vcol = loc(m_mesh_prog, "u_use_vcol");
  m_u.mesh_vcol_scale = loc(m_mesh_prog, "u_vcol_scale");
  m_u.mesh_viewproj = loc(m_mesh_prog, "u_viewproj");
  m_u.solid_color = loc(m_solid_prog, "u_color");
  m_u.solid_mvp = loc(m_solid_prog, "u_mvp");

  glGenBuffers(1, &m_inst_vbo);
  glGenTextures(1, &m_inst_tex);
  glGenVertexArrays(1, &m_imm_vao);
  glGenBuffers(1, &m_imm_vbo);
  glBindVertexArray(m_imm_vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_imm_vbo);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(LineVertex), (void*)0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(LineVertex), (void*)12);
  glBindVertexArray(0);
  uint32_t white = 0xffffffff;
  glGenTextures(1, &m_white);
  glBindTexture(GL_TEXTURE_2D, m_white);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  return true;
}

void Renderer::free_mesh(GpuMesh& m) {
  if (m.vao) glDeleteVertexArrays(1, &m.vao);
  if (m.vbo) glDeleteBuffers(1, &m.vbo);
  if (m.ebo) glDeleteBuffers(1, &m.ebo);
  m.vao = m.vbo = m.ebo = 0;
  m.prims.clear();
}

void Renderer::reset() {
  for (auto& [uid, m] : m_meshes) free_mesh(m);
  m_meshes.clear();
  for (auto& t : m_textures)
    if (t.id) glDeleteTextures(1, &t.id);
  m_textures.clear();
}

void Renderer::shutdown() {
  reset();
  glDeleteProgram(m_mesh_prog);
  glDeleteProgram(m_flat_prog);
  glDeleteProgram(m_solid_prog);
  glDeleteVertexArrays(1, &m_imm_vao);
  glDeleteBuffers(1, &m_imm_vbo);
  glDeleteBuffers(1, &m_inst_vbo);
  glDeleteTextures(1, &m_inst_tex);
  glDeleteTextures(1, &m_white);
}

unsigned Renderer::gl_texture(const Scene& scene, int ti) { return gl_texture(scene.textures, ti); }

unsigned Renderer::gl_texture(const std::vector<Texture>& textures, int ti) {
  if (ti < 0 || ti >= (int)textures.size()) return m_white;
  if ((int)m_textures.size() < (int)textures.size()) m_textures.resize(textures.size());
  auto& gt = m_textures[ti];
  const auto& t = textures[ti];
  if (gt.id && gt.version == t.version) return gt.id;
  if (!gt.id) glGenTextures(1, &gt.id);
  glBindTexture(GL_TEXTURE_2D, gt.id);
  m_bound_texture = gt.id;
  if (t.width > 0 && t.height > 0 && t.rgba.size() >= (size_t)t.width * t.height * 4) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.width, t.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 t.rgba.data());
    glGenerateMipmap(GL_TEXTURE_2D);
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  gt.version = t.version;
  return gt.id;
}

Renderer::GpuMesh& Renderer::gpu_mesh(Mesh& mesh) {
  auto& gm = m_meshes[mesh.uid];
  gm.last_used = m_frame;
  if (gm.version == mesh.version && gm.prims.size() == mesh.prims.size()) return gm;
  free_mesh(gm);
  gm.version = mesh.version;
  // one buffer for the whole mesh (one VAO bind per mesh when drawing), primitives are ranges
  std::vector<GpuVertex> verts;
  std::vector<uint32_t> indices;
  for (const auto& p : mesh.prims) {
    GpuPrim gp;
    gp.material = p.material;
    gp.has_colors = p.col.size() == p.pos.size();
    gp.base_vertex = (int)verts.size();
    gp.first_index = indices.size();
    gp.count = (int)p.idx.size();
    for (size_t i = 0; i < p.pos.size(); i++) {
      GpuVertex v;
      v.pos[0] = p.pos[i].x;
      v.pos[1] = p.pos[i].y;
      v.pos[2] = p.pos[i].z;
      Vec3 n = i < p.nrm.size() ? p.nrm[i] : Vec3{0, 1, 0};
      v.nrm[0] = n.x;
      v.nrm[1] = n.y;
      v.nrm[2] = n.z;
      Vec2 uv = i < p.uv.size() ? p.uv[i] : Vec2{0, 0};
      v.uv[0] = uv.x;
      v.uv[1] = uv.y;
      Vec4 c = gp.has_colors ? p.col[i] : Vec4{1, 1, 1, 1};
      v.col[0] = c.x;
      v.col[1] = c.y;
      v.col[2] = c.z;
      v.col[3] = c.w;
      verts.push_back(v);
    }
    indices.insert(indices.end(), p.idx.begin(), p.idx.end());
    gm.prims.push_back(gp);
  }
  glGenVertexArrays(1, &gm.vao);
  glGenBuffers(1, &gm.vbo);
  glGenBuffers(1, &gm.ebo);
  glBindVertexArray(gm.vao);
  glBindBuffer(GL_ARRAY_BUFFER, gm.vbo);
  glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(GpuVertex), verts.data(), GL_STATIC_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gm.ebo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * 4, indices.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex), (void*)offsetof(GpuVertex, pos));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex), (void*)offsetof(GpuVertex, nrm));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(GpuVertex), (void*)offsetof(GpuVertex, uv));
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(GpuVertex), (void*)offsetof(GpuVertex, col));
  glBindVertexArray(0);
  return gm;
}

void Renderer::upload_instances() {
  glBindBuffer(GL_TEXTURE_BUFFER, m_inst_vbo);
  glBufferData(GL_TEXTURE_BUFFER, m_instances.size() * sizeof(Instance), m_instances.data(), GL_STREAM_DRAW);
  glBindBuffer(GL_TEXTURE_BUFFER, 0);
  // the mesh shader reads them from texture unit 1
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_BUFFER, m_inst_tex);
  glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, m_inst_vbo);
  glActiveTexture(GL_TEXTURE0);
  m_bound_texture = ~0u;
}

void Renderer::bind_instances(size_t first) { glUniform1i(m_u.mesh_instance_base, (int)first); }

Renderer::Instance Renderer::make_instance(const Mat4& world, const Vec4& tint) {
  Instance in;
  std::memcpy(in.m, world.m, sizeof(in.m));
  in.tint[0] = tint.x;
  in.tint[1] = tint.y;
  in.tint[2] = tint.z;
  in.tint[3] = tint.w;
  return in;
}

void Renderer::set_material(const Material* mat, const std::vector<Texture>& textures, bool has_colors,
                            const RenderSettings& rs, float alpha) {
  // the state of the previous draw is kept: many primitives in a row share a material
  bool has_tex = rs.textures && mat && mat->texture >= 0;
  unsigned tex = has_tex ? gl_texture(textures, mat->texture) : m_white;
  if (tex != m_bound_texture) {
    glBindTexture(GL_TEXTURE_2D, tex);
    m_bound_texture = tex;
  }
  Vec4 base = mat ? mat->base_color : Vec4{0.8f, 0.8f, 0.8f, 1.f};
  bool vcol = rs.vertex_colors && has_colors;
  MaterialState st{has_tex ? 1 : 0, {base.x, base.y, base.z, base.w * alpha}, mat ? (int)mat->alpha : 0,
                   mat ? mat->cutoff : 0.5f, vcol ? 1 : 0, (!vcol && rs.shade_uncolored) ? 1 : 0};
  if (m_material_valid && std::memcmp(&st, &m_material, sizeof(st)) == 0) return;
  glUniform1i(m_u.mesh_has_tex, st.has_tex);
  glUniform4f(m_u.mesh_base, st.base[0], st.base[1], st.base[2], st.base[3]);
  glUniform1i(m_u.mesh_alpha_mode, st.alpha_mode);
  glUniform1f(m_u.mesh_cutoff, st.cutoff);
  glUniform1i(m_u.mesh_use_vcol, st.use_vcol);
  glUniform1i(m_u.mesh_shade, st.shade);
  m_material = st;
  m_material_valid = true;
}

void Renderer::set_view(const Camera& cam, int x, int y, int w, int h, float logical_w, float logical_h) {
  m_vp[0] = x;
  m_vp[1] = y;
  m_vp[2] = std::max(w, 1);
  m_vp[3] = std::max(h, 1);
  m_logical[0] = logical_w > 0 ? logical_w : (float)m_vp[2];
  m_logical[1] = logical_h > 0 ? logical_h : (float)m_vp[3];
  m_view = cam.view();
  m_proj = cam.proj((float)m_vp[2] / (float)m_vp[3]);
  m_viewproj = m_proj * m_view;
  m_cam_pos = cam.pos;
}

void Renderer::begin(const Camera& cam, int x, int y, int w, int h, const RenderSettings& rs,
                     float logical_w, float logical_h) {
  m_frame++;
  stats = {};
  set_view(cam, x, y, w, h, logical_w, logical_h);
  glViewport(x, y, m_vp[2], m_vp[3]);
  glEnable(GL_SCISSOR_TEST);
  glScissor(x, y, m_vp[2], m_vp[3]);
  glClearColor(rs.clear_color[0], rs.clear_color[1], rs.clear_color[2], 1.f);
  glClearDepth(1.0);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glDisable(GL_SCISSOR_TEST);
  // frustum planes (Gribb-Hartmann), in world space
  const Mat4& m = m_viewproj;
  auto row = [&](int r) { return Vec4{m.at(r, 0), m.at(r, 1), m.at(r, 2), m.at(r, 3)}; };
  Vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
  auto add = [](Vec4 a, Vec4 b, float s) {
    return Vec4{a.x + b.x * s, a.y + b.y * s, a.z + b.z * s, a.w + b.w * s};
  };
  m_planes[0] = add(r3, r0, 1);
  m_planes[1] = add(r3, r0, -1);
  m_planes[2] = add(r3, r1, 1);
  m_planes[3] = add(r3, r1, -1);
  m_planes[4] = add(r3, r2, 1);
  m_planes[5] = add(r3, r2, -1);
  if (rs.show_grid) grid(cam);
}

bool Renderer::visible(const AABB& b) const {
  if (!b.valid()) return false;
  for (const auto& p : m_planes) {
    Vec3 v{p.x >= 0 ? b.hi.x : b.lo.x, p.y >= 0 ? b.hi.y : b.lo.y, p.z >= 0 ? b.hi.z : b.lo.z};
    if (p.x * v.x + p.y * v.y + p.z * v.z + p.w < 0) return false;
  }
  return true;
}

bool Renderer::project(const Vec3& world, Vec2* out) const {
  Vec4 c = m_viewproj * Vec4(world, 1.f);
  if (c.w <= 1e-5f) return false;
  float x = c.x / c.w, y = c.y / c.w;
  out->x = (x * 0.5f + 0.5f) * m_logical[0];
  out->y = (1.f - (y * 0.5f + 0.5f)) * m_logical[1];
  return true;
}

void Renderer::grid(const Camera& cam) {
  float h = std::max(std::fabs(cam.pos.y), 1.f);
  float step = std::pow(10.f, std::floor(std::log10(h)));
  step = std::max(step, 1.f);
  const int n = 60;
  float cx = std::floor(cam.pos.x / step) * step;
  float cz = std::floor(cam.pos.z / step) * step;
  for (int i = -n; i <= n; i++) {
    float x = cx + i * step, z = cz + i * step;
    bool major_x = std::fmod(std::fabs(x), step * 10.f) < 0.5f * step;
    bool major_z = std::fmod(std::fabs(z), step * 10.f) < 0.5f * step;
    uint32_t cx_col = std::fabs(x) < 0.5f * step ? pack_rgba(0.3f, 0.35f, 0.9f, 0.9f)
                      : major_x                  ? pack_rgba(1, 1, 1, 0.16f)
                                                 : pack_rgba(1, 1, 1, 0.06f);
    uint32_t cz_col = std::fabs(z) < 0.5f * step ? pack_rgba(0.9f, 0.3f, 0.3f, 0.9f)
                      : major_z                  ? pack_rgba(1, 1, 1, 0.16f)
                                                 : pack_rgba(1, 1, 1, 0.06f);
    line({x, 0, cz - n * step}, {x, 0, cz + n * step}, cx_col);
    line({cx - n * step, 0, z}, {cx + n * step, 0, z}, cz_col);
  }
  flush(true);
}

void Renderer::line(const Vec3& a, const Vec3& b, uint32_t color) {
  m_lines.push_back({a, color});
  m_lines.push_back({b, color});
}

void Renderer::tri(const Vec3& a, const Vec3& b, const Vec3& c, uint32_t color) {
  m_tris.push_back({a, color});
  m_tris.push_back({b, color});
  m_tris.push_back({c, color});
}

void Renderer::box_lines(const Mat4& m, const AABB& b, uint32_t color) {
  Vec3 c[8];
  for (int i = 0; i < 8; i++) {
    c[i] = m.point({(i & 1) ? b.hi.x : b.lo.x, (i & 2) ? b.hi.y : b.lo.y, (i & 4) ? b.hi.z : b.lo.z});
  }
  const int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                        {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  for (auto& k : e) line(c[k[0]], c[k[1]], color);
}

void Renderer::circle(const Vec3& center, const Vec3& u, const Vec3& v, float r, uint32_t color,
                      int segments) {
  Vec3 prev = center + u * r;
  for (int i = 1; i <= segments; i++) {
    float a = (float)i / segments * 2.f * kPi;
    Vec3 p = center + (u * std::cos(a) + v * std::sin(a)) * r;
    line(prev, p, color);
    prev = p;
  }
}

void Renderer::flush(bool depth_test, bool depth_offset) {
  if (m_lines.empty() && m_tris.empty()) return;
  glUseProgram(m_flat_prog);
  glUniformMatrix4fv(m_u.flat_mvp, 1, GL_FALSE, m_viewproj.m);
  glUniform1f(m_u.flat_alpha, 1.f);
  glUniform4f(m_u.flat_override, 0, 0, 0, 0);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  if (depth_test) glEnable(GL_DEPTH_TEST);
  else glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  if (depth_offset) {
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.f, -2.f);
  }
  glBindVertexArray(m_imm_vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_imm_vbo);
  if (!m_tris.empty()) {
    glBufferData(GL_ARRAY_BUFFER, m_tris.size() * sizeof(LineVertex), m_tris.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (int)m_tris.size());
  }
  if (!m_lines.empty()) {
    glBufferData(GL_ARRAY_BUFFER, m_lines.size() * sizeof(LineVertex), m_lines.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_LINES, 0, (int)m_lines.size());
  }
  glBindVertexArray(0);
  glDisable(GL_POLYGON_OFFSET_FILL);
  glDepthMask(GL_TRUE);
  glEnable(GL_DEPTH_TEST);
  m_lines.clear();
  m_tris.clear();
}

void Renderer::draw_scene(Scene& scene, const RenderSettings& rs,
                          const std::set<uint32_t>& selection, uint32_t hovered) {
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDisable(GL_CULL_FACE);

  // world matrices and visibility of the frame
  const SceneCache& cache = scene.cache();
  const std::vector<Mat4>& worlds = cache.worlds;
  const std::vector<uint8_t>& vis = cache.visible;

  // render meshes, grouped by mesh: the nodes of a mesh (a decor prototype and its instances,
  // copies of a model) are drawn by one instanced call per primitive
  struct Bucket {
    Mesh* mesh;
    size_t first = 0;
    std::vector<size_t> nodes;
  };
  using clock = std::chrono::steady_clock;
  auto ms_since = [](clock::time_point t) {
    return std::chrono::duration<double, std::milli>(clock::now() - t).count();
  };
  auto t0 = clock::now();
  std::vector<Bucket> buckets;
  buckets.reserve(cache.mesh_groups.size());
  for (const auto& [mesh, members] : cache.mesh_groups) {
    Bucket b{mesh};
    for (uint32_t i : members) {
      if (!vis[i]) continue;
      if (!rs.show_actors && cache.actor[i]) continue;
      if (rs.frustum_culling && !visible(cache.bounds[i])) {
        stats.nodes_culled++;
        continue;
      }
      b.nodes.push_back(i);
    }
    if (!b.nodes.empty()) buckets.push_back(std::move(b));
  }
  stats.ms_cull = ms_since(t0);
  t0 = clock::now();
  m_instances.clear();
  size_t total = 0;
  for (const auto& b : buckets) total += b.nodes.size();
  m_instances.reserve(total);
  const bool any_tint = !selection.empty() || hovered;
  for (auto& b : buckets) {
    b.first = m_instances.size();
    for (auto i : b.nodes) {
      Vec4 tint{0, 0, 0, 0};
      if (any_tint) {
        uint32_t id = scene.nodes[i].id;
        if (selection.count(id)) tint = Vec4{1.f, 0.6f, 0.12f, 0.18f};
        else if (id == hovered) tint = Vec4{1.f, 1.f, 0.8f, 0.12f};
      }
      m_instances.push_back(make_instance(worlds[i], tint));
    }
  }
  upload_instances();
  stats.ms_instances = ms_since(t0);
  t0 = clock::now();

  glUseProgram(m_mesh_prog);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(m_u.mesh_tex, 0);
  glUniform1i(m_u.mesh_instances, 1);
  m_material_valid = false;
  m_bound_vao = ~0u;
  glUniform1f(m_u.mesh_vcol_scale, rs.vertex_color_scale);
  glUniformMatrix4fv(m_u.mesh_viewproj, 1, GL_FALSE, m_viewproj.m);
  if (rs.wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
  glDepthMask(GL_TRUE);
  // opaque and masked primitives, then blended ones (with depth writes too: the PS2 draws most
  // blended level geometry that way, relying on the alpha test for the holes), then the additive
  // and subtractive ones (glows, light beams) without depth writes
  for (int pass = 0; pass < 3; pass++) {
    if (pass == 0) {
      glDisable(GL_BLEND);
    } else {
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      if (pass == 2) glDepthMask(GL_FALSE);
    }
    for (const auto& b : buckets) {
      auto& gm = gpu_mesh(*b.mesh);
      bool based = false;
      for (const auto& gp : gm.prims) {
        const Material* mat = gp.material >= 0 && gp.material < (int)scene.materials.size()
                                  ? &scene.materials[gp.material]
                                  : nullptr;
        if (material_pass(mat) != pass) continue;
        if (!based) {
          bind_instances(b.first);
          based = true;
        }
        if (pass == 2) set_blend(mat->blend);
        set_material(mat, scene.textures, gp.has_colors, rs);
        if (m_bound_vao != gm.vao) {
          glBindVertexArray(gm.vao);
          m_bound_vao = gm.vao;
        }
        glDrawElementsInstancedBaseVertex(GL_TRIANGLES, gp.count, GL_UNSIGNED_INT,
                                          (void*)(gp.first_index * sizeof(uint32_t)), (GLsizei)b.nodes.size(),
                                          gp.base_vertex);
        stats.draws++;
        stats.triangles += (size_t)(gp.count / 3) * b.nodes.size();
      }
    }
  }
  set_blend(BlendMode::Alpha);
  glDisable(GL_BLEND);
  glDepthMask(GL_TRUE);
  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  glBindVertexArray(0);
  m_bound_vao = 0;
  stats.ms_draw = ms_since(t0);
  t0 = clock::now();

  // selection outline of render meshes
  glUseProgram(m_solid_prog);
  glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
  glEnable(GL_POLYGON_OFFSET_LINE);
  glPolygonOffset(-1.f, -1.f);
  glEnable(GL_BLEND);
  for (uint32_t id : selection) {
    int i = scene.index_of(id);
    if (i < 0 || i >= (int)cache.count) continue;
    auto& n = scene.nodes[i];
    if (n.kind != NodeKind::Render || !n.mesh || !vis[i]) continue;
    auto& gm = gpu_mesh(*n.mesh);
    Mat4 mvp = m_viewproj * worlds[i];
    glUniformMatrix4fv(m_u.solid_mvp, 1, GL_FALSE, mvp.m);
    glUniform4f(m_u.solid_color, 1.f, 0.62f, 0.15f, 0.55f);
    glBindVertexArray(gm.vao);
    for (const auto& gp : gm.prims) {
      glDrawElementsBaseVertex(GL_TRIANGLES, gp.count, GL_UNSIGNED_INT, (void*)(gp.first_index * sizeof(uint32_t)),
                               gp.base_vertex);
    }
  }
  glBindVertexArray(0);
  glDisable(GL_POLYGON_OFFSET_LINE);
  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

  markers(scene, rs, selection, hovered, worlds);
  stats.ms_overlays = ms_since(t0);

  // free GPU meshes unused for a while (deleted nodes, old versions)
  if (m_frame % 300 == 0) {
    for (auto it = m_meshes.begin(); it != m_meshes.end();) {
      if (m_frame - it->second.last_used > 600) {
        free_mesh(it->second);
        it = m_meshes.erase(it);
      } else {
        ++it;
      }
    }
  }
}

void Renderer::draw_mesh(Mesh& mesh, const Mat4& world, const std::vector<Material>& materials,
                         const std::vector<Texture>& textures, const RenderSettings& rs,
                         const Vec4& tint, float alpha) {
  auto& gm = gpu_mesh(mesh);
  m_instances.assign(1, make_instance(world, tint));
  upload_instances();
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDisable(GL_CULL_FACE);
  glUseProgram(m_mesh_prog);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(m_u.mesh_tex, 0);
  glUniform1i(m_u.mesh_instances, 1);
  m_material_valid = false;
  m_bound_vao = ~0u;
  glUniform1f(m_u.mesh_vcol_scale, rs.vertex_color_scale);
  glUniformMatrix4fv(m_u.mesh_viewproj, 1, GL_FALSE, m_viewproj.m);
  // a see-through preview does not hide what is behind it
  const bool ghost = alpha < 1.f;
  bind_instances(0);
  for (int pass = 0; pass < 3; pass++) {
    for (const auto& gp : gm.prims) {
      const Material* mat = gp.material >= 0 && gp.material < (int)materials.size() ? &materials[gp.material] : nullptr;
      const int mat_pass = ghost ? std::max(material_pass(mat), 1) : material_pass(mat);
      if (mat_pass != pass) continue;
      if (pass > 0) {
        glEnable(GL_BLEND);
        set_blend(pass == 2 ? mat->blend : BlendMode::Alpha);
      } else {
        glDisable(GL_BLEND);
      }
      glDepthMask(ghost || pass == 2 ? GL_FALSE : GL_TRUE);
      set_material(mat, textures, gp.has_colors, rs, alpha);
      glBindVertexArray(gm.vao);
      glDrawElementsInstancedBaseVertex(GL_TRIANGLES, gp.count, GL_UNSIGNED_INT,
                                        (void*)(gp.first_index * sizeof(uint32_t)), 1, gp.base_vertex);
      stats.draws++;
      stats.triangles += gp.count / 3;
    }
  }
  glBindVertexArray(0);
  set_blend(BlendMode::Alpha);
  glDisable(GL_BLEND);
  glDepthMask(GL_TRUE);
}

int Renderer::material_pass(const Material* mat) {
  if (!mat || mat->alpha != AlphaMode::Blend) return 0;
  return mat->blend == BlendMode::Alpha ? 1 : 2;
}

void Renderer::set_blend(BlendMode b) {
  switch (b) {
    case BlendMode::Alpha:
      glBlendEquation(GL_FUNC_ADD);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      break;
    case BlendMode::Add:
      glBlendEquation(GL_FUNC_ADD);
      glBlendFunc(GL_ONE, GL_ONE);
      break;
    case BlendMode::AddAlpha:
      glBlendEquation(GL_FUNC_ADD);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE);
      break;
    case BlendMode::Subtract:
      glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE);
      break;
    case BlendMode::Half:
      glBlendEquation(GL_FUNC_ADD);
      glBlendColor(0.f, 0.f, 0.f, 0.5f);
      glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
      break;
  }
}

void Renderer::markers(Scene& scene, const RenderSettings& rs, const std::set<uint32_t>& selection,
                       uint32_t hovered, const std::vector<Mat4>& worlds) {
  const SceneCache& cache = scene.cache();
  // an actor has a marker when the library has no model for it
  std::vector<uint8_t> has_model(scene.nodes.size(), 0);
  for (size_t i = 0; i < scene.nodes.size(); i++) {
    if (!cache.actor[i]) continue;
    int a = scene.index_of(cache.actor[i]);
    if (a >= 0) has_model[a] = 1;
  }
  for (size_t i = 0; i < scene.nodes.size(); i++) {
    auto& n = scene.nodes[i];
    if (n.kind != NodeKind::Actor || !cache.visible[i] || has_model[i]) continue;
    const bool sel = selection.count(n.id) != 0;
    if (!rs.show_actors && !sel) continue;
    const Mat4& w = worlds[i];
    const Vec3 c = w.col3(3);
    if (!sel && length2(c - m_cam_pos) > 300.f * 300.f) continue;
    AABB box;
    box.add(c - Vec3{kMarkerRadius, kMarkerRadius, kMarkerRadius});
    box.add(c + Vec3{kMarkerRadius, kMarkerRadius, kMarkerRadius});
    if (!visible(box)) continue;
    uint32_t col = sel ? kSelected : n.id == hovered ? kHovered : hash_color(n.extras.value("etype", std::string()));
    float r = kMarkerRadius;
    Vec3 px = c + Vec3{r, 0, 0}, nx = c - Vec3{r, 0, 0};
    Vec3 py = c + Vec3{0, r, 0}, ny = c - Vec3{0, r, 0};
    Vec3 pz = c + Vec3{0, 0, r}, nz = c - Vec3{0, 0, r};
    for (auto* a : {&px, &nx})
      for (auto* b : {&pz, &nz}) line(*a, *b, col);
    for (auto* a : {&px, &nx, &pz, &nz}) {
      line(*a, py, col);
      line(*a, ny, col);
    }
    // facing: local +Z
    line(c, c + normalize(w.dir({0, 0, 1})) * (r * 2.2f), col);
  }
  flush(true, true);
}

}  // namespace ogle
