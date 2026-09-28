#pragma once

// What the catalog needs of every level of the library without loading them: the decor parts of
// each level (the decor of all the levels, searched by name) and the art-names of its actors (the
// models and particle effects they choose by name). Read from the JSON of the levels' background
// files only, not their geometry, on a worker thread, and kept in a cache folder (the editor's:
// <preferences>/index) so that a level is read again only when its file changes.

#include <atomic>
#include <future>
#include <map>
#include <string>
#include <vector>

#include "io/asset_library.h"
#include "io/catalog.h"

namespace ogle {

struct LevelIndex {
  std::string level;       // the folder ("atoll")
  std::string background;  // its background file
  int64_t stamp = 0;       // the file's date when it was read
  std::vector<CatalogPart> parts;  // as list_parts() gives them for the loaded level
  // per actor type: the art-names of its actors, how many have each and the data (lump) of the
  // first one
  std::map<std::string, std::map<std::string, int>> art_names;
  std::map<std::string, std::map<std::string, json>> art_lumps;
};

// A decor part of a level of the library, for the decor of all the levels.
struct DecorEntry {
  std::string level;       // the folder it comes from
  std::string background;  // that level's background file
  int64_t stamp = 0;       // its date (thumbnails)
  CatalogPart part;
  std::string key;         // the name in lower case, for the filter
  int levels = 1;          // the levels that have a prototype of that name
};

// Reads a level's background file (its JSON chunk only).
bool index_level(const std::string& background, LevelIndex* out, std::string* error);

class LibraryIndex {
 public:
  ~LibraryIndex() { stop(); }
  // Reads the levels of `library` on a worker thread (the ones cached in `cache_dir` from there,
  // "" for no cache), after forgetting the previous library's. The levels read are added to the
  // cache when `write_cache` (not by captures and tests: their libraries are temporary).
  void start(const AssetLibrary& library, const std::string& cache_dir, bool write_cache = true);
  void stop();
  bool ready() const { return m_ready; }
  int done() const { return m_done; }
  int total() const { return m_total; }
  // Once ready: every level of the library, in its order.
  const std::vector<LevelIndex>& levels() const { return m_levels; }

 private:
  std::future<void> m_work;
  std::atomic<bool> m_ready{false};
  std::atomic<bool> m_cancel{false};
  std::atomic<int> m_done{0};
  int m_total = 0;
  std::vector<LevelIndex> m_levels;
};

// The decor parts of the levels `library` lists, in its order: a prototype once (from the first
// level that has one), the pieces of every level.
std::vector<DecorEntry> all_decor(const LibraryIndex& index, const AssetLibrary& library);

}  // namespace ogle
