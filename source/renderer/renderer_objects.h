#pragma once
#include "../pch.h"
#include "model.h"
#include "mef_native.h"
#include "../level/level_objects.h"
#include "dat_writer.h"
#include "res_writer.h"
#include "model_texture_resolution.h"
#include <map>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <set>

struct AttachInfo {
    std::string modelId;
    float px, py, pz;           // raw game-unit position from ATTA record
    float r[9];                 // 3x3 rotation matrix (r00..r08)
};

// One pickable ATTA sub-model instance, captured during the picking pass so the
// app can "promote" it into a real, editable EditRigidObj task at the same world
// transform when the user clicks it.
struct AttaPickEntry {
    int          parentObjIndex = -1;
    std::string  modelId;
    std::string  immediateParentModelId; // MEF file that directly contains this ATTA record
    glm::vec3    worldPos = glm::vec3(0.0f);
    glm::mat3    worldRot = glm::mat3(1.0f);
    float        scale = 1.0f;
    glm::vec3    localPos = glm::vec3(0.0f);
    int          recordIndex = -1;
    glm::mat4    parentWorldMat = glm::mat4(1.0f);
};

class Renderer_Objects {
public:
    // Pick IDs at/above this base denote a picked ATTA sub-model (entry =
    // returnedValue - kAttaPickBase); below it they are normal object indices.
    static constexpr int kAttaPickBase = 0x800000; // 8388608

    bool GetAttaPickEntry(int entry, AttaPickEntry& out) const {
        if (entry < 0 || entry >= (int)atta_pick_entries_.size()) return false;
        out = atta_pick_entries_[entry];
        return true;
    }
    // Permanently suppress an ATTA (by its ORIGINAL model@worldPos key) once it has
    // been promoted to a real task. Stays hidden even after the promoted object is
    // moved away — otherwise the original ATTA reappears (looks like a clone/ghost
    // and z-fights). Cleared on level load.
    void SuppressAtta(const std::string& key) { suppressed_atta_keys_.insert(key); }
    void ClearSuppressedAttas() { suppressed_atta_keys_.clear(); promoted_atta_records_.clear(); }
    void MarkAttaPromotedByRecord(const std::string& parentModelId, int recordIndex) {
        promoted_atta_records_.insert(parentModelId + ":" + std::to_string(recordIndex));
    }
    bool SuppressAttachmentInMef(const std::string& parentModelId, const std::string& attModelId, const glm::vec3& localPos);
    // Copy the loose <modelId>.mef into this level's .res archive (with .orig backup),
    // so a model that renders in-editor but is absent from the packed archive becomes
    // visible in-game. Returns true on success (or if already present). (issue 2)
    bool AddModelToLevelRes(const std::string& modelId,
                            const std::function<void(size_t,size_t)>& onProgress = nullptr,
                            int sourceLevel = 0);
    const std::vector<ModelTextureSource>& GetModelTextureSources() const {
        EnsureGlobalTextureMapLoaded();
        return texture_sources_;
    }
    bool UpdateAttaLocalPosInMef(const std::string& parentModelId, bool isBuilding, int recordIndex, const glm::vec3& newLocalPos, const glm::mat3& newLocalRot);
    // Stable key "model@roundedWorldPos" used to match an ATTA against an EditRigidObj.
    static std::string AttaOccupancyKey(const std::string& modelId, const glm::vec3& worldPos);
    Renderer_Objects();
    ~Renderer_Objects();

    bool Init();
    void Shutdown();

    void SetLevel(int level) { current_level_ = level; }
    void ClearCaches();

    // Build an in-memory index for the level's .res archives so textures and
    // models are loaded directly from the archive on demand — no disk extraction.
    // Call once after level assets are available (replacing EnsureLevelAssets).
    void LoadResCache(int levelNo, const std::string& igi_path);
    // Clear all .res indexes (called on level switch before LoadResCache for new level).
    void ClearResCache();
    // Re-index the current level's texture archive after an editor-side append.
    void RefreshTextureResCache();

