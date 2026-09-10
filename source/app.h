/******************************************************************************
 * @file    app.h
 * @brief   application class
 *****************************************************************************/

#pragma once

#include "config.h"
#include "igi_bridge.h"
#include "level/res_model_set.h"
#include "renderer/model.h"
#include "animation.h"
#include "debug_command_manager.h"
#include "player_ladder.h"
#include "player_animation_driver.h"
#include "runtime/gameplay_host.h"
#include "runtime/map_computer_camera.h"
#include "runtime/render_target.h"
#include "runtime/editor_cursor_mode.h"
#include <atomic>
#include <cstdint>
#include <optional>
#include <set>
#include <unordered_set>

/*
================================================================================
 App
================================================================================
*/
class App {
public:
  App();
  ~App();

  bool Init(int argc, char **argv);
  void Shutdown();
  void ResetLevel();
  void ResetScript();

  // level_no: 1 ~ 13
  void LoadLevel(int level_no);
  void DrawProgressOverlay(const char *title, int pct,
                           const char *stage); // reusable loading bar
  void SetGameLevel(int level_no);
  void SaveCurrentLevel();
  void FlushAttaProxiesToMef();
  void LaunchGame();
  void ExportTextureMap();
  int GetCurLevelNo() const;
  bool GetLevelZ(float x, float y, float& z) { return level_.GetTerrainZ(x, y, z); }

  // draw wireframe on top of solid mesh
  void ToggleOverlayWireframe();
  void ToggleDrawParts(int part);
  void SetDrawParts(int parts);
  void ToggleGamePlayMode();
  void ApplyAndRestartGameplay();
  bool IsGamePlayMode() const { return in_game_mode_; }
  void ToggleTerrainDrawOption(int opt);
  void ToggleTerrainModOption(int opt);
  void SetFogEnabled(bool enabled);
  void SetFogIntensity(int intensity);

  void ToggleEditMode();
  bool GetEditMode() const;
  void SetEditMode(bool enabled);
  void SetTerrainEditEnabled(bool enabled);
  bool GetTerrainEditEnabled() const;
  void TogglePauseMenu();
  bool GetPauseMode() const;
  void SetEditBrush(int brush);
  int GetEditBrush() const;
  // Returns true and consumes the click if (x,y) hit an on-screen terrain brush
  // palette button.
  bool TerrainPaletteClick(int x, int y);
  void AdjustBrushRadius(double factor);  // multiply brush radius, clamp+status
  void AdjustBrushStrength(double delta); // add to brush strength, clamp+status
  void ToggleShowHUD();
  bool GetShowHUD() const;
  void SetShowHUD(bool show);

  int GetHoverObjectIndex() const { return hover_object_index_; }
  bool GetPropEditorOpen() const { return prop_editor_open_; }

  bool GetOverlayWireframe() const;
  int GetDrawParts() const;
  int GetTerrainDrawOptions() const;
  int GetTerrainModOptions() const;
  void SetSelectedObjectScale(float scale);
  float GetSelectedObjectScale() const;

  // Command line initial settings
  void SetInitialFullscreen(int windowedW, int windowedH);
  void SetInitialDrawParts(int parts);
  void SetInitialStickToGround(bool stick);

  // events
  void OnWindowResize(int width, int height);
  void OnDisplay();
  bool InitializeGameplayWindow(
      int editor_window_id,
      int width,
      int height,
      const igi::GameplayWindowCallbacks& callbacks);
  void ShutdownGameplayWindow();
  void OnGameplayDisplay();
  void OnGameplayWindowResize(int width, int height);
  void OnGameplayWindowClose();

  // input
  void Input_OnMouse(int button, int state, int x, int y);
  void Input_OnMotion(int x, int y);
  void Input_OnMouseWheel(int wheel, int direction, int x, int y);
  void Input_OnSpecial(int key, int x, int y);
  void Input_OnSpecialUp(int key, int x, int y);
  void Input_OnKeyboard(unsigned char key, int x, int y);
  void Input_OnKeyboardUp(unsigned char key, int x, int y);

