/******************************************************************************
 * @file    renderer_objects_texture.cpp
 * @brief   Renderer_Objects: texture/DAT mapping, lookup, and application
 *          Split from renderer_objects.cpp; shares renderer_objects_internal.h.
 *****************************************************************************/
#include "renderer_objects_internal.h"

// True if a DAT texture id is flagged with an alpha-bearing pixel format
// (e.g. "009_09_1_argb8888"). These textures carry real per-texel transparency
// (sunglasses lenses, visors) that must be alpha-blended; the opaque pass would
// otherwise hard-cutout the lens (shader: discard when texColor.a < 0.5) and
// render it as a distorted black blob. RGB565/_r5g6b5 have no alpha.
static bool TextureIdHasAlpha(const std::string& texId) {
    static const char* const kAlphaSuffixes[] = {
        "_argb8888", "_argb1555", "_argb4444",
        "_a8r8g8b8", "_a1r5g5b5", "_a4r4g4b4"
    };
    for (const char* suf : kAlphaSuffixes) {
        const size_t sufLen = std::strlen(suf);
        if (texId.size() > sufLen) {
            std::string tail = texId.substr(texId.size() - sufLen);
            for (auto& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (tail == suf) return true;
        }
    }
    return false;
}

std::string Renderer_Objects::GetLevelTexturesPath() const {
    return Utils::GetExeDirectory() + "\\editor\\textures\\level" + std::to_string(current_level_);
}

std::string Renderer_Objects::GetLevelTextureDatPath() const {
    // Try local extracted DAT first
    const std::string localDat = Utils::GetExeDirectory() +
        "\\editor\\textures\\level" + std::to_string(current_level_) +
        "\\level" + std::to_string(current_level_) + ".dat";
    if (std::filesystem::exists(localDat)) return localDat;

    // Fall back to the game's DAT in the IGI root
    const std::string root = Utils::GetIGIRootPath();
    return root + "\\missions\\location0\\level" + std::to_string(current_level_) +
        "\\level" + std::to_string(current_level_) + ".dat";
}

void Renderer_Objects::LoadDatIntoMap(const std::string& datPath, std::map<std::string, std::vector<std::string>>& outMap) {
    DATFile dat = DAT_Parse(datPath);
    if (!dat.valid) {
        Logger::Get().Log(LogLevel::WARNING, "[TEX Native] DAT_Parse failed for: " + datPath + " Error: " + dat.error);
        return;
    }

    for (const auto& m : dat.models) {
        // We use operator[] instead of emplace so that later definitions
        // (which might contain the actual textures) overwrite earlier empty/stale definitions.
        outMap[m.modelName] = m.textures;
    }
}

void Renderer_Objects::EnsureTextureMapLoaded() {
    if (texture_map_level_ == current_level_) return;

    model_texture_map_cache_.clear();
    texture_map_level_ = current_level_;

    const std::string datPath = GetLevelTextureDatPath();
    Logger::Get().Log(LogLevel::INFO, "[TEX Native] Loading DAT map from " + datPath);
    LoadDatIntoMap(datPath, model_texture_map_cache_);
    Logger::Get().Log(LogLevel::INFO, "[TEX Native] DAT map loaded level=" + std::to_string(current_level_) + " models=" + std::to_string(model_texture_map_cache_.size()));
}

void Renderer_Objects::EnsureGlobalTextureMapLoaded() const {
    if (global_texture_map_loaded_) return;
    global_texture_map_loaded_ = true;

    const std::string igiRoot = Utils::GetIGIRootPath();
    const std::string exeDir  = Utils::GetExeDirectory();

    for (int lvl = 1; lvl <= 14; ++lvl) {
        // Try local extracted DAT first, then game path
        std::string localDat = exeDir + "\\editor\\textures\\level" + std::to_string(lvl) +
                               "\\level" + std::to_string(lvl) + ".dat";
        std::string gameDat  = igiRoot + "\\missions\\location0\\level" + std::to_string(lvl) +
                               "\\level" + std::to_string(lvl) + ".dat";

        std::string datPath = "";
        if (std::filesystem::exists(localDat)) datPath = localDat;
        else if (std::filesystem::exists(gameDat)) datPath = gameDat;

        if (!datPath.empty()) {
            DATFile dat = DAT_Parse(datPath);
            if (dat.valid) {
                for (const auto& m : dat.models) {
                    texture_sources_.push_back(ModelTextureSource{lvl, m.modelName, m.textures});
                    if (global_texture_map_.find(m.modelName) == global_texture_map_.end()) {
                        global_texture_map_[m.modelName] = m.textures;
                        model_level_map_[m.modelName] = lvl;
                    }
                }
                for (const auto& t : dat.allTextures) {
                    texture_level_map_.emplace(t, lvl);
                }
            }
        }
    }

    // Also scan the common package (weapons, shared models not tied to any level)
    std::string commonDat = igiRoot + "\\missions\\location0\\common\\location0.dat";
    if (std::filesystem::exists(commonDat)) {
        DATFile dat = DAT_Parse(commonDat);
        if (dat.valid) {
            for (const auto& m : dat.models) {
                if (global_texture_map_.find(m.modelName) == global_texture_map_.end()) {
                    global_texture_map_[m.modelName] = m.textures;
                    texture_sources_.push_back(ModelTextureSource{0, m.modelName, m.textures});
                    model_level_map_.emplace(m.modelName, 0);
                }
            }
            for (const auto& t : dat.allTextures) {
                if (texture_level_map_.find(t) == texture_level_map_.end())
                    texture_level_map_[t] = 0;
            }
        }
    }

    Logger::Get().Log(LogLevel::INFO,
        "[TEX Native] Global DAT map loaded: " + std::to_string(global_texture_map_.size()) + " models across all levels");
}

std::vector<std::string> Renderer_Objects::GetTextureIdsForModel(const std::string& modelId) {
    EnsureTextureMapLoaded();

    // 1. Current level exact match
    {
        auto it = model_texture_map_cache_.find(modelId);
        if (it != model_texture_map_cache_.end()) {
            return it->second;
        }
    }

    // 2. Global DAT search across all other levels exact match
    EnsureGlobalTextureMapLoaded();
    {
        auto levelIt = model_level_map_.find(modelId);
        if (levelIt != model_level_map_.end()) {
            const auto* source = FindExactTextureSource(texture_sources_, levelIt->second, modelId);
            if (source != nullptr) return source->textures;
        }
        auto it = global_texture_map_.find(modelId);
        if (it != global_texture_map_.end()) {
            return it->second;
        }
    }

    // 3. Sibling LOD fallback within same variant: NNN_VV_N -> NNN_VV_1 (e.g. 001_02_2 -> 001_02_1)
    {
        size_t p2 = modelId.rfind('_');
        if (p2 != std::string::npos && p2 > 0) {
            std::string sameVariantPrimary = modelId.substr(0, p2) + "_1";
            if (sameVariantPrimary != modelId) {
                auto it = model_texture_map_cache_.find(sameVariantPrimary);
                if (it != model_texture_map_cache_.end()) {
                    return it->second;
                }
                auto levelIt = model_level_map_.find(sameVariantPrimary);
                if (levelIt != model_level_map_.end()) {
                    const auto* source = FindExactTextureSource(
                        texture_sources_, levelIt->second, sameVariantPrimary);
                    if (source != nullptr) return source->textures;
                }
                auto git = global_texture_map_.find(sameVariantPrimary);
                if (git != global_texture_map_.end()) {
                    return git->second;
                }
            }
        }
    }

    // 4. Variant fallback within current level: NNN_XX_N -> NNN_01_1
    {
        size_t p1 = modelId.find('_');
        if (p1 != std::string::npos && p1 + 4 < modelId.size()) {
            std::string primary = modelId.substr(0, p1) + "_01_1";
            if (primary != modelId) {
                auto it = model_texture_map_cache_.find(primary);
                if (it != model_texture_map_cache_.end()) {
                    return it->second;
                }
            }
        }
    }

    // 5. Variant fallback in global DAT
    {
        size_t p1 = modelId.find('_');
        if (p1 != std::string::npos && p1 + 4 < modelId.size()) {
            std::string primary = modelId.substr(0, p1) + "_01_1";
            if (primary != modelId) {
                auto levelIt = model_level_map_.find(primary);
                if (levelIt != model_level_map_.end()) {
                    const auto* source = FindExactTextureSource(texture_sources_, levelIt->second, primary);
                    if (source != nullptr) return source->textures;
                }
                auto it = global_texture_map_.find(primary);
                if (it != global_texture_map_.end()) {
                    return it->second;
                }
            }
        }
    }

    // 5. Fallback to just the prefix (e.g. NNN_XX_X -> NNN)
    // AI models like "004_02_1" only have "004" mapped in the global DAT.
    {
        size_t p1 = modelId.find('_');
        if (p1 != std::string::npos) {
            std::string prefix = modelId.substr(0, p1);
            if (prefix != modelId) {
                auto it = model_texture_map_cache_.find(prefix);
                if (it != model_texture_map_cache_.end()) return it->second;
                
                auto git = global_texture_map_.find(prefix);
                if (git != global_texture_map_.end()) return git->second;
            }
        }
    }

    Logger::Get().Log(LogLevel::DEBUG, "[TEX Native] DAT miss (all sources) for modelId=" + modelId);
    return {};
}

std::vector<std::string> Renderer_Objects::GetTextureIdsForSourceModel(
    const std::string& modelId, int sourceLevel) {
    EnsureTextureMapLoaded();
    if (sourceLevel == current_level_) {
        const auto local = model_texture_map_cache_.find(modelId);
        if (local != model_texture_map_cache_.end()) return local->second;
    }
    EnsureGlobalTextureMapLoaded();
    const auto* source = FindExactTextureSource(texture_sources_, sourceLevel, modelId);
    return source != nullptr ? source->textures : std::vector<std::string>{};
}

std::string Renderer_Objects::FindTextureFile(const std::string& textureId) const {
    // Helper: search one directory for the texture file
    auto searchDir = [&](const std::filesystem::path& texturesPath) -> std::string {
        if (!std::filesystem::exists(texturesPath)) return "";

        const std::filesystem::path exactPath = texturesPath / (textureId + ".tex");
        if (std::filesystem::exists(exactPath)) return exactPath.string();

        for (const auto& entry : std::filesystem::directory_iterator(texturesPath)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".tex") continue;
            const std::string stem = entry.path().stem().string();
            // Match the exact id or one of the explicitly recognized pixel-format
            // suffixes. Numeric/model suffixes are part of the texture identity;
            // a loose underscore match can silently bind a different texture.
            const bool match = stem == textureId ||
                StripTextureFormatSuffix(stem) == textureId;
            if (match) {
                return entry.path().string();
            }
        }
        return "";
    };

    // 1. Search local extracted textures directory first
    std::string result = searchDir(std::filesystem::path(GetLevelTexturesPath()));
    if (!result.empty()) return result;

    // 2. Fall back to the game's own texture directory for this level
    const std::string igiTexDir = Utils::GetIGIRootPath() +
        "\\missions\\location0\\level" + std::to_string(current_level_) + "\\textures";
    result = searchDir(std::filesystem::path(igiTexDir));
    if (!result.empty()) {
        Logger::Get().Log(LogLevel::DEBUG,
            "[TEX Native] Found texture in IGI root level textures: " + result);
        return result;
    }

    // 3. Extracted common location0 textures (from location0.res)
    const std::string commonLocalTexDir = Utils::GetExeDirectory() + "\\editor\\textures\\common";
    result = searchDir(std::filesystem::path(commonLocalTexDir));
    if (!result.empty()) {
        Logger::Get().Log(LogLevel::DEBUG,
            "[TEX Native] Found texture in extracted common folder: " + result);
        return result;
    }

    // 4. Game common/shared textures folder (shared across all levels)
    const std::string igiCommonTexDir = Utils::GetIGIRootPath() + "\\textures";
    result = searchDir(std::filesystem::path(igiCommonTexDir));
    if (!result.empty()) {
        Logger::Get().Log(LogLevel::DEBUG,
            "[TEX Native] Found texture in IGI common folder: " + result);
        return result;
    }

    // 5. Game location0 common folder (shared across all location0 levels)
    const std::string igiLoc0CommonTexDir = Utils::GetIGIRootPath() + "\\missions\\location0\\common\\textures";
    result = searchDir(std::filesystem::path(igiLoc0CommonTexDir));
    if (!result.empty()) {
        Logger::Get().Log(LogLevel::DEBUG,
            "[TEX Native] Found texture in IGI location0 common folder: " + result);
        return result;
    }

    // Lazy on-demand index for cross-level texture references: extend the in-memory
    // .res index to include the target level's archives without any disk extraction.
    // Note: EnsureGlobalTextureMapLoaded/texture_level_map_ lookup is done by the
    // non-const GetOrLoadTexture caller before calling FindTextureFile for fallback.
    EnsureGlobalTextureMapLoaded();

    // 6. Search all other levels' extracted and game texture directories.
    //    Textures like "006_07_1" live in level 6's folder but are referenced by
    //    models appearing in other levels (e.g. 003_02_1 in level 7).
    const std::string exeDir2  = Utils::GetExeDirectory();
    const std::string igiRoot2 = Utils::GetIGIRootPath();
    for (int lvl = 1; lvl <= 14; ++lvl) {
        if (lvl == current_level_) continue;
        result = searchDir(exeDir2 + "\\editor\\textures\\level" + std::to_string(lvl));
        if (!result.empty()) return result;
        result = searchDir(igiRoot2 + "\\missions\\location0\\level" + std::to_string(lvl) + "\\textures");
        if (!result.empty()) return result;
    }

    // This helper is also a speculative loose-file probe used before packed
    // .res lookup. Do not report a hard error here: callers may still resolve
    // the texture from an archive and the final GetOrLoadTexture warning is
    // the authoritative missing-texture diagnostic.
    return "";
}

