/******************************************************************************
 * @file    app_editor.cpp
 * @brief   App editing/workflow: property + AI-script editing, undo/redo, task
 *          tree ops, QSC load/compile/decompile, attach promotion, game launch.
 *          Split from app.cpp; shares app_internal.h.
 *****************************************************************************/
#include "app_internal.h"
#include "runtime/auto_save_policy.h"
#include "runtime/editor_history.h"
#include "runtime/lightmap_recalc_policy.h"

std::string App::StripQuotes(const std::string& s) {
	if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
		return s.substr(1, s.size() - 2);
	return s;
}

// True if the field currently being text-edited is a multi-line box.
bool App::IsPropFieldMultiline(int field) const {
	if (field == PropPanel::kAIScriptTextField) return true;
	if (field < 0) return false; // note box (-2) is single-line
	int oi = (prop_edit_obj_index_ >= 0) ? prop_edit_obj_index_ : selected_object_index_;
	if (oi < 0) return false;
	const auto& objects = level_.GetLevelObjects().GetObjects();
	if (oi >= (int)objects.size()) return false;
	const TaskSchema* scp = GetSchema(objects[oi].type);
	if (!scp) return false;
	int fi = field / 3;
	if (fi < 0 || fi >= (int)scp->size()) return false;
	const std::string& tn = (*scp)[fi].typeName;
	return tn == "VarString" || tn == "String256";
}

void App::UpdateAIScriptScroll() {
	if (prop_text_edit_field_ != PropPanel::kAIScriptTextField) return;
	const int mc = AiScriptMaxChars(), box_lines = 12;
	auto starts = AiTextLineStarts(prop_text_buf_, mc);
	int cl = (int)(std::upper_bound(starts.begin(), starts.end(), prop_text_caret_) - starts.begin()) - 1;
	cl = std::max(0, std::min(cl, (int)starts.size() - 1));
	if (cl < ai_script_vscroll_)
		ai_script_vscroll_ = cl;
	else if (cl >= ai_script_vscroll_ + box_lines)
		ai_script_vscroll_ = cl - box_lines + 1;
}

void App::UpdateAIScriptPathHScroll() {
	if (prop_text_edit_field_ != PropPanel::kAIScriptPathField) return;
	const int mc = AiScriptMaxChars();
	if (prop_text_caret_ < ai_script_path_hscroll_)
		ai_script_path_hscroll_ = prop_text_caret_;
	if (prop_text_caret_ >= ai_script_path_hscroll_ + mc)
		ai_script_path_hscroll_ = prop_text_caret_ - mc + 1;
}

// ── AI Script editor — notepad-style helpers ───────────────────────────────
// Text selection is shared by the property fields. The AI Script text field
// additionally uses the full notepad surface (Ctrl+C/V/X/A/Z/Y + mouse drag).
bool App::HasPropTextSelection() const {
	return prop_text_sel_anchor_ >= 0 && prop_text_sel_focus_ >= 0
		&& prop_text_sel_anchor_ != prop_text_sel_focus_;
}

void App::GetPropTextSelection(int& selStart, int& selEnd) const {
	if (!HasPropTextSelection()) { selStart = selEnd = -1; return; }
	selStart = std::min(prop_text_sel_anchor_, prop_text_sel_focus_);
	selEnd   = std::max(prop_text_sel_anchor_, prop_text_sel_focus_);
}

void App::SelectPropTextAll() {
	if (prop_text_edit_field_ == -1) return;
	prop_text_sel_anchor_ = 0;
	prop_text_sel_focus_  = (int)prop_text_buf_.size();
	// Move caret to the end so subsequent typing replaces the selection.
	prop_text_caret_ = (int)prop_text_buf_.size();
	if (IsAIScriptTextFocused()) UpdateAIScriptScroll();
}

void App::PushAiTextUndo() {
	if (!IsAIScriptTextFocused()) return;
	AiTextEdit e;
	e.before      = prop_text_buf_;
	e.caret_before = prop_text_caret_;
	e.anchor_before = prop_text_sel_anchor_;
	ai_text_undo_.push_back(std::move(e));
	if ((int)ai_text_undo_.size() > kAiTextUndoMax)
		ai_text_undo_.erase(ai_text_undo_.begin());
	// Any new edit invalidates the redo stack (standard editor semantics).
	ai_text_redo_.clear();
}

void App::AiScriptDeleteSelection() {
	if (!IsAIScriptTextFocused()) return;
	if (!HasPropTextSelection()) return;
	PushAiTextUndo();
	int s, e; GetPropTextSelection(s, e);
	prop_text_buf_.erase(s, e - s);
	prop_text_caret_ = s;
	ClearPropTextSelection();
	ai_script_text_  = prop_text_buf_;
	ai_script_dirty_ = true;
	UpdateAIScriptScroll();
}

void App::AiScriptInsertText(const std::string& s) {
	if (!IsAIScriptTextFocused() || s.empty()) return;
	// Replace any active selection (single undo entry covers the whole op).
	if (HasPropTextSelection()) {
		PushAiTextUndo();
		int a, b; GetPropTextSelection(a, b);
		prop_text_buf_.erase(a, b - a);
		prop_text_caret_ = a;
		ClearPropTextSelection();
	} else {
		PushAiTextUndo();
	}
	prop_text_buf_.insert(prop_text_caret_, s);
	prop_text_caret_ += (int)s.size();
	ai_script_text_  = prop_text_buf_;
	ai_script_dirty_ = true;
	UpdateAIScriptScroll();
}

void App::AiScriptCopy() {
	if (!IsAIScriptTextFocused()) return;
	std::string txt;
	if (HasPropTextSelection()) {
		int s, e; GetPropTextSelection(s, e);
		txt = prop_text_buf_.substr(s, e - s);
	} else {
		// No selection → copy entire buffer (Notepad/most editors do this).
		txt = prop_text_buf_;
	}
	if (!txt.empty()) {
		Utils::SetClipboardText(txt);
		status_message_ = "Copied " + std::to_string(txt.size()) + " chars to clipboard";
	}
}

void App::AiScriptCut() {
	if (!IsAIScriptTextFocused()) return;
	if (HasPropTextSelection()) {
		int s, e; GetPropTextSelection(s, e);
		Utils::SetClipboardText(prop_text_buf_.substr(s, e - s));
		AiScriptDeleteSelection();
		status_message_ = "Cut selection to clipboard";
	} else {
		// No selection → cut the whole buffer (matches Notepad's Ctrl+X).
		Utils::SetClipboardText(prop_text_buf_);
		PushAiTextUndo();
		prop_text_buf_.clear();
		prop_text_caret_ = 0;
		ClearPropTextSelection();
		ai_script_text_  = prop_text_buf_;
		ai_script_dirty_ = true;
		status_message_ = "Cut entire script to clipboard";
	}
}

void App::AiScriptPaste() {
	if (!IsAIScriptTextFocused()) return;
	const std::string clip = Utils::GetClipboardText();
	if (clip.empty()) return;
	AiScriptInsertText(clip);
	status_message_ = "Pasted " + std::to_string(clip.size()) + " chars from clipboard";
}

void App::AiScriptUndo() {
	if (!IsAIScriptTextFocused()) return;
	if (ai_text_undo_.empty()) { status_message_ = "Nothing to undo"; return; }
	// Move the current state onto the redo stack so Ctrl+Y can re-apply it.
	AiTextEdit redo;
	redo.before       = prop_text_buf_;
	redo.caret_before = prop_text_caret_;
	redo.anchor_before = prop_text_sel_anchor_;
	ai_text_redo_.push_back(std::move(redo));
	AiTextEdit e = std::move(ai_text_undo_.back());
	ai_text_undo_.pop_back();
	prop_text_buf_    = e.before;
	prop_text_caret_  = e.caret_before;
	prop_text_sel_anchor_ = e.anchor_before;
	prop_text_sel_focus_  = -1; // focus was implicit (= caret) before
	ai_script_text_  = prop_text_buf_;
	ai_script_dirty_ = true;
	UpdateAIScriptScroll();
	status_message_ = "Undo (" + std::to_string(ai_text_undo_.size()) + " left)";
}

void App::AiScriptRedo() {
	if (!IsAIScriptTextFocused()) return;
	if (ai_text_redo_.empty()) { status_message_ = "Nothing to redo"; return; }
	AiTextEdit undo;
	undo.before       = prop_text_buf_;
	undo.caret_before = prop_text_caret_;
	undo.anchor_before = prop_text_sel_anchor_;
	ai_text_undo_.push_back(std::move(undo));
	AiTextEdit e = std::move(ai_text_redo_.back());
	ai_text_redo_.pop_back();
	prop_text_buf_    = e.before;
	prop_text_caret_  = e.caret_before;
	prop_text_sel_anchor_ = e.anchor_before;
	prop_text_sel_focus_  = -1;
	ai_script_text_  = prop_text_buf_;
	ai_script_dirty_ = true;
	UpdateAIScriptScroll();
	status_message_ = "Redo (" + std::to_string(ai_text_redo_.size()) + " left)";
}

void App::LoadAIScriptForSelected() {
	// AI text history is scoped to the selected script. A different object may
	// have a different file and caret state, so cross-script undo is unsafe.
	igi::ClearEditorHistory(ai_text_undo_, ai_text_redo_);
	if (ai_script_dirty_)
		status_message_ = "Warning: unsaved AI script edits discarded (save level first)";
	ai_script_path_.clear();
	ai_script_text_.clear();
	ai_script_dirty_        = false;
	ai_script_vscroll_      = 0;
	ai_script_path_hscroll_ = 0;

	if (selected_object_index_ < 0) return;
	const auto& objects = level_.GetLevelObjects().GetObjects();
	if (selected_object_index_ >= (int)objects.size()) return;
	const auto& obj = objects[selected_object_index_];

	// Only AI model types get the script section. Check both the modelId
	// (against the AITYPE_ set from IGIModels.json) AND the object type —
	// HumanSoldier/HumanSoldierFemale/HumanPlayer/HumanSoldierRPG are always
	// AI containers even if their modelId isn't tagged AITYPE_ in the JSON.
	bool isAiType = (obj.type == "HumanSoldier" || obj.type == "HumanSoldierFemale" ||
	                 obj.type == "HumanPlayer" || obj.type == "HumanSoldierRPG" ||
	                 obj.type == "HumanAI");
	bool isAiModel = (ai_model_ids_.find(obj.modelId) != ai_model_ids_.end());
	if (!isAiType && !isAiModel) return;

	// The .qvm belongs to the HumanAI child task (not the HumanSoldier parent).
	// If this object IS the HumanAI, use its own ID; otherwise walk the children
	// recursively to find a HumanAI — the AI task can be nested several levels
	// deep (e.g. HumanSoldier → Gun → HumanAI). Limit the search depth so a
	// malformed tree doesn't hang the editor.
	const LevelObject* aiTask = nullptr;
	if (obj.type == "HumanAI") {
		aiTask = &obj;
	} else {
		const int kMaxAIDepth = 15;
		std::vector<int> frontier = obj.childrenIndices;
		int depth = 0;
		while (!frontier.empty() && depth < kMaxAIDepth) {
			std::vector<int> next;
			for (int ci : frontier) {
				if (ci < 0 || ci >= (int)objects.size()) continue;
				if (objects[ci].deleted) continue;
				if (objects[ci].type == "HumanAI") {
					aiTask = &objects[ci];
					frontier.clear();
					break;
				}
				for (int gc : objects[ci].childrenIndices) next.push_back(gc);
			}
			if (aiTask) break;
			frontier = std::move(next);
			++depth;
		}
		if (!aiTask) {
			Logger::Get().Log(LogLevel::WARNING,
				"[App] HumanAI not found under HumanSoldier '" + obj.name +
				"' (taskId=" + obj.taskId + ") within " + std::to_string(kMaxAIDepth) +
				" levels of children — AI script not loaded");
		}
	}
	if (!aiTask || aiTask->taskId.empty()) {
		if (aiTask)
			Logger::Get().Log(LogLevel::WARNING,
				"[App] HumanAI found but taskId is empty for '" + obj.name +
				"' (taskId=" + obj.taskId + ") — AI script not loaded");
		return;
	}

	int levelNo = level_.GetLevelNo();
	std::string aiDir = Utils::GetIGIRootPath() +
	                    "\\missions\\location0\\level" + std::to_string(levelNo) + "\\ai";
	ai_script_path_ = aiDir + "\\" + aiTask->taskId + ".qvm";
	Logger::Get().Log(LogLevel::INFO,
		"[App] AI script loaded for " + obj.type + " '" + obj.name +
		"' (taskId=" + obj.taskId + ") -> HumanAI taskId=" + aiTask->taskId +
		" path=" + ai_script_path_);

	if (!std::filesystem::exists(ai_script_path_)) {
		ai_script_text_ = "// .qvm not found: " + ai_script_path_;
		return;
	}
	QVMFile qvm = QVM_Parse(ai_script_path_);
	if (!qvm.valid) {
		ai_script_text_ = "// decompile failed (invalid QVM): " + ai_script_path_;
		return;
	}
	ai_script_text_ = QVM_DecompileToString(qvm);
}