  // idle
  void OnIdle();

  int PickObjectAtScreenPos(int screen_x, int screen_y);
  // Promote a picked MEF ATTA sub-model (entry index from the pick pass) into a
  // real, editable EditRigidObj task at the same world transform. Selects it.
  void PromoteAttaToObject(int entry);
  std::vector<int> GetVisibleTreeNodes();

  // void					HandleMarkerInput(unsigned char
  // key); // Removed

private:
  friend class DebugCommandManager;

  struct window_state_s {
    bool full_screen_;
    bool cursor_visible_;
    int viewport_width_;
    int viewport_height_;
    int old_viewport_width_;  // before switch to fullscreen
    int old_viewport_height_; // before switch to fullscreen
  };

  struct mouse_state_s {
    bool left_button_down_;
    bool right_button_down_;
    int prior_x_;
    int prior_y_;
  };

  struct input_s {
    int keys_;
    int mouse_delta_x_;
    int mouse_delta_y_;
  };

  uint32_t frame_; // 0 ~ (0xFFFFFFFF - 1), reserve 0xFFFFFFFF as invalid frame
                   // number
  view_define_s view_define_;

  Renderer renderer_;
  Level level_;
  ResModelSet level_res_models_; // models packed in the current level's .res
  IGIBridge bridge_;
  Renderer::draw_params_s draw_params_;
  int terrain_mod_options_;
  // editor & runtime
  bool in_game_mode_ = false;
  bool progress_overlay_active_ = false; // overlay owns the surface; suppress scene repaints
  igi::GameplayHost gameplay_host_;
  std::optional<igi::EditorSnapshot> gameplay_editor_snapshot_;
  std::optional<LevelObjects> runtime_level_objects_;
  std::vector<uint8_t> runtime_initial_deleted_flags_;
  std::vector<uint8_t> runtime_conditionally_hidden_flags_;
  std::vector<uint8_t> runtime_guard_generator_hidden_flags_;
  glm::vec3 gameplay_spawn_position_ = glm::vec3(0.0f);
  float gameplay_spawn_yaw_ = 0.0f;
  float gameplay_spawn_pitch_ = 0.0f;
  bool edit_mode_;
  bool terrain_edit_enabled_;
  bool pause_mode_;
  std::string pause_level_input_ = "";
  std::string pause_search_input_ = "";
  int pause_active_input_ = -1; // -1: none, 1: search
	bool pause_terrain_expanded_ = false;
  int edit_brush_;
  double edit_brush_radius_ = 5000.0;
  double edit_brush_strength_ = 7.0;
  int selected_object_index_;
  int hover_object_index_; // Object under mouse cursor
  int last_pick_x_ = -9999;
  int last_pick_y_ = -9999;
  bool show_hud_;
  // Alt+H: gameplay HUD overlay (crosshair/health/weapon readout). Hidden by
  // default until the vanilla presentation is finished.
  bool hud_overlay_visible_ = false;
  bool show_debug_;
  bool show_help_;
  int help_scroll_offset_ = 0;
  std::vector<std::string>
      help_entries_; // keybinding lines from qedkeybindings.qsc
  bool show_magic_obj_spheres_;
  int tree_scroll_offset_;
  bool tree_decl_expanded_;
  std::string status_message_;
  DebugCommandManager debug_cmd_mgr_{this};
  bool developer_mode_ = false;

