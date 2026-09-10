/******************************************************************************
 * @file    app_input_keyboard.cpp
 * @brief   App input: keyboard/special key handlers + inline autocomplete
 *          Split from app_input.cpp; shares app_internal.h.
 *****************************************************************************/
#include "app_internal.h"
#include "runtime/graph_camera_target.h"
#include "runtime/pause_menu_state.h"

void App::Input_OnSpecial(int key, int x, int y) {
	// The pause menu owns keyboard input while it is visible.  In particular,
	// editor shortcuts such as F8 must not alter the mode behind the overlay.
	if (pause_mode_) return;

	// F8 toggles Editor <-> Game Play from anywhere; it must run before every
	// mode-specific branch so it works in both directions.
	if (key == GLUT_KEY_F8) {
		ToggleGamePlayMode();
		return;
	}
	if (in_game_mode_ && key == GLUT_KEY_F6) {
		FocusEditorWindow();
		return;
	}
	if (in_game_mode_ && key == GLUT_KEY_F7) {
		FocusGameplayWindow();
		return;
	}
	if (in_game_mode_ && key == GLUT_KEY_F5) {
		if (!IsGameplayInputFocused()) CaptureEditorSnapshotForGameplayApply();
		ApplyAndRestartGameplay();
		return;
	}
	if (in_game_mode_ && IsGameplayInputFocused() &&
		!gameplay_host_.IsGameplayWindowCurrent()) return;
	if (in_game_mode_ && IsGameplayInputFocused() && pause_mode_) return;
	if (in_game_mode_ && IsGameplayInputFocused() && !pause_mode_) {
		return;
	}

	auto& config = Config::Get();
	// Shift detection mirrors Input_OnKeyboard: GLUT modifiers are sometimes
	// not reported, so fall back to GetAsyncKeyState for Shift+arrow
	// selection in the AI Script editor.
	const bool shiftDown = (glutGetModifiers() & GLUT_ACTIVE_SHIFT) != 0 ||
	                       (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

	// Autocomplete task picker navigation
	if (ac_task_picker_open_) {
		// Build filtered list size
		std::vector<std::string> filtered;
		std::string fl = ac_task_filter_;
		std::transform(fl.begin(), fl.end(), fl.begin(), [](unsigned char c){ return std::tolower(c); });
		for (const auto& item : ac_task_items_) {
			if (fl.empty()) { filtered.push_back(item); }
			else {
				std::string il = item;
				std::transform(il.begin(), il.end(), il.begin(), [](unsigned char c){ return std::tolower(c); });
				if (il.find(fl) != std::string::npos) filtered.push_back(item);
			}
		}
		int count = (int)filtered.size();
		if (count > 0) {
			if (key == GLUT_KEY_UP)        ac_task_selected_idx_ = std::max(0, ac_task_selected_idx_ - 1);
			else if (key == GLUT_KEY_DOWN) ac_task_selected_idx_ = std::min(count - 1, ac_task_selected_idx_ + 1);
			else if (key == GLUT_KEY_PAGE_UP)   ac_task_selected_idx_ = std::max(0, ac_task_selected_idx_ - 10);
			else if (key == GLUT_KEY_PAGE_DOWN) ac_task_selected_idx_ = std::min(count - 1, ac_task_selected_idx_ + 10);
			const int row_h = CurrentUiRowHeight(), panel_h = window_state_.viewport_height_ - 100;
			const int max_vis = std::max(1, panel_h / row_h);
			if (ac_task_selected_idx_ < ac_task_scroll_offset_)
				ac_task_scroll_offset_ = ac_task_selected_idx_;
			else if (ac_task_selected_idx_ >= ac_task_scroll_offset_ + max_vis)
				ac_task_scroll_offset_ = ac_task_selected_idx_ - max_vis + 1;
		}
		return;
	}

	// Model picker navigation
	if (model_picker_open_) {
		std::vector<ModelPickerEntry> filtered;
		std::string fl = model_picker_filter_;
		std::transform(fl.begin(), fl.end(), fl.begin(), [](unsigned char c){ return std::tolower(c); });
		for (const auto& entry : level_model_entries_) {
			if (fl.empty()) { filtered.push_back(entry); }
			else {
				std::string idl = entry.label;
				std::transform(idl.begin(), idl.end(), idl.begin(), [](unsigned char c){ return std::tolower(c); });
				if (idl.find(fl) != std::string::npos) filtered.push_back(entry);
			}
		}
		int count = (int)filtered.size();
		if (count > 0) {
			if (key == GLUT_KEY_UP)        model_picker_selected_ = std::max(0, model_picker_selected_ - 1);
			else if (key == GLUT_KEY_DOWN) model_picker_selected_ = std::min(count - 1, model_picker_selected_ + 1);
			else if (key == GLUT_KEY_PAGE_UP)   model_picker_selected_ = std::max(0, model_picker_selected_ - 10);
			else if (key == GLUT_KEY_PAGE_DOWN) model_picker_selected_ = std::min(count - 1, model_picker_selected_ + 10);
			const int row_h = CurrentUiRowHeight(), panel_h = window_state_.viewport_height_ - 100;
			const int max_vis = std::max(1, panel_h / row_h);
			if (model_picker_selected_ < model_picker_scroll_)
				model_picker_scroll_ = model_picker_selected_;
			else if (model_picker_selected_ >= model_picker_scroll_ + max_vis)
				model_picker_scroll_ = model_picker_selected_ - max_vis + 1;
		}
		return;
	}

	// Arrow key navigation inside AI script / path text boxes
	if (prop_text_edit_field_ == PropPanel::kAIScriptTextField ||
	    prop_text_edit_field_ == PropPanel::kAIScriptPathField) {
		bool isScript = (prop_text_edit_field_ == PropPanel::kAIScriptTextField);
		const int mc = AiScriptMaxChars();

		// Shift+arrow extends the selection in the AI Script editor
		// (Notepad/Home/End as well). The anchor is set on first Shift+arrow
		// (or the first time the selection becomes non-empty); subsequent
		// Shift+arrows only move the focus.
		if (isScript && shiftDown && prop_text_sel_anchor_ < 0 &&
		    (key == GLUT_KEY_LEFT || key == GLUT_KEY_RIGHT ||
		     key == GLUT_KEY_UP   || key == GLUT_KEY_DOWN  ||
		     key == GLUT_KEY_HOME || key == GLUT_KEY_END)) {
			prop_text_sel_anchor_ = prop_text_caret_;
		}

		if (key == GLUT_KEY_LEFT) {
			if (prop_text_caret_ > 0) --prop_text_caret_;
			if (isScript) UpdateAIScriptScroll(); else UpdateAIScriptPathHScroll();
			if (isScript && shiftDown) prop_text_sel_focus_ = prop_text_caret_;
			else if (isScript) ClearPropTextSelection();
			return;
		}
		if (key == GLUT_KEY_RIGHT) {
			if (prop_text_caret_ < (int)prop_text_buf_.size()) ++prop_text_caret_;
			if (isScript) UpdateAIScriptScroll(); else UpdateAIScriptPathHScroll();
			if (isScript && shiftDown) prop_text_sel_focus_ = prop_text_caret_;
			else if (isScript) ClearPropTextSelection();
			return;
		}
		if (isScript) {
			if (key == GLUT_KEY_UP || key == GLUT_KEY_DOWN) {
				auto starts = AiTextLineStarts(prop_text_buf_, mc);
				int cl = (int)(std::upper_bound(starts.begin(), starts.end(), prop_text_caret_) - starts.begin()) - 1;
				cl = std::max(0, std::min(cl, (int)starts.size() - 1));
				int col = prop_text_caret_ - starts[cl];
				int nl = cl + (key == GLUT_KEY_DOWN ? 1 : -1);
				nl = std::max(0, std::min(nl, (int)starts.size() - 1));
				int next_end = (nl + 1 < (int)starts.size()) ? starts[nl + 1] : (int)prop_text_buf_.size();
				int line_len = next_end - starts[nl];
				if (line_len > 0 && (starts[nl] + line_len) <= (int)prop_text_buf_.size() &&
				    prop_text_buf_[starts[nl] + line_len - 1] == '\n') --line_len;
				prop_text_caret_ = starts[nl] + std::min(col, line_len);
				UpdateAIScriptScroll();
				if (shiftDown) prop_text_sel_focus_ = prop_text_caret_;
				else           ClearPropTextSelection();
				return;
			}
			if (key == GLUT_KEY_HOME) {
				// Home: start of current visual line; Ctrl+Home: start of buffer
				if (isScript && (glutGetModifiers() & GLUT_ACTIVE_CTRL)) {
					prop_text_caret_ = 0;
				} else {
					auto starts = AiTextLineStarts(prop_text_buf_, mc);
					int cl = (int)(std::upper_bound(starts.begin(), starts.end(), prop_text_caret_) - starts.begin()) - 1;
					cl = std::max(0, std::min(cl, (int)starts.size() - 1));
					prop_text_caret_ = starts[cl];
				}
				UpdateAIScriptScroll();
				if (shiftDown) prop_text_sel_focus_ = prop_text_caret_;
				else           ClearPropTextSelection();
				return;
			}
			if (key == GLUT_KEY_END) {
				if (isScript && (glutGetModifiers() & GLUT_ACTIVE_CTRL)) {
					prop_text_caret_ = (int)prop_text_buf_.size();
				} else {
					auto starts = AiTextLineStarts(prop_text_buf_, mc);
					int cl = (int)(std::upper_bound(starts.begin(), starts.end(), prop_text_caret_) - starts.begin()) - 1;
					cl = std::max(0, std::min(cl, (int)starts.size() - 1));
					int next_end = (cl + 1 < (int)starts.size()) ? starts[cl + 1] : (int)prop_text_buf_.size();
					if (next_end > starts[cl] && prop_text_buf_[next_end - 1] == '\n') --next_end;
					prop_text_caret_ = next_end;
				}
				UpdateAIScriptScroll();
				if (shiftDown) prop_text_sel_focus_ = prop_text_caret_;
				else           ClearPropTextSelection();
				return;
			}
			if (key == GLUT_KEY_PAGE_UP) {
				ai_script_vscroll_ = std::max(0, ai_script_vscroll_ - 12);
				return;
			}
			if (key == GLUT_KEY_PAGE_DOWN) {
				auto starts = AiTextLineStarts(prop_text_buf_, mc);
				ai_script_vscroll_ = std::min(std::max(0, (int)starts.size() - 12),
				                              ai_script_vscroll_ + 12);
				return;
			}
		}
	}

	if (task_picker_open_) {
		auto& objects = level_.GetLevelObjects().GetObjects();
		std::vector<int> picker_to_objects;
		
		std::string search_lower = task_picker_search_;
		std::transform(search_lower.begin(), search_lower.end(), search_lower.begin(), [](unsigned char c) { return std::tolower(c); });
		
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
				std::transform(label_lower.begin(), label_lower.end(), label_lower.begin(), [](unsigned char c) { return std::tolower(c); });

				if (search_lower.empty() || label_lower.find(search_lower) != std::string::npos) {
					picker_to_objects.push_back(i);
				}
			}
		}

		if (!picker_to_objects.empty()) {
			int count = (int)picker_to_objects.size();
			if (key == GLUT_KEY_UP) {
				if (task_picker_selected_idx_ > 0) {
					task_picker_selected_idx_--;
				} else {
					task_picker_selected_idx_ = count - 1;
				}
			}
			else if (key == GLUT_KEY_DOWN) {
				if (task_picker_selected_idx_ < count - 1) {
					task_picker_selected_idx_++;
				} else {
					task_picker_selected_idx_ = 0;
				}
			}
			else if (key == GLUT_KEY_PAGE_UP) {
				task_picker_selected_idx_ = std::max(0, task_picker_selected_idx_ - 10);
			}
			else if (key == GLUT_KEY_PAGE_DOWN) {
				task_picker_selected_idx_ = std::min(count - 1, task_picker_selected_idx_ + 10);
			}
			else if (key == GLUT_KEY_HOME) {
				task_picker_selected_idx_ = 0;
			}
			else if (key == GLUT_KEY_END) {
				task_picker_selected_idx_ = count - 1;
			}

			int row_h = CurrentUiRowHeight();
			int picker_h = window_state_.viewport_height_ - 100;
			int max_visible = std::max(1, picker_h / row_h);
			if (task_picker_selected_idx_ < task_picker_scroll_offset_) {
				task_picker_scroll_offset_ = task_picker_selected_idx_;
			}
			else if (task_picker_selected_idx_ >= task_picker_scroll_offset_ + max_visible) {
				task_picker_scroll_offset_ = task_picker_selected_idx_ - max_visible + 1;
			}
		}
		return;
	}

	// C2: caret movement while a property text/numeric box is being edited.
	if (prop_text_edit_field_ != -1) {
		int n = (int)prop_text_buf_.size();
		if (prop_text_caret_ < 0) prop_text_caret_ = 0;
		if (prop_text_caret_ > n) prop_text_caret_ = n;
		if (key == GLUT_KEY_LEFT)  prop_text_caret_ = std::max(0, prop_text_caret_ - 1);
		if (key == GLUT_KEY_RIGHT) prop_text_caret_ = std::min(n, prop_text_caret_ + 1);
		if (key == GLUT_KEY_HOME)  prop_text_caret_ = 0;
		if (key == GLUT_KEY_END)   prop_text_caret_ = n;
		return;
	}

	if (show_hud_ && window_state_.cursor_visible_) {
		if (key == GLUT_KEY_UP || key == GLUT_KEY_DOWN || key == GLUT_KEY_LEFT || key == GLUT_KEY_RIGHT) {
			auto visibleList = GetVisibleTreeNodes();
			if (!visibleList.empty()) {
				int current_row = -1;
				for (int i = 0; i < (int)visibleList.size(); ++i) {
					if (visibleList[i] == selected_object_index_) {
						current_row = i;
						break;
					}
				}

				if (key == GLUT_KEY_UP) {
					if (current_row > 0) {
						selected_object_index_ = visibleList[current_row - 1];
						current_row--;
					} else {
						selected_object_index_ = visibleList.back();
						current_row = (int)visibleList.size() - 1;
					}
				}
				else if (key == GLUT_KEY_DOWN) {
					if (current_row >= 0 && current_row < (int)visibleList.size() - 1) {
						selected_object_index_ = visibleList[current_row + 1];
						current_row++;
					} else {
						selected_object_index_ = visibleList.front();
						current_row = 0;
					}
				}
				else if (key == GLUT_KEY_LEFT) {
					if (selected_object_index_ == -2) {
						tree_decl_expanded_ = false;
						Logger::Get().Log(LogLevel::INFO, "[App] Collapsed Mission Declarations");
					} else if (selected_object_index_ >= 0) {
						auto& obj = level_.GetLevelObjects().GetObjects()[selected_object_index_];
						if (obj.isContainer && obj.expanded) {
							auto& nonConstObj = const_cast<LevelObject&>(obj);
							nonConstObj.expanded = false;
							Logger::Get().Log(LogLevel::INFO, "[App] Collapsed: " + obj.type);
						} else if (obj.parentIndex != -1) {
							selected_object_index_ = obj.parentIndex;
							for (int i = 0; i < (int)visibleList.size(); ++i) {
								if (visibleList[i] == selected_object_index_) {
									current_row = i;
									break;
								}
							}
						}
					}
				}
				else if (key == GLUT_KEY_RIGHT) {
					if (selected_object_index_ == -2) {
						tree_decl_expanded_ = true;
						Logger::Get().Log(LogLevel::INFO, "[App] Expanded Mission Declarations");
					} else if (selected_object_index_ >= 0) {
						auto& obj = level_.GetLevelObjects().GetObjects()[selected_object_index_];
						if (obj.isContainer) {
							if (!obj.expanded) {
								auto& nonConstObj = const_cast<LevelObject&>(obj);
								nonConstObj.expanded = true;
								Logger::Get().Log(LogLevel::INFO, "[App] Expanded: " + obj.type);
							} else if (!obj.childrenIndices.empty()) {
								int firstChild = -1;
								for (int childIdx : obj.childrenIndices) {
									if (childIdx >= 0 && childIdx < (int)level_.GetLevelObjects().GetObjects().size()) {
										if (!level_.GetLevelObjects().GetObjects()[childIdx].deleted) {
											firstChild = childIdx;
											break;
										}
									}
								}
								if (firstChild != -1) {
									selected_object_index_ = firstChild;
									auto newVisibleList = GetVisibleTreeNodes();
									for (int i = 0; i < (int)newVisibleList.size(); ++i) {
										if (newVisibleList[i] == selected_object_index_) {
											current_row = i;
											break;
										}
									}
								}
							}
						}
					}
				}

				// Auto scroll to keep selected item visible
				int row_h = CurrentUiRowHeight();
				int start_y = 30;
				int max_rows = (window_state_.viewport_height_ - 50 - start_y) / row_h;
				if (max_rows > 0) {
					if (current_row < tree_scroll_offset_) {
						tree_scroll_offset_ = current_row;
					} else if (current_row >= tree_scroll_offset_ + max_rows) {
						tree_scroll_offset_ = current_row - max_rows + 1;
					}
				}
			}
			return;
		}
	}

	// F2 always toggles TaskTree — checked before any config key overrides
	if (key == GLUT_KEY_F2) {
		show_hud_ = !show_hud_;
		bridge_.SetEnabled(show_hud_);
		Logger::Get().Log(LogLevel::INFO, std::string("[App] TaskTree ") + (show_hud_ ? "shown" : "hidden"));
		return;
	}

	// F10 always toggles the Animation Debug overlay (skeleton wireframe + status
	// panel) — independent of F2/TaskTree, and also toggles Developer Mode for Debug Commands.
	if (key == GLUT_KEY_F10) {
		show_anim_debug_ = !show_anim_debug_;
		developer_mode_ = !developer_mode_;
		if (developer_mode_) {
			debug_cmd_mgr_.Start();
			Logger::Get().Log(LogLevel::INFO, "[App] Developer Mode ON - Command Watcher Started");
		} else {
			debug_cmd_mgr_.Stop();
			Logger::Get().Log(LogLevel::INFO, "[App] Developer Mode OFF - Command Watcher Stopped");
		}
		Logger::Get().Log(LogLevel::INFO, std::string("[App] Animation Debug Info ") + (show_anim_debug_ ? "shown" : "hidden"));
		return;
	}

	// All other F-key/special-key bindings go through DispatchEventBindings (qedkeybindings.qsc only)

	// Camera movement keys only fire when SHIFT+ALT is held; plain arrow keys must not move camera.
	{
		int mods = glutGetModifiers();
		bool shiftAlt = (mods & GLUT_ACTIVE_SHIFT) && (mods & GLUT_ACTIVE_ALT);
		if (shiftAlt) {
			if (key == config.keyMoveForward)  { input_.keys_ |= MK_FORWARD;  return; }
			if (key == config.keyMoveBackward) { input_.keys_ |= MK_BACKWARD; return; }
			if (key == config.keyMoveLeft)     { input_.keys_ |= MK_LEFT;     return; }
			if (key == config.keyMoveRight)    { input_.keys_ |= MK_RIGHT;    return; }
		}
	}

	if (key == GLUT_KEY_PAGE_UP) {
		// Shift/Ctrl+PageUp = task tree operations → fall through to DispatchEventBindings
		if (!(glutGetModifiers() & (GLUT_ACTIVE_SHIFT | GLUT_ACTIVE_CTRL))) {
			viewer_.jump_speed_ *= 2.0f;
			if (viewer_.jump_speed_ > MAX_JUMP_SPEED) viewer_.jump_speed_ = MAX_JUMP_SPEED;
			printf("current jump speed set to %d\n", (int)viewer_.jump_speed_);
			return;
		}
	}

	if (key == GLUT_KEY_PAGE_DOWN) {
		// Shift/Ctrl+PageDown = task tree operations → fall through to DispatchEventBindings
		if (!(glutGetModifiers() & (GLUT_ACTIVE_SHIFT | GLUT_ACTIVE_CTRL))) {
			viewer_.jump_speed_ *= 0.5f;
			if (viewer_.jump_speed_ < MIN_JUMP_SPEED) viewer_.jump_speed_ = MIN_JUMP_SPEED;
			printf("current jump speed set to %d\n", (int)viewer_.jump_speed_);
			return;
		}
	}

	if (key == GLUT_KEY_F11) {
		auto& objects = level_.GetLevelObjects().GetObjects();
		const bool hasSelectedObject = selected_object_index_ >= 0 &&
			selected_object_index_ < (int)objects.size();
		const bool hasGraphTarget = renderer_.IsGraphOverlayVisible() ||
			renderer_.GraphSelected() >= 0;
		if (!hasSelectedObject && !hasGraphTarget) return;

		if (hasSelectedObject && !renderer_.IsGraphOverlayVisible()) {
			const auto& obj = objects[selected_object_index_];
			const float boundRadius = renderer_.GetMeshRadius(obj.modelId, obj.isBuilding) * 40.96f * obj.scale;

			// Shift+F11: wider radius framing (CameraSnapToObjectWithRadius)
			const bool shiftHeld = (glutGetModifiers() & GLUT_ACTIVE_SHIFT) != 0 ||
			                       (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

			const GraphCameraPose pose = MakeF11ObjectCameraPose(
				obj.pos, boundRadius, glm::dvec3(viewer_.forward_), shiftHeld);

			orbit_active_ = false;
			input_.mouse_delta_x_ = 0;
			input_.mouse_delta_y_ = 0;
			viewer_.velocity_ = glm::vec3(0.0f);
			viewer_.pos_ = glm::vec3(pose.position);
			viewer_.yaw_ = pose.yaw_degrees;
			viewer_.pitch_ = pose.pitch_degrees;
			UpdateViewerVectors();

			Logger::Get().Log(LogLevel::INFO,
				"[Camera] F11 CameraSnapToObject model=" + obj.modelId +
				" type=" + obj.type + " task=" + obj.taskId +
				" radius=" + std::to_string(boundRadius) +
				" distance=" + std::to_string(glm::distance(pose.position, obj.pos)) +
				" camera=(" + std::to_string(viewer_.pos_.x) + "," +
				std::to_string(viewer_.pos_.y) + "," +
				std::to_string(viewer_.pos_.z) + ")");
			return;
		}

		const std::optional<glm::dvec3> relatedGraphOrigin =
			FindRelatedGraphOrigin(objects, selected_object_index_);
		const std::string relatedGraphTaskId =
			FindRelatedGraphTaskId(objects, selected_object_index_);
		glm::dvec3 objectPosition = renderer_.GraphOverlayOffset();
		if (hasSelectedObject) {
			const auto& obj = objects[selected_object_index_];
			objectPosition = obj.pos;
		}
		const GraphCameraTarget target = ResolveF11CameraTarget(
			renderer_.GetGraphOverlaySnapshot(),
			hasSelectedObject,
			renderer_.IsGraphOverlayVisible(),
			renderer_.GraphSelected(),
			relatedGraphTaskId,
			renderer_.GraphOverlayTaskId(),
			renderer_.GraphOverlayOffset(),
			objectPosition,
			relatedGraphOrigin);
		if (target.kind == GraphCameraTargetKind::Object && !hasSelectedObject) return;
		// Orbit mode recomputes the camera position on the next input tick. F11 is
		// an explicit teleport, so cancel orbit and stale movement deltas first.
		orbit_active_ = false;
		input_.mouse_delta_x_ = 0;
		input_.mouse_delta_y_ = 0;
		viewer_.velocity_ = glm::vec3(0.0f);
		if (target.kind == GraphCameraTargetKind::GraphNode ||
			target.kind == GraphCameraTargetKind::GraphOrigin) {
			const GraphCameraPose pose = MakeF11GraphCameraPose(
				target.position, glm::dvec3(viewer_.forward_),
				6.0 * static_cast<double>(WORLD_UNITS_PER_METER));
			viewer_.pos_ = glm::vec3(pose.position);
			viewer_.yaw_ = pose.yaw_degrees;
			viewer_.pitch_ = pose.pitch_degrees;
		} else {
			float boundRadius = 2000.0f;
			if (hasSelectedObject) {
				const auto& obj = objects[selected_object_index_];
				boundRadius = renderer_.GetMeshRadius(obj.modelId, obj.isBuilding) * 40.96f * obj.scale;
				if (boundRadius < 500.0f) boundRadius = 2000.0f;
			}
			const GraphCameraPose pose = MakeF11ObjectCameraPose(
				target.position, boundRadius, glm::dvec3(viewer_.forward_), /*shiftHeld=*/false);
			viewer_.pos_ = glm::vec3(pose.position);
			viewer_.yaw_ = pose.yaw_degrees;
			viewer_.pitch_ = pose.pitch_degrees;
		}
		UpdateViewerVectors();
		const char* target_name = target.kind == GraphCameraTargetKind::GraphNode
			? "Graph Node" : target.kind == GraphCameraTargetKind::GraphOrigin
			? "Graph Origin" : "Object";
		printf("Teleported to %s [%d] at (%.2f, %.2f, %.2f)\n",
			target_name, target.node_id >= 0 ? target.node_id : selected_object_index_,
			target.position.x, target.position.y, target.position.z);
		Logger::Get().Log(LogLevel::INFO,
			std::string("[Camera] F11 target=") + target_name + " position=(" +
			std::to_string(target.position.x) + "," +
			std::to_string(target.position.y) + "," +
			std::to_string(target.position.z) + ") camera=(" +
			std::to_string(viewer_.pos_.x) + "," +
			std::to_string(viewer_.pos_.y) + "," +
			std::to_string(viewer_.pos_.z) + ")");
		return;
	}

	DispatchEventBindings();
}

