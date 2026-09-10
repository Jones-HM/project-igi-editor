/******************************************************************************
 * @file    renderer.h
 * @brief   main renderer
 *****************************************************************************/

#pragma once

#include <cstdint>
#include <string>
#include <set>
#include <unordered_map>
#include <vector>
#include <utility>
#include "renderer_objects.h"
#include "renderer_splines.h"
#include "renderer_rain.h"
#include "../level/task_schema.h"
#include "../mission_state.h"
#include "../runtime/runtime_renderer.h"
#include "graph_writer.h"
#include <functional>

/*
 Terrain brush palette layout (shared between App click hit-testing and Renderer drawing).
 5 buttons in a vertical column anchored to the bottom-right corner.
 Index 0 = Select/exit (arrow), 1=Raise, 2=Lower, 3=Soften, 4=Flatten.
 Returns rect in TOP-DOWN screen coords (y grows downward, matching GLUT mouse y).
*/
namespace TerrainPalette {
	static constexpr int kBrushCount = 5;  // Select + Raise/Lower/Soften/Flatten
	static constexpr int kBtnSize    = 36;
	static constexpr int kBtnGap     = 6;
	static constexpr int kMarginX    = 14;
	static constexpr int kMarginY    = 14;

	// Settings buttons sit below the brush column as a 2x2 grid: Radius-/Radius+
	// on one row, Strength-/Strength+ on the next. They are half-height so the
	// panel stays compact.
	static constexpr int kSetBtnH    = 24;
	static constexpr int kSetGap     = 4;

	// Shared index space between App (hit-test) and Renderer (draw).
	// 0..4 = brush column; 5..8 = settings buttons.
	enum Index {
		kSelect      = 0,
		kRaise       = 1,
		kLower       = 2,
		kSoften      = 3,
		kFlatten     = 4,
		kRadiusDec   = 5,
		kRadiusInc   = 6,
		kStrengthDec = 7,
		kStrengthInc = 8,
		kCount       = 9
	};

	// Maps a brush-column index (0..4) to the corresponding edit_brush_ value (0..3).
	// Index 0 is the Select button (no brush). Returns -1 for Select.
	inline int BrushForIndex(int idx) { return idx == 0 ? -1 : idx - 1; }
	inline int IndexForBrush(int brush) { return brush + 1; }

	inline void GetButtonRect(int idx, int viewport_w, int viewport_h, int& x, int& y, int& w, int& h) {
		int colX = viewport_w - kMarginX - kBtnSize;
		int total_brush_h = kBrushCount * kBtnSize + (kBrushCount - 1) * kBtnGap;
		int brush_top = viewport_h - kMarginY - total_brush_h
		              - (2 * kSetBtnH + kSetGap + kBtnGap);  // leave room for settings rows below
		if (idx < kBrushCount) {
			w = kBtnSize; h = kBtnSize;
			x = colX;
			y = brush_top + idx * (kBtnSize + kBtnGap);
			return;
		}
		// Settings 2x2 grid below the brush column.
		int settings_top = brush_top + total_brush_h + kBtnGap;
		int halfW = (kBtnSize - kSetGap) / 2;
		int s = idx - kRadiusDec;          // 0..3
		int row = s / 2;                    // 0 = radius row, 1 = strength row
		int col = s % 2;                    // 0 = minus, 1 = plus
		w = halfW; h = kSetBtnH;
		x = colX + col * (halfW + kSetGap);
		y = settings_top + row * (kSetBtnH + kSetGap);
	}

	// Hit-test in top-down screen coords. Returns button index 0..8 or -1.
	inline int HitTest(int mx, int my, int viewport_w, int viewport_h) {
		for (int i = 0; i < kCount; ++i) {
			int x, y, w, h;
			GetButtonRect(i, viewport_w, viewport_h, x, y, w, h);
			if (mx >= x && mx <= x + w && my >= y && my <= y + h) return i;
		}
		return -1;
	}
}

/*
 Graph-node properties panel layout (left side). Shared by the renderer (draw)
 and App (hit-test), in TOP-DOWN screen coords. Shown when a graph node is
 selected while the navigation-graph overlay is visible.
*/
namespace GraphNodePanel {
	static constexpr int PX = 10, PY = 70, PW = 320;
	static constexpr int HEADER_H = 28, ROW_H = 26, BTN = 22;
	static constexpr int kNumericFields = 6;   // X, Y, Z, Gamma, Radius, Material

	enum Index {
		kXDn=0, kXUp, kYDn, kYUp, kZDn, kZUp,
		kGDn, kGUp, kRDn, kRUp, kMDn, kMUp,
		kCrDoor, kCrView, kCrStair,
		kDelete, kSave, kCount
	};

	inline void GetButtonRect(int idx, int& x, int& y, int& w, int& h) {
		const int rowsTop = PY + HEADER_H;
		if (idx >= kXDn && idx <= kMUp) {
			const int f = idx / 2; const bool up = (idx & 1);
			y = rowsTop + f * ROW_H + 2; w = BTN; h = BTN;
			x = up ? (PX + PW - BTN - 6) : (PX + PW - 2*BTN - 12);
			return;
		}
		const int critY = rowsTop + kNumericFields * ROW_H + 4;
		if (idx >= kCrDoor && idx <= kCrStair) {
			const int i = idx - kCrDoor; w = 80; h = BTN;
			x = PX + 8 + i * (w + 6); y = critY; return;
		}
		const int actY = critY + ROW_H + 8;
		if (idx == kDelete) { x = PX + 8;   y = actY; w = 140; h = BTN + 2; return; }
		if (idx == kSave)   { x = PX + 158; y = actY; w = 140; h = BTN + 2; return; }
		x = y = w = h = 0;
	}
	inline int PanelHeight() {
		return HEADER_H + kNumericFields * ROW_H + 4 + ROW_H + 8 + (BTN + 2) + 10;
	}
	inline bool InPanel(int mx, int my) {
		return mx >= PX && mx <= PX + PW && my >= PY && my <= PY + PanelHeight();
	}
	inline int HitTest(int mx, int my) {
		for (int i = 0; i < kCount; ++i) {
			int x, y, w, h; GetButtonRect(i, x, y, w, h);
			if (mx >= x && mx <= x + w && my >= y && my <= y + h) return i;
		}
		return -1;
	}
}