  bool task_editor_open_ = false;
  bool task_picker_open_ = false;
  int task_picker_selected_idx_ = 0;
  int task_picker_scroll_offset_ = 0;
  std::string task_picker_search_ = "";
  std::string edit_string_;
  int edit_cursor_pos_ = 0;
  int edit_selection_start_ = -1;
  int edit_selection_end_ = -1;
  bool edit_dragging_ = false;
  int edit_box_w_ = 900;
  int edit_box_h_ = 150;
  int hover_tree_index_ = -1;
  int edit_scroll_x_ = 0;
  int last_tree_click_index_ = -1;
  int last_tree_click_time_ms_ = 0;
  // Unified undo/redo state capturing all editable elements
  struct UndoState {
      std::vector<LevelObject> objects;
      std::string ai_script_path;
      std::string ai_script_text;
      bool        ai_script_dirty = false;
      int         terrain_mod_options = 0;
      std::vector<uint8_t> terrain_hmp;   // snapshot of terrain height-map pixel buffer
      GraphFile   graph_overlay;
      bool        graph_overlay_visible = false;
  };

  // C1: Custom SPR cursor — multi-mode
  using CursorMode = igi::CursorMode;
  static const int NUM_CURSORS = 12;
  GLuint cursor_tex_ids_[NUM_CURSORS] = {};
  int cursor_tex_ws_[NUM_CURSORS] = {};
  int cursor_tex_hs_[NUM_CURSORS] = {};
  int cursor_loaded_count_ = 0;
  CursorMode current_cursor_mode_ = CursorMode::Default;
  bool camera_mode_moved_ = false;
  void LoadAllCursors();
  void LoadHelpEntries(); // load keybinding lines from qedkeybindings.qsc
  void UpdateCursorMode();
  void DrawCustomCursor();

	std::set<std::string> ai_model_ids_; // AITYPE_ model IDs from IGIModels.json

	// Animation system
	AnimationRegistry animRegistry_;
	igi::PlayerAnimationDriver player_animation_driver_;
	std::unordered_map<int, AnimPlayback> animPlaybacks_; // object index -> playback state
	std::unordered_map<int, AnimPlayback> gameplayAnimPlaybacks_; // fixed-step runtime state
	uint64_t lastGameplayAnimationTick_ = 0;
	std::unordered_map<int, std::vector<int>> animIdsCache_; // object index -> discovered AIAction_PlayAnimation ids
	std::unordered_map<uint32_t, uint64_t> runtime_animation_request_serials_;
	bool show_anim_debug_ = false; // F10: animation status panel (independent of F2/TaskTree)
	bool show_anim_skeleton_ = false; // B: bone wireframe overlay, off by default, independent of F10/playback state
	// modelId -> "right hand" bone index within GetOrLoadSkinGeometry's parsed bone
	// list, cached so per-frame weapon attachment doesn't re-scan bone names. -1 =
	// looked up once already and not found (don't repeat the failed search either).
	std::unordered_map<std::string, int> handBoneIndexCache_;

