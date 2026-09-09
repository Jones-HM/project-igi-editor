#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <limits>
#include "mcp/mcp_json.h"
#include "visual_integrity.h"

namespace {

igi::VisualIntegrityInput MakeInput(
    std::vector<int> expected_parts,
    std::vector<igi::VisualIntegrityView> views) {
    igi::VisualIntegrityInput input;
    input.expectedPartIds = std::move(expected_parts);
    input.views = std::move(views);
    return input;
}

igi::VisualIntegrityView MakeView(std::vector<int> part_ids,
                                  int width = 4, int height = 4) {
    igi::VisualIntegrityView view;
    view.width = width;
    view.height = height;
    view.targetMask.assign(static_cast<size_t>(width * height), 1);
    view.partIds = std::move(part_ids);
    view.sceneDepth.assign(static_cast<size_t>(width * height), 0.5f);
    view.targetDepth.assign(static_cast<size_t>(width * height), 0.5f);
    return view;
}

bool HasRule(const igi::VisualIntegrityResult& result, const char* rule) {
    return std::any_of(result.findings.begin(), result.findings.end(),
        [rule](const igi::VisualIntegrityFinding& finding) {
            return finding.rule == rule;
        });
}

igi::VisualIntegrityInput LoadEvidenceFixture(const char* path) {
    std::ifstream file(path, std::ios::binary);
    EXPECT_TRUE(file.is_open()) << path;
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    mcp::JsonValue root;
    mcp::JsonError error;
    EXPECT_TRUE(mcp::JsonParse(text, root, error)) << error.message;

    igi::VisualIntegrityInput input;
    for (const auto& part : root.at("expectedPartIds").as_array())
        input.expectedPartIds.push_back(static_cast<int>(part.as_number()));
    for (const auto& source_view : root.at("views").as_array()) {
        igi::VisualIntegrityView view;
        view.width = static_cast<int>(source_view.at("width").as_number());
        view.height = static_cast<int>(source_view.at("height").as_number());
        view.name = source_view.at("name").as_string();
        for (const auto& id : source_view.at("partIds").as_array())
            view.partIds.push_back(static_cast<int>(id.as_number()));
        for (const auto& visible : source_view.at("targetMask").as_array())
            view.targetMask.push_back(visible.as_bool() ? 1 : 0);
        view.sceneDepth.assign(view.targetMask.size(), 0.5f);
        view.targetDepth.assign(view.targetMask.size(), 0.5f);
        input.views.push_back(std::move(view));
    }
    return input;
}

}  // namespace

TEST(VisualIntegrityTest, FailsWhenExpectedPartHasNoFragments) {
    // Part 2 is projected but absent from the visible target mask.
    auto view = MakeView({1, 1, 1, 1, 1, 1, 1, 1,
                          2, 2, 2, 2, 2, 2, 2, 2});
    for (size_t index = 8; index < view.targetMask.size(); ++index)
        view.targetMask[index] = 0;
    const auto missing = igi::EvaluateVisualIntegrity(MakeInput({1, 2}, {view}));

    EXPECT_EQ(missing.status, igi::VisualIntegrityStatus::kFail);
    EXPECT_TRUE(HasRule(missing, "part-coverage"));
}

TEST(VisualIntegrityTest, ReportsMissingAuthoredMaterialCoverage) {
    auto view = MakeView({1, 1, 1, 1, 2, 2, 2, 2,
                          2, 2, 2, 2, 2, 2, 2, 2});
    view.targetMask.assign(16, 0);
    view.targetMask[0] = view.targetMask[1] = view.targetMask[2] = view.targetMask[3] = 1;
    igi::VisualIntegrityPart part;
    part.id = 2;
    part.materialSlot = 7;
    auto input = MakeInput({1, 2}, {view});
    input.expectedParts = {igi::VisualIntegrityPart{1}, part};
    const auto result = igi::EvaluateVisualIntegrity(input);
    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kFail);
    EXPECT_TRUE(HasRule(result, "material-coverage"));
}