/*
================================================================================
 Property-panel layout — single source of truth shared by the renderer (drawing)
 and the input handler (hit-testing). All rects are in SCREEN top-down pixels
 (same coordinate space as GLUT mouse x,y). The renderer converts to GL bottom-up
 by gl_y = viewport_h - screen_y.
================================================================================
*/
namespace PropPanel {

// Layout constants (kept in one place so draw + hit-test agree exactly).
static constexpr int kLeft       = 8;
static constexpr int kTop        = 8;
static constexpr int kWidth      = 460;
static constexpr int kPad        = 8;
static constexpr int kRowH       = 16;   // fallback text line height
static constexpr int kBoxH       = 18;   // fallback editable box / slider row height
static constexpr int kPadSize    = 100;  // 2D pad square
static constexpr int kZSliderW   = 22;   // vertical Z slider width

enum class WidgetKind {
    NoteBox,       // editable note (obj.name)
    PosPad,        // 2D X/Y pad
    PosZSlider,    // vertical Z slider
    SnapGround,    // button
    SnapObject,    // button
    OriSlider,     // horizontal orientation slider (Real32x9 component)
    RgbSlider,     // horizontal RGB slider (0..1) with swatch
    NumSlider,     // horizontal numeric slider (Real/Angle/Degrees)
    NumBox,        // editable numeric input box (Int/Real X/Y/Z) — click to type, drag to scrub
    StringBox,     // editable text box (String*/VarString)
    Checkbox,      // bool8 / PushButton
    ChildHeader,   // non-interactive separator label for a child task section
    AIScriptPath,  // single-line editable: resolved .qvm file path
    AIScriptText,  // multiline editable: decompiled QSC source
    AnimIdButton,  // button: toggle play/pause for one discovered AIAction_PlayAnimation id (id stored in `comp`)
    LightmapButton, // button: "Calculate Light Mapping" (Building/EditRigidObj only)
};

struct Widget {
    WidgetKind kind;
    int x1, y1, x2, y2;   // screen top-down rect
    int fieldIndex = -1;  // schema field index (-1 for note/snap)
    int comp       = 0;   // sub-component (0=X/Alpha/R, 1=Y/Beta/G, 2=Z/Gamma/B)
    int objIndex   = -1;  // owning LevelObject index; -1 = the selected/parent object
};

struct Layout {
    int panel_x = 8, panel_y = 8, panel_w = 320, panel_h = 0;
    int row_h = kRowH;
    int box_h = kBoxH;
    std::vector<Widget> widgets;
};

// Sentinel prop_text_edit_field_ values for AI script widgets.
// Must not collide with fi*3+comp range (always >= 0 for normal fields).
static constexpr int kAIScriptPathField = -10;
static constexpr int kAIScriptTextField = -11;

// Build the layout for one task type's schema. `is_multi` types (ObjectPos /
// Real32x9 / RGB) expand to multiple sub-rows. Returns rows' y positions implicitly
// via widget rects; panel_h is the total height.
inline Layout BuildLayout(const TaskSchemaNS::TaskSchema& schema, bool is_ai = false,
                          const std::vector<std::pair<int, const TaskSchemaNS::TaskSchema*>>& children = {},
                          int animBoneHierarchy = -1,
                          const std::vector<int>& animIds = {},
                          bool showLightmapButton = false,
                          int rowH = kRowH,
                          int boxH = kBoxH) {
    using namespace TaskSchemaNS;
    Layout L;
    L.panel_x = kLeft; L.panel_y = kTop; L.panel_w = kWidth;
    L.row_h = std::max(1, rowH);
    L.box_h = std::max(1, boxH);

    rowH = L.row_h;
    boxH = L.box_h;

    int y = kTop + kPad;

    // Right-side editable box geometry (used for X/Y/Z, integer & float boxes).
    const int boxW = 110;
    const int boxX2 = kLeft + kWidth - kPad;
    const int boxX1 = boxX2 - boxW;

    // Emit every field widget for one schema. Used for BOTH the parent task and
    // each child task so they share an identical interface (pads/sliders/boxes).
    // All widgets pushed during the call are tagged with `objIndex` (-1 = parent).
    auto emitFields = [&](const TaskSchemaNS::TaskSchema& sch, bool ai, int objIndex) {
        size_t start = L.widgets.size();
        for (int fi = 0; fi < (int)sch.size(); ++fi) {
            const FieldDef& fd = sch[fi];
            const std::string& tn = fd.typeName;
            bool is_pos    = (tn == "ObjectPos");
            bool is_float3 = (tn == "Real32x3" || tn == "Real64x3");
            bool is_ori    = (tn == "Real32x9");
            bool is_rgb    = (tn == "RGB" || tn == "Colour");
            bool is_str    = (tn.find("String") != std::string::npos || tn == "VarString" ||
                              tn == "EnumString32" || tn == "DropDownCombo");
            bool is_bool   = (tn == "bool8" || tn == "PushButton");
            bool is_ro     = (tn == "Graph" || tn == "AnimData" || tn == "TrainPos1D");
            bool is_int    = (tn == "Int16" || tn == "Int32" || tn == "EnumInt32");

            y += rowH;  // field header line

            if (is_pos) {
                const int pBoxX1 = kLeft + kPad + 20;
                const int pBoxX2 = pBoxX1 + 180;
                for (int c = 0; c < 3; ++c) {
                    L.widgets.push_back({WidgetKind::NumBox, pBoxX1, y, pBoxX2, y + boxH, fi, c});
                    y += boxH + 2;
                }
                int pad_x1 = kLeft + kPad;
                int pad_y1 = y;
                L.widgets.push_back({WidgetKind::PosPad, pad_x1, pad_y1,
                                     pad_x1 + kPadSize, pad_y1 + kPadSize, fi, 0});
                int zs_x1 = pad_x1 + kPadSize + 10;
                L.widgets.push_back({WidgetKind::PosZSlider, zs_x1, pad_y1,
                                     zs_x1 + kZSliderW, pad_y1 + kPadSize, fi, 2});
                y += kPadSize + 6;
                int bw = (kWidth - 2 * kPad - 8) / 2;
                L.widgets.push_back({WidgetKind::SnapGround, kLeft + kPad, y,
                                     kLeft + kPad + bw, y + boxH, -1, 0});
                L.widgets.push_back({WidgetKind::SnapObject, kLeft + kPad + bw + 8, y,
                                     kLeft + kPad + bw + 8 + bw, y + boxH, -1, 0});
                y += boxH + 4;
                y += rowH;  // "Altitude: ... meter"
            } else if (is_float3) {
                const int pBoxX1 = kLeft + kPad + 20;
                const int pBoxX2 = pBoxX1 + 180;
                for (int c = 0; c < 3; ++c) {
                    L.widgets.push_back({WidgetKind::NumBox, pBoxX1, y, pBoxX2, y + boxH, fi, c});
                    y += boxH + 2;
                }
            } else if (is_ori) {
                int sx1 = kLeft + kPad + 64;
                int sx2 = kLeft + kWidth - kPad;
                int c_start = ai ? 2 : 0;
                for (int c = c_start; c < 3; ++c) {
                    L.widgets.push_back({WidgetKind::OriSlider, sx1, y, sx2, y + boxH, fi, c});
                    y += boxH;
                }
            } else if (is_rgb) {
                for (int c = 0; c < 3; ++c) {
                    int sx1 = kLeft + kPad + 64;
                    int sx2 = kLeft + kWidth - kPad - 24;
                    L.widgets.push_back({WidgetKind::RgbSlider, sx1, y, sx2, y + boxH, fi, c});
                    y += boxH;
                }
            } else if (is_str) {
                int h = (tn == "VarString" || tn == "String256") ? boxH * 3 : boxH;
                L.widgets.push_back({WidgetKind::StringBox, kLeft + kPad, y,
                                     kLeft + kWidth - kPad, y + h, fi, 0});
                y += h + 2;
            } else if (is_bool) {
                L.widgets.push_back({WidgetKind::Checkbox, kLeft + kPad, y,
                                     kLeft + kWidth - kPad, y + boxH, fi, 0});
                y += boxH;
            } else if (is_ro) {
                y += rowH;  // read-only grey value line, no widget
            } else if (is_int) {
                L.widgets.push_back({WidgetKind::NumBox, boxX1, y, boxX2, y + boxH, fi, 0});
                y += boxH;
            } else {
                int sx1 = kLeft + kPad;
                int sx2 = boxX1 - 8;
                L.widgets.push_back({WidgetKind::NumSlider, sx1, y, sx2, y + boxH, fi, 0});
                L.widgets.push_back({WidgetKind::NumBox, boxX1, y, boxX2, y + boxH, fi, 0});
                y += boxH;
            }
            y += 4;  // gap between fields
        }
        if (objIndex != -1)
            for (size_t i = start; i < L.widgets.size(); ++i) L.widgets[i].objIndex = objIndex;
    };

    // Parent: type header + note box + [lightmap button] + fields.
    y += rowH;            // "QTasktype: <type>"
    y += rowH;            // "QTask Note (QTaskNote):"
    L.widgets.push_back({WidgetKind::NoteBox, kLeft + kPad, y, kLeft + kWidth - kPad, y + boxH, -1, 0});
    y += boxH + 6;

    // Lightmap button immediately after the note box — always visible regardless
    // of how many child sections the object has below.
    if (showLightmapButton) {
        L.widgets.push_back({WidgetKind::LightmapButton, kLeft + kPad, y,
                             kLeft + kWidth - kPad, y + boxH, -1, 0});
        y += boxH + 4;
    }

    emitFields(schema, is_ai, -1);

    // Children: a header label, then the SAME field widgets as a parent (routed
    // to the child object via objIndex). Only present when the task has children.
    for (const auto& [childIdx, cscp] : children) {
        if (!cscp) continue;
        L.widgets.push_back({WidgetKind::ChildHeader, kLeft + kPad, y,
                             kLeft + kWidth - kPad, y + rowH, -1, 0, childIdx});
        y += rowH + 2;
        emitFields(*cscp, false, childIdx);
        y += 4;
    }

    // Animation Control section — only for HumanSoldier-family objects with a
    // resolved bone hierarchy (common/ANIMS/<NNN>.IFF). One checkbox row per
    // discovered animation id (Stand Animation + AI script + PatrolPath
    // "predefined animation" commands); checking toggles play/pause for that clip.
    if (animBoneHierarchy >= 0) {
        y += rowH;  // "Bone Hierarchy: <NNN>.IFF" label line
        int rowY = y;
        if (animIds.empty()) {
            // comp = -1 sentinel: non-interactive "no animations found" placeholder,
            // so the draw/hit-test code always has a widget to anchor the section to.
            L.widgets.push_back({WidgetKind::AnimIdButton, kLeft + kPad, rowY,
                                 kLeft + kWidth - kPad, rowY + boxH, -1, -1});
            y = rowY + boxH + 4;
        } else {
            for (size_t i = 0; i < animIds.size(); ++i) {
                int y1 = rowY + (int)i * (boxH + 4);
                L.widgets.push_back({WidgetKind::AnimIdButton, kLeft + kPad, y1,
                                     kLeft + kWidth - kPad, y1 + boxH, -1, animIds[i]});
            }
            y = rowY + (int)animIds.size() * (boxH + 4) + 2;
        }
    }

    // AI Script section — only for AI tasks (HumanSoldier, HumanAI, etc.)
    if (is_ai) {
        y += rowH;  // "AI Script Path:" label line
        L.widgets.push_back({WidgetKind::AIScriptPath,
                             kLeft + kPad, y, kLeft + kWidth - kPad, y + boxH,
                             kAIScriptPathField, 0});
        y += boxH + 6;

        y += rowH;  // "AI Script:" label line
        const int scriptH = boxH * 12;
        L.widgets.push_back({WidgetKind::AIScriptText,
                             kLeft + kPad, y, kLeft + kWidth - kPad, y + scriptH,
                             kAIScriptTextField, 0});
        y += scriptH + 6;
    }

    L.panel_h = (y + kPad) - kTop;
    return L;
}

} // namespace PropPanel


