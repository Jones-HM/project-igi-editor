#include "visual_integrity.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <sstream>

namespace igi {
namespace {

bool HasExpectedSize(const VisualIntegrityView& view) {
    if (view.width <= 0 || view.height <= 0) return false;
    const size_t pixels = static_cast<size_t>(view.width) * view.height;
    return view.targetMask.size() == pixels && view.partIds.size() == pixels &&
           view.sceneDepth.size() == pixels && view.targetDepth.size() == pixels;
}

bool HasValidRenderedRgb(const VisualIntegrityView& view) {
    return view.renderedRgb.empty() ||
           view.renderedRgb.size() == static_cast<size_t>(view.width) * view.height * 3;
}

int CountChromaticRenderedPixels(const VisualIntegrityView& view, int part_id, int* samples) {
    int chromatic = 0;
    *samples = 0;
    for (size_t pixel = 0; pixel < view.partIds.size(); ++pixel) {
        if (view.targetMask[pixel] == 0 || view.partIds[pixel] != part_id) continue;
        ++*samples;
        const size_t rgb = pixel * 3;
        const uint8_t lo = std::min({view.renderedRgb[rgb], view.renderedRgb[rgb + 1],
                                     view.renderedRgb[rgb + 2]});
        const uint8_t hi = std::max({view.renderedRgb[rgb], view.renderedRgb[rgb + 1],
                                     view.renderedRgb[rgb + 2]});
        if (hi - lo >= 16) ++chromatic;
    }
    return chromatic;
}

constexpr float kMateriallyColorfulTextureRatio = 0.20f;

bool HasFiniteDepth(const VisualIntegrityView& view) {
    return std::all_of(view.sceneDepth.begin(), view.sceneDepth.end(), [](float value) {
        return std::isfinite(value);
    }) && std::all_of(view.targetDepth.begin(), view.targetDepth.end(), [](float value) {
        return std::isfinite(value);
    });
}

bool HasDepthRange(const VisualIntegrityView& view) {
    return std::all_of(view.sceneDepth.begin(), view.sceneDepth.end(),
                       [](float value) { return value >= 0.0f && value <= 1.0f; }) &&
           std::all_of(view.targetDepth.begin(), view.targetDepth.end(),
                       [](float value) { return value >= 0.0f && value <= 1.0f; });
}

void AddFinding(VisualIntegrityResult& result, const char* rule, int view_index,
                const std::string& part, int observed, int required,
                const char* reason, const VisualIntegrityView* view = nullptr) {
    VisualIntegrityFinding finding{rule, part, view_index, observed, required, reason};
    if (view != nullptr) {
        finding.view = view->name;
        finding.evidence = view->overlayPath.empty() ? view->sourceFramePath : view->overlayPath;
    }
    result.findings.push_back(std::move(finding));
}

int CountInteriorHolePixels(const VisualIntegrityView& view, float depth_epsilon,
                            const std::vector<int>& transparent_part_ids) {
    const int width = view.width;
    const int height = view.height;
    std::vector<uint8_t> visited(view.targetMask.size(), 0);
    int largest_hole = 0;
    const int dx[] = {1, -1, 0, 0};
    const int dy[] = {0, 0, 1, -1};

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int start = y * width + x;
            if (visited[start] || view.targetMask[start] != 0) continue;

            std::queue<int> pending;
            pending.push(start);
            visited[start] = 1;
            int projected_area = 0;
            bool touches_bounds = false;
            while (!pending.empty()) {
                const int index = pending.front();
                pending.pop();
                // Geometry behind a nearer scene fragment is an occlusion,
                // not a break in this target's silhouette.  Count only
                // projected pixels which should have been visible here.
                if (view.partIds[index] > 0 &&
                    std::find(transparent_part_ids.begin(), transparent_part_ids.end(),
                              view.partIds[index]) == transparent_part_ids.end() &&
                    view.sceneDepth[index] >= view.targetDepth[index] - depth_epsilon) {
                    ++projected_area;
                }
                const int px = index % width;
                const int py = index / width;
                touches_bounds |= px == 0 || py == 0 || px == width - 1 || py == height - 1;
                for (int dir = 0; dir < 4; ++dir) {
                    const int nx = px + dx[dir];
                    const int ny = py + dy[dir];
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                    const int next = ny * width + nx;
                    if (visited[next] || view.targetMask[next] != 0) continue;
                    visited[next] = 1;
                    pending.push(next);
                }
            }
            // A hole is only meaningful when it is enclosed by visible target
            // fragments. Its geometry-derived area excludes authored openings.
            if (!touches_bounds) largest_hole = std::max(largest_hole, projected_area);
        }
    }
    return largest_hole;
}

