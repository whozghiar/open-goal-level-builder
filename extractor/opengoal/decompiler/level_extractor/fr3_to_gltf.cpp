#include "fr3_to_gltf.h"

#include <algorithm>
#include <map>
#include <tuple>
#include <unordered_map>

#include "common/custom_data/Tfrag3Data.h"
#include "common/math/Vector.h"
#include "common/math/geometry.h"

#include "decompiler/level_extractor/tfrag_tie_fixup.h"

#include "third-party/tiny_gltf/tiny_gltf.h"

namespace {

/*!
 * Remove 4096 meter scaling from a transformation matrix.
 */
math::Matrix4f unscale_translation(const math::Matrix4f& in) {
  auto out = in;
  for (int i = 0; i < 3; i++) {
    out(i, 3) /= 4096.;
  }
  return out;
}

/*!
 * Convert fr3 format indices (strip format, with UINT32_MAX as restart) to unstripped tris.
 * Assumes that this is the tfrag/tie format of stripping. Will flip tris as needed so the faces
 * in this fragment all point a consistent way. However, the entire frag may be flipped.
 */
void unstrip_tfrag_tie(const std::vector<u32>& stripped_indices,
                       const std::vector<math::Vector3f>& positions,
                       std::vector<u32>& unstripped,
                       std::vector<u32>& old_to_new_start) {
  fixup_and_unstrip_tfrag_tie(stripped_indices, positions, unstripped, old_to_new_start);
}

/*!
 * Convert shrub strips. This doesn't assume anything about the strips.
 */
void unstrip_shrub_draws(const std::vector<u32>& stripped_indices,
                         std::vector<u32>& unstripped,
                         std::vector<u32>& draw_to_start,
                         std::vector<u32>& draw_to_count,
                         const std::vector<tfrag3::ShrubDraw>& draws) {
  for (auto& draw : draws) {
    draw_to_start.push_back(unstripped.size());

    for (size_t i = 2; i < draw.num_indices; i++) {
      int idx = i + draw.first_index_index;
      u32 a = stripped_indices[idx];
      u32 b = stripped_indices[idx - 1];
      u32 c = stripped_indices[idx - 2];
      if (a == UINT32_MAX || b == UINT32_MAX || c == UINT32_MAX) {
        continue;
      }
      unstripped.push_back(a);
      unstripped.push_back(b);
      unstripped.push_back(c);
    }
    draw_to_count.push_back(unstripped.size() - draw_to_start.back());
  }
}

void unstrip_tie_wind(std::vector<u32>& unstripped,
                      std::vector<std::vector<u32>>& draw_to_starts,
                      std::vector<std::vector<u32>>& draw_to_counts,
                      const std::vector<tfrag3::InstancedStripDraw>& draws) {
  for (auto& draw : draws) {
    auto& starts = draw_to_starts.emplace_back();
    auto& counts = draw_to_counts.emplace_back();

    int grp_offset = 0;

    for (const auto& grp : draw.instance_groups) {
      starts.push_back(unstripped.size());

      for (size_t i = grp_offset + 2; i < grp_offset + grp.num; i++) {
        u32 a = draw.vertex_index_stream.at(i);
        u32 b = draw.vertex_index_stream.at(i - 1);
        u32 c = draw.vertex_index_stream.at(i - 2);
        if (a == UINT32_MAX || b == UINT32_MAX || c == UINT32_MAX) {
          continue;
        }
        unstripped.push_back(a);
        unstripped.push_back(b);
        unstripped.push_back(c);
      }
      counts.push_back(unstripped.size() - starts.back());
      grp_offset += grp.num;
    }
  }
}

float merc_should_swap_tris(const std::vector<u32>& tris,
                            const std::vector<tfrag3::MercVertex>& vertices) {
  float sum = 0;
  for (size_t i = 0; i + 2 < tris.size(); i += 3) {
    const auto& a = vertices[tris[i]];
    const auto& b = vertices[tris[i + 1]];
    const auto& c = vertices[tris[i + 2]];
    const auto geometric = (b.pos_vec - a.pos_vec).cross(c.pos_vec - a.pos_vec);
    const auto from_vertices = a.normal_vec + b.normal_vec + c.normal_vec;
    const float scale = geometric.length() * from_vertices.length();
    if (scale > 0) {
      sum += geometric.dot(from_vertices) / scale;
    }
  }
  return sum;
}

/*!
 * Convert merc strips. Doesn't assume anything about strips. Output is [effect][draw] format (done
 * for each model)
 */
void unstrip_merc_draws(const std::vector<u32>& stripped_indices,
                        const tfrag3::MercModel& model,
                        const std::vector<tfrag3::MercVertex>& vertices,
                        std::vector<u32>& unstripped,
                        std::vector<std::vector<u32>>& draw_to_start,
                        std::vector<std::vector<u32>>& draw_to_count) {
  // triangles of the strip we're currently in, held back so we can flip the whole thing at once
  std::vector<u32> strip;

  auto flush_strip = [&]() {
    if (strip.empty()) {
      return;
    }
    if (merc_should_swap_tris(strip, vertices) < 0) {
      for (size_t i = 0; i + 2 < strip.size(); i += 3) {
        std::swap(strip[i], strip[i + 1]);
      }
    }
    unstripped.insert(unstripped.end(), strip.begin(), strip.end());
    strip.clear();
  };

  for (auto& effect : model.effects) {
    auto& effect_dts = draw_to_start.emplace_back();
    auto& effect_dtc = draw_to_count.emplace_back();
    for (auto& draw : effect.all_draws) {
      effect_dts.push_back(unstripped.size());

      // a triangle strip flips every other triangle, so un-flip them here. the toggle restarts
      // with each strip.
      bool toggle = false;
      for (size_t i = 2; i < draw.index_count; i++) {
        int idx = i + draw.first_index;
        u32 a = stripped_indices[idx];
        u32 b = stripped_indices[idx - 1];
        u32 c = stripped_indices[idx - 2];
        if (a == UINT32_MAX || b == UINT32_MAX || c == UINT32_MAX) {
          flush_strip();
          toggle = false;
          continue;
        }
        strip.push_back(a);
        strip.push_back(toggle ? b : c);
        strip.push_back(toggle ? c : b);
        toggle = !toggle;
      }
      flush_strip();
      effect_dtc.push_back(unstripped.size() - effect_dts.back());
    }
  }
}

/*!
 * Get just the xyz positions from a preloaded vertex vector.
 */
std::vector<math::Vector3f> extract_positions(const std::vector<tfrag3::PreloadedVertex>& vtx) {
  std::vector<math::Vector3f> result;
  for (auto& v : vtx) {
    auto& o = result.emplace_back();
    o[0] = v.x;
    o[1] = v.y;
    o[2] = v.z;
  }
  return result;
}

/*!
 * Set up a buffer for the positions of the given vertices.
 * Return the index of the accessor.
 */
template <typename T>
int make_position_buffer_accessor(const std::vector<T>& vertices, tinygltf::Model& model) {
  // first create a buffer:
  int buffer_idx = (int)model.buffers.size();
  auto& buffer = model.buffers.emplace_back();
  buffer.data.resize(sizeof(float) * 3 * vertices.size());

  // and fill it
  u8* buffer_ptr = buffer.data.data();
  for (const auto& vtx : vertices) {
    if constexpr (std::is_same<T, tfrag3::MercVertex>::value) {
      float xyz[3] = {vtx.pos[0] / 4096.f, vtx.pos[1] / 4096.f, vtx.pos[2] / 4096.f};
      memcpy(buffer_ptr, xyz, 3 * sizeof(float));
      buffer_ptr += 3 * sizeof(float);
    } else {
      float xyz[3] = {vtx.x / 4096.f, vtx.y / 4096.f, vtx.z / 4096.f};
      memcpy(buffer_ptr, xyz, 3 * sizeof(float));
      buffer_ptr += 3 * sizeof(float);
    }
  }

  // create a view of this buffer
  int buffer_view_idx = (int)model.bufferViews.size();
  auto& buffer_view = model.bufferViews.emplace_back();
  buffer_view.buffer = buffer_idx;
  buffer_view.byteOffset = 0;
  buffer_view.byteLength = buffer.data.size();
  buffer_view.byteStride = 0;  // tightly packed
  buffer_view.target = TINYGLTF_TARGET_ARRAY_BUFFER;

  int accessor_idx = (int)model.accessors.size();
  auto& accessor = model.accessors.emplace_back();
  accessor.bufferView = buffer_view_idx;
  accessor.byteOffset = 0;
  accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  accessor.count = vertices.size();
  accessor.type = TINYGLTF_TYPE_VEC3;

  return accessor_idx;
}

/*!
 * Set up a buffer for the texture coordinates of the given vertices, multiplying by scale.
 * Return the index of the accessor.
 */
template <typename T>
int make_tex_buffer_accessor(const std::vector<T>& vertices, tinygltf::Model& model, float scale) {
  // first create a buffer:
  int buffer_idx = (int)model.buffers.size();
  auto& buffer = model.buffers.emplace_back();
  buffer.data.resize(sizeof(float) * 2 * vertices.size());

  // and fill it
  u8* buffer_ptr = buffer.data.data();
  for (const auto& vtx : vertices) {
    if constexpr (std::is_same<T, tfrag3::MercVertex>::value) {
      float st[2] = {vtx.st[0] * scale, vtx.st[1] * scale};
      memcpy(buffer_ptr, st, 2 * sizeof(float));
      buffer_ptr += 2 * sizeof(float);
    } else {
      float st[2] = {vtx.s * scale, vtx.t * scale};
      memcpy(buffer_ptr, st, 2 * sizeof(float));
      buffer_ptr += 2 * sizeof(float);
    }
  }

  // create a view of this buffer
  int buffer_view_idx = (int)model.bufferViews.size();
  auto& buffer_view = model.bufferViews.emplace_back();
  buffer_view.buffer = buffer_idx;
  buffer_view.byteOffset = 0;
  buffer_view.byteLength = buffer.data.size();
  buffer_view.byteStride = 0;  // tightly packed
  buffer_view.target = TINYGLTF_TARGET_ARRAY_BUFFER;

  int accessor_idx = (int)model.accessors.size();
  auto& accessor = model.accessors.emplace_back();
  accessor.bufferView = buffer_view_idx;
  accessor.byteOffset = 0;
  accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  accessor.count = vertices.size();
  accessor.type = TINYGLTF_TYPE_VEC2;

  return accessor_idx;
}

/*!
 * The color of a vertex, for the editor. The game mixes the 8 palettes of a level with weights it
 * sets each frame: in Jak II and Jak 3 they are light layers (the sun, the lamps of each area:
 * palette 0 alone is black indoors), in Jak 1 the times of the day. Their brightest value shows
 * every area lit by its own lights.
 */
u8 lit_color(const tfrag3::PackedTimeOfDay& tod, int color, int channel) {
  u8 best = 0;
  for (int palette = 0; palette < 8; palette++) {
    best = std::max(best, tod.read(color, palette, channel));
  }
  return best;
}

/*!
 * Set up a buffer of vertex colors for tfrag (lit_color).
 */
int make_color_buffer_accessor(const std::vector<tfrag3::PreloadedVertex>& vertices,
                               tinygltf::Model& model,
                               const tfrag3::TfragTree& tfrag_tree,
                               int /*time_of_day*/) {
  // first create a buffer:
  int buffer_idx = (int)model.buffers.size();
  auto& buffer = model.buffers.emplace_back();
  buffer.data.resize(sizeof(float) * 4 * vertices.size());
  std::vector<float> floats;

  for (size_t i = 0; i < vertices.size(); i++) {
    for (int j = 0; j < 3; j++) {
      floats.push_back(((float)lit_color(tfrag_tree.colors, vertices[i].color_index, j)) / 255.f);
    }
    floats.push_back(1.f);
  }
  memcpy(buffer.data.data(), floats.data(), sizeof(float) * floats.size());

  // create a view of this buffer
  int buffer_view_idx = (int)model.bufferViews.size();
  auto& buffer_view = model.bufferViews.emplace_back();
  buffer_view.buffer = buffer_idx;
  buffer_view.byteOffset = 0;
  buffer_view.byteLength = buffer.data.size();
  buffer_view.byteStride = 0;  // tightly packed
  buffer_view.target = TINYGLTF_TARGET_ARRAY_BUFFER;

  int accessor_idx = (int)model.accessors.size();
  auto& accessor = model.accessors.emplace_back();
  accessor.bufferView = buffer_view_idx;
  accessor.byteOffset = 0;
  accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  accessor.count = vertices.size();
  accessor.type = TINYGLTF_TYPE_VEC4;

  return accessor_idx;
}

/*!
 * Set up a buffer of vertex colors for tie (lit_color).
 */
int make_color_buffer_accessor(const std::vector<tfrag3::PreloadedVertex>& vertices,
                               tinygltf::Model& model,
                               const tfrag3::TieTree& tie_tree,
                               int /*time_of_day*/) {
  // first create a buffer:
  int buffer_idx = (int)model.buffers.size();
  auto& buffer = model.buffers.emplace_back();
  buffer.data.resize(sizeof(float) * 4 * vertices.size());
  std::vector<float> floats;

  for (size_t i = 0; i < vertices.size(); i++) {
    for (int j = 0; j < 3; j++) {
      floats.push_back(((float)lit_color(tie_tree.colors, vertices[i].color_index, j)) / 255.f);
    }
    floats.push_back(1.f);
  }
  memcpy(buffer.data.data(), floats.data(), sizeof(float) * floats.size());

  // create a view of this buffer
  int buffer_view_idx = (int)model.bufferViews.size();
  auto& buffer_view = model.bufferViews.emplace_back();
  buffer_view.buffer = buffer_idx;
  buffer_view.byteOffset = 0;
  buffer_view.byteLength = buffer.data.size();
  buffer_view.byteStride = 0;  // tightly packed
  buffer_view.target = TINYGLTF_TARGET_ARRAY_BUFFER;

  int accessor_idx = (int)model.accessors.size();
  auto& accessor = model.accessors.emplace_back();
  accessor.bufferView = buffer_view_idx;
  accessor.byteOffset = 0;
  accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  accessor.count = vertices.size();
  accessor.type = TINYGLTF_TYPE_VEC4;

  return accessor_idx;
}

int make_color_buffer_accessor(const std::vector<tfrag3::MercVertex>& vertices,
                               tinygltf::Model& model) {
  // first create a buffer:
  int buffer_idx = (int)model.buffers.size();
  auto& buffer = model.buffers.emplace_back();
  buffer.data.resize(sizeof(float) * 4 * vertices.size());
  std::vector<float> floats;

  for (size_t i = 0; i < vertices.size(); i++) {
    for (int j = 0; j < 3; j++) {
      floats.push_back(((float)vertices[i].rgba[j]) / 255.f);
    }
    floats.push_back(1.f);
  }
  memcpy(buffer.data.data(), floats.data(), sizeof(float) * floats.size());

  // create a view of this buffer
  int buffer_view_idx = (int)model.bufferViews.size();
  auto& buffer_view = model.bufferViews.emplace_back();
  buffer_view.buffer = buffer_idx;
  buffer_view.byteOffset = 0;
  buffer_view.byteLength = buffer.data.size();
  buffer_view.byteStride = 0;  // tightly packed
  buffer_view.target = TINYGLTF_TARGET_ARRAY_BUFFER;

  int accessor_idx = (int)model.accessors.size();
  auto& accessor = model.accessors.emplace_back();
  accessor.bufferView = buffer_view_idx;
  accessor.byteOffset = 0;
  accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  accessor.count = vertices.size();
  accessor.type = TINYGLTF_TYPE_VEC4;

  return accessor_idx;
}

/*!
 * Set up a buffer of vertex colors for shrub (lit_color).
 */
int make_color_buffer_accessor(const std::vector<tfrag3::ShrubGpuVertex>& vertices,
                               tinygltf::Model& model,
                               const tfrag3::ShrubTree& shrub_tree,
                               int /*time_of_day*/) {
  // first create a buffer:
  int buffer_idx = (int)model.buffers.size();
  auto& buffer = model.buffers.emplace_back();
  buffer.data.resize(sizeof(float) * 4 * vertices.size());
  std::vector<float> floats;

  for (size_t i = 0; i < vertices.size(); i++) {
    for (int j = 0; j < 3; j++) {
      floats.push_back(((float)lit_color(shrub_tree.time_of_day_colors, vertices[i].color_index, j)) /
                       255.f);
    }
    floats.push_back(1.f);
  }
  memcpy(buffer.data.data(), floats.data(), sizeof(float) * floats.size());

  // create a view of this buffer
  int buffer_view_idx = (int)model.bufferViews.size();
  auto& buffer_view = model.bufferViews.emplace_back();
  buffer_view.buffer = buffer_idx;
  buffer_view.byteOffset = 0;
  buffer_view.byteLength = buffer.data.size();
  buffer_view.byteStride = 0;  // tightly packed
  buffer_view.target = TINYGLTF_TARGET_ARRAY_BUFFER;

  int accessor_idx = (int)model.accessors.size();
  auto& accessor = model.accessors.emplace_back();
  accessor.bufferView = buffer_view_idx;
  accessor.byteOffset = 0;
  accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  accessor.count = vertices.size();
  accessor.type = TINYGLTF_TYPE_VEC4;

  return accessor_idx;
}

/*!
 * Create a tinygltf buffer and buffer view for indices, and convert to gltf format.
 * The map can be used to go from slots in the old index buffer to new.
 */
int make_tfrag_tie_index_buffer_view(const std::vector<u32>& indices,
                                     const std::vector<math::Vector3f>& positions,
                                     tinygltf::Model& model,
                                     std::vector<u32>& map_out) {
  std::vector<u32> unstripped;
  unstrip_tfrag_tie(indices, positions, unstripped, map_out);

  // first create a buffer:
  int buffer_idx = (int)model.buffers.size();
  auto& buffer = model.buffers.emplace_back();
  buffer.data.resize(sizeof(u32) * unstripped.size());

  // and fill it
  memcpy(buffer.data.data(), unstripped.data(), buffer.data.size());

  // create a view of this buffer
  int buffer_view_idx = (int)model.bufferViews.size();
  auto& buffer_view = model.bufferViews.emplace_back();
  buffer_view.buffer = buffer_idx;
  buffer_view.byteOffset = 0;
  buffer_view.byteLength = buffer.data.size();
  buffer_view.byteStride = 0;  // tightly packed
  buffer_view.target = TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER;
  return buffer_view_idx;
}

int make_tie_wind_index_buffer_view(const std::vector<tfrag3::InstancedStripDraw>& draws,
                                    tinygltf::Model& model,
                                    std::vector<std::vector<u32>>& draw_to_starts,
                                    std::vector<std::vector<u32>>& draw_to_counts) {
  std::vector<u32> unstripped;
  unstrip_tie_wind(unstripped, draw_to_starts, draw_to_counts, draws);

  // first create a buffer:
  int buffer_idx = (int)model.buffers.size();
  auto& buffer = model.buffers.emplace_back();
  buffer.data.resize(sizeof(u32) * unstripped.size());

  // and fill it
  memcpy(buffer.data.data(), unstripped.data(), buffer.data.size());

  // create a view of this buffer
  int buffer_view_idx = (int)model.bufferViews.size();
  auto& buffer_view = model.bufferViews.emplace_back();
  buffer_view.buffer = buffer_idx;
  buffer_view.byteOffset = 0;
  buffer_view.byteLength = buffer.data.size();
  buffer_view.byteStride = 0;  // tightly packed
  buffer_view.target = TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER;
  return buffer_view_idx;
}

/*!
 * Create a tinygltf buffer and buffer view for indices, and convert to gltf format.
 * The map can be used to go from slots in the old index buffer to new.
 */
int make_shrub_index_buffer_view(const std::vector<u32>& indices,
                                 const std::vector<tfrag3::ShrubDraw>& draws,
                                 tinygltf::Model& model,
                                 std::vector<u32>& draw_to_start,
                                 std::vector<u32>& draw_to_count) {
  std::vector<u32> unstripped;
  unstrip_shrub_draws(indices, unstripped, draw_to_start, draw_to_count, draws);

  // first create a buffer:
  int buffer_idx = (int)model.buffers.size();
  auto& buffer = model.buffers.emplace_back();
  buffer.data.resize(sizeof(u32) * unstripped.size());

  // and fill it
  memcpy(buffer.data.data(), unstripped.data(), buffer.data.size());

  // create a view of this buffer
  int buffer_view_idx = (int)model.bufferViews.size();
  auto& buffer_view = model.bufferViews.emplace_back();
  buffer_view.buffer = buffer_idx;
  buffer_view.byteOffset = 0;
  buffer_view.byteLength = buffer.data.size();
  buffer_view.byteStride = 0;  // tightly packed
  buffer_view.target = TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER;
  return buffer_view_idx;
}

int make_merc_index_buffer_view(const std::vector<u32>& unstripped, tinygltf::Model& model) {
  // first create a buffer:
  int buffer_idx = (int)model.buffers.size();
  auto& buffer = model.buffers.emplace_back();
  buffer.data.resize(sizeof(u32) * unstripped.size());

  // and fill it
  memcpy(buffer.data.data(), unstripped.data(), buffer.data.size());

  // create a view of this buffer
  int buffer_view_idx = (int)model.bufferViews.size();
  auto& buffer_view = model.bufferViews.emplace_back();
  buffer_view.buffer = buffer_idx;
  buffer_view.byteOffset = 0;
  buffer_view.byteLength = buffer.data.size();
  buffer_view.byteStride = 0;  // tightly packed
  buffer_view.target = TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER;
  return buffer_view_idx;
}

int make_index_buffer_accessor(tinygltf::Model& model,
                               const tfrag3::StripDraw& draw,
                               const std::vector<u32>& idx_map,
                               int buffer_view_idx) {
  int accessor_idx = (int)model.accessors.size();
  auto& accessor = model.accessors.emplace_back();
  accessor.bufferView = buffer_view_idx;
  accessor.byteOffset = sizeof(u32) * idx_map.at(draw.unpacked.idx_of_first_idx_in_full_buffer);
  accessor.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
  accessor.count = draw.num_triangles * 3;
  accessor.type = TINYGLTF_TYPE_SCALAR;

  return accessor_idx;
}

int make_index_buffer_accessor(tinygltf::Model& model, u32 start, u32 count, int buffer_view_idx) {
  int accessor_idx = (int)model.accessors.size();
  auto& accessor = model.accessors.emplace_back();
  accessor.bufferView = buffer_view_idx;

  accessor.byteOffset = sizeof(u32) * start;
  accessor.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
  accessor.count = count;
  accessor.type = TINYGLTF_TYPE_SCALAR;

  return accessor_idx;
}

enum class TexImageKind {
  RGBA,
  ENVMAP_STRENGTH,
};

using TexImageMap = std::map<std::pair<int, TexImageKind>, int>;

struct EnvmapInfo {
  int texture_idx = -1;
  DrawMode mode;
};

bool blends_with_src_alpha(const DrawMode& mode) {
  if (!mode.get_ab_enable()) {
    return false;
  }
  switch (mode.get_alpha_blend()) {
    case DrawMode::AlphaBlend::SRC_DST_SRC_DST:
    case DrawMode::AlphaBlend::SRC_0_SRC_DST:
    case DrawMode::AlphaBlend::ZERO_SRC_SRC_DST:
      return true;
    default:
      return false;
  }
}

int add_image_for_tex(const tfrag3::Level& level,
                      tinygltf::Model& model,
                      int tex_idx,
                      TexImageMap& tex_image_map,
                      TexImageKind kind) {
  const auto& existing = tex_image_map.find({tex_idx, kind});
  if (existing != tex_image_map.end()) {
    return existing->second;
  }

  auto& tex = level.textures.at(tex_idx);
  int image_idx = (int)model.images.size();
  auto& image = model.images.emplace_back();
  image.pixel_type = TINYGLTF_TEXTURE_TYPE_UNSIGNED_BYTE;
  image.width = tex.w;
  image.height = tex.h;
  image.image.resize(tex.data.size() * 4);
  image.bits = 8;
  image.component = 4;
  image.mimeType = "image/png";
  image.name = tex.debug_name;
  memcpy(image.image.data(), tex.data.data(), tex.data.size() * 4);

  if (kind == TexImageKind::ENVMAP_STRENGTH) {
    image.name = tex.debug_name + "-envmap-strength";
    for (size_t i = 0; i < tex.data.size(); i++) {
      const u8 metallic = (u8)std::min<u32>(255, (u32)image.image[i * 4 + 3] * 2);
      image.image[i * 4 + 0] = 255;
      image.image[i * 4 + 1] = 0;
      image.image[i * 4 + 2] = metallic;
      image.image[i * 4 + 3] = 255;
    }
  }

  tex_image_map[{tex_idx, kind}] = image_idx;
  return image_idx;
}

int add_gltf_texture(tinygltf::Model& model,
                     int image_idx,
                     const std::string& name,
                     const DrawMode& draw_mode) {
  int texture_idx = (int)model.textures.size();
  auto& gltf_texture = model.textures.emplace_back();
  gltf_texture.name = name;
  gltf_texture.source = image_idx;
  gltf_texture.sampler = (int)model.samplers.size();
  auto& sampler = model.samplers.emplace_back();
  sampler.minFilter = draw_mode.get_filt_enable() ? TINYGLTF_TEXTURE_FILTER_LINEAR
                                                  : TINYGLTF_TEXTURE_FILTER_NEAREST;
  sampler.magFilter = draw_mode.get_filt_enable() ? TINYGLTF_TEXTURE_FILTER_LINEAR
                                                  : TINYGLTF_TEXTURE_FILTER_NEAREST;
  sampler.wrapS = draw_mode.get_clamp_s_enable() ? TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE
                                                 : TINYGLTF_TEXTURE_WRAP_REPEAT;
  sampler.wrapT = draw_mode.get_clamp_t_enable() ? TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE
                                                 : TINYGLTF_TEXTURE_WRAP_REPEAT;
  sampler.name = name;
  return texture_idx;
}

int add_material_for_tex(const tfrag3::Level& level,
                         tinygltf::Model& model,
                         int tex_idx,
                         TexImageMap& tex_image_map,
                         const DrawMode& draw_mode,
                         const EnvmapInfo* envmap = nullptr) {
  if (tex_idx < 0) {
    return 0;
  }
  int mat_idx = (int)model.materials.size();
  auto& mat = model.materials.emplace_back();
  auto& tex = level.textures.at(tex_idx);

  mat.name = tex.debug_name;
  mat.doubleSided = true;
  // the 2.0 here compensates for the ps2's weird blending where 0.5 behaves like 1.0
  mat.pbrMetallicRoughness.baseColorFactor = {2.0, 2.0, 2.0, 2.0};
  mat.pbrMetallicRoughness.baseColorTexture.texCoord = 0;  // TEXCOORD_0, I think
  mat.pbrMetallicRoughness.baseColorTexture.index = add_gltf_texture(
      model, add_image_for_tex(level, model, tex_idx, tex_image_map, TexImageKind::RGBA),
      tex.debug_name, draw_mode);
  mat.alphaMode = draw_mode.get_ab_enable() ? "BLEND" : "MASK";
  // the foreground and background renderers both use this cutoff
  mat.alphaCutoff = (float)0x26 / 255.f;
  // glTF blends with the source alpha only: the other PS2 blends go to extras.og_blend
  // (ogle-extract; "add": Cs + Cd, "add_alpha": Cs * As + Cd, "subtract": Cd - Cs * As,
  // "half": (Cs + Cd) / 2)
  const char* og_blend = nullptr;
  if (draw_mode.get_ab_enable()) {
    switch (draw_mode.get_alpha_blend()) {
      case DrawMode::AlphaBlend::SRC_0_SRC_DST:
        og_blend = "add_alpha";
        break;
      case DrawMode::AlphaBlend::SRC_0_FIX_DST:
      case DrawMode::AlphaBlend::SRC_0_DST_DST:
        og_blend = "add";
        break;
      case DrawMode::AlphaBlend::ZERO_SRC_SRC_DST:
        og_blend = "subtract";
        break;
      case DrawMode::AlphaBlend::SRC_DST_FIX_DST:
        og_blend = "half";
        break;
      default:
        break;
    }
  }
  if (og_blend) {
    tinygltf::Value::Object extras;
    extras["og_blend"] = tinygltf::Value(std::string(og_blend));
    mat.extras = tinygltf::Value(extras);
  }
  // metallic is reserved for the envmap strength below
  mat.pbrMetallicRoughness.metallicFactor = 0.0;

  if (envmap && envmap->texture_idx >= 0) {
    mat.name = tex.debug_name + "-envmap";
    if (!blends_with_src_alpha(draw_mode) && !og_blend) {
      mat.alphaMode = "OPAQUE";
    }
    mat.pbrMetallicRoughness.metallicFactor = 1.0;
    mat.pbrMetallicRoughness.roughnessFactor = 1.0;
    mat.pbrMetallicRoughness.metallicRoughnessTexture.texCoord = 0;
    mat.pbrMetallicRoughness.metallicRoughnessTexture.index = add_gltf_texture(
        model,
        add_image_for_tex(level, model, tex_idx, tex_image_map, TexImageKind::ENVMAP_STRENGTH),
        tex.debug_name + "-envmap-strength", draw_mode);

    const auto& envmap_tex = level.textures.at(envmap->texture_idx);
    tinygltf::Value::Object specular_color_texture;
    specular_color_texture["index"] = tinygltf::Value(add_gltf_texture(
        model,
        add_image_for_tex(level, model, envmap->texture_idx, tex_image_map, TexImageKind::RGBA),
        envmap_tex.debug_name, envmap->mode));
    tinygltf::Value::Object specular;
    specular["specularColorTexture"] = tinygltf::Value(specular_color_texture);
    mat.extensions["KHR_materials_specular"] = tinygltf::Value(specular);

    if (std::find(model.extensionsUsed.begin(), model.extensionsUsed.end(),
                  "KHR_materials_specular") == model.extensionsUsed.end()) {
      model.extensionsUsed.push_back("KHR_materials_specular");
    }
  }

  return mat_idx;
}

std::string texture_name(const tfrag3::Level& level, s32 tree_tex_id) {
  if (tree_tex_id < 0) {
    return fmt::format("anim-slot-{}", -(tree_tex_id + 1));
  }
  return level.textures.at(tree_tex_id).debug_name;
}

constexpr int kMaxColor = 1;

// A tfrag piece this wide or deep (meters) is the terrain: the editor locks it.
constexpr float kTerrainExtent = 60.f;

/*!
 * Add a tfrag tree under a node "<level>-tfrag-<kind>". Its triangles are split in pieces: the
 * triangles their vertices connect (vertices at the same place are one), so that a building, a
 * wall or a bridge of the level's shell is a node of its own. A piece has one primitive per draw
 * (texture) and extras.og_terrain, true for the pieces kTerrainExtent across or more.
 */
void add_tfrag(const tfrag3::Level& level,
               const tfrag3::TfragTree& tfrag_in,
               tinygltf::Model& model,
               TexImageMap& tex_image_map) {
  // copy and unpack in place
  tfrag3::TfragTree tfrag = tfrag_in;
  tfrag.unpack();

  int node_idx = (int)model.nodes.size();
  auto& node = model.nodes.emplace_back();
  node.name =
      fmt::format("{}-tfrag-{}", level.level_name, tfrag3::tfrag_tree_names[(int)tfrag.kind]);
  model.scenes.at(0).nodes.push_back(node_idx);

  const auto positions = extract_positions(tfrag.unpacked.vertices);
  int position_buffer_accessor = make_position_buffer_accessor(tfrag.unpacked.vertices, model);
  int texture_buffer_accessor = make_tex_buffer_accessor(tfrag.unpacked.vertices, model, 1.f);
  int color_accessor = make_color_buffer_accessor(tfrag.unpacked.vertices, model, tfrag, 0);

  std::vector<u32> unstripped, old_to_new;
  unstrip_tfrag_tie(tfrag.unpacked.indices, positions, unstripped, old_to_new);

  // the pieces: union-find over the vertices, the ones at the same place first
  const u32 nv = (u32)positions.size();
  std::vector<u32> up(nv);
  for (u32 i = 0; i < nv; i++) {
    up[i] = i;
  }
  auto root = [&](u32 x) {
    while (up[x] != x) {
      up[x] = up[up[x]];
      x = up[x];
    }
    return x;
  };
  auto unite = [&](u32 a, u32 b) {
    a = root(a);
    b = root(b);
    if (a != b) {
      up[std::max(a, b)] = std::min(a, b);
    }
  };
  {
    // positions in game units, to 16 units (4 mm)
    auto q = [](float f) { return (u64)(u32)(s32)std::lround(f / 16.f) & 0x1fffff; };
    std::unordered_map<u64, u32> first_at;
    first_at.reserve(nv);
    for (u32 i = 0; i < nv; i++) {
      const u64 key = q(positions[i][0]) | (q(positions[i][1]) << 21) | (q(positions[i][2]) << 42);
      auto [it, fresh] = first_at.emplace(key, i);
      if (!fresh) {
        unite(i, it->second);
      }
    }
  }
  struct DrawRange {
    u32 start = 0, count = 0;
  };
  std::vector<DrawRange> draw_ranges(tfrag.draws.size());
  for (size_t d = 0; d < tfrag.draws.size(); d++) {
    const auto& draw = tfrag.draws[d];
    if (!draw.num_triangles) {
      continue;
    }
    draw_ranges[d] = {old_to_new.at(draw.unpacked.idx_of_first_idx_in_full_buffer),
                      draw.num_triangles * 3};
    for (u32 i = 0; i + 2 < draw_ranges[d].count; i += 3) {
      const u32* t = &unstripped.at(draw_ranges[d].start + i);
      unite(t[0], t[1]);
      unite(t[0], t[2]);
    }
  }
  // piece (its root vertex) -> draw -> indices
  std::map<u32, std::map<size_t, std::vector<u32>>> pieces;
  for (size_t d = 0; d < tfrag.draws.size(); d++) {
    for (u32 i = 0; i + 2 < draw_ranges[d].count; i += 3) {
      const u32* t = &unstripped.at(draw_ranges[d].start + i);
      auto& list = pieces[root(t[0])][d];
      list.insert(list.end(), t, t + 3);
    }
  }

  // one index buffer, the pieces one after the other
  std::vector<u32> grouped;
  std::map<std::pair<u32, size_t>, DrawRange> ranges;
  for (const auto& [piece, by_draw] : pieces) {
    for (const auto& [d, indices] : by_draw) {
      ranges[{piece, d}] = {(u32)grouped.size(), (u32)indices.size()};
      grouped.insert(grouped.end(), indices.begin(), indices.end());
    }
  }
  int buffer_idx = (int)model.buffers.size();
  auto& buffer = model.buffers.emplace_back();
  buffer.data.resize(sizeof(u32) * grouped.size());
  memcpy(buffer.data.data(), grouped.data(), buffer.data.size());
  int buffer_view_idx = (int)model.bufferViews.size();
  auto& buffer_view = model.bufferViews.emplace_back();
  buffer_view.buffer = buffer_idx;
  buffer_view.byteOffset = 0;
  buffer_view.byteLength = buffer.data.size();
  buffer_view.byteStride = 0;  // tightly packed
  buffer_view.target = TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER;

  std::vector<int> draw_material(tfrag.draws.size(), -1);
  int number = 0;
  for (const auto& [piece, by_draw] : pieces) {
    // its extent and its main texture
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    size_t main_draw = by_draw.begin()->first, main_count = 0;
    for (const auto& [d, indices] : by_draw) {
      for (u32 v : indices) {
        for (int k = 0; k < 3; k++) {
          lo[k] = std::min(lo[k], positions[v][k] / 4096.f);
          hi[k] = std::max(hi[k], positions[v][k] / 4096.f);
        }
      }
      if (indices.size() > main_count) {
        main_count = indices.size();
        main_draw = d;
      }
    }
    const bool terrain = hi[0] - lo[0] >= kTerrainExtent || hi[2] - lo[2] >= kTerrainExtent;

    int c_node_idx = (int)model.nodes.size();
    auto& c_node = model.nodes.emplace_back();
    c_node.name = fmt::format("{}-{}", texture_name(level, tfrag.draws[main_draw].tree_tex_id), number++);
    tinygltf::Value::Object extras;
    extras["og_terrain"] = tinygltf::Value(terrain);
    c_node.extras = tinygltf::Value(extras);
    model.nodes[node_idx].children.push_back(c_node_idx);
    int mesh_idx = (int)model.meshes.size();
    model.meshes.emplace_back().name = c_node.name;
    model.nodes[c_node_idx].mesh = mesh_idx;

    for (const auto& [d, indices] : by_draw) {
      const auto& draw = tfrag.draws[d];
      if (draw_material[d] < 0) {
        draw_material[d] =
            add_material_for_tex(level, model, draw.tree_tex_id, tex_image_map, draw.mode);
      }
      const auto& range = ranges.at({piece, d});
      auto& prim = model.meshes[mesh_idx].primitives.emplace_back();
      prim.material = draw_material[d];
      prim.indices = make_index_buffer_accessor(model, range.start, range.count, buffer_view_idx);
      prim.attributes["POSITION"] = position_buffer_accessor;
      prim.attributes["TEXCOORD_0"] = texture_buffer_accessor;
      prim.attributes["COLOR_0"] = color_accessor;
      prim.mode = TINYGLTF_MODE_TRIANGLES;
    }
  }
}

std::unordered_map<u32, EnvmapInfo> tie_envmap_by_run_start(const tfrag3::TieTree& tie) {
  std::unordered_map<u32, EnvmapInfo> result;
  for (int cat = 0; cat < tfrag3::kNumTieCategories; cat++) {
    if (!tfrag3::is_envmap_second_draw_category((tfrag3::TieCategory)cat)) {
      continue;
    }
    for (u32 draw_idx = tie.category_draw_indices[cat];
         draw_idx < tie.category_draw_indices[cat + 1]; draw_idx++) {
      const auto& draw = tie.static_draws.at(draw_idx);
      for (const auto& run : draw.runs) {
        auto& info = result[run.vertex0];
        info.texture_idx = draw.tree_tex_id;
        info.mode = draw.mode;
      }
    }
  }
  return result;
}

// a contiguous run of the gltf index buffer, in indices
struct IndexRange {
  u32 start = 0;
  u32 count = 0;
};

// the part of a tie draw that belongs to a single prototype
struct TieProtoDraw {
  u16 proto_idx = 0;
  u32 draw_idx = 0;
  bool operator<(const TieProtoDraw& other) const {
    return std::tie(proto_idx, draw_idx) < std::tie(other.proto_idx, other.draw_idx);
  }
};

int make_tie_index_buffer_view_by_proto(const tfrag3::TieTree& tie,
                                        const std::vector<u32>& draws_to_emit,
                                        tinygltf::Model& model,
                                        std::map<TieProtoDraw, IndexRange>& ranges_out) {
  std::vector<u32> unstripped, old_to_new;
  unstrip_tfrag_tie(tie.unpacked.indices, extract_positions(tie.unpacked.vertices), unstripped,
                    old_to_new);

  std::map<TieProtoDraw, std::vector<IndexRange>> src_ranges;
  for (u32 draw_idx : draws_to_emit) {
    const auto& draw = tie.static_draws.at(draw_idx);
    u32 old_start = draw.unpacked.idx_of_first_idx_in_full_buffer;
    for (const auto& grp : draw.vis_groups) {
      if (grp.num_tris) {
        src_ranges[{grp.tie_proto_idx, draw_idx}].push_back(
            {old_to_new.at(old_start), grp.num_tris * 3});
      }
      old_start += grp.num_inds;
    }
  }

  std::vector<u32> grouped;
  for (const auto& [key, ranges] : src_ranges) {
    const u32 start = grouped.size();
    for (const auto& [src, count] : ranges) {
      grouped.insert(grouped.end(), unstripped.begin() + src, unstripped.begin() + src + count);
    }
    ranges_out[key] = {start, (u32)grouped.size() - start};
  }

  int buffer_idx = (int)model.buffers.size();
  auto& buffer = model.buffers.emplace_back();
  buffer.data.resize(sizeof(u32) * grouped.size());
  memcpy(buffer.data.data(), grouped.data(), buffer.data.size());

  int buffer_view_idx = (int)model.bufferViews.size();
  auto& buffer_view = model.bufferViews.emplace_back();
  buffer_view.buffer = buffer_idx;
  buffer_view.byteOffset = 0;
  buffer_view.byteLength = buffer.data.size();
  buffer_view.byteStride = 0;  // tightly packed
  buffer_view.target = TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER;
  return buffer_view_idx;
}

std::string proto_name(const std::vector<std::string>& names, u16 proto_idx, const char* kind) {
  if (proto_idx < names.size() && !names[proto_idx].empty()) {
    return names[proto_idx];
  }
  return fmt::format("{}-proto-{}", kind, proto_idx);
}

void add_tie(const tfrag3::Level& level,
             const tfrag3::TieTree& tie_in,
             size_t tree_idx,
             tinygltf::Model& model,
             TexImageMap& tex_image_map) {
  // copy and unpack in place
  tfrag3::TieTree tie = tie_in;
  tie.unpack();

  // we'll make a Node, Mesh, Primitive, then add the data to the primitive.
  int node_idx = (int)model.nodes.size();
  auto& node = model.nodes.emplace_back();
  node.name = fmt::format("{}-tie-{}", level.level_name, tree_idx);
  model.scenes.at(0).nodes.push_back(node_idx);

  int position_buffer_accessor = make_position_buffer_accessor(tie.unpacked.vertices, model);
  int texture_buffer_accessor = make_tex_buffer_accessor(tie.unpacked.vertices, model, 1.f);
  int colors[kMaxColor];

  for (int i = 0; i < kMaxColor; i++) {
    colors[i] = make_color_buffer_accessor(tie.unpacked.vertices, model, tie, i);
  }

  const auto envmap_by_run_start = tie_envmap_by_run_start(tie);

  std::vector<u32> draws_to_emit;
  std::map<u32, int> draw_material;
  for (int cat = 0; cat < tfrag3::kNumTieCategories; cat++) {
    const auto category = (tfrag3::TieCategory)cat;
    if (tfrag3::is_envmap_second_draw_category(category)) {
      continue;
    }
    for (u32 draw_idx = tie.category_draw_indices[cat];
         draw_idx < tie.category_draw_indices[cat + 1]; draw_idx++) {
      const auto& draw = tie.static_draws[draw_idx];

      EnvmapInfo envmap_info;
      const EnvmapInfo* envmap = nullptr;
      if (tfrag3::is_envmap_first_draw_category(category) && !draw.runs.empty()) {
        const auto& first_run = draw.runs.front();
        const auto it = envmap_by_run_start.find(first_run.vertex0 + first_run.length);
        if (it != envmap_by_run_start.end() && it->second.texture_idx >= 0 &&
            (size_t)it->second.texture_idx < level.textures.size()) {
          envmap_info = it->second;
          envmap = &envmap_info;
        }
      }

      draws_to_emit.push_back(draw_idx);
      draw_material[draw_idx] =
          add_material_for_tex(level, model, draw.tree_tex_id, tex_image_map, draw.mode, envmap);
    }
  }

  std::map<TieProtoDraw, IndexRange> ranges;
  int index_buffer_view = make_tie_index_buffer_view_by_proto(tie, draws_to_emit, model, ranges);

  int mesh_idx = -1;
  int prev_proto = -1;
  for (const auto& [key, range] : ranges) {
    if (key.proto_idx != prev_proto) {
      prev_proto = key.proto_idx;
      int c_node_idx = (int)model.nodes.size();
      auto& c_node = model.nodes.emplace_back();
      c_node.name = proto_name(tie.proto_names, key.proto_idx, "tie");
      model.nodes[node_idx].children.push_back(c_node_idx);
      mesh_idx = (int)model.meshes.size();
      model.meshes.emplace_back();
      c_node.mesh = mesh_idx;
    }

    auto& prim = model.meshes[mesh_idx].primitives.emplace_back();
    prim.material = draw_material.at(key.draw_idx);
    prim.indices = make_index_buffer_accessor(model, range.start, range.count, index_buffer_view);
    prim.attributes["POSITION"] = position_buffer_accessor;
    prim.attributes["TEXCOORD_0"] = texture_buffer_accessor;
    for (int i = 0; i < kMaxColor; i++) {
      prim.attributes[fmt::format("COLOR_{}", i)] = colors[i];
    }
    prim.mode = TINYGLTF_MODE_TRIANGLES;
  }

  if (!tie.instanced_wind_draws.empty()) {
    std::vector<std::vector<u32>> draw_to_starts, draw_to_counts;
    int wind_index_buffer_view = make_tie_wind_index_buffer_view(tie.instanced_wind_draws, model,
                                                                 draw_to_starts, draw_to_counts);

    for (size_t draw_idx = 0; draw_idx < tie.instanced_wind_draws.size(); draw_idx++) {
      const auto& wind_draw = tie.instanced_wind_draws[draw_idx];
      int mat =
          add_material_for_tex(level, model, wind_draw.tree_tex_id, tex_image_map, wind_draw.mode);
      for (size_t grp_idx = 0; grp_idx < wind_draw.instance_groups.size(); grp_idx++) {
        const auto& grp = wind_draw.instance_groups[grp_idx];
        int c_node_idx = (int)model.nodes.size();
        auto& c_node = model.nodes.emplace_back();
        c_node.name = fmt::format("wind-group-{}-{}", draw_idx, grp_idx);
        model.nodes[node_idx].children.push_back(c_node_idx);
        int c_mesh_idx = (int)model.meshes.size();
        auto& c_mesh = model.meshes.emplace_back();
        c_node.mesh = c_mesh_idx;
        auto& prim = c_mesh.primitives.emplace_back();

        const auto& info = tie.wind_instance_info.at(grp.instance_idx);
        for (int i = 0; i < 4; i++) {
          float scale = i == 3 ? (1.f / 4096.f) : 1.f;
          for (int j = 0; j < 4; j++) {
            c_node.matrix.push_back(scale * info.matrix[i][j]);
          }
        }

        prim.material = mat;
        prim.indices = make_index_buffer_accessor(model, draw_to_starts.at(draw_idx).at(grp_idx),
                                                  draw_to_counts.at(draw_idx).at(grp_idx),
                                                  wind_index_buffer_view);
        prim.attributes["POSITION"] = position_buffer_accessor;
        prim.attributes["TEXCOORD_0"] = texture_buffer_accessor;
        for (int i = 0; i < kMaxColor; i++) {
          prim.attributes[fmt::format("COLOR_{}", i)] = colors[i];
        }
        prim.mode = TINYGLTF_MODE_TRIANGLES;
      }
    }
  }
}

void add_shrub(const tfrag3::Level& level,
               const tfrag3::ShrubTree& shrub_in,
               size_t tree_idx,
               tinygltf::Model& model,
               TexImageMap& tex_image_map) {
  // copy and unpack in place
  tfrag3::ShrubTree shrub = shrub_in;
  shrub.unpack();

  // we'll make a Node, Mesh, Primitive, then add the data to the primitive.
  int node_idx = (int)model.nodes.size();
  auto& node = model.nodes.emplace_back();
  node.name = fmt::format("{}-shrub-{}", level.level_name, tree_idx);
  model.scenes.at(0).nodes.push_back(node_idx);

  int position_buffer_accessor = make_position_buffer_accessor(shrub.unpacked.vertices, model);
  int texture_buffer_accessor =
      make_tex_buffer_accessor(shrub.unpacked.vertices, model, 1.f / 4096.f);
  std::vector<u32> draw_to_start, draw_to_count;
  int index_buffer_view = make_shrub_index_buffer_view(shrub.indices, shrub.static_draws, model,
                                                       draw_to_start, draw_to_count);
  int colors[kMaxColor];
  for (int i = 0; i < kMaxColor; i++) {
    colors[i] = make_color_buffer_accessor(shrub.unpacked.vertices, model, shrub, i);
  }

  std::map<u16, std::vector<size_t>> draws_by_proto;
  for (size_t draw_idx = 0; draw_idx < shrub.static_draws.size(); draw_idx++) {
    draws_by_proto[shrub.static_draws[draw_idx].proto_idx].push_back(draw_idx);
  }

  for (const auto& [proto_idx, draw_indices] : draws_by_proto) {
    int c_node_idx = (int)model.nodes.size();
    auto& c_node = model.nodes.emplace_back();
    c_node.name = proto_name(shrub.proto_names, proto_idx, "shrub");
    model.nodes[node_idx].children.push_back(c_node_idx);
    int mesh_idx = (int)model.meshes.size();
    model.meshes.emplace_back();
    c_node.mesh = mesh_idx;

    for (size_t draw_idx : draw_indices) {
      auto& draw = shrub.static_draws[draw_idx];
      auto& prim = model.meshes[mesh_idx].primitives.emplace_back();
      prim.material =
          add_material_for_tex(level, model, draw.tree_tex_id, tex_image_map, draw.mode);
      prim.indices = make_index_buffer_accessor(model, draw_to_start.at(draw_idx),
                                                draw_to_count.at(draw_idx), index_buffer_view);
      prim.attributes["POSITION"] = position_buffer_accessor;
      prim.attributes["TEXCOORD_0"] = texture_buffer_accessor;
      for (int i = 0; i < kMaxColor; i++) {
        prim.attributes[fmt::format("COLOR_{}", i)] = colors[i];
      }
      prim.mode = TINYGLTF_MODE_TRIANGLES;
    }
  }
}

int make_weights_accessor(const std::vector<tfrag3::MercVertex>& vertices, tinygltf::Model& model) {
  // first create a buffer:
  int buffer_idx = (int)model.buffers.size();
  auto& buffer = model.buffers.emplace_back();
  buffer.data.resize(sizeof(float) * 4 * vertices.size());

  // and fill it
  u8* buffer_ptr = buffer.data.data();
  for (const auto& vtx : vertices) {
    float weights[4] = {vtx.weights[0], vtx.weights[1], vtx.weights[2], 0};
    memcpy(buffer_ptr, weights, 4 * sizeof(float));
    buffer_ptr += 4 * sizeof(float);
  }

  // create a view of this buffer
  int buffer_view_idx = (int)model.bufferViews.size();
  auto& buffer_view = model.bufferViews.emplace_back();
  buffer_view.buffer = buffer_idx;
  buffer_view.byteOffset = 0;
  buffer_view.byteLength = buffer.data.size();
  buffer_view.byteStride = 0;  // tightly packed
  buffer_view.target = TINYGLTF_TARGET_ARRAY_BUFFER;

  int accessor_idx = (int)model.accessors.size();
  auto& accessor = model.accessors.emplace_back();
  accessor.bufferView = buffer_view_idx;
  accessor.byteOffset = 0;
  accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  accessor.count = vertices.size();
  accessor.type = TINYGLTF_TYPE_VEC4;
  return accessor_idx;
}

int make_normal_buffer_accessor(const std::vector<tfrag3::MercVertex>& vertices,
                                tinygltf::Model& model) {
  // first create a buffer:
  int buffer_idx = (int)model.buffers.size();
  auto& buffer = model.buffers.emplace_back();
  buffer.data.resize(sizeof(float) * 3 * vertices.size());

  // and fill it
  u8* buffer_ptr = buffer.data.data();
  for (const auto& vtx : vertices) {
    const float len = vtx.normal_vec.length();
    const auto normal = len > 0 ? vtx.normal_vec / len : math::Vector3f(0, 1, 0);
    memcpy(buffer_ptr, normal.data(), 3 * sizeof(float));
    buffer_ptr += 3 * sizeof(float);
  }

  // create a view of this buffer
  int buffer_view_idx = (int)model.bufferViews.size();
  auto& buffer_view = model.bufferViews.emplace_back();
  buffer_view.name = "NORMAL";
  buffer_view.buffer = buffer_idx;
  buffer_view.byteOffset = 0;
  buffer_view.byteLength = buffer.data.size();
  buffer_view.byteStride = 0;  // tightly packed
  buffer_view.target = TINYGLTF_TARGET_ARRAY_BUFFER;

  int accessor_idx = (int)model.accessors.size();
  auto& accessor = model.accessors.emplace_back();
  accessor.name = "NORMAL";
  accessor.bufferView = buffer_view_idx;
  accessor.byteOffset = 0;
  accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  accessor.count = vertices.size();
  accessor.type = TINYGLTF_TYPE_VEC3;
  return accessor_idx;
}

int make_bones_accessor(const std::vector<tfrag3::MercVertex>& vertices, tinygltf::Model& model) {
  // first create a buffer:
  int buffer_idx = (int)model.buffers.size();
  auto& buffer = model.buffers.emplace_back();
  buffer.data.resize(4 * vertices.size());

  // and fill it
  u8* buffer_ptr = buffer.data.data();
  for (const auto& vtx : vertices) {
    u8 indices[4];
    for (int i = 0; i < 3; i++) {
      indices[i] = vtx.mats[i] ? vtx.mats[i] - 1 : 0;
    }
    indices[3] = 0;
    memcpy(buffer_ptr, indices, 4);
    buffer_ptr += 4;
  }

  // create a view of this buffer
  int buffer_view_idx = (int)model.bufferViews.size();
  auto& buffer_view = model.bufferViews.emplace_back();
  buffer_view.buffer = buffer_idx;
  buffer_view.byteOffset = 0;
  buffer_view.byteLength = buffer.data.size();
  buffer_view.byteStride = 0;  // tightly packed
  buffer_view.target = TINYGLTF_TARGET_ARRAY_BUFFER;

  int accessor_idx = (int)model.accessors.size();
  auto& accessor = model.accessors.emplace_back();
  accessor.bufferView = buffer_view_idx;
  accessor.byteOffset = 0;
  accessor.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
  accessor.count = vertices.size();
  accessor.type = TINYGLTF_TYPE_VEC4;
  return accessor_idx;
}

int make_inv_matrix_bind_poses(const std::vector<level_tools::Joint>& joints,
                               tinygltf::Model& model) {
  // first create a buffer:
  int buffer_idx = (int)model.buffers.size();
  auto& buffer = model.buffers.emplace_back();
  buffer.data.resize(sizeof(float) * 16 * joints.size());

  // and fill it
  for (int m = 0; m < (int)joints.size(); m++) {
    auto matrix = unscale_translation(joints[m].bind_pose_T_w);
    for (int i = 0; i < 4; i++) {
      for (int j = 0; j < 4; j++) {
        memcpy(buffer.data.data() + sizeof(float) * (i * 4 + j + m * 16), &matrix(j, i), 4);
      }
    }
  }

  // create a view of this buffer
  int buffer_view_idx = (int)model.bufferViews.size();
  auto& buffer_view = model.bufferViews.emplace_back();
  buffer_view.buffer = buffer_idx;
  buffer_view.byteOffset = 0;
  buffer_view.byteLength = buffer.data.size();
  buffer_view.byteStride = 0;  // tightly packed
  buffer_view.target = TINYGLTF_TARGET_ARRAY_BUFFER;

  int accessor_idx = (int)model.accessors.size();
  auto& accessor = model.accessors.emplace_back();
  accessor.bufferView = buffer_view_idx;
  accessor.byteOffset = 0;
  accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  accessor.count = joints.size();
  accessor.type = TINYGLTF_TYPE_MAT4;
  return accessor_idx;
}

level_tools::UncompressedJointAnim decompress_anim(const level_tools::ArtJointAnim& art_anim) {
  constexpr float kQuatScale = 0.000030517578125f;
  constexpr float kScaleScale = 0.000244140625f;
  constexpr float kTransScale = 4.f / 4096.f;

  auto read_f32 = [](const u8*& ptr) -> float {
    float v;
    memcpy(&v, ptr, 4);
    ptr += 4;
    return v;
  };
  auto read_s16 = [](const u8*& ptr) -> float {
    s16 v;
    memcpy(&v, ptr, 2);
    ptr += 2;
    return v;
  };

  const auto& ctrl = art_anim.frames;
  const auto& fixed = ctrl.fixed;
  const auto& hdr = fixed.hdr;
  int num_joints = (int)hdr.num_joints;
  int total_frames = (int)ctrl.num_frames;

  level_tools::UncompressedJointAnim out;
  out.name = art_anim.name;
  out.framerate = art_anim.speed > 0.f ? art_anim.speed * 60.f : 30.f;
  out.frames = total_frames;
  out.joints.resize(2 + num_joints);

  auto d64 = (const u8*)fixed.data64.data();
  auto d32 = (const u8*)fixed.data32.data();
  auto d16 = (const u8*)fixed.data16.data();

  if (fixed.mat[0])
    d64 += 64;
  if (fixed.mat[1])
    d64 += 64;

  for (int tqi = 0; tqi < num_joints; tqi++) {
    int ctrl_idx = tqi / 8;
    int ctrl_shift = 4 * (tqi % 8);
    int c = 0xf & (hdr.control_bits[ctrl_idx] >> ctrl_shift);
    auto& joint = out.joints[2 + tqi];

    if (!(c & 0b0001)) {
      math::Vector3f t;
      if (c & 0b1000) {
        t.x() = read_f32(d64) / 4096.f;
        t.y() = read_f32(d64) / 4096.f;
        t.z() = read_f32(d32) / 4096.f;
      } else {
        t.x() = read_s16(d32) * kTransScale;
        t.y() = read_s16(d32) * kTransScale;
        t.z() = read_s16(d16) * kTransScale;
      }
      joint.trans_frames.push_back(t);
    }

    if (!(c & 0b0010)) {
      math::Vector4f q;
      q.x() = read_s16(d64) * kQuatScale;
      q.y() = read_s16(d64) * kQuatScale;
      q.z() = read_s16(d64) * kQuatScale;
      q.w() = read_s16(d64) * kQuatScale;
      joint.quat_frames.push_back(q);
    }

    if (!(c & 0b0100)) {
      math::Vector3f s;
      s.x() = read_s16(d32) * kScaleScale;
      s.y() = read_s16(d32) * kScaleScale;
      s.z() = read_s16(d16) * kScaleScale;
      joint.scale_frames.push_back(s);
    }
  }

  for (int fi = 0; fi < total_frames; fi++) {
    const auto& frame = ctrl.frame[fi];
    const u8* data64 = (const u8*)frame.data64.data();
    const u8* data32 = (const u8*)frame.data32.data();
    const u8* data16 = (const u8*)frame.data16.data();

    if (!fixed.mat[0])
      data64 += sizeof(math::Matrix4f);
    if (!fixed.mat[1])
      data64 += sizeof(math::Matrix4f);

    for (int tqi = 0; tqi < num_joints; tqi++) {
      int ctrl_idx = tqi / 8;
      int ctrl_shift = 4 * (tqi % 8);
      int c = 0xf & (hdr.control_bits[ctrl_idx] >> ctrl_shift);
      auto& joint = out.joints[2 + tqi];

      if (c & 0b0001) {
        math::Vector3f t;
        if (c & 0b1000) {
          t.x() = read_f32(data64) / 4096.f;
          t.y() = read_f32(data64) / 4096.f;
          t.z() = read_f32(data32) / 4096.f;
        } else {
          t.x() = read_s16(data32) * kTransScale;
          t.y() = read_s16(data32) * kTransScale;
          t.z() = read_s16(data16) * kTransScale;
        }
        joint.trans_frames.push_back(t);
      }

      if (c & 0b0010) {
        math::Vector4f q;
        q.x() = read_s16(data64) * kQuatScale;
        q.y() = read_s16(data64) * kQuatScale;
        q.z() = read_s16(data64) * kQuatScale;
        q.w() = read_s16(data64) * kQuatScale;
        joint.quat_frames.push_back(q);
      }

      if (c & 0b0100) {
        math::Vector3f s;
        s.x() = read_s16(data32) * kScaleScale;
        s.y() = read_s16(data32) * kScaleScale;
        s.z() = read_s16(data16) * kScaleScale;
        joint.scale_frames.push_back(s);
      }
    }
  }

  for (int ji = 2; ji < (int)out.joints.size(); ji++) {
    auto& joint = out.joints[ji];
    while ((int)joint.trans_frames.size() < total_frames) {
      if (joint.trans_frames.empty())
        joint.trans_frames.emplace_back(0.f, 0.f, 0.f);
      else
        joint.trans_frames.push_back(joint.trans_frames[0]);
    }
    while ((int)joint.quat_frames.size() < total_frames) {
      if (joint.quat_frames.empty())
        joint.quat_frames.emplace_back(0.f, 0.f, 0.f, 1.f);
      else
        joint.quat_frames.push_back(joint.quat_frames[0]);
    }
    while ((int)joint.scale_frames.size() < total_frames) {
      if (joint.scale_frames.empty())
        joint.scale_frames.emplace_back(1.f, 1.f, 1.f);
      else
        joint.scale_frames.push_back(joint.scale_frames[0]);
    }
  }

  out.blend_shape_data = art_anim.blend_shape_data;
  return out;
}

int make_anim_float_accessor(const std::vector<float>& values, tinygltf::Model& model) {
  int buf_idx = (int)model.buffers.size();
  auto& buf = model.buffers.emplace_back();
  buf.data.resize(values.size() * sizeof(float));
  memcpy(buf.data.data(), values.data(), buf.data.size());

  int bv_idx = (int)model.bufferViews.size();
  auto& bv = model.bufferViews.emplace_back();
  bv.buffer = buf_idx;
  bv.byteOffset = 0;
  bv.byteLength = buf.data.size();

  int acc_idx = (int)model.accessors.size();
  auto& acc = model.accessors.emplace_back();
  acc.bufferView = bv_idx;
  acc.byteOffset = 0;
  acc.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  acc.count = (int)values.size();
  acc.type = TINYGLTF_TYPE_SCALAR;
  if (!values.empty()) {
    float mn = values[0], mx = values[0];
    for (float v : values) {
      mn = std::min(mn, v);
      mx = std::max(mx, v);
    }
    acc.minValues = {(double)mn};
    acc.maxValues = {(double)mx};
  }
  return acc_idx;
}

int make_anim_vec3_accessor(const std::vector<math::Vector3f>& values, tinygltf::Model& model) {
  static_assert(sizeof(math::Vector3f) == 3 * sizeof(float));
  int buf_idx = (int)model.buffers.size();
  auto& buf = model.buffers.emplace_back();
  buf.data.resize(values.size() * sizeof(math::Vector3f));
  memcpy(buf.data.data(), values.data(), buf.data.size());

  int bv_idx = (int)model.bufferViews.size();
  auto& bv = model.bufferViews.emplace_back();
  bv.buffer = buf_idx;
  bv.byteOffset = 0;
  bv.byteLength = buf.data.size();

  int acc_idx = (int)model.accessors.size();
  auto& acc = model.accessors.emplace_back();
  acc.bufferView = bv_idx;
  acc.byteOffset = 0;
  acc.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  acc.count = (int)values.size();
  acc.type = TINYGLTF_TYPE_VEC3;
  return acc_idx;
}

int make_anim_vec4_accessor(const std::vector<math::Vector4f>& values, tinygltf::Model& model) {
  static_assert(sizeof(math::Vector4f) == 4 * sizeof(float));
  int buf_idx = (int)model.buffers.size();
  auto& buf = model.buffers.emplace_back();
  buf.data.resize(values.size() * sizeof(math::Vector4f));
  memcpy(buf.data.data(), values.data(), buf.data.size());

  int bv_idx = (int)model.bufferViews.size();
  auto& bv = model.bufferViews.emplace_back();
  bv.buffer = buf_idx;
  bv.byteOffset = 0;
  bv.byteLength = buf.data.size();

  int acc_idx = (int)model.accessors.size();
  auto& acc = model.accessors.emplace_back();
  acc.bufferView = bv_idx;
  acc.byteOffset = 0;
  acc.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  acc.count = (int)values.size();
  acc.type = TINYGLTF_TYPE_VEC4;
  return acc_idx;
}

void add_animation_to_gltf(const level_tools::UncompressedJointAnim& anim,
                           const tinygltf::Skin& skin,
                           tinygltf::Model& model,
                           int mesh_node_idx,
                           int num_targets) {
  if (anim.frames == 0 || anim.joints.size() <= 2)
    return;

  auto& gltf_anim = model.animations.emplace_back();
  gltf_anim.name = anim.name;

  std::vector<float> times(anim.frames);
  for (int i = 0; i < anim.frames; i++)
    times[i] = i / anim.framerate;
  int time_acc = make_anim_float_accessor(times, model);

  int n_anim_joints = (int)anim.joints.size();
  int n_skin_joints = (int)skin.joints.size();
  for (int ji = 2; ji < n_anim_joints && ji < n_skin_joints; ji++) {
    const auto& joint = anim.joints[ji];
    int target_node = skin.joints[ji];

    auto add_channel = [&](int val_acc, const std::string& path) {
      int si = (int)gltf_anim.samplers.size();
      auto& sampler = gltf_anim.samplers.emplace_back();
      sampler.input = time_acc;
      sampler.output = val_acc;
      sampler.interpolation = "LINEAR";
      auto& channel = gltf_anim.channels.emplace_back();
      channel.sampler = si;
      channel.target_node = target_node;
      channel.target_path = path;
    };

    if ((int)joint.trans_frames.size() == anim.frames)
      add_channel(make_anim_vec3_accessor(joint.trans_frames, model), "translation");
    if ((int)joint.quat_frames.size() == anim.frames)
      add_channel(make_anim_vec4_accessor(joint.quat_frames, model), "rotation");
    if ((int)joint.scale_frames.size() == anim.frames)
      add_channel(make_anim_vec3_accessor(joint.scale_frames, model), "scale");
  }

  if (num_targets > 0 && !anim.blend_shape_data.empty() &&
      (int)anim.blend_shape_data.size() >= anim.frames * num_targets) {
    std::vector<float> weights(anim.frames * num_targets);
    for (int fi = 0; fi < anim.frames; fi++) {
      for (int ti = 0; ti < num_targets; ti++) {
        u8 raw = anim.blend_shape_data[fi * num_targets + ti];
        weights[fi * num_targets + ti] = (raw - 64.f) / 128.f;
      }
    }
    int si = (int)gltf_anim.samplers.size();
    auto& sampler = gltf_anim.samplers.emplace_back();
    sampler.input = time_acc;
    sampler.output = make_anim_float_accessor(weights, model);
    sampler.interpolation = "LINEAR";
    auto& channel = gltf_anim.channels.emplace_back();
    channel.sampler = si;
    channel.target_node = mesh_node_idx;
    channel.target_path = "weights";
  }
}

int make_vec3_float_accessor(const std::vector<float>& data, tinygltf::Model& model) {
  int buffer_idx = (int)model.buffers.size();
  auto& buffer = model.buffers.emplace_back();
  buffer.data.resize(data.size() * sizeof(float));
  memcpy(buffer.data.data(), data.data(), buffer.data.size());

  int buffer_view_idx = (int)model.bufferViews.size();
  auto& bv = model.bufferViews.emplace_back();
  bv.buffer = buffer_idx;
  bv.byteOffset = 0;
  bv.byteLength = buffer.data.size();

  int accessor_idx = (int)model.accessors.size();
  auto& accessor = model.accessors.emplace_back();
  accessor.bufferView = buffer_view_idx;
  accessor.byteOffset = 0;
  accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  accessor.count = data.size() / 3;
  accessor.type = TINYGLTF_TYPE_VEC3;
  return accessor_idx;
}

void add_blerc_targets(const tfrag3::MercModel& mmodel,
                       const std::unordered_map<u32, u32>& local_vertex,
                       u32 num_vtx,
                       tinygltf::Mesh& mesh,
                       tinygltf::Model& model) {
  // find max target index across all effects to know how many morph targets we need
  u32 num_targets = 0;
  for (const auto& effect : mmodel.effects) {
    const auto& blerc = effect.mod.blerc;
    if (blerc.int_data.empty()) {
      continue;
    }
    bool skip_next = false;
    for (u32 v : blerc.int_data) {
      if (skip_next) {
        skip_next = false;
        continue;
      }
      if (v == tfrag3::Blerc::kTargetIdxTerminator) {
        skip_next = true;
      } else {
        num_targets = std::max(num_targets, v + 1);
      }
    }
  }
  if (num_targets == 0) {
    return;
  }

  std::vector pos_deltas(num_targets, std::vector(num_vtx * 3, 0.f));
  std::vector nrm_deltas(num_targets, std::vector(num_vtx * 3, 0.f));

  for (const auto& effect : mmodel.effects) {
    const auto& blerc = effect.mod.blerc;
    if (blerc.int_data.empty() || effect.mod.mod_to_global_vertex_idx.empty()) {
      continue;
    }

    size_t float_idx = 0;
    size_t int_idx = 0;
    while (int_idx < blerc.int_data.size()) {
      float_idx++;  // skip base pos/nrm entry

      // collect (target_idx, float_data_index) pairs for this vertex
      struct TargetEntry {
        u32 tgt_idx;
        size_t float_offset;
      };
      std::vector<TargetEntry> vertex_targets;
      while (blerc.int_data[int_idx] != tfrag3::Blerc::kTargetIdxTerminator) {
        vertex_targets.push_back({blerc.int_data[int_idx++], float_idx++});
      }
      int_idx++;
      u32 dest = blerc.int_data[int_idx++];

      if (dest >= effect.mod.mod_to_global_vertex_idx.size()) {
        continue;
      }
      const auto local = local_vertex.find(effect.mod.mod_to_global_vertex_idx[dest]);
      if (local == local_vertex.end()) {
        continue;  // a vertex no triangle of the model uses
      }
      const u32 global_idx = local->second;

      for (const auto& te : vertex_targets) {
        if (te.tgt_idx >= num_targets || te.float_offset >= blerc.float_data.size()) {
          continue;
        }
        const auto& fd = blerc.float_data[te.float_offset];
        pos_deltas[te.tgt_idx][global_idx * 3 + 0] = fd.v[0] * (8192.f / 4096.f);
        pos_deltas[te.tgt_idx][global_idx * 3 + 1] = fd.v[1] * (8192.f / 4096.f);
        pos_deltas[te.tgt_idx][global_idx * 3 + 2] = fd.v[2] * (8192.f / 4096.f);
        nrm_deltas[te.tgt_idx][global_idx * 3 + 0] = fd.v[4] * 8192.f;
        nrm_deltas[te.tgt_idx][global_idx * 3 + 1] = fd.v[5] * 8192.f;
        nrm_deltas[te.tgt_idx][global_idx * 3 + 2] = fd.v[6] * 8192.f;
      }
    }
  }

  // build one accessor pair per morph target and attach to all primitives
  std::vector<std::map<std::string, int>> morph_targets;
  for (u32 t = 0; t < num_targets; t++) {
    auto& target = morph_targets.emplace_back();
    target["POSITION"] = make_vec3_float_accessor(pos_deltas[t], model);
    target["NORMAL"] = make_vec3_float_accessor(nrm_deltas[t], model);
  }

  for (auto& prim : mesh.primitives) {
    prim.targets = morph_targets;
  }
  mesh.weights.assign(num_targets, 0.0);
}

void add_merc(const tfrag3::Level& level,
              const std::map<std::string, level_tools::ArtData>& art_data,
              const tfrag3::MercModel& mmodel,
              tinygltf::Model& model,
              TexImageMap& tex_image_map,
              const std::unordered_map<int, int>& anim_slot_to_base_tex) {
  // the model's triangles. The level's vertex buffer holds the vertices of all its models: the
  // file keeps those of this model only, renumbered (ogle-extract; a model file used to carry
  // its level's whole buffer, once more per blend shape)
  std::vector<std::vector<u32>> draw_to_start, draw_to_count;
  std::vector<u32> indices;
  unstrip_merc_draws(level.merc_data.indices, mmodel, level.merc_data.vertices, indices,
                     draw_to_start, draw_to_count);
  std::unordered_map<u32, u32> local_vertex;
  std::vector<tfrag3::MercVertex> mverts;
  for (auto& idx : indices) {
    const auto [it, added] = local_vertex.try_emplace(idx, (u32)mverts.size());
    if (added) {
      mverts.push_back(level.merc_data.vertices.at(idx));
    }
    idx = it->second;
  }
  if (mverts.empty() && !level.merc_data.vertices.empty()) {
    mverts.push_back(level.merc_data.vertices.front());  // accessors need one element
  }

  // create position and uv buffers
  int position_buffer_accessor = make_position_buffer_accessor(mverts, model);
  int texture_buffer_accessor = make_tex_buffer_accessor(mverts, model, 1.f);

  int index_buffer_view = make_merc_index_buffer_view(indices, model);
  int colors = make_color_buffer_accessor(mverts, model);

  auto joints_accessor = make_bones_accessor(mverts, model);
  auto weights_accessor = make_weights_accessor(mverts, model);
  auto normal_buffer_accessor = make_normal_buffer_accessor(mverts, model);

  const auto& art = art_data.find(mmodel.name);
  int node_idx = (int)model.nodes.size();
  auto& node = model.nodes.emplace_back();
  model.scenes.at(0).nodes.push_back(node_idx);
  node.name = mmodel.name;
  int mesh_idx = (int)model.meshes.size();
  auto& mesh = model.meshes.emplace_back();
  mesh.name = node.name;
  node.mesh = mesh_idx;

  if (art != art_data.end() && !art->second.joint_group.empty()) {
    node.skin = model.skins.size();
    auto& skin = model.skins.emplace_back();
    const auto& game_bones = art->second.joint_group;
    int n_bones = game_bones.size();
    std::vector<std::vector<int>> children(n_bones);
    for (size_t i = 0; i < game_bones.size(); i++) {
      if (game_bones[i].parent_idx >= 0) {
        children.at(game_bones[i].parent_idx).push_back(i);
      }
    }
    skin.skeleton = model.nodes.size();
    for (int i = 0; i < n_bones; i++) {
      const auto& gbone = game_bones[i];
      skin.joints.push_back(skin.skeleton + i);
      auto& snode = model.nodes.emplace_back();
      snode.name = gbone.name;

      // bind pose is bind_T_w
      // for glb we want bind_parent_T_bind_child
      // so bindp_T_w * inverse(bindc_T_w)
      math::Matrix4f matrix;
      if (gbone.parent_idx >= 0) {
        matrix = unscale_translation(game_bones.at(gbone.parent_idx).bind_pose_T_w) *
                 inverse(unscale_translation(gbone.bind_pose_T_w));

      } else {
        // I think this value is ignored anyway.
        for (int r = 0; r < 4; r++) {
          for (int c = 0; c < 4; c++) {
            matrix(r, c) = (r == c) ? 1 : 0;
          }
        }
      }

      for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
          snode.matrix.push_back(matrix(c, r));
        }
      }
      for (auto child : children.at(i)) {
        snode.children.push_back(skin.skeleton + child);
      }
    }
    ASSERT(skin.skeleton + n_bones == (int)model.nodes.size());
    skin.inverseBindMatrices = make_inv_matrix_bind_poses(game_bones, model);

