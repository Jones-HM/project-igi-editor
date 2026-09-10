#include "app_internal.h"
#include "runtime/config_qvm.h"
#include "runtime/human_player_config.h"
#include "runtime/audio_system.h"
#include "runtime/gameplay_spawn.h"
#include "runtime/log_policy.h"
#include "runtime/map_computer_camera.h"
#include "runtime/auto_save_policy.h"
#include "runtime/progress_overlay_policy.h"
#include "runtime/window_style_policy.h"
#include "mission_flow_loader.h"
#include "mission_objective_loader.h"
#include "mission_state_loader.h"
#include <charconv>
#include <algorithm>
#include <array>
#include <cmath>

// GameMonitorParam, GameMonitorProc, and HOTKEY_ID_TOGGLE_GAME live in
// app_internal.h (shared with app_editor.cpp's LaunchGame). The mutable window
// subclass globals below are used only here.

// â”€â”€ Global hotkey support â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// We subclass GLUT's window so WM_HOTKEY messages reach our code even when
// the editor is iconified and the game has keyboard focus.
static WNDPROC g_origEditorWndProc = nullptr;
static App*    g_appForHotkey      = nullptr;

static LRESULT CALLBACK EditorSubclassWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_HOTKEY && static_cast<int>(wParam) == HOTKEY_ID_TOGGLE_GAME) {
		Logger::Get().Log(LogLevel::INFO, "[ToggleGame] Global hotkey fired");
		if (g_appForHotkey) g_appForHotkey->LaunchGame();
		return 0;
	}
	return CallWindowProc(g_origEditorWndProc, hwnd, msg, wParam, lParam);
}

static std::string ResolveMissionTextResource(
	const std::array<std::string, 2>& archive_paths,
	const std::string& resource_key) {
	for (const std::string& archive_path : archive_paths) {
		if (!std::filesystem::exists(archive_path)) {
			continue;
		}

		const std::vector<uint8_t> resource_data =
			RES_Extract(archive_path, resource_key);
		if (resource_data.empty()) {
			continue;
		}

		const auto terminator = std::find(
			resource_data.begin(),
			resource_data.end(),
			static_cast<uint8_t>(0));
		return std::string(resource_data.begin(), terminator);
	}
	return resource_key;
}

/*
================================================================================
 App
================================================================================
*/

App::App():
	frame_(0),
	terrain_mod_options_(-1),
	edit_mode_(true), // Enable by default as requested
	terrain_edit_enabled_(false),
	pause_mode_(false),
	edit_brush_(0), // 0: raise, 1: lower
	selected_object_index_(0),
	hover_object_index_(-1),
	show_hud_(true),
	show_debug_(false),
	show_help_(false),
	show_magic_obj_spheres_(false),
	tree_scroll_offset_(0),
	tree_decl_expanded_(false),
	status_message_(),
	noclip_mode_(true), // By default true as requested by user
	prior_frame_time_(0),
	skip_input_on_motion_once_(false)
{
	view_define_.pos_ = glm::vec3(0.0f);
	view_define_.forward_ = VEC3_Y_DIR;
	view_define_.right_ = VEC3_X_DIR;
	view_define_.up_ = VEC3_Z_DIR;
	view_define_.fovx_ = glm::radians(FOVY_IN_DEGREE);
	view_define_.fovy_ = glm::radians(FOVY_IN_DEGREE);
	view_define_.render_z_near_ = RENDER_Z_NEAR;
	view_define_.render_z_far_ = RENDER_Z_FAR;
	view_define_.render_min_depth_ = RENDER_DEPTH_MIN;
	view_define_.render_max_depth_ = RENDER_DEPTH_MAX;
	view_define_.viewport_width_ = 1;
	view_define_.viewport_height_ = 1;

	draw_params_.view_define_ = &view_define_;
	draw_params_.overlay_wireframe_ = false;
	draw_params_.draw_parts_ = -1;
	draw_params_.draw_terrain_options_ = -1;
	draw_params_.flat_sky_layer_is_visible_ = true;
	draw_params_.num_terrain_render_chunk_ = 0;
	draw_params_.selected_object_index_ = -1;

	memset(&window_state_, 0, sizeof(window_state_));
	memset(&mouse_state_, 0, sizeof(mouse_state_));
	memset(&input_, 0, sizeof(input_));

	window_state_.cursor_visible_ = true;

	memset(&viewer_, 0, sizeof(viewer_));
	viewer_.clip_to_z_ = false;
	viewer_.move_speed_ = MIN_MOVE_SPEED;
	viewer_.jump_speed_ = MIN_JUMP_SPEED;
	memset(&gameplay_viewer_, 0, sizeof(gameplay_viewer_));
	gameplay_viewer_.clip_to_z_ = false;
	gameplay_viewer_.move_speed_ = MIN_MOVE_SPEED;
	gameplay_viewer_.jump_speed_ = MIN_JUMP_SPEED;
	window_state_.cursor_visible_ = true;
}

App::~App() {
	Shutdown();
}

