#include "../source/renderer/model_texture_resolution.h"

#include <gtest/gtest.h>

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
