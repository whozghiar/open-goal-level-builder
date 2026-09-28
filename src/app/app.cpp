#include "app/app.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>

#include "core/i18n.h"
#include "core/log.h"
#include "core/prefs.h"
#include "glad/glad.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include "render/offscreen.h"

namespace ogle {

namespace fs = std::filesystem;

namespace {

fs::path to_path(const std::string& s) { return fs::path(std::u8string((const char8_t*)s.c_str())); }

void apply_style(float scale) {
  ImGui::StyleColorsDark();
  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowRounding = 4.f;
  s.FrameRounding = 4.f;
  s.GrabRounding = 3.f;
  s.TabRounding = 4.f;
  s.WindowBorderSize = 1.f;
  s.FramePadding = ImVec2(8, 5);
  s.ItemSpacing = ImVec2(8, 6);
  ImVec4* c = s.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.105f, 0.12f, 0.14f, 0.97f);
  c[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.09f, 0.1f, 1.f);
  c[ImGuiCol_TitleBgActive] = ImVec4(0.1f, 0.2f, 0.23f, 1.f);
  c[ImGuiCol_Header] = ImVec4(0.12f, 0.36f, 0.4f, 0.55f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.14f, 0.45f, 0.5f, 0.8f);
  c[ImGuiCol_HeaderActive] = ImVec4(0.16f, 0.52f, 0.58f, 1.f);
  c[ImGuiCol_Button] = ImVec4(0.15f, 0.33f, 0.37f, 0.8f);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.45f, 0.5f, 1.f);
  c[ImGuiCol_ButtonActive] = ImVec4(0.2f, 0.55f, 0.6f, 1.f);
  c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.19f, 0.22f, 1.f);
  c[ImGuiCol_CheckMark] = ImVec4(0.35f, 0.85f, 0.9f, 1.f);
  c[ImGuiCol_SliderGrab] = ImVec4(0.3f, 0.7f, 0.76f, 1.f);
  c[ImGuiCol_Tab] = ImVec4(0.13f, 0.2f, 0.23f, 1.f);
  c[ImGuiCol_TabHovered] = ImVec4(0.18f, 0.45f, 0.5f, 1.f);
  c[ImGuiCol_TabSelected] = ImVec4(0.16f, 0.4f, 0.45f, 1.f);
  s.ScaleAllSizes(scale);
}

void load_font(float scale) {
  ImGuiIO& io = ImGui::GetIO();
  const char* candidates[] = {"C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                              "/System/Library/Fonts/Supplemental/Arial.ttf"};
  for (const char* f : candidates) {
    std::error_code ec;
    if (fs::exists(f, ec)) {
      if (io.Fonts->AddFontFromFileTTF(f, 17.f)) break;
    }
  }
  ImGui::GetStyle().FontScaleDpi = scale;
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// window
// ------------------------------------------------------------------------------------------------

bool App::init_window(bool hidden, int w, int h) {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    LOG_ERROR("SDL_Init: %s", SDL_GetError());
    return false;
  }
  m_hidden = hidden;
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
  if (scale <= 0.f) scale = 1.f;
  SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  if (hidden) flags |= SDL_WINDOW_HIDDEN;
  else flags |= SDL_WINDOW_MAXIMIZED;
  m_window = SDL_CreateWindow("OpenGOAL Level Editor", (int)(w * (hidden ? 1.f : scale)),
                              (int)(h * (hidden ? 1.f : scale)), flags);
  if (!m_window) {
    LOG_ERROR("SDL_CreateWindow: %s", SDL_GetError());
    return false;
  }
  m_gl = SDL_GL_CreateContext(m_window);
  if (!m_gl) {
    LOG_ERROR("no OpenGL 3.3 context: %s", SDL_GetError());
    return false;
  }
  SDL_GL_MakeCurrent(m_window, m_gl);
  SDL_GL_SetSwapInterval(1);
  if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress))) {
    LOG_ERROR("cannot load the OpenGL functions");
    return false;
  }
  LOG_INFO("OpenGL %s, %s", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;  // the layout is fixed by the editor
  apply_style(hidden ? 1.f : scale);
  load_font(hidden ? 1.f : scale);
  ImGui_ImplSDL3_InitForOpenGL(m_window, m_gl);
  ImGui_ImplOpenGL3_Init("#version 330");
  std::string err;
  if (!renderer.init(&err) || !thumbs.init(&err)) {
    LOG_ERROR("renderer: %s", err.c_str());
    return false;
  }
  return true;
}

