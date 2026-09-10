/******************************************************************************
 * @file    app_input_mouse.cpp
 * @brief   App input: mouse button/motion/wheel + property-drag
 *          Split from app_input.cpp; shares app_internal.h.
 *****************************************************************************/
#include "app_internal.h"
#include "renderer/object_lightmap.h"
#include "runtime/pause_menu_layout.h"
#include "runtime/pause_menu_font.h"
#include "runtime/log_policy.h"
#include "runtime/pause_menu_state.h"

void App::Input_OnMouseWheel(int wheel, int direction, int x, int y) {
	(void)wheel;
	if (in_game_mode_ && IsGameplayInputFocused() &&
		!gameplay_host_.IsGameplayWindowCurrent()) return;
	if (in_game_mode_ && IsGameplayInputFocused() && !pause_mode_) {
		// Weapon switching is disabled in play mode; only the map computer
		// consumes the wheel for zoom.
		if (gameplay_host_.GetWorld().IsMapComputerOpen()) {
			gameplay_host_.GetInputRouter().OnMouseWheel(direction);
		}
		return;
	}
	if (pause_mode_) {
		const PauseMenuFontMetrics fontMetrics = GetPauseMenuFontMetrics(
			Config::Get().useEditorFont, Config::Get().systemFontSize);
		const igi::PauseMenuLayout layout = igi::BuildPauseMenuLayout(
			window_state_.viewport_width_, window_state_.viewport_height_,
			pause_terrain_expanded_, fontMetrics.lineHeight);
		int row = 3; // Resume, mode, and collision rows precede the controls below.
		const int fontRow = row++;
		const int levelRow = row++;
		const int autosaveRow = row++;
		const int loggingRow = row++;
		const int logLevelRow = row++;
		(void)loggingRow; // The checkbox is click-only; the wheel adjusts severity only.
		row += 4; // music, lightmaps, calculate-lightmaps, terrain header
		const int terrainHeaderRow = row - 1;
		if (pause_terrain_expanded_) {
			row += 5;
		}

		auto adjust = [&](int targetRow, int labelWidth, int valueWidth,
		                  int minimum, int maximum, int step, int& value) {
			if (!igi::IsPauseMenuRowHit(layout, targetRow, y)) return false;
			const igi::PauseMenuSpinner spinner = igi::BuildPauseMenuSpinner(
				layout, igi::PauseMenuRowCenter(layout, targetRow), labelWidth, valueWidth);
			if (!igi::IsPauseMenuSpinnerHit(spinner, x)) return false;
			value = igi::AdjustPauseMenuSpinner(value, direction, minimum, maximum, step);
			return true;
		};

		char fontLabel[32];
		snprintf(fontLabel, sizeof(fontLabel), "Font: %s",
			Config::Get().useEditorFont ? "Editor" : "System");
		int fontSize = Config::Get().systemFontSize;
		if (adjust(fontRow, PauseMenuTextWidth(fontLabel, Config::Get().useEditorFont, Config::Get().systemFontSize, layout.text_scale), 44, 8, 32, 1, fontSize)) {
			Config::Get().systemFontSize = fontSize;
			Config::Save();
			return;
		}

		int level = pause_level_input_.empty() ? 1 : std::atoi(pause_level_input_.c_str());
		if (adjust(levelRow, PauseMenuTextWidth("Select Level", Config::Get().useEditorFont, Config::Get().systemFontSize, layout.text_scale), 44, 1, 14, 1, level)) {
			pause_level_input_ = std::to_string(level);
			return;
		}

		int autosaveInterval = auto_save_interval_seconds_;
		const char* autosaveLabel = auto_save_enabled_ ? "Save Enable" : "Save Disable";
		if (adjust(autosaveRow, PauseMenuTextWidth(autosaveLabel, Config::Get().useEditorFont, Config::Get().systemFontSize, layout.text_scale), 44,
		           10, 3600, 10, autosaveInterval)) {
			AdjustAutoSaveInterval(autosaveInterval - auto_save_interval_seconds_);
			return;
		}

		int logLevel = Config::Get().logLevelThreshold;
		if (adjust(logLevelRow, PauseMenuTextWidth("Log Level", Config::Get().useEditorFont, Config::Get().systemFontSize, layout.text_scale), 78,
		           igi::kLogLevelDebug, igi::kLogLevelFatal, 1, logLevel)) {
			Config::Get().logLevelThreshold = logLevel;
			Config::Get().debugLogging = logLevel == igi::kLogLevelDebug;
			Config::Save();
			return;
		}

		int fogIntensity = Config::Get().fogIntensity;
		const int fogRow = pause_terrain_expanded_ ? terrainHeaderRow + 5 : -1;
		if (pause_terrain_expanded_ &&
			adjust(fogRow, PauseMenuTextWidth("Fog Intensity", Config::Get().useEditorFont, Config::Get().systemFontSize, layout.text_scale), 56,
			       0, 1000, 100, fogIntensity)) {
			Config::Get().fogIntensity = fogIntensity;
			SetFogIntensity(fogIntensity);
			Config::Save();
		}
		return;
	}
	if (show_help_) {
		// Scroll keybindings help panel
		if (direction > 0) { if (help_scroll_offset_ > 0) help_scroll_offset_--; }
		else               { help_scroll_offset_++; }
		return;
	}
	// Over property panel: scroll it
	if (prop_editor_open_ &&
	    x >= PropPanel::kLeft && x <= PropPanel::kLeft + PropPanel::kWidth) {
		const PauseMenuFontMetrics fontMetrics = GetPauseMenuFontMetrics(
			Config::Get().useEditorFont, Config::Get().systemFontSize);
		const int kScrollStep = fontMetrics.boxHeight + 4;
		if (direction > 0) prop_panel_scroll_ = std::max(0, prop_panel_scroll_ - kScrollStep);
		else               prop_panel_scroll_ += kScrollStep;
		return;
	}
	if (show_hud_ && x < 350) { // Over TreeView
		if (direction > 0) {
			if (tree_scroll_offset_ > 0) tree_scroll_offset_--;
		} else {
			tree_scroll_offset_++;
		}
		Logger::Get().Log(LogLevel::INFO, "[App] Tree scroll offset: " + std::to_string(tree_scroll_offset_));
	}
}

