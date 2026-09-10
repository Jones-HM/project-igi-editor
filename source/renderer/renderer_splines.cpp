#include "pch.h"
#include "renderer_splines.h"
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>

void Renderer_Splines::Init() {}

namespace {

glm::mat4 ToFloatMatrix(const glm::dmat4& source) {
    glm::mat4 result(1.0f);
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            result[column][row] = static_cast<float>(source[column][row]);
        }
    }
    return result;
}

} // namespace

void Renderer_Splines::Draw(
    const std::vector<LevelObject>& objects,
    GLuint ubo_mats,
    GLuint shader_program)
{
    if (!shader_program) return;

    glUseProgram(shader_program);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo_mats);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    std::vector<TraceTile> traceTiles;
    const bool captureTrace = !pending_trace_path_.empty();

    int routeIndex = 0;
    for (const auto& obj : objects) {
        if (!obj.isSplineContainer || obj.deleted) continue;
        if (Renderer_Objects::IsSkippedModelId(obj.modelId)) continue;

        std::vector<int> children;
        children.reserve(obj.childrenIndices.size());
        for (int childIndex : obj.childrenIndices) {
            if (childIndex < 0 || childIndex >= static_cast<int>(objects.size())) continue;
            const LevelObject& child = objects[childIndex];
            if (!child.deleted && child.isSplineWaypoint) children.push_back(childIndex);
        }

        // Find the first non-empty segmentModelId as fallback for waypoints that leave it blank.
        std::string fallbackSegmentModelId;
        for (int ci : children) {
            if (ci >= 0 && ci < (int)objects.size() && !objects[ci].segmentModelId.empty()) {
                fallbackSegmentModelId = objects[ci].segmentModelId;
                break;
            }
        }

        for (size_t i = 0; i + 1 < children.size(); ++i) {
            int si = children[i];
            int ei = children[i + 1];
            const int pi = (i > 0) ? children[i - 1] : si;
            const int ni = (i + 2 < children.size()) ? children[i + 2] : ei;

            DrawSplineSegment(
                objects[si], objects[ei],
                objects[pi], objects[ni],
                obj, ubo_mats, shader_program,
                routeIndex, static_cast<int>(i), traceTiles,
                fallbackSegmentModelId);
        }
        ++routeIndex;
    }

    glDisable(GL_CULL_FACE);
    glUseProgram(0);
    if (captureTrace) {
        WriteTrace(pending_trace_path_, traceTiles);
        pending_trace_path_.clear();
    }
}