bool App::Init(int argc, char** argv) {
	// Initialize logger with absolute path to exe directory
	std::string exeDir = Utils::GetExeDirectory();
	Logger::Get().Init(exeDir + "\\igi1ed.log");
	Logger::Get().Log(LogLevel::INFO, "IGI Editor Initializing...");
	igi::AudioSystem::Initialize(Utils::GetIGIRootPath());

	if (!renderer_.Init()) {
		return false;
	}

	ConfigData& cfg = Config::Get();
	// Native diagnostic sessions need their load-time evidence before the
	// initial level is opened. This changes only the in-memory developer
	// session; the authored qedconfig QSC/QVM remains untouched.
	developer_mode_ = Arg_OptionIdx(argc, argv, "--developer-mode") > -1;
	if (developer_mode_) {
		cfg.enableLogging = true;
		if (cfg.logLevelThreshold > igi::kLogLevelInfo)
			cfg.logLevelThreshold = igi::kLogLevelInfo;
	}

	renderer_.SetLightmapsEnabled(cfg.enableLightmaps);
	renderer_.SetFogEnabled(cfg.enableFog);
	renderer_.SetFogIntensity(cfg.fogIntensity);

	auto_save_enabled_ = cfg.auto_save_enabled;
	auto_save_interval_seconds_ = cfg.auto_save_interval_seconds;
	auto_save_last_time_ms_ = Sys_Milliseconds();

	// read options from command line
	draw_params_.overlay_wireframe_ = Arg_OptionIdx(argc, argv, "-wireframe") > 0;
	draw_params_.draw_parts_ = Arg_ReadInt(argc, argv, "-draw_parts", -1);
	draw_params_.draw_terrain_options_ = Arg_ReadInt(argc, argv, "-draw_terrain_opts", -1);
	// Apply config fog preference to terrain draw options on startup.
	if (!cfg.enableFog)
		draw_params_.draw_terrain_options_ &= ~Renderer_Terrain::DRAW_TERRAIN_OPT_FOG;
	terrain_mod_options_ = Arg_ReadInt(argc, argv, "-terrain_mod_opts", terrain_mod_options_);
	stick_to_ground_ = Arg_OptionIdx(argc, argv, "-stick_to_ground") > 0;

	int start_level = Arg_ReadInt(argc, argv, "-level", cfg.level);
	if (start_level >= MIN_LEVEL_NO && start_level <= MAX_LEVEL_NO) {
		try {
			LoadLevel(start_level);
		}
		catch (const std::exception& e) {
			std::string errorMsg = "Failed to load level " + std::to_string(start_level) + ":\n" + std::string(e.what());
			Utils::LogAndShowError(errorMsg, "IGI Editor - Error");
			Logger::Get().Log(LogLevel::ERR, errorMsg);
		}
		catch (...) {
			std::string errorMsg = "Failed to load level " + std::to_string(start_level) + ":\nUnknown error occurred.";
			Utils::LogAndShowError(errorMsg, "IGI Editor - Error");
			Logger::Get().Log(LogLevel::ERR, errorMsg);
		}
	}


	if (Arg_OptionIdx(argc, argv, "-yaw") > -1) {
		// override yaw
		viewer_.yaw_ = Arg_ReadFloat(argc, argv, "-yaw", 0.0f);
		UpdateViewerVectors();
	}

	if (Arg_OptionIdx(argc, argv, "-pitch") > -1) {
		// override pitch
		viewer_.pitch_ = Arg_ReadFloat(argc, argv, "-pitch", 0.0f);
		UpdateViewerVectors();
	}

	int wnd_w = Arg_ReadInt(argc, argv, "-w", 800);
	int wnd_h = Arg_ReadInt(argc, argv, "-h", 600);
	OnWindowResize(wnd_w, wnd_h);

	prior_frame_time_ = Sys_Milliseconds();

	bridge_.SetEnabled(show_hud_);
	bridge_.Start();

	if (developer_mode_) {
		debug_cmd_mgr_.Start();
		Logger::Get().Log(LogLevel::INFO, "[App] Developer Mode ON via command line");
	}
	// Set initial cursor state
	LoadAllCursors();
	LoadHelpEntries();
	LoadAutoCompleteKeywords();
	glutSetCursor(GLUT_CURSOR_NONE);

	// Cache editor HWND for minimize/restore around game launch
	editor_hwnd_ = Utils::FindWindow("IGI Editor");
	if (!editor_hwnd_) editor_hwnd_ = GetActiveWindow();

	// Subclass GLUT's window so WM_HOTKEY messages reach EditorSubclassWndProc
	// even when the editor is iconified and the game holds keyboard focus.
	if (editor_hwnd_) {
		g_appForHotkey     = this;
		g_origEditorWndProc = reinterpret_cast<WNDPROC>(
			SetWindowLongPtr(editor_hwnd_, GWLP_WNDPROC,
			                 reinterpret_cast<LONG_PTR>(EditorSubclassWndProc)));
		Logger::Get().Log(LogLevel::INFO, "[App] Editor window subclassed for global hotkey (HWND=" +
		                  std::to_string(reinterpret_cast<uintptr_t>(editor_hwnd_)) + ")");
	} else {
		Logger::Get().Log(LogLevel::WARNING, "[App] editor_hwnd_ is NULL â€” global hotkey will not work");
	}

#ifdef _WIN32
	// Borderless fullscreen: strip caption/frame/box chrome and size the
	// surface to the monitor. Pause -> Quit stays the supported exit path.
	if (editor_hwnd_) {
		const LONG_PTR style = GetWindowLongPtr(editor_hwnd_, GWL_STYLE);
		SetWindowLongPtr(editor_hwnd_, GWL_STYLE,
		                 static_cast<LONG_PTR>(
		                     igi::BorderlessFullscreenWindowStyle(style)));
		MONITORINFO monitor_info = {sizeof(MONITORINFO)};
		if (GetMonitorInfo(MonitorFromWindow(editor_hwnd_, MONITOR_DEFAULTTONEAREST),
		                   &monitor_info)) {
			SetWindowPos(editor_hwnd_, HWND_TOP,
			             monitor_info.rcMonitor.left, monitor_info.rcMonitor.top,
			             monitor_info.rcMonitor.right - monitor_info.rcMonitor.left,
			             monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top,
			             SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
			window_state_.old_viewport_width_ = window_state_.viewport_width_;
			window_state_.old_viewport_height_ = window_state_.viewport_height_;
			window_state_.full_screen_ = true;
			Logger::Get().Log(LogLevel::INFO, "[App] Editor window switched to borderless fullscreen");
		}
	}
#endif

	extern App g_app;
	static auto s_terrain_cb = [](float x, float y) -> float {
		extern App g_app;
		float z = 0.0f;
		g_app.GetLevelZ(x, y, z);
		return z;
	};
	static auto s_collision_cb = [](float x, float y, float z) -> bool {
		extern App g_app;
		return g_app.CheckCollision(glm::vec3(x, y, z));
	};
	gameplay_host_.Initialize(s_terrain_cb, s_collision_cb);
	gameplay_host_.SetGameplayInputModifier(
		[this](uint64_t, igi::PlayerInputCmd& input_command) {
			player_animation_driver_.AugmentInput(
				gameplay_host_.GetWorld(),
				input_command);
		});
	gameplay_host_.GetWorld().SetInteractionQuery(
		[this](const glm::vec3& origin, const glm::vec3& direction) {
			return HandleGameplayInteraction(origin, direction);
		});

	return true;
}

void App::Shutdown() {
	// main and the global App destructor can both reach shutdown. Teardown must
	// run once so renderer and gameplay resources are not invalidated twice.
	if (shutdown_started_) return;
	shutdown_started_ = true;

	if (igi::ShouldSaveBeforeEditorExit(
			!in_game_mode_, Config::Get().auto_save_enabled, level_.GetLevelNo())) {
		SaveCurrentLevel();
	}

	if (editor_hwnd_) {
		KillTimer(editor_hwnd_, 1);
		UnregisterHotKey(editor_hwnd_, HOTKEY_ID_TOGGLE_GAME);
		if (g_origEditorWndProc) {
			SetWindowLongPtr(editor_hwnd_, GWLP_WNDPROC,
			                 reinterpret_cast<LONG_PTR>(g_origEditorWndProc));
		}
	}
	g_appForHotkey = nullptr;
	g_origEditorWndProc = nullptr;

	gameplay_host_.SetGameplayInputModifier({});
	player_animation_driver_.ClearAnimationClips();
	if (in_game_mode_) {
		igi::EditorSnapshot restored_snapshot;
		gameplay_host_.CloseGameplay(restored_snapshot);
		in_game_mode_ = false;
		runtime_level_objects_.reset();
		runtime_initial_deleted_flags_.clear();
		runtime_conditionally_hidden_flags_.clear();
		runtime_guard_generator_hidden_flags_.clear();
	}
	gameplay_host_.ShutdownGameplayWindow();
	if (game_process_.running) {
		// Terminate an editor-owned game before tearing down its handles. The
		// monitor thread waits on hProcess, so it must be joined before hProcess
		// is closed or the editor can crash during quit.
		if (game_process_.hProcess) {
			TerminateProcess(game_process_.hProcess, 0);
			WaitForSingleObject(game_process_.hProcess, 2000);
		}
		if (game_process_.hMonitorThread) {
			WaitForSingleObject(game_process_.hMonitorThread, 2000);
			CloseHandle(game_process_.hMonitorThread);
		}
		if (game_process_.hProcess) CloseHandle(game_process_.hProcess);
		if (game_process_.hThread) CloseHandle(game_process_.hThread);
		game_process_ = {};
	}
	bridge_.Stop();
	StopLevelMusic();
	level_.Unload();
	level_.FreeTerrainCubeDataPools();
	animPlaybacks_.clear();
	gameplayAnimPlaybacks_.clear();
	lastGameplayAnimationTick_ = 0;
	animIdsCache_.clear();
	runtime_animation_request_serials_.clear();
	animRegistry_.Clear();
	renderer_.Shutdown();
	if (!g_isCLIMode) {
		AssetExtractor::CleanupExtractedAssets(Utils::GetExeDirectory());
	}
}

// â”€â”€ C1: Custom SPR cursor â€” multi-mode â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€


int App::GetCurLevelNo() const {
	return level_.GetLevelNo();
}

void App::ToggleOverlayWireframe() {
	draw_params_.overlay_wireframe_ = !draw_params_.overlay_wireframe_;
}

void App::ToggleDrawParts(int part) {
	if (draw_params_.draw_parts_ & part) {
		draw_params_.draw_parts_ &= ~part;
	}
	else {
		draw_params_.draw_parts_ |= part;
	}
}

void App::SetDrawParts(int parts) {
	draw_params_.draw_parts_ = parts;
}

void App::ToggleTerrainDrawOption(int opt) {
	if (draw_params_.draw_terrain_options_ & opt) {
		draw_params_.draw_terrain_options_ &= ~opt;
	}
	else {
		draw_params_.draw_terrain_options_ |= opt;
	}
}

void App::SetFogEnabled(bool enabled) {
    renderer_.SetFogEnabled(enabled);
}

void App::SetFogIntensity(int intensity) {
    renderer_.SetFogIntensity(intensity);
}

void App::ToggleTerrainModOption(int opt) {
	if (terrain_mod_options_ & opt) {
		terrain_mod_options_ &= ~opt;
	}
	else {
		terrain_mod_options_ |= opt;
	}
}

bool App::GetOverlayWireframe() const {
	return draw_params_.overlay_wireframe_;
}

int	App::GetDrawParts() const {
	return draw_params_.draw_parts_;
}

int	App::GetTerrainDrawOptions() const {
	return draw_params_.draw_terrain_options_;
}

int	App::GetTerrainModOptions() const {
	return terrain_mod_options_;
}

// events
void App::OnWindowResize(int width, int height) {
	editor_viewport_width_ = std::max(1, width);
	editor_viewport_height_ = std::max(1, height);
	ApplyViewportSize(editor_viewport_width_, editor_viewport_height_);
}

void App::OnGameplayWindowResize(int width, int height) {
	gameplay_viewport_width_ = std::max(1, width);
	gameplay_viewport_height_ = std::max(1, height);
	ApplyViewportSize(gameplay_viewport_width_, gameplay_viewport_height_);
}

void App::ApplyViewportSize(int width, int height) {
	width = std::max(1, width);
	height = std::max(1, height);
	window_state_.viewport_width_ = width;
	window_state_.viewport_height_ = height;

	view_define_.viewport_width_ = window_state_.viewport_width_;
	view_define_.viewport_height_ = window_state_.viewport_height_;

	glViewport(0, 0, width, height);

	// update fovx_
	float h = std::tan(view_define_.fovy_ * 0.5f);
	float w = h * width / height;
	view_define_.fovx_ = std::atan(w) * 2.0f;

	float tan_half_fovx = (float)std::tan(view_define_.fovx_ * 0.5);
	float tan_half_fovy = (float)std::tan(view_define_.fovy_ * 0.5);

	view_define_.tan_half_fovx_ = tan_half_fovx;
	view_define_.tan_half_fovy_ = tan_half_fovy;

	view_define_.half_viewport_width_div_tan_half_fovx_ = window_state_.viewport_width_ * 0.5f / tan_half_fovx;
	view_define_.half_viewport_height_div_tan_half_fovy_ = window_state_.viewport_height_ * 0.5f / tan_half_fovy;

}

void App::RestoreEditorViewport() {
	ApplyViewportSize(editor_viewport_width_, editor_viewport_height_);
}

void App::OnDisplay() {
	// A synchronous progress overlay owns the surface while it presents its
	// frame; a scene repaint dispatched by the overlay's event pump would
	// redraw and swap over the bar before it is ever seen.
	if (igi::ShouldSuppressSceneRepaint({progress_overlay_active_, false})) return;
	// Same-window gameplay: the shared surface renders gameplay whenever the
	// runtime session is active, and the editor scene otherwise.
	if (in_game_mode_ && !rendering_editor_window_) {
		OnGameplayDisplay();
		return;
	}
	// Repaint the editor surface from its authoring state while gameplay owns
	// the other window. The render-only flag prevents this zero-delta repaint
	// from advancing physics, consuming input, or changing runtime objects.
	ApplyViewportSize(editor_viewport_width_, editor_viewport_height_);
	rendering_editor_window_ = in_game_mode_;
	Frame(0.0f);
	rendering_editor_window_ = false;
}

bool App::InitializeGameplayWindow(
	int editor_window_id,
	int width,
	int height,
	const igi::GameplayWindowCallbacks& callbacks) {
	gameplay_viewport_width_ = std::max(1, width);
	gameplay_viewport_height_ = std::max(1, height);
	return gameplay_host_.InitializeGameplayWindow(
		editor_window_id, width, height, callbacks);
}

void App::ShutdownGameplayWindow() {
	gameplay_host_.ShutdownGameplayWindow();
}

void App::OnGameplayDisplay() {
	if (!in_game_mode_) return;
	// Same-window gameplay fills whatever size the shared window is now; the
	// startup -w/-h values are only a fallback for headless contexts.
	int width = glutGet(GLUT_WINDOW_WIDTH);
	int height = glutGet(GLUT_WINDOW_HEIGHT);
	gameplay_viewport_width_ = std::max(1, width);
	gameplay_viewport_height_ = std::max(1, height);
	ApplyViewportSize(gameplay_viewport_width_, gameplay_viewport_height_);
	Frame(0.0f);
}

void App::OnGameplayWindowClose() {
    gameplay_host_.NotifyGameplayWindowClosed();
    gameplay_window_close_requested_ = true;
}

void App::FocusGameplayWindow() {
	if (!in_game_mode_) return;

	gameplay_host_.FocusGameplayWindow();
	ApplyViewportSize(gameplay_viewport_width_, gameplay_viewport_height_);
	glutSetCursor(GLUT_CURSOR_NONE);
	mouse_state_.prior_x_ = gameplay_viewport_width_ >> 1;
	mouse_state_.prior_y_ = gameplay_viewport_height_ >> 1;
	glutWarpPointer(mouse_state_.prior_x_, mouse_state_.prior_y_);
	edit_mode_ = false;
	terrain_edit_enabled_ = false;
	show_hud_ = false;
	status_message_ = "Gameplay focus active (F6 focuses editor)";
}

void App::FocusEditorWindow() {
	if (!in_game_mode_) return;

	gameplay_host_.FocusEditorWindow();
	RestoreEditorViewport();
	glutSetCursor(GLUT_CURSOR_NONE);
	edit_mode_ = true;
	terrain_edit_enabled_ = false;
	show_hud_ = true;
	if (gameplay_editor_snapshot_.has_value()) {
		selected_object_index_ = gameplay_editor_snapshot_->selected_object_id;
	}
	status_message_ = "Editor focus active; press F5 to apply changes and restart gameplay";
}

void App::CaptureEditorSnapshotForGameplayApply() {
	if (!gameplay_editor_snapshot_.has_value()) return;

	// Preserve the original editor-mode flags captured at OpenGameplay while
	// refreshing the authoring camera/selection that may have changed in the
	// editor window during this focused-edit interval.
	gameplay_editor_snapshot_->camera_pos = viewer_.pos_;
	gameplay_editor_snapshot_->camera_yaw = viewer_.yaw_;
	gameplay_editor_snapshot_->camera_pitch = viewer_.pitch_;
	gameplay_editor_snapshot_->selected_object_id = selected_object_index_;
}

// AI text editor helpers â€” must be defined before Input_OnMouse and Input_OnSpecial.

// Returns flat text offsets of each visual line start.
// Lines split on '\n'; lines longer than max_chars wrap to the next visual line.

// input
void App::OnIdle() {
	if (gameplay_window_close_requested_) {
		gameplay_window_close_requested_ = false;
		if (in_game_mode_) ToggleGamePlayMode();
	}

	// freeglut pumps messages with a window-handle filter and misses WM_HOTKEY,
	// which is a thread message only retrievable via PeekMessage(NULL, ...).
	// Poll it here so F3 works while the game is running and editor is iconified.
	if (game_process_.running) {
		MSG msg = {};
		while (PeekMessage(&msg, NULL, WM_HOTKEY, WM_HOTKEY, PM_REMOVE)) {
			if (static_cast<int>(msg.wParam) == HOTKEY_ID_TOGGLE_GAME) {
				Logger::Get().Log(LogLevel::INFO, "[ToggleGame] Global hotkey received â€” stopping game");
				LaunchGame();
			}
		}
	}

	// Check game exit before the frame-rate throttle so it fires on every call,
	// even when GLUT is running slowly while the editor is iconified.
	if (game_process_.running && game_process_.hProcess) {
		bool exited = game_exited_.load(std::memory_order_acquire);
		if (!exited) {
			// Direct non-blocking poll as fallback in case monitor thread signal was missed
			DWORD waitResult = WaitForSingleObject(game_process_.hProcess, 0);
			exited = (waitResult == WAIT_OBJECT_0);
		}
		if (exited) {
			Logger::Get().Log(LogLevel::INFO, "[App] Game process exited (PID=" +
			                  std::to_string(game_process_.pid) + "), restoring editor");
			// The monitor thread is still waiting on hProcess. Join it before
			// closing that handle; closing a handle while another thread waits on
			// it is undefined and caused intermittent editor access violations.
			if (game_process_.hMonitorThread) {
				WaitForSingleObject(game_process_.hMonitorThread, 1000);
				CloseHandle(game_process_.hMonitorThread);
			}
			if (game_process_.hProcess) CloseHandle(game_process_.hProcess);
			if (game_process_.hThread) CloseHandle(game_process_.hThread);
			game_exited_.store(false, std::memory_order_relaxed);
			game_process_ = {};
			prior_frame_time_ = Sys_Milliseconds();
			glutShowWindow();
			glutPostRedisplay();
			if (editor_hwnd_) {
				ShowWindow(editor_hwnd_, SW_RESTORE);
				SetForegroundWindow(editor_hwnd_);
				BringWindowToTop(editor_hwnd_);
			}
			if (editor_hwnd_) {
				KillTimer(editor_hwnd_, 1);
				UnregisterHotKey(editor_hwnd_, HOTKEY_ID_TOGGLE_GAME);
			}
			Logger::Get().Log(LogLevel::INFO, "[ToggleGame] Global hotkey unregistered â€” editor restored");
			if (Config::Get().musicEnabled) PlayLevelMusic(level_.GetLevelNo());
			return;
		}
	}

	// While the game is running the editor window is iconified.
	// glutSwapBuffers() deadlocks on minimized windows, so skip rendering entirely.
	if (game_process_.running) return;

	int64_t cur_time = Sys_Milliseconds();
	int64_t delta_time = cur_time - prior_frame_time_;
	if (delta_time < 16) {
		return;
	}

	const bool profile_frame = []() {
		static const bool enabled = getenv("IGI_PROFILE") != nullptr;
		return enabled;
	}();
	const int64_t frame_start = profile_frame ? Sys_Milliseconds() : 0;

	if (in_game_mode_ && IsGameplayInputFocused()) {
		gameplay_host_.GetInputRouter().SetMapComputerOpen(
			gameplay_host_.GetWorld().IsMapComputerOpen());
		gameplay_host_.MakeGameplayWindowCurrent();
		// Track the live shared-window size so gameplay always fills the
		// window (resize/maximize during a run included).
		gameplay_viewport_width_ = std::max(1, glutGet(GLUT_WINDOW_WIDTH));
		gameplay_viewport_height_ = std::max(1, glutGet(GLUT_WINDOW_HEIGHT));
		ApplyViewportSize(gameplay_viewport_width_, gameplay_viewport_height_);
		Frame(delta_time * 0.001f); // convert to seconds
		gameplay_host_.GetInputRouter().SetMapComputerOpen(
			gameplay_host_.GetWorld().IsMapComputerOpen());
	} else if (in_game_mode_) {
		// The editor may own OS focus while the runtime remains active. Advance
		// the fixed-step session explicitly, then render only the authoring
		// window so editor input and painting do not become gameplay updates.
		gameplay_host_.GetInputRouter().SetMapComputerOpen(
			gameplay_host_.GetWorld().IsMapComputerOpen());
		gameplay_host_.Update(cur_time);
		gameplay_host_.GetInputRouter().SetMapComputerOpen(
			gameplay_host_.GetWorld().IsMapComputerOpen());
		ApplyViewportSize(editor_viewport_width_, editor_viewport_height_);
		rendering_editor_window_ = true;
		Frame(delta_time * 0.001f); // editor camera and authoring tools still tick
		rendering_editor_window_ = false;
	} else {
		Frame(delta_time * 0.001f); // convert to seconds
	}

	if (profile_frame) {
		static int64_t acc_ms = 0;
		static int acc_frames = 0;
		static int64_t acc_wall = 0;
		static int64_t last_report = cur_time;
		acc_ms += Sys_Milliseconds() - frame_start;
		++acc_frames;
		if (cur_time - last_report >= 1000) {
			acc_wall += cur_time - last_report;
			Logger::Get().Log(LogLevel::INFO,
				"[FrameProfile] " + std::to_string(acc_frames) + " frames in " +
				std::to_string(acc_wall) + "ms wall; busy=" +
				std::to_string(acc_ms) + "ms (" +
				std::to_string(acc_frames ? acc_ms / acc_frames : 0) + "ms/frame)");
			acc_ms = acc_frames = acc_wall = 0;
			last_report = cur_time;
		}
	}

	prior_frame_time_ = cur_time;
}

void App::Frame(float delta_seconds) {
	const bool render_gameplay = IsGameplayRenderTarget();
	const igi::RuntimeRenderSnapshot& render_snapshot =
		gameplay_host_.GetRenderSnapshot();
	if (developer_mode_ && (!rendering_editor_window_ || IsEditorInputActive())) {
		debug_cmd_mgr_.Update();
	}
	// Config::Init() can be requested at runtime. Keep the timer's cached
	// values synchronized so a disabled menu/config state cannot leave the old
	// enabled state running for the remainder of the session.
	ConfigData& runtime_cfg = Config::Get();
	if (auto_save_enabled_ != runtime_cfg.auto_save_enabled ||
		auto_save_interval_seconds_ != runtime_cfg.auto_save_interval_seconds) {
		auto_save_enabled_ = runtime_cfg.auto_save_enabled;
		auto_save_interval_seconds_ = runtime_cfg.auto_save_interval_seconds;
		auto_save_last_time_ms_ = Sys_Milliseconds();
	}
	// Auto-save timer
	if (igi::ShouldRunAutoSave(!in_game_mode_, runtime_cfg.auto_save_enabled,
			pause_mode_, level_.GetLevelNo())) {
		int64_t now = Sys_Milliseconds();
		if (now - auto_save_last_time_ms_ >=
				(int64_t)(std::max)(1, runtime_cfg.auto_save_interval_seconds) * 1000) {
			auto_save_last_time_ms_ = now;
			SaveCurrentLevel();
			status_message_ = "Auto-saved level " + std::to_string(level_.GetLevelNo());
		}
	}

	if (pause_mode_ && !rendering_editor_window_) {
		// Skip all updates when paused, just render
		if (render_gameplay) UpdateGameplayViewDefine();
		else UpdateViewDefine();
		if (render_gameplay) CaptureGameplayRenderSnapshot();
		if (mouse_state_.prior_x_ != last_pick_x_ || mouse_state_.prior_y_ != last_pick_y_) {
			hover_object_index_ = PickObjectAtScreenPos(mouse_state_.prior_x_, mouse_state_.prior_y_);
			if (hover_object_index_ >= Renderer::kAttaPickBase) hover_object_index_ = -1; // ATTA hovered (clickable; promote on click)
			last_pick_x_ = mouse_state_.prior_x_;
			last_pick_y_ = mouse_state_.prior_y_;
		}
		float ground_z = 0.0f;
		const viewer_s& active_viewer = render_gameplay ? gameplay_viewer_ : viewer_;
		level_.GetTerrainZ(active_viewer.pos_.x, active_viewer.pos_.y, ground_z);
		int propAnimBoneHierarchy; std::vector<int> propAnimIds; int propAnimActiveId; bool propAnimIsPlaying;
		ComputePropAnimUiState(propAnimBoneHierarchy, propAnimIds, propAnimActiveId, propAnimIsPlaying);
		Renderer::task_tree_view_params_s task_tree_view = {
			.show_hud_ = show_hud_,
			.hud_overlay_visible_ = hud_overlay_visible_,
			.status_msg_ = status_message_,
			.pause_mode_ = true,
			.pause_active_input_ = pause_active_input_,
			.pause_level_input_ = pause_level_input_,
			.pause_search_input_ = pause_search_input_,
		.pause_terrain_expanded_ = pause_terrain_expanded_,
			.terrain_draw_options_ = GetTerrainDrawOptions(),
			.show_debug_ = show_debug_,
			.show_help_ = show_help_,
			.edit_mode_ = edit_mode_,
			.terrain_edit_enabled_ = terrain_edit_enabled_,
			.terrain_mod_options_ = terrain_mod_options_,
			.selected_object_index_ = selected_object_index_,
			.hover_object_index_ = hover_object_index_,
			.hover_tree_index_ = hover_tree_index_,
			.mouse_x_ = mouse_state_.prior_x_,
			.mouse_y_ = mouse_state_.prior_y_,
			.tree_scroll_offset = tree_scroll_offset_,
			.tree_decl_expanded = tree_decl_expanded_,
			.level_objects_ = &GetActiveRenderLevelObjects(),
			.task_picker_open_ = task_picker_open_,
			.task_picker_selected_idx_ = task_picker_selected_idx_,
			.task_picker_scroll_offset_ = task_picker_scroll_offset_,
			.task_picker_search_ = task_picker_search_,
			.enable_camera_mode_ = Utils::IsKeyBindingPressed(Config::Get().keyEnableCamera),
			.prop_editor_open_     = prop_editor_open_,
			.prop_field_index_     = prop_field_index_,
			.prop_text_edit_field_ = prop_text_edit_field_,
			.prop_edit_obj_index_  = prop_edit_obj_index_,
			.prop_drag_obj_index_  = prop_drag_obj_index_,
			.prop_text_buf_        = prop_text_buf_,
			.prop_text_caret_      = prop_text_caret_,
			.prop_text_sel_anchor_ = prop_text_sel_anchor_,
			.prop_text_sel_focus_  = prop_text_sel_focus_,
			.prop_panel_scroll_    = prop_panel_scroll_,
			.find_open_            = find_open_,
			.find_query_           = find_query_,
			.find_result_idx_      = find_result_idx_,
			.selected_obj_is_ai    = (selected_object_index_ >= 0 &&
				selected_object_index_ < (int)level_.GetLevelObjects().GetObjects().size() &&
				(ai_model_ids_.count(level_.GetLevelObjects().GetObjects()[selected_object_index_].modelId) > 0 ||
				 [&]() {
					const std::string& t = level_.GetLevelObjects().GetObjects()[selected_object_index_].type;
					return t == "HumanSoldier" || t == "HumanSoldierFemale" ||
					       t == "HumanPlayer" || t == "HumanSoldierRPG" || t == "HumanAI";
				 }())),
			.in_game_mode_         = render_gameplay,
			.noclip_mode_          = noclip_mode_,
			.player_health_        = render_snapshot.player_health,
			.player_maximum_health_ = render_snapshot.player_maximum_health,
			.player_armor_         = render_snapshot.player_armor,
			.player_maximum_armor_ = render_snapshot.player_maximum_armor,
			.active_weapon_name_   = render_snapshot.active_weapon_name,
			.clip_ammo_            = render_snapshot.clip_ammo,
			.clip_capacity_        = render_snapshot.clip_capacity,
			.reserve_ammo_         = render_snapshot.reserve_ammo,
			.map_computer_open_    = render_snapshot.map_computer_open,
			.map_computer_objectives_ = render_snapshot.map_computer_objectives,
			.objective_text_       = render_snapshot.objective_text,
			.mission_timer_remaining_ticks_ = render_snapshot.mission_timer_remaining_ticks,
			.mission_status_messages_ = render_snapshot.mission_status_messages,
			.help_scroll_offset_   = help_scroll_offset_,
			.help_entries_         = &help_entries_,
			.show_task_type_       = show_task_type_,
			.find_mode_            = (int)find_mode_,
			.file_dialog_mode_     = (int)file_dialog_mode_,
			.file_dialog_path_     = file_dialog_path_,
			.file_dialog_caret_    = file_dialog_caret_,
			.ac_task_picker_open_  = ac_task_picker_open_,
			.ac_task_selected_idx_ = ac_task_selected_idx_,
			.ac_task_scroll_offset_= ac_task_scroll_offset_,
			.ac_task_filter_       = ac_task_filter_,
			.ac_task_items_        = &ac_task_items_,
			.model_picker_open_    = model_picker_open_,
			.model_picker_selected_= model_picker_selected_,
			.model_picker_scroll_  = model_picker_scroll_,
			.model_picker_filter_  = model_picker_filter_,
			.model_entries_        = &level_model_entries_,
			.ai_script_path_       = ai_script_path_,
			.ai_script_text_       = ai_script_text_,
			.ai_script_dirty_      = ai_script_dirty_,
			.terrain_brush_          = edit_brush_,
			.terrain_brush_radius_   = edit_brush_radius_,
			.terrain_brush_strength_ = edit_brush_strength_,
			.auto_save_enabled_        = auto_save_enabled_,
			.auto_save_interval_seconds_ = auto_save_interval_seconds_,
			.logging_enabled_ = Config::Get().enableLogging,
			.log_level_threshold_ = Config::Get().logLevelThreshold,
			.music_on_ = music_playing_,
			.lightmaps_on_ = Config::Get().enableLightmaps,
			.anim_status_  = BuildAnimStatusString(),
			.anim_playing_ = !animPlaybacks_.empty(),
			.anim_debug_visible_ = show_anim_debug_,
			.prop_anim_bone_hierarchy_ = propAnimBoneHierarchy,
			.prop_anim_ids_ = propAnimIds,
			.prop_anim_active_id_ = propAnimActiveId,
			.prop_anim_is_playing_ = propAnimIsPlaying,
		};
		draw_params_.level_objects_ = &GetActiveRenderLevelObjects();
		draw_params_.selected_object_index_ = selected_object_index_;
		draw_params_.show_magic_obj_spheres_ = show_magic_obj_spheres_;
		// Paused: the pause menu (a 2D overlay drawn at the end of renderer_.Draw)
		// must sit in front of the scene, so do NOT draw the live skinned mesh after
		// it (that painted the character on top of the menu). Instead keep the
		// object's normal static mesh in the 3D pass â€” animation isn't advancing
		// while paused anyway â€” by not skipping it here.
		draw_params_.skip_static_draw_indices_ = nullptr;
		draw_params_.terrain_id_at_world_xy_ =
			[this](double x, double y) { return level_.GetTerrainNodeId(x, y); };
		draw_params_.terrain_z_at_world_xy_ =
			[this](double x, double y, float& z) { return level_.GetTerrainZ(x, y, z); };
	renderer_.Draw(draw_params_, task_tree_view);

	DrawCustomCursor();
	glutSwapBuffers();
	return;
    }

	frame_++;
	frame_ %= 0xFFFFFFFF;	// reserve value 0xFFFFFFFF (-1) for INVALID_FRAME

	// Feed per-frame delta to renderer for continuous animations (rotor spin etc).
	// This is the minimal glue required so the committed rotor fix keeps working
	// after overlaying the exact fog implementation from v3.6.0-pre.
	extern float g_renderer_delta_secs;
	g_renderer_delta_secs = delta_seconds;

	if (render_gameplay) {
		int64_t now_ms = Sys_Milliseconds();
		const bool profile_frames = []() {
			static const bool enabled = getenv("IGI_PROFILE") != nullptr;
			return enabled;
		}();
		const int64_t profile_frame_start = profile_frames ? Sys_Milliseconds() : 0;

		// OnIdle owns the simulation update. Display callbacks may run once per
		// window, so a zero render delta must never advance gameplay a second time.
		if (delta_seconds > 0.0f) gameplay_host_.Update(now_ms);
		const int64_t profile_after_sim = profile_frames ? Sys_Milliseconds() : 0;
		ApplyRuntimeConditionalContainerStates();
		ApplyRuntimeDoorStates();
		ApplyRuntimeExplodeObjectStates();

		const auto& player = gameplay_host_.GetWorld().GetPlayer();
		gameplay_viewer_.pos_ = player.GetEyePosition();
		gameplay_viewer_.yaw_ = player.GetYaw();
		gameplay_viewer_.pitch_ = player.GetPitch();
		UpdateGameplayViewerVectors();
		ApplyRuntimeCutSceneCamera();
		UpdateGameplayMapComputerCamera(delta_seconds);
		// Publish the simulation-owned guard transforms before synchronizing the
		// mutable render copy. Gameplay presentation must not iterate AI storage
		// while the fixed-step world can be replaced or restarted.
		CaptureGameplayRenderSnapshot();

		// Update the session-owned render copy. Authoring objects remain immutable
		// while gameplay is running and are restored by dropping this copy.
		const auto& guards = render_snapshot.guards;
		auto& objects = GetActiveRenderLevelObjects().GetObjects();
		for (const auto& guard : guards) {
			if (guard.guard_id < objects.size()) {
				const size_t guard_object_index = static_cast<size_t>(guard.guard_id);
				objects[guard_object_index].deleted =
					guard.state == igi::AiGuardState::Dead ||
					!guard.runtime_enabled;
				if (objects[guard_object_index].deleted) {
					continue;
				}
				objects[guard_object_index].pos = glm::dvec3(
					guard.position.x,
					guard.position.y,
					guard.position.z);
				objects[guard_object_index].rot.z = glm::radians(
					static_cast<double>(guard.yaw));
			}
		}
		ApplyRuntimeGuardGeneratorStates();
		AdvanceGameplayAnimationClocks();
		if (profile_frames) {
			static int64_t acc_total = 0, acc_sim = 0, acc_sync = 0;
			static int acc_frames = 0;
			const int64_t frame_end = Sys_Milliseconds();
			acc_sim += profile_after_sim - profile_frame_start;
			acc_sync += frame_end - profile_after_sim;
			acc_total += frame_end - profile_frame_start;
			if (++acc_frames >= 60) {
				Logger::Get().Log(LogLevel::INFO,
					"[Profile] 60 frames: sim=" + std::to_string(acc_sim) +
					"ms sync=" + std::to_string(acc_sync) +
					"ms total=" + std::to_string(acc_total) + "ms");
				acc_total = acc_sim = acc_sync = 0;
				acc_frames = 0;
			}
		}
	} else if (!in_game_mode_ || IsEditorInputActive()) {
		ProcessInput(delta_seconds);
	}
	if (in_game_mode_ && !render_gameplay) {
		// The editor window can stay focused while gameplay continues simulating.
		// Keep the gameplay-owned animation state current across that focus change.
		CaptureGameplayRenderSnapshot();
		AdvanceGameplayAnimationClocks();
	}
	UpdateGameplayFieldOfView();

	// Update animation playback (auto-play for AI NPCs)
	if (!rendering_editor_window_ || IsEditorInputActive()) {
		if (!render_gameplay) {
			UpdateAnimations(delta_seconds);
		}
		CheckMusicLoop();
	}

	// Per-frame position-drag velocity: the pad / Z slider accelerate while held in
	// a direction and keep moving when the cursor is pinned at the window edge.
	if (IsEditorInputActive() && mouse_state_.left_button_down_ &&
		prop_field_index_ >= 0 && selected_object_index_ >= 0 &&
	    !Utils::IsKeyBindingPressed(Config::Get().keyEnableCamera)) {
		ApplyPropPositionDrag();
	}

	if (IsEditorInputActive() && edit_mode_ && terrain_edit_enabled_ &&
		mouse_state_.left_button_down_) {
		EditorProcessClick();
	}

	if (render_gameplay) UpdateGameplayViewDefine();
	else UpdateViewDefine();
	if (render_gameplay) CaptureGameplayRenderSnapshot();
	if (IsEditorInputActive() &&
		(mouse_state_.prior_x_ != last_pick_x_ ||
		 mouse_state_.prior_y_ != last_pick_y_)) {
		bool camMode    = Utils::IsKeyBindingPressed(Config::Get().keyEnableCamera);
		bool overPanel  = prop_editor_open_ &&
		                  mouse_state_.prior_x_ < (PropPanel::kLeft + PropPanel::kWidth);
		if (camMode || overPanel) {
			hover_object_index_ = -1;
		} else {
			hover_object_index_ = PickObjectAtScreenPos(mouse_state_.prior_x_, mouse_state_.prior_y_);
			if (hover_object_index_ >= Renderer::kAttaPickBase) hover_object_index_ = -1; // ATTA hovered (clickable; promote on click)
		}
		last_pick_x_ = mouse_state_.prior_x_;
		last_pick_y_ = mouse_state_.prior_y_;
	}

	vert_flat_sky_layer_s * fsl_vb = renderer_.MapFlatSkyLayersVB();
	vert_pos_a_uv_s* terrain_vb = renderer_.MapTerrainVB();
	uint32_t* terrain_ib = renderer_.MapTerrainIB();
	render_chunk_s* render_chunks = renderer_.GetTerrainRenderChunckBuffer();

	update_params_s update_params = {
		.frame_ = frame_,
		.delta_seconds_ = delta_seconds,
		.view_define_ = &view_define_,
		.flat_sky_layer_vb_ = fsl_vb,
		.terrain_mod_options_ = terrain_mod_options_,
		.terrain_vb_ = terrain_vb,
		.terrain_ib_ = terrain_ib,
		.terrain_render_chunks_ = render_chunks
	};

	level_.Update(update_params);

	if (terrain_ib) {
		renderer_.UnmapTerrainIB();
	}

	if (terrain_vb) {
		renderer_.UnmapTerrainVB();
	}

	if (fsl_vb) {
		renderer_.UnmapFlatSkyLayersVB();
	}

	draw_params_.flat_sky_layer_is_visible_ = update_params.flat_sky_layer_is_visible_;
	draw_params_.num_terrain_render_chunk_ = update_params.num_terrain_render_chunk_;
	draw_params_.level_objects_ = &GetActiveRenderLevelObjects();
	draw_params_.selected_object_index_ = render_gameplay ? -1 : selected_object_index_;
	draw_params_.show_magic_obj_spheres_ = render_gameplay ? false : show_magic_obj_spheres_;
	// All AI with an active, playing clip are skinned-replaced simultaneously
	// (skinnedReplacementIndices must outlive renderer_.Draw() below, so it's a
	// local in this Frame() call, not a temporary).
	// Live AI poses are also part of the editor preview.  The old gameplay-only
	// gate left the editor's animation clocks advancing while the static meshes
	// continued to render, which made the log claim animations were playing even
	// though no animated pose was ever submitted in editor mode.
	std::unordered_set<int> skinnedReplacementIndices =
		GetSkinnedReplacementObjectIndices(render_gameplay);
	draw_params_.skip_static_draw_indices_ = &skinnedReplacementIndices;
	draw_params_.terrain_id_at_world_xy_ =
		[this](double x, double y) { return level_.GetTerrainNodeId(x, y); };


	float ground_z = 0.0f;
	bridge_.SetEnabled(show_hud_ && !render_gameplay);
	IGIBridge::PositionData data = bridge_.GetLatestData();
	const viewer_s& render_viewer = render_gameplay ? gameplay_viewer_ : viewer_;
	level_.GetTerrainZ(render_viewer.pos_.x, render_viewer.pos_.y, ground_z);

	int propAnimBoneHierarchy; std::vector<int> propAnimIds; int propAnimActiveId; bool propAnimIsPlaying;
	ComputePropAnimUiState(propAnimBoneHierarchy, propAnimIds, propAnimActiveId, propAnimIsPlaying);

	Renderer::task_tree_view_params_s task_tree_view = {
		.show_hud_ = render_gameplay ? false : show_hud_,
		.hud_overlay_visible_ = hud_overlay_visible_,
		.status_msg_ = status_message_,
		.pause_mode_ = igi::IsPauseMenuVisible(
			pause_mode_, IsGameplayInputFocused()),
		.pause_active_input_ = pause_active_input_,
		.pause_level_input_ = pause_level_input_,
		.pause_search_input_ = pause_search_input_,
		.pause_terrain_expanded_ = pause_terrain_expanded_,
		.terrain_draw_options_ = GetTerrainDrawOptions(),
		.show_debug_ = render_gameplay ? false : show_debug_,
		.show_help_ = render_gameplay ? false : show_help_,
		.edit_mode_ = render_gameplay ? false : edit_mode_,
		.terrain_edit_enabled_ = render_gameplay ? false : terrain_edit_enabled_,
		.terrain_mod_options_ = terrain_mod_options_,
		.selected_object_index_ = render_gameplay ? -1 : selected_object_index_,
		.hover_object_index_ = render_gameplay ? -1 : hover_object_index_,
		.hover_tree_index_ = render_gameplay ? -1 : hover_tree_index_,
		.mouse_x_ = mouse_state_.prior_x_,
		.mouse_y_ = mouse_state_.prior_y_,
		.tree_scroll_offset = tree_scroll_offset_,
		.tree_decl_expanded = tree_decl_expanded_,
		.level_objects_ = &GetActiveRenderLevelObjects(),
		.task_picker_open_ = render_gameplay ? false : task_picker_open_,
		.task_picker_selected_idx_ = task_picker_selected_idx_,
		.task_picker_scroll_offset_ = task_picker_scroll_offset_,
		.task_picker_search_ = task_picker_search_,
		.enable_camera_mode_ = render_gameplay ? false : Utils::IsKeyBindingPressed(Config::Get().keyEnableCamera),
		.prop_editor_open_     = render_gameplay ? false : prop_editor_open_,
		.prop_field_index_     = prop_field_index_,
		.prop_text_edit_field_ = prop_text_edit_field_,
		.prop_edit_obj_index_  = prop_edit_obj_index_,
		.prop_drag_obj_index_  = prop_drag_obj_index_,
		.prop_text_buf_        = prop_text_buf_,
		.prop_text_caret_      = prop_text_caret_,
		.prop_text_sel_anchor_ = prop_text_sel_anchor_,
		.prop_text_sel_focus_  = prop_text_sel_focus_,
		.prop_panel_scroll_    = prop_panel_scroll_,
		.find_open_            = find_open_,
		.find_query_           = find_query_,
		.find_result_idx_      = find_result_idx_,
		.selected_obj_is_ai    = (selected_object_index_ >= 0 &&
			selected_object_index_ < (int)level_.GetLevelObjects().GetObjects().size() &&
			(ai_model_ids_.count(level_.GetLevelObjects().GetObjects()[selected_object_index_].modelId) > 0 ||
			 [&]() {
				const std::string& t = level_.GetLevelObjects().GetObjects()[selected_object_index_].type;
				return t == "HumanSoldier" || t == "HumanSoldierFemale" ||
				       t == "HumanPlayer" || t == "HumanSoldierRPG" || t == "HumanAI";
			 }())),
		.in_game_mode_         = render_gameplay,
		.noclip_mode_          = noclip_mode_,
		.player_health_        = render_snapshot.player_health,
		.player_maximum_health_ = render_snapshot.player_maximum_health,
		.player_armor_         = render_snapshot.player_armor,
		.player_maximum_armor_ = render_snapshot.player_maximum_armor,
		.active_weapon_name_   = render_snapshot.active_weapon_name,
		.clip_ammo_            = render_snapshot.clip_ammo,
		.clip_capacity_        = render_snapshot.clip_capacity,
		.reserve_ammo_         = render_snapshot.reserve_ammo,
		.map_computer_open_    = render_snapshot.map_computer_open,
		.map_computer_objectives_ = render_snapshot.map_computer_objectives,
		.objective_text_       = render_snapshot.objective_text,
		.mission_timer_remaining_ticks_ = render_snapshot.mission_timer_remaining_ticks,
		.mission_status_messages_ = render_snapshot.mission_status_messages,
		.help_scroll_offset_   = help_scroll_offset_,
		.help_entries_         = &help_entries_,
		.show_task_type_       = show_task_type_,
		.find_mode_            = (int)find_mode_,
		.file_dialog_mode_     = (int)file_dialog_mode_,
		.file_dialog_path_     = file_dialog_path_,
		.file_dialog_caret_    = file_dialog_caret_,
		.ac_task_picker_open_  = ac_task_picker_open_,
		.ac_task_selected_idx_ = ac_task_selected_idx_,
		.ac_task_scroll_offset_= ac_task_scroll_offset_,
		.ac_task_filter_       = ac_task_filter_,
		.ac_task_items_        = &ac_task_items_,
		.model_picker_open_    = model_picker_open_,
		.model_picker_selected_= model_picker_selected_,
		.model_picker_scroll_  = model_picker_scroll_,
		.model_picker_filter_  = model_picker_filter_,
		.model_entries_        = &level_model_entries_,
		.ai_script_path_        = ai_script_path_,
		.ai_script_text_        = ai_script_text_,
		.ai_script_dirty_       = ai_script_dirty_,
		.ai_script_vscroll_     = ai_script_vscroll_,
		.ai_script_path_hscroll_= ai_script_path_hscroll_,
		.terrain_brush_          = edit_brush_,
		.terrain_brush_radius_   = edit_brush_radius_,
		.terrain_brush_strength_ = edit_brush_strength_,
		.auto_save_enabled_        = auto_save_enabled_,
		.auto_save_interval_seconds_ = auto_save_interval_seconds_,
		.logging_enabled_ = Config::Get().enableLogging,
		.log_level_threshold_ = Config::Get().logLevelThreshold,
		.music_on_ = music_playing_,
		.anim_status_  = BuildAnimStatusString(),
		.anim_playing_ = !animPlaybacks_.empty(),
		.anim_debug_visible_ = show_anim_debug_,
		.prop_anim_bone_hierarchy_ = propAnimBoneHierarchy,
		.prop_anim_ids_ = propAnimIds,
		.prop_anim_active_id_ = propAnimActiveId,
		.prop_anim_is_playing_ = propAnimIsPlaying,
		.flash_effect_strength_ = render_gameplay
			? render_snapshot.flash_effect_strength
			: 0.0f,
		.muzzle_flash_strength_ = render_gameplay
			? render_snapshot.muzzle_flash_strength
			: 0.0f,
		.player_damage_effect_strength_ = render_gameplay
			? render_snapshot.player_damage_effect_strength
			: 0.0f,
	};

	renderer_.Draw(draw_params_, task_tree_view);
	DrawGameplayProjectiles();
	DrawGameplayExplosions();
	DrawGameplayGuardMuzzleFlashes();
	DrawGameplayPlayerWeapon();

    // Find the "right hand" bone index in modelId's parsed bone list (REIH+MANB
    // names), cached per modelId. This is the same index space EvaluateWorld's
    // worldTransforms and the rest-pose bone list use (both come from the same
    // shared character rig), so one lookup serves both the animating and static
    // weapon-attachment paths below.
    auto findHandBoneIndex = [this](const std::string& modelId, const ParsedGeometry* geo) -> int {
        auto cached = handBoneIndexCache_.find(modelId);
        if (cached != handBoneIndexCache_.end()) return cached->second;
        int idx = -1;
        if (geo) {
            for (size_t i = 0; i < geo->bones.size(); ++i) {
                if (geo->bones[i].name == "right hand") { idx = (int)i; break; }
            }
        }
        handBoneIndexCache_[modelId] = idx;
        return idx;
    };

    // Weapon meshes are authored with the barrel along native +Y. Attached at the
    // hand bone it comes out aligned with the forearm (appears vertical). Correction,
    // applied in the weapon's own local frame (right-multiplied onto the hand matrix
    // so it still follows the hand as the arm animates):
    //   * rotate 90Â° about X  -> swings the barrel from vertical to horizontal,
    //   * rotate 180Â° about Z  -> single horizontal flip so the barrel points the
    //     correct way, then
    //   * roll 180Â° about the barrel's OWN (post-correction) direction -> flips the
    //     weapon right-side-up (was upside down) WITHOUT changing the barrel's aim,
    //     since rotating about the barrel axis leaves that axis fixed. Computed from
    //     the actual barrel direction so it's correct regardless of the gun's frame.
    const glm::mat4 kWeaponBase =
        glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f)) *
        glm::rotate(glm::mat4(1.0f), glm::radians(180.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::vec3 kBarrelDir = glm::normalize(glm::vec3(kWeaponBase * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f)));
    const glm::mat4 kWeaponHandCorrection =
        glm::rotate(glm::mat4(1.0f), glm::radians(180.0f), kBarrelDir) * kWeaponBase;

    // Draw the live skinned mesh for every AI with an active, playing clip â€”
    // all of them animate and render in parallel, not just the selected object.
    // This MUST run whenever a clip is playing (not gated by F10) â€” the static
    // mesh is already skipped via skip_static_draw_indices_ above, so gating
    // this would leave those objects invisible. The bone wireframe stays scoped
    // to the selected object only (avoids clutter) and gated by 'B'.
    {
        auto& active_playbacks = render_gameplay
            ? gameplayAnimPlaybacks_ : animPlaybacks_;
        auto& objs = GetActiveRenderLevelObjects().GetObjects();
        for (int idx : skinnedReplacementIndices) {
            if (idx < 0 || idx >= (int)objs.size()) continue;
            auto playback_it = active_playbacks.find(idx);
            if (playback_it == active_playbacks.end()) continue;
            auto& pb = playback_it->second;
            std::vector<glm::mat4> worldTransforms;
            animRegistry_.EvaluateWorld(pb.clip, pb.currentTimeMs, worldTransforms);
            if (worldTransforms.empty()) continue;

            const auto& obj = objs[idx];
            // The BEF exporter stores the skeleton relative to its animation
            // root, while the MEF vertex stream is baked against the model's
            // native root pivot. Align the evaluated pose to that MEF bind root
            // before CPU skinning; otherwise the AI body is rendered at the
            // wrong local height and appears frozen/missing in the scene.
            const ParsedGeometry* skinGeo =
                renderer_.GetOrLoadSkinGeometry(obj.modelId, obj.isBuilding);
            if (skinGeo != nullptr && !skinGeo->bones.empty()) {
                std::vector<glm::vec3> mefBindWorld =
                    ComputeBoneWorldPositionsPublic(skinGeo->bones);
                for (glm::vec3& position : mefBindWorld) {
                    position *= kMefNativeScale;
                }
                std::vector<glm::mat4> animationBindWorld;
                animRegistry_.EvaluateWorld(pb.clip, 0.0f, animationBindWorld);
                AlignAnimationWorldToMefBind(
                    animationBindWorld, mefBindWorld, worldTransforms);
            }
            glm::mat4 objMat(1.0f);
            objMat = glm::translate(objMat, glm::vec3((float)obj.pos.x, (float)obj.pos.y, (float)obj.pos.z));
            objMat = glm::rotate(objMat, (float)obj.rot.z, glm::vec3(0, 0, 1));
            objMat = glm::rotate(objMat, (float)obj.rot.x, glm::vec3(1, 0, 0));
            objMat = glm::rotate(objMat, (float)obj.rot.y, glm::vec3(0, 1, 0));
            objMat = glm::scale(objMat, glm::vec3(40.96f * obj.scale));
            renderer_.DrawSkinnedMesh(obj.modelId, obj.isBuilding, worldTransforms, objMat);

            if (show_anim_skeleton_ && idx == selected_object_index_) {
                // boneParents is indexed by bone ID (same indexing EvaluateWorld uses for
                // worldTransforms), so DrawAnimSkeleton can connect each bone to its real
                // parent instead of assuming a flat chain of consecutive array indices.
                std::vector<int> boneParents(worldTransforms.size(), -1);
                for (const auto& b : pb.clip->bones) {
                    if (b.index >= 0 && (size_t)b.index < boneParents.size())
                        boneParents[b.index] = b.parent;
                }
                renderer_.DrawAnimSkeleton(worldTransforms, boneParents, objMat);
            }

            if (!obj.weaponModelId.empty()) {
                const ParsedGeometry* geo = renderer_.GetOrLoadSkinGeometry(obj.modelId, obj.isBuilding);
                int handIdx = findHandBoneIndex(obj.modelId, geo);
                if (handIdx >= 0 && (size_t)handIdx < worldTransforms.size()) {
                    glm::mat4 handWorldMat = objMat * worldTransforms[handIdx] * kWeaponHandCorrection;
                    renderer_.DrawAttachedMesh(obj.weaponModelId, false, handWorldMat);
                }
            }
        }
    }

    // Static/paused AI (not currently in skinnedReplacementIndices) still hold
    // their weapon, positioned at the hand bone's REST pose instead of an
    // animated transform.
    {
        auto& objs = GetActiveRenderLevelObjects().GetObjects();
        for (int idx = 0; idx < (int)objs.size(); ++idx) {
            const auto& obj = objs[idx];
            if (obj.weaponModelId.empty() || obj.deleted) continue;
            if (skinnedReplacementIndices.count(idx)) continue; // already drawn above (animated)

            const ParsedGeometry* geo = renderer_.GetOrLoadSkinGeometry(obj.modelId, obj.isBuilding);
            int handIdx = findHandBoneIndex(obj.modelId, geo);
            if (handIdx < 0 || !geo || (size_t)handIdx >= geo->bones.size()) continue;

            std::vector<glm::vec3> restPositions = ComputeBoneWorldPositionsPublic(geo->bones);
            if ((size_t)handIdx >= restPositions.size()) continue;

            glm::mat4 objMat(1.0f);
            objMat = glm::translate(objMat, glm::vec3((float)obj.pos.x, (float)obj.pos.y, (float)obj.pos.z));
            objMat = glm::rotate(objMat, (float)obj.rot.z, glm::vec3(0, 0, 1));
            objMat = glm::rotate(objMat, (float)obj.rot.x, glm::vec3(1, 0, 0));
            objMat = glm::rotate(objMat, (float)obj.rot.y, glm::vec3(0, 1, 0));
            objMat = glm::scale(objMat, glm::vec3(40.96f * obj.scale));

            glm::mat4 handLocalMat = glm::translate(glm::mat4(1.0f), restPositions[handIdx] * kMefNativeScale);
            renderer_.DrawAttachedMesh(obj.weaponModelId, false, objMat * handLocalMat * kWeaponHandCorrection);
        }
    }

	DrawCustomCursor();
	glutSwapBuffers();
}