void App::Input_OnMouse(int button, int state, int x, int y) {
	if (in_game_mode_ && IsGameplayInputFocused() &&
		!gameplay_host_.IsGameplayWindowCurrent()) return;

	if (in_game_mode_ && IsGameplayInputFocused() && !pause_mode_) {
		// Weapon fire/aim buttons are disabled in play mode (no weapon
		// gameplay); swallow them so nothing downstream reacts.
		if (gameplay_host_.GetWorld().IsMapComputerOpen()) {
			gameplay_host_.GetInputRouter().OnMouseButton(1, state == GLUT_DOWN);
		}
		// Keep relative-look deltas measured from the recenter point. A click
		// event may arrive at an arbitrary screen coordinate and must not create
		// a one-frame camera jump on the next motion callback.
		mouse_state_.prior_x_ = gameplay_viewport_width_ >> 1;
		mouse_state_.prior_y_ = gameplay_viewport_height_ >> 1;
		return;
	}

	// Update mouse position first so EditorProcessClick uses correct coords.
	mouse_state_.prior_x_ = x;
	mouse_state_.prior_y_ = y;

	bool enableCameraMode = Utils::IsKeyBindingPressed(Config::Get().keyEnableCamera);

	if (button == GLUT_LEFT_BUTTON) {
		if (GLUT_DOWN == state) {
			mouse_state_.left_button_down_ = true;
			// Any left-click outside the AI Script text box should drop the
			// current selection so stale highlighting doesn't linger. The
			// AIScriptText click handler re-installs the selection if the
			// user is dragging inside the text box.
			if (!pause_mode_ && !ai_text_dragging_) {
				ClearPropTextSelection();
			}

			// Graph node properties panel (left side): handle button clicks first.
			if (IsEditorInputActive() && renderer_.IsGraphOverlayVisible() && renderer_.GraphSelected() >= 0 &&
			    !enableCameraMode && GraphNodePanel::InPanel(x, y)) {
				const double POS = 256.0; const float GAM = 0.1f, RAD = 0.25f;
				if (GraphNodePanel::HitTest(x, y) >= 0) {
					// Any graph node panel action is undoable
					PushUndoState();
				}
				switch (GraphNodePanel::HitTest(x, y)) {
					case GraphNodePanel::kXDn: renderer_.NudgeSelectedGraphNode(-POS, 0, 0); break;
					case GraphNodePanel::kXUp: renderer_.NudgeSelectedGraphNode(+POS, 0, 0); break;
					case GraphNodePanel::kYDn: renderer_.NudgeSelectedGraphNode(0, -POS, 0); break;
					case GraphNodePanel::kYUp: renderer_.NudgeSelectedGraphNode(0, +POS, 0); break;
					case GraphNodePanel::kZDn: renderer_.NudgeSelectedGraphNode(0, 0, -POS); break;
					case GraphNodePanel::kZUp: renderer_.NudgeSelectedGraphNode(0, 0, +POS); break;
					case GraphNodePanel::kGDn: renderer_.AdjustSelectedGraphGamma(-GAM); break;
					case GraphNodePanel::kGUp: renderer_.AdjustSelectedGraphGamma(+GAM); break;
					case GraphNodePanel::kRDn: renderer_.AdjustSelectedGraphRadius(-RAD); break;
					case GraphNodePanel::kRUp: renderer_.AdjustSelectedGraphRadius(+RAD); break;
					case GraphNodePanel::kMDn: renderer_.AdjustSelectedGraphMaterial(-1); break;
					case GraphNodePanel::kMUp: renderer_.AdjustSelectedGraphMaterial(+1); break;
					case GraphNodePanel::kCrDoor:  renderer_.ToggleSelectedGraphCriteria("DOOR"); break;
					case GraphNodePanel::kCrView:  renderer_.ToggleSelectedGraphCriteria("VIEW"); break;
					case GraphNodePanel::kCrStair: renderer_.ToggleSelectedGraphCriteria("STAIR"); break;
					case GraphNodePanel::kDelete:  renderer_.DeleteSelectedGraphNode(); break;
					case GraphNodePanel::kSave:    SaveCurrentLevel(); status_message_ = "Graph + level saved."; break;
					default: break;
				}
				return;  // consume any click inside the panel
			}

			// Graph overlay editing: left-click on a node selects it and begins a
			// drag; clicking empty space deselects. Only intercepts the click when
			// the overlay is visible and a node is actually under the cursor, so
			// normal object/terrain selection is unaffected.
			if (IsEditorInputActive() && renderer_.IsGraphOverlayVisible() && !enableCameraMode) {
				int picked = renderer_.PickGraphNodeAtScreen(
					x, y, window_state_.viewport_width_, window_state_.viewport_height_);
				if (picked >= 0) {
					renderer_.SetGraphSelected(picked);
					show_hud_ = false;
					prop_editor_open_ = false;
					glm::dvec3 p;
					if (renderer_.GetGraphNodePos(picked, p)) {
						// Capture graph state before the drag so a move is undoable.
						PushUndoState();
						graph_node_manip_.active_  = true;
						graph_node_manip_.start_x_ = x;
						graph_node_manip_.start_y_ = y;
						graph_node_manip_.start_pos_ = p;
					}
					Logger::Get().Log(LogLevel::INFO,
						"[App] Graph node selected: " + std::to_string(picked));
					return;
				}
				renderer_.SetGraphSelected(-1);  // clicked empty space: deselect, fall through
				show_hud_ = true;
			}

			if (enableCameraMode) {
				int cx = window_state_.viewport_width_ >> 1;
				int cy = window_state_.viewport_height_ >> 1;
				int targetIdx = selected_object_index_;
				if (targetIdx < 0) targetIdx = hover_object_index_;
				if (targetIdx < 0) targetIdx = PickObjectAtScreenPos(cx, cy);
				if (targetIdx >= Renderer::kAttaPickBase) targetIdx = -1; // ATTA: no orbit target

				if (targetIdx >= 0) {
					const auto& obj = level_.GetLevelObjects().GetObjects()[targetIdx];
					orbit_active_ = true;
					orbit_target_pos_ = glm::vec3(obj.pos);
					orbit_distance_ = glm::distance(viewer_.pos_, orbit_target_pos_);
					if (orbit_distance_ < 0.1f) orbit_distance_ = 1.0f;
					Logger::Get().Log(LogLevel::INFO, "[App] Orbit mode activated around object: " + obj.type);
					Logger::Get().Log(LogLevel::WARNING, "[App] Orbit target pos: " + std::to_string(orbit_target_pos_.x) + ", " + std::to_string(orbit_target_pos_.y) + ", " + std::to_string(orbit_target_pos_.z) + " | Dist: " + std::to_string(orbit_distance_));
				} else {
					orbit_active_ = false;
				}
				return;
			}
			
			// C2: Property editor click handling.
			if (IsEditorInputActive() && prop_editor_open_ && selected_object_index_ >= 0) {
				auto& objects = level_.GetLevelObjects().GetObjects();
				if (selected_object_index_ < (int)objects.size()) {
					LevelObject& obj = objects[selected_object_index_];
					const TaskSchema* scp = GetSchema(obj.type);
					if (scp) {
						const TaskSchema& schema = *scp;
						bool is_ai = ai_model_ids_.count(obj.modelId) > 0;
						// Gather editable child task sections (same order as the renderer).
						std::vector<std::pair<int, const TaskSchema*>> children;
						for (int ci : obj.childrenIndices) {
							if (ci < 0 || ci >= (int)objects.size()) continue;
							if (objects[ci].deleted) continue;
							const TaskSchema* cscp = GetSchema(objects[ci].type);
							if (cscp && !cscp->empty()) children.push_back({ci, cscp});
						}
						int animBoneHierarchy; std::vector<int> animIds; int animActiveId; bool animIsPlaying;
						ComputePropAnimUiState(animBoneHierarchy, animIds, animActiveId, animIsPlaying);
						bool showLightmapButton = (obj.type == "Building" || obj.type == "EditRigidObj");
						const PauseMenuFontMetrics fontMetrics = GetPauseMenuFontMetrics(
							Config::Get().useEditorFont, Config::Get().systemFontSize);
						PropPanel::Layout L = PropPanel::BuildLayout(schema, is_ai, children, animBoneHierarchy, animIds, showLightmapButton,
							fontMetrics.rowHeight, fontMetrics.boxHeight);
						// Apply the same vertical scroll the renderer uses so hit-tests align.
						if (prop_panel_scroll_ > 0)
							for (auto& w : L.widgets) { w.y1 -= prop_panel_scroll_; w.y2 -= prop_panel_scroll_; }
						// The property panel is a foreground overlay: ANY click anywhere in
						// the left panel strip is consumed so background 3D objects are never
						// selected/manipulated while the editor is open.
						if (x >= L.panel_x && x <= L.panel_x + L.panel_w) {
							// +4px tolerance on all sides for pixel-perfect feel at any DPI
							auto inRect = [&](const PropPanel::Widget& w) {
								return x >= w.x1 - 4 && x <= w.x2 + 4 && y >= w.y1 - 4 && y <= w.y2 + 4;
							};
							// Commit any in-progress text edit before switching focus.
							CommitPropTextEdit();
							for (const auto& w : L.widgets) {
								if (!inRect(w)) continue;
								using K = PropPanel::WidgetKind;
								if (w.kind == K::ChildHeader) { return; } // separator: not interactive

								// Resolve the widget's target object: the selected parent, or
								// one of its child tasks (w.objIndex). Edits/drags route there so
								// children get the identical interface.
								int tIdx = (w.objIndex >= 0) ? w.objIndex : selected_object_index_;
								if (tIdx < 0 || tIdx >= (int)objects.size()) return;
								LevelObject& tobj = objects[tIdx];
								const TaskSchema* tscp = (tIdx == selected_object_index_) ? scp : GetSchema(tobj.type);
								if (!tscp) return;
								const TaskSchema& tsch = *tscp;
								auto fieldArg = [&](int fidx, int comp) -> int {
									return (fidx >= 0 && fidx < (int)tsch.size()) ? tsch[fidx].argOffset + comp : -1;
								};
								auto tTok = [&](int idx) -> float {
									if (idx >= 0 && idx < (int)tobj.argTokens.size()) {
										try { return std::stof(tobj.argTokens[idx]); } catch(...) {}
									}
									return 0.f;
								};

								if (w.kind == K::AnimIdButton) {
									if (w.comp >= 0) ToggleAnimationForObject(selected_object_index_, w.comp);
									return;
								} else if (w.kind == K::LightmapButton) {
									// Resolve + apply this object's baked lightmap. Moving the
									// object afterward adjusts it live, so there's no separate
									// "recalculate" action — the game-facing .olm rewrite for
									// moved objects happens automatically on Save.
									CalculateLightmapForSelectedObject();
									return;
								} else if (w.kind == K::NoteBox) {
									prop_edit_obj_index_ = tIdx;
									prop_text_edit_field_ = -2; // sentinel: editing note
									prop_text_buf_ = tobj.name;
									prop_text_caret_ = (int)prop_text_buf_.size();
								} else if (w.kind == K::SnapGround || w.kind == K::SnapObject) {
									// Snap acts on the selected object's marker manipulation.
									if (tIdx == selected_object_index_) {
										int mk = (w.kind == K::SnapGround) ? MK_MANIP_S : MK_MANIP_O;
										PushUndoState();
										input_.keys_ |= mk;
										UpdateMarkerManipulation();
										input_.keys_ &= ~mk;
										mouse_state_.left_button_down_ = false; // prevent drag overwriting snap
									}
								} else if (w.kind == K::StringBox) {
									prop_edit_obj_index_ = tIdx;
									prop_text_edit_field_ = w.fieldIndex * 3 + w.comp;
									int argIdx = fieldArg(w.fieldIndex, w.comp);
									prop_text_buf_ = (argIdx >= 0 && argIdx < (int)tobj.argTokens.size()) ? StripQuotes(tobj.argTokens[argIdx]) : "";
									prop_text_caret_ = (int)prop_text_buf_.size();
								} else if (w.kind == K::NumBox) {
									// Click to type AND arm scrub-drag (motion will scrub).
									prop_edit_obj_index_ = tIdx;
									prop_text_edit_field_ = w.fieldIndex * 3 + w.comp;
									int argIdx = fieldArg(w.fieldIndex, w.comp);
									prop_text_buf_ = (argIdx >= 0 && argIdx < (int)tobj.argTokens.size()) ? tobj.argTokens[argIdx] : "";
									prop_text_caret_ = (int)prop_text_buf_.size();
									prop_drag_obj_index_ = tIdx;
									prop_field_index_  = w.fieldIndex * 3 + w.comp;
									prop_drag_start_x_ = x;
									prop_drag_start_y_ = y;
									PushUndoState();
									prop_drag_start_val_ = tTok(argIdx);
								} else if (w.kind == K::Checkbox) {
									int argIdx = fieldArg(w.fieldIndex, w.comp);
									if (argIdx >= 0 && argIdx < (int)tobj.argTokens.size()) {
										int cur = 0; try { cur = std::stoi(tobj.argTokens[argIdx]); } catch(...) {}
										tobj.argTokens[argIdx] = (cur == 0) ? "1" : "0";
										tobj.modified = true;
										level_.GetLevelObjects().UpdateCoordinatesInLine(tobj);
									}
								} else if (w.kind == K::PosPad) {
									// drag X and Y simultaneously — store field, use comp=0 marker
									prop_drag_obj_index_ = tIdx;
									prop_field_index_  = w.fieldIndex * 3 + 0;
									prop_drag_start_x_ = x;
									prop_drag_start_y_ = y;
									PushUndoState();
									int off = fieldArg(w.fieldIndex, 0);
									prop_drag_start_val_  = tTok(off + 0);
									prop_drag_start_val2_ = tTok(off + 1);
								} else if (w.kind == K::AIScriptPath) {
									prop_edit_obj_index_ = -1;
									prop_text_edit_field_ = PropPanel::kAIScriptPathField;
									prop_text_buf_ = ai_script_path_;
									{
										int cc = std::max(0, (x - w.x1 - 3) / 7);
										prop_text_caret_ = std::min((int)prop_text_buf_.size(),
										                            ai_script_path_hscroll_ + cc);
									}
								} else if (w.kind == K::AIScriptText) {
									prop_edit_obj_index_ = -1;
									prop_text_edit_field_ = PropPanel::kAIScriptTextField;
									prop_text_buf_ = ai_script_text_;
									{
										const int char_width = GetPauseMenuFontMetrics(
											Config::Get().useEditorFont, Config::Get().systemFontSize).charWidth;
										const int mc = AiScriptMaxChars(char_width);
										int click_row = std::max(0, (y - w.y1) / L.box_h);
										int click_col = std::max(0, (x - w.x1 - 3) / char_width);
										int abs_line  = ai_script_vscroll_ + click_row;
										auto lstarts  = AiTextLineStarts(ai_script_text_, mc);
										abs_line = std::max(0, std::min(abs_line, (int)lstarts.size() - 1));
										int ls   = lstarts[abs_line];
										int next = (abs_line + 1 < (int)lstarts.size()) ? lstarts[abs_line + 1] : (int)ai_script_text_.size();
										int len  = next - ls;
										if (len > 0 && (ls + len) <= (int)ai_script_text_.size() &&
										    ai_script_text_[ls + len - 1] == '\n') --len;
										prop_text_caret_ = ls + std::min(click_col, len);
										// Click in the AI Script text starts a possible
										// drag-selection. Shift+click extends from the
										// existing anchor (or the caret if no anchor).
										const bool shiftDown = (glutGetModifiers() & GLUT_ACTIVE_SHIFT) != 0;
										if (shiftDown && prop_text_sel_anchor_ < 0) {
											prop_text_sel_anchor_ = prop_text_caret_;
										} else if (!shiftDown) {
											prop_text_sel_anchor_ = prop_text_caret_;
											prop_text_sel_focus_  = -1;
										}
										ai_text_dragging_  = true;
										// Stash the box geometry so Input_OnMotion can map
										// subsequent cursor positions back to caret indices
										// for drag-selection.
										ai_text_box_x1_ = w.x1;
										ai_text_box_y1_ = w.y1;
										ai_text_box_w_  = w.x2 - w.x1;
										ai_text_box_h_  = w.y2 - w.y1;
									}
								} else {
									// PosZSlider / OriSlider / RgbSlider / NumSlider — single-value drag
									prop_drag_obj_index_ = tIdx;
									prop_field_index_  = w.fieldIndex * 3 + w.comp;
									prop_drag_start_x_ = x;
									prop_drag_start_y_ = y;
									PushUndoState();
									prop_drag_start_val_ = tTok(fieldArg(w.fieldIndex, w.comp));
								}
								return;
							}
								return; // consume any click in the panel strip (foreground overlay)
						}
					}
				}
			}

			if (pause_mode_) {
				// *** Layout MUST match renderer_draw.cpp pause menu exactly ***
				const PauseMenuFontMetrics fontMetrics = GetPauseMenuFontMetrics(
					Config::Get().useEditorFont, Config::Get().systemFontSize);
				const igi::PauseMenuLayout pause_layout = igi::BuildPauseMenuLayout(
					window_state_.viewport_width_, window_state_.viewport_height_,
					pause_terrain_expanded_, fontMetrics.lineHeight);
				const int menu_x = pause_layout.menu_left;

				if (x >= menu_x && x <= menu_x + pause_layout.menu_width &&
				    y >= pause_layout.menu_top && y <= pause_layout.menu_top + pause_layout.menu_height) {
					mouse_state_.left_button_down_ = false;
					int clicked_input = -1;

					int btn_idx = 0;
int RESUME_ROW = btn_idx++;
				int MODE_ROW = btn_idx++;
				int CLIP_ROW = btn_idx++;
				int FONT_ROW = btn_idx++;
					int LEVEL_ROW = btn_idx++;
					int AUTOSAVE_ROW = btn_idx++;
					int LOGGING_ROW = btn_idx++;
					int LOG_LEVEL_ROW = btn_idx++;
					int MUSIC_ROW = btn_idx++;
					int LIGHTMAPS_ROW = btn_idx++;
					int LIGHTMAPS_CALC_ROW = btn_idx++;
					int TERRAIN_HEADER_ROW = btn_idx++;
				int TERRAIN_TEX_ROW = -1, TERRAIN_HGT_ROW = -1, TERRAIN_DSC_ROW = -1, TERRAIN_FOG_ROW = -1, TERRAIN_FOGINT_ROW = -1;
				if (pause_terrain_expanded_) {
					TERRAIN_TEX_ROW = btn_idx++;
					TERRAIN_HGT_ROW = btn_idx++;
					TERRAIN_DSC_ROW = btn_idx++;
					TERRAIN_FOG_ROW = btn_idx++;
					TERRAIN_FOGINT_ROW = btn_idx++;
				}
					int RESET_ROW = btn_idx++;
					int SAVE_ROW = btn_idx++;
					int QUIT_ROW = btn_idx++;

					auto btn_hit2 = [&](int idx) -> bool {
						return igi::IsPauseMenuRowHit(pause_layout, idx, y);
					};

					if      (btn_hit2(RESUME_ROW)) { TogglePauseMenu(); }
else if (btn_hit2(MODE_ROW))   { ToggleGamePlayMode(); TogglePauseMenu(); }
				else if (btn_hit2(CLIP_ROW))  { noclip_mode_ = !noclip_mode_; }
				else if (btn_hit2(FONT_ROW)) {
						// Layout MUST match renderer: dynamic label width, val_w=44
						char font_lbl[32];
						snprintf(font_lbl, sizeof(font_lbl), "Font: %s",
								 Config::Get().useEditorFont ? "Editor" : "System");
						const igi::PauseMenuSpinner spinner = igi::BuildPauseMenuSpinner(
							pause_layout, igi::PauseMenuRowCenter(pause_layout, FONT_ROW),
							PauseMenuTextWidth(font_lbl, Config::Get().useEditorFont,
								Config::Get().systemFontSize, pause_layout.text_scale), 44);
						int& fs = Config::Get().systemFontSize;
						if (x >= spinner.minus_left && x < spinner.minus_left + 22) {
							fs = std::max(8, fs - 1); Config::Save();
						} else if (x >= spinner.plus_left && x < spinner.plus_left + 22) {
							fs = std::min(32, fs + 1); Config::Save();
						} else if (x < spinner.minus_left) {
							Config::Get().useEditorFont = !Config::Get().useEditorFont; Config::Save();
						}
					}
					else if (btn_hit2(LEVEL_ROW)) {
						clicked_input = igi::kPauseMenuLevelRow;
						// Layout MUST match renderer: dynamic label width, val_w=44
						const char* lbl = "Select Level";
						const igi::PauseMenuSpinner spinner = igi::BuildPauseMenuSpinner(
							pause_layout, igi::PauseMenuRowCenter(pause_layout, LEVEL_ROW),
							PauseMenuTextWidth(lbl, Config::Get().useEditorFont,
								Config::Get().systemFontSize, pause_layout.text_scale), 44);
						int cur = pause_level_input_.empty() ? 1 : std::atoi(pause_level_input_.c_str());
						if (x >= spinner.minus_left && x < spinner.minus_left + 22) {
							cur = (cur > 1) ? cur - 1 : 1;
							pause_level_input_ = std::to_string(cur);
						} else if (x >= spinner.plus_left && x < spinner.plus_left + 22) {
							cur = (cur < 14) ? cur + 1 : 14;
							pause_level_input_ = std::to_string(cur);
						}
					}
					else if (btn_hit2(AUTOSAVE_ROW)) {
						// Layout MUST match renderer: dynamic label width, val_w=44
						const char* lbl = auto_save_enabled_
								 ? "Save Enable" : "Save Disable";
						const igi::PauseMenuSpinner spinner = igi::BuildPauseMenuSpinner(
							pause_layout, igi::PauseMenuRowCenter(pause_layout, AUTOSAVE_ROW),
							PauseMenuTextWidth(lbl, Config::Get().useEditorFont,
								Config::Get().systemFontSize, pause_layout.text_scale), 44);
						if      (x >= spinner.minus_left && x < spinner.minus_left + 22) AdjustAutoSaveInterval(-10);
						else if (x >= spinner.plus_left  && x < spinner.plus_left  + 22) AdjustAutoSaveInterval(10);
						else if (x >= spinner.group_left && x < spinner.minus_left) ToggleAutoSave();
					}
					else if (btn_hit2(LOGGING_ROW)) {
						Config::Get().enableLogging = !Config::Get().enableLogging;
						Config::Save();
						status_message_ = Config::Get().enableLogging ? "Logging: ON" : "Logging: OFF";
					}
					else if (btn_hit2(LOG_LEVEL_ROW)) {
						const igi::PauseMenuSpinner spinner = igi::BuildPauseMenuSpinner(
							pause_layout, igi::PauseMenuRowCenter(pause_layout, LOG_LEVEL_ROW),
							PauseMenuTextWidth("Log Level", Config::Get().useEditorFont,
								Config::Get().systemFontSize, pause_layout.text_scale), 78);
						int& level = Config::Get().logLevelThreshold;
						if (x >= spinner.minus_left && x < spinner.minus_left + 22) {
							level = std::max(igi::kLogLevelDebug, level - 1);
						} else if (x >= spinner.plus_left && x < spinner.plus_left + 22) {
							level = std::min(igi::kLogLevelFatal, level + 1);
						} else {
							level = igi::ClampLogLevel(level);
						}
						Config::Get().debugLogging = level == igi::kLogLevelDebug;
						Config::Save();
						status_message_ = std::string("Log level: ") + igi::LogLevelLabel(level);
					}
					else if (btn_hit2(MUSIC_ROW)) { ToggleMusic(); }
					else if (btn_hit2(LIGHTMAPS_ROW)) { igi::ObjectLightmapManager::Get().CycleRenderMode(); }
					else if (btn_hit2(LIGHTMAPS_CALC_ROW)) {
						CalculateLightmapsForAllObjects();
					}
					else if (btn_hit2(TERRAIN_HEADER_ROW)) { pause_terrain_expanded_ = !pause_terrain_expanded_; }
					else if (pause_terrain_expanded_ && btn_hit2(TERRAIN_TEX_ROW)) { ToggleTerrainModOption(1); }
					else if (pause_terrain_expanded_ && btn_hit2(TERRAIN_HGT_ROW)) { ToggleTerrainModOption(2); }
					else if (pause_terrain_expanded_ && btn_hit2(TERRAIN_DSC_ROW)) { ToggleTerrainModOption(4); }
					else if (pause_terrain_expanded_ && btn_hit2(TERRAIN_FOG_ROW)) {
						ToggleTerrainDrawOption(Renderer_Terrain::DRAW_TERRAIN_OPT_FOG);
						int dOpts = GetTerrainDrawOptions();
						bool fogOn = (dOpts & Renderer_Terrain::DRAW_TERRAIN_OPT_FOG) != 0;
						SetFogEnabled(fogOn);
						Config::Get().enableFog = fogOn;
						Config::Save();
					}
					else if (pause_terrain_expanded_ && btn_hit2(TERRAIN_FOGINT_ROW)) {
						// Layout MUST match renderer: dynamic label width, val_w=56
						const char* lbl = "Fog Intensity";
						const igi::PauseMenuSpinner spinner = igi::BuildPauseMenuSpinner(
							pause_layout, igi::PauseMenuRowCenter(pause_layout, TERRAIN_FOGINT_ROW),
							PauseMenuTextWidth(lbl, Config::Get().useEditorFont,
								Config::Get().systemFontSize, pause_layout.text_scale), 56);
						int& fi = Config::Get().fogIntensity;
						if (x >= spinner.minus_left && x < spinner.minus_left + 22) {
							fi = std::max(0, fi - 100);
							SetFogIntensity(fi); Config::Save();
						} else if (x >= spinner.plus_left && x < spinner.plus_left + 22) {
							fi = std::min(1000, fi + 100);
							SetFogIntensity(fi); Config::Save();
						}
					}
					else if (btn_hit2(RESET_ROW)) { ResetLevel(); TogglePauseMenu(); }
					else if (btn_hit2(SAVE_ROW)) { SaveCurrentLevel(); }
					else if (btn_hit2(QUIT_ROW)) { glutLeaveMainLoop(); }

					pause_active_input_ = clicked_input;
				} else {
					pause_active_input_ = -1; // Clicked outside menu
				}
				return; // Block all other interactions while paused
			}

			// Priority: on-screen terrain brush palette (bottom-right). Consume click so
			// it does not also sculpt terrain / pick an object this frame.
			if (TerrainPaletteClick(x, y)) {
				mouse_state_.left_button_down_ = false; // stop Frame() from sculpting while held
				return;
			}

			// Task picker mouse click: left-click on an item selects it
			if (task_picker_open_) {
				const int picker_x = 20;
				const int picker_w = 520;
				const int picker_items_top_y = 50; // items start below header (screen coords)
				const int row_h = CurrentUiRowHeight();

				if (x >= picker_x && x <= picker_x + picker_w && y >= picker_items_top_y) {
					int row = (y - picker_items_top_y) / row_h;
					int target_idx = task_picker_scroll_offset_ + row;

					// Build picker list (same logic as Input_OnSpecial picker handling)
					auto& objects = level_.GetLevelObjects().GetObjects();
					std::vector<int> picker_to_objects;
					std::string search_lower = task_picker_search_;
					std::transform(search_lower.begin(), search_lower.end(), search_lower.begin(),
					               [](unsigned char c) { return std::tolower(c); });
					std::set<std::string> seen_types;
					for (int i = 0; i < (int)objects.size(); ++i) {
						if (!objects[i].deleted) {
							const auto& obj = objects[i];
							if (obj.type == "Task_DeclareParameters") continue; // picker excludes declare params
							std::string type_lower = obj.type;
							std::transform(type_lower.begin(), type_lower.end(), type_lower.begin(),
							               [](unsigned char c) { return std::tolower(c); });
							if (seen_types.count(type_lower) > 0) continue;
							seen_types.insert(type_lower);
							std::string label = obj.type + "()";
							std::string label_lower = label;
							std::transform(label_lower.begin(), label_lower.end(), label_lower.begin(),
							               [](unsigned char c) { return std::tolower(c); });
							if (search_lower.empty() || label_lower.find(search_lower) != std::string::npos)
								picker_to_objects.push_back(i);
						}
					}

					if (target_idx >= 0 && target_idx < (int)picker_to_objects.size()) {
						task_picker_selected_idx_ = target_idx;
					}
					mouse_state_.left_button_down_ = false;
					return;
				}
			}

			// Model picker mouse click: keep the selected index in the same filtered
			// list that Enter will commit. Consume the whole panel so a click cannot
			// select or manipulate a background object while the picker is open.
			if (model_picker_open_) {
				const int picker_w = 280;
				const int picker_x = window_state_.viewport_width_ - picker_w;
				if (x >= picker_x && x <= window_state_.viewport_width_) {
					const int selected = ModelPickerSelectionAt(
						level_model_entries_, model_picker_filter_, x, y,
						window_state_.viewport_width_, window_state_.viewport_height_,
						model_picker_scroll_, CurrentUiRowHeight());
					if (selected >= 0) model_picker_selected_ = selected;
					mouse_state_.left_button_down_ = false;
					return;
				}
			}

			// Priority: TreeView HUD interaction
			if (show_hud_ && x < 350 && !enableCameraMode) { // Tree is on the left
				ProcessTreeViewClick(x, y);

			}
			else if (edit_mode_ && !enableCameraMode) {
				if (terrain_edit_enabled_) {
					// Left-click while terrain edit is open: exit terrain mode if an object is
					// clicked, otherwise EditorProcessClick sculpts terrain at cursor.
					int picked = PickObjectAtScreenPos(x, y);
					if (picked >= 0) {
						SetTerrainEditEnabled(false);
					}
				}
				EditorProcessClick();
				// Update manipulation data AFTER selection, so we get the newly selected object
				marker_manip_.start_x_ = x;
				marker_manip_.start_y_ = y;
				if (selected_object_index_ >= 0) {
					auto& obj = level_.GetLevelObjects().GetObjects()[selected_object_index_];
					marker_manip_.start_pos_ = obj.pos;
					marker_manip_.start_rot_ = obj.rot;
				}
			}
		}
		else if (GLUT_UP == state) {
			mouse_state_.left_button_down_ = false;
			undo_state_pushed_for_manip_ = false; // reset undo guard for next click-drag

			// Auto-recalc lightmap if the user dragged an object and it actually moved.
			if (edit_dragging_ && selected_object_index_ >= 0) {
				auto& objects = level_.GetLevelObjects().GetObjects();
				if (selected_object_index_ < (int)objects.size()) {
					const auto& obj = objects[selected_object_index_];
					bool movedPos = glm::length(obj.pos - marker_manip_.start_pos_) > 0.5;
					bool movedRot = glm::length(obj.rot - glm::dvec3(marker_manip_.start_rot_)) > 0.01;
					if (movedPos || movedRot)
						AutoRecalcLightmapForManipulated(selected_object_index_);
				}
			}

			edit_dragging_ = false;
			orbit_active_ = false;
			graph_node_manip_.active_ = false;
			prop_field_index_ = -1; // C2: stop dragging property field
			prop_drag_obj_index_ = -1;
			prop_drag_speed_ = 0.f;
			prop_last_drag_dx_ = 0;
			prop_last_drag_dy_ = 0;
			// End AI Script editor drag-selection: commit the selection
			// range (anchor / focus already populated) and stop tracking the
			// cursor. The next click in the text box will start a new range.
			ai_text_dragging_ = false;
			status_message_.clear(); // Clear movement telemetry status when mouse is released

			if (window_state_.cursor_visible_) {
				input_.mouse_delta_x_ = 0;
				input_.mouse_delta_y_ = 0;
			}
		}
	}

	// Right-click: open property editor for the object under cursor, or sculpt terrain.
	if (button == GLUT_RIGHT_BUTTON) {
		if (state == GLUT_DOWN) {
			mouse_state_.right_button_down_ = true;
			if (!pause_mode_ && !enableCameraMode) {
				int target = hover_object_index_;
				if (target >= 0) {
					if (terrain_edit_enabled_) {
						SetTerrainEditEnabled(false);
					}
					selected_object_index_ = target;
					prop_editor_open_ = true; prop_panel_scroll_ = 0; prop_text_edit_field_ = -1; prop_edit_obj_index_ = -1;
					LoadAIScriptForSelected();
				} else {
					prop_editor_open_ = false;
					// Right-click on empty space: open terrain editor (hold to sculpt)
					if (!terrain_edit_enabled_) {
						SetTerrainEditEnabled(true);
					}
				}
			}
		} else if (state == GLUT_UP) {
			mouse_state_.right_button_down_ = false;
		}
	}

	// Update cursor instantly on click/release — keep NONE if SPR cursor is active
	if (window_state_.cursor_visible_ && !pause_mode_) {
		glutSetCursor(GLUT_CURSOR_NONE);
	}

}

