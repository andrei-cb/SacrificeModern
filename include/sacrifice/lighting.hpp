#pragma once
#include "assets.hpp"

namespace sacrifice {
// LIGHT3D: base colour times intensity, inner/outer ranges. The D3D7 point-light path uses
// the outer range and attenuation 1 + 3*d*d/(outer*outer); the inner range is retained by the host.
struct PointLight {
    bool enabled{};
    Vec3 position, color;
    float intensity{}, inner{}, outer{};
    // LAND's selected-light adjustment (0x1603b080), without modifying the owning effect.
    PointLight for_terrain(float ground) const {
        auto result = *this;
        result.position.z = std::max(position.z, ground + outer * .5f);
        result.outer *= 1.25f;
        result.intensity *= 1.25f;
        return result;
    }
};
} // namespace sacrifice
