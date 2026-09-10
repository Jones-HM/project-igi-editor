#include <gtest/gtest.h>
#include "debug_command_parser.h"

TEST(DebugCommandParserTest, ParseGotoCommand) {
    auto cmd = ParseDebugCommand("goto level=5 model=123_45_6");
    ASSERT_TRUE(cmd);
    EXPECT_EQ(cmd->type, "goto");
    EXPECT_EQ(cmd->level, 5);
    EXPECT_EQ(cmd->modelId, "123_45_6");
}

TEST(DebugCommandParserTest, ParseCaptureCommand) {
    auto cmd = ParseDebugCommand("capture-model level=10 model=999_12_1");
    ASSERT_TRUE(cmd);
    EXPECT_EQ(cmd->type, "capture-model");
    EXPECT_EQ(cmd->level, 10);
    EXPECT_EQ(cmd->modelId, "999_12_1");
}

TEST(DebugCommandParserTest, ParseInvalidCommand) {
    EXPECT_FALSE(ParseDebugCommand("invalid_cmd level=1 model=test"));
}

TEST(DebugCommandParserTest, ParseSplineTraceCommand) {
    auto cmd = ParseDebugCommand("capture-spline level=12 path=artifacts/e2e/spline-trace.json");
    ASSERT_TRUE(cmd);
    EXPECT_EQ(cmd->type, "capture-spline");
    EXPECT_EQ(cmd->level, 12);
    EXPECT_EQ(cmd->path, "artifacts/e2e/spline-trace.json");
}

TEST(DebugCommandParserTest, ParseCameraCommandWithWorldPosition) {
    auto cmd = ParseDebugCommand("set-camera level=12 x=112255136 y=-42044152 z=179558752");
    ASSERT_TRUE(cmd);
    EXPECT_EQ(cmd->type, "set-camera");
    EXPECT_EQ(cmd->level, 12);
    EXPECT_TRUE(cmd->has_pos);
    EXPECT_DOUBLE_EQ(cmd->x, 112255136.0);
    EXPECT_DOUBLE_EQ(cmd->y, -42044152.0);
    EXPECT_DOUBLE_EQ(cmd->z, 179558752.0);
}
