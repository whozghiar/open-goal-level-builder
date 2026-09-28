#pragma once

#include <map>
#include <unordered_map>

#include "common/custom_data/Tfrag3Data.h"
#include "common/util/FileUtil.h"

#include "decompiler/level_extractor/common_formats.h"

#include "third-party/json.hpp"

/*!
 * What "rip_level_entities" adds to a level's background .glb, so that one file holds the whole
 * level:
 *  - the collision: one node per surface value, marked like the level builder's collision
 *    (set_collision, set_invisible), the exact pat in collide_pat and its game in collide_game,
 *  - the actors: one empty node each at its position and rotation, "kind": "actor" and the entry of
 *    <level>-actors.json (etype, aid, game_task, bsphere, lump) in its extras,
 *  - the TIE and shrub prototypes written once, in their own space: under each tree, a group per
 *    prototype holds one node per instance with its matrix, all sharing the prototype's mesh,
 *  - asset.extras.opengoal: the game and the level name.
 */
struct LevelEntities {
  std::string game;            // "jak1", "jak2", "jak3"
  nlohmann::json actors;       // extract_actors_to_json() output
  bool collision = false;      // add the level's collision mesh
};

/*!
 * Export the background geometry (tie, tfrag, shrub) to a GLTF binary format (.glb) file.
 */
void save_level_background_as_gltf(const tfrag3::Level& level,
                                   const fs::path& glb_file,
                                   const LevelEntities* entities = nullptr);
void save_level_foreground_as_gltf(
    const tfrag3::Level& level,
    const std::map<std::string, level_tools::ArtData>& art_data,
    const fs::path& glb_path,
    const std::unordered_map<std::string, u32>& animated_tex_output_to_anim_slot);