    for (int i = 0; i < n_bones; i++) {
      if (game_bones[i].parent_idx < 0) {
        model.scenes.at(0).nodes.push_back(skin.skeleton + i);
      }
    }
  }

  u32 num_blend_targets = 0;
  for (const auto& effect : mmodel.effects) {
    bool skip_next = false;
    for (u32 v : effect.mod.blerc.int_data) {
      if (skip_next) {
        skip_next = false;
        continue;
      }
      if (v == tfrag3::Blerc::kTargetIdxTerminator) {
        skip_next = true;
      } else {
        num_blend_targets = std::max(num_blend_targets, v + 1);
      }
    }
  }

  // when we have animated blend targets, blender adds an extra empty during import,
  // rename it to not conflict with the actual model
  if (num_blend_targets > 0) {
    model.nodes[node_idx].name = mmodel.name + "_blerc";
  }

  if (art != art_data.end() && !art->second.anims.empty() && node.skin >= 0 &&
      node.skin < model.skins.size()) {
    const auto& skin = model.skins[node.skin];
    for (const auto& ja : art->second.anims) {
      auto uncompressed = decompress_anim(ja);
      add_animation_to_gltf(uncompressed, skin, model, node_idx, (int)num_blend_targets);
    }
  }

  for (size_t effect_idx = 0; effect_idx < mmodel.effects.size(); effect_idx++) {
    const auto& effect = mmodel.effects[effect_idx];

    EnvmapInfo envmap_info;
    const EnvmapInfo* envmap = nullptr;
    if (effect.has_envmap && effect.envmap_texture < level.textures.size()) {
      envmap_info.texture_idx = (int)effect.envmap_texture;
      envmap_info.mode = effect.envmap_mode;
      envmap = &envmap_info;
    }

    for (size_t draw_idx = 0; draw_idx < effect.all_draws.size(); draw_idx++) {
      const auto& draw = effect.all_draws[draw_idx];
      auto& prim = mesh.primitives.emplace_back();
      // resolve texture animation draws to their base texture
      int tex_id = draw.tree_tex_id;
      if (tex_id < 0) {
        const auto it = anim_slot_to_base_tex.find(-(tex_id + 1));
        tex_id = it != anim_slot_to_base_tex.end() ? it->second : draw.tree_tex_id;
      }
      prim.material = add_material_for_tex(level, model, tex_id, tex_image_map, draw.mode, envmap);
      prim.indices =
          make_index_buffer_accessor(model, draw_to_start[effect_idx][draw_idx],
                                     draw_to_count[effect_idx][draw_idx], index_buffer_view);
      prim.attributes["POSITION"] = position_buffer_accessor;
      prim.attributes["TEXCOORD_0"] = texture_buffer_accessor;
      prim.attributes["COLOR_0"] = colors;
      prim.attributes["JOINTS_0"] = joints_accessor;
      prim.attributes["WEIGHTS_0"] = weights_accessor;
      prim.attributes["NORMAL"] = normal_buffer_accessor;
      prim.mode = TINYGLTF_MODE_TRIANGLES;
    }
  }

  add_blerc_targets(mmodel, local_vertex, (u32)mverts.size(), mesh, model);
}

