#include "pch.h"
#include "debug_command_manager.h"
#include "debug_command_parser.h"
#include "app.h"
#include "logger.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/tinygltf/stb_image_write.h"
#include "renderer/renderer.h"
#include "capture_camera.h"
#include "visual_integrity.h"
#include "level/level.h"
#include "level/level_objects.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <chrono>
#include <direct.h>
#include <cstdio>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

DebugCommandManager::DebugCommandManager(App* app) 
    : app_(app), running_(false), commands_file_path_("editor/tools/debug-command.txt") {
}

DebugCommandManager::~DebugCommandManager() {
    Stop();
}

void DebugCommandManager::Start() {
    if (running_) return;
    running_ = true;
    watcher_thread_ = std::thread(&DebugCommandManager::WatcherThread, this);
}

void DebugCommandManager::Stop() {
    running_ = false;
    if (watcher_thread_.joinable()) {
        watcher_thread_.join();
    }
}

void DebugCommandManager::WatcherThread() {
    while (running_) {
        std::ifstream file(commands_file_path_);
        if (file.is_open()) {
            std::string line;
            std::vector<std::string> lines;
            bool has_commands = false;
            while (std::getline(file, line)) {
                lines.push_back(line);
                if (line.empty()) continue;
                
                if (const auto command = ParseDebugCommand(line)) {
                    command_queue_.Push(*command);
                    has_commands = true;
                }
            }
            file.close();

            if (has_commands) {
                // Clear file
                std::ofstream out_file(commands_file_path_, std::ios::trunc);
                out_file.close();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

void DebugCommandManager::Update() {
    DebugCommand command;
    while (command_queue_.TryPop(command)) ProcessCommand(command);
}

void DebugCommandManager::ProcessCommand(const DebugCommand& cmd) {
    if (cmd.level != -1 && cmd.level != app_->GetCurLevelNo()) {
        app_->LoadLevel(cmd.level);
    }
    
    if (cmd.type == "goto") {
        GotoModel(cmd);
    } else if (cmd.type == "set-camera") {
        SetCamera(cmd);
    } else if (cmd.type == "capture-model") {
        CaptureModel(cmd);
    } else if (cmd.type == "capture-spline") {
        CaptureSplineTrace(cmd);
    } else if (cmd.type == "delete") {
        DeleteModel(cmd);
    } else if (cmd.type == "wireframe") {
        if (cmd.val) {
            if (!app_->GetOverlayWireframe()) app_->ToggleOverlayWireframe();
        } else {
            if (app_->GetOverlayWireframe()) app_->ToggleOverlayWireframe();
        }
    } else if (cmd.type == "draw-parts") {
        app_->SetDrawParts(cmd.val);
    } else if (cmd.type == "reset-level") {
        app_->ResetLevel();
    }
}

void DebugCommandManager::GotoModel(const DebugCommand& cmd) {
    auto& objects = app_->level_.GetLevelObjects().GetObjects();
    
    double cx = cmd.x, cy = cmd.y, cz = cmd.z;
    if (std::abs(cx) < 1000000.0 && std::abs(cy) < 1000000.0 && std::abs(cz) < 1000000.0) {
        cx *= 256.0; cy *= 256.0; cz *= 256.0; // Assume meters, convert to engine units
    }

    int best_idx = -1;
    double min_dist = 1e30;

    for (size_t i = 0; i < objects.size(); ++i) {
        if (!objects[i].deleted && (objects[i].modelId == cmd.modelId || objects[i].segmentModelId == cmd.modelId)) {
            if (!cmd.has_pos) {
                best_idx = (int)i;
                break; // If no pos specified, pick first
            }
            double dx = objects[i].pos.x - cx;
            double dy = objects[i].pos.y - cy;
            double dz = objects[i].pos.z - cz;
            double dist_sq = dx*dx + dy*dy + dz*dz;
            if (dist_sq < min_dist) {
                min_dist = dist_sq;
                best_idx = (int)i;
            }
        }
    }

    if (best_idx != -1) {
        app_->viewer_.pos_ = glm::vec3(objects[best_idx].pos);
        app_->viewer_.yaw_ = -objects[best_idx].rot.z;
        app_->viewer_.pitch_ = 0;
        app_->viewer_.roll_ = 0;
        
        app_->UpdateViewerVectors();
        app_->selected_object_index_ = best_idx;
    }
}

void DebugCommandManager::DeleteModel(const DebugCommand& cmd) {
    auto& objects = app_->level_.GetLevelObjects().GetObjects();
    
    double cx = cmd.x, cy = cmd.y, cz = cmd.z;
    if (std::abs(cx) < 1000000.0 && std::abs(cy) < 1000000.0 && std::abs(cz) < 1000000.0) {
        cx *= 256.0; cy *= 256.0; cz *= 256.0; // Assume meters, convert to engine units
    }

    int best_idx = -1;
    double min_dist = 1e30;

    for (size_t i = 0; i < objects.size(); ++i) {
        if (!objects[i].deleted && (objects[i].modelId == cmd.modelId || objects[i].segmentModelId == cmd.modelId)) {
            if (!cmd.has_pos) {
                best_idx = (int)i;
                break; // If no pos specified, pick first
            }
            double dx = objects[i].pos.x - cx;
            double dy = objects[i].pos.y - cy;
            double dz = objects[i].pos.z - cz;
            double dist_sq = dx*dx + dy*dy + dz*dz;
            if (dist_sq < min_dist) {
                min_dist = dist_sq;
                best_idx = (int)i;
            }
        }
    }

    if (best_idx != -1) {
        objects[best_idx].deleted = true;
        objects[best_idx].modified = true;
        if (app_->selected_object_index_ == best_idx) {
            app_->selected_object_index_ = -1;
        }
        Logger::Get().Log(LogLevel::INFO, "[App] Deleted model via developer command: " + cmd.modelId);
    }
}


static std::string FindFFmpegBin() {
    static const char* kCandidates[] = {
        "D:\\henv\\Lib\\site-packages\\imageio_ffmpeg\\binaries\\ffmpeg-win-x86_64-v7.1.exe",
        "ffmpeg.exe",
        nullptr
    };
    for (int i = 0; kCandidates[i]; ++i) {
        DWORD attr = GetFileAttributesA(kCandidates[i]);
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            char shortPath[MAX_PATH];
            if (GetShortPathNameA(kCandidates[i], shortPath, MAX_PATH) > 0)
                return std::string(shortPath);
            return std::string(kCandidates[i]);
        }
    }
    return "ffmpeg.exe";
}

static std::string JsonStr(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        if (c == '"')       out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else                out += c;
    }
    out += '"';
    return out;
}

static bool WriteImageBGRA(const char* bmpPath, const char* pngPath,
                            const unsigned char* bgra, int w, int h) {
    // Convert BGRA (bottom-up) to RGB (top-down)
    std::vector<unsigned char> rgb(w * h * 3);
    for (int y = 0; y < h; ++y) {
        const unsigned char* src = bgra + (size_t)(h - 1 - y) * w * 4;
        unsigned char*       dst = rgb.data() + (size_t)y * w * 3;
        for (int x = 0; x < w; ++x, src += 4, dst += 3) {
            dst[0] = src[2]; // R
            dst[1] = src[1]; // G
            dst[2] = src[0]; // B
        }
    }
    bool ok = true;
    if (bmpPath) ok &= (stbi_write_bmp(bmpPath, w, h, 3, rgb.data()) != 0);
    if (pngPath) ok &= (stbi_write_png(pngPath, w, h, 3, rgb.data(), w * 3) != 0);
    return ok;
}

static bool WriteDiagnosticMask(const char* pngPath, const std::vector<unsigned char>& values,
                                int w, int h, bool encode_part_id) {
    if (values.size() != static_cast<size_t>(w) * h) return false;
    std::vector<unsigned char> rgb(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t src = static_cast<size_t>(h - 1 - y) * w + x;
            const size_t dst = (static_cast<size_t>(y) * w + x) * 3;
            const unsigned char value = values[src];
            rgb[dst] = encode_part_id ? value : (value ? 255 : 0);
            rgb[dst + 1] = encode_part_id ? static_cast<unsigned char>(value * 37) : (value ? 255 : 0);
            rgb[dst + 2] = encode_part_id ? static_cast<unsigned char>(value * 97) : 0;
        }
    }
    return stbi_write_png(pngPath, w, h, 3, rgb.data(), w * 3) != 0;
}

void DebugCommandManager::CaptureSplineTrace(const DebugCommand& cmd) {
    app_->renderer_.RequestSplineTrace(cmd.path);
    Logger::Get().Log(LogLevel::INFO, "[Debug] Spline trace requested: " + cmd.path);
}

void DebugCommandManager::SetCamera(const DebugCommand& cmd) {
    double x = cmd.x;
    double y = cmd.y;
    double z = cmd.z;
    if (std::abs(x) < 1000000.0 && std::abs(y) < 1000000.0 && std::abs(z) < 1000000.0) {
        x *= 256.0;
        y *= 256.0;
        z *= 256.0;
    }

    app_->viewer_.pos_ = glm::vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
    app_->viewer_.yaw_ = static_cast<float>(cmd.yaw);
    app_->viewer_.pitch_ = static_cast<float>(cmd.pitch);
    app_->viewer_.roll_ = 0.0f;
    app_->UpdateViewerVectors();
    Logger::Get().Log(LogLevel::INFO, "[Debug] Camera set by developer command");
}

static bool WriteDiagnosticPartMask(const char* pngPath, const std::vector<int>& values,
                                    int w, int h) {
    if (values.size() != static_cast<size_t>(w) * h) return false;
    std::vector<unsigned char> rgb(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t src = static_cast<size_t>(h - 1 - y) * w + x;
            const size_t dst = (static_cast<size_t>(y) * w + x) * 3;
            const unsigned int id = values[src] > 0 ? static_cast<unsigned int>(values[src]) : 0u;
            rgb[dst] = static_cast<unsigned char>(id & 0xffu);
            rgb[dst + 1] = static_cast<unsigned char>((id >> 8) & 0xffu);
            rgb[dst + 2] = static_cast<unsigned char>((id >> 16) & 0xffu);
        }
    }
    return stbi_write_png(pngPath, w, h, 3, rgb.data(), w * 3) != 0;
}