void App::Input_OnMotion(int x, int y) {
	if (in_game_mode_ && IsGameplayInputFocused() &&
		!gameplay_host_.IsGameplayWindowCurrent()) return;
	int dx = x - mouse_state_.prior_x_;
	int dy = y - mouse_state_.prior_y_;

	if (in_game_mode_ && IsGameplayInputFocused() && !pause_mode_) {
		gameplay_host_.GetInputRouter().OnMouseMove(static_cast<float>(dx), static_cast<float>(dy));

		// Gameplay uses relative mouse look. Re-centering after each delivered
		// motion event prevents the OS cursor from reaching a window edge and
		// keeps long turns continuous, matching the first-person runtime contract.
		const int center_x = gameplay_viewport_width_ >> 1;
		const int center_y = gameplay_viewport_height_ >> 1;
		mouse_state_.prior_x_ = center_x;
		mouse_state_.prior_y_ = center_y;
		if (dx != 0 || dy != 0) {
			glutWarpPointer(center_x, center_y);
		}
		return;
	}
	if (igi::IsPauseMenuInputActive(pause_mode_, IsGameplayInputFocused())) {
		// Paused menus keep pointer hover but never feed motion into editor or
		// gameplay tools behind the modal overlay.
		mouse_state_.prior_x_ = x;
		mouse_state_.prior_y_ = y;
		return;
	}

	bool enableCameraMode = Utils::IsKeyBindingPressed(Config::Get().keyEnableCamera);
	if (enableCameraMode && (dx != 0 || dy != 0))
		camera_mode_moved_ = true;

	// Always update mouse coordinates for hover tooltip/interaction
	mouse_state_.prior_x_ = x;
	mouse_state_.prior_y_ = y;

	// AI Script editor drag-selection: while the left button is down inside
	// the text box, move the focus end of the selection with the cursor.
	// Caret is updated in tandem so the next typed character lands where the
	// cursor is.
	if (ai_text_dragging_ && prop_text_edit_field_ == PropPanel::kAIScriptTextField
	    && mouse_state_.left_button_down_) {
		const int local_x = x - ai_text_box_x1_;
		const int local_y = y - ai_text_box_y1_;
		const PauseMenuFontMetrics fontMetrics = GetPauseMenuFontMetrics(
			Config::Get().useEditorFont, Config::Get().systemFontSize);
		const int row_h   = fontMetrics.boxHeight;
		prop_text_caret_ = AiScriptPixelToCaret(prop_text_buf_,
		                                         local_x - 3, local_y,
		                                         ai_script_vscroll_,
		                                         ai_text_box_h_, AiScriptMaxChars(fontMetrics.charWidth), row_h,
		                                         fontMetrics.charWidth);
		prop_text_sel_focus_ = prop_text_caret_;
		// Auto-scroll vertically if the drag moves above/below the box.
		if (local_y < 0)
			ai_script_vscroll_ = std::max(0, ai_script_vscroll_ - 1);
		else if (local_y > ai_text_box_h_)
			ai_script_vscroll_ = ai_script_vscroll_ + 1;
		return; // consume the motion — don't pass to camera/treeview
	}

	// Priority 1: TreeView Hover
	if (show_hud_ && x < 350 && !enableCameraMode) {
		hover_tree_index_ = -1; // Reset before check
		ProcessTreeViewHover(x, y);
		hover_object_index_ = -1; // Suppress 3D tooltips while over UI
	} else {
		hover_tree_index_ = -1;
		// Priority 2: 3D Object Hover — deferred to Frame() to avoid blocking GPU sync on every mouse event
	}

	if (window_state_.cursor_visible_) {
		glutSetCursor(GLUT_CURSOR_NONE);
	}

	if (window_state_.cursor_visible_ && !enableCameraMode) {
		if (mouse_state_.left_button_down_) {
			input_.mouse_delta_x_ = dx;
			input_.mouse_delta_y_ = dy;

			// Graph-node drag takes priority over object manipulation when active.
			if (graph_node_manip_.active_ && renderer_.GraphSelected() >= 0) {
				UpdateGraphNodeManipulation(x, y);
			}
			else if (edit_mode_ && selected_object_index_ >= 0 && !terrain_edit_enabled_) {
				UpdateMarkerManipulation();
			}
		}
	}
	else {
		if (skip_input_on_motion_once_) {
			skip_input_on_motion_once_ = false;
			return;
		}

		int center_x = window_state_.viewport_width_ >> 1;
		int center_y = window_state_.viewport_height_ >> 1;

		input_.mouse_delta_x_ += x - center_x;
		input_.mouse_delta_y_ += y - center_y;

		mouse_state_.prior_x_ = x;
		mouse_state_.prior_y_ = y;

		glutWarpPointer(center_x, center_y); // move cursor to view center
		skip_input_on_motion_once_ = true;   // skip next cursor motion event caused by glutWarpPointer
	}

	if (edit_dragging_) {
		int box_x = (window_state_.viewport_width_ - edit_box_w_) / 2;
		int rel_x = x - (box_x + 20);
		int char_w = 9;
		edit_cursor_pos_ = std::max(0, std::min((int)edit_string_.size(), edit_scroll_x_ + (rel_x / char_w)));
		edit_selection_end_ = edit_cursor_pos_;
	}

	// C2: Property editor drag (2D pad / Z slider / orientation+numeric sliders)
	if (prop_field_index_ >= 0 && !enableCameraMode && selected_object_index_ >= 0) {
		auto& objects = level_.GetLevelObjects().GetObjects();
		// The drag may target the selected parent or one of its child tasks.
		int dragIdx = (prop_drag_obj_index_ >= 0) ? prop_drag_obj_index_ : selected_object_index_;
		if (dragIdx >= 0 && dragIdx < (int)objects.size()) {
			auto& obj = objects[dragIdx];
			const TaskSchema* scp = GetSchema(obj.type);
			if (scp) {
				const TaskSchema& schema = *scp;
				int fi   = prop_field_index_ / 3;
				int comp = prop_field_index_ % 3;
				// A real drag cancels any pending NumBox text-edit (scrub wins).
				if ((std::abs(x - prop_drag_start_x_) > 2 || std::abs(y - prop_drag_start_y_) > 2) &&
				    prop_text_edit_field_ == prop_field_index_) {
					prop_text_edit_field_ = -1;
				}
				if (fi < (int)schema.size()) {
					const FieldDef& fd = schema[fi];
					const std::string& tn = fd.typeName;
					bool is_pos = (tn == "ObjectPos"); // only actual position type gets pad/camera-follow
					bool is_ori = (tn == "Real32x9");
						int dxp = x - prop_drag_start_x_;

						if (is_pos) {
							// Position pad / Z slider use a per-frame velocity model
							// (ApplyPropPositionDrag) so the object accelerates while held in a
							// direction and keeps moving even when the cursor is pinned at the
							// window edge. Raw motion events do nothing for position here.
						} else {
						glm::dvec3 oldPos = obj.pos;
						glm::dvec3 oldRot = obj.rot;
						// Orientation / RGB / numeric horizontal slider or NumBox scrub.
						bool isRgb = (tn == "RGB" || tn == "Colour");
						bool isInt = (tn == "Int16" || tn == "Int32" || tn == "EnumInt32");
						float sens = is_ori ? 0.01f : (isRgb ? 0.005f : (isInt ? 0.1f : 0.05f));
						float nv = prop_drag_start_val_ + dxp * sens;
						if (isRgb) nv = std::max(0.f, std::min(1.f, nv));
						int argIdx = fd.argOffset + comp;
						if (argIdx >= 0 && argIdx < (int)obj.argTokens.size()) {
							char buf[64];
							if (isInt) snprintf(buf, sizeof(buf), "%d", (int)std::lround(nv));
							else       snprintf(buf, sizeof(buf), "%.6f", nv);
							obj.argTokens[argIdx] = buf;
						}
						if (is_ori) { // mirror to obj.rot (Alpha/Beta/Gamma -> x/y/z)
							if      (comp == 0) obj.rot.x = nv;
							else if (comp == 1) obj.rot.y = nv;
							else                obj.rot.z = nv;
						}
						// Sync single-rotation Real32 fields (Gamma/Heading) to obj.rot.z so
						// UpdateCoordinatesInLine doesn't overwrite argTokens with stale obj.rot.
						// Gamma/Heading are stored as Angle/Degrees/Real32 — any of these
						// must drive obj.rot.z so the 3D object actually turns when dragged.
						bool isSingleRot = ((tn == "Real32" || tn == "Angle" || tn == "Degrees") &&
						                    (fd.name == "Gamma" || fd.name == "Heading"));
						if (isSingleRot) obj.rot.z = (double)nv;
						obj.modified = true;
						if (is_ori) {
							glm::dmat3 deltaWorld = BuildRotMatZXY(obj.rot) * glm::transpose(BuildRotMatZXY(oldRot));
							PropagateTransformToChildren(dragIdx, glm::dvec3(0), deltaWorld, oldPos);
						}
						level_.GetLevelObjects().UpdateCoordinatesInLine(obj);
					}
				}
			}
		}
	}
}