std::string Renderer_Objects::FindTextureFileInLevel(const std::string& textureId, int levelNo) const {
    auto searchDir = [&](const std::filesystem::path& texturesPath) -> std::string {
        if (!std::filesystem::exists(texturesPath)) return "";

        const std::filesystem::path exactPath = texturesPath / (textureId + ".tex");
        if (std::filesystem::exists(exactPath)) return exactPath.string();

        for (const auto& entry : std::filesystem::directory_iterator(texturesPath)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".tex") continue;
            const std::string stem = entry.path().stem().string();
            if (stem == textureId || StripTextureFormatSuffix(stem) == textureId) {
                return entry.path().string();
            }
        }
        return "";
    };

    if (levelNo > 0) {
        const std::string level = std::to_string(levelNo);
        std::string result = searchDir(Utils::GetExeDirectory() + "\\editor\\textures\\level" + level);
        if (!result.empty()) return result;
        result = searchDir(Utils::GetIGIRootPath() + "\\missions\\location0\\level" + level + "\\textures");
        if (!result.empty()) return result;
    } else {
        std::string result = searchDir(Utils::GetExeDirectory() + "\\editor\\textures\\common");
        if (!result.empty()) return result;
        result = searchDir(Utils::GetIGIRootPath() + "\\textures");
        if (!result.empty()) return result;
        result = searchDir(Utils::GetIGIRootPath() + "\\missions\\location0\\common\\textures");
        if (!result.empty()) return result;
    }
    return "";
}

