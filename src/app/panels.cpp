// The editor's interface: menus, toolbar, the Levels and Prefabs panels on the left, the inspector
// on the right, the status bar and the floating windows. Every text comes from the translations
// (core/i18n.h, lang/*.json).

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "app/app.h"
#include "core/i18n.h"
#include "core/log.h"
#include "core/prefs.h"
#include "imgui.h"
#include "imgui_stdlib.h"

namespace ogle {

namespace fs = std::filesystem;

namespace {

fs::path to_path(const std::string& s) { return fs::path(std::u8string((const char8_t*)s.c_str())); }

std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

// the longest prefix of `s` that fits in `width` pixels, with "..." when cut
std::string fit_text(const std::string& s, float width) {
  if (ImGui::CalcTextSize(s.c_str()).x <= width) return s;
  std::string t = s;
  while (!t.empty() && ImGui::CalcTextSize((t + "...").c_str()).x > width) t.pop_back();
  return t + "...";
}

void tooltip(const char* text) {
  if (ImGui::BeginItemTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

const char* kind_label(NodeKind k) {
  switch (k) {
    case NodeKind::Render: return tr("kind.render");
    case NodeKind::Collision: return tr("kind.collision");
    case NodeKind::Actor: return tr("kind.actor");
    default: return tr("kind.group");
  }
}

const char* game_label(Game g) {
  switch (g) {
    case Game::Jak1: return tr("game.jak1");
    case Game::Jak2: return tr("game.jak2");
    default: return tr("game.jak3");
  }
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// undoable property edits
// ------------------------------------------------------------------------------------------------

void App::commit_node_edit(const Node& before, const std::string& label) {
  const Node* now = scene.find(before.id);
  if (!now) return;
  Node after = *now;
  Scene* s = &scene;
  uint32_t id = before.id;
  history.push({label,
                [s, id, before]() {
                  if (Node* n = s->find(id)) *n = before;
                },
                [s, id, after]() {
                  if (Node* n = s->find(id)) *n = after;
                }});
}

void App::edit_begin(const Node& n) {
  if (ImGui::IsItemActivated()) m_edit_before = n;
}

void App::edit_end(const std::string& label) {
  if (ImGui::IsItemDeactivatedAfterEdit() && m_edit_before) {
    commit_node_edit(*m_edit_before, label);
    m_edit_before.reset();
  } else if (ImGui::IsItemDeactivated()) {
    m_edit_before.reset();
  }
}

// ------------------------------------------------------------------------------------------------
// layout
// ------------------------------------------------------------------------------------------------

void App::draw_ui() {
  ImGuiIO& io = ImGui::GetIO();
  const float W = io.DisplaySize.x, H = io.DisplaySize.y;
  ui_menu();
  const float menu_h = ImGui::GetFrameHeight();
  const float tb_h = ImGui::GetFrameHeight() + 8.f + ImGui::GetStyle().WindowPadding.y * 2.f;
  ui_toolbar(menu_h, tb_h);
  const float top = menu_h + tb_h;
  const float status_h = ImGui::GetFrameHeight() + 8.f;
  const float panel_h = std::max(H - top - status_h, 100.f);
  m_left_w = std::clamp(m_left_w, 260.f, W * 0.45f);
  m_right_w = std::clamp(m_right_w, 260.f, W * 0.4f);
  ui_left_panel(0, top, m_left_w, panel_h);
  ui_inspector(W - m_right_w, top, m_right_w, panel_h);
  ui_statusbar(H - status_h, status_h);
  m_vp_x = m_left_w;
  m_vp_y = top;
  m_vp_w = std::max(W - m_left_w - m_right_w, 50.f);
  m_vp_h = panel_h;
  ui_story_bar();
  ui_windows();
  ui_extract_window();
}

void App::ui_menu() {
  if (!ImGui::BeginMainMenuBar()) return;
  const bool sel = !selection.empty();
  if (ImGui::BeginMenu(tr_id("menu.file").c_str())) {
    if (ImGui::MenuItem(tr("menu.file.extract"))) open_extract_window();
    ImGui::Separator();
    if (ImGui::MenuItem(tr("menu.file.new"), "Ctrl+N")) confirm_discard([this]() { new_level(); });
    if (ImGui::MenuItem(tr("menu.file.open"), "Ctrl+O"))
      confirm_discard([this]() { open_dialog(DialogAction::OpenProject); });
    const auto& recent = Prefs::get().recent_projects;
    if (ImGui::BeginMenu(tr_id("menu.file.recent").c_str(), !recent.empty())) {
      for (const auto& p : recent)
        if (ImGui::MenuItem(p.c_str())) confirm_discard([this, p]() { open_project_file(p); });
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem(tr("menu.file.save"), "Ctrl+S", false, document_open())) save(false);
    if (ImGui::MenuItem(tr("menu.file.save_as"), "Ctrl+Shift+S", false, document_open())) save(true);
    ImGui::Separator();
    if (ImGui::MenuItem(tr("menu.file.quit"))) request_quit();
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(tr_id("menu.edit").c_str())) {
    if (ImGui::MenuItem(tr("menu.edit.undo"), "Ctrl+Z", false, history.can_undo())) history.undo();
    if (ImGui::MenuItem(tr("menu.edit.redo"), "Ctrl+Y", false, history.can_redo())) history.redo();
    ImGui::Separator();
    if (ImGui::MenuItem(tr("menu.edit.copy"), "Ctrl+C", false, sel)) copy_selection();
    if (ImGui::MenuItem(tr("menu.edit.paste"), "Ctrl+V", false, !clipboard.empty())) paste();
    if (ImGui::MenuItem(tr("menu.edit.duplicate"), "Ctrl+D", false, sel)) duplicate_selection();
    if (ImGui::MenuItem(tr("menu.edit.delete"), tr("key.delete"), false, sel)) delete_selection();
    ImGui::Separator();
    if (ImGui::MenuItem(tr("menu.edit.create_prefab"), nullptr, false, sel)) m_show_new_prefab = true;
    if (ImGui::MenuItem(tr("menu.edit.select_none"), tr("key.escape"), false, sel)) select_only(0);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(tr_id("menu.view").c_str())) {
    if (ImGui::MenuItem(tr("menu.view.frame_selection"), "F", false, sel)) frame_selection();
    if (ImGui::MenuItem(tr("menu.view.frame_all"), tr("key.home"))) frame_all();
    ImGui::Separator();
    ImGui::MenuItem(tr("menu.view.actors"), nullptr, &rs.show_actors);
    ImGui::MenuItem(tr("menu.view.ocean"), nullptr, &rs.show_ocean);
    ImGui::MenuItem(tr("menu.view.labels"), nullptr, &show_labels);
    ImGui::MenuItem(tr("menu.view.grid"), nullptr, &rs.show_grid);
    ImGui::MenuItem(tr("menu.view.textures"), nullptr, &rs.textures);
    ImGui::MenuItem(tr("menu.view.log"), nullptr, &show_log);
    ImGui::Separator();
    if (ImGui::BeginMenu(tr_id("menu.view.language").c_str())) {
      for (const auto& l : i18n_languages())
        if (ImGui::MenuItem(l.name.c_str(), nullptr, l.code == i18n_language())) set_language(l.code);
      ImGui::EndMenu();
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(tr_id("menu.help").c_str())) {
    if (ImGui::MenuItem(tr("menu.help.getting_started"), "F1")) show_help = true;
    ImGui::EndMenu();
  }
  ImGui::EndMainMenuBar();
}

void App::ui_toolbar(float y, float h) {
  ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(0, y));
  ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, h));
  ImGui::Begin("##toolbar", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);
  const float bh = ImGui::GetFrameHeight() + 8.f;
  const bool sel = !selection.empty();
  auto button = [&](const char* key, const char* tip_key, bool active, bool enabled) {
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.6f, 0.66f, 1.f));
    ImGui::BeginDisabled(!enabled);
    bool pressed = ImGui::Button(tr_id(key).c_str(), ImVec2(0, bh));
    ImGui::EndDisabled();
    if (active) ImGui::PopStyleColor();
    if (tip_key) tooltip(tr(tip_key));
    ImGui::SameLine();
    return pressed;
  };
  if (button("toolbar.move", "toolbar.move.tip", gizmo.op == GizmoOp::Translate, true)) {
    stop_placement();
    gizmo.op = GizmoOp::Translate;
  }
  if (button("toolbar.rotate", "toolbar.rotate.tip", gizmo.op == GizmoOp::Rotate, true)) {
    stop_placement();
    gizmo.op = GizmoOp::Rotate;
  }
  if (button("toolbar.scale", "toolbar.scale.tip", gizmo.op == GizmoOp::Scale, true)) {
    stop_placement();
    gizmo.op = GizmoOp::Scale;
  }
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
  ImGui::Checkbox(tr_id("toolbar.snap").c_str(), &gizmo.snap.enabled);
  tooltip(tr("toolbar.snap.tip"));
  ImGui::SameLine();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 4);
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  if (button("toolbar.copy", "toolbar.copy.tip", false, sel)) copy_selection();
  if (button("toolbar.paste", "toolbar.paste.tip", false, !clipboard.empty())) paste();
  if (button("toolbar.delete", "toolbar.delete.tip", false, sel)) delete_selection();
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  if (button("toolbar.prefab", "toolbar.prefab.tip", false, sel)) m_show_new_prefab = true;
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  if (button("toolbar.undo", nullptr, false, history.can_undo())) history.undo();
  if (button("toolbar.redo", nullptr, false, history.can_redo())) history.redo();
  ImGui::End();
}