void App::Input_OnSpecialUp(int key, int x, int y) {
	if (in_game_mode_ && IsGameplayInputFocused() &&
		!gameplay_host_.IsGameplayWindowCurrent()) return;
	if (in_game_mode_ && IsGameplayInputFocused() && pause_mode_) return;
	if (in_game_mode_ && IsGameplayInputFocused() && !pause_mode_) {
		return;
	}
	if (pause_mode_) return;

	auto& config = Config::Get();

	if (key == config.keyMoveForward)  { input_.keys_ &= ~MK_FORWARD;  return; }
	if (key == config.keyMoveBackward) { input_.keys_ &= ~MK_BACKWARD; return; }
	if (key == config.keyMoveLeft)     { input_.keys_ &= ~MK_LEFT;     return; }
	if (key == config.keyMoveRight)    { input_.keys_ &= ~MK_RIGHT;    return; }
}


// Manip keys are now handled directly in Input_OnKeyboard using config values

// Inline autocomplete: complete the word left of the caret from autocomplete_keywords_.
// Repeated calls on the just-completed token cycle through ALL matches, so every keyword
// sharing a prefix is reachable. Triggered by Ctrl+Space AND Tab (Tab is reliably delivered
// by GLUT; Ctrl+Space is sometimes dropped). Returns true if it completed a token.
bool App::InlineAutocomplete() {
	Logger::Get().Log(LogLevel::INFO, "[Autocomplete] triggered: field=" +
		std::to_string(prop_text_edit_field_) + " keywords=" + std::to_string(autocomplete_keywords_.size()) +
		" buf='" + prop_text_buf_ + "' caret=" + std::to_string(prop_text_caret_));
	if (prop_text_edit_field_ == -1) {
		status_message_ = "Autocomplete: click a text box first";
		return false;
	}
	if (autocomplete_keywords_.empty()) {
		status_message_ = "Autocomplete: no keywords loaded (editor/tools/IGIAutoComplete.txt)";
		return false;
	}
	if (prop_text_caret_ < 0) prop_text_caret_ = 0;
	if (prop_text_caret_ > (int)prop_text_buf_.size()) prop_text_caret_ = (int)prop_text_buf_.size();

	// Token immediately left of the caret (alphanumerics + underscore).
	int ws = prop_text_caret_;
	while (ws > 0 && (isalnum((unsigned char)prop_text_buf_[ws-1]) || prop_text_buf_[ws-1] == '_')) ws--;
	std::string word = prop_text_buf_.substr(ws, prop_text_caret_ - ws);
	if (word.empty()) {
		status_message_ = "Autocomplete: type a prefix first";
		return false;
	}

	auto lower = [](std::string s) {
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
		return s;
	};

	// Continuation = same token position and the token equals the keyword we just inserted;
	// then we cycle within the ORIGINAL prefix's match set instead of restarting from it.
	bool cycling = (ws == ac_inline_start_ && !ac_inline_last_kw_.empty() &&
	                !ac_inline_prefix_.empty() && lower(word) == lower(ac_inline_last_kw_));
	std::string activePrefix = cycling ? ac_inline_prefix_ : word;
	std::string ap = lower(activePrefix);

	std::vector<const std::string*> matches;
	for (const auto& kw : autocomplete_keywords_) {
		std::string kwl = lower(kw);
		if (kwl.size() >= ap.size() && kwl.compare(0, ap.size(), ap) == 0)
			matches.push_back(&kw);
	}
	if (matches.empty()) {
		status_message_ = "No keyword starts with '" + activePrefix + "'";
		ac_inline_start_ = -1; ac_inline_idx_ = -1; ac_inline_last_kw_.clear();
		return false;
	}

	int idx = cycling ? ((ac_inline_idx_ + 1) % (int)matches.size()) : 0;
	const std::string& chosen = *matches[idx];
	prop_text_buf_.replace(ws, word.size(), chosen);
	prop_text_caret_ = ws + (int)chosen.size();

	ac_inline_prefix_  = activePrefix;
	ac_inline_start_   = ws;
	ac_inline_idx_     = idx;
	ac_inline_last_kw_ = chosen;

	if (matches.size() > 1)
		status_message_ = "Autocompleted (" + std::to_string(idx + 1) + "/" +
		                  std::to_string(matches.size()) + "): " + chosen + "  [Tab/Ctrl+Space to cycle]";
	else
		status_message_ = "Autocompleted: " + chosen;

	if (prop_text_edit_field_ == PropPanel::kAIScriptTextField) {
		ai_script_text_  = prop_text_buf_;
		ai_script_dirty_ = true;
		UpdateAIScriptScroll();
	}
	return true;
}

