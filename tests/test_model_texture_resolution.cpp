#include "../source/renderer/model_texture_resolution.h"
#include "../source/renderer/res_compiler.h"

#include <gtest/gtest.h>

#include "support/temp_directory.h"

TEST(ModelTextureResolution, KeepsSelectedVariantAndMaterialOrder) {
    const std::vector<ModelTextureSource> sources{
        {2, "001_02_1", {"face_b", "vest_b", "trousers_b"}},
        {1, "001_01_1", {"face_a", "vest_a"}},
        {9, "001_02_1", {"different_face", "different_vest"}}
    };

    const auto* chosen = FindExactTextureSource(sources, 2, "001_02_1");
    ASSERT_NE(chosen, nullptr);
    EXPECT_EQ(chosen->textures,
              (std::vector<std::string>{"face_b", "vest_b", "trousers_b"}));
    EXPECT_EQ(FindExactTextureSource(sources, 3, "001_02_1"), nullptr);
}

TEST(ModelTextureResolution, StripsOnlyRecognizedFormatSuffixes) {
    EXPECT_EQ(StripTextureFormatSuffix("004_13_1"), "004_13_1");
    EXPECT_EQ(StripTextureFormatSuffix("009_09_1_argb8888"), "009_09_1");
    EXPECT_EQ(StripTextureFormatSuffix("009_09_1_RGB565"), "009_09_1");
    EXPECT_EQ(StripTextureFormatSuffix("004_13_1_1"), "004_13_1_1");
    EXPECT_EQ(StripTextureFormatSuffix("004_13_1_custom"), "004_13_1_custom");
}

TEST(ModelTextureResolution, RequiredMetadataFailureCannotReportImportSuccess) {
    EXPECT_FALSE(IsCompleteModelImportMetadata({false, true}));
    EXPECT_FALSE(IsCompleteModelImportMetadata({true, false}));
    EXPECT_TRUE(IsCompleteModelImportMetadata({true, true}));
}

TEST(ModelTextureResolution, RejectsMaterialSlotsOutsideOrderedMapping) {
    EXPECT_TRUE(IsTextureMappingCompatible({0, 2, 12}, 13));
    EXPECT_FALSE(IsTextureMappingCompatible({0, 13}, 13));
    EXPECT_FALSE(IsTextureMappingCompatible({-1}, 13));
    EXPECT_FALSE(IsTextureMappingCompatible({0}, 0));
}

TEST(ModelTextureResolution, UntaggedSourceRequiresByteEquivalentBundles) {
    const std::vector<ModelSourceBundle> equivalent{
        {1, {"skin", "gear"}, {1, 2}, {{3}, {4}}},
        {8, {"skin", "gear"}, {1, 2}, {{3}, {4}}}
    };
    EXPECT_EQ(SelectUnambiguousModelSourceLevel(equivalent), 1);
    EXPECT_EQ(SelectUnambiguousModelSourceLevel({}), 0);

    auto divergentPixels = equivalent;
    divergentPixels[1].textureBytes[0] = {9};
    EXPECT_EQ(SelectUnambiguousModelSourceLevel(divergentPixels), 0);

    auto divergentMesh = equivalent;
    divergentMesh[1].meshBytes = {9};
    EXPECT_EQ(SelectUnambiguousModelSourceLevel(divergentMesh), 0);

    auto divergentMapping = equivalent;
    std::swap(divergentMapping[1].textureIds[0], divergentMapping[1].textureIds[1]);
    EXPECT_EQ(SelectUnambiguousModelSourceLevel(divergentMapping), 0);
}