void App::ui_left_panel(float x, float y, float w, float h) {
  ImGui::SetNextWindowPos(ImVec2(x, y));
  ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(260, h), ImVec2(ImGui::GetIO().DisplaySize.x * 0.45f, h));
  ImGui::Begin("##left", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus);
  m_left_w = ImGui::GetWindowWidth();
  if (ImGui::BeginTabBar("panels")) {
    const char* tabs[] = {"tab.levels", "tab.catalog", "tab.prefabs"};
    for (int i = 0; i < 3; i++) {
      ImGuiTabItemFlags flags = left_tab_request == i ? ImGuiTabItemFlags_SetSelected : 0;
      if (ImGui::BeginTabItem(tr_id(tabs[i]).c_str(), nullptr, flags)) {
        if (i == 0) ui_levels_panel();
        else if (i == 1) ui_catalog_panel();
        else ui_prefabs_panel();
        ImGui::EndTabItem();
      }
    }
    left_tab_request = -1;
    ImGui::EndTabBar();
  }
  ImGui::End();
}

// ------------------------------------------------------------------------------------------------
// levels
// ------------------------------------------------------------------------------------------------

void App::ui_levels_panel() {
  if (games.empty()) {
    ImGui::Spacing();
    ImGui::TextWrapped("%s", tr("levels.no_library"));
    ImGui::Spacing();
    if (ImGui::Button(tr_id("levels.extract").c_str(), ImVec2(-1, 40))) open_extract_window();
    if (!library_root.empty()) {
      ImGui::Spacing();
      ImGui::TextDisabled("%s", tr("levels.folder_empty"));
      ImGui::TextWrapped("%s", library_root.c_str());
    }
    return;
  }
  // the game
  const GameFolder* current = nullptr;
  for (const auto& g : games)
    if (g.path == library.root()) current = &g;
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##game", current ? current->name.c_str() : "?")) {
    for (const auto& g : games) {
      std::string label = g.name + "##" + g.game;
      if (ImGui::Selectable(label.c_str(), current == &g) && current != &g) set_library(library_root, g.game);
    }
    ImGui::EndCombo();
  }
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##filter", tr("levels.filter"), &m_levels_filter);
  const std::string f = lower(m_levels_filter);
  std::vector<const LevelEntry*> shown;
  size_t listed = 0;
  for (const auto& l : library.levels()) {
    if (!l.listed) continue;
    listed++;
    if (f.empty() || l.name.find(f) != std::string::npos || lower(l.dgo).find(f) != std::string::npos)
      shown.push_back(&l);
  }
  ImGui::TextDisabled(tr("levels.count"), shown.size(), listed);

  const bool loading = m_level_future.valid();
  const LevelEntry* selected = library.level(m_selected_level);
  const float preview = std::min(ImGui::GetContentRegionAvail().x, 180.f);
  const float footer = selected ? preview + ImGui::GetFrameHeightWithSpacing() * 3 : ImGui::GetFrameHeightWithSpacing() * 2;
  ImGui::BeginChild("levels", ImVec2(0, -footer), ImGuiChildFlags_Borders);
  ImGuiListClipper clipper;
  clipper.Begin((int)shown.size());
  while (clipper.Step()) {
    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
      const LevelEntry* l = shown[i];
      const bool is_open = l->name == scene.settings.level;
      if (ImGui::Selectable((l->name + "##" + l->name).c_str(), l->name == m_selected_level,
                            ImGuiSelectableFlags_AllowDoubleClick)) {
        m_selected_level = l->name;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !loading) {
          LevelEntry entry = *l;
          confirm_discard([this, entry]() { load_level(entry); });
        }
      }
      ImGui::SameLine();
      if (is_open) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1.f), "%s", tr("levels.open_mark"));
      else ImGui::TextDisabled("%s", l->dgo.c_str());
    }
  }
  if (shown.empty()) ImGui::TextDisabled("%s", tr("levels.none_found"));
  ImGui::EndChild();

  // the chosen level: a picture and the button that loads it
  if (selected) {
    ThumbRequest tr_req;
    tr_req.kind = ThumbRequest::Kind::Level;
    tr_req.path = selected->background;
    tr_req.stamp = selected->mtime;
    unsigned tex = thumbs.get(tr_req);
    if (tex) ImGui::Image((ImTextureID)(uint64_t)tex, ImVec2(preview, preview));
    else ImGui::Button(thumbs.failed(tr_req) ? "?" : "...", ImVec2(preview, preview));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted(selected->name.c_str());
    ImGui::TextDisabled("%s", selected->dgo.c_str());
    ImGui::EndGroup();
  }
  ImGui::BeginDisabled(!selected || loading);
  if (ImGui::Button(loading ? tr_id("levels.loading").c_str() : tr_id("levels.load").c_str(), ImVec2(-1, 34)) &&
      selected) {
    LevelEntry entry = *selected;
    confirm_discard([this, entry]() { load_level(entry); });
  }
  ImGui::EndDisabled();
  if (ImGui::SmallButton(tr_id("levels.extract_more").c_str())) open_extract_window();
}