void consolidate_buffers(tinygltf::Model& model) {
  if (model.buffers.size() <= 1) {
    return;
  }

  std::vector<size_t> buffer_start(model.buffers.size());
  std::vector<u8> merged;
  for (size_t i = 0; i < model.buffers.size(); i++) {
    merged.resize((merged.size() + 3) & ~(size_t)3);
    buffer_start[i] = merged.size();
    const auto& data = model.buffers[i].data;
    merged.insert(merged.end(), data.begin(), data.end());
  }

  for (auto& view : model.bufferViews) {
    view.byteOffset += buffer_start.at(view.buffer);
    view.buffer = 0;
  }

  model.buffers.clear();
  model.buffers.emplace_back().data = std::move(merged);
}
/*!
 * A json value (actor data) as a tinygltf value, for node extras.
 */
tinygltf::Value to_gltf_value(const nlohmann::json& j) {
  if (j.is_object()) {
    tinygltf::Value::Object o;
    for (const auto& [key, value] : j.items()) {
      o[key] = to_gltf_value(value);
    }
    return tinygltf::Value(std::move(o));
  }
  if (j.is_array()) {
    tinygltf::Value::Array a;
    for (const auto& value : j) {
      a.push_back(to_gltf_value(value));
    }
    return tinygltf::Value(std::move(a));
  }
  if (j.is_boolean()) {
    return tinygltf::Value(j.get<bool>());
  }
  if (j.is_number_integer()) {
    s64 v = j.get<s64>();
    if (v >= INT32_MIN && v <= INT32_MAX) {
      return tinygltf::Value((int)v);
    }
    return tinygltf::Value((double)v);
  }
  if (j.is_number()) {
    return tinygltf::Value(j.get<double>());
  }
  if (j.is_string()) {
    return tinygltf::Value(j.get<std::string>());
  }
  return tinygltf::Value();
}

