// Extraction of the game's assets (File > Extract game assets).
//
// The editor runs ogle-extract, built with it and installed next to its executable, and reads the
// lines it prints (see extractor/main.cpp):
//   ogle-extract --detect <iso or folder>      the game, version and serial of the disc
//   ogle-extract <iso or folder> <library>     every level of the game, to <library>/<game>/
// When it is done, the Levels panel shows that game.

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <optional>
#include <sstream>

#include "app/app.h"
#include "core/i18n.h"
#include "core/log.h"
#include "core/prefs.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "json.hpp"

namespace ogle {

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxLogLines = 400;
const ImVec4 kGood(0.45f, 0.85f, 0.5f, 1.f);
const ImVec4 kBad(1.f, 0.45f, 0.4f, 1.f);

fs::path to_path(const std::string& s) { return fs::path(std::u8string((const char8_t*)s.c_str())); }

std::string time_text(double seconds) {
  int s = (int)std::max(0.0, seconds);
  return strf("%d:%02d", s / 60, s % 60);
}

double now_seconds() { return SDL_GetTicks() / 1000.0; }

const char* game_label(Game g) {
  switch (g) {
    case Game::Jak1: return tr("game.jak1");
    case Game::Jak2: return tr("game.jak2");
    default: return tr("game.jak3");
  }
}

// "levels 147" -> the translation of extract.step.levels with its number
std::string step_text(const std::string& step) {
  std::istringstream ss(step);
  std::string code;
  ss >> code;
  size_t a = 0, b = 0;
  ss >> a >> b;
  const std::string key = "extract.step." + code;
  const char* t = tr(key.c_str());
  if (t == key) return step;  // an unknown step: as ogle-extract said it
  return strf(t, a, b);
}

// "unknown_disc SCES-12345" -> the translation of extract.error.unknown_disc, then the detail
std::string error_text(const std::string& error) {
  const size_t space = error.find(' ');
  const std::string code = error.substr(0, space);
  const std::string detail = space == std::string::npos ? std::string() : error.substr(space + 1);
  const std::string key = "extract.error." + code;
  const char* t = tr(key.c_str());
  if (t == key) return error;
  return detail.empty() ? std::string(t) : std::string(t) + ": " + detail;
}

}  // namespace

std::string App::extractor_path() {
  const char* base = SDL_GetBasePath();
  if (!base) return {};
#ifdef _WIN32
  std::string path = std::string(base) + "ogle-extract.exe";
#else
  std::string path = std::string(base) + "ogle-extract";
#endif
  std::error_code ec;
  return fs::is_regular_file(to_path(path), ec) ? path : std::string();
}

bool App::run_extractor(const std::vector<std::string>& args, bool detecting) {
  extract_stop();
  extract.pending.clear();
  extract.log.clear();
  extract.step.clear();
  extract.current.clear();
  extract.error.clear();
  extract.finished = false;
  extract.done = extract.total = 0;
  extract.detecting = detecting;
  extract.started = extract.ended = now_seconds();
  const std::string exe = extractor_path();
  if (exe.empty()) {
    extract.error = tr("extract.missing");
    return false;
  }
  std::vector<const char*> argv{exe.c_str()};
  for (const auto& a : args) argv.push_back(a.c_str());
  argv.push_back(nullptr);
  SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void*)argv.data());
  SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
  SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
  SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
#ifdef _WIN32
  // no console window of its own (its exit code is then not reported: its OGLE_ lines are)
  SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
#endif
  extract.process = SDL_CreateProcessWithProperties(props);
  SDL_DestroyProperties(props);
  if (!extract.process) {
    extract.error = std::string(tr("extract.cannot_start")) + ": " + SDL_GetError();
    LOG_ERROR("%s", extract.error.c_str());
    return false;
  }
  return true;
}

void App::extract_stop() {
  if (!extract.process) return;
  SDL_KillProcess(extract.process, true);
  SDL_WaitProcess(extract.process, true, nullptr);
  SDL_DestroyProcess(extract.process);
  extract.process = nullptr;
  extract.ended = now_seconds();
}

void App::open_extract_window() {
  extract.show = true;
  if (extract.output.empty()) extract.output = library_root.empty() ? default_library_root() : library_root;
  if (extract.source.empty()) {
    // the game shown by the Levels panel, else the one of the disc chosen last
    Game g = !library.game().empty() ? game_from_name(library.game()) : extract.game;
    extract_select_game(g);
  }
}

void App::extract_select_game(Game g) {
  if (extract.process && !extract.detecting) return;  // an extraction is running
  extract.game = g;
  const auto& sources = Prefs::get().extract_sources;
  auto it = sources.find(game_name(g));
  extract.source = it != sources.end() ? it->second : std::string();
  extract_detect(extract.source);
}

