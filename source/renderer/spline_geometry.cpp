#include "spline_geometry.h"

#include <cmath>

namespace spline_geometry {
namespace {

bool IsFinite(const glm::dvec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

} // namespace

glm::dvec3 SampleSegment(const SplineSegment& segment, double t) {
    if (segment.linear) return segment.p0 + t * (segment.p1 - segment.p0);

    const double t2 = t * t;
    const double t3 = t2 * t;
    return (2.0 * t3 - 3.0 * t2 + 1.0) * segment.p0
         + (t3 - 2.0 * t2 + t) * segment.tangent0
         + (-2.0 * t3 + 3.0 * t2) * segment.p1
         + (t3 - t2) * segment.tangent1;
}

std::optional<SplineTile> MakeAxisAlignedTile(
    glm::dvec3 begin,
    glm::dvec3 end,
    int longitudinalAxis,
    double localMin,
    double localLength,
    double crossScale) {
    if (!IsFinite(begin) || !IsFinite(end) ||
        longitudinalAxis < 0 || longitudinalAxis > 2 ||
        !std::isfinite(localMin) || !std::isfinite(localLength) ||
        !std::isfinite(crossScale) || localLength <= 1e-9 || crossScale <= 0.0) {
        return std::nullopt;
    }

    const glm::dvec3 delta = end - begin;
    const double span = glm::length(delta);
    if (!std::isfinite(span) || span <= 1e-9) return std::nullopt;

    const glm::dvec3 forward = delta / span;
    const glm::dvec3 reference = std::abs(forward.z) < 0.99
        ? glm::dvec3(0.0, 0.0, 1.0)
        : glm::dvec3(0.0, 1.0, 0.0);
    const glm::dvec3 right = glm::normalize(glm::cross(reference, forward));
    const glm::dvec3 up = glm::cross(forward, right);
    const double sx = span / localLength;

    glm::dmat4 model(1.0);
    const glm::dvec4 longitudinal(forward * sx, 0.0);
    const glm::dvec4 crossRight(right * crossScale, 0.0);
    const glm::dvec4 crossUp(up * crossScale, 0.0);
    if (longitudinalAxis == 0) {
        model[0] = longitudinal;
        model[1] = crossRight;
        model[2] = crossUp;
    } else if (longitudinalAxis == 1) {
        model[0] = crossRight;
        model[1] = longitudinal;
        model[2] = crossUp;
    } else {
        model[0] = crossRight;
        model[1] = crossUp;
        model[2] = longitudinal;
    }
    model[3] = glm::dvec4(begin - forward * (sx * localMin), 1.0);
    return SplineTile{begin, end, model};
}

std::optional<SplineTile> MakeXAlignedTile(
    glm::dvec3 begin,
    glm::dvec3 end,
    double localMinX,
    double localLength,
    double crossScale) {
    return MakeAxisAlignedTile(begin, end, 0, localMinX, localLength, crossScale);
}

} // namespace spline_geometry
