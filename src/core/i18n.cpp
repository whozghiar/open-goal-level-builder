#include "core/i18n.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>
#include <unordered_map>

#include "core/log.h"
#include "json.hpp"

namespace ogle {

namespace fs = std::filesystem;

namespace {

struct Hash {
  using is_transparent = void;
  size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
};

struct Table {
  Language lang;
  std::unordered_map<std::string, std::string, Hash, std::equal_to<>> texts;
};

// Tables are never freed: the pointers tr() returns stay valid when the language changes.
std::vector<std::unique_ptr<Table>> g_tables;
std::vector<Language> g_languages;
std::atomic<const Table*> g_current{nullptr};
std::atomic<const Table*> g_english{nullptr};
std::string g_code = "en";

const Table* find_table(const std::string& code) {
  for (const auto& t : g_tables)
    if (t->lang.code == code) return t.get();
  return nullptr;
}

}  // namespace

void i18n_init(const std::string& dir, const std::string& code) {
  std::error_code ec;
  const fs::path base(std::u8string((const char8_t*)dir.c_str()));
  for (const auto& e : fs::directory_iterator(base, ec)) {
    if (!e.is_regular_file(ec) || e.path().extension() != ".json") continue;
    const std::string lang_code = e.path().stem().string();
    if (find_table(lang_code)) continue;
    std::ifstream f(e.path(), std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) {
      LOG_WARN("%s: not a translation file", e.path().string().c_str());
      continue;
    }
    auto t = std::make_unique<Table>();
    t->lang.code = lang_code;
    t->lang.name = j.value("_language", lang_code);
    for (const auto& [k, v] : j.items())
      if (v.is_string()) t->texts.emplace(k, v.get<std::string>());
    g_tables.push_back(std::move(t));
  }
  g_languages.clear();
  for (const auto& t : g_tables) g_languages.push_back(t->lang);
  std::sort(g_languages.begin(), g_languages.end(), [](const Language& a, const Language& b) {
    return (a.code == "en") != (b.code == "en") ? a.code == "en" : a.name < b.name;
  });
  g_english = find_table("en");
  if (!g_english.load()) LOG_WARN("no English translation in %s: the interface shows its keys", dir.c_str());
  if (!i18n_select(code)) i18n_select("en");
}

bool i18n_select(const std::string& code) {
  const Table* t = find_table(code);
  if (!t) return false;
  g_current = t;
  g_code = code;
  return true;
}

const std::string& i18n_language() { return g_code; }

const std::vector<Language>& i18n_languages() { return g_languages; }

std::string i18n_system_language() {
  int count = 0;
  SDL_Locale** locales = SDL_GetPreferredLocales(&count);
  std::string found = "en";
  for (int i = 0; locales && i < count; i++) {
    if (locales[i] && locales[i]->language && find_table(locales[i]->language)) {
      found = locales[i]->language;
      break;
    }
  }
  SDL_free(locales);
  return found;
}

const char* tr(const char* key) {
  for (const Table* t : {g_current.load(), g_english.load()}) {
    if (!t) continue;
    auto it = t->texts.find(std::string_view(key));
    if (it != t->texts.end()) return it->second.c_str();
  }
  return key;
}

std::string tr_id(const char* key) { return std::string(tr(key)) + "###" + key; }

}  // namespace ogle