TEST(VisualIntegrityTest, FailsWhenAuthoredTextureUsesUntexturedFallback) {
    auto input = MakeInput({1}, {MakeView(std::vector<int>(16, 1))});
    igi::VisualIntegrityPart part;
    part.id = 1;
    part.vertexCount = 3;
    part.triangleCount = 1;
    part.textureIdentity = "authored_diffuse";
    part.textureResolved = false;
    input.expectedParts = {part};

    const auto result = igi::EvaluateVisualIntegrity(input);

    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kFail);
    EXPECT_TRUE(HasRule(result, "texture-resolution"));
}

TEST(VisualIntegrityTest, FailsWhenColorfulAuthoredMaterialRendersOnlyGrayscale) {
    auto view = MakeView(std::vector<int>(16, 1));
    view.renderedRgb.assign(16 * 3, 128);  // visible geometry, but no diffuse colour survives
    igi::VisualIntegrityPart part;
    part.id = 1;
    part.vertexCount = 3;
    part.triangleCount = 1;
    part.textureIdentity = "authored_diffuse";
    part.textureChromaticPixelRatio = 0.50f;
    auto input = MakeInput({1}, {view});
    input.expectedParts = {part};

    const auto result = igi::EvaluateVisualIntegrity(input);

    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kFail);
    EXPECT_TRUE(HasRule(result, "texture-appearance"));
}

TEST(VisualIntegrityTest, DoesNotRejectNeutralTextureWithFormatChromaNoise) {
    auto view = MakeView(std::vector<int>(16, 1));
    view.renderedRgb.assign(16 * 3, 128);
    igi::VisualIntegrityPart part;
    part.id = 1;
    part.vertexCount = 3;
    part.triangleCount = 1;
    part.textureIdentity = "004_13_1";
    // The real grayscale 004_13_1 texture reports this small ratio because
    // packed/paletted channel quantization creates apparent chroma noise.
    part.textureChromaticPixelRatio = 0.161255f;
    auto input = MakeInput({1}, {view});
    input.expectedParts = {part};

    const auto result = igi::EvaluateVisualIntegrity(input);

    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kPass);
    EXPECT_FALSE(HasRule(result, "texture-appearance"));
}

TEST(VisualIntegrityTest, DoesNotRejectSkinnedFrameFromStaticDiagnosticProjection) {
    // The visible renderer may deform an AI mesh, while the diagnostic pass
    // has only its rest-pose projection. Those two projections cannot be
    // compared for holes or per-submesh coverage.
    auto view = MakeView({1, 1, 1, 1, 2, 2, 2, 2,
                          2, 2, 2, 2, 2, 2, 2, 2});
    view.geometryProjectionMatchesRenderedFrame = false;
    for (size_t index = 4; index < view.targetMask.size(); ++index)
        view.targetMask[index] = 0;

    const auto result = igi::EvaluateVisualIntegrity(MakeInput({1, 2}, {view}));

    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kPass);
    EXPECT_FALSE(HasRule(result, "part-coverage"));
    EXPECT_FALSE(HasRule(result, "silhouette-hole"));
}

TEST(VisualIntegrityTest, DuplicateExpectedPartIdCannotPass) {
    const auto result = igi::EvaluateVisualIntegrity(MakeInput({1, 1}, {MakeView(std::vector<int>(16, 1))}));
    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kFail);
    EXPECT_TRUE(HasRule(result, "invalid-inventory"));
}

TEST(VisualIntegrityTest, StrictAttachmentMustBeAnExpectedUniquePart) {
    auto input = MakeInput({1}, {MakeView(std::vector<int>(16, 1))});
    input.strictPartIds = {2};
    const auto result = igi::EvaluateVisualIntegrity(input);
    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kFail);
    EXPECT_TRUE(HasRule(result, "invalid-inventory"));
}