// igi1conv's resolver expects lightmaps/lightmaps_unpacked/ to already exist next
// to objects.qsc; older levels only ship the packed lightmaps.res. Unpack it
// ourselves first so resolve doesn't fail with "no .olm files on disk".
bool EnsureLightmapsUnpacked(const std::string& levelDir, std::string& err) {
	const std::string lightmapsDir = levelDir + "\\lightmaps";
	const std::string unpackedDir = lightmapsDir + "\\lightmaps_unpacked";
	const std::string packedRes = lightmapsDir + "\\lightmaps.res";
	bool unpackedHasFiles = std::filesystem::exists(unpackedDir) &&
		!std::filesystem::is_empty(unpackedDir);
	if (unpackedHasFiles || !std::filesystem::exists(packedRes)) return true;

	Logger::Get().Log(LogLevel::INFO, "[Lightmap] " + unpackedDir +
		" missing/empty — unpacking " + packedRes);
	std::filesystem::create_directories(unpackedDir);
	size_t unpackedCount = 0;
	bool unpackOk = RES_ForEachEntry(packedRes,
		[&](const std::string& name, const uint8_t* data, size_t size) {
			if (size == 0) return;
			// Entry names can include subdirectory components (see
			// AssetExtractor::ExtractResIfNeeded) — flatten to just the filename so
			// ofstream doesn't silently fail opening a nonexistent subdirectory path.
			std::string filename = std::filesystem::path(name).filename().string();
			if (filename.empty()) return;
			std::ofstream out(unpackedDir + "\\" + filename, std::ios::binary);
			if (out) {
				out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
				++unpackedCount;
			} else {
				Logger::Get().Log(LogLevel::WARNING, "[Lightmap] Cannot write unpacked file: " + unpackedDir + "\\" + filename);
			}
		}, err);
	if (!unpackOk) return false;
	Logger::Get().Log(LogLevel::INFO, "[Lightmap] Unpacked " + std::to_string(unpackedCount) +
		" file(s) into " + unpackedDir);
	return true;
}

// Resolves + converts + uploads the lightmap textures for one Building/EditRigidObj,
// given an already-decompiled objects.qsc sitting next to the real lightmaps/ dir.
// Returns the number of textures uploaded (0 = no binding / all conversions failed).
size_t App::ResolveAndApplyLightmap(LevelObject& obj, const std::string& qscPath,
                                    bool olm_from_recalc) {
	// taskId="-1" (nested/ATTA tasks) can't be disambiguated by task id — resolve
	// by the object's authored position instead. The stored lightmap is keyed the
	// same way (LightmapTaskKey) so the renderer finds it.
	const bool byPos = (obj.taskId.empty() || obj.taskId == "-1");
	const std::string key = LightmapTaskKey(obj);
	std::string err;
	std::vector<std::string> olmPaths = byPos
		? igi1conv::LightmapResolveByPos(obj.modelId, qscPath, obj.original_pos.x, obj.original_pos.y, obj.original_pos.z, err)
		: igi1conv::LightmapResolve(obj.modelId, qscPath, obj.taskId, err);
	if (olmPaths.empty()) {
		// Benign cases (no baked lightmap for this object, ATTA children, cross-level refs, etc.)
		// are expected and very common — keep logs quiet.
		const bool benign =
		    err.find("no bindings found") != std::string::npos ||
		    err.find("no placement of") != std::string::npos ||
		    err.find("--model is required") != std::string::npos ||
		    err.find("no .olm file paths") != std::string::npos;
		Logger::Get().Log(benign ? LogLevel::INFO : LogLevel::WARNING,
		    "[Lightmap] resolve " + std::string(benign ? "(no lightmap) " : "failed ") + "for " + key + ": " + err);
		return 0;
	}

	// Decode each .olm straight to a GL texture in-process (no per-file subprocess).
	std::vector<GLuint> textures;
	textures.reserve(olmPaths.size());
	for (const auto& olmPath : olmPaths) {
		std::string loadErr;
		GLuint tex = LoadOlmAsTexture(olmPath, loadErr);
		if (tex == 0) {
			Logger::Get().Log(LogLevel::ERR, "[Lightmap] .olm load failed for " + olmPath + ": " + loadErr);
		}
		textures.push_back(tex);
	}

	size_t uploaded = std::count_if(textures.begin(), textures.end(), [](GLuint t) { return t != 0; });
	Logger::Get().Log(LogLevel::INFO, "[Lightmap] Uploaded " + std::to_string(uploaded) + "/" +
		std::to_string(textures.size()) + " lightmap texture(s) for " + key);
	// Log first 16 OLM paths so we can verify submesh→OLM assignment order
	if (key == "1117") {
		for (size_t i = 0; i < std::min(olmPaths.size(), size_t(16)); ++i) {
			auto fn = olmPaths[i].substr(olmPaths[i].find_last_of("\\/") + 1);
			Logger::Get().Log(LogLevel::INFO, "[Lightmap][1117] olm[" + std::to_string(i) + "] = " + fn);
		}
	}
	// Record the pose the bound .olm was baked at: the authored QSC pose for
	// shipped/reloaded bakes, or the live pose when the .olm was just
	// recalculated toward it. Recording the live pose unconditionally made the
	// next recalc compute a zero rotation delta (an identity no-op).
	glm::dvec3 prevBakedPos, prevBakedRot;
	const bool hasBake = renderer_.GetLightmapBakePose(key, prevBakedPos, prevBakedRot);
	const igi::LightmapBakeOrigin bake_origin{
	    hasBake, prevBakedPos, prevBakedRot,
	    obj.original_pos, obj.original_rot, obj.pos, glm::dvec3(obj.rot)};
	renderer_.SetLightmapForTask(key, std::move(textures),
	                             igi::RecordedBakePos(bake_origin, olm_from_recalc),
	                             igi::RecordedBakeRot(bake_origin, olm_from_recalc));
	return uploaded;
}

void App::CalculateLightmapForSelectedObject() {
	auto& objects = level_.GetLevelObjects().GetObjects();
	if (selected_object_index_ < 0 || selected_object_index_ >= (int)objects.size()) {
		Logger::Get().Log(LogLevel::WARNING, "[Lightmap] No object selected.");
		return;
	}
	LevelObject& obj = objects[selected_object_index_];
	if (obj.type != "Building" && obj.type != "EditRigidObj" && obj.type != "EditObj") {
		Logger::Get().Log(LogLevel::WARNING, "[Lightmap] \"" + obj.type + "\" objects don't carry lightmap bindings.");
		return;
	}
	// Note: taskId="-1" (nested/ATTA tasks, real placed objects with no unique
	// QSC task id) is NOT rejected here — ResolveAndApplyLightmap below resolves
	// those by authored position instead (LightmapResolveByPos) and keys them by
	// a stable position-derived id (LightmapTaskKey), so Calculate works for them.

	// igi1conv resolves a binding's .olm directory as a SIBLING of the --qsc
	// path's own directory (lightmaps/lightmaps_unpacked next to objects.qsc).
	// The editor's live qsc_path_ is a decompiled WORKING COPY under
	// editor\qed\temp\, not the real level directory, so passing it directly
	// makes igi1conv look for (and fail to find/unpack) lightmaps/ in the wrong
	// place. Decompile a fresh copy straight into the real level directory
	// instead, so the relative lookup resolves correctly, then delete it.
	const std::string levelDir = Utils::GetIGIRootPath() + "\\missions\\location0\\level" +
		std::to_string(level_.GetLevelNo());
	const std::string qvmPath = levelDir + "\\objects.qvm";
	const std::string qscPath = levelDir + "\\objects_lightmap_tmp.qsc";

	status_message_ = "Lightmap: calculating...";
	DrawProgressOverlay("Calculating Lightmap", 10, "resolving binding");

	std::string unpackErr;
	if (!EnsureLightmapsUnpacked(levelDir, unpackErr)) {
		status_message_ = "Lightmap: failed to unpack lightmaps.res: " + unpackErr;
		Logger::Get().Log(LogLevel::WARNING, "[Lightmap] unpack failed: " + unpackErr);
		return;
	}

	DrawProgressOverlay("Calculating Lightmap", 30, "decompiling objects.qvm");
	std::string decompileErr;
	if (!igi1conv::QvmDecompile(qvmPath, qscPath, decompileErr)) {
		status_message_ = "Lightmap: " + decompileErr;
		Logger::Get().Log(LogLevel::WARNING, "[Lightmap] decompile failed: " + decompileErr);
		return;
	}

	// Step 1: load the current .olm from disk so bake pose is registered.
	Logger::Get().Log(LogLevel::INFO, "[Lightmap] Resolving lightmap for model=" + obj.modelId +
		" taskId=" + obj.taskId + " qsc=" + qscPath);
	DrawProgressOverlay("Calculating Lightmap", 55, "loading current .olm");
	size_t uploaded = ResolveAndApplyLightmap(obj, qscPath);
	const std::vector<int>& children = obj.childrenIndices;
	for (size_t ci = 0; ci < children.size(); ++ci) {
		int idx = children[ci];
		if (idx < 0 || idx >= (int)objects.size()) continue;
		uploaded += ResolveAndApplyLightmap(objects[idx], qscPath);
	}

	if (uploaded == 0) {
		std::error_code qscEc; std::filesystem::remove(qscPath, qscEc);
		status_message_ = "Lightmap: no lightmap textures resolved for this object";
		return;
	}

	// Step 2: recalc the .olm files using the object's CURRENT position/rotation
	// and the live sun direction/colour. This writes updated .olm files to disk.
	DrawProgressOverlay("Calculating Lightmap", 70, "recalculating .olm with current sun/position");
	const std::string lightmapsDir = levelDir + "\\lightmaps";
	const std::string resPath      = lightmapsDir + "\\lightmaps.res";
	int baked = 0;
	if (RecalcLightmapToOlm(obj, qscPath, /*force=*/true)) ++baked;
	for (int ci : obj.childrenIndices) {
		if (ci >= 0 && ci < (int)objects.size())
			if (RecalcLightmapToOlm(objects[ci], qscPath, /*force=*/true)) ++baked;
	}

	if (baked > 0) {
		DrawProgressOverlay("Calculating Lightmap", 80, "reloading into viewport");
		ResolveAndApplyLightmap(obj, qscPath, /*olm_from_recalc=*/true);
		for (int ci : obj.childrenIndices) {
			if (ci >= 0 && ci < (int)objects.size())
				ResolveAndApplyLightmap(objects[ci], qscPath, /*olm_from_recalc=*/true);
		}
	}

	// Step 4: repack lightmaps.res so the game also picks up the new .olm.
	if (baked > 0 && std::filesystem::exists(resPath)) {
		DrawProgressOverlay("Calculating Lightmap", 90, "repacking lightmaps.res");
		const std::string unpackedDir = lightmapsDir + "\\lightmaps_unpacked";
		const std::string tmpRes      = lightmapsDir + "\\lightmaps_new.res";
		std::string repackErr;
		if (igi1conv::ResRepack(resPath, unpackedDir, tmpRes, repackErr)) {
			std::error_code ec;
			std::filesystem::rename(tmpRes, resPath, ec);
			if (ec) {
				std::filesystem::copy_file(tmpRes, resPath, std::filesystem::copy_options::overwrite_existing, ec);
				std::error_code ec2; std::filesystem::remove(tmpRes, ec2);
			}
			if (!ec) Logger::Get().Log(LogLevel::INFO, "[Lightmap] Repacked lightmaps.res with " +
				std::to_string(baked) + " new .olm(s)");
		} else {
			Logger::Get().Log(LogLevel::WARNING, "[Lightmap] Repack failed: " + repackErr);
		}
	}

	std::error_code qscEc; std::filesystem::remove(qscPath, qscEc);

	DrawProgressOverlay("Calculating Lightmap", 100, "done");
	status_message_ = baked > 0
		? "Lightmap recalculated + saved: " + std::to_string(uploaded) + " texture(s)"
		: "Lightmap applied: " + std::to_string(uploaded) + " texture(s) (no pose change to bake)";
}

