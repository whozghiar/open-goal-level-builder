#pragma once

// What the editor knows of a game beyond its extracted files: data/<game>/game-data.json, made by
// tools/gen_game_data.py from the decompiled game code.
//
//   actor roles  the actor types that are creatures (enemies, characters, the Titan suit) or logic
//                (particle spawners, cameras, triggers): levels open without them
//   water        the model of each "look" of the water actors (water, dark eco and lava pools)
//   oceans       where the ocean of a level is, and its height
//   levels       per level: its task level and mask, its ocean, whether it is a layer (the actors
//                a mission adds to another level), its companions (levels the game shows with it)
//   story        the task nodes in the order of the game, and what they change in the levels
//
// The story of Jak II and Jak 3: each level has a task mask, changed by the task nodes of its
// task level as they close; an actor exists while its kill mask and that mask share no bit.
// Some decor prototypes and actor types are shown or hidden by conditions on the nodes, and open
// nodes lend layers to levels.

#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/math.h"

namespace ogle {

enum class ActorRole { Object = 0, Creature, Logic };

// A condition on the story, evaluated at a point of the game.
struct StoryCond {
  enum Op : uint8_t { False = 0, True, Closed, Open, Complete, Not, And, Or };
  Op op = True;
  int ref = 0;  // Closed, Open: a node; Complete: a task
  std::vector<StoryCond> args;
};

struct StoryBorrow {
  std::string host;       // the level borrowing
  int slot = 0;
  std::string level;      // the layer it shows, "" to empty the slot
  bool special = false;   // only the layer's actors meant for it are shown ('special)
};

struct StoryNode {
  std::string name;
  std::string level;  // the task level whose mask it changes when it closes
  int task = -1;      // its task (StoryCond::Complete)
  enum MaskOp : uint8_t { NoMask = 0, Abs, Set, Clear };
  MaskOp mask_op = NoMask;
  uint32_t mask = 0;
  bool close_task = false;  // closing it completes its task
  bool closed = false;      // closed from the start of the game
  std::vector<int> parents;
  std::vector<StoryBorrow> borrow;
};

struct LevelInfo {
  std::string taskname;  // the task level of its task mask
  uint32_t base_mask = 0;
  std::string ocean;     // its ocean map, "" without one
  std::optional<float> ocean_height;  // set by the level (Jak 3), else the map's
  bool layer = false;    // borrowed by the story: its actors appear in other levels
  bool traffic = false;  // part of the city: the actors of the city traffic are there
  std::vector<std::string> companions;
  std::vector<StoryBorrow> borrow;  // what its slots show before the story changes them
};

struct OceanMap {
  Vec3 corner;  // meters; y: its height
  float cell = 96.f;
  int size = 48;
  std::vector<uint8_t> cells;  // size x size, rows along z: 1 where the ocean is
  Vec3 color{0.1f, 0.3f, 0.35f};
};

struct PrototypeRule {
  std::vector<std::string> names;  // decor prototypes, without ".mb"
  StoryCond when;                  // shown when true
};

// A point of the story: the nodes closed and open, the tasks complete.
struct StoryMoment {
  int step = 1;
  std::vector<uint8_t> closed, open;  // per node
  std::vector<uint8_t> complete;      // per task
};

class GameData {
 public:
  bool load(const std::string& path, std::string* error);
  bool loaded() const { return m_loaded; }
  const std::string& game() const { return m_game; }

