#include "io/library_index.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "core/i18n.h"
#include "core/log.h"

namespace ogle {

namespace fs = std::filesystem;

namespace {

constexpr int kIndexVersion = 2;  // a new one reads every level again

fs::path to_path(const std::string& s) { return fs::path(std::u8string((const char8_t*)s.c_str())); }

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

std::string cache_file(const std::string& dir, const std::string& background) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : background) h = (h ^ c) * 1099511628211ull;
  char name[32];
  std::snprintf(name, sizeof(name), "%016llx.json", (unsigned long long)h);
  return dir + "/" + name;
}

json to_json(const LevelIndex& l) {
  json parts = json::array();
  for (const auto& p : l.parts)
    parts.push_back({p.name, p.mesh_node, p.kind == CatalogPart::Kind::Piece ? 1 : 0, p.triangles, p.copies});
  return {{"version", kIndexVersion}, {"background", l.background}, {"stamp", l.stamp},
          {"parts", parts},           {"art_names", l.art_names},   {"art_lumps", l.art_lumps}};
}

bool from_json(const json& j, LevelIndex* out) {
  if (!j.is_object() || j.value("version", 0) != kIndexVersion) return false;
  auto parts = j.find("parts");
  auto arts = j.find("art_names");
  if (parts == j.end() || !parts->is_array() || arts == j.end() || !arts->is_object()) return false;
  for (const auto& p : *parts) {
    if (!p.is_array() || p.size() != 5) return false;
    CatalogPart part;
    part.name = p[0].get<std::string>();
    part.mesh_node = p[1].get<std::string>();
    part.kind = p[2].get<int>() ? CatalogPart::Kind::Piece : CatalogPart::Kind::Prototype;
    part.triangles = p[3].get<size_t>();
    part.copies = p[4].get<int>();
    out->parts.push_back(std::move(part));
  }
  for (const auto& [etype, names] : arts->items())
    for (const auto& [name, n] : names.items()) out->art_names[etype][name] = n.get<int>();
  auto lumps = j.find("art_lumps");
  if (lumps != j.end() && lumps->is_object())
    for (const auto& [etype, names] : lumps->items())
      for (const auto& [name, lump] : names.items()) out->art_lumps[etype][name] = lump;
  return true;
}

}  // namespace

bool index_level(const std::string& background, LevelIndex* out, std::string* error) {
  // the JSON chunk of the .glb: header (magic, version, length), then chunk length and type
  std::ifstream f(to_path(background), std::ios::binary);
  uint32_t head[5] = {};
  if (!f || !f.read((char*)head, sizeof(head)) || head[0] != 0x46546C67u || head[4] != 0x4E4F534Au) {
    if (error) *error = "not a glTF binary file: " + background;
    return false;
  }
  std::string text(head[3], '\0');
  if (!f.read(text.data(), text.size())) {
    if (error) *error = "truncated file: " + background;
    return false;
  }
  const json j = json::parse(text, nullptr, false);
  text = std::string();
  if (j.is_discarded() || !j.is_object()) {
    if (error) *error = "unreadable glTF JSON: " + background;
    return false;
  }
  static const json kEmpty = json::array();
  auto array = [&](const json& o, const char* key) -> const json& {
    auto it = o.find(key);
    return it != o.end() && it->is_array() ? *it : kEmpty;
  };
  const json& nodes = array(j, "nodes");
  const json& meshes = array(j, "meshes");
  const json& accessors = array(j, "accessors");
  const int n = (int)nodes.size();

  // the names the importer gives, the hierarchy, the triangles of a mesh
  std::vector<std::string> names(n);
  std::vector<int> parent(n, -1);
  std::vector<std::vector<int>> children(n);
  for (int i = 0; i < n; i++) {
    const std::string name = nodes[i].value("name", std::string());
    names[i] = name.empty() ? "node-" + std::to_string(i) : name;
    for (const auto& c : array(nodes[i], "children")) {
      const int ci = c.get<int>();
      if (ci < 0 || ci >= n) continue;
      parent[ci] = i;
      children[i].push_back(ci);
    }
  }
  auto mesh_of = [&](int i) {
    auto it = nodes[i].find("mesh");
    return it != nodes[i].end() && it->is_number_integer() ? it->get<int>() : -1;
  };
  auto triangles = [&](int mesh) {
    size_t count = 0;
    if (mesh < 0 || mesh >= (int)meshes.size()) return count;
    for (const auto& prim : array(meshes[mesh], "primitives")) {
      int acc = prim.value("indices", -1);
      if (acc < 0 && prim.contains("attributes")) acc = prim["attributes"].value("POSITION", -1);
      if (acc >= 0 && acc < (int)accessors.size()) count += accessors[acc].value("count", (size_t)0) / 3;
    }
    return count;
  };

  // the parts, as list_parts() finds them in the loaded level
  std::vector<CatalogPart> prototypes, pieces;
  for (int i = 0; i < n; i++) {
    static const json kNone;
    auto ex = nodes[i].find("extras");
    const json& extras = ex != nodes[i].end() && ex->is_object() ? *ex : kNone;
    if (extras.is_object() && extras.value("kind", std::string()) == "actor") {
      const std::string etype = extras.value("etype", std::string());
      auto lump = extras.find("lump");
      if (!etype.empty() && lump != extras.end() && lump->is_object()) {
        auto art = lump->find("art-name");
        if (art != lump->end() && art->is_string()) {
          const std::string name = art->get<std::string>();
          if (!out->art_names[etype][name]++) out->art_lumps[etype][name] = *lump;
        }
      }
    }
    const int p = parent[i];
    if (p < 0) continue;
    const std::string& pname = names[p];
    const int mesh = mesh_of(i);
    if (contains(pname, "-tie-") || contains(pname, "-shrub-")) {
      CatalogPart part;
      part.kind = CatalogPart::Kind::Prototype;
      part.name = names[i];
      if (mesh >= 0) {
        part.mesh_node = names[i];
        part.triangles = triangles(mesh);
        part.copies = 1;
      } else {
        for (int c : children[i]) {
          const int cm = mesh_of(c);
          if (cm < 0) continue;
          if (part.mesh_node.empty()) {
            part.mesh_node = names[c];
            part.triangles = triangles(cm);
          }
          part.copies++;
        }
      }
      if (!part.mesh_node.empty()) prototypes.push_back(std::move(part));
    } else if (contains(pname, "-tfrag-") && mesh >= 0) {
      auto terrain = extras.is_object() ? extras.find("og_terrain") : extras.end();
      if (!extras.is_object() || terrain == extras.end() || !terrain->is_boolean() || terrain->get<bool>()) continue;
      CatalogPart part;
      part.kind = CatalogPart::Kind::Piece;
      part.name = part.mesh_node = names[i];
      part.triangles = triangles(mesh);
      part.copies = 1;
      pieces.push_back(std::move(part));
    }
  }
  auto by_name = [](const CatalogPart& a, const CatalogPart& b) { return a.name < b.name; };
  std::sort(prototypes.begin(), prototypes.end(), by_name);
  std::sort(pieces.begin(), pieces.end(), by_name);
  prototypes.insert(prototypes.end(), pieces.begin(), pieces.end());
  out->parts = std::move(prototypes);
  return true;
}