    // Find texture/mesh bytes from the in-memory .res index.
    // Returns empty vector if not found (callers fall back to on-disk paths).
    std::vector<uint8_t> FindTextureData(const std::string& textureId) const;
    // Resolve texture bytes from one explicit level archive, preserving source
    // provenance when a foreign destination contains the same texture name.
    std::vector<uint8_t> FindTextureDataFromLevel(const std::string& textureId,
                                                  int levelNo) const;
    std::vector<uint8_t> FindMeshData(const std::string& modelId) const;
    std::vector<uint8_t> FindMeshDataFromLevel(const std::string& modelId,
                                               int levelNo) const;

    // Pre-fill the attachment cache from already-parsed geometry (avoids re-reading
    // MEF bytes that were already loaded for mesh creation in GetOrLoadMesh).
    void PrePopulateAttaFromParsed(const std::string& modelId, bool isBuilding,
                                   const std::vector<MefAttachment>& mefAttachments);

    // Diagnostics: live cache occupancy (for level-switch logging).
    size_t GetMeshCacheCount() const { return mesh_cache_.size(); }
    size_t GetTextureCacheCount() const { return texture_cache_.size(); }

    void Draw(GLuint ubo_mats, bool overlay_wireframe, const std::vector<LevelObject>& objects, int selected_object_index, int hover_object_index, int draw_parts, const glm::vec3& camera_pos, bool show_magic_obj_spheres = false, const std::unordered_set<int>* skip_static_draw_indices = nullptr);
    int PickObjectAtScreen(int x, int y, int w, int h,
                           GLuint ubo_mats,
                           const std::vector<LevelObject>& objects,
                           int draw_parts,
                           const glm::vec3& camera_pos,
                           int selected_object_index);
    int CountObjectVisiblePixels(GLuint ubo_mats,
                                 const std::vector<LevelObject>& objects,
                                 int draw_parts,
                                 const glm::vec3& camera_pos,
                                 int selected_object_index,
                                 int target_object_index,
                                 int viewport_width,
                                 int viewport_height,
                                 const std::vector<float>& scene_depth,
                                 int* target_id_pixels = nullptr);
    // Target-only material/submesh evidence for the native visual-integrity
    // gate. IDs are one-based submesh indices and are never shared with scene
    // object IDs, so unrelated objects cannot satisfy a part requirement.
    struct VisualEvidence {
        struct PartInventory {
            int id = 0;
            int vertexCount = 0;
            int triangleCount = 0;
            int materialSlot = -1;
            int alphaMode = 0;
            std::string textureIdentity;
            bool textureResolved = true;
            float textureChromaticPixelRatio = 0.0f;
            glm::vec3 localBoundsMin{0.0f};
            glm::vec3 localBoundsMax{0.0f};
        };
        int width = 0;
        int height = 0;
        std::vector<int> expectedPartIds;
        std::vector<int> strictPartIds;
        std::vector<PartInventory> expectedParts;
        std::vector<uint8_t> targetMask;
        std::vector<int> partIds;
        std::vector<float> targetDepth;
    };
    VisualEvidence CaptureObjectVisualEvidence(GLuint ubo_mats,
                                               const std::vector<LevelObject>& objects,
                                               int target_object_index,
                                               int viewport_width,
                                               int viewport_height,
                                               const std::vector<float>& scene_depth);
    static bool IsSkippedModelId(const std::string& modelId);
    glm::vec3 GetMeshExtents(const std::string& modelId, bool isBuilding);
    float GetMeshRadius(const std::string& modelId, bool isBuilding);
    glm::vec3 GetMeshCenter(const std::string& modelId, bool isBuilding);
    // Tests the camera against each top-level Building's transformed visual
    // mesh bound. This is deliberately independent of editor draw filters.
    bool IsCameraInsideBuildingBounds(const std::vector<LevelObject>& objects,
                                      const glm::vec3& cameraPos);
    float GetMeshZOffset(const std::string& modelId, bool isBuilding);
    Mesh GetOrLoadMesh(const std::string& modelId, bool isBuilding);
    std::vector<AttachInfo> GetModelAttachments(const std::string& modelId, bool isBuilding);
    std::vector<glm::vec3> GetModelMagicVertices(const std::string& modelId, bool isBuilding);
    bool IsLadderMagicObject(const std::string& modelId);
    // Render one model centered in a viewport rectangle, auto-rotated by (rotX,rotY)
    // radians, for the model-picker preview. Uploads its own preview camera into the
    // shared matrices UBO (overwritten next frame by the scene), so it must be called
    // after the main scene pass. Does not touch the fixed-function matrix stack.
    void DrawModelPreview(const std::string& modelId, GLuint ubo_mats,
                          int vpX, int vpY, int vpW, int vpH,
                          float rotX, float rotY);
    // Draws one static prop mesh at an arbitrary world matrix within the normal
    // scene pass — used to attach a weapon mesh to a live bone transform (e.g. an
    // AI's hand). Binds the shared Matrices UBO (ubo_mats) so the shader gets the
    // scene's Proj*View*GlobalScale; worldMat is the model matrix in raw world units.
    void DrawAttachedMesh(const std::string& modelId, bool isBuilding, const glm::mat4& worldMat, GLuint ubo_mats);
    GLuint GetShaderProgram() const { return shader_program_; }
    void DrawAttachmentsForSpline(const std::string& modelId, bool isBuilding,
                                  const glm::mat4& unscaledWorldMat, GLuint ubo_mats,
                                  glm::vec3 leafScale = glm::vec3(40.96f));
    // Skeletal-skin vertex/bone data for live animation playback (CPU skinning).
    // Looks up and parses the model's .mef once, then caches it (rest-pose
    // RenderVertex array + per-bone rest pivots, same data the static mesh
    // cache was built from). Returns nullptr if the model can't be found.
    const ParsedGeometry* GetOrLoadSkinGeometry(const std::string& modelId, bool isBuilding);

