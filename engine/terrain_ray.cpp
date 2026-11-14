#include "sacrifice/motion.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
std::uint32_t integer(double x) {
    if (!std::isfinite(x) || x >= 9223372036854775808.0 || x < -9223372036854775808.0)
        return 0;
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(x));
}
} // namespace
float terrain_intercept(const Terrain& t, Vec3 from, Vec3 delta, Vec3& point, float maximum_squared,
                        bool solid_only) {
    if (!(from.x >= 0 && from.y >= 0 && from.x < t.width * 10.0 && from.y < t.height * 10.0) ||
        (delta.x == 0 && delta.y == 0))
        return 0;
    int x = static_cast<int>(integer(from.x * 0.1)), y = static_cast<int>(integer(from.y * 0.1));
    const int sx = delta.x < 0 ? -1 : delta.x > 0 ? 1 : 0, sy = delta.y < 0 ? -1 : delta.y > 0 ? 1 : 0;
    const float ix = delta.x == 0 ? 1.0e14f
                                  : static_cast<float>(10.0 / std::abs(static_cast<double>(delta.x))),
                iy = delta.y == 0 ? 1.0e14f
                                  : static_cast<float>(10.0 / std::abs(static_cast<double>(delta.y)));
    float tx = delta.x == 0 ? 1.0e14f : static_cast<float>(((x + (sx > 0)) * 10.0 - from.x) / delta.x),
          ty = delta.y == 0 ? 1.0e14f : static_cast<float>(((y + (sy > 0)) * 10.0 - from.y) / delta.y);
    float z = static_cast<float>(from.z * 10.0), dz = static_cast<float>(delta.z * 10.0);
    std::uint32_t raw = integer(z);
    unsigned nx = 0, ny = 0;
    while (x >= 0 && y >= 0 && x < static_cast<int>(t.width) && y < static_cast<int>(t.height)) {
        const auto before = raw;
        const float dt = tx >= ty ? (++ny, ty) : (++nx, tx);
        z = static_cast<float>(static_cast<double>(dt) * dz + z);
        raw = integer(z);
        const std::array<std::uint16_t, 4> h{t.raw(x, y), t.raw(x + 1, y), t.raw(x, y + 1),
                                             t.raw(x + 1, y + 1)};
        const unsigned holes = (h[0] >> 15) + (h[1] >> 15) + (h[2] >> 15) + (h[3] >> 15);
        const auto [low, high] = std::minmax({unsigned(h[0] & 0x7fff), unsigned(h[1] & 0x7fff),
                                              unsigned(h[2] & 0x7fff), unsigned(h[3] & 0x7fff)});
        if ((!solid_only || holes <= 1) && (raw >= low || before >= low) && (raw <= high || before <= high)) {
            const bool main = t.main_diagonal(x, y);
            for (unsigned triangle = 0; triangle < 2; ++triangle) {
                TerrainPlane plane;
                const float px = static_cast<float>(x * 10 +
                                                    (main ? (triangle ? 2.5 : 7.5) : (triangle ? 7.5 : 2.5))),
                            py = static_cast<float>(y * 10 +
                                                    (main ? (triangle ? 7.5 : 2.5) : (triangle ? 7.5 : 2.5)));
                terrain_plane(t, px, py, plane);
                if (solid_only && !plane.solid)
                    continue;
                const auto& n = plane.normal;
                const double divisor = static_cast<double>(n.z) * delta.z +
                                       static_cast<double>(delta.y) * n.y +
                                       static_cast<double>(delta.x) * n.x;
                float hit = static_cast<float>(divisor);
                if (divisor != 0) {
                    const double fraction = (plane.distance - (static_cast<double>(n.z) * from.z +
                                                               static_cast<double>(from.y) * n.y +
                                                               static_cast<double>(from.x) * n.x)) /
                                            hit;
                    hit = static_cast<float>(fraction);
                    if (fraction > 0)
                        point = {static_cast<float>(static_cast<double>(hit) * delta.x + from.x),
                                 static_cast<float>(static_cast<double>(hit) * delta.y + from.y),
                                 static_cast<float>(static_cast<double>(hit) * delta.z + from.z)};
                }
                if (!(hit > 0))
                    continue;
                const float fx = static_cast<float>(static_cast<double>(point.x) - x * 10.0),
                            fy = static_cast<float>(static_cast<double>(point.y) - y * 10.0);
                if (fx < 0 || fy < 0 || fx >= 10 || fy >= 10)
                    continue;
                const unsigned side = main ? (fy <= fx ? 0 : 1) : (10.0 - fy >= fx ? 0 : 1);
                if (triangle == side)
                    return hit;
            }
        }
        if (maximum_squared != 0 && static_cast<double>(nx * nx + ny * ny) * 100 > maximum_squared)
            return 0;
        if (tx >= ty) {
            y += sy;
            tx = static_cast<float>(static_cast<double>(tx) - ty);
            ty = iy;
        } else {
            x += sx;
            ty = static_cast<float>(static_cast<double>(ty) - tx);
            tx = ix;
        }
    }
    return 0;
}
} // namespace sacrifice
