#pragma once

// The editor's document: a level loaded from the library, made of nodes.
//
//   Render    : visible geometry (terrain, decor instances, actor models)
//   Collision : the level's collision surfaces, kept with the level but not shown
//   Actor     : an entity of the level (etype, res-lump), with its model as a Render child
//   Group     : an empty node that organizes the others (a decor prototype and its instances)
//
// A project is saved as a .glb: the same data, with the editor's node extras (og_kind...).

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/games.h"
#include "core/math.h"
#include "json.hpp"

namespace ogle {

using json = nlohmann::json;

enum class NodeKind { Group = 0, Render, Collision, Actor, Count };
const char* kind_name(NodeKind k);  // "group", "render", "collision", "actor"
NodeKind kind_from_name(const std::string& s, NodeKind fallback);

struct Texture {
  std::string name;
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;  // width * height * 4
  uint64_t version = 1;       // bumped on change, the renderer re-uploads
};

enum class AlphaMode { Opaque = 0, Mask, Blend };
// How a Blend material mixes with what is behind it. glTF only has Alpha; the others are the PS2's
// additive and subtractive blends (glows, light beams), kept in the material's extras.og_blend.
enum class BlendMode { Alpha = 0, Add, AddAlpha, Subtract, Half };
const char* blend_mode_name(BlendMode b);  // "" for Alpha, else "add", "add_alpha"...
BlendMode blend_mode_from_name(const std::string& name);

struct Material {
  std::string name;
  int texture = -1;  // index into Scene::textures
  Vec4 base_color{1, 1, 1, 1};
  AlphaMode alpha = AlphaMode::Opaque;
  BlendMode blend = BlendMode::Alpha;
  float cutoff = 0.5f;
  bool double_sided = true;
};

struct Bvh;

struct Primitive {
  std::vector<Vec3> pos;
  std::vector<Vec3> nrm;   // same size as pos
  std::vector<Vec2> uv;    // empty or same size as pos
  std::vector<Vec4> col;   // empty or same size as pos (vertex colors, 0..1)
  std::vector<uint32_t> idx;  // triangle list
  int material = -1;          // index into Scene::materials
  std::vector<uint32_t> tri_pat;  // collision only: the surface of each triangle
  // extra vertex attributes kept for round trips, e.g. the 8 time-of-day palettes _SUNRISE...
  std::map<std::string, std::vector<Vec4>> extra_attrs;

  size_t tri_count() const { return idx.size() / 3; }
  void compute_normals();  // smooth normals from faces
};

struct Mesh {
  Mesh();
  Mesh(const Mesh& other);             // copies get a new uid
  Mesh& operator=(const Mesh&) = delete;

  std::string name;
  std::vector<Primitive> prims;
  json extras = json::object();

  uint64_t uid;          // unique for the life of the program (render cache key)
  uint64_t version = 1;  // bumped by touch()

  // derived data, rebuilt lazily after touch()
  const AABB& bounds();
  const Bvh& bvh(size_t prim);
  void touch();
  size_t tri_count() const;

 private:
  AABB m_bounds;
  bool m_bounds_valid = false;
  std::vector<std::shared_ptr<Bvh>> m_bvh;
  uint64_t m_bvh_version = 0;
};

struct Node {
  uint32_t id = 0;
  uint32_t parent = 0;  // 0 = root
  std::string name;
  NodeKind kind = NodeKind::Group;
  Transform local;
  std::shared_ptr<Mesh> mesh;    // Render, Collision
  json extras = json::object();  // actor data (etype, aid, lump...), glTF extras
  bool visible = true;
  bool locked = false;           // not selectable (the terrain)
  bool story_hidden = false;     // not shown at the point of the story viewed (not saved)
};

struct LevelSettings {
  Game game = Game::Jak2;
  std::string level;         // the library level the document comes from ("atoll")
  std::string source_group;  // its folder in the library: its models are drawn for its actors

  json to_json() const;
  static LevelSettings from_json(const json& j);
};

// World matrices, effective visibility and owning actor of every node, aligned with Scene::nodes.
// Computed once per Scene::begin_frame() instead of walking the parents of each node again for
// every query (a whole city is 150 000 nodes).
struct SceneCache {
  std::vector<Mat4> worlds;
  std::vector<uint8_t> visible;   // the node and all its parents are visible (and not hidden by the story)
  std::vector<uint32_t> actor;    // Render nodes under an actor (its model): the actor, else 0
  std::vector<AABB> bounds;       // world box of the node's mesh (Render, Collision), else empty
  // render nodes grouped by mesh (the instances of a prototype): mesh, node indices
  std::vector<std::pair<Mesh*, std::vector<uint32_t>>> mesh_groups;
  uint64_t frame = ~0ull;
  size_t count = 0;
  uint32_t first_id = 0, last_id = 0;
};

class Scene {
 public:
  std::vector<Node> nodes;  // parents before children is not required
  std::vector<Texture> textures;
  std::vector<Material> materials;
  LevelSettings settings;
  uint32_t next_id = 1;

  Node* find(uint32_t id);
  const Node* find(uint32_t id) const;
  int index_of(uint32_t id) const;
  uint32_t add(Node node);  // assigns an id when node.id == 0
  // removes the nodes and all their descendants
  void remove(const std::vector<uint32_t>& ids);
  std::vector<uint32_t> children(uint32_t id) const;
  std::vector<uint32_t> descendants(uint32_t id) const;
  bool is_ancestor(uint32_t ancestor, uint32_t id) const;
  bool effectively_visible(const Node& n) const;
  // the actor a render node belongs to (its model drawn under it), or 0
  uint32_t actor_of(const Node& n) const;

  Mat4 world(const Node& n) const;
  Mat4 world(uint32_t id) const;
  // the cache of the current frame (recomputed after begin_frame() or when nodes were added or
  // removed); queries made after a change in the same frame see the old matrices
  void begin_frame() { m_frame++; }
  const SceneCache& cache() const;
  // sets the node's local transform so that its world matrix becomes `w`
  void set_world(Node& n, const Mat4& w);

  AABB world_bounds(const Node& n) const;  // mesh bounds, or the marker of an actor
  AABB bounds() const;                      // whole scene (collision left out)
  // where most of the scene is: a stray piece far away does not shrink the view (framing)
  AABB focus_bounds() const;

  int add_material(Material m);
  int add_texture(Texture t);
  void clear();

 private:
  uint64_t m_frame = 0;
  mutable SceneCache m_cache;
};

// Marker size of actors without a model (meters)
constexpr float kMarkerRadius = 0.6f;

}  // namespace ogle