TEST(VisualIntegrityTest, NonFiniteDepthEvidenceCannotPass) {
    auto view = MakeView(std::vector<int>(16, 1));
    view.sceneDepth[0] = std::numeric_limits<float>::quiet_NaN();
    const auto result = igi::EvaluateVisualIntegrity(MakeInput({1}, {view}));
    EXPECT_NE(result.status, igi::VisualIntegrityStatus::kPass);
    EXPECT_TRUE(HasRule(result, "invalid-evidence"));
}

TEST(VisualIntegrityTest, PassesWhenEveryExpectedPartHasVisibleFragments) {
    const auto result = igi::EvaluateVisualIntegrity(MakeInput(
        {1, 2}, {MakeView({1, 1, 1, 1, 1, 1, 1, 1,
                           2, 2, 2, 2, 2, 2, 2, 2})}));

    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kPass);
    EXPECT_TRUE(result.findings.empty());
}

TEST(VisualIntegrityTest, IgnoresOtherObjectPixelsWhenTargetPartIsMissing) {
    auto view = MakeView({1, 1, 1, 1, 1, 1, 1, 1,
                          2, 2, 2, 2, 2, 2, 2, 2});
    view.targetMask = {1, 1, 1, 1, 1, 1, 1, 1,
                       0, 0, 0, 0, 0, 0, 0, 0};

    const auto result = igi::EvaluateVisualIntegrity(MakeInput({1, 2}, {view}));

    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kFail);
    EXPECT_TRUE(HasRule(result, "part-coverage"));
}

TEST(VisualIntegrityTest, RequiresProjectedPartsToAgreeWithRenderedScene) {
    auto view = MakeView({1, 1, 1, 1, 1, 1, 1, 1,
                          2, 2, 2, 2, 2, 2, 2, 2});
    // Part 2 exists in the target-only geometry projection but never agrees
    // with the visible scene. It must not be counted as renderer coverage.
    for (size_t index = 8; index < view.targetMask.size(); ++index)
        view.targetMask[index] = 0;

    const auto result = igi::EvaluateVisualIntegrity(MakeInput({1, 2}, {view}));

    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kFail);
    EXPECT_TRUE(HasRule(result, "part-coverage"));
}

TEST(VisualIntegrityTest, FailsWhenPartOnlyProducesTokenCoverageOfItsProjection) {
    auto view = MakeView(std::vector<int>(16, 1));
    view.targetMask.assign(16, 0);
    view.targetMask[0] = 1;

    auto input = MakeInput({1}, {view});
    input.minimumPartPixels = 1;
    input.minimumPartCoverageRatio = 0.25f;
    const auto result = igi::EvaluateVisualIntegrity(input);

    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kFail);
    EXPECT_TRUE(HasRule(result, "part-coverage"));
}

TEST(VisualIntegrityTest, FailsWhenTargetDepthDisagreesWithVisibleSceneDepth) {
    auto view = MakeView({1, 1, 1, 1, 1, 1, 1, 1,
                          1, 1, 1, 1, 1, 1, 1, 1});
    view.targetDepth[0] = 0.8f;

    const auto result = igi::EvaluateVisualIntegrity(MakeInput({1}, {view}));

    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kFail);
    EXPECT_TRUE(HasRule(result, "depth-consistency"));
}

TEST(VisualIntegrityTest, AcceptsVisibleTransparentPartWithoutDepthWrite) {
    auto view = MakeView(std::vector<int>(16, 1));
    view.sceneDepth.assign(16, 0.6f);
    view.targetDepth.assign(16, 0.5f);

    auto input = MakeInput({1}, {view});
    input.expectedParts = {{1, 48, 16, 0, 2}};
    const auto result = igi::EvaluateVisualIntegrity(input);

    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kPass);
    EXPECT_FALSE(HasRule(result, "depth-consistency"));
}