/*!
 * Set up a buffer for the normals of collision vertices. Return the index of the accessor.
 */
int make_collision_normal_accessor(const std::vector<tfrag3::CollisionMesh::Vertex>& vertices,
                                   tinygltf::Model& model) {
  int buffer_idx = (int)model.buffers.size();
  auto& buffer = model.buffers.emplace_back();
  buffer.data.resize(sizeof(float) * 3 * vertices.size());
  u8* buffer_ptr = buffer.data.data();
  for (const auto& vtx : vertices) {
    float n[3] = {vtx.nx / 32767.f, vtx.ny / 32767.f, vtx.nz / 32767.f};
    memcpy(buffer_ptr, n, 3 * sizeof(float));
    buffer_ptr += 3 * sizeof(float);
  }
  int buffer_view_idx = (int)model.bufferViews.size();
  auto& buffer_view = model.bufferViews.emplace_back();
  buffer_view.buffer = buffer_idx;
  buffer_view.byteOffset = 0;
  buffer_view.byteLength = buffer.data.size();
  buffer_view.byteStride = 0;
  buffer_view.target = TINYGLTF_TARGET_ARRAY_BUFFER;
  int accessor_idx = (int)model.accessors.size();
  auto& accessor = model.accessors.emplace_back();
  accessor.bufferView = buffer_view_idx;
  accessor.byteOffset = 0;
  accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  accessor.count = vertices.size();
  accessor.type = TINYGLTF_TYPE_VEC3;
  return accessor_idx;
}