    // Lightmap textures for the "Calculate Light Mapping" button, keyed by
    // the EXACT placement's taskId (not modelId) — mesh_cache_ is shared per
    // modelId across every placement of a building, so storing these on
    // SubMesh would make two placements of the same model show whichever
    // lightmap was calculated last. The draw loop looks this up per-object.
    // taskId -> {textures, pos/rot at calculation time}. The pos/rot are recorded
    // so the draw loop can detect a moved/rotated object and drop its now-stale
    // bake back to dynamic lighting instead of rendering the wrong-orientation bake.
    void SetLightmapForTask(const std::string& taskId, std::vector<GLuint> textures,
                            const glm::dvec3& bakedPos, const glm::dvec3& bakedRot);
    void ClearLightmapForTask(const std::string& taskId);
    // Free all baked lightmap textures + per-task bake/stale state (Escape-menu
    // Lightmaps OFF, and level switch). Leaves indoor-ambient metadata intact.
    void ClearAllLightmaps();
    const std::vector<GLuint>* GetLightmapForTask(const std::string& taskId) const;
    // Returns false if taskId has no recorded bake pose (treated as not stale).
    bool IsLightmapStale(const std::string& taskId, const glm::dvec3& curPos, const glm::dvec3& curRot) const;
    bool HasLightmapForTask(const std::string& taskId) const {
        auto* v = GetLightmapForTask(taskId); return v && !v->empty();
    }
    // Retrieve the pos/rot recorded at the time taskId's lightmap was baked
    // (needed as recalc's --rot-orig). Returns false if no bake is recorded.
    bool GetLightmapBakePose(const std::string& taskId, glm::dvec3& pos, glm::dvec3& rot) const {
        auto it = lightmap_bake_pose_by_task_.find(taskId);
        if (it == lightmap_bake_pose_by_task_.end()) return false;
        pos = it->second.pos; rot = it->second.rot; return true;
    }
    // Public access to model-file resolution + the resolved level sun (for the
    // editor's lightmap recalc pipeline).
    std::string GetModelFilePath(const std::string& modelId, bool isBuilding) {
        return FindModelFile(modelId, isBuilding);
    }
    // Like GetModelFilePath but falls back to extracting the MEF from the
    // res cache to a temp file when there are no extracted disk files.
    // Returns the path (disk or temp). Caller must NOT delete it (temp files
    // are tracked in tmp_mef_paths_ and cleaned on ClearCaches).
    std::string GetOrExtractMefTemp(const std::string& modelId, bool isBuilding);
    glm::vec3 GetSunDir() const { return sun_dir_; }
    glm::vec3 GetSunFrontColor() const { return sun_front_color_; }
    glm::vec3 GetSunBackColor() const { return sun_back_color_; }
    glm::vec3 GetGlobalAmbient() const { return global_ambient_; }
    void SetGlobalAmbient(const glm::vec3& amb) { global_ambient_ = amb; }