void App::shutdown_window() {
  thumbs.shutdown();
  renderer.shutdown();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  if (m_gl) SDL_GL_DestroyContext(m_gl);
  if (m_window) SDL_DestroyWindow(m_window);
  SDL_Quit();
}

void App::apply_options(const AppOptions& opts) {
  Prefs& prefs = Prefs::get();
  // the interface's language: the session's, the one chosen before, else the system's
  const char* base = SDL_GetBasePath();
  i18n_init(std::string(base ? base : "") + "lang", "en");
  const std::string lang = !opts.language.empty() ? opts.language : prefs.language;
  i18n_select(lang.empty() ? i18n_system_language() : lang);
  // the library: the session's, else the one used last when it holds an extracted game, else
  // library/ in the editor's own tree
  std::string root = opts.library;
  if (root.empty()) root = list_games(prefs.library_root).empty() ? default_library_root() : prefs.library_root;
  set_library(root, prefs.library_game);
}

void App::new_document() {
  scene.clear();
  scene.settings = LevelSettings{};
  if (!library.game().empty()) scene.settings.game = game_from_name(library.game(), scene.settings.game);
  history.clear();
  selection.clear();
  primary = hovered = 0;
  stop_placement();
  m_prefab_meshes.clear();
  m_prefab_previews.clear();
  renderer.reset();
  m_ocean.reset();
  m_ocean_map.clear();
  story_steps.clear();
  project_path.clear();
  saved_revision = history.revision();
  camera = Camera{};
}

void App::frame_all() {
  camera.yaw = 0.6f;
  camera.pitch = -0.45f;
  camera.frame(scene.focus_bounds());
}

void App::frame_selection() {
  AABB b;
  for (auto id : selection) {
    const Node* n = scene.find(id);
    if (n) b.add(scene.world_bounds(*n));
    for (auto d : scene.descendants(id)) {
      const Node* dn = scene.find(d);
      if (dn) b.add(scene.world_bounds(*dn));
    }
  }
  if (b.valid()) camera.frame(b);
}

void App::set_title() {
  if (!m_window) return;
  std::string name = !project_path.empty() ? to_path(project_path).filename().string()
                     : !scene.settings.level.empty() ? scene.settings.level
                                                     : std::string(tr("title.untitled"));
  std::string t = name + (history.revision() != saved_revision ? " *" : "") + " - OpenGOAL Level Editor";
  if (t != m_title) {
    m_title = t;
    SDL_SetWindowTitle(m_window, t.c_str());
  }
}

void App::poll_level_opening() {
  using namespace std::chrono_literals;
  if (!m_level_future.valid() || m_level_future.wait_for(0s) != std::future_status::ready) return;
  std::unique_ptr<Scene> loaded = m_level_future.get();
  if (!loaded) {
    LOG_ERROR(tr("log.level_failed"), m_level_name.c_str());
    return;
  }
  new_document();
  scene = std::move(*loaded);
  history.clear();
  saved_revision = history.revision();
  story_document_changed();
  frame_all();
  set_title();
  size_t actors = 0;
  for (const auto& n : scene.nodes) actors += n.kind == NodeKind::Actor;
  LOG_INFO(tr("log.level_opened"), m_level_name.c_str(), actors);
}

// ------------------------------------------------------------------------------------------------
// main loop
// ------------------------------------------------------------------------------------------------

int App::run(const AppOptions& opts) {
  Prefs::get().load();
  if (!init_window(opts.hidden, 1600, 900)) return 1;
  apply_options(opts);
  remote_start(opts.remote_port);
  new_document();
  if (!opts.open_path.empty()) open_project_file(opts.open_path);
  uint64_t last = SDL_GetTicksNS();
  while (m_running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) process_event(&e);
    uint64_t now = SDL_GetTicksNS();
    float dt = std::min((now - last) / 1e9f, 0.1f);
    last = now;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    frame(dt);
    SDL_GL_SwapWindow(m_window);
    autosave_tick();
    set_title();
  }
  extract_stop();
  Prefs::get().save();
  shutdown_window();
  return 0;
}