	// Level background music (game_music.wav, converted from ILSF -> PCM, looped via MCI)
	bool music_playing_ = false;
	bool PlayLevelMusic(int level_no);
	void StopLevelMusic();
	void CheckMusicLoop(); // call every frame: manually restarts playback since MCI "repeat" is unreliable for waveaudio
	void ToggleMusic();    // Escape-menu Music checkbox: stop if playing, else (re)start current level's music
	void ToggleLightmaps(); // Escape-menu Lightmaps checkbox: enable/disable applying calculated lightmaps during render
	void UpdateAnimations(float dtSec);
	void SetupRuntimePlayerAnimation();
	void ApplyRuntimeAiAnimationRequests();
	void AdvanceGameplayAnimationClocks();
	std::string BuildAnimStatusString();
	int FindHumanAiTaskId(int objIndex) const;
	const std::vector<int>& GetOrComputeAnimationIds(int objIndex);
	// Pure computation (no cache reads/writes) of GetOrComputeAnimationIds' result —
	// safe to call concurrently from worker threads. Used by LoadLevel's parallel
	// per-AI animation resolution (see app_level.cpp).
	std::vector<int> ComputeAnimationIdsForObject(int objIndex) const;
	void ToggleAnimationForObject(int objIndex, int animId);
	// "Calculate Light Mapping" property-panel button: resolves and applies
	// the selected Building/EditRigidObj's exact-placement baked lightmap.
	// No-op (with a logged warning) if no object is selected or the selected
	// object's task type doesn't carry lightmap bindings.
	void CalculateLightmapForSelectedObject();
	void CalculateLightmapsForAllObjects(); // Escape-menu Lightmaps checkbox ON: every Building/EditRigidObj in the level
	// shared resolve+convert+upload core. olm_from_recalc: the .olm on disk was
	// just re-baked toward the object's live pose (record that as bake pose);
	// otherwise record the authored QSC pose the shipped bake was made at.
	size_t ResolveAndApplyLightmap(LevelObject& obj, const std::string& qscPath,
	                               bool olm_from_recalc = false);
	// Bake the current orientation's re-light into one object's .olm files (game
	// write-back). Returns true if files were rewritten. Used by Save write-back.
	bool RecalcLightmapToOlm(LevelObject& obj, const std::string& qscPath, bool force = false);
	// Auto-triggered on manipulation end: recalculates lightmap for the manipulated
	// object + its ATTA children, reloads into the viewport, and repacks lightmaps.res.
	void AutoRecalcLightmapForManipulated(int objIndex);
	// On Save: for every lightmapped object moved/rotated since its bake, re-light
	// its .olm files and repack lightmaps.res (name-preserving) so igi.exe shows
	// the new lighting. The original lightmaps.res is in the per-level backup.
	void WriteBackRecalculatedLightmaps();
	// Fills the property panel's "Animation Control" section state for the
	// currently selected object (boneHierarchy stays -1 when not applicable).
	void ComputePropAnimUiState(int& boneHierarchy, std::vector<int>& ids, int& activeId, bool& isPlaying);
	// Returns every object index with an active, playing clip and renderable skin
	// geometry — i.e. every AI currently animating in parallel, not just the
	// selected one. Their rigid (static) mesh is skipped in favor of the live
	// skinned draw. Logs whenever the active set changes (object added/removed).
	std::unordered_set<int> GetSkinnedReplacementObjectIndices(bool gameplay_render);
	std::unordered_set<int> animSkinnedIndicesPrev_; // for change-only logging

  // C2: Typed task property editor
  bool prop_editor_open_ = false;
  int prop_field_index_ = -1;
  float prop_drag_start_val_ = 0.f;
  float prop_drag_start_val2_ = 0.f; // second axis (2D pad Y)
  int prop_drag_start_x_ = 0;
  int prop_drag_start_y_ = 0;
  int prop_text_edit_field_ = -1; // -2 = editing note
  int prop_edit_obj_index_ =
      -1; // LevelObject targeted by the active text edit (-1 = selected/parent)
  int prop_drag_obj_index_ = -1; // LevelObject targeted by the active
                                 // slider/pad drag (-1 = selected/parent)
  std::string prop_text_buf_;
  int prop_text_caret_ = 0; // caret index within prop_text_buf_
  // ── Text selection (used by AI Script editor; mouse drag, Shift+arrows, Ctrl+A).
  //   anchor = stationary end (click point), focus = moving end (caret). A
  //   selection exists when anchor >= 0 AND focus >= 0 AND anchor != focus.
  int prop_text_sel_anchor_ = -1;
  int prop_text_sel_focus_  = -1;
  // Mouse-drag selection tracking: set on mousedown in the AI Script text
  // box, cleared on mouseup. While true, the caret is moved with the cursor.
  bool ai_text_dragging_ = false;
  // Last known AI script text-box geometry (set on mousedown, used by
  // mouse-motion to map (x,y) → caret position for drag selection).
  int  ai_text_box_x1_ = 0, ai_text_box_y1_ = 0;
  int  ai_text_box_h_ = 0, ai_text_box_w_ = 0;
  // Text field captured when a model/autocomplete picker opens, so the chosen
  // item is inserted into the exact field the cursor was in — even if focus
  // changed.
  int picker_target_field_ = -1;
  int picker_target_obj_ = -1;
  int picker_target_caret_ = -1; // caret position when picker opened
  int prop_last_drag_dx_ =
      0; // last non-zero X delta (for edge-stuck continuity)
  int prop_last_drag_dy_ =
      0; // last non-zero Y delta (for edge-stuck continuity)
  float prop_drag_speed_ = 0.f; // ramping position-drag speed (units/frame)
                                // while held in a direction
  int prop_panel_scroll_ = 0; // vertical scroll offset in pixels for prop panel

