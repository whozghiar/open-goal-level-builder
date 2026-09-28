#include "extract_level.h"

#include <algorithm>
#include <set>
#include <thread>

#include "extract_anim.h"

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/SimpleThreadGroup.h"
#include "common/util/string_util.h"

#include "decompiler/level_extractor/BspHeader.h"
#include "decompiler/level_extractor/extract_actors.h"
#include "decompiler/level_extractor/extract_collide_frags.h"
#include "decompiler/level_extractor/extract_hfrag.h"
#include "decompiler/level_extractor/extract_joint_group.h"
#include "decompiler/level_extractor/extract_merc.h"
#include "decompiler/level_extractor/extract_shrub.h"
#include "decompiler/level_extractor/extract_tfrag.h"
#include "decompiler/level_extractor/extract_tie.h"
#include "decompiler/level_extractor/fr3_to_gltf.h"
#include "third-party/stb_image/stb_image_write.h"

namespace decompiler {

/*!
 * Look through files in a DGO and find the bsp-header file (the level)
 */
std::optional<ObjectFileRecord> get_bsp_file(const std::vector<ObjectFileRecord>& records,
                                             const std::string& dgo_name) {
  std::optional<ObjectFileRecord> result;
  if (str_util::ends_with(dgo_name, ".DGO")) {
    // only DGOs are valid levels, and the last file is the bsp file
    result = records.at(records.size() - 1);
  }
  return result;
}

/*!
 * Make sure a file is a valid bsp-header.
 */
bool is_valid_bsp(const decompiler::LinkedObjectFile& file) {
  if (file.segments != 1) {
    lg::error("Got {} segments, but expected 1", file.segments);
    return false;
  }

  auto& first_word = file.words_by_seg.at(0).at(0);
  if (first_word.kind() != decompiler::LinkedWord::TYPE_PTR) {
    lg::error("Expected the first word to be a type pointer, but it wasn't.");
    return false;
  }

  if (first_word.symbol_name() != "bsp-header") {
    lg::error("Expected to get a bsp-header, but got {} instead.", first_word.symbol_name());
    return false;
  }

  return true;
}

tfrag3::Texture make_texture(u32 id, const TextureDB& tex_db, bool pool_load) {
  const auto& tex = tex_db.textures.at(id);
  auto resolved = tex_db.resolve_texture(id);

  tfrag3::Texture new_tex;
  new_tex.combo_id = id;
  new_tex.w = resolved.w;
  new_tex.h = resolved.h;
  new_tex.debug_tpage_name = tex_db.tpage_names.at(tex.page);
  new_tex.debug_name = tex.name;
  new_tex.data = std::move(resolved.rgba);
  new_tex.load_to_pool = pool_load;
  return new_tex;
}

void add_all_textures_from_level(tfrag3::Level& lev,
                                 const std::string& level_name,
                                 const TextureDB& tex_db) {
  auto level_it = tex_db.texture_ids_per_level.find(level_name);
  if (level_it == tex_db.texture_ids_per_level.end()) {
    return;
  }

  for (auto id : level_it->second) {
    lev.textures.push_back(make_texture(id, tex_db, true));
  }
}

void confirm_textures_identical(const TextureDB& tex_db) {
  std::unordered_map<std::string, std::vector<u32>> tex_dupl;
  for (auto& tex : tex_db.textures) {
    auto name = tex_db.tpage_names.at(tex.second.page) + tex.second.name;
    auto it = tex_dupl.find(name);
    if (it == tex_dupl.end()) {
      tex_dupl.insert({name, tex.second.rgba_bytes});
    } else {
      bool ok = it->second == tex.second.rgba_bytes;
      if (!ok) {
        ASSERT_MSG(false, fmt::format("BAD duplicate: {} {} vs {}", name,
                                      tex.second.rgba_bytes.size(), it->second.size()));
      }
    }
  }
}

void extract_art_groups_from_level(const ObjectFileDB& db,
                                   const TextureDB& tex_db,
                                   const std::vector<level_tools::TextureRemap>& tex_remap,
                                   const std::string& dgo_name,
                                   tfrag3::Level& level_data,
                                   std::map<std::string, level_tools::ArtData>& art_group_data) {
  if (db.obj_files_by_dgo.count(dgo_name)) {
    const auto& files = db.obj_files_by_dgo.at(dgo_name);
    MercSwapInfo swapped_info;
    // build list of models to replace
    auto merc_replacements_path = file_util::get_jak_project_dir() / "custom_assets" /
                                  game_version_names[db.version()] / "merc_replacements";
    if (file_util::file_exists(merc_replacements_path.string())) {
      auto custom_models =
          file_util::find_files_in_dir(merc_replacements_path, std::regex(".*\\.glb"));
      for (auto& mdl : custom_models) {
        swapped_info.add_to_swap_list(mdl.stem().string());
      }
    }
    for (const auto& file : files) {
      if (file.name.length() > 3 && !file.name.compare(file.name.length() - 3, 3, "-ag")) {
        const auto& ag_file = db.lookup_record(file);
        extract_merc(ag_file, tex_db, db.dts, tex_remap, level_data, false, db.version(),
                     swapped_info);
        extract_joint_group(ag_file, db.dts, db.version(), art_group_data);
        extract_animations(ag_file, db.dts, db.version(), art_group_data);
      }
    }
  }
}

std::vector<level_tools::TextureRemap> extract_tex_remap(const ObjectFileDB& db,
                                                         const std::string& dgo_name) {
  auto bsp_rec = get_bsp_file(db.obj_files_by_dgo.at(dgo_name), dgo_name);
  if (!bsp_rec) {
    lg::warn("Skipping extract for {} because the BSP file was not found", dgo_name);
    return {};
  }
  std::string level_name = bsp_rec->name;

  lg::info("Processing level {} ({})", dgo_name, level_name);
  const auto& bsp_file = db.lookup_record(*bsp_rec);
  bool ok = is_valid_bsp(bsp_file.linked_data);
  ASSERT(ok);

  level_tools::BspHeader bsp_header;
  bsp_header.read_from_file(bsp_file.linked_data, db.dts, db.version(), true);

  return bsp_header.texture_remap_table;
}

level_tools::BspHeader extract_bsp_from_level(const ObjectFileDB& db,
                                              const TextureDB& tex_db,
                                              const std::string& dgo_name,
                                              const Config& config,
                                              tfrag3::Level& level_data) {
  auto hacks = config.hacks;
  auto bsp_rec = get_bsp_file(db.obj_files_by_dgo.at(dgo_name), dgo_name);
  if (!bsp_rec) {
    lg::warn("Skipping extract for {} because the BSP file was not found", dgo_name);
    return {};
  }

  lg::info("Processing {}...", dgo_name);
  const auto& bsp_file = db.lookup_record(*bsp_rec);
  bool ok = is_valid_bsp(bsp_file.linked_data);
  ASSERT(ok);

  level_tools::BspHeader bsp_header;
  bsp_header.read_from_file(bsp_file.linked_data, db.dts, db.version());
  ASSERT((int)bsp_header.drawable_tree_array.trees.size() == bsp_header.drawable_tree_array.length);

  // grrr.....
  if (db.version() == GameVersion::Jak1 && dgo_name == "TIT.DGO" && bsp_header.name == "intro") {
    bsp_header.name = "title";
  } else if (db.version() == GameVersion::Jak1 && dgo_name == "DEM.DGO" &&
             bsp_header.name == "intro") {
    bsp_header.name = "demo";
  }

  /*
  level_tools::PrintSettings settings;
  settings.expand_collide = true;
  lg::print("{}\n", bsp_header.print(settings));
   */

  const std::set<std::string> tfrag_trees = {
      "drawable-tree-tfrag",        "drawable-tree-trans-tfrag",       "drawable-tree-tfrag-trans",
      "drawable-tree-dirt-tfrag",   "drawable-tree-tfrag-water",       "drawable-tree-ice-tfrag",
      "drawable-tree-lowres-tfrag", "drawable-tree-lowres-trans-tfrag"};
  int i = 0;

  std::vector<const level_tools::DrawableTreeInstanceTie*> all_ties;
  for (auto& draw_tree : bsp_header.drawable_tree_array.trees) {
    auto as_tie_tree = dynamic_cast<level_tools::DrawableTreeInstanceTie*>(draw_tree.get());
    if (as_tie_tree) {
      all_ties.push_back(as_tie_tree);
    }
  }

  bool got_collide = false;
  for (auto& draw_tree : bsp_header.drawable_tree_array.trees) {
    if (tfrag_trees.count(draw_tree->my_type())) {
      auto as_tfrag_tree = dynamic_cast<level_tools::DrawableTreeTfrag*>(draw_tree.get());
      ASSERT(as_tfrag_tree);
      std::vector<std::pair<int, int>> expected_missing_textures;
      auto it = hacks.missing_textures_by_level.find(bsp_header.name);
      if (it != hacks.missing_textures_by_level.end()) {
        expected_missing_textures = it->second;
      }
      bool atest_disable_flag = false;
      if (db.version() >= GameVersion::Jak2) {
        if (bsp_header.texture_flags[0] & 1) {
          atest_disable_flag = true;
        }
      }
      extract_tfrag(as_tfrag_tree, fmt::format("{}-{}", dgo_name, i++),
                    bsp_header.texture_remap_table, tex_db, expected_missing_textures, level_data,
                    false, bsp_header.name, atest_disable_flag);
    } else if (draw_tree->my_type() == "drawable-tree-instance-tie") {
      auto as_tie_tree = dynamic_cast<level_tools::DrawableTreeInstanceTie*>(draw_tree.get());
      ASSERT(as_tie_tree);
      extract_tie(as_tie_tree, fmt::format("{}-{}-tie", dgo_name, i++),
                  bsp_header.texture_remap_table, tex_db, level_data, false, db.version());
    } else if (draw_tree->my_type() == "drawable-tree-instance-shrub") {
      auto as_shrub_tree =
          dynamic_cast<level_tools::shrub_types::DrawableTreeInstanceShrub*>(draw_tree.get());
      ASSERT(as_shrub_tree);
      extract_shrub(as_shrub_tree, fmt::format("{}-{}-shrub", dgo_name, i++),
                    bsp_header.texture_remap_table, tex_db, {}, level_data, false, db.version());
    } else if (draw_tree->my_type() == "drawable-tree-collide-fragment" &&
               config.extract_collision) {
      auto as_collide_frags =
          dynamic_cast<level_tools::DrawableTreeCollideFragment*>(draw_tree.get());
      ASSERT(as_collide_frags);
      ASSERT(!got_collide);
      got_collide = true;
      extract_collide_frags(as_collide_frags, all_ties, config,
                            fmt::format("{}-{}-collide", dgo_name, i++), level_data);
    } else {
      lg::print("  unsupported tree {}\n", draw_tree->my_type());
    }
  }

  if (bsp_header.collide_hash.num_items) {
    ASSERT(!got_collide);
    extract_collide_frags(bsp_header.collide_hash, all_ties, config,
                          fmt::format("{}-{}-collide", dgo_name, i++), db.dts, level_data);
  }
  if (bsp_header.hfrag) {
    extract_hfrag(bsp_header, tex_db, &level_data);
  }
  level_data.level_name = bsp_header.name;

  return bsp_header;
}

/*!
 * Extract stuff found in GAME.CGO.
 * Even though GAME.CGO isn't technically a level, the decompiler/loader treat it like one,
 * but the bsp stuff is just empty. It will contain only textures/art groups.
 */
void extract_common(const ObjectFileDB& db,
                    const TextureDB& tex_db,
                    const std::string& dgo_name,
                    const Config& config) {
  if (db.obj_files_by_dgo.count(dgo_name) == 0) {
    lg::warn("Skipping common extract for {} because the DGO was not part of the input", dgo_name);
    return;
  }

  if (tex_db.textures.size() == 0) {
    lg::warn("Skipping common extract because there were no textures in the input");
    return;
  }

  confirm_textures_identical(tex_db);

  tfrag3::Level tfrag_level;
  std::map<std::string, level_tools::ArtData> art_group_data;
  add_all_textures_from_level(tfrag_level, dgo_name, tex_db);
  extract_art_groups_from_level(db, tex_db, {}, dgo_name, tfrag_level, art_group_data);

  add_all_textures_from_level(tfrag_level, "ARTSPOOL", tex_db);
  extract_art_groups_from_level(db, tex_db, {}, "ARTSPOOL", tfrag_level, art_group_data);

  // the models every level can use (collectables, Jak...)
  const fs::path out = fs::path(config.rip_output_root) / "common" / "models";
  file_util::create_dir_if_needed(out);
  save_level_foreground_as_gltf(tfrag_level, art_group_data, out, tex_db.animated_tex_output_to_anim_slot);
  if (config.on_level_done) {
    config.on_level_done("common");
  }
}

namespace {
std::string file_safe(std::string s) {
  for (auto& c : s) {
    if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) {
      c = '_';
    }
  }
  return s;
}
}  // namespace

