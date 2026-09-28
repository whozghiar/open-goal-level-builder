// ogle-extract: extracts the levels of a Jak and Daxter game (Jak 1, 2 or 3) from the user's own
// disc into the .glb files the level editor reads. Each level gets a folder:
//
//   <output>/<game>/<level>/decor/<level>-background.glb   decor (one node per decor instance),
//                                                          collision (exact surfaces) and actors
//   <output>/<game>/<level>/models/<model>-lod0.glb ...    its models (crates, doors, enemies...)
//   <output>/<game>/<level>/textures/*.png                 its textures (--textures)
//   <output>/<game>/<level>/level.json                     what the folder holds
//   <output>/<game>/common/models/                         models every level can use
//
// It is built from the parts of OpenGOAL's decompiler that read game data (extractor/opengoal,
// ISC license), without the decompiler's code analysis, and reads its game tables from
// extractor/data/<game>/ (copied next to the executable as ogle-extract-data/).
//
//   ogle-extract <game.iso | extracted disc folder> <output folder> [--levels ATO,CIB] [--textures]
//   ogle-extract --detect <game.iso | folder>     game and version, as json
//   ogle-extract --list <game.iso | folder>       the levels of the disc, as json
//
// An .iso is read in place: detecting and listing read its file table, and an extraction copies
// only the files it reads (the DGO and CGO folders, about 400 MB of a 4 GB disc) to a work folder
// it deletes when done.
//
// Lines starting with OGLE_ are for the editor, which translates their codes:
//   OGLE_GAME {json}                        the game of the disc
//   OGLE_LEVELS [json]                      the answer to --list
//   OGLE_STEP <code> [numbers]              iso <done> <total>, read, textures, levels <count>
//   OGLE_PROGRESS <done> <total> <name>     a level (or the common models) is written
//   OGLE_DONE <folder>                      the end: the game's folder
//   OGLE_ERROR <code> [detail]              see the fail() calls

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define OGLE_ISATTY(f) _isatty(_fileno(f))
#else
#include <unistd.h>
#define OGLE_ISATTY(f) isatty(fileno(f))
#endif

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/read_iso_file.h"
#include "common/versions/versions.h"

#include "decompiler/Disasm/OpcodeInfo.h"
#include "decompiler/ObjectFile/ObjectFileDB.h"
#include "decompiler/config.h"
#include "decompiler/extractor/extractor_util.h"
#include "decompiler/level_extractor/extract_level.h"

#include "third-party/json.hpp"
#include "third-party/zstd/lib/common/xxhash.h"

using nlohmann::json;

namespace {

std::mutex g_out_mutex;

void emit(const std::string& line) {
  std::lock_guard<std::mutex> lock(g_out_mutex);
  std::printf("%s\n", line.c_str());
  std::fflush(stdout);
}

int fail(const std::string& code, const std::string& detail = "") {
  emit("OGLE_ERROR " + code + (detail.empty() ? "" : " " + detail));
  return 1;
}

json read_json(const fs::path& path) {
  return json::parse(file_util::read_text_file(path), nullptr, true, true);
}

std::string upper(std::string s) {
  for (auto& c : s) c = (char)toupper((unsigned char)c);
  return s;
}

void usage() {
  std::printf(
      "ogle-extract: extracts the levels of a Jak and Daxter game (1, 2, 3) to .glb files\n"
      "  ogle-extract <game.iso | disc folder> <output folder> [--levels ATO,CIB] [--textures]\n"
      "  ogle-extract --detect <game.iso | folder>   game and version (json)\n"
      "  ogle-extract --list <game.iso | folder>     levels of the disc (json)\n"
      "  options: --data <folder> (the games' tables), --work <folder> (files copied from the .iso)\n");
}

// ------------------------------------------------------------------------------------------------
// the disc: an extracted folder, or an .iso read in place
// ------------------------------------------------------------------------------------------------

struct Disc {
  fs::path folder;  // extracted disc
  fs::path iso;     // or an .iso and its file table
  IsoFile layout;

  bool is_iso() const { return !iso.empty(); }

