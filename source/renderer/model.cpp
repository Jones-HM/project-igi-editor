#include "model.h"
#include "mef_native.h"
#include "../pch.h"
#include "../logger.h"
#include <glm/gtc/type_ptr.hpp>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

Mesh BuildMeshFromGeometry(const ParsedGeometry& geometry, const std::string& filepath) {
    if (geometry.vertices.empty() || geometry.triangles.empty()) {
        throw std::runtime_error("MEF file contains no geometry: " + filepath);
    }

    glm::vec3 min_p(std::numeric_limits<float>::max());
    glm::vec3 max_p(std::numeric_limits<float>::lowest());

    // Type 1 (skeletal) XTRV vertices carry valid per-vertex smooth normals.
    // Other types (Type 3 lightmap) store UV data in the normal field slot, so we
    // fall back to the computed face normal for those.
    const bool useVertexNormals = (geometry.modelType == 1);

    Mesh mesh;
    
    // Copy magic vertices if they exist
    for (const auto& xv : geometry.xtvmVerts) {
        mesh.magicVertices.push_back(glm::vec3(xv.px, xv.py, xv.pz) * kMefNativeScale);
    }

    auto buildSubMesh = [&](size_t triangleStart, size_t triangleCount) -> std::optional<SubMesh> {
        std::vector<float> verts;
        verts.reserve(triangleCount * 3 * 10);

        for (size_t triIndex = triangleStart; triIndex < triangleStart + triangleCount; ++triIndex) {
            const auto& tri = geometry.triangles[triIndex];
            if (tri[0] >= geometry.vertices.size() ||
                tri[1] >= geometry.vertices.size() ||
                tri[2] >= geometry.vertices.size()) {
                continue;
            }

            const glm::vec3 p0 = geometry.vertices[tri[0]].pos;
            const glm::vec3 p1 = geometry.vertices[tri[1]].pos;
            const glm::vec3 p2 = geometry.vertices[tri[2]].pos;

            glm::vec3 faceNormal = glm::cross(p1 - p0, p2 - p0);
            const float len = glm::length(faceNormal);
            if (len > 1e-6f) {
                faceNormal /= len;
            } else {
                faceNormal = glm::vec3(0.0f, 0.0f, 1.0f);
            }

            auto addVertex = [&](uint32_t index) {
                const RenderVertex& src = geometry.vertices[index];
                verts.push_back(src.pos.x);
                verts.push_back(src.pos.y);
                verts.push_back(src.pos.z);

                min_p.x = std::min(min_p.x, src.pos.x);
                min_p.y = std::min(min_p.y, src.pos.y);
                min_p.z = std::min(min_p.z, src.pos.z);

                max_p.x = std::max(max_p.x, src.pos.x);
                max_p.y = std::max(max_p.y, src.pos.y);
                max_p.z = std::max(max_p.z, src.pos.z);

                const glm::vec3& n = useVertexNormals ? src.normal : faceNormal;
                verts.push_back(n.x);
                verts.push_back(n.y);
                verts.push_back(n.z);

                // MEF UVs use image-space V; the OpenGL texture upload uses the
                // opposite origin, so flip V at the model sampling boundary.
                verts.push_back(src.uv.x);
                verts.push_back(1.0f - src.uv.y);
                verts.push_back(geometry.modelType == 3 ? src.uv2.x : 0.0f);
                verts.push_back(geometry.modelType == 3 ? src.uv2.y : 0.0f);
            };

            addVertex(tri[0]);
            addVertex(tri[1]);
            addVertex(tri[2]);
        }

        if (verts.empty()) {
            return std::nullopt;
        }

        SubMesh sub;
        sub.textureID = 0;
        sub.alphaMode = 0;
        sub.vertexCount = static_cast<int>(verts.size() / 10);
        sub.baseColorFactor = glm::vec4(1.0f);
        sub.localMin = glm::vec3(std::numeric_limits<float>::max());
        sub.localMax = glm::vec3(std::numeric_limits<float>::lowest());
        for (size_t v = 0; v + 2 < verts.size(); v += 10) {
            const glm::vec3 p(verts[v], verts[v + 1], verts[v + 2]);
            sub.localMin = glm::min(sub.localMin, p);
            sub.localMax = glm::max(sub.localMax, p);
        }

        glGenVertexArrays(1, &sub.VAO);
        glGenBuffers(1, &sub.VBO);
        glBindVertexArray(sub.VAO);
        glBindBuffer(GL_ARRAY_BUFFER, sub.VBO);
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void*)(6 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void*)(8 * sizeof(float)));
        glEnableVertexAttribArray(3);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);

        return sub;
    };

    // Building the vertex buffer for one render block (or a group of blocks sharing
    // a material slot). Shared by both the per-material-grouped path (modelType != 3)
    // and the per-block path (modelType == 3, see below).
    auto buildBlockGroupVerts = [&](const std::vector<size_t>& blockIndices, bool& outHasAlpha) {
        std::vector<float> verts;
        outHasAlpha = false;
        for (size_t bi : blockIndices) {
            if (geometry.renderBlocks[bi].opacity < 1.0f) outHasAlpha = true;
        }
        for (size_t blockIndex : blockIndices) {
            const auto& block = geometry.renderBlocks[blockIndex];
            if (block.triangleCount == 0) continue;

            for (size_t triIndex = block.triangleStart; triIndex < block.triangleStart + block.triangleCount; ++triIndex) {
                const auto& tri = geometry.triangles[triIndex];
                if (tri[0] >= geometry.vertices.size() ||
                    tri[1] >= geometry.vertices.size() ||
                    tri[2] >= geometry.vertices.size()) {
                    continue;
                }

                const glm::vec3 p0 = geometry.vertices[tri[0]].pos;
                const glm::vec3 p1 = geometry.vertices[tri[1]].pos;
                const glm::vec3 p2 = geometry.vertices[tri[2]].pos;

                glm::vec3 faceNormal = glm::cross(p1 - p0, p2 - p0);
                const float len = glm::length(faceNormal);
                if (len > 1e-6f) {
                    faceNormal /= len;
                } else {
                    faceNormal = glm::vec3(0.0f, 0.0f, 1.0f);
                }

                auto addVertex = [&](uint32_t index) {
                    const RenderVertex& src = geometry.vertices[index];
                    verts.push_back(src.pos.x);
                    verts.push_back(src.pos.y);
                    verts.push_back(src.pos.z);

                    min_p.x = std::min(min_p.x, src.pos.x);
                    min_p.y = std::min(min_p.y, src.pos.y);
                    min_p.z = std::min(min_p.z, src.pos.z);

                    max_p.x = std::max(max_p.x, src.pos.x);
                    max_p.y = std::max(max_p.y, src.pos.y);
                    max_p.z = std::max(max_p.z, src.pos.z);

                    const glm::vec3& n = useVertexNormals ? src.normal : faceNormal;
                    verts.push_back(n.x);
                    verts.push_back(n.y);
                    verts.push_back(n.z);

                    verts.push_back(src.uv.x);
                    verts.push_back(1.0f - src.uv.y);
                    verts.push_back(geometry.modelType == 3 ? src.uv2.x : 0.0f);
                    verts.push_back(geometry.modelType == 3 ? src.uv2.y : 0.0f);
                };

                addVertex(tri[0]);
                addVertex(tri[1]);
                addVertex(tri[2]);
            }
        }
        return verts;
    };

    auto uploadSubMesh = [&](std::vector<float>& verts, int materialSlot, bool hasAlpha) -> std::optional<SubMesh> {
        if (verts.empty()) return std::nullopt;

        SubMesh sub;
        sub.textureID = 0;
        sub.vertexCount = static_cast<int>(verts.size() / 10);
        sub.baseColorFactor = glm::vec4(1.0f);
        sub.alphaMode = hasAlpha ? 2 : 0;
        sub.materialSlot = materialSlot;
        sub.localMin = glm::vec3(std::numeric_limits<float>::max());
        sub.localMax = glm::vec3(std::numeric_limits<float>::lowest());
        for (size_t v = 0; v + 2 < verts.size(); v += 10) {
            const glm::vec3 p(verts[v], verts[v + 1], verts[v + 2]);
            sub.localMin = glm::min(sub.localMin, p);
            sub.localMax = glm::max(sub.localMax, p);
        }

        // Average local-space normal (normal is floats [3..5] of each 10-float vertex) —
        // used to re-light this block's baked lightmap when the object is rotated.
        {
            glm::vec3 acc(0.0f);
            for (size_t v = 0; v + 5 < verts.size(); v += 10) {
                acc += glm::vec3(verts[v + 3], verts[v + 4], verts[v + 5]);
            }
            sub.avgNormal = (glm::length(acc) > 1e-6f) ? glm::normalize(acc) : glm::vec3(0.0f, 0.0f, 1.0f);
        }

        glGenVertexArrays(1, &sub.VAO);
        glGenBuffers(1, &sub.VBO);
        glBindVertexArray(sub.VAO);
        glBindBuffer(GL_ARRAY_BUFFER, sub.VBO);
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void*)(6 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void*)(8 * sizeof(float)));
        glEnableVertexAttribArray(3);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
        return sub;
    };

    if (!geometry.renderBlocks.empty() && geometry.modelType == 3) {
        // Lightmapped buildings: the resolved .olm files are numbered 1:1 with
        // renderBlocks in original order (igi1conv's "renderBlocks[i] -> olm file i"
        // rule), and each render block's UV2 addresses ITS OWN atlas image — they
        // cannot be merged across blocks. Grouping by material slot (as done below
        // for every other model type) would mix multiple blocks' UV2 coordinates
        // into one submesh sampling a single arbitrary lightmap texture, landing on
        // unrelated/black atlas regions. So buildings get one submesh PER render
        // block, preserving index order — including a zero-vertex placeholder for
        // empty blocks — so renderer_objects.cpp's `lightmaps[si]` lookup lines up
        // exactly with `obj00000_000NN.olm`'s block number NN.
        for (size_t i = 0; i < geometry.renderBlocks.size(); ++i) {
            bool hasAlpha = false;
            std::vector<float> verts = buildBlockGroupVerts({i}, hasAlpha);
            SubMesh sub;
            if (auto built = uploadSubMesh(verts, geometry.renderBlocks[i].materialSlot, hasAlpha)) {
                sub = *built;
                mesh.vertexCount += sub.vertexCount;
            }
            // Push even when empty (VAO==0) so subMeshes.size() == renderBlocks.size().
            mesh.subMeshes.push_back(sub);
        }
    } else if (!geometry.renderBlocks.empty()) {
        std::vector<int> materialOrder;
        std::unordered_map<int, std::vector<size_t>> groupedBlocks;
        for (size_t i = 0; i < geometry.renderBlocks.size(); ++i) {
            const int materialSlot = geometry.renderBlocks[i].materialSlot;
            if (groupedBlocks.find(materialSlot) == groupedBlocks.end()) {
                materialOrder.push_back(materialSlot);
            }
            groupedBlocks[materialSlot].push_back(i);
        }
        std::sort(materialOrder.begin(), materialOrder.end());

        for (int materialSlot : materialOrder) {
            bool hasAlpha = false;
            std::vector<float> verts = buildBlockGroupVerts(groupedBlocks[materialSlot], hasAlpha);
            auto built = uploadSubMesh(verts, materialSlot, hasAlpha);
            if (!built) continue;
            mesh.vertexCount += built->vertexCount;
            mesh.subMeshes.push_back(*built);
        }
    }

    if (mesh.subMeshes.empty()) {
        if (auto sub = buildSubMesh(0, geometry.triangles.size())) {
            mesh.vertexCount = sub->vertexCount;
            mesh.subMeshes.push_back(*sub);
        }
    }

    if (mesh.subMeshes.empty()) {
        throw std::runtime_error("MEF file resulted in no valid vertices: " + filepath);
    }

    // Portal meshes (TROP/XVTP/CFTP) are room-visibility culling volumes only.
    // They are NOT rendered visually — uploading them to GPU wastes VRAM and risks
    // accidental rendering. Skip GPU allocation; keep portalMeshes empty.

    mesh.VAO = mesh.subMeshes.front().VAO;
    mesh.VBO = mesh.subMeshes.front().VBO;
    mesh.textureID = 0;
    mesh.fromRenderMesh = geometry.fromRenderMesh;
    mesh.halfExtents = (max_p - min_p) * 0.5f;
    mesh.center = (max_p + min_p) * 0.5f;
    // Native MEF stays in the game's coordinate space, where Z is up.
    // Ground snapping must therefore use the mesh's lowest Z, not lowest Y.
    mesh.zOffset = -min_p.z;
    mesh.mainZOffset = mesh.zOffset;
    mesh.vertexData = nullptr;

    return mesh;
}

} // namespace