TEST(VisualIntegrityTest, FailsWhenInventoryCoverageFallsBelowSharedBaseline) {
    auto view = MakeView(std::vector<int>(16, 1));
    auto input = MakeInput({1, 2, 3, 4}, {view});
    input.minimumObservedPartRatio = 0.75f;

    const auto result = igi::EvaluateVisualIntegrity(input);

    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kFail);
    EXPECT_TRUE(HasRule(result, "inventory-coverage"));
}

TEST(VisualIntegrityTest, FailsWhenTargetMaskContainsLargeInteriorHole) {
    auto view = MakeView({1, 1, 1, 1, 1, 1, 1, 1,
                          1, 1, 1, 1, 1, 1, 1, 1}, 5, 5);
    view.targetMask.assign(25, 1);
    view.partIds.assign(25, 1);
    view.sceneDepth.assign(25, 0.5f);
    view.targetDepth.assign(25, 0.5f);
    view.targetMask[12] = 0;

    auto input = MakeInput({1}, {view});
    input.minimumInteriorHolePixels = 1;
    const auto result = igi::EvaluateVisualIntegrity(input);

    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kFail);
    EXPECT_TRUE(HasRule(result, "silhouette-hole"));
}

TEST(VisualIntegrityTest, DoesNotMisclassifyEnclosedSceneOcclusionAsSilhouetteHole) {
    auto view = MakeView(std::vector<int>(25, 1), 5, 5);
    view.targetMask.assign(25, 1);
    view.sceneDepth.assign(25, 0.5f);
    view.targetDepth.assign(25, 0.5f);
    view.targetMask[12] = 0;
    view.sceneDepth[12] = 0.4f;  // A nearer non-target surface covers the target.

    auto input = MakeInput({1}, {view});
    input.minimumInteriorHolePixels = 1;
    const auto result = igi::EvaluateVisualIntegrity(input);

    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kPass);
    EXPECT_FALSE(HasRule(result, "silhouette-hole"));
}

TEST(VisualIntegrityTest, DoesNotTreatTransparentProjectedRegionAsOpaqueSilhouetteHole) {
    auto view = MakeView(std::vector<int>(25, 1), 5, 5);
    view.targetMask.assign(25, 1);
    view.sceneDepth.assign(25, 0.5f);
    view.targetDepth.assign(25, 0.5f);
    view.targetMask[12] = 0;

    igi::VisualIntegrityPart transparent_part;
    transparent_part.id = 1;
    transparent_part.alphaMode = 2;
    auto input = MakeInput({1}, {view});
    input.expectedParts = {transparent_part};
    input.minimumInteriorHolePixels = 1;
    const auto result = igi::EvaluateVisualIntegrity(input);

    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kPass);
    EXPECT_FALSE(HasRule(result, "silhouette-hole"));
}

TEST(VisualIntegrityTest, AllowsAuthoredOpeningInProjectedSilhouette) {
    auto view = MakeView(std::vector<int>(25, 1), 5, 5);
    view.targetMask.assign(25, 1);
    view.sceneDepth.assign(25, 0.5f);
    view.targetDepth.assign(25, 0.5f);
    view.targetMask[12] = 0;
    view.partIds[12] = 0;  // no geometry projects into an authored opening

    auto input = MakeInput({1}, {view});
    input.minimumInteriorHolePixels = 1;
    const auto result = igi::EvaluateVisualIntegrity(input);

    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kPass);
    EXPECT_FALSE(HasRule(result, "silhouette-hole"));
}

TEST(VisualIntegrityTest, FailsWhenVisibleTargetPixelsEscapeProjectedGeometry) {
    auto view = MakeView({1, 1, 1, 1,
                          0, 0, 0, 0,
                          0, 0, 0, 0,
                          0, 0, 0, 0});
    view.targetMask.assign(16, 0);
    // The scene claims a target fragment where the target-only geometry pass
    // has no projected part. That cannot be evidence for this target.
    view.targetMask[10] = 1;

    const auto result = igi::EvaluateVisualIntegrity(MakeInput({1}, {view}));

    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kFail);
    EXPECT_TRUE(HasRule(result, "target-bounds"));
}