// Bake the current orientation's live re-light into this object's .olm files (on
// disk, in lightmaps_unpacked) so the GAME shows what the editor shows. Returns
// true if .olm files were rewritten. Used by the Save write-back to update every
// object that was moved/rotated since its lightmap was applied. Requires a real
// (non "-1") task id — the converter's recalc disambiguates by task id.
bool App::RecalcLightmapToOlm(LevelObject& obj, const std::string& qscPath, bool force) {
	if (obj.taskId.empty() || obj.taskId == "-1") return false; // recalc CLI needs a real task id
	const std::string key = LightmapTaskKey(obj);
	glm::dvec3 bakedPos, bakedRot;
	const bool hasBake = renderer_.GetLightmapBakePose(key, bakedPos, bakedRot);
	// --rot-orig must be the pose the .olm on disk was baked at: the recorded
	// bake pose, or the authored QSC pose when nothing is recorded. Using the
	// live pose for both ends made the delta zero and the recalc a no-op.
	const igi::LightmapBakeOrigin bake_origin{
	    hasBake, bakedPos, bakedRot,
	    obj.original_pos, obj.original_rot, obj.pos, glm::dvec3(obj.rot)};
	const igi::LightmapRecalcPoses poses = igi::ComputeLightmapRecalcPoses(bake_origin);
	if (!force && glm::length(poses.new_rot - poses.orig_rot) < 0.01 &&
	    glm::length(poses.new_pos - poses.orig_pos) < 1.0) return false; // unmoved

	std::string mefPath = renderer_.GetOrExtractMefTemp(obj.modelId, obj.isBuilding);
	if (mefPath.empty()) {
		Logger::Get().Log(LogLevel::WARNING, "[Lightmap] Write-back: no .mef for model " + obj.modelId);
		return false;
	}
	std::string recalcErr;
	bool ok = igi1conv::LightmapRecalc(obj.modelId, qscPath, obj.taskId, mefPath,
		poses.orig_rot, poses.new_rot, renderer_.GetSunDir(), renderer_.GetSunFrontColor(), renderer_.GetGlobalAmbient(), recalcErr);
	if (!ok) {
		Logger::Get().Log(LogLevel::WARNING, "[Lightmap] Write-back recalc failed for taskId=" + obj.taskId + ": " + recalcErr);
		return false;
	}
	Logger::Get().Log(LogLevel::INFO, "[Lightmap] Write-back recalc baked taskId=" + obj.taskId +
		" (" + obj.modelId + ") into .olm for the game");
	return true;
}

// Called automatically when the user finishes moving/rotating a lightmapped object.
// Recalculates the object's lightmap (and all its ATTA children), reloads it into
// the editor viewport, and repacks lightmaps.res so the game sees the change too.
void App::AutoRecalcLightmapForManipulated(int objIndex) {
	auto& objects = level_.GetLevelObjects().GetObjects();
	if (objIndex < 0 || objIndex >= (int)objects.size()) return;

	LevelObject& obj = objects[objIndex];
	if (obj.type != "Building" && obj.type != "EditRigidObj") return;
	if (!renderer_.HasLightmapForTask(LightmapTaskKey(obj))) return;
	// IsLightmapStale returns false when there's no bake pose (never moved from loaded pose).
	// Still proceed if the object has been modified since load — use obj.modified as fallback.
	bool stale = renderer_.IsLightmapStale(LightmapTaskKey(obj), obj.pos, obj.rot);
	if (!stale && !obj.modified) return; // definitely not moved

	const std::string levelDir = Utils::GetIGIRootPath() + "\\missions\\location0\\level" +
		std::to_string(level_.GetLevelNo());
	const std::string lightmapsDir  = levelDir + "\\lightmaps";
	const std::string unpackedDir   = lightmapsDir + "\\lightmaps_unpacked";
	const std::string resPath       = lightmapsDir + "\\lightmaps.res";

	if (!std::filesystem::exists(resPath)) return; // level has no lightmaps

	status_message_ = "Lightmap: recalculating...";
	Logger::Get().Log(LogLevel::INFO, "[Lightmap] AutoRecalc: unpacking lightmaps.res for " + obj.modelId);

	std::string unpackErr;
	if (!EnsureLightmapsUnpacked(levelDir, unpackErr)) {
		status_message_ = "Lightmap recalc: unpack failed: " + unpackErr;
		Logger::Get().Log(LogLevel::ERR, "[Lightmap] AutoRecalc: unpack failed: " + unpackErr);
		return;
	}

	const std::string qvmPath = levelDir + "\\objects.qvm";
	const std::string qscPath = levelDir + "\\objects_lightmap_tmp.qsc";
	std::string decompErr;
	Logger::Get().Log(LogLevel::INFO, "[Lightmap] AutoRecalc: decompiling objects.qvm");
	if (!igi1conv::QvmDecompile(qvmPath, qscPath, decompErr)) {
		status_message_ = "Lightmap recalc: decompile failed: " + decompErr;
		Logger::Get().Log(LogLevel::ERR, "[Lightmap] AutoRecalc: decompile failed: " + decompErr);
		return;
	}

	// Bake new .olm for the manipulated object + all its ATTA children.
	int baked = 0;
	Logger::Get().Log(LogLevel::INFO, "[Lightmap] AutoRecalc: baking .olm for taskId=" + obj.taskId);
	if (RecalcLightmapToOlm(obj, qscPath, /*force=*/true)) ++baked;
	for (int ci : obj.childrenIndices) {
		if (ci >= 0 && ci < (int)objects.size())
			if (RecalcLightmapToOlm(objects[ci], qscPath, /*force=*/true)) ++baked;
	}

	// Reload the updated .olm into the GPU texture for immediate visual feedback.
	ResolveAndApplyLightmap(obj, qscPath, /*olm_from_recalc=*/true);
	for (int ci : obj.childrenIndices) {
		if (ci >= 0 && ci < (int)objects.size())
			ResolveAndApplyLightmap(objects[ci], qscPath, /*olm_from_recalc=*/true);
	}

	std::error_code qscEc; std::filesystem::remove(qscPath, qscEc);

	if (baked > 0) {
		// Repack updated .olm files back into lightmaps.res.
		Logger::Get().Log(LogLevel::INFO, "[Lightmap] AutoRecalc: repacking lightmaps.res (" + std::to_string(baked) + " baked)");
		const std::string tmpRes = lightmapsDir + "\\lightmaps_new.res";
		std::string repackErr;
		if (igi1conv::ResRepack(resPath, unpackedDir, tmpRes, repackErr)) {
			std::error_code ec;
			std::filesystem::rename(tmpRes, resPath, ec);
			if (ec) {
				std::filesystem::copy_file(tmpRes, resPath, std::filesystem::copy_options::overwrite_existing, ec);
				std::error_code ec2; std::filesystem::remove(tmpRes, ec2);
			}
			if (!ec) {
				Logger::Get().Log(LogLevel::INFO, "[Lightmap] Auto-recalc: repacked " +
					std::to_string(baked) + " lightmap(s) into " + resPath);
				status_message_ = "Lightmap recalculated (" + std::to_string(baked) + " object(s) baked)";
			}
		} else {
			Logger::Get().Log(LogLevel::ERR, "[Lightmap] Auto-recalc: repack failed: " + repackErr);
			status_message_ = "Lightmap baked but repack failed: " + repackErr;
		}
	} else {
		status_message_ = "Lightmap reloaded (no .olm change needed)";
	}
}