/*
================================================================================
 Renderer
================================================================================
*/
class Renderer : public IRenderResLoader {
public:

	// draw parts
	static constexpr int	DRAW_TERRAIN = FLAG_BIT(0);        // 1
	static constexpr int	DRAW_SKY = FLAG_BIT(1);            // 2
	static constexpr int	DRAW_OBJECTS = FLAG_BIT(2);        // 4
	static constexpr int	DRAW_FLAT_SKY_LAYER = FLAG_BIT(3); // 8
    static constexpr int    DRAW_BUILDINGS = FLAG_BIT(4);      // 16
    static constexpr int    DRAW_PROPS = FLAG_BIT(5);          // 32
    static constexpr int    DRAW_AI = FLAG_BIT(6);             // 64
    static constexpr int    DRAW_SKYDOME = FLAG_BIT(1);        // Alias for SKY



	struct draw_params_s {
		const view_define_s* view_define_;
		bool				overlay_wireframe_;
		int					draw_parts_;
		int					draw_terrain_options_;
		bool				flat_sky_layer_is_visible_;
		int					num_terrain_render_chunk_;
		const class LevelObjects* level_objects_;
		int					selected_object_index_;
		bool				show_magic_obj_spheres_;
		// Object indices whose rigid (static) mesh should be skipped because a live
		// skinned/animated mesh is being drawn in its place instead (nullptr/empty = none).
		// All AI with active playback are skinned-replaced simultaneously, so this can
		// hold many indices at once, not just the selected object.
		const std::unordered_set<int>* skip_static_draw_indices_ = nullptr;

