#include "io/gltf_io.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>

#include "core/log.h"
#include "stb_image_write.h"
#include "tiny_gltf.h"

namespace ogle {

namespace {

// ------------------------------------------------------------------------------------------------
// tinygltf helpers
// ------------------------------------------------------------------------------------------------

json to_json(const tinygltf::Value& v) {
  if (v.IsBool()) return v.Get<bool>();
  if (v.IsInt()) return v.Get<int>();
  if (v.IsReal()) return v.Get<double>();
  if (v.IsString()) return v.Get<std::string>();
  if (v.IsArray()) {
    json a = json::array();
    for (size_t i = 0; i < v.ArrayLen(); i++) a.push_back(to_json(v.Get((int)i)));
    return a;
  }
  if (v.IsObject()) {
    json o = json::object();
    for (const auto& k : v.Keys()) o[k] = to_json(v.Get(k));
    return o;
  }
  return nullptr;
}

struct AccessorView {
  const uint8_t* data = nullptr;
  size_t count = 0;
  int comps = 0;
  int ctype = 0;
  size_t stride = 0;
  bool normalized = false;
};

bool view_accessor(const tinygltf::Model& m, int idx, AccessorView* out) {
  if (idx < 0 || idx >= (int)m.accessors.size()) return false;
  const auto& a = m.accessors[idx];
  if (a.bufferView < 0 || a.bufferView >= (int)m.bufferViews.size()) return false;
  const auto& bv = m.bufferViews[a.bufferView];
  if (bv.buffer < 0 || bv.buffer >= (int)m.buffers.size()) return false;
  const auto& buf = m.buffers[bv.buffer];
  int comps = tinygltf::GetNumComponentsInType((uint32_t)a.type);
  int csize = tinygltf::GetComponentSizeInBytes((uint32_t)a.componentType);
  if (comps <= 0 || csize <= 0) return false;
  size_t stride = bv.byteStride ? bv.byteStride : (size_t)(comps * csize);
  size_t start = bv.byteOffset + a.byteOffset;
  if (a.count > 0 && start + stride * (a.count - 1) + comps * csize > buf.data.size()) return false;
  out->data = buf.data.data() + start;
  out->count = a.count;
  out->comps = comps;
  out->ctype = a.componentType;
  out->stride = stride;
  out->normalized = a.normalized;
  return true;
}

float read_component(const uint8_t* p, int ctype, bool normalized) {
  switch (ctype) {
    case TINYGLTF_COMPONENT_TYPE_FLOAT: {
      float f;
      std::memcpy(&f, p, 4);
      return f;
    }
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
      return normalized ? p[0] / 255.f : (float)p[0];
    case TINYGLTF_COMPONENT_TYPE_BYTE: {
      int8_t v = (int8_t)p[0];
      return normalized ? std::max(v / 127.f, -1.f) : (float)v;
    }
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
      uint16_t v;
      std::memcpy(&v, p, 2);
      return normalized ? v / 65535.f : (float)v;
    }
    case TINYGLTF_COMPONENT_TYPE_SHORT: {
      int16_t v;
      std::memcpy(&v, p, 2);
      return normalized ? std::max(v / 32767.f, -1.f) : (float)v;
    }
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
      uint32_t v;
      std::memcpy(&v, p, 4);
      return (float)v;
    }
    default:
      return 0.f;
  }
}

// Reads any accessor as vec4s; missing components get `fill` (w defaults to 1 for colors).
std::vector<Vec4> read_vec4(const tinygltf::Model& m, int idx, bool force_normalized, float fill_w) {
  std::vector<Vec4> out;
  AccessorView v;
  if (!view_accessor(m, idx, &v)) return out;
  int csize = tinygltf::GetComponentSizeInBytes((uint32_t)v.ctype);
  bool norm = v.normalized || (force_normalized && v.ctype != TINYGLTF_COMPONENT_TYPE_FLOAT);
  out.resize(v.count);
  for (size_t i = 0; i < v.count; i++) {
    const uint8_t* p = v.data + i * v.stride;
    Vec4 r{0, 0, 0, fill_w};
    for (int c = 0; c < std::min(v.comps, 4); c++) r[c] = read_component(p + c * csize, v.ctype, norm);
    out[i] = r;
  }
  return out;
}