void App::extract_detect(const std::string& source) {
  if (extract.process && !extract.detecting) return;  // an extraction is running
  extract.detected_source = source;
  extract.detected_game.clear();
  extract.detected_name.clear();
  extract.detected_version.clear();
  extract.detected_serial.clear();
  extract.detect_error.clear();
  if (source.empty()) {
    extract_stop();
    extract.log.clear();
    return;
  }
  run_extractor({"--detect", source}, true);
}

void App::start_extraction() {
  Prefs& prefs = Prefs::get();
  prefs.extract_sources[game_name(extract.game)] = extract.source;
  prefs.save();
  if (run_extractor({extract.source, extract.output}, false))
    LOG_INFO(tr("log.extracting"), game_label(extract.game), extract.output.c_str());
}

void App::extract_line(const std::string& line) {
  auto rest = [&](const char* prefix) -> std::optional<std::string> {
    size_t n = strlen(prefix);
    if (line.compare(0, n, prefix) != 0) return std::nullopt;
    return line.substr(n);
  };
  if (auto r = rest("OGLE_GAME ")) {
    auto j = nlohmann::json::parse(*r, nullptr, false);
    if (j.is_object()) {
      extract.detected_game = j.value("game", std::string());
      extract.detected_name = j.value("name", std::string());
      extract.detected_version = j.value("version", std::string());
      extract.detected_serial = j.value("serial", std::string());
    }
  } else if (auto r = rest("OGLE_STEP ")) {
    extract.step = step_text(*r);
  } else if (auto r = rest("OGLE_PROGRESS ")) {
    std::istringstream ss(*r);
    ss >> extract.done >> extract.total;
    std::getline(ss >> std::ws, extract.current);
  } else if (rest("OGLE_DONE ")) {
    extract.finished = true;
  } else if (auto r = rest("OGLE_ERROR ")) {
    extract.error = error_text(*r);
  }
  extract.log.push_back(line);
  if (extract.log.size() > kMaxLogLines)
    extract.log.erase(extract.log.begin(), extract.log.begin() + (extract.log.size() - kMaxLogLines));
}

void App::extract_poll() {
  if (!extract.process) return;
  SDL_IOStream* out = SDL_GetProcessOutput(extract.process);
  auto drain = [&]() {
    char buf[8192];
    for (int i = 0; out && i < 256; i++) {
      size_t n = SDL_ReadIO(out, buf, sizeof(buf));
      if (n == 0) break;  // nothing more for now, or the end
      extract.pending.append(buf, n);
    }
    size_t nl;
    while ((nl = extract.pending.find('\n')) != std::string::npos) {
      std::string line = extract.pending.substr(0, nl);
      extract.pending.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      extract_line(line);
    }
  };
  drain();
  if (!SDL_WaitProcess(extract.process, false, nullptr)) return;
  drain();  // what it wrote before stopping
  if (!extract.pending.empty()) {
    extract_line(extract.pending);
    extract.pending.clear();
  }
  SDL_DestroyProcess(extract.process);
  extract.process = nullptr;
  extract.ended = now_seconds();
  if (extract.detecting) {
    extract.detect_error = extract.error;
    if (extract.detect_error.empty() && extract.detected_game.empty()) extract.detect_error = tr("extract.not_recognized");
    return;
  }
  if (!extract.finished) {
    if (extract.error.empty()) extract.error = tr("extract.stopped");
    LOG_ERROR("%s", extract.error.c_str());
    return;
  }
  // the Levels panel shows what was written (<library>/<game>)
  LOG_INFO(tr("log.extracted"), game_label(extract.game), time_text(extract.ended - extract.started).c_str());
  set_library(extract.output, game_name(extract.game));
  left_tab_request = 0;
}