// Escape-menu "Lightmaps" checkbox, turned ON: resolve + apply baked lightmaps for
// EVERY Building/EditRigidObj in the level (matches the game's always-on behavior),
// instead of requiring the per-object "Calculate Lightmap" button for each one.
void App::CalculateLightmapsForAllObjects() {
	auto& objects = level_.GetLevelObjects().GetObjects();
	std::vector<int> targets;
	for (int i = 0; i < (int)objects.size(); ++i) {
		const auto& o = objects[i];
		if (o.type == "Building" || o.type == "EditRigidObj" || o.type == "EditObj") targets.push_back(i);
	}
	if (targets.empty()) {
		Logger::Get().Log(LogLevel::INFO, "[Lightmap] No Building/EditRigidObj/EditObj objects in this level.");
		return;
	}

	// Expand targets to include children of each Building/EditRigidObj — Door,
	// Generator, etc. may have their own .olm lightmap bindings.
	{
		std::unordered_set<int> inTargets(targets.begin(), targets.end());
		std::vector<int> toAdd;
		for (int pi : targets) {
			for (int ci : objects[pi].childrenIndices) {
				if (ci >= 0 && ci < (int)objects.size() && !inTargets.count(ci)) {
					toAdd.push_back(ci);
					inTargets.insert(ci);
				}
			}
		}
		targets.insert(targets.end(), toAdd.begin(), toAdd.end());
	}

	const std::string levelDir = Utils::GetIGIRootPath() + "\\missions\\location0\\level" +
		std::to_string(level_.GetLevelNo());
	const std::string qvmPath = levelDir + "\\objects.qvm";
	const std::string qscPath = levelDir + "\\objects_lightmap_tmp.qsc";

	status_message_ = "Lightmaps: calculating for " + std::to_string(targets.size()) + " object(s)...";
	DrawProgressOverlay("Calculating Lightmaps", 0, "unpacking lightmaps.res");

	std::string unpackErr;
	if (!EnsureLightmapsUnpacked(levelDir, unpackErr)) {
		status_message_ = "Lightmaps: failed to unpack lightmaps.res: " + unpackErr;
		Logger::Get().Log(LogLevel::WARNING, "[Lightmap] unpack failed: " + unpackErr);
		return;
	}

	DrawProgressOverlay("Calculating Lightmaps", 2, "decompiling objects.qvm");
	std::string decompileErr;
	if (!igi1conv::QvmDecompile(qvmPath, qscPath, decompileErr)) {
		status_message_ = "Lightmaps: " + decompileErr;
		Logger::Get().Log(LogLevel::WARNING, "[Lightmap] decompile failed: " + decompileErr);
		return;
	}

	size_t objectsWithLightmaps = 0;
	for (size_t n = 0; n < targets.size(); ++n) {
		// igi1ed is a 32-bit process; on large levels (1000+ objects, each with up to
		// ~100 lightmap textures) its virtual address space runs out well before this
		// loop's own bad_alloc catch below fires elsewhere in the frame — bail out with
		// headroom to spare instead of racing the process toward a hard crash.
		MEMORYSTATUSEX memStatus{};
		memStatus.dwLength = sizeof(memStatus);
		if (GlobalMemoryStatusEx(&memStatus) && memStatus.ullAvailVirtual < 300ull * 1024 * 1024) {
			Logger::Get().Log(LogLevel::WARNING, "[Lightmap] Stopping bulk bake early at " +
				std::to_string(n) + "/" + std::to_string(targets.size()) +
				": process is low on virtual address space (32-bit build).");
			break;
		}
		LevelObject& obj = objects[targets[n]];
		DrawProgressOverlay("Calculating Lightmaps",
			2 + static_cast<int>(98 * (n + 1) / targets.size()),
			(std::to_string(n + 1) + "/" + std::to_string(targets.size()) + ": " + obj.name).c_str());
		try {
			if (ResolveAndApplyLightmap(obj, qscPath) > 0) ++objectsWithLightmaps;
		} catch (const std::bad_alloc&) {
			Logger::Get().Log(LogLevel::ERR, "[Lightmap] Out of memory loading lightmap for " + obj.name + "; skipping remaining.");
			break;
		} catch (const std::exception& e) {
			Logger::Get().Log(LogLevel::WARNING, std::string("[Lightmap] Skipped object " + obj.name + " due to: ") + e.what());
		}
	}

	std::error_code qscEc;
	std::filesystem::remove(qscPath, qscEc);

	Logger::Get().Log(LogLevel::INFO, "[Lightmap] Calculated lightmaps for " +
		std::to_string(objectsWithLightmaps) + "/" + std::to_string(targets.size()) + " object(s).");
	status_message_ = "Lightmaps calculated: " + std::to_string(objectsWithLightmaps) + "/" +
		std::to_string(targets.size()) + " object(s)";
}

// Commit the active text/numeric box (prop_text_buf_) back to the object and
// objects.qsc, then clear edit focus. Handles the note (-2) and any field box.
void App::CommitPropTextEdit() {
	if (prop_text_edit_field_ == -1) return;
	PushUndoState();

	// AI Script Path field: update path, reload and decompile the new .qvm.
	if (prop_text_edit_field_ == PropPanel::kAIScriptPathField) {
		prop_text_edit_field_ = -1;
		ai_script_path_ = prop_text_buf_;
		if (!ai_script_path_.empty() && std::filesystem::exists(ai_script_path_)) {
			QVMFile qvm = QVM_Parse(ai_script_path_);
			ai_script_text_ = qvm.valid ? QVM_DecompileToString(qvm)
			                            : "// decompile failed: " + ai_script_path_;
		} else {
			ai_script_text_ = "// file not found: " + ai_script_path_;
		}
		ai_script_dirty_ = false;
		return;
	}

	// AI Script Text field: store edited text and mark dirty.
	if (prop_text_edit_field_ == PropPanel::kAIScriptTextField) {
		prop_text_edit_field_ = -1;
		ai_script_text_ = prop_text_buf_;
		ai_script_dirty_ = true;
		return;
	}

	int field = prop_text_edit_field_;
	prop_text_edit_field_ = -1;
	// Edits may target the selected object OR one of its child tasks (weapon/ammo).
	int oi = (prop_edit_obj_index_ >= 0) ? prop_edit_obj_index_ : selected_object_index_;
	prop_edit_obj_index_ = -1;
	if (oi < 0) return;
	auto& objects = level_.GetLevelObjects().GetObjects();
	if (oi >= (int)objects.size()) return;
	auto& obj = objects[oi];

	if (field == -2) {
		// Note edit -> obj.name (and arg[2] if present, keeping quotes).
		obj.name = prop_text_buf_;
		if (obj.argTokens.size() > 2)
			obj.argTokens[2] = "\"" + StripQuotes(prop_text_buf_) + "\"";
		obj.modified = true;
		level_.GetLevelObjects().UpdateCoordinatesInLine(obj);
		return;
	}

	const TaskSchema* scp = GetSchema(obj.type);
	if (!scp) return;
	int fi = field / 3, comp = field % 3;
	if (fi >= (int)scp->size()) return;
	const FieldDef& fd = (*scp)[fi];
	int argIdx = fd.argOffset + comp;
	if (argIdx < 0 || argIdx >= (int)obj.argTokens.size()) return;

	const std::string& tn = fd.typeName;
	bool is_str = (tn.find("String") != std::string::npos || tn == "VarString" ||
	               tn == "EnumString32" || tn == "DropDownCombo");
	bool is_int = (tn == "Int16" || tn == "Int32" || tn == "EnumInt32");
	bool is_pos = (tn == "ObjectPos"); // only sync obj.pos for actual position fields

	std::string tokenVal;
	if (is_str) {
		// Preserve quoting: strings keep surrounding quotes in the QSC.
		bool hadQuotes = obj.argTokens[argIdx].size() >= 2 &&
		                 obj.argTokens[argIdx].front() == '"';
		std::string body = StripQuotes(prop_text_buf_);
		tokenVal = (hadQuotes || tn.find("String") != std::string::npos || tn == "DropDownCombo")
		               ? ("\"" + body + "\"") : body;
	} else if (is_int) {
		long v = 0; try { v = std::lround(std::stod(prop_text_buf_)); } catch(...) {}
		char buf[64]; snprintf(buf, sizeof(buf), "%ld", v); tokenVal = buf;
	} else {
		// Real / Angle / Degrees / RangeReal32 — float formatting.
		double v = 0; try { v = std::stod(prop_text_buf_); } catch(...) {}
		char buf[64]; snprintf(buf, sizeof(buf), "%.6f", v); tokenVal = buf;
	}
	obj.argTokens[argIdx] = tokenVal;

	// Capture old transform BEFORE updating so we can compute deltas for children.
	glm::dvec3 preCommitPos = obj.pos;
	glm::dvec3 preCommitRot = obj.rot;

	// Sync obj.rot after orientation field commits so the 3D marker updates.
	bool is_ori_field = (tn == "Real32x9");
	bool is_gamma_field = ((tn == "Real32" || tn == "Angle" || tn == "Degrees") && (fd.name == "Gamma" || fd.name == "Heading"));
	if (is_ori_field) {
		if (comp == 0 && fd.argOffset < (int)obj.argTokens.size())
			try { obj.rot.x = std::stod(obj.argTokens[fd.argOffset]); } catch(...) {}
		if (comp == 1 && fd.argOffset + 1 < (int)obj.argTokens.size())
			try { obj.rot.y = std::stod(obj.argTokens[fd.argOffset + 1]); } catch(...) {}
		if (comp == 2 && fd.argOffset + 2 < (int)obj.argTokens.size())
			try { obj.rot.z = std::stod(obj.argTokens[fd.argOffset + 2]); } catch(...) {}
	} else if (is_gamma_field) {
		if (fd.argOffset < (int)obj.argTokens.size())
			try { obj.rot.z = std::stod(obj.argTokens[fd.argOffset]); } catch(...) {}
	}

	// Mirror typed coords into obj.pos for ObjectPos boxes.
	if (is_pos) {
		double v = 0; try { v = std::stod(prop_text_buf_); } catch(...) {}
		if      (comp == 0) obj.pos.x = v;
		else if (comp == 1) obj.pos.y = v;
		else                obj.pos.z = v;
	}
	// Sync model field to obj.modelId so UpdateCoordinatesInLine doesn't
	// overwrite the new model with the stale obj.modelId.
	bool is_model_field = is_str && (fd.name == "Model" ||
	                                 fd.name.find("Model") != std::string::npos);

	if (is_model_field) {
		obj.modelId = StripQuotes(prop_text_buf_);
	}

	// GunPickup/AmmoPickup: the edited field is the weapon/ammo enum string, but
	// obj.modelId must hold the RESOLVED render model. Re-resolve so the viewport
	// mesh updates immediately instead of only after a reload (issue 1).
	if (obj.type == "GunPickup" || obj.type == "AmmoPickup") {
		std::string enumStr = StripQuotes(prop_text_buf_);
		if (enumStr.rfind("WEAPON_ID_", 0) == 0 || enumStr.rfind("AMMO_ID_", 0) == 0) {
			obj.modelId = level_.GetLevelObjects().ResolvePickupModelId(enumStr);
			is_model_field = true; // treat it as a model field to trigger packing and preload
		}
	}

	const int selectedModelSource = is_model_field ? pending_model_source_level_ : 0;
	if (is_model_field) pending_model_source_level_ = 0;
	if (is_model_field && !obj.modelId.empty()) {
		if (!level_res_models_.Empty() && !level_res_models_.Contains(obj.modelId)) {
			obj.modelMissingInRes = true;
			// Auto-add the foreign model immediately — no extra keypress needed.
			std::string addId = obj.modelId;
			DrawProgressOverlay(("Adding '" + addId + "' to .res").c_str(), 0, "starting");
			auto progressCb = [this, addId](size_t done, size_t total) {
				int pct = total ? (int)(done * 100 / total) : 0;
				DrawProgressOverlay(("Adding '" + addId + "' to .res").c_str(), pct, "packing textures");
			};
			if (renderer_.AddModelToLevelRes(addId, progressCb, selectedModelSource)) {
				level_res_models_.AddEntry("models\\" + addId + ".mef");
				obj.modelMissingInRes = false;
				std::string fam = addId.substr(0, addId.find('_'));
				status_message_ = "Added model family '" + fam + "' (+textures) to .res/.dat/.mtp.";
			} else {
				status_message_ = "Failed to add '" + addId + "' to level .res (see log).";
			}
		} else {
			obj.modelMissingInRes = false;
		}

		// Eagerly load the (possibly new) model now, with a progress overlay, so a heavy
		// model with many textures doesn't appear to freeze the editor on the next frame
		// (the load is otherwise lazy in Draw → looks like a hang). (user feedback)
		DrawProgressOverlay(("Loading model '" + obj.modelId + "'").c_str(), 40, "mesh & textures");
		renderer_.PreloadModel(obj.modelId, obj.isBuilding);
	}

	obj.modified = true;
	level_.GetLevelObjects().UpdateCoordinatesInLine(obj);

	// When pos or rotation is typed in, propagate the transform delta to children.
	if (is_pos || is_ori_field || is_gamma_field) {
		glm::dvec3 deltaPos   = obj.pos - preCommitPos;
		glm::dmat3 deltaWorld = BuildRotMatZXY(obj.rot) * glm::transpose(BuildRotMatZXY(preCommitRot));
		PropagateTransformToChildren(oi, deltaPos, deltaWorld, preCommitPos);
		AutoRecalcLightmapForManipulated(oi);
	}

	// If a soldier's weapon child (Gun*) enum was just edited, re-resolve the parent
	// soldier's held-weapon model so the weapon shown in the editor updates live —
	// without this it would only refresh on a full reload. If the soldier itself was
	// edited, refresh it directly (harmless no-op when nothing weapon-related changed).
	if (obj.type.rfind("Gun", 0) == 0 && obj.parentIndex >= 0) {
		level_.GetLevelObjects().ResolveSoldierWeapon(obj.parentIndex);
	} else {
		level_.GetLevelObjects().ResolveSoldierWeapon(oi);
	}

	// Live-sync the graph overlay offset when the user moves the AIGraph
	// task via the property panel — otherwise the 3D nodes/edges stay at the
	// stale position while F7 is showing the graph.
	SyncGraphOverlayOffsetFromAIGraph();
}