void App::UpdateGameplayFieldOfView() {
    constexpr float default_field_of_view_degrees = FOVY_IN_DEGREE;
    constexpr float zoomed_field_of_view_degrees = 40.0f;
    const igi::RuntimeCutSceneCamera& cut_scene_camera =
        gameplay_host_.GetWorld().GetActiveCutSceneCamera();
    const bool use_cut_scene_field_of_view = IsGameplayRenderTarget() &&
        cut_scene_camera.active &&
        std::isfinite(cut_scene_camera.field_of_view_y_radians) &&
        cut_scene_camera.field_of_view_y_radians > 0.0f;
    const bool map_camera_active = IsGameplayRenderTarget() &&
        gameplay_map_computer_camera_.IsRunning();
    const bool zoom_active = IsGameplayRenderTarget() &&
        gameplay_host_.GetWorld().IsZoomActive();
    const float desired_field_of_view = map_camera_active
        ? gameplay_map_computer_camera_.GetFieldOfView()
        : use_cut_scene_field_of_view
        ? cut_scene_camera.field_of_view_y_radians
        : glm::radians(
            zoom_active
                ? zoomed_field_of_view_degrees
                : default_field_of_view_degrees);
    if (std::abs(view_define_.fovy_ - desired_field_of_view) <= 0.0001f) {
        return;
    }

    view_define_.fovy_ = desired_field_of_view;
    const float viewport_aspect = static_cast<float>(
        window_state_.viewport_width_) /
        static_cast<float>(std::max(1, window_state_.viewport_height_));
    const float half_vertical_fov_tangent = std::tan(view_define_.fovy_ * 0.5f);
    view_define_.fovx_ = std::atan(
        half_vertical_fov_tangent * viewport_aspect) * 2.0f;
    view_define_.tan_half_fovx_ = std::tan(view_define_.fovx_ * 0.5f);
    view_define_.tan_half_fovy_ = half_vertical_fov_tangent;
    view_define_.half_viewport_width_div_tan_half_fovx_ =
        static_cast<float>(window_state_.viewport_width_) * 0.5f /
        view_define_.tan_half_fovx_;
    view_define_.half_viewport_height_div_tan_half_fovy_ =
        static_cast<float>(window_state_.viewport_height_) * 0.5f /
        view_define_.tan_half_fovy_;
}