void App::Input_OnKeyboard(unsigned char key, int x, int y) {
	auto& config = Config::Get();

	if (in_game_mode_ && IsGameplayInputFocused() &&
		!gameplay_host_.IsGameplayWindowCurrent()) return;
	if (in_game_mode_ && IsGameplayInputFocused() && !pause_mode_) {
		if (key == 27) { // ESC
			TogglePauseMenu();
			return;
		}
		gameplay_host_.GetInputRouter().OnKeyboardKey(key, true);
		return;
	}
	if (igi::IsPauseMenuInputActive(pause_mode_, IsGameplayInputFocused())) {
		if (key == 13) { // Enter
			// Enter is consumed by the pause overlay. It only commits the level
			// selector after that row has explicitly been focused; opening the
			// menu must never reload the current level as a side effect.
			if (igi::PauseMenuLevelInputFocused(pause_mode_, pause_active_input_) &&
				!pause_level_input_.empty()) {
				int lvl = std::atoi(pause_level_input_.c_str());
				if (lvl >= 1 && lvl <= 14) {
					LoadLevel(lvl);
					TogglePauseMenu();
				} else {
					Logger::Get().Log(LogLevel::ERR, "Level must be between 1 and 14.");
				}
			}
			return;
		}
		if (key == 27) {
			if (pause_active_input_ != -1) { pause_active_input_ = -1; return; }
			TogglePauseMenu();
			return;
		}
		if (igi::PauseMenuLevelInputFocused(pause_mode_, pause_active_input_)) {
			if (key == 8 && !pause_level_input_.empty()) {
				pause_level_input_.pop_back();
				return;
			}
			if (key >= '0' && key <= '9' && pause_level_input_.size() < 2) {
				pause_level_input_ += static_cast<char>(key);
				return;
			}
		}
		return;
	}

	bool ctrlDown = (glutGetModifiers() & GLUT_ACTIVE_CTRL) != 0 ||
	                (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
	bool shiftDown = (glutGetModifiers() & GLUT_ACTIVE_SHIFT) != 0 ||
	                 (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
	bool altDown = (glutGetModifiers() & GLUT_ACTIVE_ALT) != 0 ||
	               (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

	// GLUT reports Ctrl+letter as the ASCII control character (Ctrl+A=1,
	// Ctrl+W=23). The normal configurable binding path samples
	// GetAsyncKeyState, which can miss that chord after the native key event has
	// already been queued. Recognize the configured SaveState binding from the
	// actual event as well, while still honoring its modifiers and key choice.
	{
		auto saveBinding = config.eventBindings_.find("SaveState");
		if (saveBinding != config.eventBindings_.end() && saveBinding->second.ctrl &&
			!saveBinding->second.shift && !saveBinding->second.alt && ctrlDown) {
			const int vk = saveBinding->second.vkCode;
			const bool isLetter = vk >= 'A' && vk <= 'Z';
			const bool matchesEvent = key == vk ||
				(isLetter && key == static_cast<unsigned char>(vk - 'A' + 1));
			if (matchesEvent) {
				CommitPropTextEdit();
				SaveCurrentLevel();
				return;
			}
		}
	}

	// Alt+H: toggle the gameplay HUD overlay (crosshair, health bar, weapon
	// readout). Hidden by default until the vanilla presentation is finished.
	if (altDown && (key == 'h' || key == 'H')) {
		hud_overlay_visible_ = !hud_overlay_visible_;
		status_message_ = hud_overlay_visible_
			? "HUD overlay shown (Alt+H to hide)"
			: "HUD overlay hidden (Alt+H to show)";
		return;
	}

	if (ctrlDown && (key == 8 || key == 'h' || key == 'H')) { // CTRL+H
		ToggleOverlayWireframe();
		return;
	}

	// Autocomplete Ctrl combos — intercept before prop text editor so they work while editing.
	// Detect Ctrl via GLUT *and* GetAsyncKeyState: GLUT modifiers are occasionally not
	// reported for Ctrl+Space (key 0/space), which silently dropped inline autocomplete.
	if (ctrlDown) {
		// AI Script editor notepad-style shortcuts — ONLY when the AI Script text
		// field is focused. Other property text fields ignore these so the
		// editor-level Ctrl+F / Ctrl+N / Ctrl+W bindings still work as before.
		//
		// GLUT sends the ASCII control-character code for Ctrl+letter
		// (1=SOH/A, 3=ETX/C, 0x18=CAN/X, 0x16=SYN/V, 0x19=EM/Y, 0x1A=SUB/Z)
		// — NOT the letter itself. We also accept the letter codes so this
		// keeps working if a host swallows the control code.
		if (prop_text_edit_field_ == PropPanel::kAIScriptTextField) {
			const bool is_a = (key == 1  || key == 'a' || key == 'A');
			const bool is_c = (key == 3  || key == 'c' || key == 'C');
			const bool is_x = (key == 24 || key == 'x' || key == 'X');
			const bool is_v = (key == 22 || key == 'v' || key == 'V');
			const bool is_y = (key == 25 || key == 'y' || key == 'Y');
			const bool is_z = (key == 26 || key == 'z' || key == 'Z');
			if (is_a) { SelectPropTextAll(); return; }
			if (is_c) { AiScriptCopy();     return; }
			if (is_x) { AiScriptCut();      return; }
			if (is_v) { AiScriptPaste();    return; }
			// Ctrl+Z = undo, Ctrl+Y / Ctrl+Shift+Z = redo
			if (is_z) { if (shiftDown) AiScriptRedo(); else AiScriptUndo(); return; }
			if (is_y) { AiScriptRedo(); return; }
		}
		// Ctrl+A is also valid for every focused property text/numeric box. The
		// same selection state lets the next printable character replace its value.
		if ((key == 1 || key == 'a' || key == 'A') && prop_text_edit_field_ != -1) {
			SelectPropTextAll();
			return;
		}
		if (key == 14 && !shiftDown) { // Ctrl+N only (not Ctrl+Shift+N — that's TaskFindByTaskNote)
			// Only open when a property text box is focused, so Enter knows which
			// field to insert the chosen item into.
			if (prop_text_edit_field_ == -1) {
				status_message_ = "Click a text box first, then Ctrl+N to pick a task type";
				return;
			}
			picker_target_field_  = prop_text_edit_field_;
			picker_target_obj_    = prop_edit_obj_index_;
			picker_target_caret_  = prop_text_caret_;
			ac_task_items_ = autocomplete_keywords_;
			ac_task_picker_open_ = true; ac_task_selected_idx_ = 0; ac_task_scroll_offset_ = 0; ac_task_filter_.clear();
			return;
		}
		if (key == 15) { // Ctrl+O → Model ID picker (only open when active text box has XXX_XX_X content)
			if (prop_text_edit_field_ == -1) {
				status_message_ = "Click a text box first, then Ctrl+O to pick a model";
				return;
			}
			// Check if current text looks like a model ID (digits and underscores)
			bool looksLikeModelId = true;
			for (char c : prop_text_buf_)
				if (!isdigit(c) && c != '_' && c != ' ') { looksLikeModelId = false; break; }
			if (!looksLikeModelId && !prop_text_buf_.empty()) {
				return; // Only open from model ID text boxes
			}
			picker_target_field_ = prop_text_edit_field_; picker_target_obj_ = prop_edit_obj_index_;
			model_picker_open_ = true; model_picker_selected_ = 0; model_picker_scroll_ = 0;
			model_picker_filter_.clear(); // show ALL model IDs; user types to filter
			return;
		}
		if (key == 0 || key == ' ') { // Ctrl+Space → AutoComplete inline
			InlineAutocomplete();
			return;
		}
	}

	// C2: Property text editor input (caret-based).
	// Skip while a picker or file dialog is open so their keystrokes (filter typing,
	// Enter to insert/confirm) reach their own handlers below instead of being eaten
	// here — otherwise typing edits the box and Enter commits+closes the field.
	if (prop_text_edit_field_ != -1 && !ac_task_picker_open_ && !model_picker_open_ &&
	    file_dialog_mode_ == FileDialogMode::None) {
		int& caret = prop_text_caret_;
		if (caret < 0) caret = 0;
		if (caret > (int)prop_text_buf_.size()) caret = (int)prop_text_buf_.size();
		bool multiline = IsPropFieldMultiline(prop_text_edit_field_);

		// Before the text editor swallows the key, check if it matches a save
		// hotkey from qedkeybindings.qsc (e.g. Ctrl+W = SaveState, Ctrl+S =
		// SaveObjectFile). If so, commit the in-flight edit first so the AI
		// script text is saved, then let the binding fire.
		{
			auto& bindings = Config::Get().eventBindings_;
			auto matchBinding = [&](const std::string& name) -> bool {
				auto it = bindings.find(name);
				return it != bindings.end() && Utils::IsKeyBindingPressedExact(it->second);
			};
			if (matchBinding("SaveState") || matchBinding("SaveObjectFile") ||
			    matchBinding("SaveSubTaskObjectFile") || matchBinding("SaveSubTaskObjectFileParent")) {
				CommitPropTextEdit();
				// Fall through to DispatchEventBindings below.
			}
			else if (matchBinding("ToggleSaveStateOnExit") || matchBinding("ToggleAutoSave") ||
			         matchBinding("AutoSaveIntervalUp") || matchBinding("AutoSaveIntervalDown")) {
				CommitPropTextEdit();
				// Fall through to DispatchEventBindings below.
			}
		}

		// If the edit was just committed by a save hotkey, prop_text_edit_field_
		// is now -1 and this block is skipped — the key reaches DispatchEventBindings.
		if (prop_text_edit_field_ == -1) {
			// fall through to DispatchEventBindings
		}
		else {
			if (key == 27) { // ESC — cancel (revert)
				prop_text_edit_field_ = -1;
				ClearPropTextSelection();
				return;
			}
			if (key == 13) { // Enter
				if (multiline) {              // VarString/String256: insert newline
					// AI Script editor: respect active selection (replace with newline)
					if (isPropTextSel()) {
						PushAiTextUndo();
						int a, b; GetPropTextSelection(a, b);
						prop_text_buf_.erase(a, b - a);
						caret = a;
						ClearPropTextSelection();
					} else {
						PushAiTextUndo();
					}
					prop_text_buf_.insert(prop_text_buf_.begin() + caret, '\n');
					caret++;
					SyncAIScriptBuffer();
					UpdateAIScriptScroll();
				} else {                      // single-line: commit
					CommitPropTextEdit();
				}
				return;
			}
			if (key == 8) { // Backspace — delete before caret (or delete selection)
				if (isPropTextSel()) {
					PushAiTextUndo();
					int a, b; GetPropTextSelection(a, b);
					prop_text_buf_.erase(a, b - a);
					caret = a;
					ClearPropTextSelection();
					SyncAIScriptBuffer();
				} else if (caret > 0) {
					PushAiTextUndo();
					prop_text_buf_.erase(prop_text_buf_.begin() + (caret - 1));
					caret--;
					SyncAIScriptBuffer();
				}
				UpdateAIScriptScroll();
				UpdateAIScriptPathHScroll();
				return;
			}
			if (key == 127) { // Delete — delete at caret (or delete selection)
				if (isPropTextSel()) {
					PushAiTextUndo();
					int a, b; GetPropTextSelection(a, b);
					prop_text_buf_.erase(a, b - a);
					caret = a;
					ClearPropTextSelection();
					SyncAIScriptBuffer();
				} else if (caret < (int)prop_text_buf_.size()) {
					PushAiTextUndo();
					prop_text_buf_.erase(prop_text_buf_.begin() + caret);
					SyncAIScriptBuffer();
				}
				UpdateAIScriptScroll();
				return;
			}
			if (key == '\t') { // Tab → inline autocomplete (reliable trigger; GLUT may eat Ctrl+Space)
				InlineAutocomplete();
				return;
			}
			if (key >= 32 && key <= 126) { // printable: insert at caret (replace selection)
				if (isPropTextSel()) {
					PushAiTextUndo();
					int a, b; GetPropTextSelection(a, b);
					prop_text_buf_.erase(a, b - a);
					caret = a;
					ClearPropTextSelection();
				} else {
					PushAiTextUndo();
				}
				prop_text_buf_.insert(prop_text_buf_.begin() + caret, (char)key);
				caret++;
				SyncAIScriptBuffer();
				UpdateAIScriptScroll();
				UpdateAIScriptPathHScroll();
				return;
			}
			return;
		}
	}

	// File dialog input (SaveSubTask / LoadSubTask)
	if (file_dialog_mode_ != FileDialogMode::None) {
		if (key == 27) { file_dialog_mode_ = FileDialogMode::None; return; }
		if (key == 13) { ConfirmFileDialog(); return; }
		if (key == 8) {
			if (file_dialog_caret_ > 0) {
				file_dialog_path_.erase(file_dialog_path_.begin() + file_dialog_caret_ - 1);
				file_dialog_caret_--;
			}
		} else if (key >= 32 && key <= 126) {
			file_dialog_path_.insert(file_dialog_path_.begin() + file_dialog_caret_, (char)key);
			file_dialog_caret_++;
		}
		return;
	}

	// Autocomplete task picker input (Ctrl+N panel)
	if (ac_task_picker_open_) {
		if (key == 27) { ac_task_picker_open_ = false; return; }
		if (key == 13) {
			// Build filtered list and get selected item
			std::vector<std::string> filtered;
			std::string fl = ac_task_filter_;
			std::transform(fl.begin(), fl.end(), fl.begin(), [](unsigned char c){ return std::tolower(c); });
			for (auto& item : ac_task_items_) {
				if (fl.empty()) { filtered.push_back(item); }
				else {
					std::string il = item; std::transform(il.begin(), il.end(), il.begin(), [](unsigned char c){ return std::tolower(c); });
					if (il.find(fl) != std::string::npos) filtered.push_back(item);
				}
			}
			ac_task_picker_open_ = false;
			// Restore field, obj, and caret position captured when the picker opened.
			if (picker_target_field_ != -1) {
				prop_text_edit_field_ = picker_target_field_;
				prop_edit_obj_index_  = picker_target_obj_;
			}
			if (prop_text_edit_field_ != -1 && ac_task_selected_idx_ < (int)filtered.size()) {
				const std::string& item = filtered[ac_task_selected_idx_];
				bool isAI = (prop_text_edit_field_ == PropPanel::kAIScriptTextField);
				if (isAI) {
					// For AI script: INSERT at saved caret position (don't replace entire script)
					int ins = std::max(0, std::min(picker_target_caret_, (int)prop_text_buf_.size()));
					prop_text_buf_.insert(ins, item);
					prop_text_caret_ = ins + (int)item.size();
					// Commit: store into ai_script_text_ and mark dirty, keep editing
					ai_script_text_  = prop_text_buf_;
					ai_script_dirty_ = true;
					UpdateAIScriptScroll();
				} else {
					prop_text_buf_  = item;       // REPLACE field content (existing behaviour)
					prop_text_caret_ = (int)prop_text_buf_.size();
					CommitPropTextEdit();
				}
			}
			picker_target_field_ = -1; picker_target_obj_ = -1; picker_target_caret_ = -1;
			return;
		}
		if (key == 8) {
			if (!ac_task_filter_.empty()) { ac_task_filter_.pop_back(); ac_task_selected_idx_ = 0; ac_task_scroll_offset_ = 0; }
		} else if (key >= 32 && key <= 126) {
			ac_task_filter_ += (char)key;
			ac_task_selected_idx_ = 0;
			ac_task_scroll_offset_ = 0;
		}
		return;
	}

	// Model picker input (Ctrl+O panel)
	if (model_picker_open_) {
		if (key == 27) { model_picker_open_ = false; return; }
		if (key == 13) {
			std::vector<ModelPickerEntry> filtered;
			std::string fl = model_picker_filter_;
			std::transform(fl.begin(), fl.end(), fl.begin(), [](unsigned char c){ return std::tolower(c); });
			for (const auto& entry : level_model_entries_) {
				if (fl.empty()) { filtered.push_back(entry); }
				else {
					std::string idl = entry.label;
					std::transform(idl.begin(), idl.end(), idl.begin(), [](unsigned char c){ return std::tolower(c); });
					if (idl.find(fl) != std::string::npos) filtered.push_back(entry);
				}
			}
			model_picker_open_ = false;
			// Restore the field captured when the picker opened, so the choice lands in
			// the exact text box the cursor was in (even if focus changed meanwhile).
			if (picker_target_field_ >= 0) { prop_text_edit_field_ = picker_target_field_; prop_edit_obj_index_ = picker_target_obj_; }
			if (prop_text_edit_field_ >= 0 && model_picker_selected_ < (int)filtered.size()) {
				const auto& chosen = filtered[model_picker_selected_];
				prop_text_buf_ = chosen.modelId;
				pending_model_source_level_ = chosen.sourceLevel;
				prop_text_caret_ = (int)prop_text_buf_.size();
				CommitPropTextEdit();              // apply the chosen model to the field
			}
			picker_target_field_ = -1; picker_target_obj_ = -1;
			return;
		}
		if (key == 8) {
			if (!model_picker_filter_.empty()) { model_picker_filter_.pop_back(); model_picker_selected_ = 0; model_picker_scroll_ = 0; }
		} else if (key >= 32 && key <= 126) {
			model_picker_filter_ += (char)key;
			model_picker_selected_ = 0;
			model_picker_scroll_ = 0;
		}
		return;
	}

	// C3: Find bar input
	if (find_open_) {
		if (key == 27) { // ESC — close
			find_open_ = false;
			find_query_.clear();
			find_result_idx_ = -1;
			return;
		}
		if (key == 13) { // Enter — confirm
			if (find_mode_ == FindMode::SetId) {
				// Empty query → automatic unique ID; otherwise set the typed ID.
				std::string q = find_query_;
				while (!q.empty() && (q.front() == ' ' || q.front() == '\t')) q.erase(q.begin());
				while (!q.empty() && (q.back()  == ' ' || q.back()  == '\t')) q.pop_back();
				if (selected_object_index_ >= 0) {
					if (q.empty()) {
						AssignTaskID();
					} else {
						auto& objects = level_.GetLevelObjects().GetObjects();
						objects[selected_object_index_].taskId = q;
						objects[selected_object_index_].modified = true;
						level_.GetLevelObjects().UpdateCoordinatesInLine(objects[selected_object_index_]);
						SaveAndReloadObjects();
						auto& reloaded = level_.GetLevelObjects().GetObjects();
						if (!reloaded.empty()) selected_object_index_ = std::min(selected_object_index_, (int)reloaded.size() - 1);
						status_message_ = "Set Task ID: " + q;
					}
				}
				find_open_ = false; find_query_.clear(); find_result_idx_ = -1;
				return;
			}
			if (find_result_idx_ >= 0) {
				selected_object_index_ = find_result_idx_;
				// Expand all ancestor containers so the found item is visible in tree
				{
					auto& objects = level_.GetLevelObjects().GetObjects();
					int idx = find_result_idx_;
					while (idx >= 0 && idx < (int)objects.size()) {
						int parent = objects[idx].parentIndex;
						if (parent < 0 || parent >= (int)objects.size()) break;
						if (objects[parent].isContainer)
							objects[parent].expanded = true;
						idx = parent;
					}
				}
				// Scroll the tree to make the found item visible
				auto visibleList = GetVisibleTreeNodes();
				int current_row = -1;
				for (int i = 0; i < (int)visibleList.size(); ++i) {
					if (visibleList[i] == find_result_idx_) {
						current_row = i;
						break;
					}
				}
				if (current_row >= 0) {
					int row_h = CurrentUiRowHeight();
					int start_y = 30;
					int max_rows = (window_state_.viewport_height_ - 50 - start_y) / row_h;
					if (max_rows > 0) {
						if (current_row < tree_scroll_offset_)
							tree_scroll_offset_ = current_row;
						else if (current_row >= tree_scroll_offset_ + max_rows)
							tree_scroll_offset_ = current_row - max_rows + 1;
					}
				}
			}
			find_open_ = false;
			// Keep find_query_ + find_result_idx_ so TaskFindAgain (Ctrl+Shift+F)
			// can cycle through matches without reopening the search bar.
			return;
		}
		if (key == 8) { // Backspace
			if (!find_query_.empty())
				find_query_.pop_back();
		} else if (key >= 32 && key <= 126) {
			find_query_ += (char)key;
		}
		// Search (respects find_mode_). SetId is input-only — no live search.
		if (!find_query_.empty() && find_mode_ != FindMode::SetId) {
			std::string q_lower = find_query_;
			std::transform(q_lower.begin(), q_lower.end(), q_lower.begin(), [](unsigned char c){ return std::tolower(c); });
			const auto& objects = level_.GetLevelObjects().GetObjects();
			find_result_idx_ = -1;
			for (int i = 0; i < (int)objects.size(); ++i) {
				if (objects[i].deleted) continue;
				if (objects[i].type == "Task_DeclareParameters") continue;
				std::string label;
				if (find_mode_ == FindMode::TextInTask) {
					for (auto& tok : objects[i].argTokens) label += tok + " ";
				} else if (find_mode_ == FindMode::ById) {
					label = objects[i].taskId;
				} else if (find_mode_ == FindMode::ByNote) {
					label = objects[i].name;
				} else {
					label = objects[i].type + " " + objects[i].name + " " + objects[i].taskId;
				}
				std::transform(label.begin(), label.end(), label.begin(), [](unsigned char c){ return std::tolower(c); });
				// ById requires exact match — task IDs are pixel-perfect, so
				// typing "7" must not also match "73", "700", etc. All other
				// find modes keep substring matching.
				const bool is_match = (find_mode_ == FindMode::ById)
					? (label == q_lower)
					: (label.find(q_lower) != std::string::npos);
				if (is_match) {
					find_result_idx_ = i;
					break;
				}
			}
		} else {
			find_result_idx_ = -1;
		}
		return;
	}

	if (task_picker_open_) {
		if (key == 27) { // ESC - Cancel and close
			task_picker_open_ = false;
			return;
		}
		if (key == 13) { // Enter - Confirm selection
			auto& objects = level_.GetLevelObjects().GetObjects();
			std::vector<int> picker_to_objects;
			
			std::string search_lower = task_picker_search_;
			std::transform(search_lower.begin(), search_lower.end(), search_lower.begin(), [](unsigned char c) { return std::tolower(c); });
			
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
					std::transform(label_lower.begin(), label_lower.end(), label_lower.begin(), [](unsigned char c) { return std::tolower(c); });

					if (search_lower.empty() || label_lower.find(search_lower) != std::string::npos) {
						picker_to_objects.push_back(i);
					}
				}
			}

			if (task_picker_selected_idx_ >= 0 && task_picker_selected_idx_ < (int)picker_to_objects.size()) {
				if (selected_object_index_ < 0 || selected_object_index_ >= (int)objects.size()) {
					status_message_ = "Error: Must select a valid parent task first.";
					Logger::Get().Log(LogLevel::WARNING, "[App] Validation failed: Parent index is invalid.");
					task_picker_open_ = false;
					return;
				}

				int sourceIdx = picker_to_objects[task_picker_selected_idx_];
				
				std::vector<LevelObject> temp_clipboard;
				std::function<void(int, int)> copy_recurse = [&](int idx, int newParentInTemp) {
					if (idx < 0 || idx >= (int)objects.size()) return;
					
					LevelObject copy = objects[idx];
					copy.childrenIndices.clear();
					copy.parentIndex = newParentInTemp;
					copy.modified = true;
					copy.qscLine.clear();

					// Clear IDs, model names, and notes (keeping position/rotation as default)
					copy.taskId = "-1";
					copy.name = "";
					copy.original_name = "";
					copy.has_original_name = false;
					copy.modelId = "";
					copy.aiId = "";
					copy.graphId = "";
					copy.graphName = "";
					copy.primaryWeapon = "";
					copy.primaryAmmo = "";
					copy.secondaryWeapon = "";
					copy.secondaryAmmo = "";
					copy.segmentModelId = "";
					copy.secondaryModelId = "";
					copy.lensModelId = "";
					copy.splineTaskId = "";
					copy.isAttaProxy = false;
					copy.attaRecordIndex = -1;
					copy.attaParentModelId = "";

					// Synchronize cleared fields into the argTokens
					level_.GetLevelObjects().UpdateCoordinatesInLine(copy);
					
					int tempIdx = (int)temp_clipboard.size();
					temp_clipboard.push_back(copy);
					
					if (newParentInTemp != -1) {
						temp_clipboard[newParentInTemp].childrenIndices.push_back(tempIdx);
					}
					
					// Do not recursively copy children for new tasks inserted from the picker
					/*
					for (int childIdx : objects[idx].childrenIndices) {
						copy_recurse(childIdx, tempIdx);
					}
					*/
				};
				
				copy_recurse(sourceIdx, -1);

				if (!ValidateParentChildCompatibility(objects[selected_object_index_], temp_clipboard)) {
					status_message_ = "Error: Cannot add Computer to a WaterTower.";
					Logger::Get().Log(LogLevel::WARNING, "[App] Validation failed: Cannot add Computer task to WaterTower parent.");
					task_picker_open_ = false;
					return;
				}
				
				int targetParent = selected_object_index_;
				int startIdxInObjects = (int)objects.size();

				// Collect all in-use task IDs for unique ID generation
				std::set<int> usedIds;
				for (const auto& obj : objects) {
					if (obj.deleted) continue;
					if (obj.taskId.empty() || obj.taskId == "-1") continue;
					try { usedIds.insert(std::stoi(obj.taskId)); } catch (...) {}
				}

				int levelNo = level_.GetLevelNo();
				std::string aiDir = Utils::GetIGIRootPath() + "\\missions\\location0\\level" + std::to_string(levelNo) + "\\ai";

				for (size_t i = 0; i < temp_clipboard.size(); ++i) {
					LevelObject pasted = temp_clipboard[i];

					if (pasted.parentIndex == -1) {
						pasted.parentIndex = targetParent;
						if (targetParent != -1) {
							if (task_picker_insert_first_)
								objects[targetParent].childrenIndices.insert(
									objects[targetParent].childrenIndices.begin(), (int)objects.size());
							else
								objects[targetParent].childrenIndices.push_back((int)objects.size());
							objects[targetParent].modified = true;
						}
					} else {
						pasted.parentIndex += startIdxInObjects;
					}
					
					for (size_t j = 0; j < pasted.childrenIndices.size(); ++j) {
						pasted.childrenIndices[j] += startIdxInObjects;
					}
					
					if (pasted.qscFuncName == "Task_New") {
						if (pasted.type == "HumanSoldier" || pasted.type == "HumanSoldierFemale" || pasted.type == "HumanAI") {
							std::string oldId = pasted.taskId;

							// Find next available unique ID (ensuring no file conflict in AI directory)
							int newId = 1;
							while (true) {
								if (usedIds.count(newId) == 0) {
									std::string idStr = std::to_string(newId);
									std::string qvmPath = aiDir + "\\" + idStr + ".qvm";
									std::string qscPath = aiDir + "\\" + idStr + ".qsc";
									if (!std::filesystem::exists(qvmPath) && !std::filesystem::exists(qscPath)) {
										break;
									}
								}
								newId++;
							}
							usedIds.insert(newId);

							std::string newIdStr = std::to_string(newId);
							pasted.taskId = newIdStr;
							if (!pasted.argTokens.empty()) {
								pasted.argTokens[0] = newIdStr;
							}
							pasted.qscLine.clear(); // Force regeneration from argTokens on save
							pasted.modified = true;

							// For HumanAI: create and compile a new AI script (.qsc -> .qvm)
							if (pasted.type == "HumanAI") {
								std::filesystem::create_directories(aiDir);
								std::string qscPath = aiDir + "\\" + newIdStr + ".qsc";
								std::string qvmPath = aiDir + "\\" + newIdStr + ".qvm";
								std::string qscContent = 
									"if (AIFunction_GetCurrentEventType() == AIEVENT_CREATE)\n"
									"{\n"
									"  AIFunction_DefaultHandler();\n"
									"}\n"
									"else\n"
									"{\n"
									"  AIFunction_DefaultHandler();\n"
									"}";

								std::ofstream qscFile(qscPath);
								if (qscFile) {
									qscFile << qscContent;
									qscFile.close();

									// Compile QSC to QVM
									auto lexResult  = qsc::Lex(qscContent);
									auto parseResult = lexResult.ok ? qsc::Parse(lexResult.tokens) : qsc::ParseResult{};
									std::string compileErr;
									bool success = lexResult.ok && parseResult.ok &&
									               qvm::CompileToFile(*parseResult.program, qvmPath, &compileErr);
									if (success) {
										Logger::Get().Log(LogLevel::INFO, "[App] Successfully compiled new AI script: " + qvmPath);
									} else {
										std::string detail = compileErr.empty() ? "(no detail)" : compileErr;
										Logger::Get().Log(LogLevel::ERR, "[App] Failed to compile new AI script: " + detail);
										status_message_ = "Warning: Failed to compile new AI script for Task " + newIdStr + ".";
										// Remove any partial .qvm left by a failed CompileToFile
										std::error_code ecQvm;
										if (std::filesystem::exists(qvmPath)) {
											std::filesystem::remove(qvmPath, ecQvm);
											if (ecQvm) {
												Logger::Get().Log(LogLevel::WARNING, "[App] Failed to remove partial QVM: " + qvmPath + " (" + ecQvm.message() + ")");
											}
										}
									}

									// Remove temporary qsc file
									std::error_code ec;
									std::filesystem::remove(qscPath, ec);
									if (ec) {
										Logger::Get().Log(LogLevel::WARNING, "[App] Failed to remove temp QSC: " + qscPath + " (" + ec.message() + ")");
									}
								} else {
									Logger::Get().Log(LogLevel::ERR, "[App] Failed to create temp QSC file: " + qscPath);
									status_message_ = "Warning: Failed to create temp QSC file for Task " + newIdStr + ".";
								}
							}
							Logger::Get().Log(LogLevel::INFO, "[App] Assigned unique Task ID " + newIdStr + " to newly inserted " + pasted.type);
						} else {
							pasted.taskId = "-1";
							if (!pasted.argTokens.empty()) {
								pasted.argTokens[0] = "-1";
							}
						}
					}
					
					objects.push_back(pasted);
					level_.GetLevelObjects().UpdateCoordinatesInLine(objects.back());
				}
				
				// Override position with camera location if TaskNewCameraRelative
				if (task_new_at_camera_ && startIdxInObjects < (int)objects.size()) {
					objects[startIdxInObjects].pos = glm::dvec3(viewer_.pos_);
					objects[startIdxInObjects].modified = true;
					level_.GetLevelObjects().UpdateCoordinatesInLine(objects[startIdxInObjects]);
				}
				task_picker_insert_first_ = false;
				task_new_at_camera_       = false;

				selected_object_index_ = startIdxInObjects;
				SaveAndReloadObjects();

				auto& reloaded = level_.GetLevelObjects().GetObjects();
				if (!reloaded.empty()) {
					selected_object_index_ = std::min(selected_object_index_, (int)reloaded.size() - 1);
				}
				Logger::Get().Log(LogLevel::INFO, "[App] Successfully cloned task subtree into the tree hierarchy.");
			}
			task_picker_open_ = false;
			return;
		}
		if (key == 8) { // Backspace
			if (!task_picker_search_.empty()) {
				task_picker_search_.pop_back();
				task_picker_selected_idx_ = 0;
				task_picker_scroll_offset_ = 0;
			}
			return;
		}
		if (key >= 32 && key <= 126) { // Printable characters
			if (task_picker_search_.size() < 20) {
				task_picker_search_ += (char)key;
				task_picker_selected_idx_ = 0;
				task_picker_scroll_offset_ = 0;
			}
			return;
		}
		return; // Block other keyboard input while picker is open
	}

	// C3: Ctrl+F — toggle find bar (key 6 = Ctrl+F, hardcoded for convenience)
	// Don't intercept Ctrl+Shift+F — that goes to TaskFindAgain via event bindings.
	if (key == 6 && !(glutGetModifiers() & GLUT_ACTIVE_SHIFT)) {
		find_open_       = !find_open_;
		find_mode_       = FindMode::TaskNameTypeId;
		find_query_.clear();
		find_result_idx_ = -1;
		return;
	}

	// Enter: open property editor for the selected task, same as double-click.
	// For containers also toggle expanded/collapsed.
	if (key == 13 && !(glutGetModifiers() & GLUT_ACTIVE_ALT)) {
		if (selected_object_index_ >= 0) {
			auto& objects = level_.GetLevelObjects().GetObjects();
			if (selected_object_index_ < (int)objects.size()) {
				auto& obj = objects[selected_object_index_];
				prop_editor_open_ = true; prop_panel_scroll_ = 0; prop_text_edit_field_ = -1; prop_edit_obj_index_ = -1;
				LoadAIScriptForSelected();
				if (obj.isContainer) {
					obj.expanded = !obj.expanded;
					Logger::Get().Log(LogLevel::INFO, "[App] Enter opened props + toggled expand for " + obj.type);
				} else {
					Logger::Get().Log(LogLevel::INFO, "[App] Enter opened property panel for " + obj.type);
				}
				return;
			}
		}
	}

	// Check for modifier keys - if pressed, skip movement key checks
	int modifiers = glutGetModifiers();
	bool has_modifiers = (modifiers & (GLUT_ACTIVE_CTRL | GLUT_ACTIVE_SHIFT | GLUT_ACTIVE_ALT));

	// Check movement keys (regular keyboard characters) - case insensitive
	// Only check if the config key is a regular character (ASCII < 128 and not a special key code)
	// Special key codes (arrow keys) are >= 100 and should not match regular character keys
	if (!has_modifiers) {
		// Only check movement keys if they are regular characters, not special keys
		// Special keys have codes >= 100 (GLUT_KEY_UP=101, DOWN=103, LEFT=100, RIGHT=102)
		if (config.keyMoveForward < 100 && config.keyMoveForward > 0 && toupper(key) == toupper(config.keyMoveForward)) {
			input_.keys_ |= MK_FORWARD;
			return;
		}
		if (config.keyMoveBackward < 100 && config.keyMoveBackward > 0 && toupper(key) == toupper(config.keyMoveBackward)) {
			input_.keys_ |= MK_BACKWARD;
			return;
		}
		if (config.keyMoveLeft < 100 && config.keyMoveLeft > 0 && toupper(key) == toupper(config.keyMoveLeft)) {
			input_.keys_ |= MK_LEFT;
			return;
		}
		if (config.keyMoveRight < 100 && config.keyMoveRight > 0 && toupper(key) == toupper(config.keyMoveRight)) {
			input_.keys_ |= MK_RIGHT;
			return;
		}
	}

	if (key == 27) { // ESC
		if (show_help_) { show_help_ = false; return; }
		if (prop_editor_open_) { // close the property panel — commit any edit first
			CommitPropTextEdit();
			prop_editor_open_ = false;
			prop_field_index_ = -1;
			return;
		}
		TogglePauseMenu();
		return;
	}


	// DEL key: delete selected task (hardcoded for standard keyboard ergonomics)
	if (key == 127) { DeleteSelectedTask(); return; }

	// B: toggle the animation skeleton (bone) wireframe overlay. Independent of
	// F10's status panel — bones are hidden by default and only ever drawn when
	// this is on, even while an animation is actively playing.
	if (key == 'b' || key == 'B') {
		show_anim_skeleton_ = !show_anim_skeleton_;
		Logger::Get().Log(LogLevel::INFO, std::string("[App] Animation skeleton overlay ") + (show_anim_skeleton_ ? "shown" : "hidden"));
		return;
	}

	if ((key == 13) && (glutGetModifiers() & GLUT_ACTIVE_ALT)) { // ALT + ENTER toggle full screen mode
		window_state_.full_screen_ = !window_state_.full_screen_;

		if (window_state_.full_screen_) {

			window_state_.old_viewport_width_ = window_state_.viewport_width_;
			window_state_.old_viewport_height_ = window_state_.viewport_height_;

			glutFullScreen();
		}
		else {
			glutReshapeWindow(window_state_.old_viewport_width_, window_state_.old_viewport_height_);
		}

		return;
	}

	for (int i = 0; i < count_of(MOVEMENT_KEYS); ++i) {
		const movement_key_s& mk = MOVEMENT_KEYS[i];
		if (key == mk.lower_case_ || key == mk.upper_case_) {
			input_.keys_ |= mk.key_flag_;
			return;
		}
	}

	// Object Manipulation from QED config
	if (toupper(key) == toupper(config.keyRotateAlpha)) { input_.keys_ |= MK_MANIP_A; return; }
	if (toupper(key) == toupper(config.keyRotateBeta))  { input_.keys_ |= MK_MANIP_B; return; }
	if (toupper(key) == toupper(config.keyRotateGamma)) { input_.keys_ |= MK_MANIP_G; return; }
	if (toupper(key) == toupper(config.keySnapGround))  {
        PushUndoState();
        input_.keys_ |= MK_MANIP_S;
        if (selected_object_index_ >= 0) UpdateMarkerManipulation();
        input_.keys_ &= ~MK_MANIP_S;
        return;
    }
	if (toupper(key) == toupper(config.keySnapObject))  {
        PushUndoState();
        input_.keys_ |= MK_MANIP_O;
        if (selected_object_index_ >= 0) UpdateMarkerManipulation();
        input_.keys_ &= ~MK_MANIP_O;
        return;
    }
	if (key == ' ' && !(glutGetModifiers() & GLUT_ACTIVE_ALT)) {
        PushUndoState();
        input_.keys_ |= MK_MANIP_SPACE;
        if (selected_object_index_ >= 0) UpdateMarkerManipulation();
        input_.keys_ &= ~MK_MANIP_SPACE;
        return;
    }

	if (key == 't' || key == 'T') {
		level_.TeleportToHMP(viewer_.pos_);
		viewer_.pitch_ = -30.0f; // Look down at the terrain
		UpdateViewerVectors();
		printf("Teleported to Height Map Zone\n");
		return;
	}

	// HUD toggle removed as per request



	if (key == 's' || key == 'S') {
		// Snap selected object to ground (works in any mode)
		if (selected_object_index_ >= 0) {
			auto& objects = level_.GetLevelObjects().GetObjects();
			if (selected_object_index_ < (int)objects.size()) {
				auto& obj = objects[selected_object_index_];
				if (!Utils::IsUndergroundModel(obj.name, obj.modelId)) {
					float terrainZ = 0.0f;
					if (level_.GetTerrainZ(obj.pos.x, obj.pos.y, terrainZ)) {
						float zOffset = renderer_.GetMeshZOffset(obj.modelId, obj.isBuilding);
						// Snap object bottom to terrain surface
						obj.pos.z = (double)terrainZ + (double)(zOffset * 40.96f * obj.scale);
						Logger::Get().Log(LogLevel::INFO, "[App] Snapped object to ground");
					}
				}
			}
		}
		return;
	}


	// Plain Tab cycles selection; Ctrl+I and Ctrl+Shift+I must reach DispatchEventBindings.
	if (key == '\t' && !(glutGetModifiers() & (GLUT_ACTIVE_CTRL | GLUT_ACTIVE_SHIFT | GLUT_ACTIVE_ALT))) {
		LevelObjects& lo = level_.GetLevelObjects();
		if (!lo.GetObjects().empty()) {
			selected_object_index_ = (selected_object_index_ + 1) % (int)lo.GetObjects().size();
			printf("Selected Object: %d / %d\n", selected_object_index_, (int)lo.GetObjects().size());
		}
		return;
	}

	// HandleMarkerInput(key);

	DispatchEventBindings();
}


void App::Input_OnKeyboardUp(unsigned char key, int x, int y) {
	auto& config = Config::Get();

	if (in_game_mode_ && IsGameplayInputFocused() &&
		!gameplay_host_.IsGameplayWindowCurrent()) return;
	if (in_game_mode_ && IsGameplayInputFocused() && !pause_mode_) {
		gameplay_host_.GetInputRouter().OnKeyboardKey(key, false);
		return;
	}
	if (in_game_mode_ && IsGameplayInputFocused() && pause_mode_) return;

	// Check for modifier keys - if pressed, skip movement key checks
	int modifiers = glutGetModifiers();
	bool has_modifiers = (modifiers & (GLUT_ACTIVE_CTRL | GLUT_ACTIVE_SHIFT | GLUT_ACTIVE_ALT));

	// Check movement keys (regular keyboard characters) - case insensitive
	// Only check if the config key is a regular character (ASCII < 128 and not a special key code)
	// Special key codes (arrow keys) are >= 100 and should not match regular character keys
	if (!has_modifiers) {
		// Only check movement keys if they are regular characters, not special keys
		// Special keys have codes >= 100 (GLUT_KEY_UP=101, DOWN=103, LEFT=100, RIGHT=102)
		if (config.keyMoveForward < 100 && config.keyMoveForward > 0 && toupper(key) == toupper(config.keyMoveForward)) {
			input_.keys_ &= ~MK_FORWARD;
			return;
		}
		if (config.keyMoveBackward < 100 && config.keyMoveBackward > 0 && toupper(key) == toupper(config.keyMoveBackward)) {
			input_.keys_ &= ~MK_BACKWARD;
			return;
		}
		if (config.keyMoveLeft < 100 && config.keyMoveLeft > 0 && toupper(key) == toupper(config.keyMoveLeft)) {
			input_.keys_ &= ~MK_LEFT;
			return;
		}
		if (config.keyMoveRight < 100 && config.keyMoveRight > 0 && toupper(key) == toupper(config.keyMoveRight)) {
			input_.keys_ &= ~MK_RIGHT;
			return;
		}
	}

	for (int i = 0; i < count_of(MOVEMENT_KEYS); ++i) {
		const movement_key_s& mk = MOVEMENT_KEYS[i];
		if (key == mk.lower_case_ || key == mk.upper_case_) {
			input_.keys_ &= ~mk.key_flag_;
			// Don't return, check manip keys too
		}
	}
	// Object Manipulation from QED config
	if (toupper(key) == toupper(config.keyRotateAlpha)) { input_.keys_ &= ~MK_MANIP_A; undo_state_pushed_for_manip_ = false; }
	if (toupper(key) == toupper(config.keyRotateBeta))  { input_.keys_ &= ~MK_MANIP_B; undo_state_pushed_for_manip_ = false; }
	if (toupper(key) == toupper(config.keyRotateGamma)) { input_.keys_ &= ~MK_MANIP_G; undo_state_pushed_for_manip_ = false; }
	if (toupper(key) == toupper(config.keySnapGround))  { input_.keys_ &= ~MK_MANIP_S; }
	if (toupper(key) == toupper(config.keySnapObject))  { input_.keys_ &= ~MK_MANIP_O; }
}

// idle
