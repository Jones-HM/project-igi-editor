/******************************************************************************
 * @file    renderer_objects_atta.cpp
 * @brief   Renderer_Objects: ATTA building-attachment system (promotion,
 *          .res/.dat/.mtp packing, recursive load/draw of attachments).
 *          Split from renderer_objects.cpp; shares renderer_objects_internal.h.
 *****************************************************************************/
#include "renderer_objects_internal.h"

std::string Renderer_Objects::AttaOccupancyKey(const std::string& modelId, const glm::vec3& worldPos) {
    char buf[160];
    snprintf(buf, sizeof(buf), "%s@%lld,%lld,%lld", modelId.c_str(),
             (long long)llround(worldPos.x), (long long)llround(worldPos.y), (long long)llround(worldPos.z));
    return buf;
}

// Disable a baked ATTA record inside an in-memory .mef byte buffer: zero its 16-byte
// model NAME (so the engine can't resolve a model and skips it) and shove the record
// far below the world as a fallback. Returns true if a matching record was edited.
static bool DisableAttaInMefBytes(std::vector<uint8_t>& mef, const std::string& attModelId, const glm::vec3& localPos) {
    if (mef.size() < 20 || std::memcmp(mef.data(), "ILFF", 4) != 0) return false;
    bool modified = false;
    size_t off = 20;
    while (off + 16 <= mef.size()) {
        char fourcc[5] = {0};
        std::memcpy(fourcc, mef.data() + off, 4);
        uint32_t size = 0, skip = 0;
        std::memcpy(&size, mef.data() + off + 4, 4);
        std::memcpy(&skip, mef.data() + off + 12, 4);

        if (std::strcmp(fourcc, "ATTA") == 0) {
            size_t recordsStart = off + 16;
            size_t numRecords = size / 68;
            for (size_t i = 0; i < numRecords; ++i) {
                size_t base = recordsStart + i * 68;
                if (base + 68 > mef.size()) break;
                char nameBuf[16];
                std::memcpy(nameBuf, mef.data() + base, 16);
                float px, py, pz;
                std::memcpy(&px, mef.data() + base + 16, 4);
                std::memcpy(&py, mef.data() + base + 20, 4);
                std::memcpy(&pz, mef.data() + base + 24, 4);
                std::string aname(nameBuf, strnlen(nameBuf, 16));
                if (aname == attModelId &&
                    std::abs(px - localPos.x) < 1.0f &&
                    std::abs(py - localPos.y) < 1.0f &&
                    std::abs(pz - localPos.z) < 1.0f) {
                    // Rename to a non-existent model ID so the game's resource
                    // loader silently skips it (can't find the mesh, no render).
                    // Must start with a digit — names starting with '_' or a letter
                    // are treated as "magic object types" and show a warning dialog.
                    // An empty name crashes IGI's loader entirely.
                    static const char kNullName[16] = "999_99_9\0\0\0\0\0\0\0";
                    std::memcpy(mef.data() + base, kNullName, 16);
                    modified = true;
                    break; // suppress only the first matching record per ATTA chunk
                }
            }
        }
        if (skip == 0) break;
        off += skip;
    }
    return modified;
}

bool Renderer_Objects::SuppressAttachmentInMef(const std::string& parentModelId, const std::string& attModelId, const glm::vec3& localPos) {
    // Patch the level's model archive IN PLACE: parse it, disable the one ATTA record
    // inside the parent building's .mef entry, and repack ALL entries. This keeps the
    // archive complete (the old code regenerated from editor/models/levelX, which the
    // editor wipes after loading, producing an empty/broken .res).
    const std::string gameRes = Utils::GetIGIRootPath() + "\\missions\\location0\\level" +
                                std::to_string(current_level_) + "\\models\\level" +
                                std::to_string(current_level_) + ".res";
    if (!std::filesystem::exists(gameRes)) {
        Logger::Get().Log(LogLevel::WARNING, "[Renderer] SuppressAttachmentInMef: archive not found: " + gameRes);
        return false;
    }

    RESFile res = RES_Parse(gameRes);
    if (!res.valid) {
        Logger::Get().Log(LogLevel::ERR, "[Renderer] SuppressAttachmentInMef: failed to parse " + gameRes);
        return false;
    }

    auto endsWithCI = [](const std::string& s, const std::string& suf) {
        if (suf.size() > s.size()) return false;
        for (size_t i = 0; i < suf.size(); ++i)
            if (std::tolower((unsigned char)s[s.size()-suf.size()+i]) != std::tolower((unsigned char)suf[i])) return false;
        return true;
    };
    const std::string suffix = parentModelId + ".mef";

    bool modified = false;
    for (auto& e : res.entries) {
        if (endsWithCI(e.name, suffix)) {
            if (DisableAttaInMefBytes(e.data, attModelId, localPos)) { modified = true; break; }
        }
    }
    if (!modified) {
        Logger::Get().Log(LogLevel::WARNING, "[Renderer] SuppressAttachmentInMef: ATTA '" + attModelId +
            "' not found in " + parentModelId + ".mef");
        return false;
    }

    try {
        std::string bak = gameRes + ".orig";
        if (!std::filesystem::exists(bak))
            std::filesystem::copy_file(gameRes, bak, std::filesystem::copy_options::overwrite_existing);
    } catch (...) {}

    std::string err;
    if (!RES_WriteEntries(res.entries, gameRes, err)) {
        Logger::Get().Log(LogLevel::ERR, "[Renderer] SuppressAttachmentInMef: repack failed: " + err);
        return false;
    }
    Logger::Get().Log(LogLevel::INFO, "[Renderer] SuppressAttachmentInMef: disabled ATTA '" + attModelId +
        "' in " + parentModelId + ".mef and repacked " + gameRes);
    return true;
}

// Batch-add resources (models or textures) to ONE .res archive, idempotently, WITHOUT
// ever loading the whole archive into RAM. Streams the source once to discover which
// requested names already exist (case-insensitive on the full entry name), then — if any
// remain — streams it again appending only the missing entries into a .tmp and atomically
// renames .tmp over the original. The original is backed up to <resPath>.orig once before
// the first modification; if that backup fails we ABORT (archive untouched). On any
// stream/IO/rename failure the .tmp is removed and the original is left intact.
// This is the OOM-safe replacement for the old per-file RES_Parse → RES_WriteEntries path:
// the 201MB textures archive is never held in memory (peak = one entry).
static bool AddEntriesToRes(const std::string& resPath,
                            const std::vector<RESEntry>& wanted,
                            std::string& err,
                            const std::function<void(size_t,size_t)>& onProgress = nullptr,
                            bool replaceExisting = false) {
    auto equalsCI = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
        return true;
    };

    // Pass 1: stream source to find which wanted names are already present.
    std::vector<bool> present(wanted.size(), false);
    std::vector<bool> differs(wanted.size(), false);
    std::string ferr;
    if (!RES_ForEachEntry(resPath,
            [&](const std::string& name, const uint8_t* data, size_t size) {
                for (size_t i = 0; i < wanted.size(); ++i) {
                    if (!equalsCI(name, wanted[i].name)) continue;
                    present[i] = true;
                    if (size != wanted[i].data.size() ||
                        !std::equal(data, data + size, wanted[i].data.begin())) {
                        differs[i] = true;
                    }
                }
            }, ferr)) {
        err = "membership scan failed: " + ferr;
        return false;
    }

    std::vector<RESEntry> toAdd;
    for (size_t i = 0; i < wanted.size(); ++i) {
        if (!present[i] || (replaceExisting && differs[i])) toAdd.push_back(wanted[i]);
    }
    if (toAdd.empty()) return true;  // requested bytes are already present

    const std::string bak = resPath + ".orig";
    if (!std::filesystem::exists(bak)) {
        std::error_code ec;
        std::filesystem::copy_file(resPath, bak, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            err = "backup failed, aborting (archive untouched): " + ec.message();
            return false;
        }
    }

    const std::string tmp = resPath + ".tmp";
    const bool merged = replaceExisting
        ? RES_StreamMerge(resPath, toAdd, tmp, err, onProgress)
        : RES_StreamAppend(resPath, toAdd, tmp, err, onProgress);
    if (!merged) {
        std::error_code rmec;
        std::filesystem::remove(tmp, rmec);
        return false;
    }

    std::error_code rec;
    std::filesystem::rename(tmp, resPath, rec);
    if (rec) {
        err = "atomic replace failed (original untouched): " + rec.message();
        std::error_code rmec;
        std::filesystem::remove(tmp, rmec);
        return false;
    }
    return true;
}