void App::ApplyRuntimeCutSceneCamera() {
    if (!IsGameplayRenderTarget()) {
        return;
    }

    const igi::RuntimeCutSceneCamera& cut_scene_camera =
        gameplay_host_.GetWorld().GetActiveCutSceneCamera();
    if (!cut_scene_camera.active) {
        const igi::PlayerController& player =
            gameplay_host_.GetWorld().GetPlayer();
        gameplay_viewer_.pos_ = player.GetEyePosition();
        gameplay_viewer_.yaw_ = player.GetYaw();
        gameplay_viewer_.pitch_ = player.GetPitch();
        UpdateGameplayViewerVectors();
        return;
    }

    gameplay_viewer_.pos_ = cut_scene_camera.position;
    gameplay_viewer_.forward_ = cut_scene_camera.forward;
    gameplay_viewer_.right_ = cut_scene_camera.right;
    gameplay_viewer_.up_ = cut_scene_camera.up;
}

void App::UpdateGameplayMapComputerCamera(float delta_seconds) {
    if (!IsGameplayRenderTarget()) {
        return;
    }

    // The authored camera source does not expose a separate C++ map-camera
    // task. Keep the presentation bridge deterministic and renderer-free: the
    // The map computer owns a separate overhead view. Its center and discrete
    // field-of-view levels are fixed-step runtime state so drag/zoom input is
    // deterministic and the camera bridge only presents that state.
    constexpr float map_vantage_height_units =
        900.0f * igi::PlayerController::WORLD_METER;
    igi::RuntimeWorld& runtime_world = gameplay_host_.GetWorld();
    const igi::PlayerController& player = runtime_world.GetPlayer();
    const float player_field_of_view = glm::radians(
        gameplay_host_.GetWorld().IsZoomActive() ? 40.0f : FOVY_IN_DEGREE);
    const igi::RuntimeMapComputerPose live_eye{
        player.GetEyePosition(),
        glm::radians(player.GetYaw()),
        glm::radians(player.GetPitch())};
    const glm::vec3 player_position = player.GetPosition();
    const glm::vec3 map_center = runtime_world.GetMapComputerCenter();
    const igi::RuntimeMapComputerPose live_vantage{
        glm::vec3(map_center.x, map_center.y,
            player_position.z + map_vantage_height_units),
        0.0f,
        igi::RuntimeMapComputerCamera::kMapPitchRadians};
    const float map_field_of_view = runtime_world.GetMapComputerFieldOfView();
    const bool map_computer_open =
        gameplay_host_.GetWorld().IsMapComputerOpen();

    if (map_computer_open && !gameplay_map_computer_open_) {
        gameplay_map_computer_camera_.BeginOpen(
            live_eye,
            player_field_of_view,
            live_vantage,
            map_field_of_view);
        gameplay_map_computer_open_ = true;
    } else if (!map_computer_open && gameplay_map_computer_open_) {
        if (gameplay_map_computer_camera_.CanClose()) {
            gameplay_map_computer_camera_.BeginClose(
                live_vantage,
                map_field_of_view,
                live_eye,
                player_field_of_view);
        }
        gameplay_map_computer_open_ = false;
    }

    gameplay_map_computer_camera_.Update(
        delta_seconds,
        live_eye,
        live_vantage,
        map_field_of_view);
    if (!gameplay_map_computer_camera_.IsRunning()) {
        return;
    }

    const igi::RuntimeMapComputerPose& camera_pose =
        gameplay_map_computer_camera_.GetPose();
    gameplay_viewer_.pos_ = camera_pose.position;
    gameplay_viewer_.yaw_ = glm::degrees(camera_pose.yaw);
    gameplay_viewer_.pitch_ = glm::degrees(camera_pose.pitch);
    gameplay_viewer_.roll_ = 0.0f;
    UpdateGameplayViewerVectors();
}

void App::CaptureGameplayRenderSnapshot() {
    if (!IsGameplayRenderTarget()) {
        return;
    }

    igi::RuntimeRenderCamera camera;
    camera.position = gameplay_viewer_.pos_;
    camera.forward = gameplay_viewer_.forward_;
    camera.right = gameplay_viewer_.right_;
    camera.up = gameplay_viewer_.up_;
    camera.field_of_view_y_radians = view_define_.fovy_;
    camera.viewport_width = gameplay_viewport_width_;
    camera.viewport_height = gameplay_viewport_height_;
    gameplay_host_.Render(camera);
}

void App::DrawGameplayPlayerWeapon() {
	if (!IsGameplayRenderTarget()) return;
	// The first-person weapon mesh is part of the deferred HUD presentation:
	// keep it hidden unless the overlay is explicitly shown with Alt+H.
	if (!hud_overlay_visible_) return;
	if (gameplay_host_.GetWorld().GetActiveCutSceneCamera().active ||
		gameplay_host_.GetWorld().IsMapComputerOpen() ||
		gameplay_map_computer_camera_.IsRunning()) {
		return;
	}

	const igi::RuntimeRenderSnapshot& render_snapshot =
		gameplay_host_.GetRenderSnapshot();
	if (render_snapshot.active_weapon_model_id.empty() ||
		!render_snapshot.player_alive) {
		return;
	}

	const glm::vec3 forward = glm::normalize(render_snapshot.camera.forward);
	const glm::vec3 right = glm::normalize(render_snapshot.camera.right);
	const glm::vec3 up = glm::normalize(render_snapshot.camera.up);
	const glm::vec3 weapon_position = render_snapshot.camera.position +
		forward * (0.75f * igi::PlayerController::WORLD_METER) +
		right * (0.24f * igi::PlayerController::WORLD_METER) -
		up * (0.24f * igi::PlayerController::WORLD_METER);

	// Vanilla weapon meshes use +Y as the barrel axis. Build the camera basis
	// first, then apply the fixed-step HumanViewSway angles in rig-local space.
	// This keeps weapon transition timing in the simulation while leaving mesh
	// loading and OpenGL state in the presentation layer.
	glm::mat4 camera_basis(1.0f);
	camera_basis[0] = glm::vec4(right, 0.0f);
	camera_basis[1] = glm::vec4(forward, 0.0f);
	camera_basis[2] = glm::vec4(up, 0.0f);
	glm::mat4 local_view_sway(1.0f);
	local_view_sway = glm::rotate(
		local_view_sway,
		render_snapshot.weapon_view_pitch_radians,
		glm::vec3(1.0f, 0.0f, 0.0f));
	local_view_sway = glm::rotate(
		local_view_sway,
		render_snapshot.weapon_view_yaw_radians +
			render_snapshot.weapon_recoil_yaw_radians,
		glm::vec3(0.0f, 0.0f, 1.0f));
	local_view_sway = glm::rotate(
		local_view_sway,
		render_snapshot.weapon_recoil_pitch_radians,
		glm::vec3(1.0f, 0.0f, 0.0f));
	glm::mat4 weapon_model = glm::translate(glm::mat4(1.0f), weapon_position) *
		camera_basis * local_view_sway;
	weapon_model = glm::scale(
		weapon_model,
		glm::vec3(40.96f * 0.75f));
	renderer_.DrawAttachedMesh(
		render_snapshot.active_weapon_model_id,
		false,
		weapon_model);
}

void App::DrawGameplayProjectiles() {
	if (!IsGameplayRenderTarget()) return;

	const igi::RuntimeRenderSnapshot& render_snapshot =
		gameplay_host_.GetRenderSnapshot();
	for (const igi::RuntimeProjectileRenderState& projectile :
		render_snapshot.projectiles) {
		const char* model_id = nullptr;
		switch (projectile.type) {
			case igi::ProjectileType::FragGrenade: model_id = "135_01_1"; break;
			case igi::ProjectileType::Flashbang: model_id = "137_01_1"; break;
			case igi::ProjectileType::ProximityMine: model_id = "136_01_1"; break;
			case igi::ProjectileType::Rocket: model_id = "110_01_1"; break;
			case igi::ProjectileType::None: break;
		}
		if (model_id == nullptr) continue;

		glm::mat4 projectile_model = glm::translate(
			glm::mat4(1.0f),
			projectile.position);
		projectile_model = glm::rotate(
			projectile_model,
			glm::radians(static_cast<float>(projectile.tumble_ticks * 17U)),
			glm::vec3(0.0f, 0.0f, 1.0f));
		projectile_model = glm::scale(
			projectile_model,
			glm::vec3(40.96f * (projectile.type == igi::ProjectileType::Rocket
				? 0.65f
				: 0.35f)));
		renderer_.DrawAttachedMesh(model_id, false, projectile_model);
	}
}

void App::DrawGameplayExplosions() {
	if (!IsGameplayRenderTarget()) return;

	const igi::RuntimeRenderSnapshot& render_snapshot =
		gameplay_host_.GetRenderSnapshot();
	for (const igi::RuntimeExplosionRenderState& explosion :
		render_snapshot.explosions) {
		if (explosion.is_flashbang) continue;

		const float age_fraction = 1.0f - static_cast<float>(
			explosion.remaining_ticks) /
			static_cast<float>(
				igi::RuntimeExplosionRenderState::DISPLAY_DURATION_TICKS);
		const float radius_meters = std::max(
			0.75f,
			explosion.radius_units /
			igi::PlayerController::WORLD_METER * 0.35f);
		const float expansion = 0.85f + age_fraction * 0.35f;
		glm::mat4 explosion_model = glm::translate(
			glm::mat4(1.0f),
			explosion.position);
		explosion_model = glm::scale(
			explosion_model,
			glm::vec3(40.96f * radius_meters * expansion));
		renderer_.DrawAttachedMesh("1009_01_1", false, explosion_model);
	}
}

void App::DrawGameplayGuardMuzzleFlashes() {
	if (!IsGameplayRenderTarget()) return;

	const igi::RuntimeRenderSnapshot& render_snapshot =
		gameplay_host_.GetRenderSnapshot();
	for (const igi::RuntimeGuardMuzzleFlashState& flash :
		render_snapshot.guard_muzzle_flashes) {
		const float radius_meters = std::max(
			0.04f,
			0.12f * std::clamp(flash.strength, 0.0f, 1.0f));
		glm::mat4 flash_model = glm::translate(
			glm::mat4(1.0f),
			flash.position);
		flash_model = glm::scale(
			flash_model,
			glm::vec3(40.96f * radius_meters));
		renderer_.DrawAttachedMesh("1009_01_1", false, flash_model);
	}
}

void App::ToggleShowHUD() {
	show_hud_ = true;
}

bool App::GetShowHUD() const {
	return show_hud_;
}

void App::SetShowHUD(bool show) {
	show_hud_ = show;
}

void App::ToggleEditMode() {
	// Preserve the historical API used by editor commands while making the
	// gameplay session the single owner of the mode transition.
	ToggleGamePlayMode();
}

bool App::GetEditMode() const {
	return !in_game_mode_;
}

void App::SetEditMode(bool enabled) {
	const bool currently_in_editor = !in_game_mode_;
	if (enabled == currently_in_editor) {
		return;
	}
	ToggleGamePlayMode();
}

void App::SetTerrainEditEnabled(bool enabled) {
	if (enabled && in_game_mode_) {
		// Terrain queries are part of the live runtime collision contract. Keep
		// authoring terrain immutable until gameplay is closed or explicitly
		// rebuilt, rather than letting a brush edit alter an active session.
		terrain_edit_enabled_ = false;
		status_message_ = "Terrain editing is disabled during gameplay; close gameplay before changing terrain";
		return;
	}
	terrain_edit_enabled_ = enabled;
	if (enabled) {
		static const char* kNames[] = {"Raise","Lower","Soften","Flatten"};
		int b = (edit_brush_ >= 0 && edit_brush_ < 4) ? edit_brush_ : 0;
		status_message_ = std::string("Terrain edit ON | Brush: ") + kNames[b] +
			" | Radius: " + std::to_string((long)edit_brush_radius_) +
			" | Strength: " + std::to_string((long)edit_brush_strength_);
	}
	UpdateCursorMode(); // Force cursor update instantly
	glutPostRedisplay(); // Force instant UI refresh
}

bool App::GetTerrainEditEnabled() const {
	return terrain_edit_enabled_;
}

void App::TogglePauseMenu() {
	pause_mode_ = !pause_mode_;
	// Keep the application-level pause menu and the fixed-step scheduler on the
	// same state boundary. Frame() intentionally skips simulation while this menu
	// is visible, but the scheduler must also reject the wall-clock gap so resume
	// cannot replay stale input through its catch-up budget.
	if (in_game_mode_) {
		gameplay_host_.SetPaused(pause_mode_);
	}
	// cursor_visible_ stays TRUE always â€” camera lock is handled dynamically in Input_OnMotion.
	// Hiding the cursor permanently caused the "mouse stuck" bug after resuming.
	window_state_.cursor_visible_ = true;
	if (pause_mode_) {
		// Opening pause menu: seed level spinner with current level
		int cur = level_.GetLevelNo();
		if (cur > 0) pause_level_input_ = std::to_string(cur);
		pause_active_input_ = -1;
		// The menu is mouse-driven: the pointer must be visible. Gameplay
		// motion handlers return early while paused, so nothing else would
		// restore the cursor here — an invisible pointer made the menu
		// unusable in play mode.
		glutSetCursor(GLUT_CURSOR_INHERIT);
	} else {
		// Closing pause menu: reset mouse state so no stale drag occurs
		input_.mouse_delta_x_ = 0;
		input_.mouse_delta_y_ = 0;
		mouse_state_.left_button_down_ = false;
		skip_input_on_motion_once_ = false;
		if (in_game_mode_) {
			// Resume first-person look: hide the OS cursor and re-center it.
			glutSetCursor(GLUT_CURSOR_NONE);
			mouse_state_.prior_x_ = gameplay_viewport_width_ >> 1;
			mouse_state_.prior_y_ = gameplay_viewport_height_ >> 1;
			glutWarpPointer(mouse_state_.prior_x_, mouse_state_.prior_y_);
		} else {
			glutSetCursor(GLUT_CURSOR_INHERIT);
		}
	}
}

