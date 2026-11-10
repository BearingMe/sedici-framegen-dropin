#pragma once

#include <cstdint>

// The 11/11/10 reprojection packing used by the upstream implementation
// (references/sources/mobfgsr/resources/Packing.glsl). Kept as a pure mirror so
// the layout can be checked without a GPU and so both the shader and the tests
// agree on one definition.

namespace sedici::fg {

constexpr uint32_t kInvalidReprojection = 0xFFFFFFFFu;
constexpr uint32_t kDepthBits = 11;
constexpr uint32_t kXBits = 11;
constexpr uint32_t kYBits = 10;
constexpr uint32_t kMaxDepth = (1u << kDepthBits) - 1u;
constexpr int32_t kMinX = -(1 << (kXBits - 1));
constexpr int32_t kMinY = -(1 << (kYBits - 1));
constexpr int32_t kMaxX = (1 << (kXBits - 1)) - 1;
constexpr int32_t kMaxY = (1 << (kYBits - 1)) - 1;

struct Reprojection {
    float depth;
    int32_t sourceX;
    int32_t sourceY;
};

inline int32_t clamp_int(int32_t value, int32_t low, int32_t high) noexcept
{
    return value < low ? low : (value > high ? high : value);
}

// `depth` is the remapped, nearer-is-smaller value in [0, 1].
inline uint32_t pack_reprojection(float depth, int32_t sourceX, int32_t sourceY, int32_t targetX,
                                  int32_t targetY) noexcept
{
    if (!(depth > 0.0f)) depth = 0.0f;
    if (depth > 1.0f) depth = 1.0f;

    const uint32_t packedDepth = static_cast<uint32_t>(static_cast<float>(kMaxDepth) * depth);
    const int32_t relativeX = clamp_int(targetX - sourceX, kMinX, kMaxX);
    const int32_t relativeY = clamp_int(targetY - sourceY, kMinY, kMaxY);
    const uint32_t uRelativeX = static_cast<uint32_t>(relativeX - kMinX);
    const uint32_t uRelativeY = static_cast<uint32_t>(relativeY - kMinY);

    return (packedDepth << (32 - kDepthBits)) | (uRelativeX << kYBits) | uRelativeY;
}

inline float unpack_depth(uint32_t value) noexcept
{
    return static_cast<float>(value >> (32 - kDepthBits)) / static_cast<float>(kMaxDepth);
}

inline Reprojection unpack_reprojection(uint32_t value, int32_t targetX, int32_t targetY) noexcept
{
    const uint32_t relativeX = (value >> kYBits) & ((1u << kXBits) - 1u);
    const uint32_t relativeY = value & ((1u << kYBits) - 1u);
    Reprojection result{};
    result.depth = unpack_depth(value);
    result.sourceX = targetX - (static_cast<int32_t>(relativeX) + kMinX);
    result.sourceY = targetY - (static_cast<int32_t>(relativeY) + kMinY);
    return result;
}

} // namespace sedici::fg
