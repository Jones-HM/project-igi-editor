#include <gtest/gtest.h>
#include "../source/renderer/res_writer.h"
#include "../source/renderer/res_compiler.h"
#include "utils.h"
#include "support/temp_directory.h"
#include <filesystem>
#include <string>
#include <vector>

// ============================================================
//  RES_StreamAppend — streaming, atomic append tests.
//
//  Uses the small models archive (~5MB) so the test stays fast:
//     <IGIRoot>\missions\location0\level1\models\level1.res
//  Reads game data; needs IGI_GAME_PATH=D:\IGI1. Skips if absent.
// ============================================================

static std::string SrcResPath() {
    return Utils::GetIGIRootPath() +
           "\\missions\\location0\\level1\\models\\level1.res";
}

class ResStreamAppendTest : public ::testing::Test {
protected:
    test_support::TempDirectory temporary_directory_;
    std::filesystem::path src;
    std::filesystem::path out;

    void SetUp() override {
        std::string srcPath = SrcResPath();
        if (!std::filesystem::exists(srcPath))
            GTEST_SKIP() << "level1.res not found (set IGI_GAME_PATH): " << srcPath;

        src = temporary_directory_.path() / "src.res";
        out = temporary_directory_.path() / "out.res";
        std::filesystem::copy_file(srcPath, src,
            std::filesystem::copy_options::overwrite_existing);
    }

    void TearDown() override {
        std::error_code error;
        EXPECT_TRUE(temporary_directory_.Cleanup(error)) << error.message();
    }
};

TEST_F(ResStreamAppendTest, AppendsEntryAndPreservesSource) {
    RESFile original = RES_Parse(src.string());
    ASSERT_TRUE(original.valid) << original.error;
    ASSERT_GT(original.entries.size(), 0u);

    const std::vector<uint8_t> payload = {1, 2, 3, 4, 5};
    std::vector<RESEntry> add = { RESEntry{ "LOCAL:models/zzz_99_9.mef", payload } };

    std::string err;
    ASSERT_TRUE(RES_StreamAppend(src.string(), add, out.string(), err)) << err;

    // Re-parse the output: proves the skip=0 sentinel on the last BODY is correct.
    RESFile result = RES_Parse(out.string());
    ASSERT_TRUE(result.valid) << result.error;
    EXPECT_EQ(result.entries.size(), original.entries.size() + 1);

    // The appended entry is present with the exact bytes.
    bool foundNew = false;
    for (const auto& e : result.entries) {
        if (e.name == "LOCAL:models/zzz_99_9.mef") {
            foundNew = true;
            EXPECT_EQ(e.data, payload);
        }
    }
    EXPECT_TRUE(foundNew) << "appended entry missing from output";

    // The source file on disk is untouched.
    RESFile srcAfter = RES_Parse(src.string());
    ASSERT_TRUE(srcAfter.valid);
    EXPECT_EQ(srcAfter.entries.size(), original.entries.size());

    // Sample the first couple of original entries: streaming preserved name + bytes.
    const size_t sample = std::min<size_t>(2, original.entries.size());
    for (size_t i = 0; i < sample; ++i) {
        EXPECT_EQ(result.entries[i].name, original.entries[i].name);
        EXPECT_EQ(result.entries[i].data, original.entries[i].data)
            << "entry " << i << " data changed during streaming";
    }
}

TEST_F(ResStreamAppendTest, OutputReParsesValidWithLowLevelAppend) {
    // The low-level append adds exactly what it is told (membership is the caller's job).
    std::vector<RESEntry> add = {
        RESEntry{ "LOCAL:models/aaa_1.mef", {9, 8, 7} },
        RESEntry{ "LOCAL:models/bbb_2.mef", {6, 5} },
    };
    std::string err;
    ASSERT_TRUE(RES_StreamAppend(src.string(), add, out.string(), err)) << err;

    RESFile result = RES_Parse(out.string());
    ASSERT_TRUE(result.valid) << result.error;

    RESFile original = RES_Parse(src.string());
    EXPECT_EQ(result.entries.size(), original.entries.size() + 2);

    // Both appended entries present with exact bytes; last BODY sentinel valid.
    bool foundA = false, foundB = false;
    for (const auto& e : result.entries) {
        if (e.name == "LOCAL:models/aaa_1.mef") { foundA = true; EXPECT_EQ(e.data, (std::vector<uint8_t>{9,8,7})); }
        if (e.name == "LOCAL:models/bbb_2.mef") { foundB = true; EXPECT_EQ(e.data, (std::vector<uint8_t>{6,5})); }
    }
    EXPECT_TRUE(foundA);
    EXPECT_TRUE(foundB);
}

TEST_F(ResStreamAppendTest, EmptyNewEntriesIsAnError) {
    std::string err;
    EXPECT_FALSE(RES_StreamAppend(src.string(), {}, out.string(), err));
    EXPECT_FALSE(err.empty());
}

TEST_F(ResStreamAppendTest, ReplacesMatchingEntryAndAppendsMissingEntry) {
    const RESFile original = RES_Parse(src.string());
    ASSERT_TRUE(original.valid) << original.error;
    ASSERT_FALSE(original.entries.empty());

    const std::vector<uint8_t> replacement = {9, 8, 7};
    const std::vector<uint8_t> appended = {6, 5};
    const std::vector<RESEntry> wanted = {
        {original.entries.front().name, replacement},
        {"LOCAL:models/merge_new_1.mef", appended}
    };

    std::string err;
    ASSERT_TRUE(RES_StreamMerge(src.string(), wanted, out.string(), err)) << err;

    const RESFile result = RES_Parse(out.string());
    ASSERT_TRUE(result.valid) << result.error;
    ASSERT_EQ(result.entries.size(), original.entries.size() + 1);
    EXPECT_EQ(result.entries.front().name, original.entries.front().name);
    EXPECT_EQ(result.entries.front().data, replacement);
    EXPECT_EQ(result.entries.back().name, "LOCAL:models/merge_new_1.mef");
    EXPECT_EQ(result.entries.back().data, appended);
}