bool App::GetPauseMode() const {
	return pause_mode_;
}

LevelObjects& App::GetActiveRenderLevelObjects() {
	return IsGameplayRenderTarget() && runtime_level_objects_.has_value()
		? runtime_level_objects_.value()
		: level_.GetLevelObjects();
}

void App::ToggleGamePlayMode() {
	const bool entering_gameplay = !in_game_mode_;
	if (entering_gameplay) {
		if (level_.GetLevelNo() <= 0) {
			status_message_ = "Cannot enter Game Play: load a level first";
			return;
		}

		igi::EditorSnapshot snap;
		snap.camera_pos = viewer_.pos_;
		snap.camera_yaw = viewer_.yaw_;
		snap.camera_pitch = viewer_.pitch_;
		snap.was_edit_mode = edit_mode_;
		snap.was_noclip_mode = noclip_mode_;
		snap.was_hud_visible = show_hud_;
		snap.selected_object_id = selected_object_index_;
		gameplay_editor_snapshot_ = snap;
		runtime_level_objects_ = level_.GetLevelObjects();
		runtime_initial_deleted_flags_.clear();
		runtime_initial_deleted_flags_.reserve(
			runtime_level_objects_->GetObjects().size());
		runtime_conditionally_hidden_flags_.assign(
			runtime_level_objects_->GetObjects().size(),
			0U);
		runtime_guard_generator_hidden_flags_.assign(
			runtime_level_objects_->GetObjects().size(),
			0U);
		for (const LevelObject& object : runtime_level_objects_->GetObjects()) {
			runtime_initial_deleted_flags_.push_back(object.deleted ? 1U : 0U);
		}
		noclip_mode_ = false;

		// 1. Read config.qvm profile for controls, sensitivity, sound volume
		igi::ProfileConfig profile = igi::ConfigQvmLoader::GetActiveProfile();
		gameplay_host_.GetInputRouter().SetProfile(profile);

		// 2. Read humanplayer.qvm tuning for player speed, jump impulse, health
		igi::HumanPlayerTuning tuning = igi::HumanPlayerConfigLoader::Load();
		const igi::PlayerController::Tuning player_tuning = tuning.ToControllerTuning();
		gameplay_host_.GetWorld().SetPlayerTuning(player_tuning);
		std::vector<uint32_t> configured_weapon_cycle;
		for (const int weapon_id : tuning.weapon_cycle) {
			if (weapon_id >= 0) {
				configured_weapon_cycle.push_back(static_cast<uint32_t>(weapon_id));
			}
		}
		gameplay_host_.GetWorld().SetPlayerWeaponCycle(configured_weapon_cycle);

		// 3. Spawn resolution: the editor camera is the primary spawn point so
		//    playtesting starts exactly where the level was being inspected.
		//    Authored HumanPlayer and level-start positions are fallbacks for
		//    an untouched/default camera.
		glm::vec3 spawn_pos(0.0f);
		float spawn_yaw = 0.0f;
		float spawn_pitch = 0.0f;
		bool found_spawn = false;
		bool spawned_from_camera = false;

		std::vector<igi::RuntimeSpawnCandidate> authored_spawn_candidates;
		const auto& spawn_objects = runtime_level_objects_->GetObjects();
		authored_spawn_candidates.reserve(spawn_objects.size());
		for (const LevelObject& object : spawn_objects) {
			if (object.deleted || object.type != "HumanPlayer") {
				continue;
			}

			int task_id = -1;
			const char* task_id_begin = object.taskId.data();
			const char* task_id_end = task_id_begin + object.taskId.size();
			const auto parse_result = std::from_chars(
				task_id_begin,
				task_id_end,
				task_id);
			if (parse_result.ec != std::errc() || parse_result.ptr != task_id_end) {
				task_id = -1;
			}

			float authored_yaw = glm::degrees(static_cast<float>(object.rot.z));
			while (authored_yaw < 0.0f) authored_yaw += 360.0f;
			while (authored_yaw >= 360.0f) authored_yaw -= 360.0f;
			authored_spawn_candidates.push_back({
				true,
				task_id,
				{
					glm::vec3(object.pos),
					authored_yaw,
					0.0f,
				},
			});
		}

		{
			const float kSpawnSnapEps = 1.0f;
			if (glm::length(viewer_.pos_) > kSpawnSnapEps ||
				fabsf(viewer_.yaw_) > kSpawnSnapEps) {
				spawn_pos = viewer_.pos_;
				spawn_yaw = viewer_.yaw_;
				spawn_pitch = viewer_.pitch_;
				found_spawn = true;
				spawned_from_camera = true;
				Logger::Get().Log(LogLevel::INFO, "[App] Spawn at editor camera: pos=(" +
					std::to_string(spawn_pos.x) + "," + std::to_string(spawn_pos.y) +
					"," + std::to_string(spawn_pos.z) + ") yaw=" + std::to_string(spawn_yaw));
			}
		}

		if (!found_spawn) {
			if (const std::optional<igi::RuntimeSpawnPoint> authored_spawn =
					igi::SelectAuthoredPlayerSpawn(authored_spawn_candidates)) {
				spawn_pos = authored_spawn->position;
				spawn_yaw = authored_spawn->yaw;
				spawn_pitch = authored_spawn->pitch;
				found_spawn = true;
				Logger::Get().Log(LogLevel::INFO, "[App] Authored HumanPlayer spawn: pos=(" +
					std::to_string(spawn_pos.x) + "," + std::to_string(spawn_pos.y) +
					"," + std::to_string(spawn_pos.z) + ") yaw=" + std::to_string(spawn_yaw));
			}
		}

		if (!found_spawn) {
			glm::vec3 l_start = level_.GetStartPos();
			if (l_start.z != 175000000.0f && (l_start.x != 0.0f || l_start.y != 0.0f)) {
				spawn_pos = l_start;
				spawn_yaw = glm::degrees(level_.GetStartYaw());
				while (spawn_yaw < 0.0f) spawn_yaw += 360.0f;
				while (spawn_yaw >= 360.0f) spawn_yaw -= 360.0f;
				found_spawn = true;
			}
		}

		// The editor camera stores the eye position. Runtime physics stores the
		// player's body base, so remove the verified standing eye height before
		// handing this spawn to the fixed-step controller.
		if (found_spawn && spawned_from_camera) {
			spawn_pos.z -= player_tuning.standing_eye_height_units;
		}
		gameplay_spawn_position_ = spawn_pos;
		gameplay_spawn_yaw_ = spawn_yaw;
		gameplay_spawn_pitch_ = spawn_pitch;

		if (!gameplay_host_.OpenGameplay(snap)) {
			gameplay_editor_snapshot_.reset();
			runtime_level_objects_.reset();
			runtime_initial_deleted_flags_.clear();
			runtime_conditionally_hidden_flags_.clear();
			runtime_guard_generator_hidden_flags_.clear();
			status_message_ = "Cannot enter Game Play: runtime session is already active";
			return;
		}
		if (!gameplay_host_.HasGameplayWindow()) {
			igi::EditorSnapshot ignored_snapshot;
			gameplay_host_.CloseGameplay(ignored_snapshot);
			gameplay_editor_snapshot_.reset();
			runtime_level_objects_.reset();
			runtime_initial_deleted_flags_.clear();
			runtime_conditionally_hidden_flags_.clear();
			runtime_guard_generator_hidden_flags_.clear();
			status_message_ = "Cannot enter Game Play: gameplay window is unavailable";
			return;
		}
		in_game_mode_ = true;
		gameplay_host_.FocusGameplayWindow();
		glutSetCursor(GLUT_CURSOR_NONE);
		mouse_state_.prior_x_ = gameplay_viewport_width_ >> 1;
		mouse_state_.prior_y_ = gameplay_viewport_height_ >> 1;
		glutWarpPointer(mouse_state_.prior_x_, mouse_state_.prior_y_);

		// Resolve authored gates before registering dynamic actors. Runtime
		// transforms and visibility stay in the gameplay object copy.
		SetupRuntimeMissionState();
		ApplyRuntimeConditionalContainerStates();
		SetupRuntimeDoors();

		// Register in-level enemies into the runtime AI system (patrol waypoints
		// come from each enemy's AIGraph). Runtime transforms stay in the render copy.
		SetupLevelAiGuards();
		gameplay_host_.GetWorld().RefreshAuthoredGuardGeneratorStates();
		ApplyRuntimeGuardGeneratorStates();
		SetupRuntimeLadders();
		SetupRuntimePlayerAnimation();
		gameplayAnimPlaybacks_.clear();
		lastGameplayAnimationTick_ = 0;
		SetupRuntimeInteractionState();

		// Initialize authored mission objectives for this specific level.
		InitializeGameplayMissionObjectives();
		int current_lvl = level_.GetLevelNo();
		if (current_lvl <= 0) current_lvl = 1;

		// Start authentic background music for the level
		PlayLevelMusic(current_lvl);

		gameplay_host_.GetWorld().GetPlayer().SetPosition(spawn_pos);
		gameplay_host_.GetWorld().GetPlayer().SetOrientation(spawn_yaw, spawn_pitch);
		ConfigureGameplayExtractionFallback(spawn_pos, spawn_yaw);
		gameplay_viewer_.pos_ = gameplay_host_.GetWorld().GetPlayer().GetEyePosition();
		gameplay_viewer_.yaw_ = spawn_yaw;
		gameplay_viewer_.pitch_ = spawn_pitch;
		gameplay_viewer_.roll_ = 0.0f;
		gameplay_map_computer_camera_.Reset();
		gameplay_map_computer_open_ = false;
		UpdateGameplayViewerVectors();

		// Disable all editor modes and editor tools
		edit_mode_ = false;
		terrain_edit_enabled_ = false;
		prop_editor_open_ = false;
		task_picker_open_ = false;
		ac_task_picker_open_ = false;
		model_picker_open_ = false;
		show_hud_ = false;
		selected_object_index_ = -1;
		hover_object_index_ = -1;
		status_message_ = "Game Mode Active (Profile: " + profile.name + "): WASD move, Mouse look/fire, Space jump, Right Ctrl crouch, C map, E activate, R reload, F8 editor/gameplay toggle, F5 apply/restart, F6 editor, F7 gameplay, ESC menu";
	} else {
		igi::EditorSnapshot snap;
		if (!gameplay_host_.CloseGameplay(snap)) {
			status_message_ = "Cannot leave Game Play: runtime session is not active";
			return;
		}
		gameplay_host_.HideGameplayWindow();
		RestoreEditorViewport();
		in_game_mode_ = false;
		gameplay_editor_snapshot_.reset();
		runtime_level_objects_.reset();
		runtime_initial_deleted_flags_.clear();
		runtime_conditionally_hidden_flags_.clear();
		runtime_guard_generator_hidden_flags_.clear();
		runtime_animation_request_serials_.clear();
		gameplayAnimPlaybacks_.clear();
		lastGameplayAnimationTick_ = 0;
		gameplay_map_computer_camera_.Reset();
		gameplay_map_computer_open_ = false;
		player_animation_driver_.ClearAnimationClips();
		viewer_.pos_ = snap.camera_pos;
		viewer_.yaw_ = snap.camera_yaw;
		viewer_.pitch_ = snap.camera_pitch;
		edit_mode_ = snap.was_edit_mode;
		selected_object_index_ = snap.selected_object_id;
		noclip_mode_ = snap.was_noclip_mode;
		show_hud_ = snap.was_hud_visible;
		input_.keys_ = 0;
		input_.mouse_delta_x_ = 0.0f;
		input_.mouse_delta_y_ = 0.0f;
		skip_input_on_motion_once_ = true;
		glutSetCursor(GLUT_CURSOR_INHERIT);
		UpdateViewerVectors();
		status_message_ = "Editor Mode Restored";
	}
}

void App::ApplyAndRestartGameplay() {
	if (!in_game_mode_ || !gameplay_host_.IsGameplayActive() ||
		!gameplay_editor_snapshot_.has_value()) {
		status_message_ = "Cannot apply gameplay: no active runtime session";
		return;
	}
	if (!IsGameplayInputFocused()) {
		CaptureEditorSnapshotForGameplayApply();
	}

	// Rebuild every mutable adapter from the current authoring level copy. The
	// source LevelObjects remain untouched; runtime deaths, door motion, and
	// animation requests are discarded as part of the explicit restart.
	runtime_level_objects_ = level_.GetLevelObjects();
	runtime_initial_deleted_flags_.clear();
	runtime_initial_deleted_flags_.reserve(
		runtime_level_objects_->GetObjects().size());
	runtime_conditionally_hidden_flags_.assign(
		runtime_level_objects_->GetObjects().size(),
		0U);
	runtime_guard_generator_hidden_flags_.assign(
		runtime_level_objects_->GetObjects().size(),
		0U);
	for (const LevelObject& object : runtime_level_objects_->GetObjects()) {
		runtime_initial_deleted_flags_.push_back(object.deleted ? 1U : 0U);
	}
	runtime_animation_request_serials_.clear();
	gameplayAnimPlaybacks_.clear();
	lastGameplayAnimationTick_ = 0;

	if (!gameplay_host_.ApplyAndRestartGameplay(*gameplay_editor_snapshot_)) {
		status_message_ = "Cannot apply gameplay: runtime session is not active";
		return;
	}

	SetupRuntimeMissionState();
	ApplyRuntimeConditionalContainerStates();
	SetupRuntimeDoors();
	SetupLevelAiGuards();
	gameplay_host_.GetWorld().RefreshAuthoredGuardGeneratorStates();
	ApplyRuntimeGuardGeneratorStates();
	SetupRuntimeLadders();
	SetupRuntimePlayerAnimation();
	gameplayAnimPlaybacks_.clear();
	lastGameplayAnimationTick_ = 0;
	SetupRuntimeInteractionState();
	InitializeGameplayMissionObjectives();

	// RuntimeWorld::Reset intentionally starts at the neutral origin. Restore
	// the authored spawn used when gameplay was opened so Apply+Restart is
	// deterministic and does not turn a source edit into a teleport surprise.
	gameplay_host_.GetWorld().GetPlayer().SetPosition(gameplay_spawn_position_);
	gameplay_host_.GetWorld().GetPlayer().SetOrientation(
		gameplay_spawn_yaw_, gameplay_spawn_pitch_);
	ConfigureGameplayExtractionFallback(
		gameplay_spawn_position_,
		gameplay_spawn_yaw_);
	gameplay_viewer_.pos_ = gameplay_host_.GetWorld().GetPlayer().GetEyePosition();
	gameplay_viewer_.yaw_ = gameplay_spawn_yaw_;
	gameplay_viewer_.pitch_ = gameplay_spawn_pitch_;
	gameplay_viewer_.roll_ = 0.0f;
	gameplay_map_computer_camera_.Reset();
	gameplay_map_computer_open_ = false;
	UpdateGameplayViewerVectors();
	pause_mode_ = false;
	gameplay_host_.SetPaused(false);
	FocusGameplayWindow();
	status_message_ = "Gameplay applied and restarted from the current editor snapshot";
}

void App::ConfigureGameplayExtractionFallback(
	const glm::vec3& spawn_position,
	float spawn_yaw) {
	igi::RuntimeWorld& runtime_world = gameplay_host_.GetWorld();
	if (runtime_world.GetLevelFlow().HasAuthoredMissionFlow()) {
		// Vanilla LevelFlow owns mission completion/failure. The synthetic zone
		// exists only for incomplete/editor-authored levels without that task.
		runtime_world.ClearExtractionZone();
		return;
	}

	const float spawn_yaw_radians = glm::radians(spawn_yaw);
	const glm::vec3 extraction_direction(
		-sinf(spawn_yaw_radians), cosf(spawn_yaw_radians), 0.0f);
	glm::vec3 extraction_center = spawn_position +
		extraction_direction * (40.0f * igi::PlayerController::WORLD_METER);
	float extraction_ground_height = extraction_center.z;
	if (GetLevelZ(extraction_center.x, extraction_center.y, extraction_ground_height)) {
		extraction_center.z = extraction_ground_height;
	}
	runtime_world.SetExtractionZone(
		extraction_center,
		8.0f * igi::PlayerController::WORLD_METER);
}