  // actors
  ActorRole role(const std::string& etype) const;
  const std::unordered_map<std::string, ActorRole>& roles() const { return m_roles; }  // creatures, logic
  bool is_water(const std::string& etype) const;
  // a particle effect (part-spawner: its art-name names the particle group)
  bool is_effect(const std::string& etype) const { return m_effects.count(etype) != 0; }
  // logic the catalog does not offer: it does nothing, brings creatures, runs a cutscene
  bool is_unlisted(const std::string& etype) const { return m_unlisted.count(etype) != 0; }
  // the water types (water-anim and its subtypes)
  std::vector<std::string> water_types() const;
  const std::string& water_model(int look) const;  // "" when unknown
  int water_look_count() const { return (int)m_water_looks.size(); }
  // the object types (drawn, neither creatures nor logic) and their family: crate, platform...
  const std::unordered_map<std::string, std::string>& objects() const { return m_objects; }
  std::string object_family(const std::string& etype) const;
  // the models a type loads (its code), the one it draws first; null when the game data has none
  const std::vector<std::string>* type_models(const std::string& etype) const;
  // the model of a skeleton group named `name` (without "skel-"), "" when it has the same name:
  // the actors that choose their model by name (art-name) give the group's
  std::string skeleton_model(const std::string& name) const;
  std::string skeleton_of_model(const std::string& model) const;  // the reverse, "" when none
  const std::unordered_map<std::string, std::vector<std::string>>& all_type_models() const { return m_type_models; }

  const LevelInfo* level(const std::string& name) const;
  const OceanMap* ocean(const std::string& name) const;

  // The story, in steps: at step s the nodes before s are closed (1: the start of the game,
  // step_count(): the end).
  bool has_story() const { return !m_nodes.empty(); }
  int step_count() const { return (int)m_nodes.size(); }
  const StoryNode& node(int i) const { return m_nodes[i]; }
  StoryMoment moment(int step) const;
  bool eval(const StoryCond& c, const StoryMoment& m) const;
  // The task mask of the actors of `level` at that moment, in `host` (the level opened: the city
  // traffic and the primary levels change bits).
  uint32_t task_mask(const std::string& level, const std::string& host, const StoryMoment& m) const;
  // Whether an actor with that kill mask exists under that task mask, in a level shown normally
  // or 'special (only the actors meant for it).
  bool actor_alive(uint32_t kill_mask, uint32_t task_mask, bool special) const;
  // The layers `host` shows at that moment: layer -> shown 'special.
  std::map<std::string, bool> borrowed(const std::string& host, const StoryMoment& m) const;
  // Every layer `host` shows at some point of the story.
  std::vector<std::string> layers_of(const std::string& host) const;
  const std::vector<PrototypeRule>* prototype_rules(const std::string& level) const;
  const StoryCond* type_rule(const std::string& etype) const;
  // The height of the ocean of `level` at that step (meters).
  float ocean_height(const std::string& level, int step) const;

 private:
  bool m_loaded = false;
  std::string m_game;
  std::unordered_map<std::string, ActorRole> m_roles;
  std::vector<std::string> m_creature_suffixes;
  std::unordered_map<std::string, bool> m_water_types;
  std::set<std::string> m_effects, m_unlisted;
  std::vector<std::string> m_water_looks;
  std::unordered_map<std::string, std::string> m_objects;
  std::unordered_map<std::string, std::vector<std::string>> m_type_models;
  std::unordered_map<std::string, std::string> m_skeletons;
  std::map<std::string, LevelInfo> m_levels;
  std::map<std::string, OceanMap> m_oceans;
  std::vector<StoryNode> m_nodes;
  std::unordered_map<std::string, int> m_node_index;
  std::vector<std::string> m_tasks;
  std::vector<std::vector<int>> m_task_closers;  // per task: the nodes completing it
  int m_reset_node = -1;                         // its borrows empty the slots first
  std::map<std::string, std::vector<PrototypeRule>> m_prototypes;
  std::unordered_map<std::string, StoryCond> m_types;
  struct HeightRule {
    StoryCond when;
    float height = 0;
  };
  std::map<std::string, std::vector<HeightRule>> m_heights;
  uint32_t m_bit_special = 1u << 12, m_bit_primary = 1u << 13, m_bit_ctywide = 1u << 14, m_bit_never = 1u << 15;
};

// data/<game>/game-data.json next to the editor, else in the source tree it was built from.
std::string game_data_path(const std::string& game);

}  // namespace ogle