std::vector<uint32_t> read_indices(const tinygltf::Model& m, int idx) {
  std::vector<uint32_t> out;
  AccessorView v;
  if (!view_accessor(m, idx, &v)) return out;
  out.resize(v.count);
  for (size_t i = 0; i < v.count; i++) {
    const uint8_t* p = v.data + i * v.stride;
    switch (v.ctype) {
      case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        out[i] = p[0];
        break;
      case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
        uint16_t s;
        std::memcpy(&s, p, 2);
        out[i] = s;
        break;
      }
      default: {
        uint32_t s;
        std::memcpy(&s, p, 4);
        out[i] = s;
        break;
      }
    }
  }
  return out;
}

std::vector<uint32_t> read_u32_view(const tinygltf::Model& m, int view_idx) {
  std::vector<uint32_t> out;
  if (view_idx < 0 || view_idx >= (int)m.bufferViews.size()) return out;
  const auto& bv = m.bufferViews[view_idx];
  if (bv.buffer < 0 || bv.buffer >= (int)m.buffers.size()) return out;
  const auto& buf = m.buffers[bv.buffer];
  if (bv.byteOffset + bv.byteLength > buf.data.size()) return out;
  out.resize(bv.byteLength / 4);
  std::memcpy(out.data(), buf.data.data() + bv.byteOffset, out.size() * 4);
  return out;
}

// Keys of the OpenGOAL level builder on collision nodes: a rip's collision is marked with them,
// and collide_pat is the exact surface of the node (ogle-extract, the decompiler's rips).
const char* kCollisionKeys[] = {"set_collision", "set_invisible", "ignore",       "collide_material",
                                "collide_event", "collide_mode",  "nolineofsight", "noedge",
                                "nocamera",      "noentity",      "collide_pat",  "og_game",
                                "collide_game",  "kind"};