void App::SetupRuntimeMissionState() {
	const auto& authored_objects = runtime_level_objects_.has_value()
		? runtime_level_objects_->GetObjects()
		: level_.GetLevelObjects().GetObjects();

	const auto is_descendant_of = [&authored_objects](
		int object_index,
		int ancestor_index) {
		int parent_index = authored_objects[object_index].parentIndex;
		for (size_t depth = 0;
			 depth < authored_objects.size() &&
			 parent_index >= 0 &&
			 parent_index < static_cast<int>(authored_objects.size());
			 ++depth) {
			if (parent_index == ancestor_index) {
				return true;
			}
			parent_index = authored_objects[parent_index].parentIndex;
		}
		return false;
	};

	const auto collect_descendants = [
		&authored_objects,
		&is_descendant_of](int ancestor_index) {
		std::vector<int> descendant_indices;
		for (int object_index = 0;
			 object_index < static_cast<int>(authored_objects.size());
			 ++object_index) {
			if (object_index != ancestor_index &&
				is_descendant_of(object_index, ancestor_index)) {
				descendant_indices.push_back(object_index);
			}
		}
		return descendant_indices;
	};

	const auto is_guard_object = [](const LevelObject& object) {
		return object.type == "HumanSoldier" ||
			object.type == "HumanSoldierFemale" ||
			object.type == "HumanSoldierRPG";
	};

	const auto try_read_finite_float = [](const std::string& token, float& value) {
		if (token.empty()) {
			return false;
		}
		try {
			size_t parsed_characters = 0;
			value = std::stof(token, &parsed_characters);
			return parsed_characters == token.size() && std::isfinite(value);
		} catch (...) {
			return false;
		}
	};

	const auto try_read_boolean = [](const std::string& token, bool& value) {
		if (token == "TRUE" || token == "true" || token == "1") {
			value = true;
			return true;
		}
		if (token == "FALSE" || token == "false" || token == "0") {
			value = false;
			return true;
		}
		return false;
	};

	std::vector<igi::MissionStateTaskSource> task_sources;
	for (int object_index = 0;
		 object_index < static_cast<int>(authored_objects.size());
		 ++object_index) {
		const LevelObject& authored_object = authored_objects[object_index];
		// CutScene tasks are deliberately never collected: gameplay is the
		// engine/interaction slice only, with no intro/end cinematics.
		if (authored_object.deleted ||
			(authored_object.type != "AreaActivate" &&
				authored_object.type != "EditVariable" &&
				authored_object.type != "LevelTimer" &&
				authored_object.type != "StatusMessage" &&
				authored_object.type != "ConditionalSound" &&
				authored_object.type != "ConditionalContainer" &&
				authored_object.type != "GuardGenerator" &&
				authored_object.type != "ExplodeObject")) {
			continue;
		}

		igi::MissionStateTaskSource task_source;
		task_source.task_type = authored_object.type;
		task_source.task_id = authored_object.taskId;
		task_source.object_index = object_index;
		task_source.argument_tokens = authored_object.argTokens;
		if (authored_object.type == "ConditionalContainer") {
			task_source.descendant_object_indices =
				collect_descendants(object_index);
		}
		if (authored_object.type == "GuardGenerator") {
			for (const int descendant_index : collect_descendants(object_index)) {
				if (descendant_index < 0 ||
					descendant_index >= static_cast<int>(authored_objects.size()) ||
					authored_objects[descendant_index].deleted ||
					!is_guard_object(authored_objects[descendant_index])) {
					continue;
				}
				task_source.guard_object_indices.push_back(descendant_index);
			}
		}
		task_sources.push_back(std::move(task_source));
	}

	igi::AuthoredMissionStateDefinitions definitions =
		igi::LoadAuthoredMissionStateDefinitions(task_sources);
	const std::string game_root = Utils::GetIGIRootPath();
	const std::array<std::string, 2> mission_text_archive_paths = {
		game_root + "\\language\\ENGLISH\\objectives.res",
		game_root + "\\language\\USA\\objectives.res",
	};
	for (igi::AuthoredMissionStatusMessage& status_message :
		definitions.status_messages) {
		status_message.display_text = ResolveMissionTextResource(
			mission_text_archive_paths,
			status_message.text_resource);
	}
	const size_t area_count = definitions.area_activations.size();
	const size_t edit_variable_count = definitions.edit_variables.size();
	const size_t timer_count = definitions.level_timers.size();
	const size_t cut_scene_count = definitions.cut_scenes.size();
	const size_t conditional_sound_count = definitions.conditional_sounds.size();
	const size_t conditional_container_count =
		definitions.conditional_containers.size();
	const size_t guard_generator_count = definitions.guard_generators.size();
	const size_t explode_object_count = definitions.explode_objects.size();
	const size_t status_message_count = definitions.status_messages.size();
	gameplay_host_.GetWorld().SetAuthoredMissionState(
		std::move(definitions.area_activations),
		std::move(definitions.edit_variables),
		std::move(definitions.level_timers),
		std::move(definitions.status_messages),
		std::move(definitions.cut_scenes),
		std::move(definitions.conditional_sounds),
		std::move(definitions.explode_objects),
		std::move(definitions.conditional_containers),
		std::move(definitions.guard_generators));

	Logger::Get().Log(
		LogLevel::INFO,
		"[Gameplay] Loaded " + std::to_string(area_count) +
		" authored AreaActivate task(s) and " +
		std::to_string(edit_variable_count) +
		" authored EditVariable task(s), " +
		std::to_string(timer_count) +
		" LevelTimer task(s), " +
		std::to_string(cut_scene_count) +
		" CutScene task(s), " +
		std::to_string(conditional_sound_count) +
		" ConditionalSound task(s), " +
		std::to_string(conditional_container_count) +
		" ConditionalContainer task(s), " +
		std::to_string(guard_generator_count) +
		" GuardGenerator task(s), " +
		std::to_string(explode_object_count) +
		" ExplodeObject task(s), " +
		std::to_string(status_message_count) +
		" StatusMessage task(s)");
}

void App::SetupRuntimeDoors() {
	const auto& authored_objects = runtime_level_objects_.has_value()
		? runtime_level_objects_->GetObjects()
		: level_.GetLevelObjects().GetObjects();

	const auto read_token = [](const LevelObject& object, size_t index) {
		if (index >= object.argTokens.size()) {
			return std::string();
		}
		return App::StripQuotes(object.argTokens[index]);
	};
	const auto read_float = [&read_token](const LevelObject& object, size_t index, float fallback) {
		const std::string token = read_token(object, index);
		if (token.empty()) {
			return fallback;
		}
		try {
			return std::stof(token);
		} catch (...) {
			return fallback;
		}
	};
	const auto read_bool = [&read_token](const LevelObject& object, size_t index) {
		const std::string token = read_token(object, index);
		return token == "1" || token == "TRUE" || token == "true";
	};

	std::vector<igi::RuntimeDoorDefinition> door_definitions;
	door_definitions.reserve(authored_objects.size());
	for (int object_index = 0;
		 object_index < static_cast<int>(authored_objects.size());
		 ++object_index) {
		const LevelObject& authored_object = authored_objects[object_index];
		if (authored_object.deleted || authored_object.type != "Door") {
			continue;
		}

		igi::RuntimeDoorDefinition definition;
		definition.object_index = object_index;
		definition.task_id = authored_object.taskId;
		definition.closed_position_units = authored_object.pos;
		definition.closed_rotation_radians = static_cast<float>(authored_object.rot.z);
		definition.slide_offset_units = glm::vec3(
			read_float(authored_object, 6, 0.0f) * igi::PlayerController::WORLD_METER,
			read_float(authored_object, 7, 0.0f) * igi::PlayerController::WORLD_METER,
			read_float(authored_object, 8, 0.0f) * igi::PlayerController::WORLD_METER);
		definition.maximum_angle_degrees = read_float(authored_object, 13, 0.0f);
		definition.open_time_seconds = read_float(authored_object, 14, 1.0f);
		definition.pickable = read_bool(authored_object, 15);
		definition.open_expression = read_token(authored_object, 17);
		definition.close_expression = read_token(authored_object, 18);
		definition.locked_expression = read_token(authored_object, 19);
		definition.open_sound = read_token(authored_object, 20);
		definition.close_sound = read_token(authored_object, 21);
		definition.move_sound = read_token(authored_object, 22);
		door_definitions.push_back(std::move(definition));
	}

	gameplay_host_.GetWorld().SetAuthoredDoors(std::move(door_definitions));
}

void App::SetupRuntimeInteractionState() {
	const auto& authored_objects = runtime_level_objects_.has_value()
		? runtime_level_objects_->GetObjects()
		: level_.GetLevelObjects().GetObjects();
	const auto set_boolean = [this](
		const std::string& task_type,
		const std::string& task_id,
		const std::string& field,
		bool value) {
		if (task_id.empty() || task_id == "-1") {
			return;
		}
		gameplay_host_.GetWorld().SetMissionStateBoolean(
			task_type + "_" + task_id + "." + field,
			value);
	};

	for (const LevelObject& authored_object : authored_objects) {
		if (authored_object.deleted) {
			continue;
		}
		if (authored_object.type == "Terminal") {
			set_boolean(authored_object.type, authored_object.taskId, "isHacked", false);
			set_boolean(authored_object.type, authored_object.taskId, "isExploded", false);
		} else if (authored_object.type == "Switch") {
			set_boolean(authored_object.type, authored_object.taskId, "isPressed", false);
			set_boolean(authored_object.type, authored_object.taskId, "isLastPressed", false);
		} else if (authored_object.type == "Generator") {
			set_boolean(authored_object.type, authored_object.taskId, "isOn", false);
		} else if (authored_object.type == "Car") {
			set_boolean(authored_object.type, authored_object.taskId, "isUsed", false);
		} else if (authored_object.type == "GunPickup" ||
				authored_object.type == "AmmoPickup" ||
				authored_object.type == "GenericPickup") {
			set_boolean(authored_object.type, authored_object.taskId, "isPickedUp", false);
			set_boolean("GenericPickup", authored_object.taskId, "isPickedUp", false);
		} else if (authored_object.type == "GenericTBA") {
			set_boolean(authored_object.type, authored_object.taskId, "isFinished", false);
			set_boolean(authored_object.type, authored_object.taskId, "isFinishedThisTick", false);
		}
	}
}

void App::ApplyRuntimeDoorStates() {
	if (!runtime_level_objects_.has_value()) {
		return;
	}

	auto& objects = runtime_level_objects_->GetObjects();
	for (const igi::RuntimeDoorSnapshot& snapshot :
		gameplay_host_.GetWorld().GetDoorSnapshots()) {
		if (snapshot.object_index < 0 ||
			snapshot.object_index >= static_cast<int>(objects.size())) {
			continue;
		}

		LevelObject& object = objects[static_cast<size_t>(snapshot.object_index)];
		glm::mat4 orientation(1.0f);
		orientation = glm::rotate(
			orientation,
			snapshot.closed_rotation_radians,
			glm::vec3(0.0f, 0.0f, 1.0f));
		orientation = glm::rotate(
			orientation,
			static_cast<float>(object.rot.x),
			glm::vec3(1.0f, 0.0f, 0.0f));
		orientation = glm::rotate(
			orientation,
			static_cast<float>(object.rot.y),
			glm::vec3(0.0f, 1.0f, 0.0f));
		const glm::vec3 world_slide_offset = glm::vec3(
			orientation * glm::vec4(snapshot.slide_offset_units, 0.0f));

		object.pos = snapshot.closed_position_units +
			glm::dvec3(world_slide_offset);
		object.rot.z = static_cast<double>(snapshot.closed_rotation_radians) +
			snapshot.angle_radians;
	}
}

void App::ApplyRuntimeConditionalContainerStates() {
	if (!runtime_level_objects_.has_value()) {
		return;
	}

	auto& objects = runtime_level_objects_->GetObjects();
	if (runtime_initial_deleted_flags_.size() != objects.size()) {
		runtime_initial_deleted_flags_.clear();
		runtime_initial_deleted_flags_.reserve(objects.size());
		runtime_conditionally_hidden_flags_.assign(objects.size(), 0U);
		for (const LevelObject& object : objects) {
			runtime_initial_deleted_flags_.push_back(object.deleted ? 1U : 0U);
		}
	} else if (runtime_conditionally_hidden_flags_.size() != objects.size()) {
		runtime_conditionally_hidden_flags_.assign(objects.size(), 0U);
	}

	std::vector<uint8_t> visible_by_conditional_container(
		objects.size(),
		1U);
	for (const igi::RuntimeConditionalContainerSnapshot& snapshot :
		 gameplay_host_.GetWorld().GetConditionalContainerSnapshots()) {
		if (snapshot.is_running) {
			continue;
		}
		for (const int descendant_index : snapshot.descendant_object_indices) {
			if (descendant_index >= 0 &&
				descendant_index < static_cast<int>(objects.size())) {
				visible_by_conditional_container[
					static_cast<size_t>(descendant_index)] = 0U;
			}
		}
	}

	for (size_t object_index = 0; object_index < objects.size(); ++object_index) {
		if (runtime_initial_deleted_flags_[object_index] != 0U) {
			objects[object_index].deleted = true;
			continue;
		}

		const bool is_conditionally_hidden =
			visible_by_conditional_container[object_index] == 0U;
		if (is_conditionally_hidden) {
			objects[object_index].deleted = true;
		} else if (runtime_conditionally_hidden_flags_[object_index] != 0U) {
			// Only restore visibility that this gate hid. A pickup, dead guard,
			// or authored explosion deleted by gameplay remains deleted.
			objects[object_index].deleted = false;
		}
		runtime_conditionally_hidden_flags_[object_index] =
			is_conditionally_hidden ? 1U : 0U;
	}
}

void App::ApplyRuntimeGuardGeneratorStates() {
	if (!runtime_level_objects_.has_value()) {
		return;
	}

	auto& objects = runtime_level_objects_->GetObjects();
	if (runtime_guard_generator_hidden_flags_.size() != objects.size()) {
		runtime_guard_generator_hidden_flags_.assign(objects.size(), 0U);
	}

	std::vector<uint8_t> visible_by_guard_generator(objects.size(), 1U);
	for (const igi::RuntimeGuardGeneratorSnapshot& snapshot :
		 gameplay_host_.GetWorld().GetGuardGeneratorSnapshots()) {
		const std::unordered_set<int> active_guard_object_indices(
			snapshot.active_guard_object_indices.begin(),
			snapshot.active_guard_object_indices.end());
		for (size_t guard_index = 0;
			 guard_index < snapshot.guard_object_indices.size();
			 ++guard_index) {
			const int object_index = snapshot.guard_object_indices[guard_index];
			if (object_index < 0 ||
				object_index >= static_cast<int>(objects.size())) {
				continue;
			}
			const bool is_enabled = active_guard_object_indices.contains(object_index);
			visible_by_guard_generator[static_cast<size_t>(object_index)] =
				visible_by_guard_generator[static_cast<size_t>(object_index)] &&
				is_enabled;
		}
	}

	for (size_t object_index = 0; object_index < objects.size(); ++object_index) {
		if (runtime_initial_deleted_flags_.size() == objects.size() &&
			runtime_initial_deleted_flags_[object_index] != 0U) {
			objects[object_index].deleted = true;
			continue;
		}

		const bool is_hidden_by_guard_generator =
			visible_by_guard_generator[object_index] == 0U;
		if (is_hidden_by_guard_generator) {
			objects[object_index].deleted = true;
		} else if (runtime_guard_generator_hidden_flags_[object_index] != 0U) {
			const bool is_conditionally_hidden =
				runtime_conditionally_hidden_flags_.size() == objects.size() &&
				runtime_conditionally_hidden_flags_[object_index] != 0U;
			const igi::AiGuardEntity* guard =
				gameplay_host_.GetWorld().GetAi().FindGuard(
					static_cast<uint32_t>(object_index));
			const bool is_dead = guard != nullptr &&
				guard->state == igi::AiGuardState::Dead;
			if (!is_conditionally_hidden && !is_dead) {
				// Restore only visibility owned by this gate. Dead guards and
				// other authored gates remain deleted for the session.
				objects[object_index].deleted = false;
			}
		}
		runtime_guard_generator_hidden_flags_[object_index] =
			is_hidden_by_guard_generator ? 1U : 0U;
	}
}

void App::ApplyRuntimeExplodeObjectStates() {
	if (!runtime_level_objects_.has_value()) {
		return;
	}

	auto& objects = runtime_level_objects_->GetObjects();
	for (const igi::RuntimeExplodeObjectSnapshot& snapshot :
		gameplay_host_.GetWorld().GetExplodeObjectSnapshots()) {
		if (!snapshot.is_exploded ||
			snapshot.object_index < 0 ||
			snapshot.object_index >= static_cast<int>(objects.size())) {
			continue;
		}

		LevelObject& object = objects[static_cast<size_t>(snapshot.object_index)];
		if (snapshot.destroyed_model_name.empty()) {
			object.deleted = true;
			continue;
		}

		object.modelId = snapshot.destroyed_model_name;
	}
}

void App::InitializeGameplayMissionObjectives() {
	const int loaded_level_number = level_.GetLevelNo();
	const uint32_t mission_number = static_cast<uint32_t>(
		loaded_level_number > 0 ? loaded_level_number : 1);

	const auto& authored_objects = runtime_level_objects_.has_value()
		? runtime_level_objects_->GetObjects()
		: level_.GetLevelObjects().GetObjects();

	std::vector<igi::MissionObjectiveTaskSource> objective_task_sources;
	objective_task_sources.reserve(authored_objects.size());
	std::vector<igi::MissionFlowTaskSource> flow_task_sources;
	flow_task_sources.reserve(authored_objects.size());
	std::unordered_map<int32_t, igi::MissionObjectiveLocation> objective_locations;
	objective_locations.reserve(authored_objects.size());
	for (const LevelObject& authored_object : authored_objects) {
		if (authored_object.deleted) {
			continue;
		}
		if (!authored_object.taskId.empty() && authored_object.taskId != "-1") {
			int32_t task_id = -1;
			const char* task_id_begin = authored_object.taskId.data();
			const char* task_id_end = task_id_begin + authored_object.taskId.size();
			const auto parse_result = std::from_chars(
				task_id_begin,
				task_id_end,
				task_id);
			if (parse_result.ec == std::errc() && parse_result.ptr == task_id_end) {
				objective_locations.emplace(
					task_id,
					igi::MissionObjectiveLocation{
						authored_object.pos.x,
						authored_object.pos.y,
						authored_object.pos.z});
			}
		}
		if (authored_object.type == "DefineComputerObjective") {
			igi::MissionObjectiveTaskSource task_source;
			task_source.task_type = authored_object.type;
			task_source.argument_tokens = authored_object.argTokens;
			objective_task_sources.push_back(std::move(task_source));
			continue;
		}
		if (authored_object.type != "LevelFlow") {
			continue;
		}

		igi::MissionFlowTaskSource task_source;
		task_source.task_type = authored_object.type;
		task_source.argument_tokens = authored_object.argTokens;
		flow_task_sources.push_back(std::move(task_source));
	}

	std::vector<igi::AuthoredMissionObjectiveSet> authored_objective_sets =
		igi::LoadAuthoredMissionObjectiveDefinitions(objective_task_sources);
	const std::vector<igi::AuthoredMissionFlowDefinition> authored_flow_definitions =
		igi::LoadAuthoredMissionFlowDefinitions(flow_task_sources);
	igi::AuthoredMissionFlowDefinition authored_flow;
	if (!authored_flow_definitions.empty()) {
		// LevelFlow is normally unique. Preserve the last authored row if an
		// edited snapshot contains duplicates, matching task-update order.
		authored_flow = authored_flow_definitions.back();
	}
	if (authored_objective_sets.empty()) {
		Logger::Get().Log(
			LogLevel::WARNING,
			"[Gameplay] No authored DefineComputerObjective task found for level " +
			std::to_string(mission_number) + "; using runtime fallback objectives");
	}

	const std::string game_root = Utils::GetIGIRootPath();
	const std::array<std::string, 2> objective_archive_paths = {
		game_root + "\\language\\ENGLISH\\objectives.res",
		game_root + "\\language\\USA\\objectives.res",
	};
	igi::MissionObjectiveTextResolver text_resolver =
		[objective_archive_paths](const std::string& resource_key) {
			return ResolveMissionTextResource(objective_archive_paths, resource_key);
		};
	igi::MissionObjectiveLocationResolver location_resolver =
		[objective_locations = std::move(objective_locations)](
			int32_t link_task_id,
			igi::MissionObjectiveLocation& location) {
			const auto location_iterator = objective_locations.find(link_task_id);
			if (location_iterator == objective_locations.end()) {
				return false;
			}
			location = location_iterator->second;
			return true;
		};

	gameplay_host_.GetWorld().GetLevelFlow().InitializeMission(
		mission_number,
		std::move(authored_objective_sets),
		std::move(text_resolver),
		authored_flow,
		std::move(location_resolver));

	Logger::Get().Log(
		LogLevel::INFO,
		"[Gameplay] Loaded " + std::to_string(authored_flow_definitions.size()) +
		" authored LevelFlow task(s)");
}