// Per-frame velocity-ramped position drag for the property editor's 2D pad and Z
// slider. Called once per frame while the field is held. The object accelerates
// the longer the cursor is held away from the drag-start point (mirrors the
// ALT+SHIFT camera boost) and keeps moving even when the cursor is pinned at the
// window edge. The camera is intentionally NOT moved, so the object visibly
// travels through the world.
void App::ApplyPropPositionDrag() {
	if (prop_field_index_ < 0) return;
	auto& objects = level_.GetLevelObjects().GetObjects();
	int dragIdx = (prop_drag_obj_index_ >= 0) ? prop_drag_obj_index_ : selected_object_index_;
	if (dragIdx < 0 || dragIdx >= (int)objects.size()) return;
	auto& obj = objects[dragIdx];
	const TaskSchema* scp = GetSchema(obj.type);
	if (!scp) return;
	const TaskSchema& schema = *scp;
	int fi = prop_field_index_ / 3;
	int comp = prop_field_index_ % 3;
	if (fi >= (int)schema.size()) return;
	const FieldDef& fd = schema[fi];
	if (fd.typeName != "ObjectPos") return;   // only the pad / Z slider use this model

	int dispx = mouse_state_.prior_x_ - prop_drag_start_x_;
	int dispy = mouse_state_.prior_y_ - prop_drag_start_y_;
	const int   kDead   = 5;        // cursor dead zone around the drag start
	const float kBase   = 25.0f;    // units/frame just past the dead zone
	const float kGrowth = 1.05f;    // per-frame acceleration multiplier
	const float kMax    = 8000.0f;  // speed cap (units/frame)

	auto curArg = [&](int idx) -> double {
		return (idx >= 0 && idx < (int)obj.argTokens.size()) ? std::atof(obj.argTokens[idx].c_str()) : 0.0;
	};
	auto writeArg = [&](int idx, double v) {
		if (idx >= 0 && idx < (int)obj.argTokens.size()) {
			char buf[64]; snprintf(buf, sizeof(buf), "%.6f", v);
			obj.argTokens[idx] = buf;
		}
	};

	glm::dvec3 oldPos = obj.pos;
	bool moved = false;

	if (comp == 0) {            // 2D pad → X / Y
		float len = std::sqrt((float)(dispx * dispx + dispy * dispy));
		if (len < (float)kDead) { prop_drag_speed_ = 0.f; return; }
		prop_drag_speed_ = (prop_drag_speed_ < kBase) ? kBase : std::min(kMax, prop_drag_speed_ * kGrowth);
		float ux = dispx / len, uy = dispy / len;
		double nx = curArg(fd.argOffset + 0) + ux * prop_drag_speed_;
		double ny = curArg(fd.argOffset + 1) - uy * prop_drag_speed_;  // screen-y down → world-y up
		writeArg(fd.argOffset + 0, nx);
		writeArg(fd.argOffset + 1, ny);
		obj.pos.x = nx; obj.pos.y = ny;
		moved = true;
	} else if (comp == 1) {     // Y numeric box → Y only
		if (std::abs(dispy) < kDead) { prop_drag_speed_ = 0.f; return; }
		prop_drag_speed_ = (prop_drag_speed_ < kBase) ? kBase : std::min(kMax, prop_drag_speed_ * kGrowth);
		float uy = (dispy < 0) ? 1.f : -1.f;   // drag up → +Y
		double ny = curArg(fd.argOffset + 1) + uy * prop_drag_speed_;
		writeArg(fd.argOffset + 1, ny);
		obj.pos.y = ny;
		moved = true;
	} else if (comp == 2) {     // Z slider
		if (std::abs(dispy) < kDead) { prop_drag_speed_ = 0.f; return; }
		prop_drag_speed_ = (prop_drag_speed_ < kBase) ? kBase : std::min(kMax, prop_drag_speed_ * kGrowth);
		float uy = (dispy < 0) ? 1.f : -1.f;   // drag up → +Z
		double nz = curArg(fd.argOffset + 2) + uy * prop_drag_speed_;
		writeArg(fd.argOffset + 2, nz);
		obj.pos.z = nz;
		moved = true;
	}

	if (moved) {
		obj.modified = true;
		glm::dvec3 deltaPos = obj.pos - oldPos;
		PropagateTransformToChildren(dragIdx, deltaPos, glm::dmat3(1.0), oldPos);
		level_.GetLevelObjects().UpdateCoordinatesInLine(obj);
		// Live-sync the graph overlay offset when the user drags an AIGraph
		// task's position pad / Z slider — the 3D nodes/edges follow along
		// while F7 is showing the graph.
		SyncGraphOverlayOffsetFromAIGraph();
		// Camera follows the object so it stays in view as it accelerates/travels.
		viewer_.pos_ += glm::vec3(deltaPos);
	}
}