    // Escape-menu "Lightmaps" checkbox — when false, calculated lightmaps are
    // never bound during render regardless of SetLightmapForTask state.
    void SetLightmapsEnabled(bool enabled) { lightmaps_enabled_ = enabled; }
    bool LightmapsEnabled() const { return lightmaps_enabled_; }

    // Real level sun direction/color, parsed from the level's Dirlight/DirlightKeyframe
    // QSC task. Replaces the previous hardcoded shader light direction/colors so
    // dynamic-lit (non-lightmapped, or stale-lightmap) faces match the game.
    void SetSunLight(const glm::vec3& dir, const glm::vec3& frontColor, const glm::vec3& backColor) {
        sun_dir_ = dir; sun_front_color_ = frontColor; sun_back_color_ = backColor;
    }

    // The level's GlobalLight "Texture filter gamma" (e.g. 0.675) — the game applies
    // this as a post-lighting pow() curve; gamma < 1 brightens mid-tones. Without it
    // our raw linear lighting looks noticeably flatter/darker than in-game.
    void SetGlobalGamma(float gamma) { global_gamma_ = gamma; }

    // Atmospheric fog color and far-clip distance (from GlobalLightKeyframe).
    // Used by the object shader to match the game's warm haze over buildings.
    void SetFogParams(const glm::vec3& color, float far_dist) {
        fog_color_ = color; fog_far_ = far_dist;
    }
    void SetFogEnabled(bool enabled) { fog_enabled_ = enabled; }

    // Indoor ambient fallback: each Building/EditRigidObj's LightmapInfo child
    // declares a dim "Indoors ambient light" (e.g. 0.08) used as fallback when
    // no baked lightmap exists, so interiors look dark/warm like the game instead
    // of receiving full outdoor sun. Populated per level load from app_level.cpp.
    void SetIndoorAmbientForTask(const std::string& taskId, const glm::vec3& rgb) {
        indoor_ambient_by_task_[taskId] = rgb;
    }
    const glm::vec3* GetIndoorAmbientForTask(const std::string& taskId) const {
        auto it = indoor_ambient_by_task_.find(taskId);
        return it != indoor_ambient_by_task_.end() ? &it->second : nullptr;
    }

private:
    // Per-level .res index (name→offset) — avoids extracting to editor/textures or editor/models.
    // Multiple .res files (current level + common) are merged into one flat map.
    struct ResIndex {
        std::string                                     res_path;
        std::unordered_map<std::string, ResEntryInfo>  index;
    };
    std::vector<ResIndex> res_tex_indexes_;   // level + common texture .res indexes
    std::vector<ResIndex> res_model_indexes_; // level + common model .res indexes
    std::vector<std::string> tmp_mef_paths_;  // temp files created by GetOrExtractMefTemp