void LibraryIndex::start(const AssetLibrary& library, const std::string& cache_dir, bool write_cache) {
  stop();
  m_levels.clear();
  for (const auto& l : library.levels()) {
    LevelIndex li;
    li.level = l.name;
    li.background = l.background;
    li.stamp = l.mtime;
    m_levels.push_back(std::move(li));
  }
  m_total = (int)m_levels.size();
  m_done = 0;
  m_cancel = false;
  m_ready = false;
  m_work = std::async(std::launch::async, [this, cache_dir, write_cache]() {
    std::error_code ec;
    if (!cache_dir.empty() && write_cache) fs::create_directories(to_path(cache_dir), ec);
    int read = 0;
    for (auto& l : m_levels) {
      if (m_cancel) return;
      const std::string cache = cache_dir.empty() ? std::string() : cache_file(cache_dir, l.background);
      bool cached = false;
      if (!cache.empty()) {
        std::ifstream f(to_path(cache), std::ios::binary);
        if (f) {
          const json j = json::parse(f, nullptr, false);
          LevelIndex c;
          if (!j.is_discarded() && j.value("background", std::string()) == l.background &&
              j.value("stamp", (int64_t)0) == l.stamp && from_json(j, &c)) {
            l.parts = std::move(c.parts);
            l.art_names = std::move(c.art_names);
            l.art_lumps = std::move(c.art_lumps);
            cached = true;
          }
        }
      }
      if (!cached) {
        std::string err;
        LevelIndex fresh;
        if (index_level(l.background, &fresh, &err)) {
          l.parts = std::move(fresh.parts);
          l.art_names = std::move(fresh.art_names);
          l.art_lumps = std::move(fresh.art_lumps);
          if (!cache.empty() && write_cache) {
            std::ofstream f(to_path(cache), std::ios::binary);
            if (f) f << to_json(l).dump();
          }
          read++;
        } else {
          LOG_WARN("%s", err.c_str());
        }
      }
      m_done++;
    }
    if (read) LOG_INFO(tr("log.index_read"), read);
    m_ready = true;
  });
}

std::vector<DecorEntry> all_decor(const LibraryIndex& index, const AssetLibrary& library) {
  std::vector<DecorEntry> out;
  if (!index.ready()) return out;
  std::map<std::string, size_t> first;  // prototype -> its entry
  for (const auto& li : index.levels()) {
    const LevelEntry* level = library.level(li.level);
    if (!level || !level->listed) continue;
    for (const auto& p : li.parts) {
      if (p.kind == CatalogPart::Kind::Prototype) {
        auto it = first.find(p.name);
        if (it != first.end()) {
          out[it->second].levels++;
          continue;
        }
        first[p.name] = out.size();
      }
      DecorEntry e;
      e.level = li.level;
      e.background = li.background;
      e.stamp = level->mtime;
      e.part = p;
      e.key = p.name;
      for (auto& c : e.key) c = (char)std::tolower((unsigned char)c);
      out.push_back(std::move(e));
    }
  }
  return out;
}

void LibraryIndex::stop() {
  m_cancel = true;
  if (m_work.valid()) m_work.wait();
  m_work = std::future<void>();
  m_ready = false;
}

}  // namespace ogle
