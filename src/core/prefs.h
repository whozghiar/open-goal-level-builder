#pragma once

// The editor's preferences, kept between sessions in the user's settings folder
// (%APPDATA%/OpenGOAL/open-goal-level-editor on Windows, ~/.local/share/... on Linux): where the
// extracted games are, the discs they came from, the language, recent projects. Thumbnails are
// cached next to them.

#include <map>
#include <string>
#include <vector>

namespace ogle {

struct Prefs {
  // the folder ogle-extract writes to: one subfolder per extracted game (jak1/, jak2/, jak3/)
  std::string library_root;
  std::string library_game;                            // game shown by the Levels panel ("jak2")
  std::map<std::string, std::string> extract_sources;  // game -> the user's .iso or disc folder
  std::string language;                                // "" = the system's
  std::vector<std::string> recent_projects;
  // the command line modes (captures, tests) read the preferences, never write them
  static inline bool read_only = false;

  // folder of the preferences and of the thumbnail cache (created on demand), '/' separated
  static std::string dir();
  static Prefs& get();
  void load();
  void save() const;
  void add_recent_project(const std::string& path);
};

}  // namespace ogle