GLuint Renderer_Objects::GetOrLoadTexture(const std::string& textureId) {
    if (textureId.empty()) return 0;

    const std::string cacheKey = std::to_string(current_level_) + ":" + textureId;
    auto it = texture_cache_.find(cacheKey);
    if (it != texture_cache_.end()) {
        return it->second;
    }

    // ── 1. Try in-memory .res index (preferred: no disk extraction) ───────────
    {
        std::vector<uint8_t> bytes = FindTextureData(textureId);
        if (!bytes.empty()) {
            pics_s pics{};
            if (Tex_LoadFromMemory(bytes.data(), bytes.size(), pics) && pics.pics_ && pics.num_pic_ > 0) {
                const pic_s* pic = pics.pics_;
                Logger::Get().Log(LogLevel::INFO,
                    "[TEX] ResCache loaded " + textureId +
                    " " + std::to_string(pic->width_) + "x" + std::to_string(pic->height_));
                const GLuint tex = GL_RegisterTexture(pic, GL_REPEAT, GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR, true);
                texture_cache_[cacheKey] = tex;
                Pic_FreePics(pics);
                return tex;
            }
            Pic_FreePics(pics);
        }
    }

    // ── 1b. Lazy cross-level .res index if texture not found in current level ────
    {
        EnsureGlobalTextureMapLoaded();
        int targetLvl = -1;
        auto tit = texture_level_map_.find(textureId);
        if (tit == texture_level_map_.end()) {
            const std::string stripped = StripTextureFormatSuffix(textureId);
            tit = texture_level_map_.find(stripped);
        }
        if (tit != texture_level_map_.end() && tit->second != current_level_) {
            targetLvl = tit->second;
            Logger::Get().Log(LogLevel::INFO, "[TEX] Lazy indexing level " +
                std::to_string(targetLvl) + " .res for cross-level: " + textureId);
            LoadResCache(targetLvl, Utils::GetIGIRootPath());
        }

        std::vector<uint8_t> xBytes = FindTextureData(textureId);
        if (xBytes.empty()) {
            // DAT files are not guaranteed to provide a complete owner map.
            // Search all other packed texture archives using the indexed reader.
            for (int lvl = 1; lvl <= 14 && xBytes.empty(); ++lvl) {
                if (lvl == current_level_ || lvl == targetLvl) continue;
                LoadResCache(lvl, Utils::GetIGIRootPath());
                xBytes = FindTextureData(textureId);
            }
        }

        if (!xBytes.empty()) {
            pics_s pics2{};
            if (Tex_LoadFromMemory(xBytes.data(), xBytes.size(), pics2) && pics2.pics_ && pics2.num_pic_ > 0) {
                const GLuint tex = GL_RegisterTexture(pics2.pics_, GL_REPEAT, GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR, true);
                texture_cache_[cacheKey] = tex;
                Pic_FreePics(pics2);
                return tex;
            }
            Pic_FreePics(pics2);
        }
    }

    // ── 2. Fall back to on-disk search (game dir loose files) ─────────────────
    const std::string strippedId = StripTextureFormatSuffix(textureId);
    std::string texturePath = FindTextureFile(textureId);
    if (texturePath.empty() && strippedId != textureId) {
        texturePath = FindTextureFile(strippedId);
    }
    if (texturePath.empty()) {
        Logger::Get().Log(LogLevel::WARNING, "[TEX] Texture NOT FOUND: " + textureId);
        return 0;
    }

    pics_s pics{};
    if (!Tex_Load(texturePath.c_str(), pics) || !pics.pics_ || pics.num_pic_ <= 0) {
        Logger::Get().Log(LogLevel::ERR, "[TEX] Failed to decode: " + texturePath);
        Pic_FreePics(pics);
        return 0;
    }

    const pic_s* pic = pics.pics_;
    Logger::Get().Log(LogLevel::INFO,
        "[TEX] Disk loaded " + textureId +
        " " + std::to_string(pic->width_) + "x" + std::to_string(pic->height_) +
        " path=" + texturePath);

    const GLuint texture = GL_RegisterTexture(pic, GL_REPEAT, GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR, true);
    texture_cache_[cacheKey] = texture;
    Pic_FreePics(pics);
    return texture;
}

