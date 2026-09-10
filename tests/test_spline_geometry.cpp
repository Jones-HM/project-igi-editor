#include "../source/renderer/spline_geometry.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

using spline_geometry::MakeXAlignedTile;
using spline_geometry::SampleSegment;
using spline_geometry::SplineSegment;

TEST(SplineGeometry, PlacesBothEdgesAtLargeAuthoredEndpoints) {
    const glm::dvec3 a(112255136, -42044152, 179558752);
    const glm::dvec3 b = a + glm::dvec3(0.25, -1000, 0.5);

    const auto tile = MakeXAlignedTile(a, b, -2.0, 4.0, 40.96);

    ASSERT_TRUE(tile.has_value());
    const auto first = glm::dvec3(tile->model * glm::dvec4(-2, 0, 0, 1));
    const auto last = glm::dvec3(tile->model * glm::dvec4(2, 0, 0, 1));
    EXPECT_LT(glm::length(first - a), 1e-6);
    EXPECT_LT(glm::length(last - b), 1e-6);
}

TEST(SplineGeometry, PlacesYAlignedModelEdgesAtLargeAuthoredEndpoints) {
    const glm::dvec3 a(112255136, -42044152, 179558752);
    const glm::dvec3 b = a + glm::dvec3(0.25, -1000, 0.5);

    const auto tile = spline_geometry::MakeAxisAlignedTile(a, b, 1, -50.0, 100.0, 40.96);

    ASSERT_TRUE(tile.has_value());
    const auto first = glm::dvec3(tile->model * glm::dvec4(0, -50, 0, 1));
    const auto last = glm::dvec3(tile->model * glm::dvec4(0, 50, 0, 1));
    EXPECT_LT(glm::length(first - a), 1e-6);
    EXPECT_LT(glm::length(last - b), 1e-6);
}

TEST(SplineGeometry, RejectsInvalidLongitudinalAxis) {
    EXPECT_FALSE(spline_geometry::MakeAxisAlignedTile({0, 0, 0}, {1, 0, 0}, -1, 0, 1, 1));
    EXPECT_FALSE(spline_geometry::MakeAxisAlignedTile({0, 0, 0}, {1, 0, 0}, 3, 0, 1, 1));
}

TEST(SplineGeometry, RejectsZeroSpan) {
    EXPECT_FALSE(MakeXAlignedTile({0, 0, 0}, {0, 0, 0}, 0, 1, 40.96));
}

TEST(SplineGeometry, RejectsNonFiniteTileInputs) {
    EXPECT_FALSE(MakeXAlignedTile({0, 0, 0}, {1, 0, 0},
                                  std::numeric_limits<double>::quiet_NaN(), 1, 1));
    EXPECT_FALSE(MakeXAlignedTile({0, 0, 0}, {1, 0, 0}, 0,
                                  std::numeric_limits<double>::infinity(), 1));
    EXPECT_FALSE(MakeXAlignedTile({0, 0, 0}, {1, 0, 0}, 0, 1, 0));
}

TEST(SplineGeometry, LinearSegmentsStayOnAuthoredChord) {
    const SplineSegment segment{
        {10, 20, 30}, {50, 60, 70},
        {-1000, 500, 200}, {900, -400, -100}, true};

    EXPECT_EQ(SampleSegment(segment, 0.0), segment.p0);
    EXPECT_EQ(SampleSegment(segment, 0.5), (glm::dvec3{30, 40, 50}));
    EXPECT_EQ(SampleSegment(segment, 1.0), segment.p1);
}

TEST(SplineGeometry, CurvedSegmentsPreserveEndpointsAndUseTangents) {
    const SplineSegment segment{
        {0, 0, 0}, {10, 0, 0},
        {0, 20, 0}, {0, 30, 0}, false};

    EXPECT_EQ(SampleSegment(segment, 0.0), segment.p0);
    EXPECT_EQ(SampleSegment(segment, 1.0), segment.p1);
    EXPECT_GT(SampleSegment(segment, 0.25).y, 0.0);
    EXPECT_LT(SampleSegment(segment, 0.75).y, 0.0);
}

TEST(SplineGeometry, HandlesVerticalAndShortTilesWithoutInvalidFrame) {
    const auto vertical = MakeXAlignedTile({1, 2, 3}, {1, 2, 3.001}, 0, 0.002, 40.96);
    ASSERT_TRUE(vertical.has_value());
    for (int column = 0; column < 3; ++column) {
        const glm::dvec3 axis(vertical->model[column]);
        EXPECT_TRUE(std::isfinite(axis.x));
        EXPECT_TRUE(std::isfinite(axis.y));
        EXPECT_TRUE(std::isfinite(axis.z));
    }
}