void extract_from_level(const ObjectFileDB& db,
                        const TextureDB& tex_db,
                        const std::string& dgo_name,
                        const Config& config) {
  if (db.obj_files_by_dgo.count(dgo_name) == 0) {
    lg::warn("Skipping extract for {} because the DGO was not part of the input", dgo_name);
    return;
  }
  tfrag3::Level level_data;
  std::map<std::string, level_tools::ArtData> art_group_data;
  add_all_textures_from_level(level_data, dgo_name, tex_db);

  // the bsp header file data
  auto bsp_header = extract_bsp_from_level(db, tex_db, dgo_name, config, level_data);
  extract_art_groups_from_level(db, tex_db, bsp_header.texture_remap_table, dgo_name, level_data,
                                art_group_data);

  // <root>/<level>/decor: the background with its collision, actors and decor instances
  const std::string& level_name = level_data.level_name;
  const std::string name = config.rip_level_folder ? config.rip_level_folder(dgo_name, level_name) : level_name;
  const fs::path level_dir = fs::path(config.rip_output_root) / name;
  const fs::path decor = level_dir / "decor" / fmt::format("{}-background.glb", name);
  file_util::create_dir_if_needed_for_file(decor);
  LevelEntities entities;
  entities.game = game_version_names[config.game_version];
  entities.actors = nlohmann::json::parse(extract_actors_to_json(bsp_header.actors));
  entities.collision = true;
  save_level_background_as_gltf(level_data, decor, &entities);

  // <root>/<level>/models: one file per model of the level's art groups
  const fs::path models = level_dir / "models";
  file_util::create_dir_if_needed(models);
  save_level_foreground_as_gltf(level_data, art_group_data, models,
                                tex_db.animated_tex_output_to_anim_slot);

  // <root>/<level>/textures: the level's textures as png, when asked
  size_t png_count = 0;
  if (config.rip_textures_png) {
    const fs::path textures = level_dir / "textures";
    file_util::create_dir_if_needed(textures);
    for (const auto& tex : level_data.textures) {
      if (tex.w == 0 || tex.h == 0 || tex.data.size() < (size_t)tex.w * tex.h) {
        continue;
      }
      auto file = textures / (file_safe(tex.debug_name) + ".png");
      if (stbi_write_png(file.string().c_str(), tex.w, tex.h, 4, tex.data.data(), tex.w * 4)) {
        png_count++;
      }
    }
  }

  // <root>/<level>/level.json: what the folder holds
  nlohmann::json info;
  info["level"] = level_name;
  info["dgo"] = dgo_name;
  info["game"] = game_version_names[config.game_version];
  info["decor"] = fmt::format("decor/{}-background.glb", name);
  info["actors"] = entities.actors.is_array() ? entities.actors.size() : 0;
  // the actors by type: how many, and the data of the first one (a template for new ones)
  auto& types = info["actor_types"] = nlohmann::json::object();
  if (entities.actors.is_array()) {
    for (const auto& actor : entities.actors) {
      const std::string etype = actor.value("etype", std::string());
      if (etype.empty()) {
        continue;
      }
      auto& t = types[etype];
      if (t.is_null()) {
        t = {{"count", 0}, {"lump", actor.value("lump", nlohmann::json::object())}};
      }
      t["count"] = t["count"].get<int>() + 1;
      // the actors that choose their model by name (art-name): every name, once
      const auto lump = actor.value("lump", nlohmann::json::object());
      if (lump.contains("art-name") && lump["art-name"].is_string()) {
        auto& names = t["art_names"];
        if (!names.is_array()) {
          names = nlohmann::json::array();
        }
        if (std::find(names.begin(), names.end(), lump["art-name"]) == names.end()) {
          names.push_back(lump["art-name"]);
        }
      }
    }
  }
  info["collision_triangles"] = level_data.collision.vertices.size() / 3;
  info["models"] = nlohmann::json::array();
  for (const auto& model : level_data.merc_data.models) {
    info["models"].push_back(model.name);
  }
  info["textures"] = level_data.textures.size();
  info["texture_pngs"] = png_count;
  file_util::write_text_file(level_dir / "level.json", info.dump(2));
  if (config.on_level_done) {
    config.on_level_done(name);
  }
}

void extract_all_levels(const ObjectFileDB& db,
                        const TextureDB& tex_db,
                        const std::vector<std::string>& dgo_names,
                        const std::string& common_name,
                        const Config& config) {
  extract_common(db, tex_db, common_name, config);
  // levels in parallel, as many as the processor runs (each one holds its whole data)
  int num_workers = std::max(1, std::min((int)dgo_names.size(), (int)std::thread::hardware_concurrency()));
  if (tex_db.replace_texture_dir) {
    num_workers = 1;
  }
  SimpleThreadGroup threads;
  threads.run([&](int idx) { extract_from_level(db, tex_db, dgo_names[idx], config); },
              dgo_names.size(), num_workers);
  threads.join();
}

}  // namespace decompiler