/*!
 * Set up an index buffer 0, 1, 2... count - 1 (a triangle list). Return the index of the accessor.
 */
int make_sequential_index_accessor(size_t count, tinygltf::Model& model) {
  int buffer_idx = (int)model.buffers.size();
  auto& buffer = model.buffers.emplace_back();
  buffer.data.resize(sizeof(u32) * count);
  for (u32 i = 0; i < count; i++) {
    memcpy(buffer.data.data() + i * sizeof(u32), &i, sizeof(u32));
  }
  int buffer_view_idx = (int)model.bufferViews.size();
  auto& buffer_view = model.bufferViews.emplace_back();
  buffer_view.buffer = buffer_idx;
  buffer_view.byteOffset = 0;
  buffer_view.byteLength = buffer.data.size();
  buffer_view.byteStride = 0;
  buffer_view.target = TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER;
  int accessor_idx = (int)model.accessors.size();
  auto& accessor = model.accessors.emplace_back();
  accessor.bufferView = buffer_view_idx;
  accessor.byteOffset = 0;
  accessor.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
  accessor.count = count;
  accessor.type = TINYGLTF_TYPE_SCALAR;
  return accessor_idx;
}

/*!
 * Add the level's collision: one node per surface value (pat), with the extras of the level
 * builder's collision (set_collision, set_invisible) and the exact pat. The builder's own fields
 * (collide_mode, collide_material...) use Jak 1's layout: they are only written for Jak 1.
 */