    int current_level_ = 1;
    bool lightmaps_enabled_ = false;
    glm::vec3 sun_dir_ = glm::vec3(0.5f, 1.0f, 0.5f);
    glm::vec3 sun_front_color_ = glm::vec3(0.6f, 0.6f, 0.6f);
    glm::vec3 sun_back_color_  = glm::vec3(0.4f, 0.4f, 0.4f);
    glm::vec3 global_ambient_  = glm::vec3(0.15f, 0.15f, 0.15f);
    float global_gamma_ = 1.0f;
    glm::vec3 fog_color_ = glm::vec3(0.15f, 0.15f, 0.15f);
    float fog_far_ = 1e9f; // huge default = no fog until SetupFog() is called from level.cpp
	bool fog_enabled_ = true;
	float elapsed_time_secs_ = 0.0f;      // rotor animation accumulator (Heli main/tail)
	std::string current_draw_obj_type_; // set before DrawAttachmentsRecursive for Heli rotor spin
	std::map<std::string, glm::vec3> indoor_ambient_by_task_; // taskId -> LightmapInfo "Indoors ambient light"
    struct BakePose {
        glm::dvec3 pos;
        glm::dvec3 rot;
        glm::vec3  sun_dir; // sun direction at bake time — stale if sun moved significantly
    };
    std::map<std::string, BakePose> lightmap_bake_pose_by_task_;
    std::map<std::string, Mesh> mesh_cache_;
    std::map<std::string, GLuint> texture_cache_;
    std::map<std::string, std::vector<std::string>> model_texture_map_cache_;
    mutable std::map<std::string, std::vector<std::string>> global_texture_map_;
    mutable bool global_texture_map_loaded_ = false;
    mutable std::vector<ModelTextureSource> texture_sources_;
    mutable std::map<std::string, int> model_level_map_;
    mutable std::map<std::string, int> texture_level_map_;
    std::map<std::string, std::vector<AttachInfo>> attachment_cache_;
    std::map<std::string, ParsedGeometry> skin_geometry_cache_;
    std::map<std::string, std::vector<GLuint>> lightmap_textures_by_task_;
    // Per-pick-pass capture of pickable ATTA sub-models (see AttaPickEntry).
    std::vector<AttaPickEntry> atta_pick_entries_;
    // Keys (model@pos) of EditRigidObj tasks; an ATTA matching one is suppressed
    // (it has been promoted to / duplicated by a real task). Rebuilt each frame.
    std::unordered_set<std::string> editrigid_occupancy_;
    // Persistent keys of ATTAs promoted this session (by their ORIGINAL position),
    // so they stay hidden even after the promoted object is moved.
    std::unordered_set<std::string> suppressed_atta_keys_;
    std::set<std::string>           promoted_atta_records_; // "parentModelId:recordIndex"
    int texture_map_level_ = -1;

    // Persistent in-memory DAT: accumulates AddModelToLevelRes additions within
    // a session so mtp_decoder (which may rewrite the .dat on disk as a side-effect)
    // cannot cause previously-added families to be lost on subsequent adds.
    DATFile persistent_dat_;
    std::string persistent_dat_path_;  // path this dat was loaded from

    GLuint shader_program_;
    GLint  loc_glass_min_ = -1;  // u_glassMin location (glass sheen floor), set in Draw
    GLuint ubo_binding_point_;
    GLuint selection_vao_, selection_vbo_;
    GLuint selection_shader_ = 0;
    std::unordered_set<std::string> logged_draw_buildings_;
    std::set<std::string> window_model_ids_;
    bool window_ids_loaded_ = false;

    std::set<std::string> ai_model_ids_;
    bool ai_ids_loaded_ = false;

    std::map<std::string, float> portal_distances_;
    bool portal_distances_loaded_ = false;

    std::unordered_set<std::string> deathzone_ids_;
    bool deathzone_ids_loaded_ = false;

    std::set<std::string> magicobj_ids_;
    std::set<std::string> ladder_magicobj_ids_;
    bool magicobj_ids_loaded_ = false;
    GLuint sphere_vao_ = 0;
    GLuint sphere_vbo_ = 0;
    int sphere_vertex_count_ = 0;