		// Returns the terrain CTR node id at a world (x,y), or -1. Set by App so the
		// renderer can show the terrain id under the cursor without depending on Level. (issue 3)
		std::function<int(double,double)> terrain_id_at_world_xy_;

		// Returns the terrain surface height z at a world (x,y); false over a hole/edge.
		// Set by App so the renderer can project the 3D brush ring onto the surface.
		std::function<bool(double,double,float&)> terrain_z_at_world_xy_;
	};

	struct task_tree_view_params_s {
		bool show_hud_;
		// Alt+H toggle: gameplay HUD overlay (crosshair, health bar, weapon
		// readout). Off by default until the vanilla look is finished.
		bool hud_overlay_visible_ = false;
		std::string status_msg_;
		bool pause_mode_;
		int pause_active_input_ = -1;
		std::string pause_level_input_;
		std::string pause_search_input_;
	bool pause_terrain_expanded_ = false;
		int terrain_draw_options_ = 0;
		bool show_debug_;
		bool show_help_;
		bool edit_mode_;
		bool terrain_edit_enabled_;
		int terrain_mod_options_ = 0;
		int selected_object_index_;
		int hover_object_index_;
		int hover_tree_index_;
		int mouse_x_;
		int mouse_y_;
		int tree_scroll_offset = 0; // For scrolling the object tree
		bool tree_decl_expanded = false;
		const class LevelObjects* level_objects_;
		bool task_picker_open_;
		int task_picker_selected_idx_;
		int task_picker_scroll_offset_;
		std::string task_picker_search_;
		bool enable_camera_mode_;

		// C2: Property editor
		bool prop_editor_open_     = false;
		int  prop_field_index_     = -1;
		int  prop_text_edit_field_ = -1;
		int  prop_edit_obj_index_  = -1;  // LevelObject targeted by active text edit (-1 = parent)
		int  prop_drag_obj_index_  = -1;  // LevelObject targeted by active slider/pad drag (-1 = parent)
		std::string prop_text_buf_;
		int  prop_text_caret_      = 0;
		// AI Script editor text selection (driven by Ctrl+Shift+arrow / mouse
		// drag / Ctrl+A). prop_text_sel_focus_ < 0 OR equal to anchor means
		// no selection; otherwise the range [anchor, focus) is highlighted.
		int  prop_text_sel_anchor_ = -1;
		int  prop_text_sel_focus_  = -1;
		int  prop_panel_scroll_    = 0;  // vertical scroll offset (pixels)