void App::PushUndoState() {
	UndoState state;
	state.objects = level_.GetLevelObjects().GetObjects();
	state.ai_script_path = ai_script_path_;
	state.ai_script_text = ai_script_text_;
	state.ai_script_dirty = ai_script_dirty_;
	state.terrain_mod_options = terrain_mod_options_;
	state.terrain_hmp = level_.SnapshotTerrainHMP();
	if (renderer_.IsGraphOverlayVisible()) {
		state.graph_overlay = renderer_.GetGraphOverlaySnapshot();
		state.graph_overlay_visible = true;
	}
	igi::PushEditorUndo(undo_stack_, redo_stack_, std::move(state));
}

void App::SaveAndReloadObjects() {
	level_.SaveAndReloadObjects();
	EvaluateTrainTrackPositions();
	SnapObjectsToTerrain();
	RebuildLevelModelIds();
	// Reloading objects recreates them from the QSC, so the AIGraph task's pos
	// is the source of truth again — re-apply it to the graph overlay.
	SyncGraphOverlayOffsetFromAIGraph();
}

void App::SyncGraphOverlayOffsetFromAIGraph() {
	// No-op unless the user has pressed F7 to show the graph (the offset is
	// only consumed by the overlay draw path; the on-disk graph<id>.dat never
	// changes here, only the displayed world position does).
	if (!renderer_.IsGraphOverlayVisible()) return;
	const std::string& tid = renderer_.GraphOverlayTaskId();
	if (tid.empty()) return;
	for (const auto& o : level_.GetLevelObjects().GetObjects()) {
		if (o.type == "AIGraph" && o.taskId == tid) {
			const glm::dvec3& current = renderer_.GraphOverlayOffset();
			if (current != o.pos) {
				renderer_.SetGraphOverlayOffset(o.pos);
				Logger::Get().Log(LogLevel::INFO,
					"[App] Graph overlay offset live-synced to AIGraph " + tid +
					" pos=(" + std::to_string(o.pos.x) + ", " +
					std::to_string(o.pos.y) + ", " + std::to_string(o.pos.z) + ")");
			}
			return;
		}
	}
}

void App::RebuildLevelModelIds() {
	level_model_entries_.clear();
	level_.GetLevelObjects().LoadModelNames();
	std::map<std::string, std::set<int>> sourceLevels;
	for (const auto& source : renderer_.GetModelTextureSources()) {
		if (source.level > 0) sourceLevels[source.modelId].insert(source.level);
	}
	for (const auto& pair : level_.GetLevelObjects().GetModelNamesMap()) {
		const std::string& m = pair.first;
		bool ok = m.size() >= 7;
		if (ok) {
			for (char c : m) if (!isdigit(c) && c != '_') { ok = false; break; }
		}
		if (!ok) continue;
		const auto found = sourceLevels.find(m);
		if (found == sourceLevels.end()) {
			level_model_entries_.push_back({m, 0, m});
			continue;
		}
		for (int levelNo : found->second) {
			level_model_entries_.push_back(
				{m, levelNo, m + "  [Level " + std::to_string(levelNo) + "]"});
		}
	}
}

void App::Undo() {
	if (undo_stack_.empty()) { status_message_ = "Nothing to undo"; return; }
	// Save current state to redo stack
	UndoState current;
	current.objects = level_.GetLevelObjects().GetObjects();
	current.ai_script_path = ai_script_path_;
	current.ai_script_text = ai_script_text_;
	current.ai_script_dirty = ai_script_dirty_;
	current.terrain_mod_options = terrain_mod_options_;
	current.terrain_hmp = level_.SnapshotTerrainHMP();
	if (renderer_.IsGraphOverlayVisible()) {
		current.graph_overlay = renderer_.GetGraphOverlaySnapshot();
		current.graph_overlay_visible = true;
	}
	UndoState restored;
	if (!igi::MoveEditorUndoToRedo(undo_stack_, redo_stack_, std::move(current), restored)) {
		status_message_ = "Nothing to undo";
		return;
	}

	// Restore from undo stack
	const UndoState& s = restored;
	level_.GetLevelObjects().GetObjects() = s.objects;
	ai_script_path_ = s.ai_script_path;
	ai_script_text_ = s.ai_script_text;
	ai_script_dirty_ = s.ai_script_dirty;
	terrain_mod_options_ = s.terrain_mod_options;
	level_.RestoreTerrainHMP(s.terrain_hmp);

	if (s.graph_overlay_visible) {
		renderer_.RestoreGraphOverlay(s.graph_overlay);
		if (!renderer_.IsGraphOverlayVisible())
			renderer_.SetGraphOverlayVisible(true);
	} else {
		if (renderer_.IsGraphOverlayVisible())
			renderer_.SetGraphOverlayVisible(false);
	}

	// Mark ATTA proxies as modified so FlushAttaProxiesToMef() rewrites their
	// local positions back into the MEF binary. Without this, undoing a building
	// ATTA move left the MEF at the post-edit position while the proxy's world
	// pos reverted — the 3D view and the saved level disagreed.
	for (auto& o : level_.GetLevelObjects().GetObjects())
		if (o.isAttaProxy) o.modified = true;
	FlushAttaProxiesToMef();

	// ATTA proxies have taskId=-1 and are deliberately never serialized to QSC
	// (see CreateAttaProxy comment) — SaveAndReloadObjects() re-parses objects_
	// from that QSC, which silently drops every ATTA proxy. Capture them here and
	// re-append after the reload, or undoing/redoing an ATTA move makes the
	// proxy (and anything selected on it) vanish from the scene.
	std::vector<LevelObject> attaProxies;
	for (const auto& o : level_.GetLevelObjects().GetObjects())
		if (o.isAttaProxy) attaProxies.push_back(o);
	std::string selectedAttaKey;
	if (selected_object_index_ >= 0 && selected_object_index_ < (int)level_.GetLevelObjects().GetObjects().size()) {
		const auto& sel = level_.GetLevelObjects().GetObjects()[selected_object_index_];
		if (sel.isAttaProxy) selectedAttaKey = sel.attaParentModelId + ":" + std::to_string(sel.attaRecordIndex);
	}

	SaveAndReloadObjects();

	{
		auto& reloaded = level_.GetLevelObjects().GetObjects();
		for (auto& proxy : attaProxies) {
			std::string key = proxy.attaParentModelId + ":" + std::to_string(proxy.attaRecordIndex);
			int newIdx = (int)reloaded.size();
			// Re-link to the parent building so it still moves with it after undo.
			proxy.parentIndex = -1;
			for (int pi = 0; pi < (int)reloaded.size(); ++pi) {
				if (reloaded[pi].modelId == proxy.attaParentModelId &&
				    (reloaded[pi].isBuilding || reloaded[pi].type == "Building")) {
					proxy.parentIndex = pi;
					reloaded[pi].childrenIndices.push_back(newIdx);
					break;
				}
			}
			reloaded.push_back(proxy);
			if (key == selectedAttaKey) selected_object_index_ = newIdx;
		}
	}

	// The objects vector was just replaced with the snapshot; if a visible
	// graph overlay's task id is present in the restored list, the AIGraph
	// task's pos is now the canonical one and the overlay must follow it.
	SyncGraphOverlayOffsetFromAIGraph();
	status_message_ = "Undo";
}

void App::Redo() {
	if (redo_stack_.empty()) { status_message_ = "Nothing to redo"; return; }
	// Save current state to undo stack
	UndoState current;
	current.objects = level_.GetLevelObjects().GetObjects();
	current.ai_script_path = ai_script_path_;
	current.ai_script_text = ai_script_text_;
	current.ai_script_dirty = ai_script_dirty_;
	current.terrain_mod_options = terrain_mod_options_;
	current.terrain_hmp = level_.SnapshotTerrainHMP();
	if (renderer_.IsGraphOverlayVisible()) {
		current.graph_overlay = renderer_.GetGraphOverlaySnapshot();
		current.graph_overlay_visible = true;
	}
	UndoState restored;
	if (!igi::MoveEditorRedoToUndo(undo_stack_, redo_stack_, std::move(current), restored)) {
		status_message_ = "Nothing to redo";
		return;
	}

	// Restore from redo stack
	const UndoState& s = restored;
	level_.GetLevelObjects().GetObjects() = s.objects;
	ai_script_path_ = s.ai_script_path;
	ai_script_text_ = s.ai_script_text;
	ai_script_dirty_ = s.ai_script_dirty;
	terrain_mod_options_ = s.terrain_mod_options;
	level_.RestoreTerrainHMP(s.terrain_hmp);

	if (s.graph_overlay_visible) {
		renderer_.RestoreGraphOverlay(s.graph_overlay);
		if (!renderer_.IsGraphOverlayVisible())
			renderer_.SetGraphOverlayVisible(true);
	} else {
		if (renderer_.IsGraphOverlayVisible())
			renderer_.SetGraphOverlayVisible(false);
	}

	// Same ATTA-proxy resync as Undo — see comment there.
	for (auto& o : level_.GetLevelObjects().GetObjects())
		if (o.isAttaProxy) o.modified = true;
	FlushAttaProxiesToMef();

	// Same ATTA-proxy survival fix as Undo — see comment there.
	std::vector<LevelObject> attaProxies;
	for (const auto& o : level_.GetLevelObjects().GetObjects())
		if (o.isAttaProxy) attaProxies.push_back(o);
	std::string selectedAttaKey;
	if (selected_object_index_ >= 0 && selected_object_index_ < (int)level_.GetLevelObjects().GetObjects().size()) {
		const auto& sel = level_.GetLevelObjects().GetObjects()[selected_object_index_];
		if (sel.isAttaProxy) selectedAttaKey = sel.attaParentModelId + ":" + std::to_string(sel.attaRecordIndex);
	}

	SaveAndReloadObjects();

	{
		auto& reloaded = level_.GetLevelObjects().GetObjects();
		for (auto& proxy : attaProxies) {
			std::string key = proxy.attaParentModelId + ":" + std::to_string(proxy.attaRecordIndex);
			int newIdx = (int)reloaded.size();
			proxy.parentIndex = -1;
			for (int pi = 0; pi < (int)reloaded.size(); ++pi) {
				if (reloaded[pi].modelId == proxy.attaParentModelId &&
				    (reloaded[pi].isBuilding || reloaded[pi].type == "Building")) {
					proxy.parentIndex = pi;
					reloaded[pi].childrenIndices.push_back(newIdx);
					break;
				}
			}
			reloaded.push_back(proxy);
			if (key == selectedAttaKey) selected_object_index_ = newIdx;
		}
	}

	// Same graph-overlay live-sync as Undo — see comment there.
	SyncGraphOverlayOffsetFromAIGraph();
	status_message_ = "Redo";
}