TEST(VisualIntegrityTest, ClassifiesFullyOccludedPartAsInconclusive) {
    auto view = MakeView({1, 1, 1, 1, 1, 1, 1, 1,
                          2, 2, 2, 2, 2, 2, 2, 2});
    for (size_t index = 8; index < view.targetMask.size(); ++index) {
        view.targetMask[index] = 0;
        view.sceneDepth[index] = 0.4f;  // a nearer non-target scene surface
        view.targetDepth[index] = 0.5f;
    }

    const auto result = igi::EvaluateVisualIntegrity(MakeInput({1, 2}, {view}));

    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kInconclusive);
    EXPECT_TRUE(HasRule(result, "occluded-part"));
    EXPECT_FALSE(HasRule(result, "part-coverage"));
}

TEST(VisualIntegrityTest, SerializesActionableFindingWithSeverity) {
    igi::VisualIntegrityResult result;
    result.status = igi::VisualIntegrityStatus::kFail;
    result.findings.push_back({"part-coverage", "12", 2, 0, 4,
                               "expected target part produced no visible fragments"});

    const std::string json = igi::VisualIntegrityJson(result);

    EXPECT_NE(json.find("\"rule\":\"part-coverage\""), std::string::npos);
    EXPECT_NE(json.find("\"severity\":\"error\""), std::string::npos);
    EXPECT_NE(json.find("\"schemaVersion\":1"), std::string::npos);
    EXPECT_NE(json.find("\"summary\":{"), std::string::npos);
    EXPECT_NE(json.find("\"expectedMinimum\":4"), std::string::npos);
}

TEST(VisualIntegrityTest, FindingReferencesTheDiagnosticOverlay) {
    auto view = MakeView({1, 1, 1, 1,
                          2, 2, 2, 2,
                          2, 2, 2, 2,
                          2, 2, 2, 2});
    view.name = "Ext_000";
    view.sourceFramePath = "views/Ext_000.png";
    view.overlayPath = "overlays/Ext_000-diagnostic.png";
    for (size_t index = 4; index < view.targetMask.size(); ++index)
        view.targetMask[index] = 0;

    const auto result = igi::EvaluateVisualIntegrity(MakeInput({1, 2}, {view}));
    ASSERT_EQ(result.status, igi::VisualIntegrityStatus::kFail);
    const auto finding = std::find_if(result.findings.begin(), result.findings.end(),
        [](const igi::VisualIntegrityFinding& value) {
            return value.rule == "part-coverage";
        });
    ASSERT_NE(finding, result.findings.end());
    EXPECT_EQ(finding->view, "Ext_000");
    EXPECT_EQ(finding->evidence, "overlays/Ext_000-diagnostic.png");
}

TEST(VisualIntegrityTest, MarksUnmeasurableProjectedPartAsInconclusive) {
    auto view = MakeView({1, 0, 0, 0,
                          0, 0, 0, 0,
                          0, 0, 0, 0,
                          0, 0, 0, 0});
    view.targetMask.assign(16, 0);
    view.targetMask[0] = 1;

    auto input = MakeInput({1}, {view});
    input.minimumPartPixels = 2;
    const auto result = igi::EvaluateVisualIntegrity(input);

    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kInconclusive);
    EXPECT_TRUE(HasRule(result, "insufficient-projection"));
}

TEST(VisualIntegrityTest, FailsTransparentPartWithoutDepthEvidence) {
    auto view = MakeView({1, 1, 1, 1, 1, 1, 1, 1,
                          1, 1, 1, 1, 1, 1, 1, 1});
    view.sceneDepth.assign(16, 1.0f);
    view.targetDepth.assign(16, 1.0f);

    igi::VisualIntegrityPart transparent_part;
    transparent_part.id = 1;
    transparent_part.alphaMode = 2;
    auto input = MakeInput({1}, {view});
    input.expectedParts = {transparent_part};
    const auto result = igi::EvaluateVisualIntegrity(input);

    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kFail);
    EXPECT_TRUE(HasRule(result, "transparency-evidence"));
}