		// C3: Find bar
		bool find_open_       = false;
		std::string find_query_;
		int  find_result_idx_ = -1;
		bool selected_obj_is_ai    = false;

		bool in_game_mode_ = false;
		bool noclip_mode_ = true;
		float player_health_ = 100.0f;
		float player_maximum_health_ = 100.0f;
		float player_armor_ = 100.0f;
		float player_maximum_armor_ = 100.0f;
		std::string active_weapon_name_ = "M16A2";
		uint32_t clip_ammo_ = 30;
		uint32_t clip_capacity_ = 30;
		uint32_t reserve_ammo_ = 120;
		bool map_computer_open_ = false;
		std::vector<igi::RuntimeMapComputerObjective> map_computer_objectives_;
		std::string objective_text_ = "";
		int64_t mission_timer_remaining_ticks_ = -1;
		std::vector<igi::MissionStatusMessageDisplay> mission_status_messages_;

		// Help panel (keybindings from qedkeybindings.qsc)
		int  help_scroll_offset_    = 0;
		const std::vector<std::string>* help_entries_ = nullptr;

		// Task type column toggle
		bool show_task_type_ = false;

		// Find mode: 0=name/type/id, 1=text-in-task, 2=by-id, 3=by-note
		int  find_mode_ = 0;

		// File dialog
		int         file_dialog_mode_ = 0; // 0=none,1=SaveSubTask,2=SaveSubTaskParent,3=LoadSubTask
		std::string file_dialog_path_;
		int         file_dialog_caret_ = 0;

		// Autocomplete task picker (left panel)
		bool ac_task_picker_open_   = false;
		int  ac_task_selected_idx_  = 0;
		int  ac_task_scroll_offset_ = 0;
		std::string ac_task_filter_;
		const std::vector<std::string>* ac_task_items_ = nullptr;

		// Model ID picker (right panel)
		bool model_picker_open_    = false;
		int  model_picker_selected_ = 0;
		int  model_picker_scroll_   = 0;
		std::string model_picker_filter_;
		const std::vector<ModelPickerEntry>* model_entries_ = nullptr;

		// AI Script editor state
		std::string ai_script_path_;
		std::string ai_script_text_;
		bool        ai_script_dirty_        = false;
		int         ai_script_vscroll_      = 0;
		int         ai_script_path_hscroll_ = 0;

		// Terrain brush palette / cursor ring
		int    terrain_brush_          = 0;   // 0=Raise,1=Lower,2=Soften,3=Flatten
		double terrain_brush_radius_   = 0.0; // world-space brush radius (for cursor ring scale)
		double terrain_brush_strength_ = 0.0; // brush strength (settings readout)
		// Auto-save state
		bool   auto_save_enabled_        = false;
		int    auto_save_interval_seconds_ = 300;
		bool   logging_enabled_          = false;
		int    log_level_threshold_      = 1;
		bool   music_on_                 = false; // Escape-menu Music checkbox state
		bool   lightmaps_on_             = false; // Escape-menu Lightmaps checkbox state

		// Animation state
		std::string anim_status_;           // summary text of animations playing
		bool        anim_playing_ = false;  // true if any animation is active
		bool        anim_debug_visible_ = false; // F10: show status panel + skeleton overlay

		// Property panel "Animation Control" section (selected object only).
		int              prop_anim_bone_hierarchy_ = -1; // -1 = section hidden
		std::vector<int> prop_anim_ids_;                 // discovered AIAction_PlayAnimation ids
		int              prop_anim_active_id_ = -1;      // currently loaded clip's animId (-1 = none)
		bool             prop_anim_is_playing_ = false;
		float            flash_effect_strength_ = 0.0f;
		float            muzzle_flash_strength_ = 0.0f;
		float            player_damage_effect_strength_ = 0.0f;
	};


	Renderer();
	~Renderer();

	bool					Init();
	void					Shutdown();

	void					BeginLoadLevel();
	void					SetLevel(int level) { objects_.SetLevel(level); }
	void					LoadResCache(int levelNo, const std::string& igi_path) { objects_.LoadResCache(levelNo, igi_path); }

	// Diagnostics: live object cache occupancy (for level-switch logging).
	size_t					GetMeshCacheCount() const { return objects_.GetMeshCacheCount(); }
	size_t					GetTextureCacheCount() const { return objects_.GetTextureCacheCount(); }

	// interface of IRendererLoader
	void					SetupClearColor(const glm::vec4& color) override;
	void					SetupFog(const glm::vec4& color, float fog_far) override;
	void					SetFogEnabled(bool enabled) { objects_.SetFogEnabled(enabled); }
	void					SetFogIntensity(int intensity);
	void					SetupSkydome(const skydome_define_s& d) override;

	void					LoadFlatSkyLayerTex(int layer_no, const pic_s* pic) override;
	void					LoadTerrainMatTex(const pic_s* pic) override;
	void					LoadTerrainLMPTex(const pic_s* pic) override;

	vert_flat_sky_layer_s *	MapFlatSkyLayersVB();
	void					UnmapFlatSkyLayersVB();

	vert_pos_a_uv_s*		MapTerrainVB();
	void					UnmapTerrainVB();

	uint32_t*				MapTerrainIB();
	void					UnmapTerrainIB();

	render_chunk_s*			GetTerrainRenderChunckBuffer();

	void					Draw(const draw_params_s& params, const task_tree_view_params_s& task_tree_view);