void App::PropagateTransformToChildren(int parentIdx, const glm::dvec3& deltaPos, const glm::dmat3& deltaWorld, const glm::dvec3& pivot) {
	auto& objects = level_.GetLevelObjects().GetObjects();
	if (parentIdx < 0 || parentIdx >= (int)objects.size()) return;
	std::vector<int> children = objects[parentIdx].childrenIndices;

	for (int childIdx : children) {
		if (childIdx < 0 || childIdx >= (int)objects.size()) continue;
		LevelObject& child = objects[childIdx];

		// Rotate child's relative position around the parent's old pivot, then translate
		glm::dvec3 relPos = child.pos - pivot;
		child.pos = pivot + glm::dvec3(deltaWorld * relPos) + deltaPos;

		// Compose child's orientation with the parent's world-space rotation delta
		child.rot = ExtractEulerZXY(deltaWorld * BuildRotMatZXY(child.rot));

		if (child.type == "HumanSoldier" || child.type == "HumanSoldierFemale") {
			child.graphPos += deltaPos;
		}

		child.modified = true;
		level_.GetLevelObjects().UpdateCoordinatesInLine(child);
		PropagateTransformToChildren(childIdx, deltaPos, deltaWorld, pivot);
	}
}

int App::PickObjectAtScreenPos(int screen_x, int screen_y) {
	const auto& objects = level_.GetLevelObjects().GetObjects();
	if (objects.empty()) return -1;

	int w = window_state_.viewport_width_;
	int h = window_state_.viewport_height_;
	if (w == 0 || h == 0) return -1;

	return renderer_.PickObjectAtScreen(
		screen_x, screen_y, w, h,
		view_define_, level_.GetLevelObjects().GetObjects(),
		renderer_.DRAW_OBJECTS | renderer_.DRAW_BUILDINGS | renderer_.DRAW_PROPS,
		selected_object_index_);
}

void App::PromoteAttaToObject(int entry) {
	AttaPickEntry e;
	if (!renderer_.GetAttaPickEntry(entry, e)) return;

	auto& objects = level_.GetLevelObjects().GetObjects();
	PushUndoState();

	// Create a lightweight proxy LevelObject so the existing gizmo/movement system
	// can move the ATTA. The proxy is NOT serialized to QSC. When the user saves or
	// launches the game, FlushAttaProxiesToMef() converts the proxy's world position
	// back to local coordinates and patches the bytes directly in the MEF binary.
	// No renaming, no QSC tasks, no game-engine warnings.
	// Extract Euler angles (Rz * Rx * Ry order) from the captured world rotation matrix.
	// GLM column-major: m[col][row]. For Rz*Rx*Ry: sin(rx)=m[1][2], etc.
	const glm::mat3& m = e.worldRot;
	float sx = std::max(-1.0f, std::min(1.0f, m[1][2]));
	float rx = std::asin(sx);
	float ry, rz;
	if (std::fabs(std::cos(rx)) > 1e-4f) {
		ry = std::atan2(-m[0][2], m[2][2]);
		rz = std::atan2(-m[1][0], m[1][1]);
	} else {
		ry = 0.0f;
		rz = std::atan2(m[0][1], m[0][0]);
	}

	LevelObject obj;
	obj.type        = "EditRigidObj";
	obj.name        = "ATTA_PROXY:" + e.immediateParentModelId + ":" + std::to_string(e.recordIndex);
	obj.taskId      = "-1";
	obj.modelId     = e.modelId;
	obj.pos         = glm::dvec3(e.worldPos);
	obj.rot         = glm::vec3(rx, ry, rz);
	obj.scale       = (e.scale > 0.f) ? e.scale : 1.0f;
	obj.isBuilding  = false;
	obj.deleted     = false;
	obj.modified    = false;
	obj.isAttaProxy         = true;
	obj.attaRecordIndex     = e.recordIndex;
	obj.attaParentModelId   = e.immediateParentModelId;
	obj.attaIsBuilding      = false;
	obj.attaInvParentMat    = glm::inverse(e.parentWorldMat);

	int newIdx = (int)objects.size();

	// Link to the parent building so PropagateTransformToChildren moves this
	// proxy when the parent building is dragged/rotated.
	for (int pi = 0; pi < (int)objects.size(); ++pi) {
		if (objects[pi].modelId == e.immediateParentModelId &&
		    (objects[pi].isBuilding || objects[pi].type == "Building")) {
			obj.parentIndex = pi;
			objects[pi].childrenIndices.push_back(newIdx);
			break;
		}
	}

	objects.push_back(obj);

	// Suppress this ATTA record from being re-offered for picking.
	renderer_.MarkAttaPromotedByRecord(e.immediateParentModelId, e.recordIndex);

	selected_object_index_ = newIdx;
	marker_manip_.start_pos_ = objects[newIdx].pos;
	marker_manip_.start_rot_ = objects[newIdx].rot;
	status_message_ = "Editing ATTA '" + e.modelId + "' — move then Save to apply to .res";
	Logger::Get().Log(LogLevel::INFO,
		"[App] ATTA '" + e.modelId + "' selected for direct MEF edit"
		" (record " + std::to_string(e.recordIndex) + " in " + e.immediateParentModelId + ".mef)");
}

void App::LoadQSCForLevel(int level_no) {
	// New level: forget ATTA suppressions from the previous one (a freshly loaded
	// level's saved EditRigidObj tasks re-suppress their ATTAs via live occupancy).
	renderer_.ClearSuppressedAttas();
	try {
		namespace fs = std::filesystem;

		std::string qsc_dest = Utils::GetExeDirectory() + "\\editor\\qed\\temp\\objects.qsc";
		std::string qvm_source = Utils::GetLevelQVMPath(level_no);
		
		Logger::Get().Log(LogLevel::INFO, "[App] [LoadQSCForLevel] Always reading level objects.qvm directly: " + qvm_source);
		Logger::Get().Log(LogLevel::INFO, "[App] [LoadQSCForLevel] Destination QSC: " + qsc_dest);

		// Decompile from the game QVM directly to the destination QSC
		DecompileFromGame(level_no);
		Logger::Get().Log(LogLevel::INFO, "[App] [LoadQSCForLevel] SUCCESS: Loaded/Decompiled level from QVM to: " + qsc_dest);
	}
	catch (const std::exception& e) {
		std::string errorMsg = "Error loading Level " + std::to_string(level_no) + ":\n" + std::string(e.what());
		Utils::LogAndShowError(errorMsg, "IGI Editor - Error");
		Logger::Get().Log(LogLevel::ERR, errorMsg);
	}
	catch (...) {
		std::string errorMsg = "Unknown error loading Level " + std::to_string(level_no);
		Utils::LogAndShowError(errorMsg, "IGI Editor - Error");
		Logger::Get().Log(LogLevel::ERR, errorMsg);
	}
}

void App::DecompileFromGame(int level_no) {
	try {
		namespace fs = std::filesystem;

		std::string qvm_source = Utils::GetLevelQVMPath(level_no);
		std::string qsc_dest = Utils::GetExeDirectory() + "\\editor\\qed\\temp\\objects.qsc";

		if (!fs::exists(qvm_source)) {
			std::string errorMsg = "Game QVM not found at:\n" + qvm_source + "\n\nPlease check your IGI game path in qedconfig.txt";
			Utils::LogAndShowError(errorMsg, "IGI Editor - Error");
			Logger::Get().Log(LogLevel::ERR, "[App] Game QVM not found at: " + qvm_source);
			return;
		}

		QVMFile qvm = QVM_Parse(qvm_source);
		bool success = qvm.valid && QVM_Decompile(qvm, qsc_dest);
		if (!success) {
			std::string errorMsg = "Failed to decompile QVM from:\n" + qvm_source;
			Utils::LogAndShowError(errorMsg, "IGI Editor - Error");
			Logger::Get().Log(LogLevel::ERR, "[App] Failed to decompile from game QVM");
		}
	}
	catch (const std::exception& e) {
		std::string errorMsg = "Error decompiling QVM for level " + std::to_string(level_no) + ":\n" + std::string(e.what());
		Utils::LogAndShowError(errorMsg, "IGI Editor - Error");
		Logger::Get().Log(LogLevel::ERR, errorMsg);
	}
	catch (...) {
		std::string errorMsg = "Unknown error decompiling QVM for level " + std::to_string(level_no);
		Utils::LogAndShowError(errorMsg, "IGI Editor - Error");
		Logger::Get().Log(LogLevel::ERR, errorMsg);
	}
}