  // an entry of the .iso ("DGO/ATO.DGO"), names compared without case
  const IsoFile::Entry* entry(const std::string& rel) const {
    const IsoFile::Entry* e = &layout.root;
    size_t start = 0;
    while (e && start <= rel.size()) {
      size_t slash = rel.find('/', start);
      std::string part = upper(rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
      const IsoFile::Entry* next = nullptr;
      for (const auto& c : e->children)
        if (upper(c.name) == part) next = &c;
      e = next;
      if (slash == std::string::npos) break;
      start = slash + 1;
    }
    return e;
  }

  bool has(const std::string& rel) const {
    std::error_code ec;
    return is_iso() ? entry(rel) != nullptr : fs::is_regular_file(folder / rel, ec);
  }
};

bool read_iso_entry(FILE* fp, const IsoFile::Entry& e, std::vector<uint8_t>* out) {
#ifdef _WIN32
  if (_fseeki64(fp, (long long)e.offset_in_file, SEEK_SET)) return false;
#else
  if (fseeko(fp, (off_t)e.offset_in_file, SEEK_SET)) return false;
#endif
  out->resize(e.size);
  return e.size == 0 || fread(out->data(), 1, e.size, fp) == e.size;
}

bool open_disc(const fs::path& input, Disc* disc, std::string* error) {
  std::error_code ec;
  if (fs::is_directory(input, ec)) {
    disc->folder = input;
    if (!fs::exists(input / "DGO", ec)) {
      *error = "not_jak " + input.string();
      return false;
    }
    return true;
  }
  if (!fs::is_regular_file(input, ec)) {
    *error = "not_found " + input.string();
    return false;
  }
  auto fp = file_util::open_file(input, "rb");
  if (!fp) {
    *error = "unreadable " + input.string();
    return false;
  }
  // an ISO 9660 image says so in its first volume descriptor
  char magic[5] = {};
  bool iso = fseek(fp, 0x8001, SEEK_SET) == 0 && fread(magic, 1, 5, fp) == 5 && std::string(magic, 5) == "CD001";
  if (iso) disc->layout = find_files_in_iso(fp);
  fclose(fp);
  if (!iso) {
    *error = "not_iso " + input.string();
    return false;
  }
  disc->iso = input;
  if (!disc->has("DGO")) {
    *error = "not_jak " + input.string();
    return false;
  }
  return true;
}

struct DiscInfo {
  std::string game;     // jak1, jak2, jak3
  std::string version;  // decompiler config version: ntsc_v1, pal, jp...
  std::string name;
  std::string serial;
};

// The executable of the disc (SCES_516.08...) gives the serial, its hash the exact version.
bool detect(const Disc& disc, DiscInfo* info, std::string* error) {
  std::optional<std::string> serial;
  std::optional<uint64_t> elf_hash;
  if (!disc.is_iso()) {
    std::tie(serial, elf_hash) = findElfFile(disc.folder);
  } else {
    const std::regex elf_name(".{4}_.{3}\\..{2}");
    for (const auto& e : disc.layout.root.children) {
      if (e.is_dir || !std::regex_match(e.name, elf_name)) continue;
      serial = e.name.substr(0, 4) + "-" + e.name.substr(5, 3) + e.name.substr(9, 2);
      auto fp = file_util::open_file(disc.iso, "rb");
      std::vector<uint8_t> elf;
      if (fp && read_iso_entry(fp, e, &elf)) elf_hash = XXH64(elf.data(), elf.size(), 0);
      if (fp) fclose(fp);
      break;
    }
  }
  if (!serial || !elf_hash) {
    *error = "no_executable";
    return false;
  }
  const auto& db = extractor_iso_database();
  auto by_serial = db.find(serial.value());
  if (by_serial == db.end()) {
    *error = "unknown_disc " + serial.value();
    return false;
  }
  auto meta = by_serial->second.find(elf_hash.value());
  const ISOMetadata& m = meta != by_serial->second.end() ? meta->second : by_serial->second.begin()->second;
  info->game = m.game_name;
  info->version = m.decomp_config_version;
  info->name = m.canonical_name;
  info->serial = serial.value();
  return true;
}

// The files of an .iso the extraction reads, copied to `dest` (kept when already there).
bool copy_from_iso(const Disc& disc, const std::vector<std::string>& files, const fs::path& dest,
                   std::string* error) {
  auto fp = file_util::open_file(disc.iso, "rb");
  if (!fp) {
    *error = "unreadable " + disc.iso.string();
    return false;
  }
  std::error_code ec;
  size_t done = 0;
  std::vector<uint8_t> data;
  for (const auto& rel : files) {
    done++;
    const IsoFile::Entry* e = disc.entry(rel);
    if (!e || e->is_dir) continue;
    const fs::path out = dest / rel;
    if (fs::exists(out, ec) && fs::file_size(out, ec) == e->size) continue;
    if (done % 10 == 1 || done == files.size())
      emit("OGLE_STEP iso " + std::to_string(done) + " " + std::to_string(files.size()));
    fs::create_directories(out.parent_path(), ec);
    if (!read_iso_entry(fp, *e, &data)) {
      fclose(fp);
      *error = "iso_read " + rel;
      return false;
    }
    file_util::write_binary_file(out, data.data(), data.size());
  }
  fclose(fp);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  std::string mode, data_dir, work_dir;
  std::vector<std::string> positional;
  std::set<std::string> only_levels;
  bool textures = false;
  for (size_t i = 0; i < args.size(); i++) {
    const std::string& a = args[i];
    auto next = [&]() { return i + 1 < args.size() ? args[++i] : std::string(); };
    if (a == "--help" || a == "-h") {
      usage();
      return 0;
    } else if (a == "--detect" || a == "--list") {
      mode = a;
    } else if (a == "--levels") {
      std::string list = next(), cur;
      for (char c : list + ",") {
        if (c == ',') {
          if (!cur.empty()) {
            cur = upper(cur);
            if (cur.size() < 4 || cur.substr(cur.size() - 4) != ".DGO") cur += ".DGO";
            only_levels.insert(cur);
          }
          cur.clear();
        } else if (c != ' ') {
          cur += c;
        }
      }
    } else if (a == "--textures") {
      textures = true;
    } else if (a == "--data") {
      data_dir = next();
    } else if (a == "--work") {
      work_dir = next();
    } else {
      positional.push_back(a);
    }
  }
  if (positional.empty() || (mode.empty() && positional.size() < 2)) {
    usage();
    return 1;
  }

  if (!OGLE_ISATTY(stdout)) lg::disable_ansi_colors();
  lg::set_stdout_level(lg::level::warn);
  lg::initialize();

  const fs::path exe_dir = fs::path(file_util::get_current_executable_path()).parent_path();
  // the decompiler's code reads and writes below its "project" folder: the output folder (the
  // folder of the executable when only reading a disc), never another checkout
  file_util::setup_project_path(mode.empty() ? fs::absolute(positional[1]) : exe_dir, true);
  const fs::path data = data_dir.empty() ? exe_dir / "ogle-extract-data" : fs::path(data_dir);
  Disc disc;
  DiscInfo info;
  std::string err;
  if (!open_disc(fs::absolute(positional[0]), &disc, &err)) return fail(err);
  if (!detect(disc, &info, &err)) return fail(err);
  emit("OGLE_GAME " + json{{"game", info.game}, {"version", info.version}, {"name", info.name},
                           {"serial", info.serial}, {"iso", disc.is_iso()}}.dump());
  if (mode == "--detect") return 0;

  const fs::path tables = data / info.game;
  std::error_code ec;
  if (!fs::exists(tables / "all-types.gc", ec)) return fail("tables_missing", tables.string());
  json inputs, names;
  try {
    inputs = read_json(tables / "inputs.json");
    names = read_json(tables / "levels.json");
  } catch (const std::exception& e) {
    return fail("tables_unreadable", e.what());
  }
  // the levels of the tables that are on this disc
  std::vector<std::string> levels;
  for (const auto& l : inputs.at("levels_to_extract")) {
    std::string dgo = l.get<std::string>();
    if ((only_levels.empty() || only_levels.count(dgo)) && disc.has("DGO/" + dgo)) levels.push_back(dgo);
  }
  if (mode == "--list") {
    json list = json::array();
    for (const auto& dgo : levels) list.push_back({{"dgo", dgo}, {"names", names.value(dgo, json::array())}});
    emit("OGLE_LEVELS " + list.dump());
    return 0;
  }
  if (levels.empty()) return fail("no_levels");

  // the configuration of the decompiler's data readers, for level extraction only
  decompiler::Config config;
  config.game_version = game_name_to_version(info.game);
  config.game_name = info.game;
  config.all_types_file = (tables / "all-types.gc").string();
  // like jak-project's configs: only Jak 1 names its object files from a map, Jak 2 and 3 use
  // the names found in the DGOs
  if (info.game == "jak1") {
    std::string objs = "all_objs.json";
    if (info.version == "pal") objs = "all_objs_jak1_pal.json";
    if (info.version == "jp") objs = "all_objs_jak1_jp.json";
    config.obj_file_name_map_file = (tables / objs).string();
  }
  for (const auto& d : inputs.at("dgo_names")) {
    std::string name = d.get<std::string>();
    // every level (their textures complete each other) and the common file (textures and shared
    // models), when on the disc
    bool level = name.size() > 3 && name.substr(name.size() - 3) == "DGO";
    bool common = name.size() >= 8 && name.substr(name.size() - 8) == "GAME.CGO";
    if ((level || common) && disc.has(name)) config.dgo_names.push_back(name);
  }
  if (inputs.contains("str_texture_file_names"))
    config.str_texture_file_names = inputs["str_texture_file_names"].get<std::vector<std::string>>();
  if (inputs.contains("str_art_file_names"))
    config.str_art_file_names = inputs["str_art_file_names"].get<std::vector<std::string>>();
  config.common_tpages = inputs.value("common_tpages", json::array()).get<std::unordered_set<int>>();
  config.animated_textures = inputs.value("animated_textures", json::array()).get<std::unordered_set<std::string>>();
  for (const auto& entry : read_json(tables / "missing_textures.json")) {
    int tpage = entry.at(1).get<int>(), idx = entry.at(2).get<int>();
    config.hacks.missing_textures_by_level[entry.at(0).get<std::string>()].emplace_back(tpage, idx);
  }
  config.levels_to_extract = levels;
  config.levels_extract = true;
  config.rip_levels = true;
  config.rip_level_entities = true;
  config.extract_collision = true;
  config.rip_textures_png = textures;

  const fs::path output = fs::absolute(positional[1]);
  fs::create_directories(output / info.game, ec);
  if (!fs::is_directory(output / info.game, ec)) return fail("cannot_create", (output / info.game).string());
  config.rip_output_root = (output / info.game).string();
  // a level written by the DGO the game loads it from goes to <level>/; another DGO holding the
  // same level (a test copy) to <level>-<dgo>/
  std::map<std::string, std::string> level_owner;
  for (const auto& [dgo, level_names] : names.items())
    for (const auto& n : level_names) level_owner[n.get<std::string>()] = dgo;
  config.rip_level_folder = [level_owner](const std::string& dgo, const std::string& level) {
    auto it = level_owner.find(level);
    if (it == level_owner.end() || it->second == dgo) return level;
    std::string suffix = dgo.substr(0, dgo.find('.'));
    for (auto& c : suffix) c = (char)tolower((unsigned char)c);
    return level + "-" + suffix;
  };
  std::atomic<size_t> done{0};
  const size_t total = levels.size() + 1;  // + the common models
  config.on_level_done = [&](const std::string& name) {
    emit("OGLE_PROGRESS " + std::to_string(++done) + " " + std::to_string(total) + " " + name);
  };

  // an .iso: the files read, copied to the work folder
  fs::path folder = disc.folder;
  const fs::path work = work_dir.empty() ? output / "_disc" / disc.iso.stem() : fs::path(work_dir);
  if (disc.is_iso()) {
    std::vector<std::string> files = config.dgo_names;
    for (const auto& s : config.str_texture_file_names) files.push_back(s);
    for (const auto& s : config.str_art_file_names) files.push_back(s);
    if (!copy_from_iso(disc, files, work, &err)) return fail(err);
    folder = work;
  }

  try {
    emit("OGLE_STEP read");
    decompiler::init_opcode_info();
    std::vector<fs::path> dgos, tex_strs, art_strs;
    for (const auto& d : config.dgo_names) dgos.push_back(folder / d);
    for (const auto& s : config.str_texture_file_names)
      if (fs::exists(folder / s, ec)) tex_strs.push_back(folder / s);
    for (const auto& s : config.str_art_file_names)
      if (fs::exists(folder / s, ec)) art_strs.push_back(folder / s);
    decompiler::ObjectFileDB db(dgos, fs::path(config.obj_file_name_map_file), {}, {}, tex_strs, art_strs, config);
    db.process_link_data(config);
    db.find_code(config);
    db.process_labels();
    emit("OGLE_STEP textures");
    decompiler::TextureDB tex_db;
    const fs::path scratch = output / "_work";
    db.process_tpages(tex_db, scratch / "textures", config, scratch / "import");
    emit("OGLE_STEP levels " + std::to_string(levels.size()));
    decompiler::extract_all_levels(db, tex_db, levels, "GAME.CGO", config);
    fs::remove_all(scratch, ec);
  } catch (const std::exception& e) {
    return fail("extraction", e.what());
  }
  if (disc.is_iso() && work_dir.empty()) fs::remove_all(output / "_disc", ec);

  // library.json: the levels of this folder, with those of earlier extractions
  const fs::path lib_file = output / info.game / "library.json";
  std::set<std::string> all_levels(levels.begin(), levels.end());
  try {
    if (fs::exists(lib_file, ec))
      for (const auto& l : read_json(lib_file).value("levels", json::array())) all_levels.insert(l.get<std::string>());
  } catch (const std::exception&) {
  }
  json lib = {{"game", info.game}, {"version", info.version}, {"name", info.name}, {"serial", info.serial},
              {"levels", all_levels}, {"generator", "ogle-extract"}};
  file_util::write_text_file(lib_file, lib.dump(2));
  emit("OGLE_DONE " + (output / info.game).string());
  return 0;
}