	// Navigation-graph overlay: load a specific graph .dat (graph<taskId>.dat for
	// the selected AIGraph task) for display over the 3D view, toggle visibility,
	// and query state. Returns true if the graph loaded with at least one node.
	// Node positions in the file are LOCAL to the AIGraph task's graph origin;
	// `worldOffset` is that task's world position and is added for display/pick.
	bool					LoadGraphOverlayFile(const std::string& graphFilePath,
								const glm::dvec3& worldOffset);
	void					ToggleGraphOverlay() { graph_overlay_visible_ = !graph_overlay_visible_; }
	bool					IsGraphOverlayVisible() const { return graph_overlay_visible_; }
	void					SetGraphSelected(int id) { graph_overlay_selected_ = id; }
	int						GraphSelected() const { return graph_overlay_selected_; }
	// Metadata for the loaded graph (task id + Area name from graph_level<N>.json).
	void					SetGraphOverlayMeta(const std::string& taskId, const std::string& area)
								{ graph_overlay_taskid_ = taskId; graph_overlay_area_ = area; }
	// Pick the nearest graph node to screen (mx,my) using the last frame's
	// matrices; returns the node id or -1. (mx,my) are GLUT top-left coords.
	int						PickGraphNodeAtScreen(int mx, int my, int vpW, int vpH);
	// Read / edit a node's world position (IGI units) by id. SetGraphNodePos
	// marks the overlay dirty so it can be saved back to its .dat.
	bool					GetGraphNodePos(int id, glm::dvec3& out) const;
	void					SetGraphNodePos(int id, const glm::dvec3& p);
	bool					GraphOverlayDirty() const { return graph_overlay_dirty_; }
	const std::string&		GraphOverlayTaskId() const { return graph_overlay_taskid_; }
	size_t					GraphOverlayNodeCount() const { return graph_overlay_.nodes.size(); }
	size_t					GraphOverlayEdgeCount() const { return graph_overlay_.edges.size(); }
	// Save the full edited graph back to the loaded graph<taskId>.dat.
	bool					SaveGraphOverlay();
	// Snapshot / restore the overlay graph state for undo/redo.
	GraphFile				GetGraphOverlaySnapshot() const { return graph_overlay_; }
	void					RestoreGraphOverlay(const GraphFile& snap) { graph_overlay_ = snap; graph_overlay_dirty_ = true; }
	void					SetGraphOverlayVisible(bool v) { graph_overlay_visible_ = v; }
	// Live-update the world offset used to render the graph overlay. The graph
	// nodes are stored in coords local to the AIGraph task's graph origin, so
	// moving the task in the TaskTree (or via Undo/Redo) must keep the renderer's
	// offset in sync or the 3D nodes/edges will stay at the stale position while
	// F7 is showing the graph. The caller is responsible for matching taskId.
	void					SetGraphOverlayOffset(const glm::dvec3& newOffset) { graph_overlay_offset_ = newOffset; }
	const glm::dvec3&		GraphOverlayOffset() const { return graph_overlay_offset_; }
	// Edit controls (operate on the selected node), marking the overlay dirty:
	void					ScaleSelectedGraphNode(float factor);  // radius *= factor; factor<=0 resets to 1
	int						CreateGraphNode();                     // adds a node near the selection; returns new id
	void					DeleteSelectedGraphNode();             // removes the selected node and its edges
	// Two-step link editing. First call marks the selected node as the link
	// source; second call adds/removes an edge between the source and the
	// currently selected node, then clears the source. Returns a status string
	// for the HUD (empty = nothing happened, e.g. no node selected).
	std::string				AddGraphLinkStep();
	std::string				RemoveGraphLinkStep();
	void					ClearGraphLinkSource() { graph_link_source_ = -1; }
	int						GraphLinkSource() const { return graph_link_source_; }
	// Toggle on-screen node ID labels for the graph overlay.
	void					ToggleGraphLabels() { graph_overlay_show_labels_ = !graph_overlay_show_labels_; }
	bool					GraphLabelsVisible() const { return graph_overlay_show_labels_; }
	// Fine edits for the properties panel (operate on the selected node, mark dirty):
	void					NudgeSelectedGraphNode(double dx, double dy, double dz);
	void					AdjustSelectedGraphGamma(float d);
	void					AdjustSelectedGraphRadius(float d);
	void					AdjustSelectedGraphMaterial(int d);
	void					ToggleSelectedGraphCriteria(const std::string& key);  // "DOOR"/"VIEW"/"STAIR"
	bool					GetSelectedGraphNode(GraphNode& out) const;
	// Draw the left-side properties panel for the selected node.
	void					DrawGraphNodePanel(const draw_params_s& params,
								const std::function<void(int,int,const char*,float,float,float)>& draw_text_sm);
    // Draw animated skeleton wireframe for the given bone transforms, connecting
    // each bone to its real parent (boneParents, indexed by bone ID).
    void                    DrawAnimSkeleton(const std::vector<glm::mat4>& boneWorldTransforms,
                                             const std::vector<int>& boneParents,
                                             const glm::mat4& objWorldMat);
    // CPU-skins the model's rest-pose mesh against the given (BEF) bone world
    // transforms and draws it as a solid mesh — the actual visible alternative
    // to the wireframe overlay above. boneWorldTransforms come from
    // AnimationRegistry::EvaluateWorld (BEF skeleton indexing); the model's own
    // .mef bone list is assumed to share that same indexing (true for the
    // shared character rigs — see GetIgi1HardcodedBones).
    void                    DrawSkinnedMesh(const std::string& modelId, bool isBuilding,
                                            const std::vector<glm::mat4>& boneWorldTransforms,
                                            const glm::mat4& objWorldMat);
    // True if DrawSkinnedMesh can actually render modelId (skin geometry loads and has
    // vertices/triangles). Callers must check this before skipping an object's static
    // draw in favor of the skinned one — otherwise a model whose skin geometry fails to
    // load goes permanently invisible (neither static nor skinned draw ever runs).
    bool                    HasSkinGeometry(const std::string& modelId, bool isBuilding) {
        const ParsedGeometry* geo = objects_.GetOrLoadSkinGeometry(modelId, isBuilding);
        return geo && HasRenderableSkinnedGeometry(*geo);
    }
    // Exposes the parsed bone list (REIH+MANB names/parents/rest pivots) for callers
    // that need to locate a named bone (e.g. "right hand") for weapon attachment.
    const ParsedGeometry*   GetOrLoadSkinGeometry(const std::string& modelId, bool isBuilding) {
        return objects_.GetOrLoadSkinGeometry(modelId, isBuilding);
    }
    // Draws one static prop mesh at an arbitrary world matrix in the normal scene
    // pass — used to attach a weapon mesh to a live bone transform (an AI's hand).
    void                    DrawAttachedMesh(const std::string& modelId, bool isBuilding, const glm::mat4& worldMat) {
        objects_.DrawAttachedMesh(modelId, isBuilding, worldMat, ubo_mats_);
    }
    void                    SetSplineTerrainQuery(std::function<bool(double, double, float&)> fn) { splines_.SetTerrainQuery(std::move(fn)); }
    void                    RequestSplineTrace(std::string path) { splines_.RequestTrace(std::move(path)); }
	glm::vec3				GetMeshExtents(const std::string& modelId, bool isBuilding) { return objects_.GetMeshExtents(modelId, isBuilding); }
	float					GetMeshRadius(const std::string& modelId, bool isBuilding) { return objects_.GetMeshRadius(modelId, isBuilding); }
	glm::vec3				GetMeshCenter(const std::string& modelId, bool isBuilding) { return objects_.GetMeshCenter(modelId, isBuilding); }
	float					GetMeshZOffset(const std::string& modelId, bool isBuilding) { return objects_.GetMeshZOffset(modelId, isBuilding); }
	int						PickObjectAtScreen(int x, int y, int w, int h,
											   const view_define_s& vd,
											   const std::vector<LevelObject>& objects,
											   int draw_parts,
											   int selected_object_index) {
		SetupUBOMats(vd);
		return objects_.PickObjectAtScreen(x, y, w, h, ubo_mats_, objects, draw_parts, vd.pos_, selected_object_index);
	}
	int				CountObjectVisiblePixels(const view_define_s& vd,
											 const std::vector<LevelObject>& objects,
											 int draw_parts,
											 int selected_object_index,
											 int target_object_index,
											 const std::vector<float>& scene_depth,
											 int* target_id_pixels = nullptr) {
		SetupUBOMats(vd);
		return objects_.CountObjectVisiblePixels(ubo_mats_, objects, draw_parts,
			vd.pos_, selected_object_index, target_object_index,
			vd.viewport_width_, vd.viewport_height_, scene_depth, target_id_pixels);
	}
	Renderer_Objects::VisualEvidence CaptureObjectVisualEvidence(
		const view_define_s& vd, const std::vector<LevelObject>& objects,
		int target_object_index, const std::vector<float>& scene_depth) {
		SetupUBOMats(vd);
		return objects_.CaptureObjectVisualEvidence(ubo_mats_, objects, target_object_index,
			vd.viewport_width_, vd.viewport_height_, scene_depth);
	}
	// Pass-throughs for ATTA promotion (see Renderer_Objects).
	static constexpr int kAttaPickBase = Renderer_Objects::kAttaPickBase;
	bool GetAttaPickEntry(int entry, AttaPickEntry& out) const { return objects_.GetAttaPickEntry(entry, out); }
	void SuppressAtta(const std::string& key) { objects_.SuppressAtta(key); }
	void SetLightmapForTask(const std::string& taskId, std::vector<GLuint> textures,
	                        const glm::dvec3& bakedPos, const glm::dvec3& bakedRot) {
		objects_.SetLightmapForTask(taskId, std::move(textures), bakedPos, bakedRot);
	}
	void ClearLightmapForTask(const std::string& taskId) { objects_.ClearLightmapForTask(taskId); }
	void ClearAllLightmaps() { objects_.ClearAllLightmaps(); }
	bool HasLightmapForTask(const std::string& taskId) const { return objects_.HasLightmapForTask(taskId); }
	bool IsLightmapStale(const std::string& taskId, const glm::dvec3& pos, const glm::dvec3& rot) const {
		return objects_.IsLightmapStale(taskId, pos, rot);
	}
	bool GetLightmapBakePose(const std::string& taskId, glm::dvec3& pos, glm::dvec3& rot) const {
		return objects_.GetLightmapBakePose(taskId, pos, rot);
	}
	std::string GetModelFilePath(const std::string& modelId, bool isBuilding) {
		return objects_.GetModelFilePath(modelId, isBuilding);
	}
	std::vector<AttachInfo> GetModelAttachments(const std::string& modelId, bool isBuilding) {
		return objects_.GetModelAttachments(modelId, isBuilding);
	}
	std::vector<glm::vec3> GetModelMagicVertices(const std::string& modelId, bool isBuilding) {
		return objects_.GetModelMagicVertices(modelId, isBuilding);
	}
	bool IsLadderMagicObject(const std::string& modelId) {
		return objects_.IsLadderMagicObject(modelId);
	}
	std::string GetOrExtractMefTemp(const std::string& modelId, bool isBuilding) {
		return objects_.GetOrExtractMefTemp(modelId, isBuilding);
	}
	glm::vec3 GetSunDir() const { return objects_.GetSunDir(); }
	glm::vec3 GetSunFrontColor() const { return objects_.GetSunFrontColor(); }
	glm::vec3 GetSunBackColor() const { return objects_.GetSunBackColor(); }
	glm::vec3 GetGlobalAmbient() const { return objects_.GetGlobalAmbient(); }
	void SetGlobalAmbient(const glm::vec3& amb) { objects_.SetGlobalAmbient(amb); }
	void SetLightmapsEnabled(bool enabled) { objects_.SetLightmapsEnabled(enabled); }
	void SetSunLight(const glm::vec3& dir, const glm::vec3& frontColor, const glm::vec3& backColor) {
		objects_.SetSunLight(dir, frontColor, backColor);
	}
	void SetGlobalGamma(float gamma) { objects_.SetGlobalGamma(gamma); }
	void SetIndoorAmbientForTask(const std::string& taskId, const glm::vec3& rgb) {
		objects_.SetIndoorAmbientForTask(taskId, rgb);
	}
	// RainEffect QSC task (per-level, absent on levels with no rain/snow like level2).
	// startMeters/endMeters are the "Traceline start"/"Traceline end" fields.
	void SetRainEffect(bool active, bool is_snow, float startMeters, float endMeters, float alpha) {
		rain_.SetParams(active, is_snow, startMeters, endMeters, alpha);
	}
	void ClearSuppressedAttas() { objects_.ClearSuppressedAttas(); }
	bool SuppressAttachmentInMef(const std::string& parentModelId, const std::string& attModelId, const glm::vec3& localPos) {
		return objects_.SuppressAttachmentInMef(parentModelId, attModelId, localPos);
	}
	bool AddModelToLevelRes(const std::string& modelId,
	                        const std::function<void(size_t,size_t)>& onProgress = nullptr,
	                        int sourceLevel = 0) {
		return objects_.AddModelToLevelRes(modelId, onProgress, sourceLevel);
	}
	const std::vector<ModelTextureSource>& GetModelTextureSources() const {
		return objects_.GetModelTextureSources();
	}
	// Force the mesh + textures for a model to load now (so a heavy model doesn't
	// appear to hang the editor on the next frame). No-op if already cached.
	void PreloadModel(const std::string& modelId, bool isBuilding) {
		objects_.GetOrLoadMesh(modelId, isBuilding);
	}
	bool UpdateAttaLocalPosInMef(const std::string& parentModelId, bool isBuilding, int recordIndex, const glm::vec3& newLocalPos, const glm::mat3& newLocalRot) {
		return objects_.UpdateAttaLocalPosInMef(parentModelId, isBuilding, recordIndex, newLocalPos, newLocalRot);
	}
	void MarkAttaPromotedByRecord(const std::string& parentModelId, int recordIndex) {
		objects_.MarkAttaPromotedByRecord(parentModelId, recordIndex);
	}
	static std::string AttaOccupancyKey(const std::string& modelId, const glm::vec3& worldPos) {
		return Renderer_Objects::AttaOccupancyKey(modelId, worldPos);
	}

private:


	struct ubo_mats_s {
		glm::mat4			mvp_mat_follow_view_;
		glm::mat4			mvp_flat_sky_layer_;
		glm::mat4			mvp_objects_;
	};

	struct ubo_fog_s {
		glm::vec4			color_;
		float				far_;
	};

	GLuint					ubo_mats_;
	GLuint					ubo_fog_;

	// Level-computed fog base (color/far), scaled by fog_intensity_ (0-200%, see
	// ApplyFogIntensity) to produce the effective fog pushed to the UBO/objects/skydome.
	void					ApplyFogIntensity();
	glm::vec4				fog_base_color_ = glm::vec4(0.15f, 0.15f, 0.15f, 1.0f);
	float					fog_base_far_ = 1e9f;
	int						fog_intensity_ = 200;

	Renderer_Skydome		skydome_;
	Renderer_FlatSkyLayers	flat_sky_layers_;
	Renderer_Terrain		terrain_;
	Renderer_Objects		objects_;
	Renderer_Splines		splines_;
	Renderer_Rain			rain_;
	bool                    shutdown_started_ = false;
	std::unordered_set<std::string>	logged_skinned_draws_;
	std::unordered_map<std::string, float>	skinned_pose_samples_;
	std::unordered_set<std::string>	logged_skinned_pose_updates_;