void App::LaunchGame() {
	if (game_process_.running) {
		// ── Toggle OFF: stop the running game ──────────────────────────────────
		Logger::Get().Log(LogLevel::INFO, "[ToggleGame] Game is running (PID=" +
		                  std::to_string(game_process_.pid) + ") — stopping...");

		// 1. Post WM_CLOSE to every window owned by the game (graceful request)
		int closedWindows = 0;
		struct CloseCtx { DWORD pid; int* count; };
		CloseCtx ctx{ game_process_.pid, &closedWindows };
		EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
			DWORD wndPid = 0;
			GetWindowThreadProcessId(hwnd, &wndPid);
			auto* c = reinterpret_cast<CloseCtx*>(lp);
			if (wndPid == c->pid) {
				PostMessage(hwnd, WM_CLOSE, 0, 0);
				(*c->count)++;
			}
			return TRUE;
		}, reinterpret_cast<LPARAM>(&ctx));
		Logger::Get().Log(LogLevel::INFO, "[ToggleGame] WM_CLOSE posted to " +
		                  std::to_string(closedWindows) + " window(s)");

		// 2. Force-terminate immediately so we don't block the main thread.
		//    Old DirectX full-screen games rarely honour WM_CLOSE anyway.
		BOOL killed = TerminateProcess(game_process_.hProcess, 0);
		Logger::Get().Log(LogLevel::INFO, "[ToggleGame] TerminateProcess(" +
		                  std::to_string(game_process_.pid) + ") = " +
		                  (killed ? "OK" : "FAILED (err=" + std::to_string(GetLastError()) + ")"));

		// The background monitor thread will detect the process exit and set
		// game_exited_ = true; OnIdle will then clean up handles and restore the editor.
		Logger::Get().Log(LogLevel::INFO, "[ToggleGame] Waiting for OnIdle to restore editor...");
		return;
	}

	// ── Toggle ON: save level and launch the game ──────────────────────────────
	Logger::Get().Log(LogLevel::INFO, "[ToggleGame] Game is not running — launching level " +
	                  std::to_string(level_.GetLevelNo()));

	// Stop the editor's own level music before handing off — igi.exe plays its
	// own copy of the same track, so leaving ours running doubles it up.
	StopLevelMusic();

	if (igi::ShouldSaveBeforeExternalGameLaunch(
			!in_game_mode_, Config::Get().auto_save_enabled)) {
		SaveCurrentLevel();
		Logger::Get().Log(LogLevel::INFO, "[ToggleGame] Level saved");
	} else {
		Logger::Get().Log(LogLevel::INFO,
			"[ToggleGame] Auto-save disabled; launching without persisting editor changes");
	}

	std::string workDir = Utils::GetIGIRootPath();
	std::string cmdLine = workDir + "\\igi.exe level" + std::to_string(level_.GetLevelNo());
	Logger::Get().Log(LogLevel::INFO, "[ToggleGame] Launching: " + cmdLine);

	STARTUPINFOA si = {};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi = {};

	std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end());
	cmdBuf.push_back('\0');

	if (!CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
	                    0, nullptr, workDir.c_str(), &si, &pi)) {
		DWORD err = GetLastError();
		std::string errMsg = "Failed to launch igi.exe (error " + std::to_string(err) + ")";
		Logger::Get().Log(LogLevel::ERR, "[ToggleGame] " + errMsg);
		Utils::LogAndShowError(errMsg, "IGI Editor - Launch Error");
		return;
	}
	Logger::Get().Log(LogLevel::INFO, "[ToggleGame] CreateProcess OK — PID=" +
	                  std::to_string(pi.dwProcessId));

	// Keep our own PROCESS_ALL_ACCESS handle for TerminateProcess / WaitForSingleObject
	HANDLE hGame = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pi.dwProcessId);
	if (!hGame) {
		DWORD err = GetLastError();
		Logger::Get().Log(LogLevel::ERR, "[ToggleGame] OpenProcess failed (error=" +
		                  std::to_string(err) + ") — cannot track game");
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		return;
	}
	CloseHandle(pi.hProcess);  // release the CreateProcess copy; we use hGame

	game_process_.hProcess = hGame;
	game_process_.hThread  = pi.hThread;
	game_process_.pid      = pi.dwProcessId;
	game_process_.running  = true;

	// Spawn background monitor — WaitForSingleObject(INFINITE) on the game process.
	// Sets game_exited_ when the process exits (by any means), so OnIdle can restore.
	game_exited_.store(false, std::memory_order_relaxed);
	auto* monParam = new GameMonitorParam{hGame, &game_exited_};
	DWORD monTid = 0;
	game_process_.hMonitorThread = CreateThread(nullptr, 0, GameMonitorProc, monParam, 0, &monTid);
	Logger::Get().Log(LogLevel::INFO, "[ToggleGame] Monitor thread started (TID=" +
	                  std::to_string(monTid) + ")");

	// Register global hotkey so F3 (or whatever keyToggleGame is bound to) fires
	// even when the game has focus and the editor is iconified.
	if (editor_hwnd_) {
		const auto& kb = Config::Get().keyToggleGame;
		UINT mods = (kb.ctrl  ? MOD_CONTROL : 0)
		          | (kb.shift ? MOD_SHIFT   : 0)
		          | (kb.alt   ? MOD_ALT     : 0)
		          | MOD_NOREPEAT;
		if (RegisterHotKey(editor_hwnd_, HOTKEY_ID_TOGGLE_GAME, mods, kb.vkCode)) {
			Logger::Get().Log(LogLevel::INFO, "[ToggleGame] Global hotkey registered (VK=0x" +
			                  [&]{ std::ostringstream ss; ss << std::hex << kb.vkCode; return ss.str(); }() + ")");
		} else {
			Logger::Get().Log(LogLevel::WARNING, "[ToggleGame] RegisterHotKey failed (err=" +
			                  std::to_string(GetLastError()) + ") — F3 won't work while game runs");
		}
	}

	// Fire WM_TIMER every 100ms while iconified so freeglut's message loop keeps running
	// (without a timer it blocks in WaitMessage and OnIdle never fires).
	if (editor_hwnd_) SetTimer(editor_hwnd_, 1, 100, NULL);

	// Iconify via GLUT so its internal state stays consistent (raw ShowWindow breaks the idle loop)
	glutIconifyWindow();
	Logger::Get().Log(LogLevel::INFO, "[ToggleGame] Editor iconified — game is now active");
}

void App::SaveAndCompile() {
	namespace fs = std::filesystem;

	Logger::Get().Log(LogLevel::INFO, "[App] SaveAndCompile() starting");

	std::string qsc_source = Utils::GetExeDirectory() + "\\editor\\qed\\temp\\objects.qsc";
	std::string qvm_dest = Utils::GetLevelQVMPath(level_.GetLevelNo());

	Logger::Get().Log(LogLevel::INFO, "[App] Full QSC path: " + qsc_source);
	Logger::Get().Log(LogLevel::INFO, "[App] QVM destination: " + qvm_dest);

	if (!fs::exists(qsc_source)) {
		Logger::Get().Log(LogLevel::ERR, "[App] QSC file not found at: " + qsc_source);
		return;
	}

	// Backup existing QVM before overwriting so we can revert if compile produces garbage
	std::vector<uint8_t> qvm_backup;
	{
		std::ifstream backup_in(qvm_dest, std::ios::binary);
		if (backup_in) {
			qvm_backup.assign(std::istreambuf_iterator<char>(backup_in),
			                  std::istreambuf_iterator<char>());
			Logger::Get().Log(LogLevel::INFO, "[App] Backed up existing QVM (" +
			                  std::to_string(qvm_backup.size()) + " bytes)");
		}
	}

	Logger::Get().Log(LogLevel::INFO, "[App] Compiling QSC (native)");
	std::ifstream qscFile(qsc_source);
	std::string qscSrc((std::istreambuf_iterator<char>(qscFile)), std::istreambuf_iterator<char>());
	auto lexResult  = qsc::Lex(qscSrc);
	auto parseResult = lexResult.ok ? qsc::Parse(lexResult.tokens) : qsc::ParseResult{};
	std::string compileErr;
	bool success = lexResult.ok && parseResult.ok &&
	               qvm::CompileToFile(*parseResult.program, qvm_dest, &compileErr);
	if (success) {
		// Round-trip validate: parse the QVM we just wrote to catch silent corruption
		QVMFile written_qvm = QVM_Parse(qvm_dest);
		if (!written_qvm.valid) {
			Logger::Get().Log(LogLevel::ERR, "[App] CRITICAL: Written QVM failed validation — reverting to backup");
			if (!qvm_backup.empty()) {
				std::ofstream revert(qvm_dest, std::ios::binary | std::ios::trunc);
				if (revert) {
					revert.write(reinterpret_cast<const char*>(qvm_backup.data()), qvm_backup.size());
					Logger::Get().Log(LogLevel::INFO, "[App] Backup QVM restored successfully");
				} else {
					Logger::Get().Log(LogLevel::ERR, "[App] FATAL: Could not restore backup QVM");
				}
			}
			Utils::LogAndShowError(
				"Save failed: the compiled QVM was invalid and has been reverted.\n"
				"Your edits are NOT lost — they remain in the editor.",
				"IGI Editor - Save Error");
			return;
		}
		Logger::Get().Log(LogLevel::INFO, "[App] QVM round-trip validation passed. Deployed to: " + qvm_dest);
	} else {
		std::string detail = compileErr.empty() ? "(no detail)" : compileErr;
		Logger::Get().Log(LogLevel::ERR, "[App] Failed to compile QSC. Detail: " + detail);
		Utils::LogAndShowError("Compile failed. Error: " + detail, "IGI Editor - Compile Error");
	}
}

void App::SetInitialFullscreen(int windowedW, int windowedH) {
	// Mark fullscreen as active and remember the windowed size so ALT+ENTER can
	// restore a sane window. main() calls glutFullScreen() to actually enter it.
	window_state_.full_screen_ = true;
	window_state_.old_viewport_width_  = windowedW;
	window_state_.old_viewport_height_ = windowedH;
}

void App::SetInitialDrawParts(int parts) {
	if (parts != 0) {
		draw_params_.draw_parts_ = parts;
		Logger::Get().Log(LogLevel::INFO, "[App] Set initial draw_parts to: " + std::to_string(parts));
	}
}

void App::SetInitialStickToGround(bool stick) {
	stick_to_ground_ = stick;
	if (stick) {
		SnapObjectsToTerrain();
		Logger::Get().Log(LogLevel::INFO, "[App] Enabled stick_to_ground mode");
	}
}

void App::ProcessTreeViewClick(int mx, int my) {
    if (!level_.GetLevelObjects().GetObjects().empty()) {
        auto& objects = level_.GetLevelObjects().GetObjects();
        int tree_x = 20;
        int row_h = CurrentUiRowHeight();
        int start_y = 30;
        int current_row = 0;

        bool found = false;
        std::function<void(int, int)> check_node = [&](int idx, int depth) {
            if (found || idx < 0 || idx >= (int)objects.size()) return;
            const auto& obj = objects[idx];
            if (obj.deleted) return;
            
            int x = tree_x + (depth * 18);
            int y = start_y + (current_row - tree_scroll_offset_) * row_h;
            current_row++;

            if (y >= start_y && y < window_state_.viewport_height_ - 50) {
                // Check if interaction was on the node area (including [+] and label)
                if (mx >= x - 20 && mx <= x + 300 && my >= y && my <= y + row_h) {
                    found = true;
                    if (mx <= x + 5) { // Clicked on toggle area
                        if (obj.isContainer && !obj.childrenIndices.empty()) {
                            auto& nonConstObj = const_cast<LevelObject&>(obj);
                            nonConstObj.expanded = !nonConstObj.expanded;
                            Logger::Get().Log(LogLevel::INFO, "[App] Toggled tree node: " + obj.type);
                        }
                    } else { // Clicked on label area
                        selected_object_index_ = idx;
                        int currentTime = glutGet(GLUT_ELAPSED_TIME);
                        bool isDoubleClick = (idx == last_tree_click_index_ && (currentTime - last_tree_click_time_ms_ < 400));
                        last_tree_click_index_ = idx;
                        last_tree_click_time_ms_ = currentTime;

                        if (isDoubleClick) {
                            prop_editor_open_ = true; prop_panel_scroll_ = 0; prop_text_edit_field_ = -1; prop_edit_obj_index_ = -1;
                            LoadAIScriptForSelected();
                            Logger::Get().Log(LogLevel::INFO, "[App] Double clicked object from tree and opened property panel.");
                        } else {
                            Logger::Get().Log(LogLevel::INFO, "[App] Selected object from tree: " + obj.type);
                        }
                    }
                }
            }

            if (!found && obj.expanded) {
                for (int childIdx : obj.childrenIndices) {
                    check_node(childIdx, depth + 1);
                }
            }
        };

        std::vector<int> root_decls;
        std::vector<int> root_others;
        for (int i = 0; i < (int)objects.size(); ++i) {
            if (objects[i].parentIndex == -1 && !objects[i].deleted) {
                if (objects[i].type == "Task_DeclareParameters") root_decls.push_back(i);
                else root_others.push_back(i);
            }
        }

        if (!found && !root_decls.empty()) {
            int y = start_y + (current_row - tree_scroll_offset_) * row_h;
            current_row++;
            if (y >= start_y && y < window_state_.viewport_height_ - 50) {
                if (mx >= tree_x - 20 && mx <= tree_x + 300 && my >= y && my <= y + row_h) {
                    found = true;
                    if (mx <= tree_x + 5) {
                        tree_decl_expanded_ = !tree_decl_expanded_;
                        Logger::Get().Log(LogLevel::INFO, "[App] Toggled Mission Declarations");
                    } else {
                        selected_object_index_ = -2;
                    }
                }
            }
            if (!found && tree_decl_expanded_) {
                for (int idx : root_decls) check_node(idx, 1);
            }
        }

        if (!found) {
            for (int idx : root_others) {
                if (found) break;
                check_node(idx, 0);
            }
        }
    }
}