static bool WriteDiagnosticOverlay(const char* pngPath, const unsigned char* bgra,
                                   const std::vector<uint8_t>& target_mask,
                                   const std::vector<int>& part_ids, int w, int h) {
    if (!bgra || target_mask.size() != static_cast<size_t>(w) * h ||
        part_ids.size() != target_mask.size()) return false;
    std::vector<unsigned char> rgb(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t src = static_cast<size_t>(h - 1 - y) * w + x;
            const size_t dst = (static_cast<size_t>(y) * w + x) * 3;
            const unsigned char* color = bgra + src * 4;
            if (target_mask[src] == 0) {
                rgb[dst] = color[2] / 3;
                rgb[dst + 1] = color[1] / 3;
                rgb[dst + 2] = color[0] / 3;
                continue;
            }
            const unsigned char part = static_cast<unsigned char>(part_ids[src] & 0xFF);
            rgb[dst] = static_cast<unsigned char>((static_cast<int>(color[2]) + 255) / 2);
            rgb[dst + 1] = static_cast<unsigned char>((static_cast<int>(color[1]) + part * 37) / 2);
            rgb[dst + 2] = static_cast<unsigned char>((static_cast<int>(color[0]) + part * 97) / 2);
        }
    }
    return stbi_write_png(pngPath, w, h, 3, rgb.data(), w * 3) != 0;
}