// ------------------------------------------------------------------------------------------------
// catalog
// ------------------------------------------------------------------------------------------------

void App::ui_catalog_panel() {
  if (library.empty()) {
    ImGui::Spacing();
    ImGui::TextWrapped("%s", tr("catalog.no_library"));
    return;
  }
  if (ImGui::RadioButton(tr_id("catalog.objects").c_str(), m_catalog_tab == 0)) m_catalog_tab = 0;
  tooltip(tr("catalog.objects.tip"));
  ImGui::SameLine();
  if (ImGui::RadioButton(tr_id("catalog.models").c_str(), m_catalog_tab == 1)) m_catalog_tab = 1;
  tooltip(tr("catalog.models.tip"));
  ImGui::SameLine();
  if (ImGui::RadioButton(tr_id("catalog.decor").c_str(), m_catalog_tab == 2)) m_catalog_tab = 2;
  tooltip(tr("catalog.decor.tip"));
  if (m_catalog_tab == 0) ui_catalog_objects();
  else if (m_catalog_tab == 1) ui_catalog_models();
  else ui_catalog_decor();
}

namespace {

// A grid of thumbnails with their names, rows outside the view skipped (the catalog has
// hundreds of cells). `cell(i)` draws cell i: its button and its name.
void thumbnail_grid(int count, float size, const std::function<void(int)>& cell) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float cell_w = size + 6 + style.ItemSpacing.x;
  const int per_row = std::max(1, (int)((ImGui::GetContentRegionAvail().x + style.ItemSpacing.x) / cell_w));
  const int rows = (count + per_row - 1) / per_row;
  const float row_h = size + 6 + ImGui::GetTextLineHeightWithSpacing() + style.ItemSpacing.y;
  ImGuiListClipper clipper;
  clipper.Begin(rows, row_h);
  while (clipper.Step()) {
    for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; r++) {
      for (int c = 0; c < per_row; c++) {
        const int i = r * per_row + c;
        if (i >= count) break;
        if (c) ImGui::SameLine();
        ImGui::PushID(i);
        ImGui::BeginGroup();
        cell(i);
        ImGui::EndGroup();
        ImGui::PopID();
      }
    }
  }
}

}  // namespace

void App::ui_catalog_objects() {
  // the family, the filter
  std::vector<std::string> families;
  for (const auto& o : catalog)
    if (families.empty() || families.back() != o.family) families.push_back(o.family);
  ImGui::SetNextItemWidth(-1);
  const std::string current = m_catalog_family.empty() ? tr("catalog.all_families") : tr(("family." + m_catalog_family).c_str());
  if (ImGui::BeginCombo("##family", current.c_str())) {
    if (ImGui::Selectable(tr("catalog.all_families"), m_catalog_family.empty())) m_catalog_family.clear();
    for (const auto& f : families)
      if (ImGui::Selectable(tr(("family." + f).c_str()), m_catalog_family == f)) m_catalog_family = f;
    ImGui::EndCombo();
  }
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##catalog-filter", tr("catalog.filter"), &m_catalog_filter);
  const std::string f = lower(m_catalog_filter);
  std::vector<const CatalogObject*> shown;
  for (const auto& o : catalog) {
    if (!m_catalog_family.empty() && o.family != m_catalog_family) continue;
    if (!f.empty() && o.name.find(f) == std::string::npos && o.etype.find(f) == std::string::npos &&
        o.model.find(f) == std::string::npos)
      continue;
    shown.push_back(&o);
  }
  ImGui::TextDisabled(tr("catalog.count"), shown.size(), catalog.size());
  if (catalog.empty()) {
    ImGui::TextWrapped("%s", tr("catalog.empty"));
    return;
  }
  ImGui::BeginChild("objects");
  const float size = 80.f;
  thumbnail_grid((int)shown.size(), size, [&](int i) {
    const CatalogObject& o = *shown[i];
    ThumbRequest req;
    req.kind = ThumbRequest::Kind::Model;
    req.path = o.model_path;
    unsigned tex = o.model_path.empty() ? 0 : thumbs.get(req);
    const bool placing = tool == ToolMode::Place && placement.kind == Placement::Kind::Object &&
                         placement.object.name == o.name && placement.object.etype == o.etype;
    if (placing) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3, 3));
    const char* label = !o.model_path.empty()   ? (thumbs.failed(req) ? "?" : "...")
                        : o.family == "effect" ? tr("catalog.effect")
                                               : tr("catalog.marker");
    const bool clicked = tex ? ImGui::ImageButton("##img", (ImTextureID)(uint64_t)tex, ImVec2(size, size))
                             : ImGui::Button((std::string(label) + "##img").c_str(), ImVec2(size + 6, size + 6));
    ImGui::PopStyleVar();
    if (placing) ImGui::PopStyleColor();
    if (ImGui::BeginItemTooltip()) {
      ImGui::TextUnformatted(o.name.c_str());
      ImGui::TextDisabled("%s", tr(("family." + o.family).c_str()));
      ImGui::Text(tr("inspector.type"), o.etype.c_str());
      if (o.family == "effect") {
        ImGui::Text(tr("catalog.particles"), o.lump.value("art-name", std::string()).c_str());
        ImGui::TextDisabled("%s", tr("catalog.effect_tip"));
      } else if (o.model.empty()) {
        ImGui::TextDisabled("%s", tr("catalog.no_model"));
      } else {
        ImGui::Text(tr("inspector.model"), o.model.c_str());
      }
      if (o.count) {
        std::string levels;
        for (const auto& l : o.levels) levels += (levels.empty() ? "" : ", ") + l;
        ImGui::TextWrapped(tr("catalog.found"), o.count, levels.c_str());
      } else {
        ImGui::TextDisabled("%s", tr("catalog.not_found"));
      }
      ImGui::TextDisabled("%s", tr("catalog.place_tip"));
      ImGui::EndTooltip();
    }
    if (clicked) start_object_placement(o, false);
    if (ImGui::BeginDragDropSource()) {
      if (!m_drag_from_panel) {
        m_drag_from_panel = true;
        start_object_placement(o, true);
      }
      int dummy = 0;
      ImGui::SetDragDropPayload("OGLE_PREFAB", &dummy, sizeof(dummy));
      ImGui::TextUnformatted(o.name.c_str());
      ImGui::EndDragDropSource();
    }
    ImGui::TextUnformatted(fit_text(o.name, size + 6).c_str());
  });
  ImGui::EndChild();
}

