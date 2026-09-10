#include <gtest/gtest.h>
#include "../source/renderer/dat_writer.h"
#include "utils.h"
#include <filesystem>
#include <algorithm>

static std::string SrcDat() {
    return Utils::GetIGIRootPath() + "\\missions\\location0\\level1\\level1.dat";
}

TEST(DatWriterTest, RoundTripPreservesStructure) {
    if (!std::filesystem::exists(SrcDat())) GTEST_SKIP() << "level1.dat not available";
    DATFile a = DAT_Parse(SrcDat());
    ASSERT_TRUE(a.valid) << a.error;
    auto tmp = (std::filesystem::temp_directory_path() / "igi_dat_rt.dat").string();
    std::string err;
    ASSERT_TRUE(DAT_WriteNative(a, tmp, err)) << err;
    DATFile b = DAT_Parse(tmp);
    ASSERT_TRUE(b.valid) << b.error;
    EXPECT_EQ(b.models.size(), a.models.size());
    EXPECT_EQ(b.allTextures.size(), a.allTextures.size());
    EXPECT_EQ(b.declaredModelCount, a.declaredModelCount);
    EXPECT_EQ(b.declaredTextureCount, a.declaredTextureCount);
    // Spot-check first model + its textures preserved exactly.
    ASSERT_FALSE(a.models.empty());
    EXPECT_EQ(b.models.front().modelName, a.models.front().modelName);
    EXPECT_EQ(b.models.front().textures, a.models.front().textures);
    std::filesystem::remove(tmp);
}

TEST(DatWriterTest, AddModelThenRoundTrip) {
    if (!std::filesystem::exists(SrcDat())) GTEST_SKIP() << "level1.dat not available";
    DATFile a = DAT_Parse(SrcDat());
    ASSERT_TRUE(a.valid);
    bool present = false;
    DAT_AddModel(a, "999_77_1", {"019_03_1", "zzz_xy_9"}, present);
    EXPECT_FALSE(present);
    auto tmp = (std::filesystem::temp_directory_path() / "igi_dat_add.dat").string();
    std::string err;
    ASSERT_TRUE(DAT_WriteNative(a, tmp, err)) << err;
    DATFile b = DAT_Parse(tmp);
    ASSERT_TRUE(b.valid);
    // New model present with its 2 textures; counts bumped.
    bool found = false;
    for (auto& m : b.models) if (m.modelName == "999_77_1") { found = true; EXPECT_EQ(m.textures.size(), 2u); }
    EXPECT_TRUE(found);
    EXPECT_EQ(b.declaredModelCount, a.declaredModelCount); // a was already bumped by DAT_AddModel
    EXPECT_NE(std::find(b.allTextures.begin(), b.allTextures.end(), "zzz_xy_9"), b.allTextures.end());
    std::filesystem::remove(tmp);
}

TEST(DatWriterTest, UpsertModelReplacesConflictingMappingInPlace) {
    DATFile dat;
    dat.valid = true;
    dat.models = {{"keep_before", {"old_a"}}, {"target", {"wrong"}}, {"keep_after", {"old_b"}}};
    dat.allTextures = {"old_a", "wrong", "old_b"};

    EXPECT_TRUE(DAT_UpsertModel(dat, "target", {"right_a", "right_b"}));
    ASSERT_EQ(dat.models.size(), 3u);
    EXPECT_EQ(dat.models[0].modelName, "keep_before");
    EXPECT_EQ(dat.models[1].modelName, "target");
    EXPECT_EQ(dat.models[1].textures, (std::vector<std::string>{"right_a", "right_b"}));
    EXPECT_EQ(dat.models[2].modelName, "keep_after");
    EXPECT_NE(std::find(dat.allTextures.begin(), dat.allTextures.end(), "right_b"), dat.allTextures.end());
    EXPECT_FALSE(DAT_UpsertModel(dat, "target", {"right_a", "right_b"}));
}