void add_collision(const tfrag3::Level& level, const std::string& game, tinygltf::Model& model) {
  const auto& verts = level.collision.vertices;
  if (verts.size() < 3) {
    return;
  }
  std::map<u32, std::vector<tfrag3::CollisionMesh::Vertex>> by_pat;
  for (size_t i = 0; i + 2 < verts.size(); i += 3) {
    auto& list = by_pat[verts[i].pat];
    for (int k = 0; k < 3; k++) {
      list.push_back(verts[i + k]);
    }
  }

  int root_idx = (int)model.nodes.size();
  model.nodes.emplace_back().name = fmt::format("{}-collision", level.level_name);
  model.scenes.at(0).nodes.push_back(root_idx);

  for (const auto& [pat, tri_verts] : by_pat) {
    int mesh_idx = (int)model.meshes.size();
    auto& mesh = model.meshes.emplace_back();
    mesh.name = fmt::format("{}-collision-{:08x}", level.level_name, pat);
    auto& prim = mesh.primitives.emplace_back();
    prim.attributes["POSITION"] = make_position_buffer_accessor(tri_verts, model);
    prim.attributes["NORMAL"] = make_collision_normal_accessor(tri_verts, model);
    prim.indices = make_sequential_index_accessor(tri_verts.size(), model);
    prim.mode = TINYGLTF_MODE_TRIANGLES;

    tinygltf::Value::Object extras;
    extras["set_collision"] = tinygltf::Value(1);
    extras["set_invisible"] = tinygltf::Value(1);
    extras["collide_pat"] = tinygltf::Value((int)pat);
    extras["collide_game"] = tinygltf::Value(game);
    if (game == "jak1") {
      // goal_src/jak1/engine/collide/pat-h.gc
      extras["noentity"] = tinygltf::Value((int)(pat & 1));
      extras["nocamera"] = tinygltf::Value((int)((pat >> 1) & 1));
      extras["noedge"] = tinygltf::Value((int)((pat >> 2) & 1));
      extras["collide_mode"] = tinygltf::Value((int)((pat >> 3) & 7));
      extras["collide_material"] = tinygltf::Value((int)((pat >> 6) & 63));
      extras["nolineofsight"] = tinygltf::Value((int)((pat >> 12) & 1));
      extras["collide_event"] = tinygltf::Value((int)((pat >> 14) & 63));
    }

    int node_idx = (int)model.nodes.size();
    auto& node = model.nodes.emplace_back();
    node.name = mesh.name;
    node.mesh = mesh_idx;
    node.extras = tinygltf::Value(std::move(extras));
    model.nodes[root_idx].children.push_back(node_idx);
  }
}