  // C3: Ctrl+F find
  bool find_open_ = false;
  std::string find_query_;
  int find_result_idx_ = -1;
  enum class FindMode { TaskNameTypeId, TextInTask, ById, ByNote, SetId };
  FindMode find_mode_ = FindMode::TaskNameTypeId;

  // AI script editor state (HumanSoldier / HumanAI prop panel section)
  std::string ai_script_path_;
  std::string ai_script_text_;
  bool ai_script_dirty_ = false;
  int ai_script_vscroll_ = 0;      // first visible visual line
  int ai_script_path_hscroll_ = 0; // first visible char in path box

  // AI Script editor undo/redo. Records snapshot the full text + caret+anchor
  // before/after each committed edit (typed character, paste, cut, delete,
  // replace) so Ctrl+Z / Ctrl+Y behave like a standard notepad.
  struct AiTextEdit {
    std::string before;
    std::string after;
    int caret_before = 0;
    int caret_after  = 0;
    int anchor_before = -1;
    int anchor_after  = -1;
  };
  std::vector<AiTextEdit> ai_text_undo_;
  std::vector<AiTextEdit> ai_text_redo_;
  // Cap the history depth — each entry is the full text, so unbounded growth
  // would balloon memory on a long editing session.
  static constexpr int kAiTextUndoMax = 100;

  // Task type view toggle
  bool show_task_type_ = false;

  // Task picker creation flags
  bool task_picker_insert_first_ = false;
  bool task_new_at_camera_ = false;

  // Camera strafe
  bool camera_strafe_free_ = false;

  // File dialog (SaveSubTask, LoadSubTask)
  enum class FileDialogMode {
    None,
    SaveSubTask,
    SaveSubTaskParent,
    LoadSubTask,
    SaveObjectFile
  };
  FileDialogMode file_dialog_mode_ = FileDialogMode::None;
  std::string file_dialog_path_;
  int file_dialog_caret_ = 0;

  // Autocomplete keyword picker (Ctrl+N — left panel)
  bool ac_task_picker_open_ = false;
  int ac_task_selected_idx_ = 0;
  int ac_task_scroll_offset_ = 0;
  std::string ac_task_filter_;
  std::vector<std::string> ac_task_items_;

  // Model ID picker (Ctrl+O — right panel)
  bool model_picker_open_ = false;
  int model_picker_selected_ = 0;
  int model_picker_scroll_ = 0;
  std::string model_picker_filter_;
  int pending_model_source_level_ = 0;

  // AutoComplete inline keywords (Ctrl+Space)
  std::vector<std::string> autocomplete_keywords_;
  // Inline-autocomplete cycling state (Ctrl+Space / Tab): lets repeated presses
  // step through every keyword that shares the originally-typed prefix.
  std::string ac_inline_prefix_;  // original prefix being cycled
  std::string ac_inline_last_kw_; // keyword inserted by the last completion
  int ac_inline_start_ = -1;      // token start position of the last completion
  int ac_inline_idx_ = -1;        // index into the current match set

  // All modelIds from current level objects in XXX_XX_X format (for model
  // picker)
  std::vector<ModelPickerEntry> level_model_entries_;

  bool sync_from_game_once_;
  int last_game_level_;
  int last_loaded_level_ =
      -1; // level currently loaded; -1 = none yet (first load == fresh process)
  int level_root_index_;
  std::vector<LevelObject> clipboard_;