TEST(ModelTextureResolution, DedupePickerRowsKeepsOneRowPerModelId) {
    const std::vector<ModelPickerEntry> entries{
        {"001_01_1", 1, "001_01_1  [Level 1]"},
        {"001_02_1", 1, "001_02_1  [Level 1]"},
        {"001_02_1", 2, "001_02_1  [Level 2]"},
        {"001_02_1", 8, "001_02_1  [Level 8]"},
        {"009_01_1", 0, "009_01_1"}
    };

    // The current level owns the model: the single kept row must carry it so
    // Enter applies without a redundant foreign re-import.
    const auto deduped = DedupeModelPickerEntries(entries, 2);
    ASSERT_EQ(deduped.size(), 3u);
    EXPECT_EQ(deduped[0].modelId, "001_01_1");
    EXPECT_EQ(deduped[0].sourceLevel, 1);
    EXPECT_EQ(deduped[0].label, "001_01_1");
    EXPECT_EQ(deduped[1].modelId, "001_02_1");
    EXPECT_EQ(deduped[1].sourceLevel, 2);
    EXPECT_EQ(deduped[1].label, "001_02_1");
    EXPECT_EQ(deduped[2].modelId, "009_01_1");
    EXPECT_EQ(deduped[2].sourceLevel, 0);

    // Otherwise the smallest owning level wins, keeping the import source
    // deterministic for models the destination level does not own.
    const auto dedupedForeign = DedupeModelPickerEntries(entries, 7);
    ASSERT_EQ(dedupedForeign.size(), 3u);
    EXPECT_EQ(dedupedForeign[1].modelId, "001_02_1");
    EXPECT_EQ(dedupedForeign[1].sourceLevel, 1);
}

TEST(ModelTextureResolution, ModelPickerClickSelectsTheDisplayedFilteredRow) {
    const std::vector<ModelPickerEntry> entries = DedupeModelPickerEntries(
        {
            {"001_01_1", 1, "001_01_1  [Level 1]"},
            {"001_02_1", 1, "001_02_1  [Level 1]"},
            {"001_02_1", 8, "001_02_1  [Level 8]"},
            {"009_01_1", 2, "009_01_1  [Level 2]"}
        },
        7);

    const auto filtered = FilterModelPickerEntries(entries, "001_02");
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].modelId, "001_02_1");
    EXPECT_EQ(filtered[0].sourceLevel, 1);

    // The model panel is the rightmost 280px; its rows begin below the 50px
    // header. A click on the first filtered row must select that exact entry.
    EXPECT_EQ(ModelPickerSelectionAt(entries, "001_02", 1100, 55,
                                      1280, 720, 0, 16), 0);
    EXPECT_EQ(ModelPickerSelectionAt(entries, "001_02", 900, 55,
                                      1280, 720, 0, 16), -1);
    EXPECT_EQ(ModelPickerSelectionAt(entries, "001_02", 1100, 710,
                                      1280, 720, 0, 16), -1);
}

TEST(ModelTextureResolution, ArrowNavigationChangesPreviewedModel) {
    // One row per unique model ID: every Up/Down step must change the
    // previewed model instead of only changing a duplicated source label.
    const std::vector<ModelPickerEntry> entries = DedupeModelPickerEntries(
        {
            {"001_01_1", 1, "001_01_1  [Level 1]"},
            {"001_02_1", 1, "001_02_1  [Level 1]"},
            {"001_02_1", 2, "001_02_1  [Level 2]"},
            {"009_01_1", 2, "009_01_1  [Level 2]"}
        },
        7);
    const auto filtered = FilterModelPickerEntries(entries, "");
    ASSERT_EQ(filtered.size(), 3u);

    int selected = 0;
    selected = MoveModelPickerSelection(selected, (int)filtered.size(), 1);
    EXPECT_EQ(filtered[selected].modelId, "001_02_1");
    selected = MoveModelPickerSelection(selected, (int)filtered.size(), 1);
    EXPECT_EQ(filtered[selected].modelId, "009_01_1");
    selected = MoveModelPickerSelection(selected, (int)filtered.size(), -1);
    EXPECT_EQ(filtered[selected].modelId, "001_02_1");
}

TEST(ModelTextureResolution, EnterAppliesTheSelectedModel) {
    const std::vector<ModelPickerEntry> entries = DedupeModelPickerEntries(
        {
            {"001_01_1", 1, "001_01_1  [Level 1]"},
            {"001_02_1", 1, "001_02_1  [Level 1]"},
            {"001_02_1", 2, "001_02_1  [Level 2]"},
            {"009_01_1", 2, "009_01_1  [Level 2]"}
        },
        7);

    // Typing a model ID narrows the list; Enter commits the highlighted row
    // with its deterministic source level for the import provenance.
    const auto commit = ResolveModelPickerCommit(entries, "001_02", 0);
    ASSERT_TRUE(commit.has_value());
    EXPECT_EQ(commit->modelId, "001_02_1");
    EXPECT_EQ(commit->sourceLevel, 1);

    EXPECT_FALSE(ResolveModelPickerCommit(entries, "001_02", 1).has_value());
    EXPECT_FALSE(ResolveModelPickerCommit(entries, "", 7).has_value());
    EXPECT_FALSE(ResolveModelPickerCommit(entries, "no-such-model", 0).has_value());
}