void Renderer_Objects::ApplyTexturesToMesh(Mesh& mesh, const std::string& modelId, const std::string& parentModelId) {
    std::vector<std::string> textureIds = GetTextureIdsForModel(modelId);
    bool isDatMiss = textureIds.empty();

    // When a sub-model has no DAT entry, IGI ATTA sub-models commonly share material slots with their
    // parent building, so inherit the parent's texture list in that case.
    if (!parentModelId.empty() && isDatMiss) {
        const std::vector<std::string> parentIds = GetTextureIdsForModel(parentModelId);
        const bool parentHasRealEntry = !parentIds.empty();
        if (parentHasRealEntry) {
            Logger::Get().Log(LogLevel::INFO,
                "[TEX Native] Sub-model '" + modelId + "' has no DAT entry; inheriting " +
                std::to_string(parentIds.size()) + " texture(s) from parent '" + parentModelId + "'");
            textureIds = parentIds;
            isDatMiss = false;
        }
    }

    // Conversely, some vehicles or magic models (_01_1) have no textures in DAT, but their interior 
    // attachments (_02_1) DO have textures. If we still have a DAT miss, inherit from the typical child.
    if (isDatMiss) {
        size_t p1 = modelId.find("_01_1");
        if (p1 != std::string::npos) {
            std::string childId = modelId.substr(0, p1) + "_02_1";
            std::vector<std::string> childTexIds = GetTextureIdsForModel(childId);
            if (!childTexIds.empty()) {
                Logger::Get().Log(LogLevel::INFO,
                    "[TEX Native] Model '" + modelId + "' has no DAT entry; inheriting " +
                    std::to_string(childTexIds.size()) + " texture(s) from child '" + childId + "'");
                textureIds = childTexIds;
                isDatMiss = false;
            }
        }
    }

    // If it's still a DAT miss (not explicitly in dat), fallback to self name first.
    if (isDatMiss) {
        textureIds = { modelId };
    }

    if (textureIds.empty()) {
        Logger::Get().Log(LogLevel::INFO, "[TEX Native] No textures mapped for modelId=" + modelId);
        return;
    }

    // For AI character models (sunglasses-bearing), load the STRIPPED texture id
    // so "009_09_1_argb8888" → "009_09_1.tex" (opaque body texture → solid black
    // glasses). The _argb8888 variant is a tiny 32×32 semi-transparent overlay
    // used by the game engine; loading it in the editor gives barely-visible lenses.
    // Non-AI models (guard tower, lattice) need the exact _argb8888.tex for
    // transparent rendering and keep the "exact first" preference.
    EnsureAiModelIdsLoaded();
    const bool isAiModel = ai_model_ids_.count(modelId) > 0;

    std::vector<GLuint> textures;
    textures.reserve(textureIds.size());
    for (const auto& textureId : textureIds) {
        const std::string loadId = (isAiModel && TextureIdHasAlpha(textureId))
            ? StripTextureFormatSuffix(textureId)
            : textureId;
        textures.push_back(GetOrLoadTexture(loadId));
    }

    if (!mesh.subMeshes.empty()) {
        size_t assigned = 0;

        // Find the best valid (non-zero) texture to use as fallback for submeshes
        // that fall outside the DAT texture list range.
        GLuint fallbackTexture = 0;
        for (const GLuint t : textures) {
            if (t != 0) { fallbackTexture = t; }
        }

        for (size_t i = 0; i < mesh.subMeshes.size(); ++i) {
            GLuint texture = 0;
            const int matSlot = mesh.subMeshes[i].materialSlot;
            std::string resolvedTexId; // tracks the actual texture id string for ARGB detection

            if (textures.size() == 1) {
                texture = textures[0];
                resolvedTexId = textureIds.empty() ? "" : textureIds[0];
            } else if (matSlot >= 0 && static_cast<size_t>(matSlot) < textures.size()) {
                texture = textures[matSlot];
                resolvedTexId = (static_cast<size_t>(matSlot) < textureIds.size()) ? textureIds[matSlot] : "";
            } else if (matSlot > 0 && !textures.empty()) {
                EnsureAiModelIdsLoaded();
                if (ai_model_ids_.count(modelId) > 0) {
                    // materialSlot is out of range — sunglasses-bearing characters (009_01_1,
                    // 014_01_1, 014_02_1) where the MEF block references a slot beyond the
                    // local DAT texture list. Resolve via global DAT.
                    bool resolved = false;
                    EnsureGlobalTextureMapLoaded();
                    {
                        auto git = global_texture_map_.find(modelId);
                        if (git != global_texture_map_.end() &&
                            static_cast<size_t>(matSlot) < git->second.size()) {
                            const std::string& globalTexId = git->second[static_cast<size_t>(matSlot)];
                            GLuint globalTex = GetOrLoadTexture(globalTexId);
                            if (globalTex) {
                                texture = globalTex;
                                resolvedTexId = globalTexId; // preserve for ARGB detection
                                resolved = true;
                                Logger::Get().Log(LogLevel::INFO,
                                    "[TEX Native] AI materialSlot out of range resolved via global DAT for modelId=" + modelId +
                                    " submeshIndex=" + std::to_string(i) +
                                    " materialSlot=" + std::to_string(matSlot) +
                                    " globalTexId=" + globalTexId);
                            }
                        }
                    }
                    if (!resolved) {
                        texture = textures.back();
                        resolvedTexId = textureIds.empty() ? "" : textureIds.back();
                        Logger::Get().Log(LogLevel::WARNING,
                            "[TEX Native] AI materialSlot out of range, using last texture for modelId=" + modelId +
                            " submeshIndex=" + std::to_string(i) +
                            " materialSlot=" + std::to_string(matSlot) +
                            " textureCount=" + std::to_string(textures.size()));
                    }
                } else {
                    size_t wrapped = static_cast<size_t>(matSlot) % textures.size();
                    texture = textures[wrapped];
                    resolvedTexId = (wrapped < textureIds.size()) ? textureIds[wrapped] : "";
                    Logger::Get().Log(LogLevel::WARNING,
                        "[TEX Native] materialSlot out of range, wrapping for modelId=" + modelId +
                        " submeshIndex=" + std::to_string(i) +
                        " materialSlot=" + std::to_string(matSlot) +
                        " textureCount=" + std::to_string(textures.size()) +
                        " wrappedSlot=" + std::to_string(wrapped));
                }
            } else if (i < textures.size()) {
                texture = textures[i];
                resolvedTexId = (i < textureIds.size()) ? textureIds[i] : "";
            } else {
                texture = fallbackTexture;
            }

            mesh.subMeshes[i].textureID = texture;
            mesh.subMeshes[i].textureName = resolvedTexId;
            if (texture) {
                ++assigned;
                // ARGB-format textures carry real per-texel alpha (sunglasses lenses,
                // guard tower lattice, wire fences, etc.). Use resolvedTexId which
                // captures the actual texture id in ALL code paths including out-of-range
                // ARGB textures on non-AI models (lattice, fences, decals) need alpha
                // blending via the transparent pass. AI model ARGB sub-meshes (sunglasses)
                // must render OPAQUE with the cutout shader so the black lens pixels show
                // as solid black — blending them makes them see-through.
                if (TextureIdHasAlpha(resolvedTexId) && ai_model_ids_.count(modelId) == 0) {
                    mesh.subMeshes[i].alphaMode = 2;
                    mesh.subMeshes[i].baseColorFactor.a = 0.95f;
                }
            }
        }

        if (!mesh.subMeshes.empty()) {
            mesh.textureID = mesh.subMeshes.front().textureID;
            mesh.textureName = mesh.subMeshes.front().textureName;
        }

        Logger::Get().Log(
            LogLevel::INFO,
            "[TEX Native] Applied textures to modelId=" + modelId +
            " subMeshes=" + std::to_string(mesh.subMeshes.size()) +
            " datTextures=" + std::to_string(textureIds.size()) +
            " assigned=" + std::to_string(assigned));

        if (textureIds.size() != mesh.subMeshes.size()) {
            Logger::Get().Log(
                LogLevel::WARNING,
                "[TEX Native] WARNING: Texture/submesh count mismatch for modelId=" + modelId +
                " subMeshes=" + std::to_string(mesh.subMeshes.size()) +
                " datTextures=" + std::to_string(textureIds.size()));
        }
        return;
    }

    mesh.textureID = textures.front();
    if (mesh.textureName.empty() && !textureIds.empty()) mesh.textureName = textureIds.front();
    Logger::Get().Log(
        LogLevel::INFO,
        "[TEX Native] Applied legacy single texture to modelId=" + modelId +
        " textureId=" + textureIds.front() +
        " glId=" + std::to_string(mesh.textureID));
}