  std::vector<UndoState> undo_stack_;
  std::vector<UndoState> redo_stack_;
  bool undo_state_pushed_for_manip_ = false;

  // Auto-save state
  bool    auto_save_enabled_ = false;
  int     auto_save_interval_seconds_ = 300;
  int64_t auto_save_last_time_ms_ = 0;
  bool    shutdown_started_ = false;

  int64_t prior_frame_time_;

  window_state_s window_state_;
  int editor_viewport_width_ = 1;
  int editor_viewport_height_ = 1;
  int gameplay_viewport_width_ = 1;
  int gameplay_viewport_height_ = 1;
  mouse_state_s mouse_state_;
  input_s input_;

  bool skip_input_on_motion_once_;

  // Game launch state
  struct game_process_s {
    HANDLE hProcess = NULL;
    HANDLE hThread = NULL;
    HANDLE hMonitorThread = NULL;
    DWORD pid = 0;
    bool running = false;
  } game_process_;
  std::atomic<bool> game_exited_{
      false}; // set by monitor thread when game process exits
  HWND editor_hwnd_ = NULL;
  bool gameplay_window_close_requested_ = false;
  // True only while an OS repaint is rendering the editor window during an
  // active gameplay session. This keeps the editor surface visible without
  // allowing that repaint to advance or mutate the runtime session.
  bool rendering_editor_window_ = false;

  bool orbit_active_ = false;
  glm::vec3 orbit_target_pos_ = glm::vec3(0.0f);
  float orbit_distance_ = 0.0f;

  // viewer
  struct viewer_s {
    glm::vec3 pos_;
    glm::vec3 forward_;
    glm::vec3 right_;
    glm::vec3 up_;
    glm::vec3 velocity_;
    float yaw_;   // in degree
    float pitch_; // in degree
    float roll_;  // in degree
    float move_speed_;
    float jump_speed_;
    bool clip_to_z_;
    bool on_ground_;
  } viewer_;
  viewer_s gameplay_viewer_;
  igi::RuntimeMapComputerCamera gameplay_map_computer_camera_;
  bool gameplay_map_computer_open_ = false;

  enum class ManipulationMode {
    None,
    MoveXY,
    MoveXZ,
    RotateAlpha,
    RotateBeta,
    RotateGamma
  };

  struct marker_manip_s {
    ManipulationMode mode_ = ManipulationMode::None;
    int start_x_ = 0;
    int start_y_ = 0;
    glm::dvec3 start_pos_;
    glm::vec3 start_rot_;
  } marker_manip_;
  bool manipulation_dirty_ = false;

  // Graph-node drag state (parallel to marker_manip_, for AIGraph node editing).
  struct graph_node_manip_s {
    bool active_ = false;
    int  start_x_ = 0;
    int  start_y_ = 0;
    glm::dvec3 start_pos_;
  } graph_node_manip_;
  void UpdateGraphNodeManipulation(int x, int y);

  void Frame(float delta_seconds);
  void ApplyViewportSize(int width, int height);
  void RestoreEditorViewport();
  void DrawGameplayPlayerWeapon();
  void DrawGameplayProjectiles();
  void DrawGameplayExplosions();
  void DrawGameplayGuardMuzzleFlashes();
  void UpdateGameplayFieldOfView();
  void ApplyRuntimeCutSceneCamera();
  void UpdateGameplayMapComputerCamera(float delta_seconds);
  void CaptureGameplayRenderSnapshot();
  void DispatchEventBindings();

  void ProcessInput(float delta_seconds);
  bool CheckCollision(const glm::vec3 &next_pos);
  igi::RuntimeInteractionResult HandleGameplayInteraction(
      const glm::vec3& interaction_origin,
      const glm::vec3& interaction_direction);
  void SnapObjectsToTerrain();
  void EvaluateTrainTrackPositions();