void App::ui_catalog_models() {
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##models-filter", tr("catalog.filter"), &m_catalog_filter);
  const std::string f = lower(m_catalog_filter);
  std::vector<const CatalogModel*> shown;
  for (const auto& m : catalog_models)
    if (f.empty() || m.name.find(f) != std::string::npos || m.group.find(f) != std::string::npos) shown.push_back(&m);
  ImGui::TextDisabled(tr("catalog.count"), shown.size(), catalog_models.size());
  ImGui::BeginChild("models");
  const float size = 80.f;
  thumbnail_grid((int)shown.size(), size, [&](int i) {
    const CatalogModel& m = *shown[i];
    ThumbRequest req;
    req.kind = ThumbRequest::Kind::Model;
    req.path = m.path;
    unsigned tex = thumbs.get(req);
    const bool placing = tool == ToolMode::Place && placement.kind == Placement::Kind::Model && placement.path == m.path;
    if (placing) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3, 3));
    const bool clicked = tex ? ImGui::ImageButton("##img", (ImTextureID)(uint64_t)tex, ImVec2(size, size))
                             : ImGui::Button(thumbs.failed(req) ? "?" : "...", ImVec2(size + 6, size + 6));
    ImGui::PopStyleVar();
    if (placing) ImGui::PopStyleColor();
    if (ImGui::BeginItemTooltip()) {
      ImGui::TextUnformatted(m.name.c_str());
      ImGui::TextDisabled(tr("catalog.from"), m.group.c_str());
      ImGui::TextDisabled("%s", tr("catalog.model_tip"));
      ImGui::TextDisabled("%s", tr("catalog.place_tip"));
      ImGui::EndTooltip();
    }
    if (clicked) start_model_placement(m, false);
    if (ImGui::BeginDragDropSource()) {
      if (!m_drag_from_panel) {
        m_drag_from_panel = true;
        start_model_placement(m, true);
      }
      int dummy = 0;
      ImGui::SetDragDropPayload("OGLE_PREFAB", &dummy, sizeof(dummy));
      ImGui::TextUnformatted(m.name.c_str());
      ImGui::EndDragDropSource();
    }
    ImGui::TextUnformatted(fit_text(m.name, size + 6).c_str());
  });
  ImGui::EndChild();
}

void App::ui_decor_cell(const std::string& background, int64_t stamp, const CatalogPart& p, const std::string& from,
                        int levels, float size) {
  ThumbRequest req;
  req.kind = ThumbRequest::Kind::Part;
  req.path = background;
  req.node = p.mesh_node;
  req.stamp = stamp;
  unsigned tex = thumbs.get(req);
  const bool placing = tool == ToolMode::Place && placement.kind == Placement::Kind::Part &&
                       placement.path == background && placement.part.mesh_node == p.mesh_node;
  if (placing) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3, 3));
  const bool clicked = tex ? ImGui::ImageButton("##img", (ImTextureID)(uint64_t)tex, ImVec2(size, size))
                           : ImGui::Button(thumbs.failed(req) ? "?" : "...", ImVec2(size + 6, size + 6));
  ImGui::PopStyleVar();
  if (placing) ImGui::PopStyleColor();
  if (ImGui::BeginItemTooltip()) {
    ImGui::TextUnformatted(p.name.c_str());
    if (p.kind == CatalogPart::Kind::Prototype) ImGui::TextDisabled(tr("catalog.prototype"), p.copies);
    else ImGui::TextDisabled("%s", tr("catalog.piece"));
    if (!from.empty()) ImGui::TextDisabled(tr("catalog.from"), from.c_str());
    if (levels > 1) ImGui::TextDisabled(tr("catalog.in_levels"), levels);
    ImGui::TextDisabled(tr("inspector.triangles"), p.triangles);
    ImGui::TextDisabled("%s", tr("catalog.place_tip"));
    ImGui::EndTooltip();
  }
  if (clicked) start_part_placement(background, p, false);
  if (ImGui::BeginDragDropSource()) {
    if (!m_drag_from_panel) {
      m_drag_from_panel = true;
      start_part_placement(background, p, true);
    }
    int dummy = 0;
    ImGui::SetDragDropPayload("OGLE_PREFAB", &dummy, sizeof(dummy));
    ImGui::TextUnformatted(p.name.c_str());
    ImGui::EndDragDropSource();
  }
  ImGui::TextUnformatted(fit_text(p.name, size + 6).c_str());
}