// One frame into the bound framebuffer: interface, input, tools, thumbnails, viewport.
void App::frame(float dt) {
  m_time += dt;
  // the scene cache (world matrices...) is kept while nothing changes: the history did not move,
  // no gizmo drag, no field being dragged (nodes added or removed are detected by the cache)
  if (history.revision() != m_frame_revision || gizmo.active() || ImGui::IsAnyItemActive()) scene.begin_frame();
  m_frame_revision = history.revision();
  // nodes added or brought back by the history are shown as the story says
  if (history.revision() != m_story_revision) apply_story_view();
  process_dialog_results();
  store.update();
  if (!m_index_applied && library_index.ready()) apply_library_index();
  poll_level_opening();
  remote_poll();
  extract_poll();

  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();
  draw_ui();
  handle_shortcuts();
  update_camera(dt);
  viewport_interaction();
  ui_labels();
  ui_viewport_hint();
  thumbs.update();

  int fbw, fbh, ww, wh;
  SDL_GetWindowSizeInPixels(m_window, &fbw, &fbh);
  SDL_GetWindowSize(m_window, &ww, &wh);
  float sx = ww > 0 ? (float)fbw / ww : 1.f, sy = wh > 0 ? (float)fbh / wh : 1.f;
  glViewport(0, 0, fbw, fbh);
  glClearColor(0.07f, 0.08f, 0.09f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  int px = (int)(m_vp_x * sx), pw = (int)(m_vp_w * sx), ph = (int)(m_vp_h * sy);
  int py = fbh - (int)((m_vp_y + m_vp_h) * sy);
  // what the tools changed this frame is drawn now
  if (history.revision() != m_frame_revision || gizmo.active() || ImGui::IsAnyItemActive()) scene.begin_frame();
  m_frame_revision = history.revision();
  renderer.begin(camera, px, py, pw, ph, rs, m_vp_w, m_vp_h);
  renderer.draw_scene(scene, rs, selection, hovered);
  if (rs.show_ocean && m_ocean && !scene.nodes.empty())
    renderer.draw_mesh(*m_ocean, Mat4::translate({0, m_ocean_height, 0}), m_ocean_materials, {}, rs);
  draw_tool_overlays();
  glViewport(0, 0, fbw, fbh);

  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void App::process_event(const void* ev) {
  const SDL_Event& e = *(const SDL_Event*)ev;
  ImGui_ImplSDL3_ProcessEvent(&e);
  switch (e.type) {
    case SDL_EVENT_QUIT:
      request_quit();
      break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      if (e.window.windowID == SDL_GetWindowID(m_window)) request_quit();
      break;
    case SDL_EVENT_DROP_FILE:
      // a project dropped on the window opens; a folder is taken as the library
      if (e.drop.data) {
        std::string path = e.drop.data;
        std::error_code ec;
        if (fs::is_directory(to_path(path), ec)) set_library(path, "");
        else confirm_discard([this, path]() { open_project_file(path); });
      }
      break;
    case SDL_EVENT_MOUSE_MOTION:
      m_mouse_rel.x += e.motion.xrel;
      m_mouse_rel.y += e.motion.yrel;
      break;
    default:
      break;
  }
}

void App::update_camera(float dt) {
  ImGuiIO& io = ImGui::GetIO();
  Vec2 rel = m_mouse_rel;
  m_mouse_rel = {0, 0};
  const bool rmb = ImGui::IsMouseDown(ImGuiMouseButton_Right);
  if (!m_looking && rmb && m_mouse_in_vp && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    m_looking = true;
    SDL_SetWindowRelativeMouseMode(m_window, true);
  }
  if (m_looking && !rmb) {
    m_looking = false;
    SDL_SetWindowRelativeMouseMode(m_window, false);
  }
  if (m_looking) {
    camera.yaw -= rel.x * 0.0035f;
    camera.pitch = std::clamp(camera.pitch - rel.y * 0.0035f, -1.55f, 1.55f);
    if (io.MouseWheel != 0) camera.speed = std::clamp(camera.speed * std::pow(1.2f, io.MouseWheel), 0.5f, 2000.f);
    const bool* keys = SDL_GetKeyboardState(nullptr);
    Vec3 mv{0, 0, 0};
    // physical keys: WASD on QWERTY is ZQSD on AZERTY
    if (keys[SDL_SCANCODE_W]) mv += camera.forward();
    if (keys[SDL_SCANCODE_S]) mv -= camera.forward();
    if (keys[SDL_SCANCODE_D]) mv += camera.right();
    if (keys[SDL_SCANCODE_A]) mv -= camera.right();
    if (keys[SDL_SCANCODE_E] || keys[SDL_SCANCODE_SPACE]) mv += Vec3{0, 1, 0};
    if (keys[SDL_SCANCODE_Q] || keys[SDL_SCANCODE_C]) mv -= Vec3{0, 1, 0};
    float sp = camera.speed;
    if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) sp *= 4.f;
    if (keys[SDL_SCANCODE_LCTRL]) sp *= 0.25f;
    if (length2(mv) > 0) camera.pos += normalize(mv) * sp * dt;
    return;
  }
  if (!m_mouse_in_vp && !m_panning && !m_orbiting) return;
  // wheel: move forward (Ctrl + wheel turns the prefab being placed)
  if (m_mouse_in_vp && io.MouseWheel != 0) {
    if (tool == ToolMode::Place && io.KeyCtrl) {
      placement.yaw += io.MouseWheel * radians(15.f);
    } else {
      camera.pos += camera.forward() * io.MouseWheel * std::max(camera.speed * 0.35f, 0.4f);
    }
  }
  // middle button: pan
  if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && m_mouse_in_vp) m_panning = true;
  if (m_panning) {
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
      m_panning = false;
    } else {
      float k = std::max(camera.speed, 1.f) * 0.006f;
      camera.pos -= camera.right() * (io.MouseDelta.x * k);
      camera.pos += camera.up() * (io.MouseDelta.y * k);
    }
  }
  // Alt + left button: orbit around the selection or the point under the cursor
  if (io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_mouse_in_vp) {
    m_orbiting = true;
    if (!selection.empty()) {
      m_orbit_pivot = selection_frame().col3(3);
    } else {
      PickHit h = pick_surface(m_mouse_vp);
      m_orbit_pivot = h.hit() ? h.point : camera.pos + camera.forward() * 10.f;
    }
    m_orbit_dist = std::max(length(m_orbit_pivot - camera.pos), 0.5f);
    camera.look_at(m_orbit_pivot);
  }
  if (m_orbiting) {
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      m_orbiting = false;
    } else {
      camera.yaw -= io.MouseDelta.x * 0.005f;
      camera.pitch = std::clamp(camera.pitch - io.MouseDelta.y * 0.005f, -1.55f, 1.55f);
      camera.pos = m_orbit_pivot - camera.forward() * m_orbit_dist;
    }
  }
}