	glm::mat4				mat_proj_;
	glm::mat4				mat_view_;
	void					SetupUBOMats(const view_define_s& vd);

	// Navigation-graph overlay state.
	GraphFile				graph_overlay_;
	glm::dvec3				graph_overlay_offset_{0.0};  // AIGraph task world pos (node coords are local to it)
	std::string				graph_overlay_path_;
	std::string				graph_overlay_taskid_;       // AIGraph task id (graph<id>.dat)
	std::string				graph_overlay_area_;         // Area name from graph_level<N>.json
	bool					graph_overlay_visible_ = false;
	bool					graph_overlay_dirty_ = false;
	int						graph_overlay_selected_ = -1;
	int						graph_link_source_ = -1;        // first node of a two-step link edit (-1 = none)
	bool					graph_overlay_show_labels_ = true;  // on-screen node ID labels
	// Draw the graph overlay (nodes/edges/labels + hover/selection tooltip) using
	// the active screen-space GL state; `draw_text_sm` is the caller's label lambda
	// and (mouseX,mouseY) are GLUT top-left cursor coords for hover detection.
	void					DrawGraphOverlayInternal(const draw_params_s& params,
								const std::function<void(int,int,const char*,float,float,float)>& draw_text_sm,
								int mouseX, int mouseY);
	// Render navigation-graph nodes and edges as true 3D solid geometry (depth-tested,
	// perspective-scaled). Called in the 3D scene pass, before the 2D HUD.
	void					DrawGraphNodes3D(const draw_params_s& params);

};