void App::ui_catalog_decor() {
  // the level whose decor is shown, or every level
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##decor-level", m_decor_level.empty() ? tr("catalog.all_levels") : m_decor_level.c_str(),
                        ImGuiComboFlags_HeightLarge)) {
    if (ImGui::Selectable(tr("catalog.all_levels"), m_decor_level.empty()) && !m_decor_level.empty()) {
      m_decor_level.clear();
      m_decor_scene.reset();
      m_decor_parts.clear();
    }
    for (const auto& l : library.levels()) {
      if (!l.listed) continue;
      if (ImGui::Selectable(l.name.c_str(), l.name == m_decor_level) && l.name != m_decor_level) {
        m_decor_level = l.name;
        m_decor_scene.reset();
        m_decor_parts.clear();
      }
    }
    ImGui::EndCombo();
  }
  const LevelEntry* level = library.level(m_decor_level);
  if (!level) {
    m_decor_level.clear();
    ui_catalog_decor_all();
    return;
  }
  if (!m_decor_scene) {
    m_decor_scene = store.level(level->background, true);
    if (m_decor_scene) m_decor_parts = list_parts(*m_decor_scene);
  }
  if (!m_decor_scene) {
    if (store.failed_level(level->background)) ImGui::TextWrapped("%s", tr("catalog.decor.failed"));
    else ImGui::TextDisabled(tr("catalog.decor.loading"), level->name.c_str());
    return;
  }
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##decor-filter", tr("catalog.filter"), &m_catalog_filter);
  const std::string f = lower(m_catalog_filter);
  std::vector<const CatalogPart*> shown;
  for (const auto& p : m_decor_parts)
    if (f.empty() || lower(p.name).find(f) != std::string::npos) shown.push_back(&p);
  ImGui::TextDisabled(tr("catalog.parts_count"), shown.size(), m_decor_parts.size());
  if (m_decor_parts.empty()) {
    ImGui::TextWrapped("%s", tr("catalog.decor.none"));
    return;
  }
  ImGui::BeginChild("parts");
  const float size = 80.f;
  thumbnail_grid((int)shown.size(), size,
                 [&](int i) { ui_decor_cell(level->background, level->mtime, *shown[i], std::string(), 1, size); });
  ImGui::EndChild();
}

void App::ui_catalog_decor_all() {
  if (!library_index.ready()) {
    // the levels are being read (once: then from the cache)
    const int total = std::max(1, library_index.total());
    ImGui::TextDisabled(tr("catalog.indexing"), library_index.done(), library_index.total());
    ImGui::ProgressBar((float)library_index.done() / total, ImVec2(-1, 0), "");
    return;
  }
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##decor-all-filter", tr("catalog.decor.filter"), &m_catalog_filter);
  const std::string f = lower(m_catalog_filter);
  if (f.empty()) {
    ImGui::TextWrapped(tr("catalog.decor.search"), m_all_prototypes, m_all_pieces);
    return;
  }
  if (f != m_all_parts_filter) {
    m_all_parts_filter = f;
    m_all_parts_shown.clear();
    for (size_t i = 0; i < all_parts.size(); i++)
      if (all_parts[i].key.find(f) != std::string::npos) m_all_parts_shown.push_back(i);
  }
  ImGui::TextDisabled(tr("catalog.parts_count"), m_all_parts_shown.size(), all_parts.size());
  ImGui::BeginChild("all-parts");
  const float size = 80.f;
  thumbnail_grid((int)m_all_parts_shown.size(), size, [&](int i) {
    const DecorEntry& e = all_parts[m_all_parts_shown[i]];
    ui_decor_cell(e.background, e.stamp, e.part, e.level, e.levels, size);
  });
  ImGui::EndChild();
}

// ------------------------------------------------------------------------------------------------
// prefabs
// ------------------------------------------------------------------------------------------------

void App::ui_prefabs_panel() {
  if (prefab_folder().empty()) {
    ImGui::Spacing();
    ImGui::TextWrapped("%s", tr("prefabs.no_library"));
    return;
  }
  const bool sel = !selection.empty();
  ImGui::BeginDisabled(!sel);
  if (ImGui::Button(tr_id("prefabs.create").c_str(), ImVec2(-1, 34))) m_show_new_prefab = true;
  ImGui::EndDisabled();
  tooltip(tr("prefabs.create.tip"));
  ImGui::Separator();
  if (prefabs.empty()) {
    ImGui::TextWrapped("%s", tr("prefabs.empty"));
    return;
  }
  ImGui::BeginChild("prefabs");
  const float size = 96.f;
  const float cell_w = size + 6 + ImGui::GetStyle().ItemSpacing.x;
  const int per_row = std::max(1, (int)((ImGui::GetContentRegionAvail().x + ImGui::GetStyle().ItemSpacing.x) / cell_w));
  std::string to_delete;
  for (size_t i = 0; i < prefabs.size(); i++) {
    const PrefabInfo& p = prefabs[i];
    ImGui::PushID(p.path.c_str());
    if (i % per_row) ImGui::SameLine();
    ImGui::BeginGroup();
    ThumbRequest req;
    req.kind = ThumbRequest::Kind::Model;
    req.path = p.path;
    req.stamp = p.mtime;
    unsigned tex = thumbs.get(req);
    const bool placing = tool == ToolMode::Place && placement.path == p.path;
    if (placing) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3, 3));
    bool clicked = tex ? ImGui::ImageButton("##img", (ImTextureID)(uint64_t)tex, ImVec2(size, size))
                       : ImGui::Button(thumbs.failed(req) ? "?" : "...", ImVec2(size + 6, size + 6));
    ImGui::PopStyleVar();
    if (placing) ImGui::PopStyleColor();
    tooltip((p.name + "\n" + tr("prefabs.cell_tip")).c_str());
    if (clicked) start_placement(p.path, p.name, false);
    if (ImGui::BeginDragDropSource()) {
      if (!m_drag_from_panel) {
        m_drag_from_panel = true;
        start_placement(p.path, p.name, true);
      }
      int dummy = 0;
      ImGui::SetDragDropPayload("OGLE_PREFAB", &dummy, sizeof(dummy));
      ImGui::TextUnformatted(p.name.c_str());
      ImGui::EndDragDropSource();
    }
    if (ImGui::BeginPopupContextItem("ctx")) {
      ImGui::TextDisabled("%s", p.name.c_str());
      if (ImGui::MenuItem(tr("prefabs.place"))) start_placement(p.path, p.name, false);
      if (ImGui::MenuItem(tr("prefabs.delete"))) to_delete = p.path;
      ImGui::EndPopup();
    }
    ImGui::TextUnformatted(fit_text(p.name, size + 6).c_str());
    ImGui::EndGroup();
    ImGui::PopID();
  }
  ImGui::EndChild();
  if (!to_delete.empty()) {
    if (placement.path == to_delete) stop_placement();
    std::error_code ec;
    fs::remove(to_path(to_delete), ec);
    store.forget_model(to_delete);
    m_prefab_scenes.erase(to_delete);
    refresh_prefabs();
    LOG_INFO(tr("log.prefab_deleted"), to_path(to_delete).stem().string().c_str());
  }
}