static bool WriteDiagnosticDepth(const char* path, const std::vector<float>& depth) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) return false;
    output.write(reinterpret_cast<const char*>(depth.data()),
                 static_cast<std::streamsize>(depth.size() * sizeof(float)));
    return output.good();
}
void DebugCommandManager::CaptureModel(const DebugCommand& cmd) {
    auto& objects = app_->level_.GetLevelObjects().GetObjects();

    // A model capture is an authored-asset inspection, not a gameplay view.
    // Building ATTA visibility is normally gated by the level's portal distance;
    // that gate can be much smaller than the orbit needed to frame the complete
    // model (for example, a building may author a one-metre portal distance).
    // Disable only the renderer's distance gate for this scoped capture and
    // restore the user's setting on every exit path.
    struct ScopedCaptureLod {
        bool enabled;
        ScopedCaptureLod() : enabled(Config::Get().enableLOD) {
            Config::Get().enableLOD = false;
        }
        ~ScopedCaptureLod() {
            Config::Get().enableLOD = enabled;
        }
    } scopedCaptureLod;

    double cx = cmd.x, cy = cmd.y, cz = cmd.z;
    if (std::abs(cx) < 1000000.0 && std::abs(cy) < 1000000.0 && std::abs(cz) < 1000000.0)
        cx *= 256.0, cy *= 256.0, cz *= 256.0;

    int target_idx = -1;
    double min_dist = 1e30;
    for (size_t i = 0; i < objects.size(); ++i) {
        if (!objects[i].deleted &&
            (objects[i].modelId == cmd.modelId || objects[i].segmentModelId == cmd.modelId)) {
            if (!cmd.has_pos) { target_idx = (int)i; break; }
            double dx = objects[i].pos.x - cx,
                   dy = objects[i].pos.y - cy,
                   dz = objects[i].pos.z - cz;
            double d2 = dx*dx + dy*dy + dz*dz;
            if (d2 < min_dist) { min_dist = d2; target_idx = (int)i; }
        }
    }
    if (target_idx == -1) {
        Logger::Get().Log(LogLevel::WARNING, "[CaptureModel] Model not found: " + cmd.modelId);
        return;
    }

    auto& obj = objects[target_idx];
    app_->selected_object_index_ = target_idx;
    Logger::Get().Log(LogLevel::INFO, "[CaptureModel] target=" + obj.modelId +
        " taskId=" + obj.taskId +
        " lightmap=" + std::to_string(app_->renderer_.HasLightmapForTask(LightmapTaskKey(obj))));

    // Viewport
    const int W = app_->window_state_.viewport_width_;
    const int H = app_->window_state_.viewport_height_;
    const int bgraBytes = W * H * 4;

    // ── Category & Framing Calculation ──────────────────────────────────────
    const bool isAI = (obj.type.rfind("Human", 0) == 0) || (obj.type == "AI") || (obj.type.find("Soldier") != std::string::npos);
    const bool isVehicle = (obj.type == "Car" || obj.type == "Heli" || obj.type == "Train" || obj.type == "Plane" || obj.type == "CarAI");
    const bool isBuilding = obj.isBuilding || (obj.type == "Building");

    // Query mesh extents (in MEF units, 1 MEF unit = 40.96 engine units)
    glm::vec3 meshExt = app_->renderer_.GetMeshExtents(obj.modelId, obj.isBuilding);
    const float scale = (obj.scale > 0.0f) ? obj.scale : 1.0f;
    const float kMefToEngine = 40.96f * scale;

    glm::vec3 extEngine = meshExt * kMefToEngine;
    float boundRadius = glm::length(extEngine);

    // Compute true 3D center offset rotated by object's orientation
    glm::vec3 meshCenter = app_->renderer_.GetMeshCenter(obj.modelId, obj.isBuilding);
    glm::mat4 rotMat(1.0f);
    rotMat = glm::rotate(rotMat, (float)obj.rot.z, glm::vec3(0.0f, 0.0f, 1.0f));
    rotMat = glm::rotate(rotMat, (float)obj.rot.x, glm::vec3(1.0f, 0.0f, 0.0f));
    rotMat = glm::rotate(rotMat, (float)obj.rot.y, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::vec3 centerOffset = glm::vec3(rotMat * glm::vec4(meshCenter * kMefToEngine, 1.0f));

    double targetX = obj.pos.x + centerOffset.x;
    double targetY = obj.pos.y + centerOffset.y;
    double targetZ = obj.pos.z + centerOffset.z;

    double kOrbitRadius    = 400.0;
    double kExteriorHeight = 150.0;
    double kInteriorHeight = 100.0;

    if (isAI) {
        // AI characters: boundRadius ~ 4345
        // User requested: "very close camera man fix it move far away now little far fix this man listen this this one"
        // Move camera back to ~7000 units so full body from head to boots is cleanly framed with generous margins.
        kOrbitRadius    = std::clamp((double)boundRadius * 1.65, 7000.0, 7500.0);
        kExteriorHeight = kOrbitRadius * 0.20;
        kInteriorHeight = targetZ;
    } else if (isVehicle) {
        // Vehicles: cars, trucks, APCs, helicopters, planes
        kOrbitRadius    = std::clamp((double)boundRadius * 1.35, 6000.0, 25000.0);
        targetZ        += std::max((double)extEngine.z * 0.4, 80.0);
        kExteriorHeight = kOrbitRadius * 0.30;
        kInteriorHeight = targetZ;
    } else if (isBuilding) {
        // Buildings: water towers, hangars, offices, bunkers
        // "for buildings are too much it can be far away but not that far OK"
        kOrbitRadius    = igi::ResolveBuildingCaptureOrbitRadius(boundRadius);
        targetZ        += std::max((double)extEngine.z * 0.45, 250.0);
        kExteriorHeight = kOrbitRadius * 0.30;
        kInteriorHeight = std::clamp((double)extEngine.z * 0.5, 800.0, 5000.0);
    } else {
        // Rigid objects: chair, table, desk, barrel, computer, phone, alarm switch, pickups
        // Scale proportionally to boundRadius so large weapons (Dragunov, boundRadius 2700)
        // and small props are both framed perfectly without clipping
        kOrbitRadius    = std::clamp((double)boundRadius * 1.40, 450.0, 8000.0);
        targetZ        += std::max((double)extEngine.z * 0.4, 20.0);
        kExteriorHeight = kOrbitRadius * 0.28;
        kInteriorHeight = targetZ;
    }

    const float extPitchDeg = -glm::degrees(static_cast<float>(
        std::atan2(kExteriorHeight, kOrbitRadius)));

    const float detailRadius = isBuilding
        ? (float)kOrbitRadius
        : isAI
            ? (float)kOrbitRadius * 0.85f
            : std::max((float)kOrbitRadius * 0.65f, 300.0f);
    const float exteriorDistance = std::sqrt(
        (float)(kOrbitRadius * kOrbitRadius +
                kExteriorHeight * kExteriorHeight));
    const float detailDistance = isBuilding
        ? exteriorDistance
        : std::sqrt(detailRadius * detailRadius +
                    (float)(kExteriorHeight * 0.25 * kExteriorHeight * 0.25));
    const float captureNearPlane = igi::ResolveCaptureNearPlane(
        std::min(exteriorDistance, detailDistance), RENDER_Z_NEAR);
    struct ScopedCaptureNearPlane {
        float original = RENDER_Z_NEAR;
        explicit ScopedCaptureNearPlane(float replacement) { RENDER_Z_NEAR = replacement; }
        ~ScopedCaptureNearPlane() { RENDER_Z_NEAR = original; }
    } scopedCaptureNearPlane(captureNearPlane);
    struct ScopedCaptureDrawParts {
        App* app;
        int original;
        ScopedCaptureDrawParts(App* target, int original_parts)
            : app(target), original(original_parts) {}
        ~ScopedCaptureDrawParts() { app->SetDrawParts(original); }
    } scopedCaptureDrawParts(app_, app_->GetDrawParts());

    // Clear terrain so hills/ground do not clip through models
    int captureDrawParts = scopedCaptureDrawParts.original & ~Renderer::DRAW_TERRAIN;
    // For props, AI, and vehicles: hide enclosing buildings so indoor objects are never occluded by outside walls
    if (!isBuilding) {
        captureDrawParts &= ~Renderer::DRAW_BUILDINGS;
    }
    app_->SetDrawParts(captureDrawParts);

    Logger::Get().Log(LogLevel::INFO, "[CaptureModel] Camera framing: radius=" +
        std::to_string(kOrbitRadius) + " height=" + std::to_string(kExteriorHeight) +
        " pitch=" + std::to_string(extPitchDeg) + " targetZ=" + std::to_string(targetZ) +
        " boundRadius=" + std::to_string(boundRadius) + " near=" + std::to_string(captureNearPlane));

    // Output paths
    _mkdir("screenshots");
    char donePath[256], evidPath[256];
    snprintf(donePath, sizeof(donePath),  "screenshots/Level%02d_Model%s_Done.txt",
             cmd.level, cmd.modelId.c_str());
    snprintf(evidPath, sizeof(evidPath),  "screenshots/Level%02d_Model%s_evidence.jsonl",
             cmd.level, cmd.modelId.c_str());
    _unlink(donePath);
    _unlink(evidPath);

    // Evidence JSONL (one record per captured view)
    std::ofstream evFile(evidPath, std::ios::out | std::ios::trunc);
    igi::VisualIntegrityInput visualInput;
    visualInput.minimumPartPixels = 1;
    // The shared baseline permits ordinary self-occlusion while rejecting
    // captures where most expected geometry never produces target fragments.
    visualInput.minimumObservedPartRatio = 0.75f;
    // Projected part IDs distinguish authored openings from missing fragments,
    // so the silhouette rule can safely examine target-mask holes.
    visualInput.minimumInteriorHolePixels = 16;
    visualInput.requireTemporalEvidence = cmd.orbit_frames > 0;
    std::vector<std::string> visualMaskPaths;
    std::vector<std::string> visualPartPaths;
    std::vector<std::string> visualDepthPaths;
    std::vector<std::string> visualOverlayPaths;
    std::vector<Renderer_Objects::VisualEvidence::PartInventory> visualExpectedParts;

    // Camera setter
    auto set_camera = [&](float camX, float camY, float camZ, float yawDeg, float pitchDeg) {
        app_->viewer_.pos_   = glm::vec3(camX, camY, camZ);
        app_->viewer_.yaw_   = yawDeg;
        app_->viewer_.pitch_ = pitchDeg;
        app_->UpdateViewerVectors();
        app_->UpdateViewDefine();
    };

    // Sync still capture: double-render -> glReadPixels -> BMP + PNG + evidence
    std::vector<unsigned char> bgra(bgraBytes);
    std::vector<float> sceneDepth(static_cast<size_t>(W) * H);
    auto capture_still = [&](const char* suffix,
                              float camX, float camY, float camZ,
                              float yawDeg, float pitchDeg,
                              int temporalGroup = -1) {
        set_camera(camX, camY, camZ, yawDeg, pitchDeg);
        app_->OnDisplay(); // fill back-buffer
        app_->OnDisplay(); // present to front
        glReadBuffer(GL_FRONT);
        glReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, bgra.data());
        glReadPixels(0, 0, W, H, GL_DEPTH_COMPONENT, GL_FLOAT, sceneDepth.data());
        glReadBuffer(GL_BACK);

        int targetIdPixels = 0;
        int visiblePixels = app_->renderer_.CountObjectVisiblePixels(
            app_->view_define_, objects, captureDrawParts,
            app_->selected_object_index_, target_idx, sceneDepth, &targetIdPixels);

        // If occluded by a prop/crate in front of camera, step forward towards target
        float actualCamX = camX, actualCamY = camY, actualCamZ = camZ;
        if (visiblePixels == 0 && !isBuilding) {
            for (float stepFraction : { 0.75f, 0.55f, 0.40f }) {
                float testX = (float)targetX + (camX - (float)targetX) * stepFraction;
                float testY = (float)targetY + (camY - (float)targetY) * stepFraction;
                float testZ = (float)targetZ + (camZ - (float)targetZ) * stepFraction;
                set_camera(testX, testY, testZ, yawDeg, pitchDeg);
                app_->OnDisplay();
                app_->OnDisplay();
                glReadBuffer(GL_FRONT);
                glReadPixels(0, 0, W, H, GL_DEPTH_COMPONENT, GL_FLOAT, sceneDepth.data());
                glReadBuffer(GL_BACK);
                int testIdPixels = 0;
                int testVisible = app_->renderer_.CountObjectVisiblePixels(
                    app_->view_define_, objects, captureDrawParts,
                    app_->selected_object_index_, target_idx, sceneDepth, &testIdPixels);
                if (testVisible > 0) {
                    actualCamX = testX; actualCamY = testY; actualCamZ = testZ;
                    visiblePixels = testVisible;
                    targetIdPixels = testIdPixels;
                    glReadBuffer(GL_FRONT);
                    glReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, bgra.data());
                    glReadBuffer(GL_BACK);
                    break;
                }
            }
        }

        const double totalPixels = static_cast<double>(W) * H;
        const double targetCoverage = (totalPixels > 0.0)
            ? (static_cast<double>(visiblePixels) / totalPixels)
            : 0.0;
        const bool targetVisible = (visiblePixels > 0);

        char bmpPath[256], pngPath[256];
        snprintf(bmpPath, sizeof(bmpPath), "screenshots/Level%02d_Model%s_%s.bmp",
                 cmd.level, cmd.modelId.c_str(), suffix);
        snprintf(pngPath, sizeof(pngPath), "screenshots/Level%02d_Model%s_%s.png",
                 cmd.level, cmd.modelId.c_str(), suffix);
        WriteImageBGRA(bmpPath, pngPath, bgra.data(), W, H);

        char maskPath[256], partPath[256], depthPath[256], overlayPath[256];
        snprintf(maskPath, sizeof(maskPath), "screenshots/Level%02d_Model%s_%s.object-id.png",
                 cmd.level, cmd.modelId.c_str(), suffix);
        snprintf(partPath, sizeof(partPath), "screenshots/Level%02d_Model%s_%s.material-id.png",
                 cmd.level, cmd.modelId.c_str(), suffix);
        snprintf(depthPath, sizeof(depthPath), "screenshots/Level%02d_Model%s_%s.depth.bin",
                 cmd.level, cmd.modelId.c_str(), suffix);
        snprintf(overlayPath, sizeof(overlayPath), "screenshots/Level%02d_Model%s_%s-diagnostic.png",
                 cmd.level, cmd.modelId.c_str(), suffix);

        const auto visualEvidence = app_->renderer_.CaptureObjectVisualEvidence(
            app_->view_define_, objects, target_idx, sceneDepth);
        if (visualEvidence.width == W && visualEvidence.height == H) {
            igi::VisualIntegrityView visualView;
            visualView.width = W;
            visualView.height = H;
            visualView.targetMask = visualEvidence.targetMask;
            visualView.partIds = visualEvidence.partIds;
            visualView.sceneDepth = sceneDepth;
            visualView.targetDepth = visualEvidence.targetDepth;
            // The visual-ID pass uses static VAO data. Do not use its
            // projection to judge a frame whose visible object was replaced
            // by the live CPU-skinned draw.
            visualView.geometryProjectionMatchesRenderedFrame =
                app_->GetSkinnedReplacementObjectIndices(false).count(target_idx) == 0;
            visualView.renderedRgb.resize(static_cast<size_t>(W) * H * 3);
            for (size_t pixel = 0; pixel < static_cast<size_t>(W) * H; ++pixel) {
                visualView.renderedRgb[pixel * 3] = bgra[pixel * 4 + 2];
                visualView.renderedRgb[pixel * 3 + 1] = bgra[pixel * 4 + 1];
                visualView.renderedRgb[pixel * 3 + 2] = bgra[pixel * 4];
            }
            visualView.name = suffix;
            visualView.sourceFramePath = pngPath;
            visualView.overlayPath = overlayPath;
            visualView.temporalGroup = temporalGroup;
            visualInput.views.push_back(std::move(visualView));
            if (visualInput.expectedPartIds.empty()) {
                visualInput.expectedPartIds = visualEvidence.expectedPartIds;
                visualInput.strictPartIds = visualEvidence.strictPartIds;
                for (const auto& part : visualEvidence.expectedParts) {
                    visualInput.expectedParts.push_back({part.id, part.vertexCount,
                        part.triangleCount, part.materialSlot, part.alphaMode,
                        part.textureIdentity, part.textureResolved,
                        part.textureChromaticPixelRatio,
                        part.localBoundsMin, part.localBoundsMax});
                }
                visualExpectedParts = visualEvidence.expectedParts;
            }

            WriteDiagnosticMask(maskPath, visualEvidence.targetMask, W, H, false);
            WriteDiagnosticPartMask(partPath, visualEvidence.partIds, W, H);
            WriteDiagnosticDepth(depthPath, visualEvidence.targetDepth);
            WriteDiagnosticOverlay(overlayPath, bgra.data(), visualEvidence.targetMask,
                                   visualEvidence.partIds, W, H);
            visualMaskPaths.emplace_back(maskPath);
            visualPartPaths.emplace_back(partPath);
            visualDepthPaths.emplace_back(depthPath);
            visualOverlayPaths.emplace_back(overlayPath);
        }

        if (evFile.is_open()) {
            evFile << "{\"level\":" << cmd.level
                   << ",\"modelId\":" << JsonStr(cmd.modelId)
                   << ",\"taskId\":"  << JsonStr(cmd.taskId.empty() ? obj.taskId : cmd.taskId)
                   << ",\"view\":"    << JsonStr(std::string(suffix))
                   << ",\"camera\":{\"x\":" << actualCamX << ",\"y\":" << actualCamY
                   << ",\"z\":" << actualCamZ << ",\"yaw\":" << yawDeg
                   << ",\"pitch\":" << pitchDeg << "}"
                   << ",\"bmp\":" << JsonStr(std::string(bmpPath))
                   << ",\"png\":" << JsonStr(std::string(pngPath))
                   << ",\"source\":\"rendered-framebuffer\""
                   << ",\"rendered\":true"
                   << ",\"targetVisible\":" << (targetVisible ? "true" : "false")
                   << ",\"targetPixels\":" << visiblePixels
                   << ",\"targetIdPixels\":" << targetIdPixels
                   << ",\"targetCoverage\":" << targetCoverage
                   << ",\"visualObjectMask\":" << JsonStr(visualMaskPaths.empty() ? "" : visualMaskPaths.back())
                   << ",\"visualMaterialMask\":" << JsonStr(visualPartPaths.empty() ? "" : visualPartPaths.back())
                   << ",\"visualDepth\":" << JsonStr(visualDepthPaths.empty() ? "" : visualDepthPaths.back())
                   << ",\"visualOverlay\":" << JsonStr(visualOverlayPaths.empty() ? "" : visualOverlayPaths.back())
                   << "}\n";
        }
    };

    // 12 exterior shots (30 degrees apart) give flat or occluded models enough
    // camera directions for the runner to retain clear, GPU-proven views.
    for (int angle = 0; angle < 360; angle += 30) {
        const float rad = glm::radians((float)angle);
        const float camX = (float)targetX - std::sin(rad) * (float)kOrbitRadius;
        const float camY = (float)targetY - std::cos(rad) * (float)kOrbitRadius;
        const float camZ = (float)targetZ + (float)kExteriorHeight;
        char suffix[32]; snprintf(suffix, sizeof(suffix), "Ext_%03d", angle);
        capture_still(suffix, camX, camY, camZ, (float)((360 - angle) % 360), extPitchDeg);
    }

    // Sample the initial and completed orbit pose with the same diagnostic
    // buffers used by still capture.  These frames bracket the encoded orbit
    // video and provide deterministic same-camera temporal evidence.
    if (cmd.orbit_frames > 0) {
        capture_still("Orbit_000", (float)targetX, (float)targetY - (float)kOrbitRadius,
                      (float)targetZ + (float)kExteriorHeight, 0.0f, extPitchDeg, 0);
        capture_still("Orbit_360", (float)targetX, (float)targetY - (float)kOrbitRadius,
                      (float)targetZ + (float)kExteriorHeight, 0.0f, extPitchDeg, 0);
    }

    // 4 interior / detail shots (90 degrees apart)
    for (int angle = 0; angle < 360; angle += 90) {
        char suffix[32]; snprintf(suffix, sizeof(suffix), "Int_%03d", angle);
        if (isBuilding) {
            // Inside the building looking outwards
            capture_still(suffix,
                (float)targetX, (float)targetY, (float)(obj.pos.z + kInteriorHeight),
                (float)((360 - angle) % 360), 0.0f);
        } else {
            // Close-up detail inspection view from 4 cardinal directions
            const float rad = glm::radians((float)angle);
            const float closeCamX = (float)targetX - std::sin(rad) * detailRadius;
            const float closeCamY = (float)targetY - std::cos(rad) * detailRadius;
            const float closeCamZ = (float)targetZ + (float)(kExteriorHeight * 0.25);
            const float closePitch = -glm::degrees(static_cast<float>(
                std::atan2(kExteriorHeight * 0.25, detailRadius)));
            capture_still(suffix, closeCamX, closeCamY, closeCamZ, (float)((360 - angle) % 360), closePitch);
        }
    }

    // Orbit video: PBO ping-pong -> FFmpeg stdin pipe
    bool orbitOk = false;
    std::string encError;
    if (cmd.orbit_frames > 0) {
        const int videoFps = (cmd.video_fps > 0) ? cmd.video_fps : 12;
        char videoPath[256], sidecarPath[256];
        snprintf(videoPath,   sizeof(videoPath),   "screenshots/Level%02d_Model%s_orbit.mp4",
                 cmd.level, cmd.modelId.c_str());
        snprintf(sidecarPath, sizeof(sidecarPath), "screenshots/Level%02d_Model%s_orbit.json",
                 cmd.level, cmd.modelId.c_str());

        std::string ffmpegBin = FindFFmpegBin();
        // Pipe: raw BGRA (bottom-up from OpenGL) -> vflip -> libx264/yuv420p MP4
        char pipeCmd[1024];
        snprintf(pipeCmd, sizeof(pipeCmd),
            "%s -y -f rawvideo -pix_fmt bgra -s %dx%d -r %d -i - "
            "-vf vflip "
            "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -crf 20 -movflags +faststart "
            "%s 2>screenshots/ffmpeg_err.log",
            ffmpegBin.c_str(), W, H, videoFps, videoPath);

        Logger::Get().Log(LogLevel::INFO, "[CaptureModel] Opening FFmpeg stdin pipe: " + std::string(pipeCmd));
        FILE* pipe = _popen(pipeCmd, "wb");

        if (!pipe) {
            encError = "_popen failed";
            Logger::Get().Log(LogLevel::WARNING, "[CaptureModel] " + encError);
        } else {
            // PBO ping-pong setup
            GLuint pbos[2] = {0, 0};
            glGenBuffers(2, pbos);
            for (int i = 0; i < 2; ++i) {
                glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[i]);
                glBufferData(GL_PIXEL_PACK_BUFFER, bgraBytes, nullptr, GL_STREAM_READ);
            }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

            int curPbo = 0;

            // Warm-up render to seed PBO[0] before the loop
            {
                const float camX = (float)targetX - 0.0f * (float)kOrbitRadius;
                const float camY = (float)targetY - 1.0f * (float)kOrbitRadius;
                const float camZ = (float)targetZ + (float)kExteriorHeight;
                set_camera(camX, camY, camZ, 0.0f, extPitchDeg);
                app_->OnDisplay();
            }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[curPbo]);
            glReadBuffer(GL_FRONT);
            glReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, 0); // async DMA start
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

            for (int frame = 0; frame < cmd.orbit_frames; ++frame) {
                const float angle_deg = (360.0f * (float)frame) / (float)cmd.orbit_frames;
                const float rad = glm::radians(angle_deg);
                set_camera(
                    (float)targetX - std::sin(rad) * (float)kOrbitRadius,
                    (float)targetY - std::cos(rad) * (float)kOrbitRadius,
                    (float)targetZ + (float)kExteriorHeight,
                    std::fmod(360.0f - angle_deg, 360.0f), extPitchDeg);
                app_->OnDisplay(); // render next frame

                // Kick async readback for THIS frame into next PBO
                int nextPbo = curPbo ^ 1;
                glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[nextPbo]);
                glReadBuffer(GL_FRONT);
                glReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, 0);
                glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

                // Map PREVIOUS PBO (overlaps with current render) -> pipe to FFmpeg
                glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[curPbo]);
                void* ptr = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
                if (ptr) {
                    fwrite(ptr, 1, bgraBytes, pipe);
                    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                }
                glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                curPbo = nextPbo;
            }

            // Flush last PBO (frame cmd.orbit_frames-1's readback)
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[curPbo]);
            void* ptr = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
            if (ptr) {
                fwrite(ptr, 1, bgraBytes, pipe);
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

            glDeleteBuffers(2, pbos);

            int exitCode = _pclose(pipe); // blocks until FFmpeg finishes encoding
            DWORD attr = GetFileAttributesA(videoPath);
            orbitOk = (exitCode == 0) ||
                      (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
            Logger::Get().Log(LogLevel::INFO, "[CaptureModel] FFmpeg exit=" +
                std::to_string(exitCode) + (orbitOk ? " MP4 ok" : " MP4 missing"));
        }

        // Sidecar JSON
        std::ofstream sidecar(sidecarPath);
        sidecar << "{\"file\":" << JsonStr(std::string(videoPath))
                << ",\"frames\":" << cmd.orbit_frames
                << ",\"fps\":" << ((cmd.video_fps > 0) ? cmd.video_fps : 12)
                << ",\"durationSeconds\":" <<
                   ((double)cmd.orbit_frames / ((cmd.video_fps > 0) ? cmd.video_fps : 12))
                << ",\"source\":\"rendered-framebuffer\""
                << ",\"ok\":" << (orbitOk ? "true" : "false")
                << ",\"encoderError\":" << JsonStr(encError)
                << "}\n";
    }

    const igi::VisualIntegrityResult visualResult = igi::EvaluateVisualIntegrity(visualInput);
    char visualJsonPath[256];
    snprintf(visualJsonPath, sizeof(visualJsonPath), "screenshots/Level%02d_Model%s_visual-integrity.json",
             cmd.level, cmd.modelId.c_str());
    {
        std::ofstream visualFile(visualJsonPath, std::ios::out | std::ios::trunc);
        visualFile << "{\"schemaVersion\":1,\"object\":{\"level\":" << cmd.level
                   << ",\"taskId\":" << JsonStr(cmd.taskId.empty() ? obj.taskId : cmd.taskId)
                   << ",\"modelId\":" << JsonStr(obj.modelId)
                   << "},\"transform\":{\"authored\":{\"position\":["
                   << obj.pos.x << ',' << obj.pos.y << ',' << obj.pos.z << "],\"rotation\":["
                   << obj.rot.x << ',' << obj.rot.y << ',' << obj.rot.z << "]},\"runtime\":{\"position\":["
                   << obj.pos.x << ',' << obj.pos.y << ',' << obj.pos.z << "],\"rotation\":["
                   << obj.rot.x << ',' << obj.rot.y << ',' << obj.rot.z
                   << "]},\"matchesAuthored\":true},\"capture\":{\"viewport\":["
                   << W << ',' << H << "],\"stillViews\":" << visualInput.views.size()
                   << ",\"source\":\"rendered-framebuffer\"},\"thresholds\":{\"minimumObservedPartRatio\":"
                   << visualInput.minimumObservedPartRatio
                   << ",\"minimumPartPixels\":" << visualInput.minimumPartPixels
                   << ",\"minimumInteriorHolePixels\":" << visualInput.minimumInteriorHolePixels
                   << "},\"expectedParts\":[";
        for (size_t i = 0; i < visualExpectedParts.size(); ++i) {
            if (i) visualFile << ',';
            const auto& part = visualExpectedParts[i];
            visualFile << "{\"id\":" << part.id << ",\"vertexCount\":" << part.vertexCount
                       << ",\"triangleCount\":" << part.triangleCount
                       << ",\"materialSlot\":" << part.materialSlot
                       << ",\"alphaMode\":" << part.alphaMode
                       << ",\"textureIdentity\":" << JsonStr(part.textureIdentity)
                       << ",\"textureResolved\":" << (part.textureResolved ? "true" : "false")
                       << ",\"textureChromaticPixelRatio\":" << part.textureChromaticPixelRatio
                       << ",\"localBoundsMin\":[" << part.localBoundsMin.x << ','
                       << part.localBoundsMin.y << ',' << part.localBoundsMin.z << ']'
                       << ",\"localBoundsMax\":[" << part.localBoundsMax.x << ','
                       << part.localBoundsMax.y << ',' << part.localBoundsMax.z << ']'
                       << '}';
        }
        visualFile << "],\"projectedParts\":[";
        for (size_t viewIndex = 0; viewIndex < visualInput.views.size(); ++viewIndex) {
            const auto& view = visualInput.views[viewIndex];
            if (viewIndex) visualFile << ',';
            visualFile << "{\"view\":" << JsonStr(view.name)
                       << ",\"geometryProjectionMatchesRenderedFrame\":"
                       << (view.geometryProjectionMatchesRenderedFrame ? "true" : "false")
                       << ",\"parts\":[";
            std::map<int, std::array<int, 7>> projected;
            for (int y = 0; y < view.height; ++y) {
                for (int x = 0; x < view.width; ++x) {
                    const size_t pixel = static_cast<size_t>(y) * view.width + x;
                    const int id = view.partIds[pixel];
                    if (id <= 0) continue;
                    auto& bounds = projected[id];
                    if (bounds[4] == 0) {
                        bounds = {x, y, x, y, 0, 0, 0};
                    } else {
                        bounds[0] = std::min(bounds[0], x);
                        bounds[1] = std::min(bounds[1], y);
                        bounds[2] = std::max(bounds[2], x);
                        bounds[3] = std::max(bounds[3], y);
                    }
                    ++bounds[4];
                    if (view.targetMask[pixel] != 0) ++bounds[5];
                    if (view.targetDepth[pixel] < 1.0f) ++bounds[6];
                }
            }
            bool firstPart = true;
            for (const auto& entry : projected) {
                if (!firstPart) visualFile << ',';
                firstPart = false;
                const auto& bounds = entry.second;
                visualFile << "{\"id\":" << entry.first
                           << ",\"minX\":" << bounds[0] << ",\"minY\":" << bounds[1]
                           << ",\"maxX\":" << bounds[2] << ",\"maxY\":" << bounds[3]
                           << ",\"projectedPixels\":" << bounds[4]
                           << ",\"observedPixels\":" << bounds[5]
                           << ",\"depthEvidencePixels\":" << bounds[6] << '}';
            }
            visualFile << "]}";
        }
        visualFile << "],\"evidence\":{\"objectMasks\":[";
        for (size_t i = 0; i < visualMaskPaths.size(); ++i) {
            if (i) visualFile << ',';
            visualFile << JsonStr(visualMaskPaths[i]);
        }
        visualFile << "],\"materialMasks\":[";
        for (size_t i = 0; i < visualPartPaths.size(); ++i) {
            if (i) visualFile << ',';
            visualFile << JsonStr(visualPartPaths[i]);
        }
        visualFile << "],\"depthBuffers\":[";
        for (size_t i = 0; i < visualDepthPaths.size(); ++i) {
            if (i) visualFile << ',';
            visualFile << JsonStr(visualDepthPaths[i]);
        }
        visualFile << "],\"overlays\":[";
        for (size_t i = 0; i < visualOverlayPaths.size(); ++i) {
            if (i) visualFile << ',';
            visualFile << JsonStr(visualOverlayPaths[i]);
        }
        visualFile << "]},\"visualIntegrity\":" << igi::VisualIntegrityJson(visualResult) << "}\n";
    }
    evFile.close();

    // Done sentinel — written last so PowerShell wait function triggers only when all files exist
    std::ofstream doneFile(donePath);
    doneFile << "DONE frames=" << cmd.orbit_frames
             << " orbit=" << (orbitOk ? "ok" : "skip") << "\n";
    doneFile.close();

    Logger::Get().Log(LogLevel::INFO, "[CaptureModel] Complete model=" + cmd.modelId +
        " orbit=" + (orbitOk ? "ok" : "skip"));
}