  bool stick_to_ground_ = false;
  bool noclip_mode_ = true; // By default true as requested by user
  void UpdateViewerVectors();
  void UpdateGameplayViewerVectors();
  void FocusGameplayWindow();
  void FocusEditorWindow();
  void CaptureEditorSnapshotForGameplayApply();
  bool IsGameplayInputFocused() const {
    return gameplay_host_.GetInputRouter().GetFocus() ==
      igi::WindowFocusTarget::GameplayWindow;
  }
  bool IsEditorInputActive() const {
    return igi::IsEditorInteractionActive(
      pause_mode_, in_game_mode_, IsGameplayInputFocused());
  }
  void UpdateViewerVectors(viewer_s& viewer);
  void UpdateViewDefine();
  void UpdateGameplayViewDefine();
  void UpdateViewDefine(const viewer_s& viewer, view_define_s& view_define);
  bool IsGameplayRenderTarget() const {
    return igi::ResolveRenderTarget(
               in_game_mode_, rendering_editor_window_) ==
           igi::RenderTarget::Gameplay;
  }
  void EditorProcessClick();
  void ApplyPropPositionDrag(); // per-frame velocity-ramped position drag (pad
                                // / Z slider)
  void UpdateMarkerManipulation();
  void PropagateTransformToChildren(int parentIdx, const glm::dvec3 &deltaPos,
                                    const glm::dmat3 &deltaWorld,
                                    const glm::dvec3 &pivot);
  void PushUndoState();
  void ToggleAutoSave();
  void AdjustAutoSaveInterval(int delta_seconds);
  // C2: property-panel text-edit helpers.
  void
  CommitPropTextEdit(); // writes prop_text_buf_ to argTokens; clears edit state
  bool IsPropFieldMultiline(int field) const; // VarString/String256 box?
  static std::string StripQuotes(const std::string &s);
  void LoadAIScriptForSelected();
  void UpdateAIScriptScroll();      // keep vscroll so caret is visible
  void UpdateAIScriptPathHScroll(); // keep hscroll so caret is visible
  void SaveAndReloadObjects();
  void Undo();
  void Redo();

  // ── AI Script editor helpers (notepad-style) ──
  // The Ctrl+C/V/X/A/Z/Y shortcuts and mouse-drag selection only activate
  // when the AI Script text field is focused (prop_text_edit_field_ ==
  // kAIScriptTextField). All other property text fields ignore them so the
  // editor-level bindings (undo stack, find, etc.) still fire as before.
  bool IsAIScriptTextFocused() const {
    return prop_text_edit_field_ == PropPanel::kAIScriptTextField;
  }
  // Push the current AI Script buffer + caret + selection onto the undo
  // stack. Called immediately BEFORE any mutating operation (insert, delete,
  // paste, cut, replace). Captures state at rest, not the post-edit state.
  void PushAiTextUndo();
  // Notepad-style edit ops (operate on prop_text_buf_ + prop_text_caret_ +
  // prop_text_sel_*, and sync ai_script_text_ + ai_script_dirty_). Each
  // mutating op pushes an undo entry. No-ops if the AI Script editor isn't
  // focused.
  void SelectPropTextAll();
  void AiScriptCopy();
  void AiScriptCut();
  void AiScriptPaste();
  void AiScriptUndo();
  void AiScriptRedo();
  void AiScriptDeleteSelection();
  // Insert plain text at the caret (replaces any active selection). Used by
  // typing, paste, and autocomplete.
  void AiScriptInsertText(const std::string& s);
  // Clear the current text selection. Safe to call even when no selection.
  void ClearPropTextSelection() {
    prop_text_sel_anchor_ = -1;
    prop_text_sel_focus_  = -1;
  }
  // True iff the current prop text field has a non-empty selection.
  bool HasPropTextSelection() const;
  // Returns the [start,end) of the selection in caret order (start <= end).
  void GetPropTextSelection(int& selStart, int& selEnd) const;
  // Inline synonym (used inside the hot input loops where verbosity matters).
  bool isPropTextSel() const { return HasPropTextSelection(); }
  // Mirror the live edit buffer into the committed ai_script_text_ and mark
  // the script dirty so the save hotkey picks it up. No-op when the AI Script
  // text field isn't the active prop text field.
  void SyncAIScriptBuffer() {
    if (prop_text_edit_field_ == PropPanel::kAIScriptTextField) {
      ai_script_text_  = prop_text_buf_;
      ai_script_dirty_ = true;
    }
  }