void App::ui_extract_window() {
  if (!extract.show) return;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                          ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(600, 560), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(1.f);
  if (!ImGui::Begin(tr_id("extract.title").c_str(), &extract.show)) {
    ImGui::End();
    return;
  }
  static const std::string exe = extractor_path();
  if (exe.empty()) {
    ImGui::TextColored(kBad, "%s", tr("extract.missing"));
    const char* base = SDL_GetBasePath();
    ImGui::TextWrapped(tr("extract.missing.help"), base ? base : "?");
    ImGui::End();
    return;
  }
  const bool running = extract.process != nullptr;
  const bool extracting = running && !extract.detecting;
  ImGui::TextWrapped("%s", tr("extract.intro"));

  // 1. the game
  ImGui::SeparatorText(tr("extract.game"));
  ImGui::BeginDisabled(extracting);
  for (Game g : {Game::Jak1, Game::Jak2, Game::Jak3}) {
    if (g != Game::Jak1) ImGui::SameLine();
    if (ImGui::RadioButton(game_label(g), extract.game == g) && extract.game != g) extract_select_game(g);
  }
  ImGui::EndDisabled();
  for (const auto& gf : games)
    if (gf.game == game_name(extract.game))
      ImGui::TextDisabled(tr("extract.already"), gf.levels, gf.path.c_str());

  // 2. the disc
  ImGui::SeparatorText(tr("extract.disc"));
  ImGui::BeginDisabled(extracting);
  const float buttons = ImGui::CalcTextSize(tr("extract.iso_file")).x + ImGui::CalcTextSize(tr("extract.folder")).x +
                        ImGui::GetStyle().FramePadding.x * 4 + ImGui::GetStyle().ItemSpacing.x * 2;
  ImGui::SetNextItemWidth(-buttons);
  ImGui::InputTextWithHint("##source", tr("extract.disc.hint"), &extract.source);
  if (ImGui::IsItemDeactivatedAfterEdit()) extract_detect(extract.source);
  ImGui::SameLine();
  if (ImGui::Button(tr_id("extract.iso_file").c_str())) open_dialog(DialogAction::ExtractSourceIso);
  ImGui::SameLine();
  if (ImGui::Button(tr_id("extract.folder").c_str())) open_dialog(DialogAction::ExtractSourceFolder);
  ImGui::EndDisabled();
  const Game detected = game_from_name(extract.detected_game, Game::Jak1);
  const bool recognized = !extract.detected_game.empty() && extract.detected_source == extract.source;
  const bool matches = recognized && detected == extract.game;
  if (running && extract.detecting) {
    ImGui::TextDisabled("%s", tr("extract.detecting"));
  } else if (recognized) {
    ImGui::TextColored(matches ? kGood : kBad, "%s", extract.detected_name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s  -  %s", extract.detected_version.c_str(), extract.detected_serial.c_str());
    if (!matches) ImGui::TextColored(kBad, tr("extract.wrong_game"), game_label(detected), game_label(extract.game));
  } else if (!extract.detect_error.empty() && extract.detected_source == extract.source) {
    ImGui::TextColored(kBad, "%s", extract.detect_error.c_str());
  } else if (extract.source.empty()) {
    ImGui::TextDisabled(tr("extract.disc.choose"), game_label(extract.game));
  }

  // 3. where
  ImGui::SeparatorText(tr("extract.destination"));
  ImGui::BeginDisabled(extracting);
  ImGui::SetNextItemWidth(-ImGui::CalcTextSize(tr("extract.choose")).x - ImGui::GetStyle().FramePadding.x * 2 -
                          ImGui::GetStyle().ItemSpacing.x);
  ImGui::InputTextWithHint("##output", tr("extract.destination.hint"), &extract.output);
  ImGui::SameLine();
  if (ImGui::Button(tr_id("extract.choose").c_str())) open_dialog(DialogAction::ExtractOutput);
  ImGui::EndDisabled();
  if (!extract.output.empty())
    ImGui::TextDisabled(tr("extract.destination.where"), extract.output.c_str(), game_name(extract.game));

  ImGui::Spacing();
  ImGui::BeginDisabled(running || !matches || extract.output.empty());
  if (ImGui::Button(tr_id("extract.start").c_str(), ImVec2(260, 36))) start_extraction();
  ImGui::EndDisabled();

  // progress and outcome of an extraction
  if (!extract.detecting && (running || !extract.log.empty())) {
    ImGui::Spacing();
    const double elapsed = (running ? now_seconds() : extract.ended) - extract.started;
    if (running) {
      float fraction = extract.total ? (float)extract.done / (float)extract.total : 0.f;
      std::string text = extract.total ? strf("%zu / %zu  %s", extract.done, extract.total, extract.current.c_str())
                                       : std::string(tr("extract.preparing"));
      ImGui::ProgressBar(fraction, ImVec2(-110, 0), text.c_str());
      ImGui::SameLine();
      if (ImGui::Button(tr_id("extract.cancel").c_str(), ImVec2(-1, 0))) {
        extract_stop();
        extract.error = tr("extract.cancelled");
      }
      ImGui::TextDisabled("%s  -  %s", extract.step.c_str(), time_text(elapsed).c_str());
    } else if (extract.finished) {
      ImGui::TextColored(kGood, tr("extract.done"), time_text(elapsed).c_str(), extract.done > 0 ? extract.done - 1 : 0);
    } else if (!extract.error.empty()) {
      ImGui::TextColored(kBad, "%s", extract.error.c_str());
    }
  }
  if (!extract.log.empty() && ImGui::CollapsingHeader(tr_id("extract.log").c_str())) {
    ImGui::BeginChild("extract_log", ImVec2(0, 150), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& l : extract.log) ImGui::TextUnformatted(l.c_str());
    if (running && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
    if (ImGui::SmallButton(tr_id("log.copy").c_str())) {
      std::string all;
      for (const auto& l : extract.log) all += l + "\n";
      ImGui::SetClipboardText(all.c_str());
    }
  }
  ImGui::End();
}

}  // namespace ogle