Mesh loadObjModel(const std::string& filepath, const std::string& /*unused*/) {
    Logger::Get().Log(
        LogLevel::DEBUG,
        "[MEF Binary Native] Loading file=" + filepath);

    const ParsedGeometry geometry = ParseMefFile(filepath);

    Logger::Get().Log(
        LogLevel::DEBUG,
        "[MEF Binary Native] Geometry source=" + std::string(geometry.fromRenderMesh ? "XTRV/DNER" : "XTVC/ECFC fallback") +
        " modelType=" + std::to_string(geometry.modelType) +
        " renderLayout=" + geometry.renderLayout +
        " vertexCount=" + std::to_string(geometry.vertices.size()) +
        " triangleCount=" + std::to_string(geometry.triangles.size()) +
        " renderBlocks=" + std::to_string(geometry.renderBlockCount) +
        " collisionVerts=" + std::to_string(geometry.collisionVertexCount) +
        " collisionFaces=" + std::to_string(geometry.collisionFaceCount) +
        " bones=" + std::to_string(geometry.bones.size()) +
        " attachments=" + std::to_string(geometry.attachments.size()) +
        " importScale=" + std::to_string(kMefNativeScale));

    Mesh mesh = BuildMeshFromGeometry(geometry, filepath);

    Logger::Get().Log(
        LogLevel::INFO,
        "[MEF Binary Native] Mesh bounds center=(" +
            std::to_string(mesh.center.x) + "," +
            std::to_string(mesh.center.y) + "," +
            std::to_string(mesh.center.z) + ")" +
        " halfExtents=(" +
            std::to_string(mesh.halfExtents.x) + "," +
            std::to_string(mesh.halfExtents.y) + "," +
            std::to_string(mesh.halfExtents.z) + ")" +
        " zOffset=" + std::to_string(mesh.zOffset) +
        " mainZOffset=" + std::to_string(mesh.mainZOffset) +
        " subMeshes=" + std::to_string(mesh.subMeshes.size()) +
        " gpuVertices=" + std::to_string(mesh.vertexCount));

    std::cout << "[MEF Binary Native] Loaded: " << filepath
              << " | Vertices: " << mesh.vertexCount << "\n";
    Logger::Get().Log(LogLevel::INFO,
        "[MEF Binary Native] Loaded: " + filepath + " | Vertices: " + std::to_string(mesh.vertexCount));
    return mesh;
}