int json_int(const json& e, const char* key, int dflt = 0) {
  if (!e.is_object() || !e.contains(key)) return dflt;
  const auto& v = e.at(key);
  if (v.is_number()) return (int)v.get<double>();
  if (v.is_boolean()) return v.get<bool>() ? 1 : 0;
  return dflt;
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// import
// ------------------------------------------------------------------------------------------------

namespace {

bool import_gltf_file(Scene& scene, const std::string& path, const ImportOptions& opts, ImportResult* result,
                      std::string* error) {
  tinygltf::TinyGLTF loader;
  tinygltf::Model model;
  std::string err, warn;
  bool ok;
  std::string lower = path;
  for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
  if (lower.size() > 5 && lower.substr(lower.size() - 5) == ".gltf") {
    ok = loader.LoadASCIIFromFile(&model, &err, &warn, path);
  } else {
    ok = loader.LoadBinaryFromFile(&model, &err, &warn, path);
  }
  if (!warn.empty()) LOG_WARN("%s", warn.c_str());
  if (!ok) {
    if (error) *error = err.empty() ? "unreadable glTF file" : err;
    return false;
  }
  ImportResult res;
  res.generator = model.asset.generator;
  json asset_extras = to_json(model.asset.extras);
  if (opts.read_settings && asset_extras.is_object() && asset_extras.contains("ogle")) {
    const auto& o = asset_extras["ogle"];
    if (o.contains("settings")) scene.settings = LevelSettings::from_json(o["settings"]);
  }
  // a decompiler rip says which game it comes from
  if (opts.game_from_file && asset_extras.is_object() && asset_extras.contains("opengoal") &&
      asset_extras["opengoal"].is_object() && asset_extras["opengoal"].contains("game")) {
    scene.settings.game = game_from_name(asset_extras["opengoal"]["game"].get<std::string>(), scene.settings.game);
  }
  res.asset_extras = asset_extras;

  // images -> textures
  std::vector<int> image_to_texture(model.images.size(), -1);
  for (size_t i = 0; i < model.images.size(); i++) {
    const auto& img = model.images[i];
    Texture t;
    t.name = img.name.empty() ? ("image-" + std::to_string(i)) : img.name;
    if (img.width > 0 && img.height > 0 && !img.image.empty() && img.bits == 8 &&
        img.component >= 1 && img.component <= 4) {
      t.width = img.width;
      t.height = img.height;
      t.rgba.resize((size_t)t.width * t.height * 4);
      for (size_t p = 0; p < (size_t)t.width * t.height; p++) {
        const unsigned char* src = img.image.data() + p * img.component;
        unsigned char* dst = t.rgba.data() + p * 4;
        if (img.component >= 3) {
          dst[0] = src[0];
          dst[1] = src[1];
          dst[2] = src[2];
        } else {
          dst[0] = dst[1] = dst[2] = src[0];
        }
        dst[3] = img.component == 4 ? src[3] : img.component == 2 ? src[1] : 255;
      }
    } else {
      // unsupported: 2x2 magenta checker, so the problem is visible
      t.width = t.height = 2;
      t.rgba = {255, 0, 255, 255, 40, 0, 40, 255, 40, 0, 40, 255, 255, 0, 255, 255};
      LOG_WARN("image %s: unsupported format, replaced by a checker", t.name.c_str());
    }
    image_to_texture[i] = scene.add_texture(std::move(t));
  }

  // materials
  std::vector<int> material_map(model.materials.size(), -1);
  for (size_t i = 0; i < model.materials.size(); i++) {
    const auto& gm = model.materials[i];
    Material m;
    m.name = gm.name.empty() ? ("material-" + std::to_string(i)) : gm.name;
    const auto& pbr = gm.pbrMetallicRoughness;
    if (pbr.baseColorFactor.size() == 4) {
      m.base_color = {(float)pbr.baseColorFactor[0], (float)pbr.baseColorFactor[1],
                      (float)pbr.baseColorFactor[2], (float)pbr.baseColorFactor[3]};
    }
    int ti = pbr.baseColorTexture.index;
    if (ti >= 0 && ti < (int)model.textures.size()) {
      int src = model.textures[ti].source;
      if (src >= 0 && src < (int)image_to_texture.size()) m.texture = image_to_texture[src];
    }
    m.alpha = gm.alphaMode == "MASK"    ? AlphaMode::Mask
              : gm.alphaMode == "BLEND" ? AlphaMode::Blend
                                        : AlphaMode::Opaque;
    m.cutoff = (float)gm.alphaCutoff;
    m.double_sided = gm.doubleSided;
    json mat_extras = to_json(gm.extras);
    if (mat_extras.is_object() && mat_extras.contains("og_blend") && mat_extras["og_blend"].is_string()) {
      m.blend = blend_mode_from_name(mat_extras["og_blend"].get<std::string>());
      if (m.blend != BlendMode::Alpha) m.alpha = AlphaMode::Blend;
    }
    material_map[i] = scene.add_material(std::move(m));
  }

  // accessors are decoded once: the decompiler's rips share one vertex array between all the
  // primitives of a tfrag/TIE tree
  std::map<std::tuple<int, bool, float>, std::shared_ptr<std::vector<Vec4>>> accessor_cache;
  auto cached_vec4 = [&](int acc, bool force_normalized, float fill_w) {
    auto key = std::make_tuple(acc, force_normalized, fill_w);
    auto it = accessor_cache.find(key);
    if (it == accessor_cache.end()) {
      it = accessor_cache
               .emplace(key, std::make_shared<std::vector<Vec4>>(read_vec4(model, acc, force_normalized, fill_w)))
               .first;
    }
    return it->second;
  };

  // meshes (shared between nodes: instancing is kept)
  std::vector<std::shared_ptr<Mesh>> mesh_cache(model.meshes.size());
  auto get_mesh = [&](int mi) -> std::shared_ptr<Mesh> {
    if (mi < 0 || mi >= (int)model.meshes.size()) return nullptr;
    if (mesh_cache[mi]) return mesh_cache[mi];
    const auto& gmesh = model.meshes[mi];
    auto mesh = std::make_shared<Mesh>();
    mesh->name = gmesh.name.empty() ? ("mesh-" + std::to_string(mi)) : gmesh.name;
    json mesh_extras = to_json(gmesh.extras);
    std::vector<int> pat_views;
    if (mesh_extras.is_object() && mesh_extras.contains("og_tri_pat_views")) {
      pat_views = mesh_extras["og_tri_pat_views"].get<std::vector<int>>();
      mesh_extras.erase("og_tri_pat_views");
    }
    if (mesh_extras.is_object()) mesh->extras = mesh_extras;
    for (size_t pi = 0; pi < gmesh.primitives.size(); pi++) {
      const auto& gp = gmesh.primitives[pi];
      int mode = gp.mode < 0 ? TINYGLTF_MODE_TRIANGLES : gp.mode;
      if (mode != TINYGLTF_MODE_TRIANGLES && mode != TINYGLTF_MODE_TRIANGLE_STRIP &&
          mode != TINYGLTF_MODE_TRIANGLE_FAN) {
        continue;
      }
      auto pos_it = gp.attributes.find("POSITION");
      if (pos_it == gp.attributes.end()) continue;
      Primitive p;
      const auto pos_all = cached_vec4(pos_it->second, false, 1.f);
      const size_t nv = pos_all->size();
      if (nv == 0) continue;
      // indices first: only the vertices this primitive uses are kept
      std::vector<uint32_t> raw;
      if (gp.indices >= 0) {
        raw = read_indices(model, gp.indices);
      } else {
        raw.resize(nv);
        for (size_t i = 0; i < nv; i++) raw[i] = (uint32_t)i;
      }
      bool bad = false;
      for (auto i : raw) bad |= i >= nv;
      if (bad) {
        LOG_WARN("mesh %s: indices out of range, primitive skipped", mesh->name.c_str());
        continue;
      }
      std::vector<uint32_t> used;
      if (raw.size() * 4 < nv) {
        // a small part of a large vertex array (the pieces of a level's shell share theirs)
        std::unordered_map<uint32_t, uint32_t> remap;
        remap.reserve(raw.size());
        for (auto& i : raw) {
          auto [it, fresh] = remap.emplace(i, (uint32_t)used.size());
          if (fresh) used.push_back(i);
          i = it->second;
        }
      } else {
        std::vector<uint32_t> remap(nv, UINT32_MAX);
        for (auto& i : raw) {
          if (remap[i] == UINT32_MAX) {
            remap[i] = (uint32_t)used.size();
            used.push_back(i);
          }
          i = remap[i];
        }
      }
      auto gather = [&](const std::vector<Vec4>& all, auto&& fn) {
        if (all.size() != nv) return false;
        for (auto v : used) fn(all[v]);
        return true;
      };
      gather(*pos_all, [&](const Vec4& v) { p.pos.push_back(v.xyz()); });
      for (const auto& [name, acc] : gp.attributes) {
        if (name == "POSITION") continue;
        if (name == "NORMAL") {
          gather(*cached_vec4(acc, false, 0.f), [&](const Vec4& v) { p.nrm.push_back(v.xyz()); });
        } else if (name == "TEXCOORD_0") {
          gather(*cached_vec4(acc, true, 0.f), [&](const Vec4& v) { p.uv.push_back({v.x, v.y}); });
        } else if (name == "COLOR_0") {
          gather(*cached_vec4(acc, true, 1.f), [&](const Vec4& v) { p.col.push_back(v); });
        } else if (!name.empty() && name[0] == '_') {
          auto& dst = p.extra_attrs[name];
          gather(*cached_vec4(acc, true, 1.f), [&](const Vec4& v) { dst.push_back(v); });
        }
      }
      const size_t nu = p.pos.size();
      if (p.nrm.size() != nu) p.nrm.clear();
      if (p.uv.size() != nu) p.uv.clear();
      if (p.col.size() != nu) p.col.clear();
      for (auto it = p.extra_attrs.begin(); it != p.extra_attrs.end();) {
        it = it->second.size() == nu ? std::next(it) : p.extra_attrs.erase(it);
      }
      if (mode == TINYGLTF_MODE_TRIANGLES) {
        p.idx = std::move(raw);
        p.idx.resize(p.idx.size() / 3 * 3);
      } else if (mode == TINYGLTF_MODE_TRIANGLE_STRIP) {
        for (size_t i = 2; i < raw.size(); i++) {
          if (i % 2 == 0) p.idx.insert(p.idx.end(), {raw[i - 2], raw[i - 1], raw[i]});
          else p.idx.insert(p.idx.end(), {raw[i - 1], raw[i - 2], raw[i]});
        }
      } else {
        for (size_t i = 2; i < raw.size(); i++) p.idx.insert(p.idx.end(), {raw[0], raw[i - 1], raw[i]});
      }
      if (p.nrm.empty()) p.compute_normals();
      if (gp.material >= 0 && gp.material < (int)material_map.size()) {
        p.material = material_map[gp.material];
      }
      if (pi < pat_views.size() && pat_views[pi] >= 0) {
        p.tri_pat = read_u32_view(model, pat_views[pi]);
        if (p.tri_pat.size() != p.tri_count()) p.tri_pat.clear();
      }
      res.triangles += p.tri_count();
      mesh->prims.push_back(std::move(p));
    }
    mesh_cache[mi] = mesh;
    return mesh;
  };

  // nodes
  std::function<void(int, uint32_t)> visit = [&](int gi, uint32_t parent) {
    const auto& gn = model.nodes[gi];
    Node n;
    n.parent = parent;
    n.name = gn.name.empty() ? ("node-" + std::to_string(gi)) : gn.name;
    if (gn.matrix.size() == 16) {
      Mat4 m;
      for (int i = 0; i < 16; i++) m.m[i] = (float)gn.matrix[i];
      n.local = Transform::from_matrix(m);
    } else {
      if (gn.translation.size() == 3)
        n.local.t = {(float)gn.translation[0], (float)gn.translation[1], (float)gn.translation[2]};
      if (gn.rotation.size() == 4)
        n.local.r = Quat{(float)gn.rotation[0], (float)gn.rotation[1], (float)gn.rotation[2],
                         (float)gn.rotation[3]}
                        .normalized();
      if (gn.scale.size() == 3)
        n.local.s = {(float)gn.scale[0], (float)gn.scale[1], (float)gn.scale[2]};
    }
    json extras = to_json(gn.extras);
    if (!extras.is_object()) extras = json::object();
    n.mesh = get_mesh(gn.mesh);

    NodeKind kind = n.mesh ? NodeKind::Render : NodeKind::Group;
    if (extras.contains("og_kind")) {
      kind = kind_from_name(extras["og_kind"].get<std::string>(), kind);
    } else if (json_int(extras, "set_collision") && json_int(extras, "set_invisible") &&
               !json_int(extras, "ignore") && n.mesh) {
      kind = NodeKind::Collision;
    }
    if (extras.value("kind", std::string()) == "actor" && extras.contains("etype") && !n.mesh) {
      // an actor of a rip: etype, aid, game_task, bsphere and lump, as the game has them
      kind = NodeKind::Actor;
      extras.erase("kind");
    }
    n.kind = kind;
    if (kind == NodeKind::Collision && n.mesh) {
      // one surface per triangle: from the project's own data, or the node's collide_pat
      bool has_pats = true;
      for (const auto& p : n.mesh->prims) has_pats &= p.tri_pat.size() == p.tri_count();
      if (!has_pats) {
        const uint32_t pat = extras.contains("collide_pat") && extras["collide_pat"].is_number_integer()
                                 ? extras["collide_pat"].get<uint32_t>()
                                 : 0;
        for (auto& p : n.mesh->prims) p.tri_pat.assign(p.tri_count(), pat);
      }
      for (const char* k : kCollisionKeys) extras.erase(k);
    }
    n.visible = !extras.value("og_hidden", false);
    n.locked = extras.value("og_locked", false);
    for (const char* k : {"og_kind", "og_id", "og_hidden", "og_locked"}) extras.erase(k);
    n.extras = extras;
    uint32_t id = scene.add(std::move(n));
    res.nodes++;
    if (parent == opts.parent) res.roots.push_back(id);
    for (int c : gn.children) {
      if (c >= 0 && c < (int)model.nodes.size()) visit(c, id);
    }
  };

  std::vector<int> roots;
  if (!model.scenes.empty()) {
    int si = model.defaultScene >= 0 ? model.defaultScene : 0;
    roots = model.scenes[si].nodes;
  } else {
    std::set<int> children;
    for (const auto& n : model.nodes)
      for (int c : n.children) children.insert(c);
    for (int i = 0; i < (int)model.nodes.size(); i++)
      if (!children.count(i)) roots.push_back(i);
  }
  for (int r : roots) {
    if (r >= 0 && r < (int)model.nodes.size()) visit(r, opts.parent);
  }
  if (result) *result = res;
  return true;
}

}  // namespace

bool import_gltf(Scene& scene, const std::string& path, const ImportOptions& opts, ImportResult* result,
                 std::string* error) {
  // a damaged file must not take the editor down (tinygltf and json throw on some of them)
  try {
    return import_gltf_file(scene, path, opts, result, error);
  } catch (const std::exception& e) {
    if (error) *error = std::string("unreadable glTF file: ") + e.what();
    return false;
  }
}

// ------------------------------------------------------------------------------------------------
// export
// ------------------------------------------------------------------------------------------------

namespace {

class GlbWriter {
 public:
  json doc;
  std::vector<uint8_t> bin;

  GlbWriter() {
    doc["asset"] = {{"version", "2.0"}, {"generator", "open-goal-level-editor"}};
    doc["bufferViews"] = json::array();
    doc["accessors"] = json::array();
  }

  int add_view(const void* data, size_t bytes, int target) {
    while (bin.size() % 4) bin.push_back(0);
    size_t offset = bin.size();
    const uint8_t* p = (const uint8_t*)data;
    bin.insert(bin.end(), p, p + bytes);
    json bv = {{"buffer", 0}, {"byteOffset", offset}, {"byteLength", bytes}};
    if (target) bv["target"] = target;
    doc["bufferViews"].push_back(bv);
    return (int)doc["bufferViews"].size() - 1;
  }

  int add_accessor(const void* data, size_t count, int comps, int ctype, int target,
                   const char* type, bool minmax) {
    size_t csize = ctype == 5126 || ctype == 5125 ? 4 : ctype == 5123 ? 2 : 1;
    int view = add_view(data, count * comps * csize, target);
    json acc = {{"bufferView", view}, {"componentType", ctype}, {"count", count}, {"type", type}};
    if (minmax && ctype == 5126 && count > 0) {
      const float* f = (const float*)data;
      std::vector<float> lo(comps, 1e30f), hi(comps, -1e30f);
      for (size_t i = 0; i < count; i++) {
        for (int c = 0; c < comps; c++) {
          lo[c] = std::min(lo[c], f[i * comps + c]);
          hi[c] = std::max(hi[c], f[i * comps + c]);
        }
      }
      acc["min"] = lo;
      acc["max"] = hi;
    }
    doc["accessors"].push_back(acc);
    return (int)doc["accessors"].size() - 1;
  }

  bool write(const std::string& path, std::string* error) {
    // no geometry (an empty document): no buffer, no binary chunk (a buffer is never empty)
    if (!bin.empty()) doc["buffers"] = json::array({json{{"byteLength", bin.size()}}});
    std::string text = doc.dump();
    while (text.size() % 4) text.push_back(' ');
    while (bin.size() % 4) bin.push_back(0);
    const uint32_t total = 12 + 8 + (uint32_t)text.size() + (bin.empty() ? 0 : 8 + (uint32_t)bin.size());
    std::ofstream f(std::filesystem::path(std::u8string((const char8_t*)path.c_str())),
                    std::ios::binary);
    if (!f) {
      if (error) *error = "cannot write " + path;
      return false;
    }
    auto u32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
    u32(0x46546C67);  // "glTF"
    u32(2);
    u32(total);
    u32((uint32_t)text.size());
    u32(0x4E4F534A);  // JSON
    f.write(text.data(), text.size());
    if (!bin.empty()) {
      u32((uint32_t)bin.size());
      u32(0x004E4942);  // BIN
      f.write((const char*)bin.data(), bin.size());
    }
    return (bool)f;
  }
};

void png_write_cb(void* ctx, void* data, int size) {
  auto* v = (std::vector<uint8_t>*)ctx;
  v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
}

struct ExportContext {
  const Scene& scene;
  GlbWriter w;
  std::map<int, int> material_map;  // scene material -> gltf material
  std::map<int, int> texture_map;   // scene texture -> gltf texture
  std::map<uint64_t, int> mesh_map; // mesh uid -> gltf mesh
  bool sampler_added = false;

  explicit ExportContext(const Scene& s) : scene(s) {
    w.doc["nodes"] = json::array();
    w.doc["meshes"] = json::array();
  }

  int texture(int ti) {
    auto it = texture_map.find(ti);
    if (it != texture_map.end()) return it->second;
    if (ti < 0 || ti >= (int)scene.textures.size()) return -1;
    const auto& t = scene.textures[ti];
    if (t.width <= 0 || t.height <= 0) return -1;
    std::vector<uint8_t> png;
    stbi_write_png_to_func(png_write_cb, &png, t.width, t.height, 4, t.rgba.data(), t.width * 4);
    int view = w.add_view(png.data(), png.size(), 0);
    if (!w.doc.contains("images")) w.doc["images"] = json::array();
    if (!w.doc.contains("textures")) w.doc["textures"] = json::array();
    if (!sampler_added) {
      // linear, repeat. The builder asserts that every texture has a sampler.
      w.doc["samplers"] = json::array(
          {json{{"magFilter", 9729}, {"minFilter", 9987}, {"wrapS", 10497}, {"wrapT", 10497}}});
      sampler_added = true;
    }
    w.doc["images"].push_back({{"name", t.name}, {"bufferView", view}, {"mimeType", "image/png"}});
    w.doc["textures"].push_back({{"source", (int)w.doc["images"].size() - 1}, {"sampler", 0}});
    int gi = (int)w.doc["textures"].size() - 1;
    texture_map[ti] = gi;
    return gi;
  }

  int material(int mi) {
    auto it = material_map.find(mi);
    if (it != material_map.end()) return it->second;
    if (mi < 0 || mi >= (int)scene.materials.size()) return -1;
    const auto& m = scene.materials[mi];
    json pbr = {{"baseColorFactor", {m.base_color.x, m.base_color.y, m.base_color.z, m.base_color.w}},
                {"metallicFactor", 0.0},
                {"roughnessFactor", 1.0}};
    int ti = texture(m.texture);
    if (ti >= 0) pbr["baseColorTexture"] = {{"index", ti}};
    json jm = {{"name", m.name}, {"pbrMetallicRoughness", pbr}, {"doubleSided", m.double_sided}};
    if (m.alpha == AlphaMode::Mask) {
      jm["alphaMode"] = "MASK";
      jm["alphaCutoff"] = m.cutoff;
    } else if (m.alpha == AlphaMode::Blend) {
      // the PS2 also drops the texels of blended surfaces under a reference: kept like the
      // extractor writes it (without it, water and glass would come back with glTF's 0.5)
      jm["alphaMode"] = "BLEND";
      jm["alphaCutoff"] = m.cutoff;
    }
    if (m.blend != BlendMode::Alpha) jm["extras"] = {{"og_blend", blend_mode_name(m.blend)}};
    if (!w.doc.contains("materials")) w.doc["materials"] = json::array();
    w.doc["materials"].push_back(jm);
    int gi = (int)w.doc["materials"].size() - 1;
    material_map[mi] = gi;
    return gi;
  }

  json primitive(const Primitive& p, bool with_material) {
    json jp;
    jp["mode"] = 4;
    json attrs;
    attrs["POSITION"] = w.add_accessor(p.pos.data(), p.pos.size(), 3, 5126, 34962, "VEC3", true);
    std::vector<Vec3> nrm = p.nrm;
    if (nrm.size() != p.pos.size()) {
      Primitive tmp;
      tmp.pos = p.pos;
      tmp.idx = p.idx;
      tmp.compute_normals();
      nrm = tmp.nrm;
    }
    attrs["NORMAL"] = w.add_accessor(nrm.data(), nrm.size(), 3, 5126, 34962, "VEC3", false);
    if (p.uv.size() == p.pos.size())
      attrs["TEXCOORD_0"] = w.add_accessor(p.uv.data(), p.uv.size(), 2, 5126, 34962, "VEC2", false);
    if (p.col.size() == p.pos.size())
      attrs["COLOR_0"] = w.add_accessor(p.col.data(), p.col.size(), 4, 5126, 34962, "VEC4", false);
    for (const auto& [name, data] : p.extra_attrs) {
      if (data.size() == p.pos.size())
        attrs[name] = w.add_accessor(data.data(), data.size(), 4, 5126, 34962, "VEC4", false);
    }
    jp["attributes"] = attrs;
    jp["indices"] = w.add_accessor(p.idx.data(), p.idx.size(), 1, 5125, 34963, "SCALAR", false);
    if (with_material && p.material >= 0) {
      int mi = material(p.material);
      if (mi >= 0) jp["material"] = mi;
    }
    return jp;
  }

  // the whole mesh, shared between nodes (project and object exports)
  int mesh(const std::shared_ptr<Mesh>& m, bool collision) {
    auto it = mesh_map.find(m->uid);
    if (it != mesh_map.end()) return it->second;
    json jm;
    jm["name"] = m->name;
    json prims = json::array();
    std::vector<int> pat_views;
    for (const auto& p : m->prims) {
      if (p.idx.empty()) continue;
      prims.push_back(primitive(p, !collision));
      if (collision) {
        pat_views.push_back(p.tri_pat.size() == p.tri_count()
                                ? w.add_view(p.tri_pat.data(), p.tri_pat.size() * 4, 0)
                                : -1);
      }
    }
    jm["primitives"] = prims;
    json extras = m->extras.is_object() ? m->extras : json::object();
    if (collision) extras["og_tri_pat_views"] = pat_views;
    if (!extras.empty()) jm["extras"] = extras;
    w.doc["meshes"].push_back(jm);
    int gi = (int)w.doc["meshes"].size() - 1;
    mesh_map[m->uid] = gi;
    return gi;
  }

  int add_node(json jn) {
    w.doc["nodes"].push_back(std::move(jn));
    return (int)w.doc["nodes"].size() - 1;
  }
};

void trs_json(json& jn, const Transform& t) {
  jn["translation"] = {t.t.x, t.t.y, t.t.z};
  jn["rotation"] = {t.r.x, t.r.y, t.r.z, t.r.w};
  jn["scale"] = {t.s.x, t.s.y, t.s.z};
}

}  // namespace

bool export_glb(const Scene& scene, const std::string& path, const ExportOptions& opts,
                std::string* error) {
  ExportContext ctx(scene);
  json scene_roots = json::array();

  // hierarchy with local transforms and the editor's extras
  std::set<uint32_t> included;
  const Mat4 pivot_inv = opts.pivot.inverse();
  if (opts.mode == ExportMode::Prefab) {
    for (auto id : opts.only) {
      included.insert(id);
      for (auto d : scene.descendants(id)) included.insert(d);
    }
  } else {
    for (const auto& n : scene.nodes) included.insert(n.id);
  }
  std::map<uint32_t, int> gltf_index;
  // first pass: create the nodes
  for (const auto& n : scene.nodes) {
    if (!included.count(n.id)) continue;
    json jn;
    jn["name"] = n.name;
    Transform t = n.local;
    bool is_root = !included.count(n.parent);
    if (opts.mode == ExportMode::Prefab && is_root) {
      // its parents are not in the file: its world matrix, relative to the pivot
      t = Transform::from_matrix(pivot_inv * scene.world(n));
    }
    trs_json(jn, t);
    if (n.mesh) jn["mesh"] = ctx.mesh(n.mesh, n.kind == NodeKind::Collision);
    json extras = n.extras.is_object() ? n.extras : json::object();
    extras["og_kind"] = kind_name(n.kind);
    if (opts.mode == ExportMode::Project) extras["og_id"] = n.id;
    if (!n.visible) extras["og_hidden"] = true;
    if (n.locked) extras["og_locked"] = true;
    jn["extras"] = extras;
    gltf_index[n.id] = ctx.add_node(jn);
  }
  // second pass: children and roots
  for (const auto& n : scene.nodes) {
    if (!included.count(n.id)) continue;
    int gi = gltf_index[n.id];
    if (included.count(n.parent)) {
      auto& parent = ctx.w.doc["nodes"][gltf_index[n.parent]];
      if (!parent.contains("children")) parent["children"] = json::array();
      parent["children"].push_back(gi);
    } else {
      scene_roots.push_back(gi);
    }
  }
  if (opts.mode == ExportMode::Project) {
    ctx.w.doc["asset"]["extras"] = {{"ogle", {{"settings", scene.settings.to_json()}}}};
  }

  ctx.w.doc["scenes"] = json::array({json{{"nodes", scene_roots}}});
  ctx.w.doc["scene"] = 0;
  if (ctx.w.doc["meshes"].empty()) ctx.w.doc.erase("meshes");
  return ctx.w.write(path, error);
}

}  // namespace ogle