/*!
 * Add the level's actors: one empty node each, at its position and rotation, with "kind": "actor"
 * and the rest of its <level>-actors.json entry in the extras.
 */
void add_actors(const tfrag3::Level& level, const nlohmann::json& actors, tinygltf::Model& model) {
  if (!actors.is_array() || actors.empty()) {
    return;
  }
  int root_idx = (int)model.nodes.size();
  model.nodes.emplace_back().name = fmt::format("{}-actors", level.level_name);
  model.scenes.at(0).nodes.push_back(root_idx);

  int index = 0;
  for (const auto& actor : actors) {
    nlohmann::json extras = actor;
    extras.erase("trans");
    extras.erase("quat");
    extras["kind"] = "actor";

    int node_idx = (int)model.nodes.size();
    auto& node = model.nodes.emplace_back();
    const auto& lump = actor.value("lump", nlohmann::json::object());
    if (lump.is_object() && lump.contains("name") && lump["name"].is_string()) {
      node.name = lump["name"].get<std::string>();
    } else {
      node.name = fmt::format("{}-{}", actor.value("etype", std::string("actor")), index);
    }
    const auto& trans = actor.value("trans", nlohmann::json::array());
    if (trans.is_array() && trans.size() >= 3) {
      node.translation = {trans[0].get<double>(), trans[1].get<double>(), trans[2].get<double>()};
    }
    const auto& quat = actor.value("quat", nlohmann::json::array());
    if (quat.is_array() && quat.size() == 4) {
      node.rotation = {quat[0].get<double>(), quat[1].get<double>(), quat[2].get<double>(),
                       quat[3].get<double>()};
    }
    node.extras = to_gltf_value(extras);
    model.nodes[root_idx].children.push_back(node_idx);
    index++;
  }
}

/*!
 * A float vertex attribute in a buffer of its own. Return the index of the accessor.
 */
int add_float_accessor(tinygltf::Model& model, const std::vector<float>& data, int components) {
  int buffer_idx = (int)model.buffers.size();
  auto& buffer = model.buffers.emplace_back();
  buffer.data.resize(sizeof(float) * data.size());
  memcpy(buffer.data.data(), data.data(), buffer.data.size());
  int buffer_view_idx = (int)model.bufferViews.size();
  auto& buffer_view = model.bufferViews.emplace_back();
  buffer_view.buffer = buffer_idx;
  buffer_view.byteOffset = 0;
  buffer_view.byteLength = buffer.data.size();
  buffer_view.byteStride = 0;
  buffer_view.target = TINYGLTF_TARGET_ARRAY_BUFFER;
  int accessor_idx = (int)model.accessors.size();
  auto& accessor = model.accessors.emplace_back();
  accessor.bufferView = buffer_view_idx;
  accessor.byteOffset = 0;
  accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  accessor.count = data.size() / components;
  accessor.type = components == 2 ? TINYGLTF_TYPE_VEC2 : components == 3 ? TINYGLTF_TYPE_VEC3 : TINYGLTF_TYPE_VEC4;
  return accessor_idx;
}

/*!
 * A triangle list index buffer of its own. Return the index of the accessor.
 */
int add_index_accessor(tinygltf::Model& model, const std::vector<u32>& indices) {
  int buffer_idx = (int)model.buffers.size();
  auto& buffer = model.buffers.emplace_back();
  buffer.data.resize(sizeof(u32) * indices.size());
  memcpy(buffer.data.data(), indices.data(), buffer.data.size());
  int buffer_view_idx = (int)model.bufferViews.size();
  auto& buffer_view = model.bufferViews.emplace_back();
  buffer_view.buffer = buffer_idx;
  buffer_view.byteOffset = 0;
  buffer_view.byteLength = buffer.data.size();
  buffer_view.byteStride = 0;
  buffer_view.target = TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER;
  int accessor_idx = (int)model.accessors.size();
  auto& accessor = model.accessors.emplace_back();
  accessor.bufferView = buffer_view_idx;
  accessor.byteOffset = 0;
  accessor.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
  accessor.count = indices.size();
  accessor.type = TINYGLTF_TYPE_SCALAR;
  return accessor_idx;
}

/*!
 * An instance matrix of the fr3 data (columns, translation in game units) as a glTF node matrix.
 */
std::vector<double> instance_matrix(const std::array<math::Vector4f, 4>& mat) {
  std::vector<double> out;
  for (int c = 0; c < 4; c++) {
    for (int r = 0; r < 3; r++) {
      out.push_back(c == 3 ? mat[c][r] / 4096. : mat[c][r]);
    }
    out.push_back(c == 3 ? 1. : 0.);
  }
  return out;
}

/*!
 * One instance of a TIE or shrub prototype: the matrix it is drawn with, and its triangles as
 * indices of the prototype's packed vertices (the same for every instance of a prototype when
 * the packed data shares them), per material.
 */
struct ProtoInstance {
  s32 matrix_idx = -1;                        // -1: the vertices are already in world space
  std::map<int, std::vector<u32>> tris;       // material -> 3 packed vertex indices per triangle
  std::unordered_map<u32, u32> unpacked_of;   // packed vertex -> one unpacked vertex (colors)
};

/*!
 * Write a prototype: a group node named after it under `parent`, one mesh per distinct geometry
 * (in the prototype's space), and one node per instance with its matrix. The instances of a
 * prototype share its mesh: an editor selects and moves each one.
 */
template <typename VertexFn, typename ColorFn>
void emit_instanced_proto(tinygltf::Model& model,
                          int parent,
                          const std::string& name,
                          const std::vector<ProtoInstance>& instances,
                          const std::vector<std::array<math::Vector4f, 4>>& matrices,
                          VertexFn&& packed_vertex,  // (u32 packed, float pos[3], float st[2])
                          ColorFn&& unpacked_color) {  // (u32 unpacked, float rgba[4])
  int group_idx = (int)model.nodes.size();
  model.nodes.emplace_back().name = name;
  model.nodes[parent].children.push_back(group_idx);

  std::map<std::map<int, std::vector<u32>>, int> mesh_of_geometry;
  int count = 0;
  for (const auto& inst : instances) {
    auto found = mesh_of_geometry.find(inst.tris);
    int mesh_idx;
    if (found != mesh_of_geometry.end()) {
      mesh_idx = found->second;
    } else {
      // the vertices this geometry uses, renumbered
      std::map<u32, u32> local;
      for (const auto& [mat, tris] : inst.tris) {
        for (u32 v : tris) {
          local.emplace(v, 0);
        }
      }
      u32 next = 0;
      for (auto& [v, idx] : local) {
        idx = next++;
      }
      std::vector<float> pos, st, colors;
      for (const auto& [v, idx] : local) {
        float p[3], t[2], c[4];
        packed_vertex(v, p, t);
        pos.insert(pos.end(), {p[0], p[1], p[2]});
        st.insert(st.end(), {t[0], t[1]});
        auto u = inst.unpacked_of.find(v);
        if (u != inst.unpacked_of.end()) {
          unpacked_color(u->second, c);
        } else {
          c[0] = c[1] = c[2] = c[3] = 1.f;
        }
        colors.insert(colors.end(), {c[0], c[1], c[2], c[3]});
      }
      int pos_acc = add_float_accessor(model, pos, 3);
      int st_acc = add_float_accessor(model, st, 2);
      int col_acc = add_float_accessor(model, colors, 4);
      mesh_idx = (int)model.meshes.size();
      model.meshes.emplace_back().name = name;
      for (const auto& [mat, tris] : inst.tris) {
        std::vector<u32> indices;
        indices.reserve(tris.size());
        for (u32 v : tris) {
          indices.push_back(local.at(v));
        }
        auto& prim = model.meshes[mesh_idx].primitives.emplace_back();
        prim.material = mat;
        prim.indices = add_index_accessor(model, indices);
        prim.attributes["POSITION"] = pos_acc;
        prim.attributes["TEXCOORD_0"] = st_acc;
        prim.attributes["COLOR_0"] = col_acc;
        prim.mode = TINYGLTF_MODE_TRIANGLES;
      }
      mesh_of_geometry.emplace(inst.tris, mesh_idx);
    }
    int node_idx = (int)model.nodes.size();
    auto& node = model.nodes.emplace_back();
    node.name = fmt::format("{}-{}", name, count++);
    node.mesh = mesh_idx;
    if (inst.matrix_idx >= 0 && inst.matrix_idx < (int)matrices.size()) {
      node.matrix = instance_matrix(matrices[inst.matrix_idx]);
    }
    model.nodes[group_idx].children.push_back(node_idx);
  }
}

/*!
 * Add a TIE tree with one node per instance: under "<level>-tie-<n>", a group per prototype
 * holding a node per instance, and a group of the wind instances (swaying foliage).
 */