    // GPU color picking FBO
    GLuint pick_fbo_          = 0;
    GLuint pick_color_tex_    = 0;
    GLuint pick_depth_rb_     = 0;
    GLuint pick_shader_prog_  = 0;
    int    pick_fbo_w_        = 0;
    int    pick_fbo_h_        = 0;

    void LoadAttachmentsRecursive(const std::string& modelId, bool isBuilding, std::unordered_set<std::string>& visited);
    void DrawAttachmentsRecursive(const std::string& topLevelModelId, const std::string& parentModelId, bool isBuilding, const glm::mat4& parentWorldMat,
                                   bool isTransparentPass, GLint loc_model, GLint loc_dirlight,
                                   GLint loc_ambient, GLint loc_useTex, GLint loc_tex, GLint loc_alpha,
                                   std::unordered_set<std::string>& drawn,
                                   glm::vec3 leafScale = glm::vec3(40.96f));

    // Like DrawAttachmentsRecursive but uses the picking shader. Each ATTA sub-model
    // that has no matching EditRigidObj (occupancy check) gets a UNIQUE pick ID
    // (kAttaPickBase + entry) and is recorded in atta_pick_entries_ with its world
    // transform, so the app can promote it into an editable task on click. ATTAs
    // already promoted/duplicated by an EditRigidObj are skipped.
    void DrawAttachmentsForPicking(const std::string& parentModelId, bool isBuilding,
                                   const glm::mat4& parentWorldMat, float parentScale,
                                   GLint loc_model, GLint loc_id, int parentObjIndex,
                                   std::unordered_set<std::string>& drawn,
                                   int selected_object_index);
    // True if this ATTA has been promoted/duplicated (live occupancy OR persistent).
    bool IsAttaPromoted(const std::string& modelId, const glm::vec3& worldPos) const {
        const std::string k = AttaOccupancyKey(modelId, worldPos);
        return editrigid_occupancy_.count(k) > 0 || suppressed_atta_keys_.count(k) > 0;
    }
    static bool IsVehicleType(const std::string& type);
    void EnsurePortalDistancesLoaded();
    void EnsureWindowModelIdsLoaded();
    void EnsureAiModelIdsLoaded();
    void EnsureDeathZoneIdsLoaded();
    void EnsureMagicObjIdsLoaded();
    void InitSphereMesh();
    void DrawMagicObjSpheres(const std::vector<LevelObject>& objects, GLuint ubo_mats);
    void DrawSelectionBox(const LevelObject& obj, GLuint ubo_mats, const glm::vec4& color);
    Mesh CreateCubeMesh();
    Mesh CreateTextMesh(const std::string& text);
    void AddCharacterVertices(std::vector<float>& vertices, char c, float x, float y, float scale);
    std::string FindModelFile(const std::string& modelId, bool isBuilding);
    std::string FindTextureFile(const std::string& textureId) const;
    std::string FindTextureFileInLevel(const std::string& textureId, int levelNo) const;
    std::string GetLevelTexturesPath() const;
    std::string GetLevelTextureDatPath() const;
    void LoadDatIntoMap(const std::string& datPath, std::map<std::string, std::vector<std::string>>& outMap);
    void EnsureTextureMapLoaded();
    void EnsureGlobalTextureMapLoaded() const;
    std::vector<std::string> GetTextureIdsForModel(const std::string& modelId);
    std::vector<std::string> GetTextureIdsForSourceModel(const std::string& modelId, int sourceLevel);
    GLuint GetOrLoadTexture(const std::string& textureId);
    void ApplyTexturesToMesh(Mesh& mesh, const std::string& modelId, const std::string& parentModelId = "");
    void InitSelectionBox();
    void InitPickingFBO(int w, int h);
    void DrawForPicking(GLuint ubo_mats,
                        const std::vector<LevelObject>& objects,
                        int draw_parts,
                        const glm::vec3& camera_pos,
                        int selected_object_index);
};