igi::RuntimeInteractionResult App::HandleGameplayInteraction(
	const glm::vec3& interaction_origin,
	const glm::vec3& interaction_direction) {
	constexpr float interaction_range = 2.5f * igi::PlayerController::WORLD_METER;
	constexpr float minimum_facing_dot = 0.2f;

	glm::vec3 horizontal_direction(interaction_direction.x, interaction_direction.y, 0.0f);
	const float direction_length = glm::length(horizontal_direction);
	if (direction_length <= 0.0001f) {
		return {};
	}
	horizontal_direction /= direction_length;

	const bool use_gameplay_snapshot =
		igi::ResolveRuntimeAssetTarget(
			in_game_mode_,
			runtime_level_objects_.has_value()) ==
		igi::RuntimeAssetTarget::GameplaySnapshot;
	LevelObjects& gameplay_level_objects = use_gameplay_snapshot &&
			runtime_level_objects_.has_value()
		? runtime_level_objects_.value()
		: GetActiveRenderLevelObjects();
	auto& objects = gameplay_level_objects.GetObjects();
	int nearest_index = -1;
	float nearest_distance = interaction_range;
	for (int object_index = 0; object_index < static_cast<int>(objects.size()); ++object_index) {
		const auto& object = objects[object_index];
		const bool is_interactable =
			object.type == "Door" || object.type == "Terminal" ||
			object.type == "Switch" || object.type == "Generator" ||
			object.type == "GunPickup" || object.type == "AmmoPickup" ||
			object.type == "GenericPickup" || object.type == "GenericTBA" ||
			object.type == "Car";
		if (object.deleted || !is_interactable) {
			continue;
		}

		const glm::vec3 offset = glm::vec3(object.pos) - interaction_origin;
		const glm::vec2 horizontal_offset(offset.x, offset.y);
		const float distance = glm::length(horizontal_offset);
		if (distance <= 0.0001f || distance > nearest_distance) {
			continue;
		}

		const glm::vec2 direction_to_object = horizontal_offset / distance;
		const float facing_dot = glm::dot(
			glm::vec2(horizontal_direction.x, horizontal_direction.y),
			direction_to_object);
		if (facing_dot < minimum_facing_dot) {
			continue;
		}

		nearest_index = object_index;
		nearest_distance = distance;
	}

	if (nearest_index < 0) {
		return {};
	}

	auto& target = objects[nearest_index];
	const auto current_objective_requires_authored_state = [this]() {
		for (const igi::MissionObjective& objective :
				 gameplay_host_.GetWorld().GetLevelFlow().GetObjectives()) {
			if (objective.is_primary && objective.state == igi::ObjectiveState::Pending) {
				return !objective.completion_expression.empty();
			}
		}
		return false;
	};

	if (target.type == "Door") {
		if (!gameplay_host_.GetWorld().ToggleDoor(nearest_index)) {
			status_message_ = "Door locked";
			return {};
		}
		status_message_ = "Door opened";
		return {true, !current_objective_requires_authored_state()};
	}

	if (target.type == "Terminal") {
		const std::string terminal_prefix = "Terminal_" + target.taskId;
		gameplay_host_.GetWorld().SetMissionStateBoolean(
			terminal_prefix + ".isHacked",
			true);
		gameplay_host_.GetWorld().SetMissionStatePulse(
			terminal_prefix + ".isHackedThisTick");
		status_message_ = "Terminal hacked";
		return {true, !current_objective_requires_authored_state()};
	}

	if (target.type == "Switch") {
		const std::string switch_prefix = "Switch_" + target.taskId;
		gameplay_host_.GetWorld().SetMissionStateBoolean(
			switch_prefix + ".isPressed",
			true);
		gameplay_host_.GetWorld().SetMissionStatePulse(
			switch_prefix + ".isLastPressed");
		status_message_ = "Switch activated";
		return {true, !current_objective_requires_authored_state()};
	}

	if (target.type == "Generator") {
		gameplay_host_.GetWorld().SetMissionStateBoolean(
			"Generator_" + target.taskId + ".isOn",
			true);
		status_message_ = "Generator started";
		return {true, !current_objective_requires_authored_state()};
	}

	if (target.type == "GunPickup") {
		if (gameplay_host_.GetWorld().GetWeapons().SelectWeaponByScriptId(target.weaponEnumId)) {
			gameplay_host_.GetWorld().SetMissionStateBoolean(
				"GunPickup_" + target.taskId + ".isPickedUp",
				true);
			gameplay_host_.GetWorld().SetMissionStateBoolean(
				"GenericPickup_" + target.taskId + ".isPickedUp",
				true);
			target.deleted = true;
			status_message_ = "Weapon acquired: " +
				gameplay_host_.GetWorld().GetWeapons().GetActiveWeapon().name;
			return {true, false};
		}
		return {};
	}

	if (target.type == "AmmoPickup") {
		uint32_t rounds = gameplay_host_.GetWorld().GetWeapons().GetActiveWeapon().clip_capacity;
		if (target.argTokens.size() > 10) {
			try {
				rounds = static_cast<uint32_t>(
					std::stoul(StripQuotes(target.argTokens[10])));
			} catch (...) {
				// Preserve the clip-sized fallback for malformed authored counts.
			}
		}
		gameplay_host_.GetWorld().GetWeapons().AddReserveAmmo(rounds);
		gameplay_host_.GetWorld().SetMissionStateBoolean(
			"AmmoPickup_" + target.taskId + ".isPickedUp",
			true);
		gameplay_host_.GetWorld().SetMissionStateBoolean(
			"GenericPickup_" + target.taskId + ".isPickedUp",
			true);
		target.deleted = true;
		status_message_ = "Ammunition acquired";
		return {true, false};
	}

	if (target.type == "GenericPickup") {
		gameplay_host_.GetWorld().SetMissionStateBoolean(
			"GenericPickup_" + target.taskId + ".isPickedUp",
			true);
		target.deleted = true;
		status_message_ = "Item acquired";
		return {true, !current_objective_requires_authored_state()};
	}

	if (target.type == "GenericTBA") {
		const std::string generic_prefix = "GenericTBA_" + target.taskId;
		gameplay_host_.GetWorld().SetMissionStateBoolean(
			generic_prefix + ".isFinished",
			true);
		gameplay_host_.GetWorld().SetMissionStatePulse(
			generic_prefix + ".isFinishedThisTick");
		target.deleted = true;
		status_message_ = "Objective item placed";
		return {true, !current_objective_requires_authored_state()};
	}

	if (target.type == "Car") {
		gameplay_host_.GetWorld().SetMissionStateBoolean(
			"Car_" + target.taskId + ".isUsed",
			true);
		status_message_ = "Vehicle entered";
		// Vanilla M1's EditVariable_105 is advanced by the authored car/area
		// task path, which is not yet a general task-tree native. Keep the
		// existing interaction fallback so the vertical slice remains playable.
		return {true, true};
	}

	status_message_ = "Objective interaction complete";
	return {true, true};
}

// Registers every in-level enemy (soldier family) into the runtime AiSystem so
// they patrol along their AIGraph waypoints during Game Play mode. Guard ids
// equal the level-object index, which the frame loop uses to update the
// session-owned render copy.
static bool LastTwoIntArgs(const std::string& qscLine, int& a, int& b);

void App::SetupLevelAiGuards() {
	auto& ai = gameplay_host_.GetWorld().GetAi();
	ai.Clear();
	gameplay_host_.GetWorld().ClearGuardScripts();

	auto& objects = GetActiveRenderLevelObjects().GetObjects();

	// Build a lookup of AIGraph taskId -> world pos (node coords in the .dat are
	// local to the AIGraph task's own position).
	std::map<std::string, glm::dvec3> graph_world_offsets;
	for (const auto& obj : objects) {
		if (!obj.deleted && obj.type == "AIGraph" && !obj.taskId.empty()) {
			graph_world_offsets[obj.taskId] = obj.pos;
		}
	}

	// Cache parsed nav graphs per graph id so soldiers sharing a graph (most of
	// them) don't re-parse the route table for every guard.
	std::map<int, std::shared_ptr<const GraphFile>> graph_cache;

	int level_no = (last_loaded_level_ >= 1) ? last_loaded_level_ : 1;

	for (int i = 0; i < (int)objects.size(); ++i) {
		const auto& obj = objects[i];
		if (obj.deleted) continue;
		bool isEnemy = (obj.type == "HumanSoldier" || obj.type == "HumanSoldierFemale" ||
		                obj.type == "HumanSoldierRPG");
		if (!isEnemy) continue;

		// No position filtering: each of the fourteen levels owns a different
		// world origin (observed z spans ~1.61e8-1.75e8), so no magnitude
		// heuristic can separate "unresolved" from "authored". A guard whose
		// task truly lacks a position simply stands still in play.

		igi::AiGuardEntity guard;
		guard.id = (uint32_t)i;
		guard.name = obj.name.empty() ? obj.type : obj.name;
		guard.mission_state_type = obj.type;
		guard.mission_task_id = obj.taskId;
		guard.weapon_script_id = !obj.weaponEnumId.empty()
			? obj.weaponEnumId
			: obj.primaryWeapon;
		guard.position = glm::vec3((float)obj.pos.x, (float)obj.pos.y, (float)obj.pos.z);
		guard.yaw = glm::degrees((float)obj.rot.z);
		// Declared idle clip (HumanSoldier arg@10); vanilla falls back to 2
		// when the task declares none or the set does not hold it.
		guard.stand_animation = (obj.standAnimation >= 0) ? obj.standAnimation : 2;

		// Resolve the AIGraph this enemy patrols (from its HumanAI child) and the
		// AI behavior script task id (ai/<taskId>.qvm).
		int aiGraphTaskId = -1;
		int aiTaskId = -1;
		std::string ai_script_path;
		for (int ci : obj.childrenIndices) {
			if (ci < 0 || ci >= (int)objects.size()) continue;
			if (!objects[ci].deleted && objects[ci].type == "HumanAI") {
				aiGraphTaskId = objects[ci].aiGraphTaskId;
				try { aiTaskId = std::stoi(objects[ci].taskId); }
				catch (...) { aiTaskId = -1; }
				break;
			}
		}

		if (aiGraphTaskId >= 0) {
			std::string tid = std::to_string(aiGraphTaskId);
			auto offIt = graph_world_offsets.find(tid);
			if (offIt != graph_world_offsets.end()) {
				auto cached = graph_cache.find(aiGraphTaskId);
				if (cached == graph_cache.end()) {
					std::string graphPath = Utils::GetIGIRootPath() +
						"\\missions\\location0\\level" + std::to_string(level_no) +
						"\\graphs\\graph" + tid + ".dat";
					auto g = std::make_shared<const GraphFile>(GRAPH_Parse(graphPath));
					graph_cache[aiGraphTaskId] = g;
					cached = graph_cache.find(aiGraphTaskId);
				}
				if (cached->second->valid && !cached->second->nodes.empty()) {
					guard.graph = cached->second;
					guard.graph_offset = glm::vec3((float)offIt->second.x,
					                               (float)offIt->second.y,
					                               (float)offIt->second.z);
					// Snapshot the graph nodes as waypoints too, so the Suspicious
					// investigation path keeps its existing behaviour.
					for (const auto& n : guard.graph->nodes) {
						guard.waypoints.push_back(glm::vec3(
							(float)(offIt->second.x + n.x),
							(float)(offIt->second.y + n.y),
							(float)(offIt->second.z + n.z)));
					}
					// The guard starts at the graph node nearest its spawn (local coords).
					guard.current_node = GRAPH_NearestNode(*guard.graph,
						obj.pos.x - offIt->second.x,
						obj.pos.y - offIt->second.y,
						obj.pos.z - offIt->second.z);
				}
			}
		}
		if (guard.waypoints.empty()) {
			// No usable AIGraph â€” stand guard in place (still hears/sees player).
			guard.waypoints.push_back(guard.position);
		}

		// Build the OpenIGI patrol command list from the AI script's
		// AIAction_Patrol(pathId) plus that PatrolPath task's children.
		if (aiTaskId >= 0) {
			std::string qvmPath = Utils::GetIGIRootPath() + "\\missions\\location0\\level" +
				std::to_string(level_no) + "\\ai\\" + std::to_string(aiTaskId) + ".qvm";
			ai_script_path = qvmPath;
			int pathId = FindAiScriptPatrolPathId(qvmPath);
			if (pathId >= 0) {
				// Find the PatrolPath task the script names (a child of this soldier
				// in retail data), then collect its PatrolPathCommand children in order.
				int ppIndex = -1;
				for (int ci : obj.childrenIndices) {
					if (ci < 0 || ci >= (int)objects.size()) continue;
					const auto& child = objects[ci];
					if (child.deleted || child.type != "PatrolPath") continue;
					try {
						if (std::stoi(child.taskId) == pathId) { ppIndex = ci; break; }
					} catch (...) {}
				}
				if (ppIndex < 0) {
					// Retail data does not always nest the PatrolPath under the
					// soldier; fall back to a global id search.
					for (int ci = 0; ci < (int)objects.size(); ++ci) {
						const auto& child = objects[ci];
						if (child.deleted || child.type != "PatrolPath") continue;
						try {
							if (std::stoi(child.taskId) == pathId) { ppIndex = ci; break; }
						} catch (...) {}
					}
				}
				if (ppIndex >= 0) {
					for (int pci : objects[ppIndex].childrenIndices) {
							if (pci < 0 || pci >= (int)objects.size()) continue;
							const auto& pcmd = objects[pci];
							if (pcmd.deleted || pcmd.type != "PatrolPathCommand") continue;
							int cmdCode = -1, param = -1;
							if (LastTwoIntArgs(pcmd.qscLine, cmdCode, param)) {
								guard.patrol_commands.push_back(
									igi::AiPatrolCommand{ (igi::AiPatrolCommandKind)cmdCode, param });
							}
					}
					guard.active_patrol_path_id = pathId;
					Logger::Get().Log(LogLevel::INFO,
						"[AI] Guard " + guard.name + " patrol path " +
						std::to_string(pathId) + ": " +
						std::to_string(guard.patrol_commands.size()) + " commands");
				} else {
					Logger::Get().Log(LogLevel::WARNING,
						"[AI] Guard " + guard.name + " names patrol path " +
						std::to_string(pathId) + " but no matching PatrolPath child");
				}
			}
		}

		// Keep every authored route available to AIAction_Patrol. The retail
		// script may select a different path after an event or script variable
		// changes; the runtime switches routes at the action boundary.
		for (int ci : obj.childrenIndices) {
			if (ci < 0 || ci >= (int)objects.size()) continue;
			const auto& patrol_path = objects[ci];
			if (patrol_path.deleted || patrol_path.type != "PatrolPath") continue;

			int route_id = -1;
			try { route_id = std::stoi(patrol_path.taskId); } catch (...) { continue; }
			std::vector<igi::AiPatrolCommand> route_commands;
			for (int pci : patrol_path.childrenIndices) {
				if (pci < 0 || pci >= (int)objects.size()) continue;
				const auto& command = objects[pci];
				if (command.deleted || command.type != "PatrolPathCommand") continue;
				int command_kind = -1, operand = -1;
				if (LastTwoIntArgs(command.qscLine, command_kind, operand)) {
					route_commands.push_back(igi::AiPatrolCommand{
						(igi::AiPatrolCommandKind)command_kind,
						operand});
				}
			}
			if (!route_commands.empty()) {
				guard.patrol_routes[route_id] = std::move(route_commands);
			}
		}

		ai.RegisterGuard(guard);
		if (!ai_script_path.empty()
			&& !gameplay_host_.GetWorld().AttachGuardScriptFromFile(guard.id, ai_script_path)) {
			Logger::Get().Log(LogLevel::WARNING,
				"[AI] Could not attach retail QVM " + ai_script_path);
		}
	}
	Logger::Get().Log(LogLevel::INFO, "[App] Registered " +
		std::to_string(ai.GetGuards().size()) + " enemy guards for Game Play");
}

void App::AddRuntimeLadderIfPresent(
    const std::string& model_id,
    bool is_building,
    const glm::mat4& draw_world_matrix,
    const glm::mat3& task_orientation,
    std::vector<igi::LadderPlacement>& ladder_placements) {
    if (!renderer_.IsLadderMagicObject(model_id)) {
        return;
    }

    const std::vector<glm::vec3> magic_vertices =
        renderer_.GetModelMagicVertices(model_id, is_building);
    if (magic_vertices.size() <= 3U) {
        Logger::Get().Log(LogLevel::WARNING,
            "[Gameplay] Ladder model " + model_id +
            " has no complete magic-vertex climb line");
        return;
    }

    // Mesh magic vertices are imported through the same 40.96 model-unit
    // conversion used by the renderer. The ladder contract consumes gameplay
    // units, so apply the renderer's leaf scale before world transformation.
    constexpr float model_units_to_runtime_units = 40.96f;
    const glm::mat4 magic_world_matrix = draw_world_matrix * glm::scale(
        glm::mat4(1.0f),
        glm::vec3(model_units_to_runtime_units));
    const auto transform_magic_vertex = [
        &magic_world_matrix,
        &magic_vertices](size_t index) {
        return glm::vec3(magic_world_matrix * glm::vec4(
            magic_vertices[index],
            1.0f));
    };

    ladder_placements.emplace_back(
        glm::vec3(draw_world_matrix[3]),
        transform_magic_vertex(1),
        transform_magic_vertex(2),
        transform_magic_vertex(3),
        task_orientation);
}