TEST(VisualIntegrityTest, FailsWhenRuntimeTransformDoesNotMatchAuthoring) {
    auto view = MakeView({1, 1, 1, 1, 1, 1, 1, 1,
                          1, 1, 1, 1, 1, 1, 1, 1});
    view.transformMatchesAuthored = false;

    const auto result = igi::EvaluateVisualIntegrity(MakeInput({1}, {view}));

    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kFail);
    EXPECT_TRUE(HasRule(result, "transform-agreement"));
}

TEST(VisualIntegrityTest, FailsWhenRepeatedCameraFramesChangeTargetArea) {
    auto first = MakeView({1, 1, 1, 1, 1, 1, 1, 1,
                           1, 1, 1, 1, 1, 1, 1, 1});
    first.temporalGroup = 7;
    auto second = first;
    second.targetMask.assign(16, 0);
    second.targetMask[0] = 1;
    second.targetMask[1] = 1;
    second.targetMask[4] = 1;
    second.targetMask[5] = 1;

    auto input = MakeInput({1}, {first, second});
    input.requireTemporalEvidence = true;
    input.maximumTemporalAreaDelta = 0.10f;
    const auto result = igi::EvaluateVisualIntegrity(input);

    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kFail);
    EXPECT_TRUE(HasRule(result, "temporal-stability"));
}

TEST(VisualIntegrityTest, MarksMissingRepeatedFrameEvidenceAsInconclusive) {
    auto view = MakeView({1, 1, 1, 1, 1, 1, 1, 1,
                          1, 1, 1, 1, 1, 1, 1, 1});
    view.temporalGroup = 7;

    auto input = MakeInput({1}, {view});
    input.requireTemporalEvidence = true;
    const auto result = igi::EvaluateVisualIntegrity(input);

    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kInconclusive);
    EXPECT_TRUE(HasRule(result, "temporal-evidence"));
}

TEST(VisualIntegrityTest, AllowsCameraPredictedAreaChangeWhenCoverageIsStable) {
    auto first = MakeView({1, 1, 1, 1, 1, 1, 1, 1,
                           1, 1, 1, 1, 1, 1, 1, 1});
    first.temporalGroup = 8;
    auto second = first;
    // The camera projects only a quarter of the target in the second frame,
    // but every projected fragment remains visible.
    second.partIds.assign(16, 0);
    second.targetMask.assign(16, 0);
    second.partIds[0] = second.partIds[1] = second.partIds[4] = second.partIds[5] = 1;
    second.targetMask[0] = second.targetMask[1] = second.targetMask[4] = second.targetMask[5] = 1;

    auto input = MakeInput({1}, {first, second});
    input.requireTemporalEvidence = true;
    input.maximumTemporalAreaDelta = 0.10f;
    const auto result = igi::EvaluateVisualIntegrity(input);

    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kPass);
    EXPECT_FALSE(HasRule(result, "temporal-stability"));
}

TEST(VisualIntegrityTest, WatchtowerBaselineFixturePassesWithoutModelSpecificRules) {
    const auto result = igi::EvaluateVisualIntegrity(
        LoadEvidenceFixture("tests/fixtures/visual_integrity/watchtower-baseline.json"));

    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kPass);
    EXPECT_EQ(result.partsExpected, 2);
    EXPECT_EQ(result.partsObserved, 2);
}

TEST(VisualIntegrityTest, WinchHouseNegativeFixtureFailsForUnderCoveredGeometry) {
    const auto result = igi::EvaluateVisualIntegrity(
        LoadEvidenceFixture("tests/fixtures/visual_integrity/winchhouse-undercovered.json"));

    EXPECT_EQ(result.status, igi::VisualIntegrityStatus::kFail);
    EXPECT_TRUE(HasRule(result, "part-coverage"));
}