void App::viewport_interaction() {
  ImGuiIO& io = ImGui::GetIO();
  m_mouse_vp = {io.MousePos.x - m_vp_x, io.MousePos.y - m_vp_y};
  bool inside = m_mouse_vp.x >= 0 && m_mouse_vp.y >= 0 && m_mouse_vp.x < m_vp_w && m_mouse_vp.y < m_vp_h;
  // while a prefab is dragged from its panel ImGui owns the mouse: the viewport is "under" it
  // when no window is
  const bool modal = m_show_discard || m_show_new_prefab;
  m_mouse_over_vp = inside && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) && !modal;
  m_mouse_in_vp = inside && !io.WantCaptureMouse && !modal;
  const ImGuiPayload* payload = ImGui::GetDragDropPayload();
  if (!payload || !payload->IsDataType("OGLE_PREFAB")) m_drag_from_panel = false;
  if (m_looking || m_orbiting || m_panning) {
    hovered = 0;
    return;
  }
  bool clicked = m_mouse_in_vp && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt;
  bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
  if (clicked) m_lmb_in_vp = true;
  if (tool == ToolMode::Place) place_tool(clicked);
  else select_tool(clicked, released);
  if (released) m_lmb_in_vp = false;
}

void App::select_tool(bool clicked, bool released) {
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  // hover
  if (m_mouse_in_vp && !gizmo.active() && !m_box_selecting) {
    PickFilter f;
    f.actors = rs.show_actors;
    f.include_locked = true;
    hovered = pick(scene, camera.ray(m_mouse_vp.x, m_mouse_vp.y, m_vp_w, m_vp_h), f).node;
    // a locked node (the terrain) hides what is behind it but is not chosen
    if (const Node* hn = scene.find(hovered); hn && hn->locked) hovered = 0;
    // an actor's model selects the actor
    if (const Node* hn = scene.find(hovered))
      if (uint32_t actor = scene.actor_of(*hn)) hovered = actor;
  } else if (!m_mouse_in_vp) {
    hovered = 0;
  }

  // gizmo
  if (!selection_roots().empty()) {
    GizmoInput gi;
    gi.mouse = m_mouse_vp;
    gi.viewport_pos = {m_vp_x, m_vp_y};
    gi.viewport_size = {m_vp_w, m_vp_h};
    gi.mouse_in_viewport = m_mouse_in_vp && !m_box_selecting;
    gi.pressed = clicked;
    gi.down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    gi.released = released;
    GizmoResult res = gizmo.update(camera, renderer, gi, selection_frame(), dl);
    if (res.started) {
      m_drag_change = std::make_unique<NodeChange>(scene);
      m_drag_start.clear();
      m_drag_scale.clear();
      for (auto id : selection_roots()) {
        const Node* n = scene.find(id);
        if (!n) continue;
        m_drag_change->save(id);
        m_drag_start[id] = scene.world(*n);
        m_drag_scale[id] = n->local.s;
      }
    }
    if (res.dragging) {
      if (gizmo.op == GizmoOp::Scale) {
        // each element along its own axes, around its origin
        for (auto& [id, s0] : m_drag_scale)
          if (Node* n = scene.find(id)) n->local.s = {s0.x * res.scale.x, s0.y * res.scale.y, s0.z * res.scale.z};
      } else {
        for (auto& [id, start] : m_drag_start)
          if (Node* n = scene.find(id)) scene.set_world(*n, res.delta * start);
      }
    }
    if (res.finished && m_drag_change) {
      const char* label = gizmo.op == GizmoOp::Translate ? "undo.move" : gizmo.op == GizmoOp::Rotate ? "undo.rotate" : "undo.scale";
      history.push(m_drag_change->commit(tr(label)));
      m_drag_change.reset();
      m_drag_start.clear();
    }
    if (gizmo.active() || (gizmo.hovering() && clicked)) return;
  }

  // click and box selection
  if (clicked) {
    m_box_start = m_mouse_vp;
    m_box_selecting = false;
  }
  if (m_lmb_in_vp && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !gizmo.active()) {
    if (!m_box_selecting && (m_mouse_vp - m_box_start).length() > 6.f) m_box_selecting = true;
    if (m_box_selecting) {
      ImVec2 a(m_vp_x + std::min(m_box_start.x, m_mouse_vp.x), m_vp_y + std::min(m_box_start.y, m_mouse_vp.y));
      ImVec2 b(m_vp_x + std::max(m_box_start.x, m_mouse_vp.x), m_vp_y + std::max(m_box_start.y, m_mouse_vp.y));
      dl->AddRectFilled(a, b, IM_COL32(80, 190, 210, 40));
      dl->AddRect(a, b, IM_COL32(80, 190, 210, 200));
    }
  }
  if (released && m_lmb_in_vp) {
    if (m_box_selecting) {
      if (!io.KeyShift && !io.KeyCtrl) selection.clear();
      float x0 = std::min(m_box_start.x, m_mouse_vp.x), x1 = std::max(m_box_start.x, m_mouse_vp.x);
      float y0 = std::min(m_box_start.y, m_mouse_vp.y), y1 = std::max(m_box_start.y, m_mouse_vp.y);
      const SceneCache& cache = scene.cache();
      for (size_t i = 0; i < scene.nodes.size(); i++) {
        const Node& n = scene.nodes[i];
        if ((n.kind != NodeKind::Render && n.kind != NodeKind::Actor) || n.locked || !cache.visible[i]) continue;
        if (cache.actor[i]) continue;  // an actor's model: the actor is selected instead
        if (n.kind == NodeKind::Actor && !rs.show_actors) continue;
        AABB b = scene.world_bounds(n);
        Vec2 s;
        if (!b.valid() || !renderer.project(b.center(), &s)) continue;
        if (s.x >= x0 && s.x <= x1 && s.y >= y0 && s.y <= y1) {
          selection.insert(n.id);
          primary = n.id;
        }
      }
    } else if (m_mouse_in_vp) {
      if (hovered) {
        if (io.KeyShift || io.KeyCtrl) toggle_select(hovered);
        else select_only(hovered);
      } else if (!io.KeyShift && !io.KeyCtrl) {
        select_only(0);
      }
    }
    m_box_selecting = false;
  }
}