// ------------------------------------------------------------------------------------------------
// inspector
// ------------------------------------------------------------------------------------------------

void App::ui_inspector(float x, float y, float w, float h) {
  ImGui::SetNextWindowPos(ImVec2(x, y));
  ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(260, h), ImVec2(ImGui::GetIO().DisplaySize.x * 0.4f, h));
  ImGui::Begin(tr_id("inspector.title").c_str(), nullptr,
               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
  m_right_w = ImGui::GetWindowWidth();
  if (tool == ToolMode::Place) {
    ImGui::SeparatorText(tr("inspector.placing"));
    ImGui::TextWrapped("%s", placement.name.c_str());
    ImGui::TextWrapped("%s", tr("inspector.placing.help"));
    if (ImGui::Button(tr_id("inspector.placing.stop").c_str())) stop_placement();
    ImGui::Spacing();
  }
  Node* n = scene.find(primary);
  if (!n || selection.empty()) {
    ImGui::SeparatorText(tr("inspector.level"));
    if (!document_open()) {
      ImGui::TextWrapped("%s", tr("inspector.no_level"));
    } else {
      ImGui::Text("%s  (%s)", scene.settings.level.empty() ? tr("title.untitled") : scene.settings.level.c_str(),
                  game_label(scene.settings.game));
      // counted again only when the document changes (a city is 150 000 nodes)
      static uint64_t counted_rev = ~0ull;
      static size_t counted_nodes = ~(size_t)0;
      static size_t objects = 0, actors = 0;
      static std::vector<uint32_t> companions, layers;
      if (counted_rev != history.revision() || counted_nodes != scene.nodes.size()) {
        counted_rev = history.revision();
        counted_nodes = scene.nodes.size();
        const SceneCache& cache = scene.cache();
        objects = actors = 0;
        companions.clear();
        layers.clear();
        for (size_t i = 0; i < scene.nodes.size(); i++) {
          const Node& node = scene.nodes[i];
          if (node.kind == NodeKind::Render && !cache.actor[i]) objects++;
          if (node.kind == NodeKind::Actor) actors++;
          if (node.extras.contains("og_companion")) companions.push_back(node.id);
          if (node.extras.contains("og_layer")) layers.push_back(node.id);
        }
      }
      ImGui::Text(tr("inspector.counts"), objects, actors);
      // the levels shown with it, the layers of the missions
      if (!companions.empty()) {
        ImGui::SeparatorText(tr("inspector.companions"));
        for (auto id : companions) {
          Node* c = scene.find(id);
          if (!c) continue;
          const Node before = *c;
          bool v = c->visible;
          if (ImGui::Checkbox((c->name + "##companion").c_str(), &v)) {
            c->visible = v;
            commit_node_edit(before, tr("undo.visibility"));
          }
          tooltip(tr("inspector.companions.tip"));
        }
      }
      if (!layers.empty()) {
        size_t shown = 0;
        for (auto id : layers)
          if (const Node* l = scene.find(id)) shown += l->visible && !l->story_hidden;
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped(tr("inspector.layers"), layers.size(), shown);
        ImGui::PopStyleColor();
        tooltip(tr("inspector.layers.tip"));
      }
      ImGui::Spacing();
      ImGui::TextWrapped("%s", tr("inspector.hint"));
    }
    ImGui::End();
    return;
  }
  if (selection.size() > 1) {
    ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), tr("inspector.selected_count"), selection.size());
  }
  ui_node_inspector(*n);
  ImGui::End();
}

void App::ui_node_inspector(Node& n) {
  ImGui::SeparatorText(kind_label(n.kind));
  ImGui::TextWrapped("%s", n.name.c_str());
  if (n.kind == NodeKind::Actor) {
    ImGui::Text(tr("inspector.type"), n.extras.value("etype", std::string("?")).c_str());
    for (auto c : scene.children(n.id))
      if (const Node* cn = scene.find(c); cn && cn->extras.contains("og_actor_visual"))
        ImGui::TextDisabled(tr("inspector.model"), cn->name.c_str());
  }
  // where it is
  {
    const Node snap = n;  // before this frame's edits, for undo
    Vec3 t = n.local.t;
    Vec3 e = n.local.r.to_euler_deg();
    ImGui::PushItemWidth(-90);
    if (ImGui::DragFloat3(tr_id("inspector.position").c_str(), &t.x, 0.05f, 0, 0, "%.2f")) n.local.t = t;
    edit_begin(snap);
    edit_end(tr("undo.move"));
    if (ImGui::DragFloat3(tr_id("inspector.rotation").c_str(), &e.x, 0.5f, 0, 0, "%.0f deg"))
      n.local.r = Quat::from_euler_deg(e);
    edit_begin(snap);
    edit_end(tr("undo.rotate"));
    Vec3 sc = n.local.s;
    if (ImGui::DragFloat3(tr_id("inspector.scale").c_str(), &sc.x, 0.01f, 0.01f, 100.f, "%.2f")) {
      if (m_scale_uniform) {
        // the value changed gives the ratio of the three
        const float before[3] = {n.local.s.x, n.local.s.y, n.local.s.z}, after[3] = {sc.x, sc.y, sc.z};
        for (int i = 0; i < 3; i++)
          if (after[i] != before[i] && before[i] > 0) {
            const float k = after[i] / before[i];
            sc = {before[0] * k, before[1] * k, before[2] * k};
            break;
          }
      }
      n.local.s = {std::max(sc.x, 0.01f), std::max(sc.y, 0.01f), std::max(sc.z, 0.01f)};
    }
    edit_begin(snap);
    edit_end(tr("undo.scale"));
    ImGui::PopItemWidth();
    ImGui::Checkbox(tr_id("inspector.scale.uniform").c_str(), &m_scale_uniform);
    tooltip(tr("inspector.scale.uniform.tip"));
    if (n.kind == NodeKind::Actor && (n.local.s.x != 1.f || n.local.s.y != 1.f || n.local.s.z != 1.f)) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.75f, 0.35f, 1));
      ImGui::TextWrapped("%s", tr("inspector.scale.actor"));
      ImGui::PopStyleColor();
    }
  }
  ImGui::Spacing();
  if (ImGui::Button(tr_id("inspector.copy").c_str())) copy_selection();
  ImGui::SameLine();
  if (ImGui::Button(tr_id("inspector.duplicate").c_str())) duplicate_selection();
  ImGui::SameLine();
  if (ImGui::Button(tr_id("inspector.delete").c_str())) {
    delete_selection();
    return;
  }
  if (ImGui::Button(tr_id("inspector.create_prefab").c_str(), ImVec2(-1, 0))) m_show_new_prefab = true;
  if (n.kind == NodeKind::Render && n.mesh) {
    ImGui::SeparatorText(tr("inspector.mesh"));
    ImGui::TextWrapped("%s", n.mesh->name.c_str());
    ImGui::TextDisabled(tr("inspector.triangles"), n.mesh->tri_count());
    size_t users = 0;
    for (const auto& o : scene.nodes)
      if (o.mesh == n.mesh) users++;
    if (users > 1) ImGui::TextDisabled(tr("inspector.copies"), users);
  }
}