void Renderer_Splines::DrawSplineSegment(
    const LevelObject& start,
    const LevelObject& end,
    const LevelObject& prev,
    const LevelObject& nextNext,
    const LevelObject& parent,
    GLuint ubo_mats,
    GLuint shader_program,
    int routeIndex,
    int intervalIndex,
    std::vector<TraceTile>& traceTiles,
    const std::string& fallbackSegmentModelId)
{
    const std::string& segModelId = start.segmentModelId.empty() ? fallbackSegmentModelId : start.segmentModelId;
    if (segModelId.empty()) return;
    if (Renderer_Objects::IsSkippedModelId(segModelId)) return;

    Mesh mesh = obj_renderer_.GetOrLoadMesh(segModelId, false);
    if (mesh.vertexCount == 0) return;

    GLint loc_model    = glGetUniformLocation(shader_program, "u_model");
    GLint loc_dirlight = glGetUniformLocation(shader_program, "u_dirlight");
    GLint loc_ambient  = glGetUniformLocation(shader_program, "u_ambient");
    GLint loc_useTex      = glGetUniformLocation(shader_program, "u_useTexture");
    GLint loc_tex         = glGetUniformLocation(shader_program, "u_texture");
    GLint loc_useLightmap = glGetUniformLocation(shader_program, "u_useLightmap");
    GLint loc_alpha       = glGetUniformLocation(shader_program, "u_alpha");
    GLint loc_baseColor   = glGetUniformLocation(shader_program, "u_baseColor");
    GLint loc_tint        = glGetUniformLocation(shader_program, "u_tint");
    GLint loc_glassMin    = glGetUniformLocation(shader_program, "u_glassMin");

    const glm::dvec3 p0 = start.pos;
    const glm::dvec3 p1 = end.pos;
    const glm::dvec3 pPrev = prev.pos;
    const glm::dvec3 pNext = nextNext.pos;

    glm::dvec3 tan0 = (p1 - pPrev) * 0.5;
    glm::dvec3 tan1 = (pNext - p0) * 0.5;

    // Clamp tangent magnitude to interval length to prevent overshoot at transitions.
    const double intervalLen = glm::length(p1 - p0);
    const double t0len = glm::length(tan0);
    const double t1len = glm::length(tan1);
    if (t0len > intervalLen && t0len > 0.0) tan0 *= intervalLen / t0len;
    if (t1len > intervalLen && t1len > 0.0) tan1 *= intervalLen / t1len;

    // The segment model's longest measured local extent is its longitudinal axis.
    // Segment families may use either X or Y for that axis; do not assume the
    // first model's orientation applies to every segment model.
    const double LENGTH_SCALE = 40.96;
    int longitudinalAxis = 0;
    if (mesh.halfExtents.y > mesh.halfExtents.x && mesh.halfExtents.y >= mesh.halfExtents.z) {
        longitudinalAxis = 1;
    } else if (mesh.halfExtents.z > mesh.halfExtents.x && mesh.halfExtents.z > mesh.halfExtents.y) {
        longitudinalAxis = 2;
    }
    const double localMin = static_cast<double>(mesh.center[longitudinalAxis]) -
                            static_cast<double>(mesh.halfExtents[longitudinalAxis]);
    const double localLen = static_cast<double>(mesh.halfExtents[longitudinalAxis]) * 2.0;
    if (!std::isfinite(localLen) || localLen <= 1e-9) return;

    // The authored field is the number of matrices generated for each interval.
    // A bounded fallback is used only for older data that has no authored count;
    // it cannot silently stretch a valid authored path.
    const int steps = parent.splineSegmentCount > 0
        ? std::min(parent.splineSegmentCount, 4096)
        : std::max(1, static_cast<int>(std::lround(intervalLen / (localLen * LENGTH_SCALE))));
    if (steps <= 0) return;

    const spline_geometry::SplineSegment segment{p0, p1, tan0, tan1, parent.linearSegments};

    for (int i = 0; i < steps; ++i) {
        const double ta = static_cast<double>(i) / static_cast<double>(steps);
        const double tb = static_cast<double>(i + 1) / static_cast<double>(steps);

        // Tile endpoints sampled on the Hermite curve. Tiles stay at their authored
        // (data) Z — the game does NOT lift track onto terrain; where the track dips
        // below the surface it is simply hidden by the terrain (e.g. track entering a
        // cutting/under a hill). Snapping to terrain wrongly exposed those buried runs.
        const glm::dvec3 a = spline_geometry::SampleSegment(segment, ta);
        const glm::dvec3 b = spline_geometry::SampleSegment(segment, tb);
        const auto tile = spline_geometry::MakeAxisAlignedTile(
            a, b, longitudinalAxis, localMin, localLen, LENGTH_SCALE);
        if (!tile.has_value()) continue;

        const double sx = glm::length(b - a) / localLen;
        glm::dmat4 unscaledDouble = tile->model;
        for (int axis = 0; axis < 3; ++axis) {
            unscaledDouble[axis] /= axis == longitudinalAxis ? sx : LENGTH_SCALE;
        }
        const glm::mat4 unscaledModel = ToFloatMatrix(unscaledDouble);
        const glm::mat4 model = ToFloatMatrix(tile->model);

        glUniformMatrix4fv(loc_model, 1, GL_FALSE, glm::value_ptr(model));

        // Spline tiles are opaque ordinary objects. Rebind every shared-object
        // uniform that a preceding lightmapped/glass draw can change.
        glUniform1i(loc_useLightmap, 0);
        glUniform1f(loc_alpha, 1.0f);
        glUniform4f(loc_baseColor, 1.0f, 1.0f, 1.0f, 1.0f);
        glUniform3f(loc_tint, 1.0f, 1.0f, 1.0f);
        glUniform1f(loc_glassMin, 0.0f);

        for (const auto& sub : mesh.subMeshes) {
            if (sub.VAO == 0 || sub.vertexCount == 0) continue;
            if (sub.textureID > 0) {
                glUniform3f(loc_dirlight, 0.6f, 0.6f, 0.6f);
                glUniform3f(loc_ambient,  0.4f, 0.4f, 0.4f);
                glUniform1i(loc_useTex, 1);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, sub.textureID);
                glUniform1i(loc_tex, 0);
            } else {
                glUniform3f(loc_dirlight, 0.7f, 0.7f, 0.7f);
                glUniform3f(loc_ambient,  0.2f, 0.2f, 0.2f);
                glUniform1i(loc_useTex, 0);
            }
            glBindVertexArray(sub.VAO);
            glDrawArrays(GL_TRIANGLES, 0, sub.vertexCount);
        }
        glBindVertexArray(0);

        // Rails/details (ATTA) use the same orientation and the same X stretch so they
        // stay aligned with the stretched deck tile.
        glm::vec3 attachmentScale(static_cast<float>(LENGTH_SCALE));
        attachmentScale[longitudinalAxis] = static_cast<float>(sx);
        obj_renderer_.DrawAttachmentsForSpline(segModelId, /*isBuilding=*/false, unscaledModel, ubo_mats,
                                               attachmentScale);

        if (!pending_trace_path_.empty()) {
            TraceTile trace;
            trace.routeIndex = routeIndex;
            trace.intervalIndex = intervalIndex;
            trace.tileIndex = i;
            trace.steps = steps;
            trace.longitudinalAxis = longitudinalAxis;
            trace.containerModelId = parent.modelId;
            trace.containerTaskId = parent.taskId;
            trace.segmentModelId = segModelId;
            trace.routeBegin = p0;
            trace.routeEnd = p1;
            trace.tileBegin = tile->begin;
            trace.tileEnd = tile->end;
            trace.attachmentScale = attachmentScale;
            trace.meshSubmeshCount = static_cast<int>(mesh.subMeshes.size());
            trace.meshVertexCount = mesh.vertexCount;
            for (int column = 0; column < 4; ++column) {
                for (int row = 0; row < 4; ++row) {
                    trace.model[static_cast<size_t>(column * 4 + row)] = tile->model[column][row];
                    trace.unscaledModel[static_cast<size_t>(column * 4 + row)] = unscaledDouble[column][row];
                }
            }
            traceTiles.push_back(std::move(trace));
        }
        glUseProgram(shader_program);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo_mats);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glUniform1i(loc_useLightmap, 0);
        glUniform1f(loc_alpha, 1.0f);
        glUniform4f(loc_baseColor, 1.0f, 1.0f, 1.0f, 1.0f);
        glUniform3f(loc_tint, 1.0f, 1.0f, 1.0f);
        glUniform1f(loc_glassMin, 0.0f);
    }
}