void App::place_tool(bool clicked) {
  hovered = 0;
  m_place_valid = false;
  const bool over = placement.once ? m_mouse_over_vp : m_mouse_in_vp;
  if (over) {
    Ray r = camera.ray(m_mouse_vp.x, m_mouse_vp.y, m_vp_w, m_vp_h);
    m_place_matrix = placement_matrix(pick_surface(m_mouse_vp), r);
    m_place_valid = true;
  }
  if (placement.once) {
    // dragged from the Prefabs panel: dropped when the button is released
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      if (m_place_valid) place_at(m_place_matrix);
      stop_placement();
    }
    return;
  }
  if (clicked && m_place_valid) place_at(m_place_matrix);
}

void App::draw_tool_overlays() {
  if (tool != ToolMode::Place || !m_place_valid) return;
  // what is placed, see-through, where it would go (its mesh, merged into the document once)
  std::shared_ptr<Mesh> preview = placement_mesh();
  const Vec3 c = m_place_matrix.col3(3);
  if (preview) {
    renderer.draw_mesh(*preview, m_place_matrix, scene.materials, scene.textures, rs, {0.35f, 0.75f, 1.f, 0.25f}, 0.7f);
  } else {
    // a cross while the prefab loads
    const float r = 1.f;
    renderer.line(c - Vec3{r, 0, 0}, c + Vec3{r, 0, 0}, 0xff808080);
    renderer.line(c - Vec3{0, 0, r}, c + Vec3{0, 0, r}, 0xff808080);
    renderer.line(c, c + Vec3{0, r * 1.5f, 0}, 0xff808080);
  }
  renderer.circle(c + Vec3{0, 0.03f, 0}, {1, 0, 0}, {0, 0, 1}, 0.5f, 0xff40d0ff, 24);
  renderer.flush(true);
}

