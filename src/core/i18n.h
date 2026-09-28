#pragma once

// Translations of the interface. Each language is a json file in the lang/ folder next to the
// executable (lang/en.json, lang/fr.json...), mapping keys to texts:
//
//   {"_language": "Français", "menu.file": "Fichier", "status.objects": "%zu objets", ...}
//
// en.json holds every key. A key missing from another language falls back to English, then to the
// key itself. A text with printf arguments keeps the same arguments in every language (the
// selftest checks it). Adding a language: copy en.json to <code>.json and translate the values.

#include <string>
#include <vector>

namespace ogle {

struct Language {
  std::string code;  // file name without .json: "en", "fr"
  std::string name;  // its "_language" entry: "English", "Français"
};

// Reads the languages of `dir` and selects `code` (English when there is no such file).
void i18n_init(const std::string& dir, const std::string& code);
bool i18n_select(const std::string& code);
const std::string& i18n_language();             // code of the selected language
const std::vector<Language>& i18n_languages();  // the files of the folder, English first
// The language the system prefers among those with a file, else "en".
std::string i18n_system_language();

// The text of a key in the selected language.
const char* tr(const char* key);
// An ImGui label whose id does not change with the language: "<text>###<key>".
std::string tr_id(const char* key);

}  // namespace ogle