  // If the graph overlay is visible and a loaded AIGraph task exists in the
  // current objects list, copy that task's world pos into the renderer's
  // overlay offset so the 3D nodes/edges follow the task live (F7 view).
  void SyncGraphOverlayOffsetFromAIGraph();
  // Registers every in-level enemy (soldier family) into the runtime AiSystem
  // using its AIGraph waypoints. Runtime transforms are rendered through the
  // session-owned LevelObjects copy.
  void SetupLevelAiGuards();
  void InitializeGameplayMissionObjectives();
  void ConfigureGameplayExtractionFallback(
      const glm::vec3& spawn_position,
      float spawn_yaw);
  void SetupRuntimeMissionState();
  void SetupRuntimeInteractionState();
  void SetupRuntimeDoors();
  void ApplyRuntimeConditionalContainerStates();
  void ApplyRuntimeGuardGeneratorStates();
  void ApplyRuntimeDoorStates();
  void ApplyRuntimeExplodeObjectStates();
  void SetupRuntimeLadders();
  void CollectAttachedRuntimeLadders(
      const std::string& model_id,
      bool is_building,
      const glm::mat4& draw_world_matrix,
      const glm::mat3& task_orientation,
      std::unordered_set<std::string>& ancestry,
      std::vector<igi::LadderPlacement>& ladder_placements);
  void AddRuntimeLadderIfPresent(
      const std::string& model_id,
      bool is_building,
      const glm::mat4& draw_world_matrix,
      const glm::mat3& task_orientation,
      std::vector<igi::LadderPlacement>& ladder_placements);
  LevelObjects& GetActiveRenderLevelObjects();
  void PushTaskEditorUndoState();
  void UndoTaskEditorChange();
  void RedoTaskEditorChange();
  void ReplaceTaskEditorSelection(const std::string &text);
  void SyncSelectedTaskToLiveQsc(bool keepEditorOpen);
  void SaveTaskEditorChanges(bool keepEditorOpen);

  bool IsComputer(const LevelObject &obj);
  bool IsWaterTower(const LevelObject &obj);
  bool ValidateParentChildCompatibility(
      const LevelObject &parent, const std::vector<LevelObject> &addedSubtree);
  void SaveTaskSubtreeToFile(int idx, const std::string &path);
  void ConfirmFileDialog();
  void LoadAutoCompleteKeywords();
  bool InlineAutocomplete(); // complete/cycle the token left of the caret
  void RebuildLevelModelIds();

public:
  // QSC/QVM workflow
  void LoadQSCForLevel(int level_no);
  void SaveAndCompile();
  void DecompileFromGame(int level_no);
  void ProcessTreeViewClick(int mx, int my);
  void ProcessTreeViewHover(int mx, int my);
  void CreateNewTask();
  void DeleteSelectedTask();
  void CopySelectedTask(bool includeSubtree);
  void PasteTask();
  void AssignTaskID();
  void ModifyTaskParameters();
  void LookupSelectedModelName();
  void LookupSelectedModelId();
  void CopySelectedModelName();
  void CopySelectedModelId();
  void SearchModelById(std::optional<std::string> query = std::nullopt);
  void SearchModelByName(std::optional<std::string> query = std::nullopt);
  void LookupHoveredModelName();
  void LookupHoveredModelId();
  void ClearStatusMessage();
};