void App::handle_shortcuts() {
  ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput || m_looking) return;
  const bool ctrl = io.KeyCtrl, shift = io.KeyShift;
  auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
  if (ctrl && pressed(ImGuiKey_Z)) {
    if (shift) history.redo();
    else history.undo();
  } else if (ctrl && pressed(ImGuiKey_Y)) {
    history.redo();
  } else if (ctrl && pressed(ImGuiKey_S)) {
    save(shift);
  } else if (ctrl && pressed(ImGuiKey_O)) {
    confirm_discard([this]() { open_dialog(DialogAction::OpenProject); });
  } else if (ctrl && pressed(ImGuiKey_N)) {
    confirm_discard([this]() { new_level(); });
  } else if (ctrl && pressed(ImGuiKey_C)) {
    copy_selection();
  } else if (ctrl && pressed(ImGuiKey_V)) {
    paste();
  } else if (ctrl && pressed(ImGuiKey_D)) {
    duplicate_selection();
  } else if (pressed(ImGuiKey_Escape)) {
    if (tool == ToolMode::Place) stop_placement();
    else select_only(0);
  } else if (tool == ToolMode::Place && !ctrl && pressed(ImGuiKey_R)) {
    placement.yaw += radians(shift ? -45.f : 45.f);
  } else if (pressed(ImGuiKey_Delete)) {
    delete_selection();
  } else if (!ctrl && pressed(ImGuiKey_F)) {
    frame_selection();
  } else if (pressed(ImGuiKey_Home)) {
    frame_all();
  } else if (!ctrl && pressed(ImGuiKey_W)) {
    gizmo.op = GizmoOp::Translate;
  } else if (!ctrl && pressed(ImGuiKey_E)) {
    gizmo.op = GizmoOp::Rotate;
  } else if (!ctrl && pressed(ImGuiKey_R)) {
    gizmo.op = GizmoOp::Scale;
  } else if (pressed(ImGuiKey_F1)) {
    show_help = !show_help;
  }
}

// ------------------------------------------------------------------------------------------------
// captures (command line)
// ------------------------------------------------------------------------------------------------

