// open-goal-level-editor: a simple level editor for OpenGOAL (Jak 1, Jak 2, Jak 3).
//
//   open-goal-level-editor [project.glb] [--library <folder>] [--lang en|fr]
//   open-goal-level-editor --screenshot <level|disc|-> <image.png> [setup] [--size WxH] [--library <folder>]
//                          [--lang code] [--camera x,y,z,yaw,pitch] [--story off|<step>|<node>]
//                          [--select a,b,...]
//   open-goal-level-editor --mcp [--remote-port <port>]
//   open-goal-level-editor --selftest

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include "app/app.h"
#include "core/log.h"
#include "core/prefs.h"

namespace ogle {
int run_selftest();
int run_mcp_server(int port);
}

using namespace ogle;

namespace {

std::vector<std::string> utf8_args(int argc, char** argv) {
  std::vector<std::string> out;
#ifdef _WIN32
  // argv is in the ANSI code page on Windows: take the UTF-16 command line instead
  int n = 0;
  LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
  if (w) {
    for (int i = 0; i < n; i++) {
      int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
      std::string s(len > 0 ? len - 1 : 0, '\0');
      if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
      out.push_back(s);
    }
    LocalFree(w);
    return out;
  }
#endif
  for (int i = 0; i < argc; i++) out.push_back(argv[i]);
  return out;
}

void usage() {
  std::printf(
      "open-goal-level-editor\n"
      "  [project.glb]                        opens a project\n"
      "  --library <folder>                   the folder of the extracted games (for this session)\n"
      "  --lang <code>                        the interface language (for this session): en, fr...\n"
      "  --screenshot <in> <image.png> [setup] [--size WxH] [--camera x,y,z,yaw,pitch]\n"
      "               [--story off|<step>|<node>] [--select a,b,...]\n"
      "                                       runs the editor in a hidden window and saves a capture;\n"
      "                                       setup: levels, select, scale, prefab, catalog, models,\n"
      "                                       decor, decor-all (<in> = a level name of the library),\n"
      "                                       project (<in> = a project .glb; select, scale and prefab\n"
      "                                       take one too), extract (<in> = an .iso or a disc folder),\n"
      "                                       help; --story: every state, a step, or just after a task\n"
      "                                       node; --select: the elements select, scale and prefab use\n"
      "  --mcp                                an MCP server (stdin/stdout) giving an assistant the editor's\n"
      "                                       tools; it starts the editor when none is running\n"
      "  --remote-port <port>                 the editor's port for the MCP server (47821; 0: none)\n"
      "  --hidden                             no window, the preferences left unchanged (tests)\n"
      "  --selftest                           internal tests\n");
}

}  // namespace

int main(int argc, char** argv) {
  auto args = utf8_args(argc, argv);
  AppOptions opts;
  std::string mode;
  std::vector<std::string> positional;
  int width = 1600, height = 900;
  for (size_t i = 1; i < args.size(); i++) {
    const std::string& a = args[i];
    auto next = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
    if (a == "--help" || a == "-h") {
      usage();
      return 0;
    } else if (a == "--selftest" || a == "--screenshot" || a == "--mcp") {
      mode = a;
    } else if (a == "--library") {
      opts.library = next();
    } else if (a == "--lang") {
      opts.language = next();
    } else if (a == "--camera") {
      std::string s = next();
      opts.camera_set = std::sscanf(s.c_str(), "%f,%f,%f,%f,%f", &opts.camera_pos.x, &opts.camera_pos.y,
                                    &opts.camera_pos.z, &opts.camera_yaw, &opts.camera_pitch) == 5;
    } else if (a == "--remote-port") {
      opts.remote_port = std::atoi(next().c_str());
    } else if (a == "--hidden") {
      opts.hidden = true;
    } else if (a == "--story") {
      opts.story = next();
    } else if (a == "--select") {
      opts.select = next();
    } else if (a == "--size") {
      std::string s = next();
      std::sscanf(s.c_str(), "%dx%d", &width, &height);
    } else {
      positional.push_back(a);
    }
  }

  // only the editor itself keeps what the user changes (not the command line modes, nor a hidden
  // editor of the tests)
  if (!mode.empty() || opts.hidden) Prefs::read_only = true;
  if (mode == "--selftest") return run_selftest();
  if (mode == "--mcp") return run_mcp_server(opts.remote_port > 0 ? opts.remote_port : kRemotePort);
  if (mode == "--screenshot") {
    if (positional.size() < 2) {
      usage();
      return 1;
    }
    App app;
    return app.ui_screenshot(opts, positional[0], positional[1], width, height,
                             positional.size() > 2 ? positional[2] : "levels");
  }
  if (!positional.empty()) opts.open_path = positional[0];
  App app;
  return app.run(opts);
}