std::string EscapeJson(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        if (ch == '\\' || ch == '"') escaped.push_back('\\');
        escaped.push_back(ch);
    }
    return escaped;
}

const char* FindingSeverity(const VisualIntegrityFinding& finding) {
    // Occlusion is useful diagnostic evidence, but it does not prove that the
    // target renderer dropped a part. The same is true when there is too
    // little projected/temporal data to make a deterministic decision.
    if (finding.rule == "occluded-part" ||
        finding.rule == "insufficient-projection" ||
        finding.rule == "temporal-evidence" ||
        finding.rule == "projection-evidence") {
        return "warning";
    }
    return "error";
}

struct TemporalMeasurement {
    int observedPixels = 0;
    int projectedPixels = 0;
};

int CountPart(const std::vector<int>& parts, int id) {
    return static_cast<int>(std::count(parts.begin(), parts.end(), id));
}

}  // namespace

VisualIntegrityResult EvaluateVisualIntegrity(const VisualIntegrityInput& input) {
    VisualIntegrityResult result;
    if (input.expectedPartIds.empty() || input.views.empty()) return result;
    if (!std::isfinite(input.minimumPartCoverageRatio) ||
        input.minimumPartCoverageRatio < 0.0f || input.minimumPartCoverageRatio > 1.0f ||
        !std::isfinite(input.minimumObservedPartRatio) ||
        input.minimumObservedPartRatio < 0.0f || input.minimumObservedPartRatio > 1.0f ||
        !std::isfinite(input.depthEpsilon) || input.depthEpsilon < 0.0f ||
        !std::isfinite(input.maximumTemporalAreaDelta) ||
        input.maximumTemporalAreaDelta < 0.0f || input.maximumTemporalAreaDelta > 1.0f ||
        input.minimumInteriorHolePixels < 0 || input.minimumPartPixels < 1) {
        AddFinding(result, "invalid-inventory", -1, "", 0, 0,
                   "visual-integrity thresholds must be finite and within their allowed ranges");
        result.status = VisualIntegrityStatus::kFail;
        return result;
    }

    std::vector<int> expected_ids = input.expectedPartIds;
    std::sort(expected_ids.begin(), expected_ids.end());
    if (expected_ids.front() <= 0 ||
        std::adjacent_find(expected_ids.begin(), expected_ids.end()) != expected_ids.end()) {
        AddFinding(result, "invalid-inventory", -1, "", 0, 0,
                   "expected part IDs must be unique positive values");
        result.status = VisualIntegrityStatus::kFail;
        return result;
    }
    std::vector<int> strict_ids = input.strictPartIds;
    std::sort(strict_ids.begin(), strict_ids.end());
    if ((!strict_ids.empty() && strict_ids.front() <= 0) ||
        std::adjacent_find(strict_ids.begin(), strict_ids.end()) != strict_ids.end() ||
        std::any_of(strict_ids.begin(), strict_ids.end(), [&](int id) {
            return !std::binary_search(expected_ids.begin(), expected_ids.end(), id);
        })) {
        AddFinding(result, "invalid-inventory", -1, "", 0, 0,
                   "strict part IDs must be unique expected parts");
        result.status = VisualIntegrityStatus::kFail;
        return result;
    }

    if (!input.expectedParts.empty()) {
        std::vector<int> inventory_ids;
        inventory_ids.reserve(input.expectedParts.size());
        for (const VisualIntegrityPart& part : input.expectedParts) {
            inventory_ids.push_back(part.id);
            const bool has_geometry_metadata = part.vertexCount != 0 || part.triangleCount != 0 ||
                part.localBoundsMin.x != 0.0f || part.localBoundsMin.y != 0.0f ||
                part.localBoundsMin.z != 0.0f || part.localBoundsMax.x != 0.0f ||
                part.localBoundsMax.y != 0.0f || part.localBoundsMax.z != 0.0f ||
                !part.textureIdentity.empty();
            if (part.id <= 0 ||
                (has_geometry_metadata && (part.vertexCount <= 0 || part.triangleCount <= 0)) ||
                !std::isfinite(part.localBoundsMin.x) ||
                !std::isfinite(part.localBoundsMin.y) ||
                !std::isfinite(part.localBoundsMin.z) ||
                !std::isfinite(part.localBoundsMax.x) ||
                !std::isfinite(part.localBoundsMax.y) ||
                !std::isfinite(part.localBoundsMax.z) ||
                !std::isfinite(part.textureChromaticPixelRatio) ||
                part.textureChromaticPixelRatio < 0.0f || part.textureChromaticPixelRatio > 1.0f ||
                (has_geometry_metadata && part.localBoundsMin.x > part.localBoundsMax.x) ||
                (has_geometry_metadata && part.localBoundsMin.y > part.localBoundsMax.y) ||
                (has_geometry_metadata && part.localBoundsMin.z > part.localBoundsMax.z)) {
                AddFinding(result, "invalid-inventory", -1, std::to_string(part.id), 0, 0,
                           "expected geometry inventory contains invalid counts or bounds");
                result.status = VisualIntegrityStatus::kFail;
                return result;
            }
            if (!part.textureIdentity.empty() && !part.textureResolved) {
                AddFinding(result, "texture-resolution", -1, std::to_string(part.id), 0, 1,
                           "authored texture identity fell back to untextured rendering");
            }
        }
        std::sort(inventory_ids.begin(), inventory_ids.end());
        if (std::adjacent_find(inventory_ids.begin(), inventory_ids.end()) != inventory_ids.end() ||
            inventory_ids != expected_ids) {
            AddFinding(result, "invalid-inventory", -1, "", 0, 0,
                       "geometry inventory IDs must exactly match expected part IDs");
            result.status = VisualIntegrityStatus::kFail;
            return result;
        }
    }

    // A part is expected only when the target-only geometry pass projects it in
    // at least one sampled view.  Authoring inventory alone is insufficient:
    // a zero-area plane or a permanently back-facing sheet has no observable
    // screen-space coverage requirement.
    std::vector<int> projected_parts;
    std::vector<int> observed_parts;
    std::vector<int> occluded_parts;
    std::vector<int> missing_parts;
    std::vector<int> transparent_part_ids;
    for (const VisualIntegrityPart& part : input.expectedParts) {
        if (part.alphaMode == 2) transparent_part_ids.push_back(part.id);
    }
    bool has_valid_view = false;
    bool has_projection_evidence = false;
    bool has_inconclusive_evidence = false;
    std::map<int, std::vector<TemporalMeasurement>> temporal_samples;
    for (size_t view_index = 0; view_index < input.views.size(); ++view_index) {
        const VisualIntegrityView& view = input.views[view_index];
        if (!HasExpectedSize(view) || !HasValidRenderedRgb(view) || !HasFiniteDepth(view) || !HasDepthRange(view)) {
            AddFinding(result, "invalid-evidence", static_cast<int>(view_index), "", 0, 0,
                       "target, material, and depth buffers must match the viewport and depth range", &view);
            continue;
        }
        has_valid_view = true;
        ++result.viewsChecked;
        bool view_failed = false;
        if (!view.transformMatchesAuthored) {
            AddFinding(result, "transform-agreement", static_cast<int>(view_index), "", 0, 1,
                       "runtime target transform differs from its authored task anchor", &view);
            view_failed = true;
        }
        // The ordinary scene may have been drawn from a live skinned pose,
        // while this diagnostic only has rest-pose vertices. Do not compare
        // those different silhouettes or material projections.
        if (!view.geometryProjectionMatchesRenderedFrame) {
            if (view_failed) ++result.viewsFailed;
            else ++result.viewsPassed;
            continue;
        }
        has_projection_evidence = true;
        int target_pixels = 0;
        int projected_target_pixels = 0;
        for (size_t pixel = 0; pixel < view.targetMask.size(); ++pixel) {
            if (view.partIds[pixel] > 0 &&
                !std::binary_search(expected_ids.begin(), expected_ids.end(), view.partIds[pixel])) {
                AddFinding(result, "invalid-evidence", static_cast<int>(view_index),
                           std::to_string(view.partIds[pixel]), 0, 0,
                           "diagnostic buffer contains a part ID absent from the geometry inventory", &view);
                view_failed = true;
                continue;
            }
            // `partIds` is the target-only, geometry-derived projection.  It
            // establishes what can be demanded from this camera.  A fragment
            // is observed only when its depth also agrees with the rendered
            // scene, preventing the diagnostic pass from certifying geometry
            // that the normal renderer failed to emit.
            if (view.partIds[pixel] > 0) {
                projected_parts.push_back(view.partIds[pixel]);
                ++projected_target_pixels;
            }
            if (view.targetMask[pixel] == 0) continue;
            if (view.partIds[pixel] <= 0) {
                AddFinding(result, "target-bounds", static_cast<int>(view_index), "", 0, 1,
                           "visible target pixel is outside the projected target geometry", &view);
                view_failed = true;
                continue;
            }
            if (view.partIds[pixel] > 0) observed_parts.push_back(view.partIds[pixel]);
            ++target_pixels;
            const bool is_transparent_part = std::find(transparent_part_ids.begin(),
                transparent_part_ids.end(), view.partIds[pixel]) != transparent_part_ids.end();
            // Blended fragments can be visibly composited without writing the
            // scene depth buffer. Their visibility is established by the
            // target capture's ordering evidence, not depth equality.
            if (!is_transparent_part && view.sceneDepth[pixel] < 1.0f &&
                std::fabs(view.sceneDepth[pixel] - view.targetDepth[pixel]) > input.depthEpsilon) {
                AddFinding(result, "depth-consistency", static_cast<int>(view_index), "",
                           0, 0, "target depth differs from the visible scene depth", &view);
                view_failed = true;
                break;
            }
        }
        for (size_t pixel = 0; pixel < view.partIds.size(); ++pixel) {
            const int part_id = view.partIds[pixel];
            if (part_id <= 0 || view.targetMask[pixel] != 0) continue;
            if (view.sceneDepth[pixel] < view.targetDepth[pixel] - input.depthEpsilon) {
                occluded_parts.push_back(part_id);
            } else {
                missing_parts.push_back(part_id);
            }
        }
        if (target_pixels == 0) {
            AddFinding(result, "target-presence", static_cast<int>(view_index), "", 0, 1,
                       "target produced no visible fragments", &view);
            view_failed = true;
        }
        if (view.temporalGroup >= 0 && target_pixels > 0) {
            temporal_samples[view.temporalGroup].push_back(
                {target_pixels, projected_target_pixels});
        }
        const int largest_hole = CountInteriorHolePixels(
            view, input.depthEpsilon, transparent_part_ids);
        if (largest_hole >= input.minimumInteriorHolePixels) {
            AddFinding(result, "silhouette-hole", static_cast<int>(view_index), "",
                       largest_hole, input.minimumInteriorHolePixels,
                       "target mask contains an unexplained interior hole", &view);
            view_failed = true;
        }
        if (view_failed) ++result.viewsFailed;
        else ++result.viewsPassed;
    }

    if (!has_valid_view) return result;
    result.partsExpected = static_cast<int>(input.expectedPartIds.size());
    if (!has_projection_evidence && !input.expectedParts.empty()) {
        AddFinding(result, "projection-evidence", -1, "", 0, 1,
                   "substantive geometry inventory has no view with a matching rendered-frame projection");
        has_inconclusive_evidence = true;
    }
    auto evidence_for_part = [&](int part_id) -> const VisualIntegrityView* {
        for (const VisualIntegrityView& view : input.views) {
            if (HasExpectedSize(view) &&
                std::find(view.partIds.begin(), view.partIds.end(), part_id) != view.partIds.end()) {
                return &view;
            }
        }
        return nullptr;
    };
    if (has_projection_evidence) {
    for (int expected_part : input.expectedPartIds) {
        const VisualIntegrityView* part_view = evidence_for_part(expected_part);
        const int projected = CountPart(projected_parts, expected_part);
        if (projected < input.minimumPartPixels) {
            if (projected > 0) {
                AddFinding(result, "insufficient-projection", -1, std::to_string(expected_part),
                           projected, input.minimumPartPixels,
                           "projected part area is too small to classify reliably", part_view);
                has_inconclusive_evidence = true;
            }
            continue;
        }
        const int pixels = CountPart(observed_parts, expected_part);
        if (pixels < input.minimumPartPixels) {
            const int missing = CountPart(missing_parts, expected_part);
            const int occluded = CountPart(occluded_parts, expected_part);
            if (missing == 0 && occluded > 0) {
                AddFinding(result, "occluded-part", -1, std::to_string(expected_part), pixels,
                           input.minimumPartPixels,
                           "expected target part is fully occluded by a nearer non-target scene surface", part_view);
                has_inconclusive_evidence = true;
            } else {
                AddFinding(result, "part-coverage", -1, std::to_string(expected_part), pixels,
                           input.minimumPartPixels,
                           "expected target part produced too few visible fragments", part_view);
            }
            const auto inventory_part = std::find_if(input.expectedParts.begin(), input.expectedParts.end(),
                [expected_part](const VisualIntegrityPart& part) { return part.id == expected_part; });
            if (inventory_part != input.expectedParts.end() && inventory_part->materialSlot >= 0) {
                AddFinding(result, "material-coverage", -1, std::to_string(inventory_part->materialSlot),
                           pixels, input.minimumPartPixels,
                           "expected authored material slot produced too few target fragments", part_view);
            }
        }
    }
    if (input.minimumPartCoverageRatio > 0.0f) {
        for (int expected_part : input.expectedPartIds) {
            for (size_t view_index = 0; view_index < input.views.size(); ++view_index) {
                const VisualIntegrityView& view = input.views[view_index];
                if (!HasExpectedSize(view)) continue;
                const int projected = CountPart(view.partIds, expected_part);
                if (projected < input.minimumPartPixels) continue;
                int observed = 0;
                for (size_t pixel = 0; pixel < view.partIds.size(); ++pixel) {
                    if (view.partIds[pixel] == expected_part && view.targetMask[pixel] != 0)
                        ++observed;
                }
                const int required = std::max(input.minimumPartPixels,
                    static_cast<int>(std::ceil(projected * input.minimumPartCoverageRatio)));
                if (observed < required) {
                    AddFinding(result, "part-coverage", static_cast<int>(view_index),
                               std::to_string(expected_part), observed, required,
                               "visible target fragments cover too little of the projected part area", &view);
                }
            }
        }
    }
    for (const VisualIntegrityPart& part : input.expectedParts) {
        if (part.alphaMode != 2) continue;
        const int projected = CountPart(projected_parts, part.id);
        if (projected < input.minimumPartPixels) continue;
        int depth_evidence = 0;
        for (const VisualIntegrityView& view : input.views) {
            if (!HasExpectedSize(view)) continue;
            for (size_t pixel = 0; pixel < view.partIds.size(); ++pixel) {
                if (view.partIds[pixel] == part.id && view.targetMask[pixel] != 0 &&
                    view.targetDepth[pixel] < 1.0f) {
                    ++depth_evidence;
                }
            }
        }
        if (depth_evidence < input.minimumPartPixels) {
            AddFinding(result, "transparency-evidence", -1, std::to_string(part.id),
                       depth_evidence, input.minimumPartPixels,
                       "transparent target part has no visible fragment with depth/order evidence",
                       evidence_for_part(part.id));
        }
    }
    }
    // A textured render may have complete geometry coverage while still losing its
    // diffuse binding. Only textures proven to contain substantial colour are
    // assessed, so deliberately neutral materials remain outside this rule.
    for (const VisualIntegrityPart& part : input.expectedParts) {
        // Packed/paletted grayscale textures can report a small amount of
        // channel divergence from format quantization. Require substantial
        // source chroma before treating grayscale output as a texture fault.
        if (part.textureChromaticPixelRatio < kMateriallyColorfulTextureRatio) continue;
        int samples = 0;
        int chromatic = 0;
        for (const VisualIntegrityView& view : input.views) {
            if (!HasExpectedSize(view) || !view.geometryProjectionMatchesRenderedFrame ||
                view.renderedRgb.empty()) continue;
            int view_samples = 0;
            chromatic += CountChromaticRenderedPixels(view, part.id, &view_samples);
            samples += view_samples;
        }
        if (samples >= 16 && chromatic == 0) {
            AddFinding(result, "texture-appearance", -1, std::to_string(part.id), chromatic, 1,
                       "a materially colorful authored texture rendered only grayscale target fragments",
                       evidence_for_part(part.id));
        }
    }
    std::sort(observed_parts.begin(), observed_parts.end());
    observed_parts.erase(std::unique(observed_parts.begin(), observed_parts.end()), observed_parts.end());
    result.partsObserved = static_cast<int>(observed_parts.size());
    if (has_projection_evidence && input.minimumObservedPartRatio > 0.0f) {
        const int required_observed_parts = static_cast<int>(std::ceil(
            static_cast<float>(result.partsExpected) * input.minimumObservedPartRatio));
        if (result.partsObserved < required_observed_parts) {
            std::ostringstream missing_part_ids;
            int listed_parts = 0;
            for (int expected_part : input.expectedPartIds) {
                if (std::binary_search(observed_parts.begin(), observed_parts.end(), expected_part)) continue;
                if (listed_parts++ > 0) missing_part_ids << ',';
                missing_part_ids << expected_part;
                if (listed_parts == 8) break;
            }
            AddFinding(result, "inventory-coverage", -1, missing_part_ids.str(),
                       result.partsObserved, required_observed_parts,
                       "too little expected geometry produced visible target fragments across the capture");
        }
    }
    if (input.requireTemporalEvidence) {
        bool has_repeated_group = false;
        for (const auto& group : temporal_samples) {
            const auto& samples = group.second;
            if (samples.size() < 2) continue;
            has_repeated_group = true;
            const TemporalMeasurement& baseline = samples.front();
            for (size_t index = 1; index < samples.size(); ++index) {
                const float baseline_coverage = static_cast<float>(baseline.observedPixels) /
                    static_cast<float>(std::max(1, baseline.projectedPixels));
                const float coverage = static_cast<float>(samples[index].observedPixels) /
                    static_cast<float>(std::max(1, samples[index].projectedPixels));
                const float coverage_delta = std::abs(coverage - baseline_coverage);
                if (coverage_delta > input.maximumTemporalAreaDelta) {
                    AddFinding(result, "temporal-stability", group.first, "",
                               samples[index].observedPixels, baseline.observedPixels,
                               "camera-normalized target coverage changed beyond the calibrated temporal tolerance");
                }
            }
        }
        if (!has_repeated_group) {
            AddFinding(result, "temporal-evidence", -1, "", 0, 2,
                       "no repeated same-camera target frames were captured");
            has_inconclusive_evidence = true;
        }
    }
    const bool has_error = std::any_of(result.findings.begin(), result.findings.end(),
        [](const VisualIntegrityFinding& finding) {
            return std::string(FindingSeverity(finding)) == "error";
        });
    result.status = has_error ? VisualIntegrityStatus::kFail
        : (has_inconclusive_evidence ? VisualIntegrityStatus::kInconclusive
                                     : VisualIntegrityStatus::kPass);
    return result;
}