void add_tie_instanced(const tfrag3::Level& level,
                       const tfrag3::TieTree& tie_in,
                       size_t tree_idx,
                       tinygltf::Model& model,
                       TexImageMap& tex_image_map) {
  tfrag3::TieTree tie = tie_in;
  tie.unpack();

  int node_idx = (int)model.nodes.size();
  model.nodes.emplace_back().name = fmt::format("{}-tie-{}", level.level_name, tree_idx);
  model.scenes.at(0).nodes.push_back(node_idx);

  // unpacked vertex -> the matrix group it was unpacked by (tie.unpack() walks them in order)
  const auto& groups = tie.packed_vertices.matrix_groups;
  std::vector<u32> group_of(tie.unpacked.vertices.size(), 0);
  std::vector<u32> group_first(groups.size(), 0);
  {
    u32 i = 0;
    for (u32 g = 0; g < groups.size(); g++) {
      group_first[g] = i;
      for (u32 v = groups[g].start_vert; v < groups[g].end_vert && i < group_of.size(); v++) {
        group_of[i++] = g;
      }
    }
  }

  // materials of the static draws, like add_tie()
  const auto envmap_by_run_start = tie_envmap_by_run_start(tie);
  std::vector<u32> draws_to_emit;
  std::map<u32, int> draw_material;
  for (int cat = 0; cat < tfrag3::kNumTieCategories; cat++) {
    const auto category = (tfrag3::TieCategory)cat;
    if (tfrag3::is_envmap_second_draw_category(category)) {
      continue;
    }
    for (u32 draw_idx = tie.category_draw_indices[cat];
         draw_idx < tie.category_draw_indices[cat + 1]; draw_idx++) {
      const auto& draw = tie.static_draws[draw_idx];
      EnvmapInfo envmap_info;
      const EnvmapInfo* envmap = nullptr;
      if (tfrag3::is_envmap_first_draw_category(category) && !draw.runs.empty()) {
        const auto& first_run = draw.runs.front();
        const auto it = envmap_by_run_start.find(first_run.vertex0 + first_run.length);
        if (it != envmap_by_run_start.end() && it->second.texture_idx >= 0 &&
            (size_t)it->second.texture_idx < level.textures.size()) {
          envmap_info = it->second;
          envmap = &envmap_info;
        }
      }
      draws_to_emit.push_back(draw_idx);
      draw_material[draw_idx] =
          add_material_for_tex(level, model, draw.tree_tex_id, tex_image_map, draw.mode, envmap);
    }
  }

  // triangles, per prototype and instance (the instance is the matrix of its vertices)
  std::vector<u32> unstripped, old_to_new;
  unstrip_tfrag_tie(tie.unpacked.indices, extract_positions(tie.unpacked.vertices), unstripped,
                    old_to_new);
  std::map<u16, std::map<s64, ProtoInstance>> protos;
  for (u32 draw_idx : draws_to_emit) {
    const auto& draw = tie.static_draws.at(draw_idx);
    const int mat = draw_material.at(draw_idx);
    u32 old_start = draw.unpacked.idx_of_first_idx_in_full_buffer;
    for (const auto& grp : draw.vis_groups) {
      if (grp.num_tris) {
        u32 first = old_to_new.at(old_start);
        for (u32 t = 0; t < grp.num_tris; t++) {
          u32 tri[3];
          for (int k = 0; k < 3; k++) {
            tri[k] = unstripped.at(first + t * 3 + k);
          }
          const u32 g = group_of.at(tri[0]);
          // world space groups are one instance each, the others one per matrix
          const s64 key = groups[g].matrix_idx >= 0 ? groups[g].matrix_idx : -1 - (s64)g;
          auto& inst = protos[grp.tie_proto_idx][key];
          inst.matrix_idx = groups[g].matrix_idx;
          auto& list = inst.tris[mat];
          for (int k = 0; k < 3; k++) {
            const u32 gk = group_of.at(tri[k]);
            const u32 packed = groups[gk].start_vert + (tri[k] - group_first[gk]);
            list.push_back(packed);
            inst.unpacked_of.emplace(packed, tri[k]);
          }
        }
      }
      old_start += grp.num_inds;
    }
  }

  for (const auto& [proto_idx, by_instance] : protos) {
    std::vector<ProtoInstance> instances;
    for (const auto& [key, inst] : by_instance) {
      instances.push_back(inst);
    }
    emit_instanced_proto(
        model, node_idx, proto_name(tie.proto_names, proto_idx, "tie"), instances,
        tie.packed_vertices.matrices,
        [&](u32 packed, float* pos, float* st) {
          const auto& v = tie.packed_vertices.vertices.at(packed);
          pos[0] = v.x / 4096.f;
          pos[1] = v.y / 4096.f;
          pos[2] = v.z / 4096.f;
          st[0] = v.s;
          st[1] = v.t;
        },
        [&](u32 unpacked, float* rgba) {
          const auto& v = tie.unpacked.vertices.at(unpacked);
          for (int j = 0; j < 3; j++) {
            rgba[j] = lit_color(tie.colors, v.color_index, j) / 255.f;
          }
          rgba[3] = 1.f;
        });
  }

  // wind instances: one node each, its primitives on the tree's vertex arrays
  if (!tie.instanced_wind_draws.empty()) {
    int position_buffer_accessor = make_position_buffer_accessor(tie.unpacked.vertices, model);
    int texture_buffer_accessor = make_tex_buffer_accessor(tie.unpacked.vertices, model, 1.f);
    int color_accessor = make_color_buffer_accessor(tie.unpacked.vertices, model, tie, 0);
    std::vector<std::vector<u32>> draw_to_starts, draw_to_counts;
    int wind_index_buffer_view = make_tie_wind_index_buffer_view(tie.instanced_wind_draws, model,
                                                                 draw_to_starts, draw_to_counts);
    std::map<u32, int> mesh_of_instance;
    int wind_group = (int)model.nodes.size();
    model.nodes.emplace_back().name = fmt::format("{}-tie-{}-wind", level.level_name, tree_idx);
    model.nodes[node_idx].children.push_back(wind_group);
    for (size_t draw_idx = 0; draw_idx < tie.instanced_wind_draws.size(); draw_idx++) {
      const auto& wind_draw = tie.instanced_wind_draws[draw_idx];
      int mat =
          add_material_for_tex(level, model, wind_draw.tree_tex_id, tex_image_map, wind_draw.mode);
      for (size_t grp_idx = 0; grp_idx < wind_draw.instance_groups.size(); grp_idx++) {
        const auto& grp = wind_draw.instance_groups[grp_idx];
        auto it = mesh_of_instance.find(grp.instance_idx);
        if (it == mesh_of_instance.end()) {
          int mesh_idx = (int)model.meshes.size();
          model.meshes.emplace_back().name = fmt::format("wind-{}", grp.instance_idx);
          it = mesh_of_instance.emplace(grp.instance_idx, mesh_idx).first;
          int c_node_idx = (int)model.nodes.size();
          auto& c_node = model.nodes.emplace_back();
          c_node.name = fmt::format("wind-{}", grp.instance_idx);
          c_node.mesh = mesh_idx;
          const auto& info = tie.wind_instance_info.at(grp.instance_idx);
          c_node.matrix = instance_matrix(info.matrix);
          model.nodes[wind_group].children.push_back(c_node_idx);
        }
        auto& prim = model.meshes[it->second].primitives.emplace_back();
        prim.material = mat;
        prim.indices = make_index_buffer_accessor(model, draw_to_starts.at(draw_idx).at(grp_idx),
                                                  draw_to_counts.at(draw_idx).at(grp_idx),
                                                  wind_index_buffer_view);
        prim.attributes["POSITION"] = position_buffer_accessor;
        prim.attributes["TEXCOORD_0"] = texture_buffer_accessor;
        prim.attributes["COLOR_0"] = color_accessor;
        prim.mode = TINYGLTF_MODE_TRIANGLES;
      }
    }
  }
}

/*!
 * Add a shrub tree with one node per instance: under "<level>-shrub-<n>", a group per prototype
 * holding a node per instance.
 */
void add_shrub_instanced(const tfrag3::Level& level,
                         const tfrag3::ShrubTree& shrub_in,
                         size_t tree_idx,
                         tinygltf::Model& model,
                         TexImageMap& tex_image_map) {
  tfrag3::ShrubTree shrub = shrub_in;
  shrub.unpack();

  int node_idx = (int)model.nodes.size();
  model.nodes.emplace_back().name = fmt::format("{}-shrub-{}", level.level_name, tree_idx);
  model.scenes.at(0).nodes.push_back(node_idx);

  // unpacked vertex -> the instance group it was unpacked by (shrub.unpack() walks them in order)
  const auto& groups = shrub.packed_vertices.instance_groups;
  std::vector<u32> group_of(shrub.unpacked.vertices.size(), 0);
  std::vector<u32> group_first(groups.size(), 0);
  {
    u32 i = 0;
    for (u32 g = 0; g < groups.size(); g++) {
      group_first[g] = i;
      for (u32 v = groups[g].start_vert; v < groups[g].end_vert && i < group_of.size(); v++) {
        group_of[i++] = g;
      }
    }
  }

  std::vector<u32> unstripped, draw_to_start, draw_to_count;
  unstrip_shrub_draws(shrub.indices, unstripped, draw_to_start, draw_to_count, shrub.static_draws);
  std::map<u16, std::map<s64, ProtoInstance>> protos;
  for (size_t draw_idx = 0; draw_idx < shrub.static_draws.size(); draw_idx++) {
    const auto& draw = shrub.static_draws[draw_idx];
    const int mat = add_material_for_tex(level, model, draw.tree_tex_id, tex_image_map, draw.mode);
    const u32 start = draw_to_start.at(draw_idx), count = draw_to_count.at(draw_idx);
    for (u32 t = 0; t + 2 < count; t += 3) {
      const u32 g = group_of.at(unstripped.at(start + t));
      const s64 key = groups[g].matrix_idx;
      auto& inst = protos[draw.proto_idx][key];
      inst.matrix_idx = groups[g].matrix_idx;
      auto& list = inst.tris[mat];
      for (int k = 0; k < 3; k++) {
        const u32 u = unstripped.at(start + t + k);
        const u32 gk = group_of.at(u);
        const u32 packed = groups[gk].start_vert + (u - group_first[gk]);
        list.push_back(packed);
        inst.unpacked_of.emplace(packed, u);
      }
    }
  }

  for (const auto& [proto_idx, by_instance] : protos) {
    std::vector<ProtoInstance> instances;
    for (const auto& [key, inst] : by_instance) {
      instances.push_back(inst);
    }
    emit_instanced_proto(
        model, node_idx, proto_name(shrub.proto_names, proto_idx, "shrub"), instances,
        shrub.packed_vertices.matrices,
        [&](u32 packed, float* pos, float* st) {
          const auto& v = shrub.packed_vertices.vertices.at(packed);
          pos[0] = v.x / 4096.f;
          pos[1] = v.y / 4096.f;
          pos[2] = v.z / 4096.f;
          st[0] = v.s / 4096.f;
          st[1] = v.t / 4096.f;
        },
        [&](u32 unpacked, float* rgba) {
          const auto& v = shrub.unpacked.vertices.at(unpacked);
          for (int j = 0; j < 3; j++) {
            rgba[j] = lit_color(shrub.time_of_day_colors, v.color_index, j) / 255.f;
          }
          rgba[3] = 1.f;
        });
  }
}

}  // namespace

/*!
 * Export the background geometry (tie, tfrag, shrub) to a GLTF binary format (.glb) file.
 * With `entities`, the level's collision and actors too (see LevelEntities).
 */
void save_level_background_as_gltf(const tfrag3::Level& level,
                                   const fs::path& glb_file,
                                   const LevelEntities* entities) {
  // the top level container for everything is the model.
  tinygltf::Model model;

  // a "scene" is a traditional scene graph, made up of Nodes.
  // sadly, attempting to nest stuff makes the blender importer unhappy, so we just dump
  // everything into the top level.
  model.scenes.emplace_back();

  // hack, add a default material.
  tinygltf::Material mat;
  mat.pbrMetallicRoughness.baseColorFactor = {1.0f, 0.9f, 0.9f, 1.0f};
  mat.pbrMetallicRoughness.metallicFactor = 0.0;
  mat.doubleSided = true;
  model.materials.push_back(mat);

  TexImageMap tex_image_map;

  // add all hi-lod tfrag trees
  for (const auto& tfrag : level.tfrag_trees.at(0)) {
    add_tfrag(level, tfrag, model, tex_image_map);
  }

  // with entities (for editors), the TIE and shrub prototypes are written once in their own space
  // and each instance is a node with its matrix
  for (size_t i = 0; i < level.tie_trees.at(0).size(); i++) {
    if (entities) {
      add_tie_instanced(level, level.tie_trees.at(0)[i], i, model, tex_image_map);
    } else {
      add_tie(level, level.tie_trees.at(0)[i], i, model, tex_image_map);
    }
  }

  for (size_t i = 0; i < level.shrub_trees.size(); i++) {
    if (entities) {
      add_shrub_instanced(level, level.shrub_trees[i], i, model, tex_image_map);
    } else {
      add_shrub(level, level.shrub_trees[i], i, model, tex_image_map);
    }
  }

  if (entities) {
    if (entities->collision) {
      add_collision(level, entities->game, model);
    }
    add_actors(level, entities->actors, model);
    tinygltf::Value::Object info;
    info["game"] = tinygltf::Value(entities->game);
    info["level"] = tinygltf::Value(level.level_name);
    tinygltf::Value::Object asset_extras;
    asset_extras["opengoal"] = tinygltf::Value(std::move(info));
    model.asset.extras = tinygltf::Value(std::move(asset_extras));
  }

  model.asset.generator = "opengoal";
  consolidate_buffers(model);
  tinygltf::TinyGLTF gltf;
  gltf.WriteGltfSceneToFile(&model, glb_file.string(),
                            true,   // embedImages
                            true,   // embedBuffers
                            true,   // pretty print
                            true);  // write binary
}

void save_level_foreground_as_gltf(
    const tfrag3::Level& level,
    const std::map<std::string, level_tools::ArtData>& art_data,
    const fs::path& glb_path,
    const std::unordered_map<std::string, u32>& animated_tex_output_to_anim_slot) {
  // map animated texture slots back to the base texture
  std::unordered_map<int, int> anim_slot_to_base_tex;
  for (int i = 0; i < (int)level.textures.size(); i++) {
    const auto it = animated_tex_output_to_anim_slot.find(level.textures[i].debug_name);
    if (it != animated_tex_output_to_anim_slot.end()) {
      anim_slot_to_base_tex[(int)it->second] = i;
    }
  }

  for (size_t model_idx = 0; model_idx < level.merc_data.models.size(); model_idx++) {
    const auto& mmodel = level.merc_data.models[model_idx];

    // the top level container for everything is the model.
    tinygltf::Model model;

    // a "scene" is a traditional scene graph, made up of Nodes.
    // sadly, attempting to nest stuff makes the blender importer unhappy, so we just dump
    // everything into the top level.
    model.scenes.emplace_back();

    // hack, add a default material.
    tinygltf::Material mat;
    mat.pbrMetallicRoughness.baseColorFactor = {1.0f, 0.9f, 0.9f, 1.0f};
    mat.pbrMetallicRoughness.metallicFactor = 0.0;
    mat.doubleSided = true;
    model.materials.push_back(mat);

    TexImageMap tex_image_map;

    add_merc(level, art_data, mmodel, model, tex_image_map, anim_slot_to_base_tex);

    model.asset.generator = "opengoal";
    consolidate_buffers(model);

    auto glb_file = glb_path / fmt::format("{}.glb", mmodel.name);
    file_util::create_dir_if_needed_for_file(glb_file);

    tinygltf::TinyGLTF gltf;
    gltf.WriteGltfSceneToFile(&model, glb_file.string(),
                              true,   // embedImages
                              true,   // embedBuffers
                              true,   // pretty print
                              true);  // write binary
  }
}