// ------------------------------------------------------------------------------------------------
// status bar, viewport texts
// ------------------------------------------------------------------------------------------------

void App::ui_statusbar(float y, float h) {
  ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(0, y));
  ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, h));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
  ImGui::Begin("##status", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::TextUnformatted(tr(tool == ToolMode::Place ? "status.place" : "status.select"));
  ImGui::SameLine();
  if (history.revision() != saved_revision && document_open()) {
    ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "|  %s", tr("status.unsaved"));
    ImGui::SameLine();
  }
  if (m_level_future.valid() || store.pending()) {
    ImGui::TextColored(ImVec4(0.5f, 0.85f, 1, 1), "|  %s", tr("status.loading"));
    ImGui::SameLine();
  }
  if (m_remote.clients() > 0) {
    ImGui::TextColored(ImVec4(0.75f, 0.6f, 1, 1), "|  %s", tr("status.remote"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("status.remote.tip"));
    ImGui::SameLine();
  }
  auto lines = Log::get().lines();
  if (!lines.empty()) {
    const auto& last = lines.back();
    ImVec4 col = last.level == LogLevel::Error  ? ImVec4(1, 0.45f, 0.4f, 1)
                 : last.level == LogLevel::Warn ? ImVec4(1, 0.8f, 0.4f, 1)
                                                : ImVec4(0.6f, 0.65f, 0.7f, 1);
    ImGui::TextColored(col, "|  %s", last.text.c_str());
    if (ImGui::IsItemClicked()) show_log = true;
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

// The story bar, at the bottom of the viewport: the point of the game the level is shown at.
void App::ui_story_bar() {
  const GameData* data = document_game_data();
  if (!data || !data->has_story() || scene.nodes.empty() || m_vp_w < 200.f) return;
  const ImGuiStyle& style = ImGui::GetStyle();
  const float pad = 8.f;
  const float h = ImGui::GetFrameHeight() + style.WindowPadding.y * 2.f;
  ImGui::SetNextWindowPos(ImVec2(m_vp_x + pad, m_vp_y + m_vp_h - h - pad));
  ImGui::SetNextWindowSize(ImVec2(m_vp_w - 2 * pad, h));
  ImGui::SetNextWindowBgAlpha(0.88f);
  ImGui::Begin("##story", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing);
  StoryView v = story;
  bool changed = ImGui::Checkbox(tr_id("story.enabled").c_str(), &v.enabled);
  tooltip(tr("story.enabled.tip"));
  // the states of the level: from the start, and from each step that changes it
  int state_start = 1, state = 1;
  for (int c : story_steps)
    if (c <= v.step) {
      state_start = c;
      state++;
    }
  int prev = 0, next = 0;
  if (state_start > 1) {
    prev = 1;
    for (int c : story_steps)
      if (c < state_start) prev = c;
  }
  for (int c : story_steps)
    if (c > v.step) {
      next = c;
      break;
    }
  const float label_w = std::min(420.f, ImGui::GetContentRegionAvail().x * 0.42f);
  ImGui::SameLine();
  ImGui::BeginDisabled(!v.enabled);
  ImGui::BeginDisabled(!prev);
  if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) {
    v.step = prev;
    changed = true;
  }
  ImGui::EndDisabled();
  tooltip(tr("story.prev.tip"));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(std::max(80.f, ImGui::GetContentRegionAvail().x - label_w - ImGui::GetFrameHeight() -
                                             style.ItemSpacing.x * 2));
  const std::string format = "%d / " + std::to_string(data->step_count());
  changed |= ImGui::SliderInt("##step", &v.step, 1, data->step_count(), format.c_str(), ImGuiSliderFlags_AlwaysClamp);
  tooltip(tr("story.slider.tip"));
  ImGui::SameLine();
  ImGui::BeginDisabled(!next);
  if (ImGui::ArrowButton("##next", ImGuiDir_Right)) {
    v.step = next;
    changed = true;
  }
  ImGui::EndDisabled();
  tooltip(tr("story.next.tip"));
  ImGui::EndDisabled();
  ImGui::SameLine();
  std::string text;
  if (!v.enabled) {
    text = tr("story.all");
  } else {
    text = v.step <= 1 ? std::string(tr("story.start")) : strf(tr("story.after"), data->node(v.step - 1).name.c_str());
    text += "   " + strf(tr("story.state"), state, (int)story_steps.size() + 1);
  }
  ImGui::TextUnformatted(fit_text(text, label_w).c_str());
  if (v.enabled && v.step > 1) tooltip(data->node(v.step - 1).name.c_str());
  ImGui::End();
  if (changed) set_story_view(v);
}

// A word in the empty viewport: where to start.
void App::ui_viewport_hint() {
  if (document_open() || tool == ToolMode::Place) return;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const char* lines[] = {tr(games.empty() ? "viewport.hint.extract" : "viewport.hint.load"),
                         tr("viewport.hint.camera")};
  float y = m_vp_y + m_vp_h * 0.44f;
  for (const char* l : lines) {
    ImVec2 s = ImGui::CalcTextSize(l);
    dl->AddText(ImVec2(m_vp_x + (m_vp_w - s.x) * 0.5f, y), IM_COL32(200, 210, 220, 170), l);
    y += s.y + 8;
  }
}

// Names of the actors near the camera, and of the selection.
void App::ui_labels() {
  if (!show_labels) return;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  int drawn = 0;
  const bool few_selected = selection.size() <= 12;
  const SceneCache& cache = scene.cache();
  for (size_t i = 0; i < scene.nodes.size() && drawn <= 300; i++) {
    const auto& n = scene.nodes[i];
    const bool sel = few_selected && selection.count(n.id) != 0;
    if (!sel && (n.kind != NodeKind::Actor || !rs.show_actors)) continue;
    if (!cache.visible[i]) continue;
    Vec3 p = cache.worlds[i].col3(3);
    AABB b = n.mesh ? n.mesh->bounds().transformed(cache.worlds[i]) : AABB{};
    if (b.valid() && !b.expanded(1.f).contains(p)) p = b.center();  // meshes in world space: origin at 0
    if (!sel && length(p - camera.pos) > 60.f) continue;
    Vec2 s;
    if (!renderer.project(p + Vec3{0, 0.9f, 0}, &s)) continue;
    if (s.x < 0 || s.y < 0 || s.x > m_vp_w || s.y > m_vp_h) continue;
    std::string text = n.name;
    if (n.kind == NodeKind::Actor) text += "  (" + n.extras.value("etype", std::string()) + ")";
    ImVec2 pos(m_vp_x + s.x, m_vp_y + s.y);
    ImVec2 size = ImGui::CalcTextSize(text.c_str());
    dl->AddRectFilled(ImVec2(pos.x - 3, pos.y - 1), ImVec2(pos.x + size.x + 3, pos.y + size.y + 1),
                      IM_COL32(10, 12, 14, 150), 3.f);
    dl->AddText(pos, sel ? IM_COL32(255, 180, 80, 255) : IM_COL32(230, 232, 236, 230), text.c_str());
    drawn++;
  }
}

// ------------------------------------------------------------------------------------------------
// floating windows and popups
// ------------------------------------------------------------------------------------------------

void App::ui_windows() {
  if (show_help) {
    ImGui::SetNextWindowSize(ImVec2(600, 560), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(tr_id("help.title").c_str(), &show_help)) {
      ImGui::SeparatorText(tr("help.start"));
      for (const char* k : {"help.start.1", "help.start.2", "help.start.3", "help.start.4", "help.start.5", "help.start.6",
                            "help.start.7"})
        ImGui::TextWrapped("%s", tr(k));
      // a bullet whose text wraps at the edge of the window
      auto bullet = [](const char* text) {
        ImGui::Bullet();
        ImGui::TextWrapped("%s", text);
      };
      ImGui::SeparatorText(tr("help.camera"));
      for (const char* k : {"help.camera.1", "help.camera.2", "help.camera.3", "help.camera.4"}) bullet(tr(k));
      ImGui::SeparatorText(tr("help.story"));
      for (const char* k : {"help.story.1", "help.story.2", "help.story.3"}) bullet(tr(k));
      ImGui::SeparatorText(tr("help.editing"));
      for (const char* k : {"help.editing.1", "help.editing.2", "help.editing.3", "help.editing.4", "help.editing.5"})
        bullet(tr(k));
    }
    ImGui::End();
  }

  if (show_log) {
    ImGui::SetNextWindowSize(ImVec2(760, 360), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(tr_id("log.title").c_str(), &show_log)) {
      auto lines = Log::get().lines();
      if (ImGui::SmallButton(tr_id("log.copy").c_str())) {
        std::string all;
        for (const auto& l : lines) all += l.text + "\n";
        ImGui::SetClipboardText(all.c_str());
      }
      ImGui::BeginChild("lines", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
      for (const auto& l : lines) {
        ImVec4 col = l.level == LogLevel::Error  ? ImVec4(1, 0.45f, 0.4f, 1)
                     : l.level == LogLevel::Warn ? ImVec4(1, 0.8f, 0.4f, 1)
                                                 : ImVec4(0.8f, 0.82f, 0.85f, 1);
        ImGui::TextColored(col, "%s", l.text.c_str());
      }
      if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20) ImGui::SetScrollHereY(1.f);
      ImGui::EndChild();
    }
    ImGui::End();
  }

  // a name for the prefab made of the selection
  if (m_show_new_prefab) {
    ImGui::OpenPopup(tr_id("prefab_popup.title").c_str());
    m_show_new_prefab = false;
    m_new_prefab_name.clear();
    if (const Node* n = scene.find(primary)) m_new_prefab_name = n->mesh ? n->mesh->name : n->name;
    if (m_new_prefab_name.size() > 3 && m_new_prefab_name.substr(m_new_prefab_name.size() - 3) == ".mb")
      m_new_prefab_name.resize(m_new_prefab_name.size() - 3);
  }
  if (ImGui::BeginPopupModal(tr_id("prefab_popup.title").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text(tr("prefab_popup.text"), selection_roots().size());
    if (prefab_folder().empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", tr("log.prefab_no_library"));
    ImGui::SetNextItemWidth(360);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    bool enter = ImGui::InputText(tr_id("prefab_popup.name").c_str(), &m_new_prefab_name,
                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::BeginDisabled(m_new_prefab_name.empty() || prefab_folder().empty() || selection.empty());
    if (ImGui::Button(tr_id("prefab_popup.create").c_str(), ImVec2(140, 0)) || (enter && !m_new_prefab_name.empty())) {
      create_prefab(m_new_prefab_name);
      left_tab_request = 1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr_id("popup.cancel").c_str(), ImVec2(140, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  // unsaved changes before loading or quitting
  if (m_show_discard) {
    ImGui::OpenPopup(tr_id("discard.title").c_str());
    m_show_discard = false;
  }
  if (ImGui::BeginPopupModal(tr_id("discard.title").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted(tr("discard.text"));
    ImGui::Spacing();
    if (ImGui::Button(tr_id("discard.save").c_str(), ImVec2(160, 0))) {
      save(false);
      ImGui::CloseCurrentPopup();
      // saved at once when the project has a file; else the Save as dialog opens and the action waits
      // for the user to ask again
      if (history.revision() == saved_revision && m_pending_discard) m_pending_discard();
      m_pending_discard = nullptr;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr_id("discard.discard").c_str(), ImVec2(160, 0))) {
      ImGui::CloseCurrentPopup();
      if (m_pending_discard) m_pending_discard();
      m_pending_discard = nullptr;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr_id("popup.cancel").c_str(), ImVec2(120, 0))) {
      m_pending_discard = nullptr;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

}  // namespace ogle