namespace {

void WriteTraceNumber(std::ostream& output, double value) {
    output << std::setprecision(17) << value;
}

void WriteTraceVec3(std::ostream& output, const glm::dvec3& value) {
    output << '[';
    WriteTraceNumber(output, value.x); output << ',';
    WriteTraceNumber(output, value.y); output << ',';
    WriteTraceNumber(output, value.z); output << ']';
}

void WriteTraceString(std::ostream& output, const std::string& value) {
    output << '"';
    for (const char c : value) {
        if (c == '"') output << "\\\"";
        else if (c == '\\') output << "\\\\";
        else if (c == '\n') output << "\\n";
        else if (c == '\r') output << "\\r";
        else output << c;
    }
    output << '"';
}

void WriteTraceMatrix(std::ostream& output, const std::array<double, 16>& matrix) {
    output << '[';
    for (size_t i = 0; i < matrix.size(); ++i) {
        if (i != 0) output << ',';
        WriteTraceNumber(output, matrix[i]);
    }
    output << ']';
}

} // namespace

void Renderer_Splines::WriteTrace(const std::string& path, const std::vector<TraceTile>& traceTiles) {
    std::ofstream output(path, std::ios::trunc);
    if (!output.is_open()) return;

    output << "{\"schemaVersion\":1,\"tileCount\":" << traceTiles.size() << ",\"tiles\":[";
    for (size_t i = 0; i < traceTiles.size(); ++i) {
        if (i != 0) output << ',';
        const TraceTile& tile = traceTiles[i];
        output << "{\"routeIndex\":" << tile.routeIndex
               << ",\"intervalIndex\":" << tile.intervalIndex
               << ",\"tileIndex\":" << tile.tileIndex
               << ",\"steps\":" << tile.steps
               << ",\"longitudinalAxis\":" << tile.longitudinalAxis
               << ",\"containerModelId\":";
        WriteTraceString(output, tile.containerModelId);
        output << ",\"containerTaskId\":";
        WriteTraceString(output, tile.containerTaskId);
        output << ",\"segmentModelId\":";
        WriteTraceString(output, tile.segmentModelId);
        output << ",\"routeBegin\":";
        WriteTraceVec3(output, tile.routeBegin);
        output << ",\"routeEnd\":";
        WriteTraceVec3(output, tile.routeEnd);
        output << ",\"tileBegin\":";
        WriteTraceVec3(output, tile.tileBegin);
        output << ",\"tileEnd\":";
        WriteTraceVec3(output, tile.tileEnd);
        output << ",\"model\":";
        WriteTraceMatrix(output, tile.model);
        output << ",\"unscaledModel\":";
        WriteTraceMatrix(output, tile.unscaledModel);
        output << ",\"attachmentScale\":[";
        WriteTraceNumber(output, tile.attachmentScale.x); output << ',';
        WriteTraceNumber(output, tile.attachmentScale.y); output << ',';
        WriteTraceNumber(output, tile.attachmentScale.z); output << ']';
        output << ",\"meshSubmeshCount\":" << tile.meshSubmeshCount
               << ",\"meshVertexCount\":" << tile.meshVertexCount << '}';
    }
    output << "]}";
}