TEST(ModelTextureResolution, ExplicitForeignSourceReplacesExistingModelId) {
    EXPECT_TRUE(ModelSourceRequiresImport(true, false, 1, 0));
    EXPECT_FALSE(ModelSourceRequiresImport(true, true, 1, 1));
    EXPECT_TRUE(ModelSourceRequiresImport(true, true, 1, 7));
    EXPECT_FALSE(ModelSourceRequiresImport(true, true, 1, 0));
    EXPECT_TRUE(ModelSourceRequiresImport(false, false, 1, 7));
    EXPECT_FALSE(ModelSourceRequiresImport(false, false, 1, 0));
}

TEST(ModelTextureResolution, ArrowNavigationChangesSelectionWithinFilteredList) {
    EXPECT_EQ(MoveModelPickerSelection(0, 3, 1), 1);
    EXPECT_EQ(MoveModelPickerSelection(1, 3, -1), 0);
    EXPECT_EQ(MoveModelPickerSelection(2, 3, 1), 2);
    EXPECT_EQ(MoveModelPickerSelection(0, 3, -1), 0);
    EXPECT_EQ(MoveModelPickerSelection(0, 0, 1), 0);
}

namespace {

std::array<ModelSourceArchiveIndex, 2> IndexSyntheticBundle(
    const std::string& levelRes, const std::string& commonRes) {
    std::array<ModelSourceArchiveIndex, 2> bundle;
    const std::string paths[2] = {levelRes, commonRes};
    for (size_t slot = 0; slot < 2; ++slot) {
        bundle[slot].resPath = paths[slot];
        std::string error;
        EXPECT_TRUE(RES_BuildIndex(paths[slot], bundle[slot].entries, error)) << error;
    }
    return bundle;
}

std::vector<uint8_t> ReadFromDisk(const std::string& resPath, const ResEntryInfo& info) {
    return RES_ReadEntry(resPath, info);
}

}  // namespace

TEST(ModelTextureResolution, LevelMissingFallsBackToCommonArchive) {
    test_support::TempDirectory temp;
    const std::string levelRes = (temp.path() / "level.res").string();
    const std::string commonRes = (temp.path() / "common.res").string();

    // The texture lives only in the shared common archive; the level archive
    // holds an unrelated entry, mirroring 001_02_1's polluted level reses.
    std::string error;
    ASSERT_TRUE(RES_WriteEntries(
        {RESEntry{"LOCAL:textures/unrelated.tex", {7, 7, 7}}}, levelRes, error)) << error;
    const std::vector<uint8_t> commonBytes = {1, 2, 3, 4};
    ASSERT_TRUE(RES_WriteEntries(
        {RESEntry{"LOCAL:textures/shared_tex.tex", commonBytes}}, commonRes, error)) << error;

    const auto bundle = IndexSyntheticBundle(levelRes, commonRes);
    EXPECT_EQ(FindModelSourceEntry(bundle, "shared_tex", true, ReadFromDisk), commonBytes);
    EXPECT_TRUE(FindModelSourceEntry(bundle, "missing_tex", true, ReadFromDisk).empty());
}

TEST(ModelTextureResolution, LevelArchiveWinsOverCommonArchive) {
    test_support::TempDirectory temp;
    const std::string levelRes = (temp.path() / "level.res").string();
    const std::string commonRes = (temp.path() / "common.res").string();

    // Same entry name in both archives with divergent bytes: the source bundle
    // is the level archive, so its bytes must win.
    const std::vector<uint8_t> levelMesh = {9, 8, 7};
    std::string error;
    ASSERT_TRUE(RES_WriteEntries(
        {RESEntry{"LOCAL:models/dup_model.mef", levelMesh}}, levelRes, error)) << error;
    ASSERT_TRUE(RES_WriteEntries(
        {RESEntry{"LOCAL:models/dup_model.mef", {1, 1, 1}}}, commonRes, error)) << error;

    const auto bundle = IndexSyntheticBundle(levelRes, commonRes);
    EXPECT_EQ(FindModelSourceEntry(bundle, "dup_model", false, ReadFromDisk), levelMesh);
}