// Family prefix of a model id = the substring before the FIRST underscore
// (e.g. "405_01_1" -> "405", "igiagent_1" -> "igiagent"). The whole MEF family
// shares this prefix: hull + LODs + sub-parts are SEPARATE mefs the game needs
// all of (e.g. 405_01_1, 405_01_2..5, 405_02_1.., 405_03_1..). If there is no
// underscore the id is its own prefix.
static std::string ModelFamilyPrefix(const std::string& modelId) {
    const size_t us = modelId.find('_');
    return (us == std::string::npos) ? modelId : modelId.substr(0, us);
}

// True if `stem` (a .mef filename without extension) belongs to family `prefix`,
// i.e. it begins with exactly "<prefix>_". Scoped to the EXACT prefix-group so
// "405" does not pull in "4050" or unrelated ids. Case-insensitive.
static bool StemInFamily(const std::string& stem, const std::string& prefix) {
    const std::string want = prefix + "_";
    if (stem.size() <= want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::tolower((unsigned char)stem[i]) != std::tolower((unsigned char)want[i]))
            return false;
    return true;
}

// Run the bundled standalone converter (editor/tools/igi1conv.exe) and wait.
// The editor delegates ALL .mtp generation to igi1conv — its `dat to-mtp`
// reproduces the original game .mtp byte-for-byte, whereas an in-process
// incremental MTP write corrupted the model→texture mapping (transparent /
// wrong textures in-game and after reload). Returns true on exit code 0.
static bool RunIgi1conv(const std::string& args, std::string& err) {
    const std::string exe = Utils::GetExeDirectory() + "\\editor\\tools\\igi1conv\\igi1conv.exe";
    if (!std::filesystem::exists(exe)) {
        err = "igi1conv.exe not found: " + exe;
        return false;
    }
    std::string cmdLine = "\"" + exe + "\" " + args;

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::vector<char> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back('\0');

    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        err = "CreateProcess failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code != 0) { err = "igi1conv exit code " + std::to_string(code); return false; }
    return true;
}

struct StagedImportFile {
    std::string staged;
    std::string target;
    std::string backup;
};

static bool PublishStagedImportFiles(const std::vector<StagedImportFile>& files,
                                     std::string& error) {
    std::vector<StagedImportFile> backedUp;
    auto restore = [&]() {
        bool restored = true;
        for (auto it = backedUp.rbegin(); it != backedUp.rend(); ++it) {
            std::error_code ec;
            std::filesystem::copy_file(it->backup, it->target,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) restored = false;
        }
        return restored;
    };

    for (const auto& file : files) {
        std::error_code ec;
        if (!std::filesystem::copy_file(file.target, file.backup,
                std::filesystem::copy_options::overwrite_existing, ec)) {
            error = "backup failed for " + file.target + ": " + ec.message();
            if (!restore()) error += "; rollback also failed";
            return false;
        }
        backedUp.push_back(file);

        ec.clear();
        std::filesystem::copy_file(file.staged, file.target,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "publish failed for " + file.target + ": " + ec.message();
            if (!restore()) error += "; rollback also failed";
            return false;
        }
    }
    return true;
}