Mesh loadObjModelFromMemory(const std::vector<uint8_t>& bytes, const std::string& modelId) {
    const ParsedGeometry geometry = ParseMefFileFromMemory(bytes);
    return BuildMeshFromGeometry(geometry, modelId);
}

void renderModel(const Mesh& mesh) {
    if (mesh.subMeshes.empty() && mesh.VAO == 0) return;

    if (!mesh.subMeshes.empty()) {
        for (const auto& sub : mesh.subMeshes) {
            if (sub.VAO == 0 || sub.vertexCount == 0) continue;
            glBindVertexArray(sub.VAO);
            glDrawArrays(GL_TRIANGLES, 0, sub.vertexCount);
        }
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    } else {
        glBindVertexArray(mesh.VAO);
        glDrawArrays(GL_TRIANGLES, 0, mesh.vertexCount);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
}

void destroyModel(Mesh& mesh) {
    for (auto& sub : mesh.subMeshes) {
        if (sub.VBO) glDeleteBuffers(1, &sub.VBO);
        if (sub.VAO) glDeleteVertexArrays(1, &sub.VAO);
        sub.VAO = sub.VBO = sub.textureID = 0;
    }
    mesh.subMeshes.clear();

    for (auto& sub : mesh.portalMeshes) {
        if (sub.VBO) glDeleteBuffers(1, &sub.VBO);
        if (sub.VAO) glDeleteVertexArrays(1, &sub.VAO);
        sub.VAO = sub.VBO = sub.textureID = 0;
    }
    mesh.portalMeshes.clear();

    if (mesh.vertexData) {
        delete[] mesh.vertexData;
        mesh.vertexData = nullptr;
    }
    mesh.VAO = mesh.VBO = 0;
    mesh.vertexCount = 0;
}
