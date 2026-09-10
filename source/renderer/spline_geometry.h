#pragma once

#include <glm/glm.hpp>

#include <optional>

namespace spline_geometry {

struct SplineTile {
    glm::dvec3 begin;
    glm::dvec3 end;
    glm::dmat4 model;
};

struct SplineSegment {
    glm::dvec3 p0;
    glm::dvec3 p1;
    glm::dvec3 tangent0;
    glm::dvec3 tangent1;
    bool linear = false;
};

glm::dvec3 SampleSegment(const SplineSegment& segment, double t);

std::optional<SplineTile> MakeXAlignedTile(
    glm::dvec3 begin,
    glm::dvec3 end,
    double localMinX,
    double localLength,
    double crossScale);

// Build a tile for a model whose longitudinal local axis is 0 (X), 1 (Y), or
// 2 (Z), retaining the orientation of the model's two cross-section axes.
std::optional<SplineTile> MakeAxisAlignedTile(
    glm::dvec3 begin,
    glm::dvec3 end,
    int longitudinalAxis,
    double localMin,
    double localLength,
    double crossScale);

} // namespace spline_geometry