const char* VisualIntegrityStatusName(VisualIntegrityStatus status) {
    switch (status) {
        case VisualIntegrityStatus::kPass: return "PASS";
        case VisualIntegrityStatus::kFail: return "FAIL";
        case VisualIntegrityStatus::kInconclusive: return "INCONCLUSIVE";
    }
    return "INCONCLUSIVE";
}

std::string VisualIntegrityJson(const VisualIntegrityResult& result) {
    std::ostringstream output;
    output << "{\"schemaVersion\":1,\"status\":\"" << VisualIntegrityStatusName(result.status)
           << "\",\"viewsChecked\":" << result.viewsChecked
           << ",\"viewsPassed\":" << result.viewsPassed
           << ",\"viewsFailed\":" << result.viewsFailed
           << ",\"partsExpected\":" << result.partsExpected
           << ",\"partsObserved\":" << result.partsObserved
           << ",\"summary\":{\"viewsChecked\":" << result.viewsChecked
           << ",\"viewsPassed\":" << result.viewsPassed
           << ",\"viewsFailed\":" << result.viewsFailed
           << ",\"partsExpected\":" << result.partsExpected
           << ",\"partsObserved\":" << result.partsObserved << "}"
           << ",\"findings\":[";
    for (size_t index = 0; index < result.findings.size(); ++index) {
        const auto& finding = result.findings[index];
        if (index != 0) output << ',';
        output << "{\"rule\":\"" << EscapeJson(finding.rule)
               << "\",\"severity\":\"" << FindingSeverity(finding)
               << "\",\"part\":\"" << EscapeJson(finding.part)
               << "\",\"viewIndex\":" << finding.viewIndex
               << ",\"observedPixels\":" << finding.observedPixels
               << ",\"requiredPixels\":" << finding.requiredPixels
               << ",\"observed\":" << finding.observedPixels
               << ",\"expectedMinimum\":" << finding.requiredPixels
               << ",\"reason\":\"" << EscapeJson(finding.reason) << "\""
               << ",\"views\":";
        if (finding.view.empty()) output << "[]";
        else output << "[\"" << EscapeJson(finding.view) << "\"]";
        output << ",\"evidence\":";
        if (finding.evidence.empty()) output << "[]";
        else output << "[\"" << EscapeJson(finding.evidence) << "\"]";
        output << '}';
    }
    output << "]}";
    return output.str();
}

}  // namespace igi