void App::ProcessTreeViewHover(int mx, int my) {
    int tree_x = 20;
    int start_y = 30;
    int row_h = CurrentUiRowHeight();
    int current_row = 0;
    
    auto& objects = level_.GetLevelObjects().GetObjects();
    
    bool found = false;
    std::function<void(int, int)> check_node = [&](int idx, int depth) {
        if (found || idx < 0 || idx >= (int)objects.size()) return;
        auto& obj = objects[idx];
        if (obj.deleted) return;
        
        int x = tree_x + (depth * 18);
        int y = start_y + (current_row - tree_scroll_offset_) * row_h;
        current_row++;

        if (y >= start_y && y < window_state_.viewport_height_ - 50) {
            if (mx >= x - 20 && mx <= x + 300 && my >= y && my <= y + row_h) {
                hover_tree_index_ = idx;
                found = true;
            }
        }

        if (!found && obj.expanded) {
            for (int childIdx : obj.childrenIndices) {
                check_node(childIdx, depth + 1);
            }
        }
    };

    std::vector<int> root_decls;
    std::vector<int> root_others;
    for (int i = 0; i < (int)objects.size(); ++i) {
        if (objects[i].parentIndex == -1 && !objects[i].deleted) {
            if (objects[i].type == "Task_DeclareParameters") root_decls.push_back(i);
            else root_others.push_back(i);
        }
    }

    if (!found && !root_decls.empty()) {
        int y = start_y + (current_row - tree_scroll_offset_) * row_h;
        current_row++;
        if (y >= start_y && y < window_state_.viewport_height_ - 50) {
            if (mx >= tree_x - 20 && mx <= tree_x + 300 && my >= y && my <= y + row_h) {
                found = true;
                hover_tree_index_ = -1;
            }
        }
        if (!found && tree_decl_expanded_) {
            for (int idx : root_decls) check_node(idx, 1);
        }
    }

    if (!found) {
        for (int idx : root_others) {
            if (found) break;
            check_node(idx, 0);
        }
    }
}

void App::CreateNewTask() {
    if (task_picker_open_) return;
    PushUndoState();
    auto& objects = level_.GetLevelObjects().GetObjects();
    if (selected_object_index_ < 0 && !objects.empty()) {
        status_message_ = "Error: Must select a valid parent task first.";
        return;
    }
    if (objects.empty()) {
        LevelObject newObj;
        newObj.qscFuncName = "Task_New";
        newObj.type = "Container";
        newObj.name = "NewTask_0";
        newObj.pos = glm::dvec3(viewer_.pos_);
        newObj.rot = glm::vec3(0.0f);
        newObj.scale = 1.0f;
        newObj.isContainer = true;
        newObj.expanded = true;
        newObj.modified = true;
        newObj.taskId = "-1";
        
        objects.push_back(newObj);
        selected_object_index_ = 0;
        level_.GetLevelObjects().UpdateCoordinatesInLine(objects.back());
        SaveAndReloadObjects();
        return;
    }

    task_picker_open_ = true;
    task_picker_selected_idx_ = 0;
    task_picker_scroll_offset_ = 0;
    task_picker_search_ = "";
    Logger::Get().Log(LogLevel::INFO, "[App] Opened Task Picker overlay");
}

void App::DeleteSelectedTask() {
    if (selected_object_index_ < 0) return;
    PushUndoState();
    auto& objects = level_.GetLevelObjects().GetObjects();
    if (selected_object_index_ >= (int)objects.size()) return;
    int parentIndex = objects[selected_object_index_].parentIndex;

    std::function<void(int)> delete_recurse = [&](int idx) {
        if (idx < 0 || idx >= (int)objects.size()) return;
        objects[idx].deleted = true;
        for (int childIdx : objects[idx].childrenIndices) {
            delete_recurse(childIdx);
        }
    };

    delete_recurse(selected_object_index_);
    SaveAndReloadObjects();
    auto& reloaded = level_.GetLevelObjects().GetObjects();
    if (reloaded.empty()) selected_object_index_ = -1;
    else if (parentIndex >= 0 && parentIndex < (int)reloaded.size()) selected_object_index_ = parentIndex;
    else selected_object_index_ = std::min(selected_object_index_, (int)reloaded.size() - 1);
    Logger::Get().Log(LogLevel::INFO, "[App] Deleted task and its subtree");
}

void App::CopySelectedTask(bool includeSubtree) {
    if (selected_object_index_ < 0) return;
    auto& objects = level_.GetLevelObjects().GetObjects();
    clipboard_.clear();

    std::function<void(int, int)> copy_recurse = [&](int idx, int newParentInClipboard) {
        if (idx < 0 || idx >= (int)objects.size()) return;
        
        LevelObject copy = objects[idx];
        copy.childrenIndices.clear();
        copy.parentIndex = newParentInClipboard;
        
        int clipboardIdx = (int)clipboard_.size();
        clipboard_.push_back(copy);
        
        if (newParentInClipboard != -1) {
            clipboard_[newParentInClipboard].childrenIndices.push_back(clipboardIdx);
        }

        if (includeSubtree) {
            for (int childIdx : objects[idx].childrenIndices) {
                copy_recurse(childIdx, clipboardIdx);
            }
        }
    };

    copy_recurse(selected_object_index_, -1);
    Logger::Get().Log(LogLevel::INFO, "[App] Copied task to clipboard (subtree: " + std::string(includeSubtree ? "yes" : "no") + ")");
}

void App::PasteTask() {
    if (clipboard_.empty()) return;
    PushUndoState();
    auto& objects = level_.GetLevelObjects().GetObjects();
    if (selected_object_index_ < 0 || selected_object_index_ >= (int)objects.size()) {
        status_message_ = "Error: Must select a valid parent task first.";
        Logger::Get().Log(LogLevel::WARNING, "[App] Validation failed: Parent index is invalid for Paste operation.");
        return;
    }
    if (!ValidateParentChildCompatibility(objects[selected_object_index_], clipboard_)) {
        status_message_ = "Error: Cannot add Computer to a WaterTower.";
        Logger::Get().Log(LogLevel::WARNING, "[App] Validation failed: Cannot paste Computer task to WaterTower parent.");
        return;
    }
    int targetParent = selected_object_index_;

    int startIdxInObjects = (int)objects.size();

    // Collect all in-use task IDs for unique ID generation (same method as AssignTaskID)
    std::set<int> usedIds;
    for (const auto& obj : objects) {
        if (obj.deleted) continue;
        if (obj.taskId.empty() || obj.taskId == "-1") continue;
        try { usedIds.insert(std::stoi(obj.taskId)); } catch (...) {}
    }

    // AI folder path for QVM file copying
    int levelNo = level_.GetLevelNo();
    std::string aiDir = Utils::GetIGIRootPath() + "\\missions\\location0\\level" + std::to_string(levelNo) + "\\ai";
    
    // Copy all from clipboard to objects
    for (size_t i = 0; i < clipboard_.size(); ++i) {
        LevelObject pasted = clipboard_[i];
        
        // Update indices to point into objects_ vector
        if (pasted.parentIndex == -1) {
            pasted.parentIndex = targetParent;
            if (targetParent != -1) {
                objects[targetParent].childrenIndices.push_back((int)objects.size());
            }
        } else {
            pasted.parentIndex += startIdxInObjects;
        }

        for (size_t j = 0; j < pasted.childrenIndices.size(); ++j) {
            pasted.childrenIndices[j] += startIdxInObjects;
        }

        pasted.modified = true;

        // Generate unique task IDs for AI NPC child tasks
        if (pasted.qscFuncName == "Task_New" &&
            (pasted.type == "HumanSoldier" || pasted.type == "HumanSoldierFemale" || pasted.type == "HumanAI")) {

            std::string oldId = pasted.taskId;

            // Find next available unique ID
            int newId = 1;
            while (usedIds.count(newId)) newId++;
            usedIds.insert(newId);

            std::string newIdStr = std::to_string(newId);
            pasted.taskId = newIdStr;
            if (!pasted.argTokens.empty()) {
                pasted.argTokens[0] = newIdStr;
            }
            pasted.qscLine.clear(); // Force regeneration from argTokens on save

            // For HumanAI: copy the QVM file with the new ID
            if (pasted.type == "HumanAI" && !oldId.empty() && oldId != "-1") {
                std::string srcQvm = aiDir + "\\" + oldId + ".qvm";
                std::string dstQvm = aiDir + "\\" + newIdStr + ".qvm";
                try {
                    if (std::filesystem::exists(srcQvm)) {
                        std::filesystem::create_directories(aiDir);
                        std::filesystem::copy_file(srcQvm, dstQvm, std::filesystem::copy_options::overwrite_existing);
                        Logger::Get().Log(LogLevel::INFO, "[App] Copied AI QVM: " + srcQvm + " -> " + dstQvm);
                    } else {
                        Logger::Get().Log(LogLevel::WARNING, "[App] AI QVM not found for copy: " + srcQvm);
                    }
                } catch (const std::exception& e) {
                    Logger::Get().Log(LogLevel::ERR, "[App] Failed to copy AI QVM: " + std::string(e.what()));
                }
            }

            Logger::Get().Log(LogLevel::INFO, "[App] Assigned unique Task ID " + newIdStr + " to pasted " + pasted.type + " (was " + oldId + ")");
        }

        objects.push_back(pasted);
    }

    selected_object_index_ = startIdxInObjects;
    SaveAndReloadObjects();
    auto& reloaded = level_.GetLevelObjects().GetObjects();
    if (!reloaded.empty()) selected_object_index_ = std::min(selected_object_index_, (int)reloaded.size() - 1);
    
    Logger::Get().Log(LogLevel::INFO, "[App] Pasted task(s) from clipboard");
}

void App::AssignTaskID() {
    if (selected_object_index_ < 0) return;
    auto& objects = level_.GetLevelObjects().GetObjects();

    // Collect all in-use IDs (0..4000 range)
    std::set<int> usedIds;
    for (int i = 0; i < (int)objects.size(); ++i) {
        if (objects[i].deleted) continue;
        if (objects[i].taskId.empty() || objects[i].taskId == "-1") continue;
        try { usedIds.insert(std::stoi(objects[i].taskId)); } catch (...) {}
    }

    // Check if selected already has a valid unique ID
    const std::string& curId = objects[selected_object_index_].taskId;
    if (!curId.empty() && curId != "-1") {
        try {
            int cur = std::stoi(curId);
            int count = (int)std::count_if(objects.begin(), objects.end(), [&](const LevelObject& o){
                if (o.deleted) return false;
                try { return std::stoi(o.taskId) == cur; } catch (...) { return false; }
            });
            if (count > 1) {
                status_message_ = "Error: duplicate Task ID " + curId + " — assigning new unique ID";
            } else {
                status_message_ = "Task ID " + curId + " is already unique";
                return;
            }
        } catch (...) {}
    }

    // Find lowest positive integer not in use
    int newId = 1;
    while (usedIds.count(newId)) newId++;

    objects[selected_object_index_].taskId = std::to_string(newId);
    objects[selected_object_index_].modified = true;
    level_.GetLevelObjects().UpdateCoordinatesInLine(objects[selected_object_index_]);
    SaveAndReloadObjects();
    auto& reloaded = level_.GetLevelObjects().GetObjects();
    if (!reloaded.empty()) selected_object_index_ = std::min(selected_object_index_, (int)reloaded.size() - 1);

    status_message_ = "Assigned unique Task ID: " + std::to_string(newId);
    Logger::Get().Log(LogLevel::INFO, "[App] Assigned unique Task ID: " + std::to_string(newId));
}

void App::ModifyTaskParameters() {
	Logger::Get().Log(LogLevel::INFO, "[App] ModifyTaskParameters (Stub - parameter UI needed)");
}

void App::ClearStatusMessage() {
	status_message_.clear();
}