// Setups (what the capture shows):
//   levels  : the Levels panel, the level `in` (a level name of the library) loaded
//   select  : the level `in`, one of its decor instances selected, the gizmo on it
//   scale   : a catwalk of the level `in` stretched along its own X axis, the gizmo scaling it;
//             for a project `in` (.glb), its first stretched element
//   project : the project `in` (a .glb saved by the editor), the objects of the Catalog panel
// select, scale and prefab also take a project; --select a,b,... chooses the elements they select
//   prefab  : the level `in`, a prefab made of a few instances (in a temporary folder) and placed
//             next to them, the Prefabs panel open
//   extract : the extraction window on a disc (`in`, an .iso or a folder), extracting it to a
//             temporary folder deleted after the capture
//   help    : the Getting started window
//   catalog : the level `in` loaded, the objects of the Catalog panel
//   models  : the level `in` loaded, the models of the Catalog panel
//   decor   : the level `in` loaded, its decor in the Catalog panel
//   decor-all : the level `in` loaded, the decor of every level searched for "catwalk"
int App::ui_screenshot(const AppOptions& opts, const std::string& in, const std::string& out, int w,
                       int h, const std::string& setup) {
  Prefs::get().load();
  if (!init_window(true, w, h)) return 1;
  int result = 1;
  const fs::path tmp = fs::temp_directory_path() / "ogle-capture";
  {
    std::error_code ec;
    fs::remove_all(tmp, ec);
    apply_options(opts);
    new_document();
    m_prefab_dir_override = (tmp / "prefabs").generic_string();
    refresh_prefabs();
    const bool project_in = in.size() > 4 && (in.compare(in.size() - 4, 4, ".glb") == 0 ||
                                              in.compare(in.size() - 4, 4, ".GLB") == 0);
    const bool with_project =
        setup == "project" || (project_in && (setup == "select" || setup == "scale" || setup == "prefab"));
    const bool with_level = !with_project && (setup == "levels" || setup == "select" || setup == "scale" ||
                                              setup == "prefab" || setup == "catalog" || setup == "models" ||
                                              setup == "decor" || setup == "decor-all");
    if (with_project) open_project_file(in);  // the preferences are not written in this mode
    if (setup == "project") left_tab_request = 1;
    if (with_level) {
      if (const LevelEntry* level = library.level(in)) {
        m_selected_level = level->name;
        load_level(*level);
      } else {
        LOG_ERROR("capture: no level %s in the library", in.c_str());
      }
    }
    if (setup == "extract") {
      open_extract_window();
      extract.output = (tmp / "library").generic_string();
      extract.source = in;
      extract_detect(in);
    }
    if (setup == "help") show_help = true;
    if (setup == "catalog" || setup == "models" || setup == "decor" || setup == "decor-all") {
      left_tab_request = 1;
      m_catalog_tab = setup == "decor" || setup == "decor-all" ? 2 : setup == "models" ? 1 : 0;
      if (setup == "decor") m_decor_level = in;
      if (setup == "decor-all") m_catalog_filter = "catwalk";
    }
    OffscreenTarget target;
    int fbw, fbh;
    SDL_GetWindowSizeInPixels(m_window, &fbw, &fbh);
    if (target.create(fbw, fbh)) {
      bool prepared = false, extraction_started = false;
      int settled = 0;
      auto start = std::chrono::steady_clock::now();
      for (int frame_index = 0;; frame_index++) {
        SDL_Delay(2);  // frames are not paced by vsync here: let the loading threads work
        SDL_Event e;
        while (SDL_PollEvent(&e)) process_event(&e);
        glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
        frame(1.f / 60.f);
        // loading, or the catalog not complete yet (the index of the library)
        bool loading = m_level_future.valid() || store.pending() > 0 || thumbs.pending() > 0 || extract.process ||
                       !m_index_applied;
        if (setup == "extract" && !extraction_started && !extract.process && extract.detected_source == in &&
            extract.detect_error.empty()) {
          extraction_started = true;
          extract.game = game_from_name(extract.detected_game);
          start_extraction();
          loading = true;
        }
        if (!prepared && !m_level_future.valid() && frame_index > 1) {
          prepared = true;
          // an instance of a decor prototype with copies, near the middle of the level (a catwalk
          // to stretch for `scale`): selected, then a prefab of it and its neighbours
          const Node* target_node = nullptr;
          const Vec3 middle = scene.focus_bounds().center();
          float best = 1e30f;
          // the elements asked for (--select), else the first stretched one of a project
          std::vector<uint32_t> named;
          if (!opts.select.empty()) {
            const std::string list = "," + opts.select + ",";
            for (const auto& n : scene.nodes)
              if (list.find("," + n.name + ",") != std::string::npos) named.push_back(n.id);
            if (!named.empty()) target_node = scene.find(named.front());
          }
          if (!target_node && with_project && setup == "scale")
            for (const auto& n : scene.nodes)
              if (!target_node && n.kind == NodeKind::Render && n.mesh &&
                  (n.local.s.x != 1.f || n.local.s.y != 1.f || n.local.s.z != 1.f))
                target_node = &n;
          for (const auto& n : scene.nodes) {
            if (with_project || target_node) break;
            const Node* p = scene.find(n.parent);
            if (n.kind == NodeKind::Render && n.mesh && !n.locked && p && p->kind == NodeKind::Group &&
                scene.children(p->id).size() > 3 && n.mesh->tri_count() > 40) {
              float d = length(scene.world_bounds(n).center() - middle);
              if (setup == "scale" && n.name.find("catwalk") == std::string::npos) d += 1e6f;
              if (d < best) {
                best = d;
                target_node = &n;
              }
            }
          }
          if (setup == "select" || setup == "scale" || setup == "prefab") {
            if (target_node) {
              select_only(target_node->id);
              for (auto id : named) selection.insert(id);
              if (setup == "scale") {
                gizmo.op = GizmoOp::Scale;
                if (!with_project && named.empty()) scene.find(target_node->id)->local.s = {2.5f, 1.f, 1.f};
              }
              AABB b = scene.world_bounds(*target_node);
              camera.yaw = 0.7f;
              camera.pitch = -0.4f;
              camera.frame(b.expanded(with_project ? 10.f : 6.f));
            }
            if (setup == "prefab" && target_node) {
              // the selected instance and the instances next to it (or the elements asked for)
              AABB near = scene.world_bounds(*target_node).expanded(4.f);
              for (const auto& n : scene.nodes)
                if (named.empty() && n.kind == NodeKind::Render && n.mesh && !n.locked && !scene.actor_of(n) &&
                    near.contains(scene.world_bounds(n).center()) && selection.size() < 6)
                  selection.insert(n.id);
              create_prefab("My prefab");
              left_tab_request = 2;
              AABB b;
              for (auto id : selection) b.add(scene.world_bounds(*scene.find(id)));
              if (!prefabs.empty()) {
                // a copy beside them, on the ground they stand on, where nothing hides it: toward
                // the camera first, then on either side, then behind
                const float step = std::max(b.size().x, b.size().z) + 3.f;
                Vec3 toward = -camera.forward();
                toward.y = 0;
                toward = length2(toward) > 1e-6f ? normalize(toward) : Vec3{0, 0, 1};
                const Vec3 side = camera.right();
                PickFilter f;
                f.actors = false;
                f.include_locked = true;
                Vec3 p = b.center() + side * step;
                p.y = b.lo.y;
                for (const Vec3& dir : {toward, -side, side, -toward}) {
                  const Vec3 q = b.center() + dir * step;
                  PickHit ground = pick(scene, Ray{Vec3{q.x, b.hi.y + 30.f, q.z}, {0, -1, 0}}, f);
                  if (ground.hit() && std::fabs(ground.point.y - b.lo.y) < 2.f) {
                    p = ground.point;
                    break;
                  }
                }
                start_placement(prefabs.front().path, prefabs.front().name, false);
                place_at(Mat4::translate(p));
                stop_placement();
                for (auto id : selection) b.add(scene.world_bounds(*scene.find(id)));
              }
              camera.pitch = -0.6f;
              camera.frame(b.expanded(3.f));
            }
          } else if (setup != "extract") {
            frame_all();
          }
          if (!opts.story.empty()) {
            StoryView v = story;
            v.enabled = opts.story != "off";
            if (const GameData* data = document_game_data(); data && v.enabled) {
              v.step = std::atoi(opts.story.c_str());
              for (int i = 0; i < data->step_count(); i++)
                if (data->node(i).name == opts.story) v.step = i + 1;
            }
            set_story_view(v);
          }
        }
        if (opts.camera_set && prepared) {
          camera.pos = opts.camera_pos;
          camera.yaw = radians(opts.camera_yaw);
          camera.pitch = radians(opts.camera_pitch);
        }
        if (prepared && !loading) settled++;
        else settled = 0;
        double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (settled > 8 || elapsed > 180.0) break;
      }
      glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
      frame(1.f / 60.f);
      glFinish();
      result = target.save_png(out) ? 0 : 1;
      LOG_INFO("interface captured: %s (%zu nodes, %zu draws, %zu triangles)", out.c_str(), scene.nodes.size(),
               renderer.stats.draws, renderer.stats.triangles);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }
  shutdown_window();  // the loading threads let go of the files
  std::error_code ec;
  fs::remove_all(tmp, ec);
  return result;
}

}  // namespace ogle