bool Renderer_Objects::AddModelToLevelRes(const std::string& modelId,
                                          const std::function<void(size_t,size_t)>& onProgress,
                                          int sourceLevel) {
    const std::string levelDir = Utils::GetIGIRootPath() + "\\missions\\location0\\level" +
        std::to_string(current_level_);
    const std::string modelsRes   = levelDir + "\\models\\level"   + std::to_string(current_level_) + ".res";
    const std::string texturesRes = levelDir + "\\textures\\level" + std::to_string(current_level_) + ".res";

    if (!std::filesystem::exists(modelsRes)) {
        Logger::Get().Log(LogLevel::WARNING, "[Renderer] AddModelToLevelRes: models archive missing: " + modelsRes);
        return false;
    }
    if (!std::filesystem::exists(texturesRes)) {
        Logger::Get().Log(LogLevel::WARNING, "[Renderer] AddModelToLevelRes: textures archive missing: " + texturesRes);
        return false;
    }

    // ── Enumerate the whole <prefix>_* MEF family on disk ────────────────────
    // IGI models are families: adding only <modelId>.mef leaves the hull/LODs/
    // sub-parts behind, which works on a level that already has them but breaks
    // (transparent/crash) elsewhere. Scan the SAME dirs FindModelFile searches,
    // collect every "<prefix>_*.mef", dedupe by base model id (stem).
    const std::string prefix = ModelFamilyPrefix(modelId);

    std::vector<std::string> familyDirs;
    {
        const std::string exeDir = Utils::GetExeDirectory();
        const std::string lvl = std::to_string(current_level_);
        familyDirs.push_back(exeDir + "\\editor\\models\\level" + lvl);
        familyDirs.push_back(Utils::GetIGIModelsPath(current_level_));
        for (int l = 1; l <= 14; ++l) {
            if (l == current_level_) continue;
            familyDirs.push_back(exeDir + "\\editor\\models\\level" + std::to_string(l));
            familyDirs.push_back(Utils::GetIGIModelsPath(l));
        }
        familyDirs.push_back(exeDir + "\\editor\\models\\common");
        familyDirs.push_back(Utils::GetIGIRootPath() + "\\missions\\location0\\common\\models");
        familyDirs.push_back(Utils::GetIGIRootPath() + "\\editor\\models");
    }

    // Map base model id -> resolved .mef path (first dir wins; deduped by id).
    std::map<std::string, std::string> familyModels; // ordered for stable logging
    for (const std::string& dir : familyDirs) {
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            const std::filesystem::path& p = entry.path();
            if (p.extension() != ".mef") continue;
            const std::string stem = p.stem().string();
            if (!StemInFamily(stem, prefix)) continue;
            familyModels.emplace(stem, p.string()); // keep first path seen for a given id
        }
    }

    // ALWAYS include the originally-requested model even if the glob missed it.
    // GetOrExtractMefTemp tries disk first, then falls back to pulling the mesh
    // bytes straight out of the OWNING level's packed .res (via the cross-level
    // ResCache) and extracting to a temp file — needed when the foreign model was
    // never extracted to disk (only lives inside another level's archive).
    if (!familyModels.count(modelId)) {
        EnsureGlobalTextureMapLoaded();
        auto mit = model_level_map_.find(modelId);
        if (mit != model_level_map_.end() && mit->second != current_level_) {
            LoadResCache(mit->second, Utils::GetIGIRootPath());
        }
        std::string mefPath = GetOrExtractMefTemp(modelId, /*isBuilding=*/false);
        if (mefPath.empty()) mefPath = GetOrExtractMefTemp(modelId, true);
        if (!mefPath.empty()) familyModels.emplace(modelId, mefPath);
    }

    // Same fallback for the REST of the family (LODs/sub-parts) when the disk
    // scan found nothing for them: consult the global model->level map (built
    // from every level's .dat) for family siblings, load that level's ResCache,
    // and extract each sibling to a temp file.
    {
        EnsureGlobalTextureMapLoaded();
        for (const auto& gm : global_texture_map_) {
            const std::string& stem = gm.first;
            if (familyModels.count(stem)) continue;
            if (!StemInFamily(stem, prefix)) continue;
            auto mit = model_level_map_.find(stem);
            if (mit != model_level_map_.end() && mit->second != current_level_) {
                LoadResCache(mit->second, Utils::GetIGIRootPath());
            }
            std::string mefPath = GetOrExtractMefTemp(stem, /*isBuilding=*/false);
            if (mefPath.empty()) mefPath = GetOrExtractMefTemp(stem, true);
            if (!mefPath.empty()) familyModels.emplace(stem, mefPath);
        }
    }

    // ── Resolve ATTA sub-model dependencies across families ──────────────────
    // A family model's .mef can ATTA-reference a sub-model in a completely
    // different numeric prefix (e.g. hull "420_04_1" references "235_01_1").
    // Without pulling those in too, the dependency is never packed into this
    // level's .res/.dat/.mtp, so the game reports "VirModel not available"
    // even though the parent renders fine in the editor (via the cross-level
    // ResCache fallback used only for in-viewport preview).
    {
        std::vector<std::string> worklist;
        for (const auto& fm : familyModels) worklist.push_back(fm.first);
        std::unordered_set<std::string> visited(worklist.begin(), worklist.end());

        while (!worklist.empty()) {
            std::string cur = worklist.back();
            worklist.pop_back();
            auto it = familyModels.find(cur);
            if (it == familyModels.end()) continue;

            std::ifstream mf(it->second, std::ios::binary);
            std::vector<uint8_t> mefBytes((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
            if (mefBytes.empty()) continue;

            ParsedGeometry geo;
            try { geo = ParseMefFileFromMemory(mefBytes); } catch (...) { continue; }

            for (const auto& a : geo.mefAttachments) {
                std::string aname(a.name, strnlen(a.name, 16));
                if (aname.empty() || !visited.insert(aname).second) continue;
                if (familyModels.count(aname)) continue;

                std::string mefPath = GetOrExtractMefTemp(aname, /*isBuilding=*/false);
                if (mefPath.empty()) mefPath = GetOrExtractMefTemp(aname, true);
                if (mefPath.empty()) {
                    Logger::Get().Log(LogLevel::WARNING, "[Renderer] AddModelToLevelRes: ATTA dependency '" +
                        aname + "' (needed by '" + cur + "') not found anywhere; skipping");
                    continue;
                }
                familyModels.emplace(aname, mefPath);
                worklist.push_back(aname);
                Logger::Get().Log(LogLevel::INFO, "[Renderer] AddModelToLevelRes: pulling in ATTA dependency '" +
                    aname + "' (needed by '" + cur + "')");
            }
        }
    }

    if (familyModels.empty()) {
        Logger::Get().Log(LogLevel::WARNING, "[Renderer] AddModelToLevelRes: no .mef found for family '" +
            prefix + "' (requested " + modelId + ")");
        return false;
    }

    // Resolve every mapping before touching either destination archive. The
    // requested model uses its owning level's exact DAT entry, so a stale
    // destination entry cannot become the import source.
    EnsureGlobalTextureMapLoaded();
    std::map<std::string, std::vector<std::string>> familyTextureIds;
    std::map<std::string, int> familySourceLevels;
    int requestedSourceLevel = sourceLevel;
    if (sourceLevel <= 0) {
        // The same model and texture IDs can denote different assets in
        // different levels. An untagged import is therefore valid only when
        // every exact source bundle is byte-identical.
        std::vector<ModelSourceBundle> bundles;
        std::unordered_set<int> sourceLevels;
        for (const auto& candidate : texture_sources_) {
            if (candidate.modelId != modelId || candidate.level <= 0 ||
                !sourceLevels.insert(candidate.level).second) continue;

            LoadResCache(candidate.level, Utils::GetIGIRootPath());
            ModelSourceBundle bundle;
            bundle.level = candidate.level;
            bundle.textureIds = candidate.textures;
            bundle.meshBytes = FindMeshDataFromLevel(modelId, candidate.level);
            for (const auto& textureId : candidate.textures) {
                bundle.textureBytes.push_back(
                    FindTextureDataFromLevel(textureId, candidate.level));
            }
            bundles.push_back(std::move(bundle));
        }
        requestedSourceLevel = SelectUnambiguousModelSourceLevel(bundles);
        if (requestedSourceLevel == 0) {
            Logger::Get().Log(LogLevel::WARNING,
                "[Renderer] AddModelToLevelRes: ambiguous source for '" + modelId +
                "'; select a source level explicitly");
            return false;
        }
        Logger::Get().Log(LogLevel::INFO,
            "[Renderer] AddModelToLevelRes: untagged equivalent source level " +
            std::to_string(requestedSourceLevel) + " for '" + modelId +
            "'");
    }
    if (requestedSourceLevel < 1 || requestedSourceLevel > 14) {
        Logger::Get().Log(LogLevel::WARNING,
            "[Renderer] AddModelToLevelRes: invalid source level " + std::to_string(requestedSourceLevel));
        return false;
    }
    for (const auto& fm : familyModels) {
        int familySourceLevel = requestedSourceLevel;
        auto mapping = GetTextureIdsForSourceModel(fm.first, familySourceLevel);
        if (mapping.empty() && fm.first != modelId) {
            // ATTA helper meshes can intentionally inherit their parent's
            // material list when they have no standalone DAT entry.
            mapping = GetTextureIdsForModel(fm.first);
        }
        if (mapping.empty()) {
            Logger::Get().Log(LogLevel::WARNING,
                "[Renderer] AddModelToLevelRes: unresolved texture mapping for '" + fm.first + "'");
            return false;
        }
        familyTextureIds.emplace(fm.first, std::move(mapping));
        familySourceLevels.emplace(fm.first, familySourceLevel);
    }

    // 1. Read ALL family MEFs before changing either destination archive.
    std::vector<RESEntry> modelEntries;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> looseModels; // for editor-content copy
    modelEntries.reserve(familyModels.size());
    for (const auto& fm : familyModels) {
        const int familySourceLevel = familySourceLevels.at(fm.first);
        LoadResCache(familySourceLevel, Utils::GetIGIRootPath());
        std::vector<uint8_t> mefBytes = FindMeshDataFromLevel(fm.first, familySourceLevel);
        if (mefBytes.empty()) {
            std::ifstream mf(fm.second, std::ios::binary);
            mefBytes.assign(std::istreambuf_iterator<char>(mf), std::istreambuf_iterator<char>());
        }
        if (mefBytes.empty()) {
            Logger::Get().Log(LogLevel::INFO, "[Renderer] AddModelToLevelRes: empty/unreadable family .mef, skipping: " + fm.second);
            continue;
        }
        looseModels.emplace_back(fm.first, mefBytes);
        modelEntries.push_back(RESEntry{ "LOCAL:models/" + fm.first + ".mef", std::move(mefBytes) });
    }
    if (modelEntries.empty()) {
        Logger::Get().Log(LogLevel::WARNING, "[Renderer] AddModelToLevelRes: all family .mef files empty/unreadable for '" + prefix + "'");
        return false;
    }

    const std::string datPath = levelDir + "\\level" + std::to_string(current_level_) + ".dat";
    const std::string mtpPath = levelDir + "\\level" + std::to_string(current_level_) + ".mtp";
    if (!std::filesystem::exists(datPath) || !std::filesystem::exists(mtpPath)) {
        Logger::Get().Log(LogLevel::WARNING,
            "[Renderer] AddModelToLevelRes: complete destination DAT/MTP pair is required");
        return false;
    }

    DATFile stagedDat;
    if (!persistent_dat_.valid || persistent_dat_path_ != datPath) {
        persistent_dat_ = DAT_Parse(datPath);
        persistent_dat_path_ = datPath;
    }
    if (!persistent_dat_.valid) {
        Logger::Get().Log(LogLevel::WARNING,
            "[Renderer] AddModelToLevelRes: destination DAT parse failed: " + persistent_dat_.error);
        return false;
    }
    stagedDat = persistent_dat_;

    const auto stageId = std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::filesystem::path stagingDir = std::filesystem::path(levelDir) /
        (".igi-import-staging-" + stageId);
    const std::filesystem::path stagingBackupDir = stagingDir / "backups";
    std::error_code stageEc;
    std::filesystem::create_directories(stagingBackupDir, stageEc);
    if (stageEc) {
        Logger::Get().Log(LogLevel::WARNING,
            "[Renderer] AddModelToLevelRes: staging directory creation failed: " + stageEc.message());
        return false;
    }
    const auto cleanupStaging = [&]() {
        std::error_code ec;
        std::filesystem::remove_all(stagingDir, ec);
    };
    const std::string stagedModelsRes = (stagingDir / "models.res").string();
    const std::string stagedTexturesRes = (stagingDir / "textures.res").string();
    const std::string stagedDatPath = (stagingDir / "level.dat").string();
    const std::string stagedMtpPath = (stagingDir / "level.mtp").string();
    for (const auto& pair : std::vector<std::pair<std::string, std::string>>{
            {modelsRes, stagedModelsRes}, {texturesRes, stagedTexturesRes},
            {datPath, stagedDatPath}, {mtpPath, stagedMtpPath}}) {
        std::error_code ec;
        if (!std::filesystem::copy_file(pair.first, pair.second,
                std::filesystem::copy_options::overwrite_existing, ec)) {
            Logger::Get().Log(LogLevel::WARNING,
                "[Renderer] AddModelToLevelRes: staging copy failed for " + pair.first + ": " + ec.message());
            cleanupStaging();
            return false;
        }
    }

    // Validate every MEF material slot against the selected ordered mapping
    // before staging any archive append. An out-of-range slot would otherwise
    // silently bind a different texture at runtime.
    for (const auto& fm : familyModels) {
        std::ifstream mf(fm.second, std::ios::binary);
        std::vector<uint8_t> mefBytes((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
        if (mefBytes.empty()) continue;
        try {
            const ParsedGeometry geometry = ParseMefFileFromMemory(mefBytes, fm.first);
            std::vector<int> materialSlots;
            if (geometry.renderBlocks.empty()) {
                materialSlots.push_back(0);
            } else {
                materialSlots.reserve(geometry.renderBlocks.size());
                for (const auto& block : geometry.renderBlocks) {
                    materialSlots.push_back(block.materialSlot);
                }
            }
            if (!IsTextureMappingCompatible(materialSlots,
                                             familyTextureIds.at(fm.first).size())) {
                Logger::Get().Log(LogLevel::WARNING,
                    "[Renderer] AddModelToLevelRes: MEF material slot exceeds ordered texture mapping for '" +
                    fm.first + "'");
                cleanupStaging();
                return false;
            }
        } catch (const std::exception& e) {
            Logger::Get().Log(LogLevel::WARNING,
                "[Renderer] AddModelToLevelRes: MEF material validation failed for '" +
                fm.first + "': " + e.what());
            cleanupStaging();
            return false;
        }
    }

    // 2. Gather ALL family textures before changing either destination archive.
    //    batched streaming append (the textures .res is 200MB+ — stream it once,
    //    never per-texture, and never RES_Parse it into RAM).
    int texAdded = 0;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> looseTextures;  // for editor-content copy
    std::vector<RESEntry> texEntries;
    std::set<std::string> seenTex;
    EnsureGlobalTextureMapLoaded();
    // AddModelToLevelRes appends to the current archive. Refresh before
    // resolving family textures so a previous import cannot leave a stale
    // archive index hiding entries that are already packed on disk.
    RefreshTextureResCache();
    for (const auto& fm : familyModels) {
        for (const std::string& texId : familyTextureIds.at(fm.first)) {
            if (!seenTex.insert(texId).second) continue; // dedupe across the family
            std::vector<uint8_t> texBytes;
            const int sourceLevel = familySourceLevels.at(fm.first);
            LoadResCache(sourceLevel, Utils::GetIGIRootPath());
            texBytes = FindTextureDataFromLevel(texId, sourceLevel);

            // Loose files are a fallback for a source level that has no packed
            // copy. Search only that level; a destination-level loose file with
            // the same ID must never override the selected source bytes.
            std::string texPath;
            if (texBytes.empty()) texPath = FindTextureFileInLevel(texId, sourceLevel);
            if (!texPath.empty() && texBytes.empty()) {
                std::ifstream tf(texPath, std::ios::binary);
                texBytes.assign(std::istreambuf_iterator<char>(tf), std::istreambuf_iterator<char>());
            }
            // Foreign texture not on disk: pull it straight from the owning
            // level's packed .res via the cross-level ResCache (same fallback
            // GetOrLoadTexture already uses for in-editor rendering).
            if (texBytes.empty() && sourceLevel != current_level_) {
                auto tit = texture_level_map_.find(texId);
                if (tit != texture_level_map_.end() && tit->second != current_level_) {
                    LoadResCache(tit->second, Utils::GetIGIRootPath());
                    texBytes = FindTextureDataFromLevel(texId, tit->second);
                }
            }
            if (texBytes.empty()) {
                // DAT files are not guaranteed to provide a complete owner
                // map for cross-level references. Search every packed texture
                // archive using the same indexed reader used by rendering.
                for (int level = 1; level <= 14 && texBytes.empty(); ++level) {
                    LoadResCache(level, Utils::GetIGIRootPath());
                    texBytes = FindTextureDataFromLevel(texId, level);
                }
            }
            if (texBytes.empty()) {
                Logger::Get().Log(LogLevel::WARNING, "[Renderer] AddModelToLevelRes: required texture " + texId +
                    " not found on disk or in any level's .res");
                cleanupStaging();
                return false;
            }
            looseTextures.emplace_back(texId, texBytes);
            texEntries.push_back(RESEntry{ "LOCAL:textures/" + texId + ".tex", std::move(texBytes) });
        }
    }

    // 3. Batch-add ALL family MEFs to the models archive (single streaming append).
    // This happens only after every required texture was resolved and packed.
    std::string err;
    if (!AddEntriesToRes(stagedModelsRes, modelEntries, err, nullptr, true)) {
        Logger::Get().Log(LogLevel::ERR, "[Renderer] AddModelToLevelRes: model add failed: " + err);
        cleanupStaging();
        return false;
    }
    Logger::Get().Log(LogLevel::INFO, "[Renderer] AddModelToLevelRes: staged family '" + prefix + "' (" +
        std::to_string(modelEntries.size()) + " mef)");

    if (!texEntries.empty()) {
        std::string stagedTextureError;
        if (!AddEntriesToRes(stagedTexturesRes, texEntries, stagedTextureError, onProgress, true)) {
            Logger::Get().Log(LogLevel::WARNING,
                "[Renderer] AddModelToLevelRes: staged texture pack failed: " + stagedTextureError);
            cleanupStaging();
            return false;
        }
        texAdded = static_cast<int>(texEntries.size());
    }

    // 3. Register the model + its textures so the GAME can resolve the model's
    //    materials (otherwise it renders transparent in-game). We update the TEXT
    //    level<N>.dat then drive mtp_decoder.exe to regenerate the binary level<N>.mtp
    //    (the game accepts the tool's output; a natively-written .mtp crashed it).
    //    Required metadata publication is part of import success. A model archive
    //    append without a matching DAT/MTP mapping is not a complete import.
    for (const auto& fm : familyModels) {
        if (!DAT_UpsertModel(stagedDat, fm.first, familyTextureIds.at(fm.first))) {
            continue;
        }
    }
    // DAT_UpsertModel returns false for both an idempotent match and an
    // unsuccessful/no-op path.  Verify the staged result explicitly so a
    // missing or mismatched required mapping cannot be published as success.
    for (const auto& fm : familyModels) {
        const auto mappingIt = std::find_if(stagedDat.models.begin(), stagedDat.models.end(),
            [&](const DATModelEntry& entry) { return entry.modelName == fm.first; });
        if (mappingIt == stagedDat.models.end() ||
            mappingIt->textures != familyTextureIds.at(fm.first)) {
            Logger::Get().Log(LogLevel::WARNING,
                "[Renderer] AddModelToLevelRes: staged DAT mapping verification failed for '" + fm.first + "'");
            cleanupStaging();
            return false;
        }
    }
    std::string datError;
    if (!DAT_WriteNative(stagedDat, stagedDatPath, datError)) {
        Logger::Get().Log(LogLevel::WARNING,
            "[Renderer] AddModelToLevelRes: staged DAT write failed: " + datError);
        cleanupStaging();
        return false;
    }
    std::string convErr;
    if (!RunIgi1conv("dat to-mtp \"" + stagedDatPath + "\" -o \"" + stagedMtpPath + "\"", convErr)) {
        Logger::Get().Log(LogLevel::WARNING,
            "[Renderer] AddModelToLevelRes: staged MTP conversion failed: " + convErr);
        cleanupStaging();
        return false;
    }
    const DATFile verifiedDat = DAT_Parse(stagedDatPath);
    if (!verifiedDat.valid || std::filesystem::file_size(stagedMtpPath) == 0) {
        Logger::Get().Log(LogLevel::WARNING,
            "[Renderer] AddModelToLevelRes: staged metadata verification failed");
        cleanupStaging();
        return false;
    }

    const std::vector<StagedImportFile> files{
        {stagedModelsRes, modelsRes, (stagingBackupDir / "models.res").string()},
        {stagedTexturesRes, texturesRes, (stagingBackupDir / "textures.res").string()},
        {stagedDatPath, datPath, (stagingBackupDir / "level.dat").string()},
        {stagedMtpPath, mtpPath, (stagingBackupDir / "level.mtp").string()}};
    std::string publishError;
    if (!PublishStagedImportFiles(files, publishError)) {
        Logger::Get().Log(LogLevel::WARNING,
            "[Renderer] AddModelToLevelRes: import publication failed: " + publishError);
        cleanupStaging();
        return false;
    }
    persistent_dat_ = verifiedDat;
    persistent_dat_path_ = datPath;
    cleanupStaging();

    // 4. Best-effort: copy loose files into the editor's content dirs so the editor
    //    keeps resolving them after a level reload. Failure here is non-fatal.
    try {
        const std::string exeDir = Utils::GetExeDirectory();
        const std::string lvl = std::to_string(current_level_);
        std::filesystem::path modelDstEditor = std::filesystem::path(exeDir) / "editor" / "models" / ("level" + lvl);
        std::filesystem::path modelDstContent = std::filesystem::path(exeDir) / "content" / "models" / ("level" + lvl);
        std::filesystem::create_directories(modelDstEditor);
        std::filesystem::create_directories(modelDstContent);
        for (const auto& m : looseModels) {
            std::ofstream out1((modelDstEditor / (m.first + ".mef")).string(), std::ios::binary);
            out1.write(reinterpret_cast<const char*>(m.second.data()), (std::streamsize)m.second.size());
            std::ofstream out2((modelDstContent / (m.first + ".mef")).string(), std::ios::binary);
            out2.write(reinterpret_cast<const char*>(m.second.data()), (std::streamsize)m.second.size());
        }
        if (!looseTextures.empty()) {
            std::filesystem::path texDstEditor = std::filesystem::path(exeDir) / "editor" / "textures" / ("level" + lvl);
            std::filesystem::path texDstContent = std::filesystem::path(exeDir) / "content" / "textures" / ("level" + lvl);
            std::filesystem::create_directories(texDstEditor);
            std::filesystem::create_directories(texDstContent);
            for (const auto& t : looseTextures) {
                std::ofstream out1((texDstEditor / (t.first + ".tex")).string(), std::ios::binary);
                out1.write(reinterpret_cast<const char*>(t.second.data()), (std::streamsize)t.second.size());
                std::ofstream out2((texDstContent / (t.first + ".tex")).string(), std::ios::binary);
                out2.write(reinterpret_cast<const char*>(t.second.data()), (std::streamsize)t.second.size());
            }
        }
    } catch (const std::exception& e) {
        Logger::Get().Log(LogLevel::WARNING, std::string("[Renderer] AddModelToLevelRes: editor-content copy failed: ") + e.what());
    }

    // 6. Invalidate caches and reload mapping so subsequent rendering sees the new assets immediately
    texture_map_level_ = -1;
    EnsureTextureMapLoaded();
    RefreshTextureResCache();
    for (const auto& fm : familyModels) {
        const auto texs = familyTextureIds.at(fm.first);
        global_texture_map_[fm.first] = texs;
        model_level_map_[fm.first] = current_level_;
        for (const auto& t : texs) {
            texture_level_map_[t] = current_level_;
        }
        mesh_cache_.erase(std::to_string(current_level_) + ":object:" + fm.first);
        mesh_cache_.erase(std::to_string(current_level_) + ":building:" + fm.first);
        skin_geometry_cache_.erase(fm.first);
    }

    Logger::Get().Log(LogLevel::INFO, "[Renderer] AddModelToLevelRes: packed family '" + prefix + "' = " +
        std::to_string(modelEntries.size()) + " model(s) + " + std::to_string(texAdded) + " texture(s)");
    return true;
}

bool Renderer_Objects::UpdateAttaLocalPosInMef(
    const std::string& parentModelId, bool isBuilding,
    int recordIndex, const glm::vec3& newLocalPos, const glm::mat3& newLocalRot)
{
    // 1. Update in-memory attachment cache so the editor renders at the new position.
    const std::string prefix = isBuilding ? "building:" : "object:";
    const std::string attKey = std::to_string(current_level_) + ":" + prefix + parentModelId;
    auto cit = attachment_cache_.find(attKey);
    if (cit != attachment_cache_.end() && recordIndex < (int)cit->second.size()) {
        cit->second[recordIndex].px = newLocalPos.x;
        cit->second[recordIndex].py = newLocalPos.y;
        cit->second[recordIndex].pz = newLocalPos.z;
    }

    // 2. Patch px/py/pz bytes in the MEF entry inside the level .res.
    const std::string gameRes = Utils::GetIGIRootPath() +
        "\\missions\\location0\\level" + std::to_string(current_level_) +
        "\\models\\level" + std::to_string(current_level_) + ".res";
    if (!std::filesystem::exists(gameRes)) {
        Logger::Get().Log(LogLevel::WARNING, "[Renderer] UpdateAttaLocalPosInMef: res not found: " + gameRes);
        return false;
    }
    RESFile res = RES_Parse(gameRes);
    if (!res.valid) return false;

    auto endsWithCI = [](const std::string& s, const std::string& suf) {
        if (suf.size() > s.size()) return false;
        for (size_t i = 0; i < suf.size(); ++i)
            if (std::tolower((unsigned char)s[s.size()-suf.size()+i]) !=
                std::tolower((unsigned char)suf[i])) return false;
        return true;
    };

    bool patched = false;
    for (auto& e : res.entries) {
        if (!endsWithCI(e.name, parentModelId + ".mef")) continue;
        size_t off = 20; // skip ILFF(16) + IRES(4)
        while (off + 16 <= e.data.size()) {
            char fc[5] = {0}; std::memcpy(fc, e.data.data() + off, 4);
            uint32_t size = 0, skip = 0;
            std::memcpy(&size, e.data.data() + off + 4, 4);
            std::memcpy(&skip, e.data.data() + off + 12, 4);
            if (std::strcmp(fc, "ATTA") == 0) {
                size_t base = off + 16 + (size_t)recordIndex * 68;
                if (base + 68 <= e.data.size()) {
                    // Write position
                    std::memcpy(e.data.data() + base + 16, &newLocalPos.x, 4);
                    std::memcpy(e.data.data() + base + 20, &newLocalPos.y, 4);
                    std::memcpy(e.data.data() + base + 24, &newLocalPos.z, 4);
                    // Write rotation matrix: MEF stores r[9] as column-major floats
                    // matching the GLM mat3 column layout [col][row].
                    float rot9[9] = {
                        newLocalRot[0][0], newLocalRot[0][1], newLocalRot[0][2],
                        newLocalRot[1][0], newLocalRot[1][1], newLocalRot[1][2],
                        newLocalRot[2][0], newLocalRot[2][1], newLocalRot[2][2]
                    };
                    std::memcpy(e.data.data() + base + 28, rot9, 36);
                    patched = true;
                }
                break;
            }
            if (skip == 0) break;
            off += skip;
        }
        if (patched) break;
    }
    if (!patched) {
        Logger::Get().Log(LogLevel::WARNING, "[Renderer] UpdateAttaLocalPosInMef: record " +
            std::to_string(recordIndex) + " not found in " + parentModelId + ".mef");
        return false;
    }

    try {
        std::string bak = gameRes + ".orig";
        if (!std::filesystem::exists(bak))
            std::filesystem::copy_file(gameRes, bak, std::filesystem::copy_options::overwrite_existing);
    } catch (...) {}

    std::string err;
    if (!RES_WriteEntries(res.entries, gameRes, err)) {
        Logger::Get().Log(LogLevel::ERR, "[Renderer] UpdateAttaLocalPosInMef: repack failed: " + err);
        return false;
    }
    Logger::Get().Log(LogLevel::INFO, "[Renderer] UpdateAttaLocalPosInMef: patched record " +
        std::to_string(recordIndex) + " in " + parentModelId + ".mef -> local(" +
        std::to_string(newLocalPos.x) + "," + std::to_string(newLocalPos.y) + "," +
        std::to_string(newLocalPos.z) + ")");
    return true;
}

void Renderer_Objects::PrePopulateAttaFromParsed(const std::string& modelId, bool isBuilding,
                                                  const std::vector<MefAttachment>& mefAttachments) {
    std::string cacheKey = std::to_string(current_level_) + ":" +
                           (isBuilding ? "building:" : "object:") + modelId;
    if (attachment_cache_.find(cacheKey) != attachment_cache_.end()) return;

    std::vector<AttachInfo> attaches;
    for (const auto& a : mefAttachments) {
        std::string aname(a.name, strnlen(a.name, 16));
        if (aname.empty()) continue;
        AttachInfo info;
        info.modelId = aname;
        info.px = a.px; info.py = a.py; info.pz = a.pz;
        info.r[0]=a.r00; info.r[1]=a.r01; info.r[2]=a.r02;
        info.r[3]=a.r03; info.r[4]=a.r04; info.r[5]=a.r05;
        info.r[6]=a.r06; info.r[7]=a.r07; info.r[8]=a.r08;
        attaches.push_back(info);
    }
    attachment_cache_[cacheKey] = std::move(attaches);
}

std::vector<AttachInfo> Renderer_Objects::GetModelAttachments(
    const std::string& modelId,
    bool isBuilding) {
    GetOrLoadMesh(modelId, isBuilding);
    const std::string cacheKey = std::to_string(current_level_) + ":" +
        (isBuilding ? "building:" : "object:") + modelId;
    const auto attachment_iterator = attachment_cache_.find(cacheKey);
    if (attachment_iterator == attachment_cache_.end()) {
        return {};
    }
    return attachment_iterator->second;
}

std::vector<glm::vec3> Renderer_Objects::GetModelMagicVertices(
    const std::string& modelId,
    bool isBuilding) {
    return GetOrLoadMesh(modelId, isBuilding).magicVertices;
}

void Renderer_Objects::LoadAttachmentsRecursive(const std::string& modelId, bool isBuilding,
                                                 std::unordered_set<std::string>& visited) {
    if (!visited.insert(modelId).second) return; // cycle guard

    std::string cacheKey = std::to_string(current_level_) + ":" +
                           (isBuilding ? "building:" : "object:") + modelId;

    // If attachment_cache_ already has this entry (from PrePopulateAttaFromParsed or a
    // previous call), skip re-parsing the MEF but still ensure every sub-model's mesh
    // is loaded — PrePopulateAttaFromParsed only fills the structural list, not meshes.
    auto existingIt = attachment_cache_.find(cacheKey);
    if (existingIt != attachment_cache_.end()) {
        for (const auto& att : existingIt->second) {
            std::string subKey = std::to_string(current_level_) + ":" +
                                 (isBuilding ? "building:" : "object:") + att.modelId;
            if (mesh_cache_.find(subKey) == mesh_cache_.end()) {
                bool loaded = false;
                std::vector<uint8_t> subBytes = FindMeshData(att.modelId);
                if (!subBytes.empty()) {
                    try {
                        Mesh subMesh = loadObjModelFromMemory(subBytes, att.modelId);
                        ApplyTexturesToMesh(subMesh, att.modelId, modelId);
                        mesh_cache_[subKey] = subMesh;
                        loaded = true;
                    } catch (...) {}
                }
                if (!loaded) {
                    std::string subFile = FindModelFile(att.modelId, isBuilding);
                    if (!subFile.empty()) {
                        try {
                            Mesh subMesh = loadObjModel(subFile, "");
                            ApplyTexturesToMesh(subMesh, att.modelId, modelId);
                            mesh_cache_[subKey] = subMesh;
                            loaded = true;
                        } catch (...) {}
                    } else {
                        // FindModelFile may have lazily indexed a cross-level .res as a
                        // side effect (its cross-level lookup block) -- retry the ResCache
                        // now that the owning level's models.res is indexed.
                        std::vector<uint8_t> lazyBytes = FindMeshData(att.modelId);
                        if (!lazyBytes.empty()) {
                            try {
                                Mesh subMesh = loadObjModelFromMemory(lazyBytes, att.modelId);
                                ApplyTexturesToMesh(subMesh, att.modelId, modelId);
                                mesh_cache_[subKey] = subMesh;
                                loaded = true;
                            } catch (...) {}
                        }
                    }
                    if (!loaded) {
                        Mesh empty; mesh_cache_[subKey] = empty;
                    }
                }
            }
            LoadAttachmentsRecursive(att.modelId, isBuilding, visited);
        }
        return;
    }

    // Parse the parent MEF to find ATTA records — try res cache first.
    ParsedGeometry geo;
    {
        bool parsedOk = false;
        std::vector<uint8_t> mefBytes = FindMeshData(modelId);
        if (!mefBytes.empty()) {
            try { geo = ParseMefFileFromMemory(mefBytes); parsedOk = true; } catch (...) {}
        }
        if (!parsedOk) {
            std::string filepath = FindModelFile(modelId, isBuilding);
            if (filepath.empty()) {
                attachment_cache_[cacheKey] = {};
                return;
            }
            try { geo = ParseMefFile(filepath); } catch (...) {}
        }
    }

    std::vector<AttachInfo> attaches;
    for (const auto& a : geo.mefAttachments) {
        std::string aname(a.name, strnlen(a.name, 16));
        if (aname.empty()) continue;

        AttachInfo info;
        info.modelId = aname;
        info.px = a.px; info.py = a.py; info.pz = a.pz;
        info.r[0]=a.r00; info.r[1]=a.r01; info.r[2]=a.r02;
        info.r[3]=a.r03; info.r[4]=a.r04; info.r[5]=a.r05;
        info.r[6]=a.r06; info.r[7]=a.r07; info.r[8]=a.r08;
        attaches.push_back(info);

        // Pre-warm the sub-model mesh — try res cache first, disk fallback.
        std::string subKey = std::to_string(current_level_) + ":" +
                             (isBuilding ? "building:" : "object:") + aname;
        if (mesh_cache_.find(subKey) == mesh_cache_.end()) {
            bool loaded = false;
            std::vector<uint8_t> subBytes = FindMeshData(aname);
            if (!subBytes.empty()) {
                try {
                    Mesh subMesh = loadObjModelFromMemory(subBytes, aname);
                    ApplyTexturesToMesh(subMesh, aname, modelId);
                    mesh_cache_[subKey] = subMesh;
                    Logger::Get().Log(LogLevel::INFO,
                        "[Renderer_Objects] ATTA sub-model loaded from ResCache: " + aname +
                        " (" + std::to_string(subMesh.vertexCount) + " verts)");
                    loaded = true;
                } catch (const std::exception& se) {
                    Logger::Get().Log(LogLevel::WARNING,
                        "[Renderer_Objects] ATTA ResCache parse failed for " + aname + ": " + se.what());
                }
            }
            if (!loaded) {
                std::string subFile = FindModelFile(aname, isBuilding);
                if (subFile.empty()) {
                    // FindModelFile may have lazily indexed a cross-level .res as a side
                    // effect (its cross-level lookup block) -- retry the ResCache now that
                    // the owning level's models.res is indexed, before giving up.
                    std::vector<uint8_t> lazyBytes = FindMeshData(aname);
                    if (!lazyBytes.empty()) {
                        try {
                            Mesh subMesh = loadObjModelFromMemory(lazyBytes, aname);
                            ApplyTexturesToMesh(subMesh, aname, modelId);
                            mesh_cache_[subKey] = subMesh;
                            Logger::Get().Log(LogLevel::INFO,
                                "[Renderer_Objects] ATTA sub-model loaded from ResCache (lazy): " + aname +
                                " (" + std::to_string(subMesh.vertexCount) + " verts)");
                            loaded = true;
                        } catch (const std::exception& se) {
                            Logger::Get().Log(LogLevel::WARNING,
                                "[Renderer_Objects] ATTA lazy ResCache parse failed for " + aname + ": " + se.what());
                        }
                    }
                    if (!loaded) {
                        Logger::Get().Log(LogLevel::WARNING,
                            "[Renderer_Objects] ATTA sub-model NOT FOUND: " + aname);
                        Mesh empty; mesh_cache_[subKey] = empty;
                    }
                } else {
                    try {
                        Mesh subMesh = loadObjModel(subFile, "");
                        ApplyTexturesToMesh(subMesh, aname, modelId);
                        mesh_cache_[subKey] = subMesh;
                        Logger::Get().Log(LogLevel::INFO,
                            "[Renderer_Objects] ATTA sub-model loaded from disk: " + aname +
                            " (" + std::to_string(subMesh.vertexCount) + " verts)");
                    } catch (const std::exception& se) {
                        Logger::Get().Log(LogLevel::ERR,
                            "[Renderer_Objects] ATTA sub-model load FAILED: " + aname + ": " + se.what());
                        Mesh empty; mesh_cache_[subKey] = empty;
                    }
                }
            }
        }
        // Recurse: parse this child's own ATTA section
        LoadAttachmentsRecursive(aname, isBuilding, visited);
    }

    attachment_cache_[cacheKey] = std::move(attaches);
    Logger::Get().Log(LogLevel::INFO,
        "[Renderer_Objects] Attachments for '" + modelId + "': " +
        std::to_string(attachment_cache_[cacheKey].size()));
}

// ─── DrawAttachmentsRecursive ────────────────────────────────────────────────
// Draws all ATTA children of parentModelId, then recurses into each child's
// own attachments. parentWorldMat is the UN-SCALED world transform of the parent
// (translate + rotate only), so children can position themselves relative to it.
// The 40.96 scale is applied only at the leaf draw call.
void Renderer_Objects::DrawAttachmentsRecursive(
    const std::string& topLevelModelId, const std::string& parentModelId, bool isBuilding, const glm::mat4& parentWorldMat,
    bool isTransparentPass, GLint loc_model, GLint loc_dirlight,
    GLint loc_ambient, GLint loc_useTex, GLint loc_tex, GLint loc_alpha,
    std::unordered_set<std::string>& drawn,
    glm::vec3 leafScale)
{
    // Skip rendering attachments for any weapon model
    if (IsWeaponModel(parentModelId)) return;
    std::string prefix = isBuilding ? "building:" : "object:";
    std::string attCacheKey = std::to_string(current_level_) + ":" + prefix + parentModelId;
    auto ait = attachment_cache_.find(attCacheKey);
    if (ait == attachment_cache_.end()) return;

    const auto& attsR = ait->second;
    for (size_t rri = 0; rri < attsR.size(); ++rri) {
        const auto &att = attsR[rri];
        // Find the mesh
        std::string subKey = std::to_string(current_level_) + ":" + prefix + att.modelId;
        auto sit = mesh_cache_.find(subKey);
        if (sit == mesh_cache_.end() || sit->second.vertexCount == 0) {
            subKey = std::to_string(current_level_) + ":object:" + att.modelId;
            sit = mesh_cache_.find(subKey);
        }
        if (sit == mesh_cache_.end() || sit->second.vertexCount == 0) continue;
        const Mesh &subMesh = sit->second;

        // Skip ATTA sub-models that are TASKTYPE_DEATHZONE magic objects — they are
        // invisible trigger/boarding zones, not visual geometry.
        EnsureDeathZoneIdsLoaded();
        if (deathzone_ids_.count(att.modelId)) continue;

        // Build the ATTA local rotation matrix (DirectX row-major → GLM column-major)
        glm::mat4 attLocalRot(
            att.r[0], att.r[1], att.r[2], 0.f,
            att.r[3], att.r[4], att.r[5], 0.f,
            att.r[6], att.r[7], att.r[8], 0.f,
            0.f,      0.f,      0.f,      1.f
        );

        // ATTA px/py/pz are raw floats from the MEF file, in the same coordinate
        // space as obj.pos (raw game units). parentWorldMat is also in raw game units
        // (translate=obj.pos, no scale), so no conversion needed.
        glm::vec3 localOff(att.px, att.py, att.pz);
        glm::vec3 worldPos = glm::vec3(parentWorldMat * glm::vec4(localOff, 1.f));

        // Extract parent rotation (upper-left 3x3 of the unscaled parent mat)
        glm::mat4 parentRot = parentWorldMat;
        parentRot[3] = glm::vec4(0.f, 0.f, 0.f, 1.f); // zero out translation

        // Unscaled world matrix for this attachment (used by children)
        glm::mat4 childWorldMat(1.0f);
        childWorldMat = glm::translate(childWorldMat, worldPos);
        childWorldMat = childWorldMat * parentRot * attLocalRot;

		// Helicopter rotor spin preview: rotor ATTA children of Heli objects spin
		// continuously so the viewport reads as alive (the game itself animates
		// rotors via runtime; we only preview in the editor). The main rotor is
		// the child at the highest local Z (top of fuselage); the tail rotor is
		// at the most-negative local Y (far tail). modelType-3 ATTA children are
		// typically the rotor blades; fall through for any other children.
		if (current_draw_obj_type_ == "Heli") {
			float maxZ = att.pz, minY = att.py;
			for (const auto& sib : attsR) {
				if (sib.pz > maxZ) maxZ = sib.pz;
				if (sib.py < minY) minY = sib.py;
			}
			const bool isMainRotor = (att.pz >= maxZ - 1.0f);
			const bool isTailRotor = (!isMainRotor && att.py <= minY + 1.0f);

			// Rebuild the placed transform from static ATTA data every frame.
			// Hub is locked. Yaw constant, pitch animates for main rotor (2 orientation axes fixed by locking shaft column, 1 moving).
			// Spin is applied in the placed attachment frame (post-multiply) so blades rotate 360 on their shaft with no orbit/translation.
			glm::vec3 hub = worldPos;
			glm::mat3 base;
			base[0] = glm::vec3(childWorldMat[0]);
			base[1] = glm::vec3(childWorldMat[1]);
			base[2] = glm::vec3(childWorldMat[2]);

			if (isMainRotor) {
				// Spin around placed column 0 (maps to pitch per IGI Yaw(Z)-Pitch(X)-Roll(Y) order).
				// Post-multiply rotates geometry in attachment local; lock column 0 so disk tilt (yaw/roll of plane) stays fixed.
				glm::vec3 n = base[0];
				if (glm::length(n) > 0.0001f) n = glm::normalize(n); else n = glm::vec3(1,0,0);

				float angle = elapsed_time_secs_ * 15.0f;  // positive; flip if blades spin opposite to expected
				glm::mat4 Rloc = glm::rotate(glm::mat4(1.0f), angle, glm::vec3(1,0,0)); // local X (pitch)
				glm::mat3 spun = base * glm::mat3(Rloc);
				spun[0] = n;  // keep shaft axis fixed (yaw constant, disk plane tilt fixed)

				childWorldMat[0] = glm::vec4(spun[0], 0.0f);
				childWorldMat[1] = glm::vec4(spun[1], 0.0f);
				childWorldMat[2] = glm::vec4(spun[2], 0.0f);
				childWorldMat[3] = glm::vec4(hub, 1.0f);
			} else if (isTailRotor) {
				// Tail rotor spins around its own placed lateral axis.
				int best = 0;
				float bestAbs = glm::abs(base[0].x);
				for (int c = 1; c < 3; ++c) {
					if (glm::abs(base[c].x) > bestAbs) { bestAbs = glm::abs(base[c].x); best = c; }
				}
				glm::vec3 n = glm::normalize(base[best]);

				float angle = -elapsed_time_secs_ * 25.0f;
				glm::mat4 R = glm::rotate(glm::mat4(1.0f), angle, n);

				glm::mat3 spun = glm::mat3(R) * base;
				spun[best] = n;

				childWorldMat[0] = glm::vec4(spun[0], 0.0f);
				childWorldMat[1] = glm::vec4(spun[1], 0.0f);
				childWorldMat[2] = glm::vec4(spun[2], 0.0f);
				childWorldMat[3] = glm::vec4(hub, 1.0f);
			}
		}

        // If this ATTA has a proxy object editing it, skip rendering it here —
        // the proxy renders it — but still recurse into children.
        if (IsAttaPromoted(att.modelId, worldPos) ||
            promoted_atta_records_.count(parentModelId + ":" + std::to_string(rri)) > 0) {
            std::string childKey = parentModelId + ">" + att.modelId;
            if (drawn.insert(childKey).second) {
                DrawAttachmentsRecursive(topLevelModelId, att.modelId, isBuilding, childWorldMat, isTransparentPass,
                                         loc_model, loc_dirlight, loc_ambient,
                                         loc_useTex, loc_tex, loc_alpha, drawn, leafScale);
            }
            continue;
        }

        // Skip sub-models that have only collision fallback geometry (no XTRV/DNER render
        // vertices). These render as misshapen boxes with fabricated UVs.  Still recurse
        // into their children — those may have proper render geometry.
        if (!subMesh.fromRenderMesh) {
            std::string childKey = parentModelId + ">" + att.modelId;
            if (drawn.insert(childKey).second) {
                DrawAttachmentsRecursive(topLevelModelId, att.modelId, isBuilding, childWorldMat, isTransparentPass,
                                         loc_model, loc_dirlight, loc_ambient,
                                         loc_useTex, loc_tex, loc_alpha, drawn, leafScale);
            }
            continue;
        }

        // Skip untextured sub-models (trigger zones, boarding areas, collision triggers)
        // — they'd appear as featureless dark/gray slabs with no visual value.
        // Exception: small untextured sub-models (e.g. sunglasses and other character
        // head/body accessories) are legitimate visual geometry — they carry no DAT texture
        // entry but should still draw via the hash-color fallback path below. Trigger/boarding
        // slabs span several metres (half-extent in the ~1000s of raw units); accessory
        // geometry is head-sized (a few hundred units at most), so a max-half-extent threshold
        // separates them cleanly without re-introducing the slabs.
        {
            bool subHasTex = (subMesh.textureID > 0);
            for (const auto& s : subMesh.subMeshes) {
                if (s.textureID > 0) { subHasTex = true; break; }
            }
            const float kSlabHalfExtent = 700.0f; // ~0.17 m: above any accessory, below any slab
            const float subMaxHalfExtent = std::max(subMesh.halfExtents.x,
                std::max(subMesh.halfExtents.y, subMesh.halfExtents.z));
            const bool isLargeSlab = (subMaxHalfExtent >= kSlabHalfExtent);
            EnsureAiModelIdsLoaded();
            const bool isAiModel = (ai_model_ids_.count(topLevelModelId) > 0);
            if (!subHasTex && (!isAiModel || isLargeSlab)) {
                std::string childKey = parentModelId + ">" + att.modelId;
                if (drawn.insert(childKey).second) {
                    DrawAttachmentsRecursive(topLevelModelId, att.modelId, isBuilding, childWorldMat, isTransparentPass,
                                             loc_model, loc_dirlight, loc_ambient,
                                             loc_useTex, loc_tex, loc_alpha, drawn, leafScale);
                }
                continue;
            }
        }

        // Scaled model matrix: X uses leafScale.x (intervalLen for splines, 40.96 for buildings)
        glm::mat4 attModel = glm::scale(childWorldMat, leafScale);

        glUniformMatrix4fv(loc_model, 1, GL_FALSE, glm::value_ptr(attModel));

        // Determine rendering pass requirements:
        //   attIsWindow: named "WINDOW"/"GLASS" → transparent pass only
        //   attHasAlpha: any submesh has alphaMode==2 (DNER rawOpacity non-zero)
        //                → draw in BOTH passes (opaque pass discards α<0.5 pixels,
        //                  transparent pass blends the rest)
        const bool attIsWindow = window_model_ids_.count(att.modelId) > 0;
        const bool attIsTree = (att.modelId == "900_01_1" ||
                                att.modelId == "902_01_1" ||
                                att.modelId == "905_01_1");
        bool attHasAlpha = false;
        if (!attIsTree) {
            for (const auto& s : subMesh.subMeshes) {
                if (s.alphaMode == 2) { attHasAlpha = true; break; }
            }
        }
        // No level-12 forced-opaque case: it discarded the winch-house ARGB
        // glass in the opaque pass (invisible until ATTA promotion).

        // Skip this sub-model if it doesn't belong in the current pass. Window
        // models can contain both opaque structural blocks and genuinely alpha
        // blended panes, so the model-level window classification must not force
        // the opaque blocks through the transparent pass.
        const bool drawInThisPass = !isTransparentPass || attHasAlpha;
        if (!drawInThisPass) {
            // Still recurse into children — they may need a different pass
            std::string childKey = parentModelId + ">" + att.modelId;
            if (drawn.insert(childKey).second) {
                DrawAttachmentsRecursive(topLevelModelId, att.modelId, isBuilding, childWorldMat, isTransparentPass,
                                          loc_model, loc_dirlight, loc_ambient,
                                          loc_useTex, loc_tex, loc_alpha, drawn, leafScale);
            }
            continue;
        }

        glUniform1f(loc_alpha, 1.0f);
        glUniform1f(loc_glass_min_, 0.0f);

        // Draw sub-model submeshes
        if (!subMesh.subMeshes.empty()) {
            for (const auto &sub : subMesh.subMeshes) {
                if (sub.VAO == 0 || sub.vertexCount == 0) continue;

                // In the transparent pass, only draw alpha submeshes (with blending).
                // Opaque blocks of a mixed window model belong in the opaque pass so
                // they write depth and remain visible through the normal scene path.
                bool subNeedsBlend = (sub.alphaMode == 2);
                if (attIsTree) subNeedsBlend = false;
                if (attIsWindow) {
                    if (isTransparentPass != subNeedsBlend) continue;
                } else if (isTransparentPass && !subNeedsBlend) {
                    continue;
                }

                if (subNeedsBlend) {
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                    glUniform1f(loc_alpha, attIsWindow ? 0.55f : sub.baseColorFactor.a);
                    if (attIsWindow) glUniform1f(loc_glass_min_, 0.30f);
                }

                if (sub.textureID > 0) {
                    // Same normal lighting for window/glass attachments so they stay
                    // clear and see-through (see top-level path — flat-gray reverted).
                    glUniform3f(loc_dirlight, 0.6f, 0.6f, 0.6f);
                    glUniform3f(loc_ambient,  global_ambient_.r, global_ambient_.g, global_ambient_.b);
                    glUniform1i(loc_useTex, 1);
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, sub.textureID);
                    glUniform1i(loc_tex, 0);
                } else {
                    // Untextured accessory (e.g. sunglasses): hash-color fallback, matching
                    // the untextured submesh path used for top-level objects above.
                    size_t hash = std::hash<std::string>{}(att.modelId);
                    float hr = global_ambient_.r + (float)(hash & 0xFF) / 255.0f * global_ambient_.r;
                    float hg = global_ambient_.g + (float)((hash >> 8) & 0xFF) / 255.0f * global_ambient_.g;
                    float hb = global_ambient_.b + (float)((hash >> 16) & 0xFF) / 255.0f * global_ambient_.b;
                    glUniform3f(loc_dirlight, hr * 0.6f, hg * 0.6f, hb * 0.6f);
                    glUniform3f(loc_ambient,  hr * global_ambient_.r, hg * global_ambient_.g, hb * global_ambient_.b);
                    glUniform1i(loc_useTex, 0);
                }
                glBindVertexArray(sub.VAO);
                glDrawArrays(GL_TRIANGLES, 0, sub.vertexCount);

                if (subNeedsBlend) {
                    glDisable(GL_BLEND);
                    glUniform1f(loc_alpha, 1.0f);
                    if (attIsWindow) glUniform1f(loc_glass_min_, 0.0f);
                }
            }
            glBindVertexArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        } else if (subMesh.textureID > 0) {
            glUniform3f(loc_dirlight, 0.6f, 0.6f, 0.6f);
            glUniform3f(loc_ambient,  global_ambient_.r, global_ambient_.g, global_ambient_.b);
            glUniform1i(loc_useTex, 1);
            GL_BindTexture2D(0, subMesh.textureID);
            glUniform1i(loc_tex, 0);
            renderModel(subMesh);
        }

        if (attIsWindow) {
            glDisable(GL_BLEND);
            glUniform1f(loc_alpha, 1.0f);
            glUniform1f(loc_glass_min_, 0.0f); // don't leak the glass sheen floor
        }

        // Recurse into this attachment's own children
        std::string childKey = parentModelId + ">" + att.modelId;
        if (drawn.insert(childKey).second) {
            DrawAttachmentsRecursive(topLevelModelId, att.modelId, isBuilding, childWorldMat, isTransparentPass,
                                      loc_model, loc_dirlight, loc_ambient,
                                      loc_useTex, loc_tex, loc_alpha, drawn, leafScale);
        }
    }
}

void Renderer_Objects::DrawAttachmentsForSpline(
    const std::string& modelId, bool isBuilding,
    const glm::mat4& unscaledWorldMat, GLuint ubo_mats,
    glm::vec3 leafScale)
{
    EnsureWindowModelIdsLoaded();

    glUseProgram(shader_program_);
    glBindBufferBase(GL_UNIFORM_BUFFER, ubo_binding_point_, ubo_mats);

    GLint loc_model    = glGetUniformLocation(shader_program_, "u_model");
    GLint loc_dirlight = glGetUniformLocation(shader_program_, "u_dirlight");
    GLint loc_ambient  = glGetUniformLocation(shader_program_, "u_ambient");
    GLint loc_useTex   = glGetUniformLocation(shader_program_, "u_useTexture");
    GLint loc_tex      = glGetUniformLocation(shader_program_, "u_texture");
    GLint loc_alpha    = glGetUniformLocation(shader_program_, "u_alpha");

    glUniform1f(loc_alpha, 1.0f);

    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-2.0f, -2.0f);

    // Opaque pass
    {
        std::unordered_set<std::string> drawn;
        DrawAttachmentsRecursive(modelId, modelId, isBuilding, unscaledWorldMat, /*isTransparentPass=*/false,
                                 loc_model, loc_dirlight, loc_ambient, loc_useTex, loc_tex, loc_alpha, drawn, leafScale);
    }

    // Transparent pass (windows / glass)
    {
        std::unordered_set<std::string> drawn;
        DrawAttachmentsRecursive(modelId, modelId, isBuilding, unscaledWorldMat, /*isTransparentPass=*/true,
                                 loc_model, loc_dirlight, loc_ambient, loc_useTex, loc_tex, loc_alpha, drawn, leafScale);
    }

    // Restore blend/depth state — transparent pass may leave them dirty.
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glUniform1f(loc_alpha, 1.0f);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glUseProgram(0);
}