void App::CollectAttachedRuntimeLadders(
    const std::string& model_id,
    bool is_building,
    const glm::mat4& draw_world_matrix,
    const glm::mat3& task_orientation,
    std::unordered_set<std::string>& ancestry,
    std::vector<igi::LadderPlacement>& ladder_placements) {
    if (model_id.empty() || !ancestry.insert(model_id).second) {
        return;
    }

    AddRuntimeLadderIfPresent(
        model_id,
        is_building,
        draw_world_matrix,
        task_orientation,
        ladder_placements);

    glm::mat4 parent_rotation = draw_world_matrix;
    parent_rotation[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    for (const AttachInfo& attachment : renderer_.GetModelAttachments(model_id, is_building)) {
        const glm::mat4 attachment_rotation(
            attachment.r[0], attachment.r[1], attachment.r[2], 0.0f,
            attachment.r[3], attachment.r[4], attachment.r[5], 0.0f,
            attachment.r[6], attachment.r[7], attachment.r[8], 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f);
        const glm::vec3 local_offset(
            attachment.px,
            attachment.py,
            attachment.pz);
        const glm::vec3 world_position = glm::vec3(
            draw_world_matrix * glm::vec4(local_offset, 1.0f));
        const glm::mat4 child_world_matrix = glm::translate(
            glm::mat4(1.0f),
            world_position) * parent_rotation * attachment_rotation;

        // The task frame uses the attachment transpose, while magic vertices
        // use the draw frame. This follows the reference's separate
        // childDrawOrientation/childTaskOrientation paths.
        const glm::mat3 child_task_orientation = task_orientation *
            glm::transpose(glm::mat3(attachment_rotation));
        CollectAttachedRuntimeLadders(
            attachment.modelId,
            is_building,
            child_world_matrix,
            child_task_orientation,
            ancestry,
            ladder_placements);
    }

    ancestry.erase(model_id);
}

void App::SetupRuntimeLadders() {
    std::vector<igi::LadderPlacement> ladder_placements;
    const bool use_gameplay_snapshot =
        igi::ResolveRuntimeAssetTarget(
            in_game_mode_,
            runtime_level_objects_.has_value()) ==
        igi::RuntimeAssetTarget::GameplaySnapshot;
    const LevelObjects& gameplay_level_objects = use_gameplay_snapshot &&
            runtime_level_objects_.has_value()
        ? runtime_level_objects_.value()
        : GetActiveRenderLevelObjects();
    const auto& objects = gameplay_level_objects.GetObjects();
    for (const LevelObject& object : objects) {
        if (object.deleted || object.modelId.empty()) {
            continue;
        }

        glm::mat4 object_rotation(1.0f);
        object_rotation = glm::rotate(
            object_rotation,
            static_cast<float>(object.rot.z),
            glm::vec3(0.0f, 0.0f, 1.0f));
        object_rotation = glm::rotate(
            object_rotation,
            static_cast<float>(object.rot.x),
            glm::vec3(1.0f, 0.0f, 0.0f));
        object_rotation = glm::rotate(
            object_rotation,
            static_cast<float>(object.rot.y),
            glm::vec3(0.0f, 1.0f, 0.0f));

        const glm::mat4 object_world_matrix = glm::translate(
            glm::mat4(1.0f),
            glm::vec3(object.pos)) * object_rotation;
        std::unordered_set<std::string> ancestry;
        CollectAttachedRuntimeLadders(
            object.modelId,
            object.isBuilding,
            object_world_matrix,
            glm::mat3(object_rotation),
            ancestry,
            ladder_placements);
    }

    gameplay_host_.GetWorld().SetLadderPlacements(std::move(ladder_placements));
    Logger::Get().Log(LogLevel::INFO,
        "[Gameplay] Registered " + std::to_string(
            gameplay_host_.GetWorld().GetLadderPlacements().size()) +
        " authored ladder placements");
}

void App::SetupRuntimePlayerAnimation() {
    player_animation_driver_.ClearAnimationClips();

    const LevelObjects& gameplay_level_objects = runtime_level_objects_.has_value()
        ? runtime_level_objects_.value()
        : level_.GetLevelObjects();
    const auto& objects = gameplay_level_objects.GetObjects();

    int player_bone_hierarchy = -1;
    int player_object_index = -1;
    for (int object_index = 0;
         object_index < static_cast<int>(objects.size());
         ++object_index) {
        const LevelObject& object = objects[static_cast<size_t>(object_index)];
        if (!object.deleted && object.type == "HumanPlayer" &&
            object.boneHierarchy >= 0) {
            player_bone_hierarchy = object.boneHierarchy;
            player_object_index = object_index;
            break;
        }
    }

    if (player_bone_hierarchy < 0) {
        Logger::Get().Log(
            LogLevel::WARNING,
            "[Gameplay] No HumanPlayer animation hierarchy found; using physics movement fallback");
        return;
    }

    // HumanLocomotionStates.cs plus the three authored ladder animations. The
    // registry is already imported during level loading, so this setup only
    // injects immutable clip addresses into the fixed-step driver.
    constexpr int player_animation_ids[] = {
        2, 4, 37, 35, 64, 70, 68, 11, 10,
        57, 74, 72, 75, 77, 76, 81, 80,
        168, 169, 170,
    };
    int resolved_clip_count = 0;
    for (const int animation_id : player_animation_ids) {
        const AnimationClip* animation_clip = animRegistry_.GetClipByAnimId(
            player_bone_hierarchy,
            animation_id);
        if (animation_clip == nullptr) {
            continue;
        }
        player_animation_driver_.SetAnimationClip(animation_id, animation_clip);
        ++resolved_clip_count;
    }

    Logger::Get().Log(
        LogLevel::INFO,
        "[Gameplay] Player animation driver resolved " +
        std::to_string(resolved_clip_count) + "/19 clips for object " +
        std::to_string(player_object_index) +
        (resolved_clip_count == 0 ? "; physics movement fallback remains active" : ""));
}

void App::SetEditBrush(int brush) {
	if (brush < 0) brush = 0;
	if (brush > 3) brush = 3;
	edit_brush_ = brush;
	static const char* kNames[] = {"Raise", "Lower", "Soften", "Flatten"};
	status_message_ = std::string("Terrain brush: ") + kNames[edit_brush_] +
		"  (radius " + std::to_string((long)edit_brush_radius_) +
		", strength " + std::to_string((long)edit_brush_strength_) + ")";
	UpdateCursorMode();
	glutPostRedisplay();
}

int App::GetEditBrush() const {
	return edit_brush_;
}

bool App::TerrainPaletteClick(int x, int y) {
	if (!terrain_edit_enabled_) return false;
	int idx = TerrainPalette::HitTest(x, y, window_state_.viewport_width_, window_state_.viewport_height_);
	if (idx < 0) return false;
	switch (idx) {
	case TerrainPalette::kSelect:
		// Select/exit button: leave terrain edit, back to object editing.
		SetTerrainEditEnabled(false);
		break;
	case TerrainPalette::kRadiusDec:   AdjustBrushRadius(0.8);    break;
	case TerrainPalette::kRadiusInc:   AdjustBrushRadius(1.25);   break;
	case TerrainPalette::kStrengthDec: AdjustBrushStrength(-1.0); break;
	case TerrainPalette::kStrengthInc: AdjustBrushStrength(1.0);  break;
	default:
		SetEditBrush(TerrainPalette::BrushForIndex(idx));
		break;
	}
	return true;
}

void App::AdjustBrushRadius(double factor) {
	edit_brush_radius_ *= factor;
	if (edit_brush_radius_ < 5000.0)   edit_brush_radius_ = 5000.0;
	if (edit_brush_radius_ > 250000.0) edit_brush_radius_ = 250000.0;
	status_message_ = "Brush radius: " + std::to_string((long)edit_brush_radius_);
	glutPostRedisplay();
}

void App::AdjustBrushStrength(double delta) {
	edit_brush_strength_ += delta;
	if (edit_brush_strength_ < 1.0)   edit_brush_strength_ = 1.0;
	if (edit_brush_strength_ > 100.0) edit_brush_strength_ = 100.0;
	status_message_ = "Brush strength: " + std::to_string((long)edit_brush_strength_);
	glutPostRedisplay();
}

void App::SetSelectedObjectScale(float scale) {
	if (selected_object_index_ >= 0 && selected_object_index_ < (int)level_.GetLevelObjects().GetObjects().size()) {
		level_.GetLevelObjects().GetObjects()[selected_object_index_].scale = scale;
		Logger::Get().Log(LogLevel::INFO, "[App] Scale changed to " + std::to_string(scale) + " for object " + std::to_string(selected_object_index_));
	}
}

float App::GetSelectedObjectScale() const {
	if (selected_object_index_ >= 0 && selected_object_index_ < (int)level_.GetLevelObjects().GetObjects().size()) {
		return level_.GetLevelObjects().GetObjects()[selected_object_index_].scale;
	}
	return 1.0f;
}

#include <glm/ext/matrix_projection.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

// â”€â”€ Animation system â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// (Per-object auto-play init now lives in LoadLevel's parallel resolution pass;
//  see app_level.cpp. There is no single-object initializer anymore.)

void App::UpdateAnimations(float dtSec) {
    // Skip if global pause is on
    // (renderer pause check is done in the renderer)

    // For each active playback, update time
    for (auto& [idx, pb] : animPlaybacks_) {
        pb.Update(dtSec * 1000.f);
    }
}

void App::ApplyRuntimeAiAnimationRequests() {
    const auto& guards = gameplay_host_.GetWorld().GetAi().GetGuards();
    const auto& objects = GetActiveRenderLevelObjects().GetObjects();

    for (const auto& guard : guards) {
        if (guard.state == igi::AiGuardState::Dead ||
            guard.requested_animation < 0 ||
            guard.animation_request_serial == 0 ||
            guard.id >= objects.size()) {
            continue;
        }

        const auto& object = objects[guard.id];
        if (object.deleted || object.boneHierarchy < 0) {
            continue;
        }

        const auto applied_serial = runtime_animation_request_serials_.find(guard.id);
        if (applied_serial != runtime_animation_request_serials_.end() &&
            applied_serial->second == guard.animation_request_serial) {
            continue;
        }

        const AnimationClip* requested_clip = animRegistry_.GetClipByAnimId(
            object.boneHierarchy,
            guard.requested_animation);
        if (requested_clip == nullptr) {
            continue;
        }

		auto& playback = gameplayAnimPlaybacks_[static_cast<int>(guard.id)];
        if (playback.clip != requested_clip || !playback.playing) {
            playback.Start(requested_clip);
            playback.forceLoop = true;
        }
        runtime_animation_request_serials_[guard.id] = guard.animation_request_serial;
	}
}

void App::AdvanceGameplayAnimationClocks() {
	ApplyRuntimeAiAnimationRequests();
	const uint64_t simulation_tick = gameplay_host_.GetRenderSnapshot().simulation_tick;
	if (simulation_tick <= lastGameplayAnimationTick_) {
		return;
	}

	const uint64_t elapsed_ticks = simulation_tick - lastGameplayAnimationTick_;
	const float elapsed_ms = static_cast<float>(elapsed_ticks) *
		(1000.0f / static_cast<float>(igi::GameClock::TICK_RATE_HZ));
	for (auto& [object_index, playback] : gameplayAnimPlaybacks_) {
		(void)object_index;
		playback.Update(elapsed_ms);
	}
	lastGameplayAnimationTick_ = simulation_tick;
}

std::string App::BuildAnimStatusString() {
    if (animPlaybacks_.empty()) return {};

    std::string s;
    int count = 0;
    auto& objects = level_.GetLevelObjects().GetObjects();
    for (const auto& [idx, pb] : animPlaybacks_) {
        if (!pb.clip) continue;
        if (count >= 5) { // cap at 5 lines
            s += "... and " + std::to_string((int)animPlaybacks_.size() - count) + " more\n";
            break;
        }
        std::string name = (idx >= 0 && idx < (int)objects.size()) ? objects[idx].name : ("#" + std::to_string(idx));
        if (name.empty()) name = objects[idx].modelId;
        s += name + ": " + pb.clip->name;
        if (pb.playing) {
            int ms = (int)pb.currentTimeMs;
            int d = pb.clip->duration_ms();
            s += " [" + std::to_string(ms) + "/" + std::to_string(d) + "ms]";
        } else {
            s += " [paused]";
        }
        s += "\n";
        count++;
    }
    if (!s.empty() && s.back() == '\n') s.pop_back();
    return s;
}

// Parses the last two comma-separated args of a flat "Task_New(...)" line,
// e.g. PatrolPathCommand's (cmdCode, param) pair in
// `Task_New(-1, "PatrolPathCommand", "Plays predefined animation 240", 0, 240)`.
static bool LastTwoIntArgs(const std::string& qscLine, int& a, int& b) {
    size_t close = qscLine.rfind(')');
    if (close == std::string::npos) return false;
    size_t open = qscLine.rfind('(', close);
    if (open == std::string::npos) return false;
    std::string inner = qscLine.substr(open + 1, close - open - 1);
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= inner.size(); ++i) {
        if (i == inner.size() || inner[i] == ',') {
            parts.push_back(inner.substr(start, i - start));
            start = i + 1;
        }
    }
    if (parts.size() < 2) return false;
    try {
        b = std::stoi(parts[parts.size() - 1]);
        a = std::stoi(parts[parts.size() - 2]);
    } catch (...) { return false; }
    return true;
}

int App::FindHumanAiTaskId(int objIndex) const {
    auto& objects = level_.GetLevelObjects().GetObjects();
    if (objIndex < 0 || objIndex >= (int)objects.size()) return -1;
    for (int ci : objects[objIndex].childrenIndices) {
        if (ci < 0 || ci >= (int)objects.size()) continue;
        if (objects[ci].deleted || objects[ci].type != "HumanAI") continue;
        try { return std::stoi(objects[ci].taskId); } catch (...) { return -1; }
    }
    return -1;
}

const std::vector<int>& App::GetOrComputeAnimationIds(int objIndex) {
    static const std::vector<int> kEmpty;
    auto cached = animIdsCache_.find(objIndex);
    if (cached != animIdsCache_.end()) return cached->second;

    auto& objects = level_.GetLevelObjects().GetObjects();
    if (objIndex < 0 || objIndex >= (int)objects.size() || objects[objIndex].boneHierarchy < 0) return kEmpty;

    return animIdsCache_[objIndex] = ComputeAnimationIdsForObject(objIndex);
}

// Pure computation, no cache reads/writes â€” safe to call concurrently from worker
// threads (level-load parallel animation resolution) as long as no other thread is
// mutating LevelObjects/AnimationRegistry at the same time (registry must already
// be fully imported â€” see LoadLevel's sequential ImportAnimations pre-pass).
std::vector<int> App::ComputeAnimationIdsForObject(int objIndex) const {
    auto& objects = level_.GetLevelObjects().GetObjects();
    if (objIndex < 0 || objIndex >= (int)objects.size()) return {};
    const auto& obj = objects[objIndex];
    if (obj.boneHierarchy < 0) return {};

    std::vector<int> ids;
    if (obj.standAnimation >= 0) ids.push_back(obj.standAnimation);

    int aiTaskId = FindHumanAiTaskId(objIndex);
    if (aiTaskId >= 0 && last_loaded_level_ >= 0) {
        std::string qvmPath = Utils::GetIGIRootPath() + "\\missions\\location0\\level" +
            std::to_string(last_loaded_level_) + "\\ai\\" + std::to_string(aiTaskId) + ".qvm";
        Logger::Get().Log(LogLevel::INFO, "[Anim] Resolving animation ids for object " +
            std::to_string(objIndex) + " via AI task " + std::to_string(aiTaskId) + " (" + qvmPath + ")");
        for (int id : FindAiScriptAnimationIds(qvmPath)) {
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        }
    } else {
        Logger::Get().Log(LogLevel::DEBUG, "[Anim] Object " + std::to_string(objIndex) +
            " has no HumanAI child task â€” only Stand Animation id (if any) is available");
    }

    // PatrolPath children can include "PatrolPathCommand" entries that play a
    // predefined animation (cmdCode == 0, param == animation id), independent
    // of the AI behavior script's own AIAction_PlayAnimation call.
    for (int ci : obj.childrenIndices) {
        if (ci < 0 || ci >= (int)objects.size()) continue;
        if (objects[ci].deleted || objects[ci].type != "PatrolPath") continue;
        for (int pci : objects[ci].childrenIndices) {
            if (pci < 0 || pci >= (int)objects.size()) continue;
            const auto& pcmd = objects[pci];
            if (pcmd.deleted || pcmd.type != "PatrolPathCommand") continue;
            int cmdCode = -1, param = -1;
            if (LastTwoIntArgs(pcmd.qscLine, cmdCode, param) && cmdCode == 0) {
                Logger::Get().Log(LogLevel::INFO, "[Anim] Object " + std::to_string(objIndex) +
                    " PatrolPathCommand plays predefined animation " + std::to_string(param));
                if (std::find(ids.begin(), ids.end(), param) == ids.end()) ids.push_back(param);
            }
        }
    }

    Logger::Get().Log(LogLevel::INFO, "[Anim] Object " + std::to_string(objIndex) + " (" + obj.modelId +
        ", bone hierarchy " + std::to_string(obj.boneHierarchy) + "): " + std::to_string(ids.size()) +
        " animation id(s) available");

    return ids;
}

void App::ToggleAnimationForObject(int objIndex, int animId) {
    auto& objects = level_.GetLevelObjects().GetObjects();
    if (objIndex < 0 || objIndex >= (int)objects.size()) return;
    const auto& obj = objects[objIndex];
    if (obj.boneHierarchy < 0) return;

    if (!animRegistry_.ImportAnimations(obj.boneHierarchy)) {
        Logger::Get().Log(LogLevel::WARNING, "[Anim] Could not import bone hierarchy " +
            std::to_string(obj.boneHierarchy) + " for object " + std::to_string(objIndex));
        return;
    }
    const AnimationClip* clip = animRegistry_.GetClipByAnimId(obj.boneHierarchy, animId);
    if (!clip) {
        Logger::Get().Log(LogLevel::WARNING, "[Anim] Animation id " + std::to_string(animId) +
            " not found in bone hierarchy " + std::to_string(obj.boneHierarchy));
        return;
    }

    auto& pb = animPlaybacks_[objIndex];
    if (pb.clip == clip && pb.playing) {
        pb.Pause();
        Logger::Get().Log(LogLevel::INFO, "[Anim] Paused '" + clip->name + "' for object " + std::to_string(objIndex));
    } else {
        if (pb.clip == clip) {
            pb.Resume();
            Logger::Get().Log(LogLevel::INFO, "[Anim] Resumed '" + clip->name + "' for object " + std::to_string(objIndex));
        } else {
            pb.Start(clip);
            Logger::Get().Log(LogLevel::INFO, "[Anim] Playing '" + clip->name + "' for object " + std::to_string(objIndex));
        }
    }
}

std::unordered_set<int> App::GetSkinnedReplacementObjectIndices(bool gameplay_render) {
    std::unordered_set<int> result;
    auto& objs = GetActiveRenderLevelObjects().GetObjects();
    const auto& active_playbacks = gameplay_render
        ? gameplayAnimPlaybacks_ : animPlaybacks_;
    for (const auto& [idx, pb] : active_playbacks) {
        if (idx < 0 || idx >= (int)objs.size()) continue;
        const auto& obj = objs[idx];
        // Only replace the static mesh if the skinned replacement can actually
        // render — otherwise a model whose skin geometry fails to load goes
        // permanently invisible (neither static nor skinned draw produces anything).
        if (!ShouldUseSkinnedReplacement(pb, idx, objs.size(), obj.deleted,
                                         renderer_.HasSkinGeometry(obj.modelId, obj.isBuilding))) continue;
        result.insert(idx);
    }

    if (result != animSkinnedIndicesPrev_) {
        Logger::Get().Log(LogLevel::INFO, "[Anim] Skinned/animated replacement active for " +
            std::to_string(result.size()) + " AI object(s) in parallel: " +
            [&]() {
                std::string s;
                for (int idx : result) s += std::to_string(idx) + " ";
                return s;
            }());
        animSkinnedIndicesPrev_ = result;
    }
    return result;
}

void App::ComputePropAnimUiState(int& boneHierarchy, std::vector<int>& ids, int& activeId, bool& isPlaying) {
    boneHierarchy = -1;
    ids.clear();
    activeId = -1;
    isPlaying = false;

    if (!prop_editor_open_ || selected_object_index_ < 0) return;
    auto& objects = level_.GetLevelObjects().GetObjects();
    if (selected_object_index_ >= (int)objects.size()) return;
    const auto& obj = objects[selected_object_index_];
    if (obj.boneHierarchy < 0) return;

    boneHierarchy = obj.boneHierarchy;
    ids = GetOrComputeAnimationIds(selected_object_index_);
    auto pbIt = animPlaybacks_.find(selected_object_index_);
    if (pbIt != animPlaybacks_.end() && pbIt->second.clip) {
        activeId = pbIt->second.clip->animId;
        isPlaying = pbIt->second.playing;
    }
}

void App::ToggleAutoSave() {
	auto_save_enabled_ = !auto_save_enabled_;
	auto_save_last_time_ms_ = Sys_Milliseconds();
	Config::Get().auto_save_enabled = auto_save_enabled_;
	Config::Save();
	status_message_ = auto_save_enabled_ ? "Auto-save: ON" : "Auto-save: OFF";
}

void App::AdjustAutoSaveInterval(int delta_seconds) {
	auto_save_interval_seconds_ += delta_seconds;
	if (auto_save_interval_seconds_ < 10)   auto_save_interval_seconds_ = 10;
	if (auto_save_interval_seconds_ > 3600) auto_save_interval_seconds_ = 3600;
	auto_save_last_time_ms_ = Sys_Milliseconds();
	Config::Get().auto_save_interval_seconds = auto_save_interval_seconds_;
	Config::Save();
	status_message_ = "Auto-save interval: " + std::to_string(auto_save_interval_seconds_) + "s";
}
