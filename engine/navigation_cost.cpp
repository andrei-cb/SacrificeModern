#include "sacrifice/navigation.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
int integer(double value) {
    if (!std::isfinite(value) || value >= 9223372036854775808.0 || value < -9223372036854775808.0)
        return 0;
    return std::bit_cast<int>(static_cast<unsigned>(static_cast<std::int64_t>(value)));
}
int add(int a, int b) {
    return std::bit_cast<int>(static_cast<unsigned>(a) + static_cast<unsigned>(b));
}
int multiply(int a, int b) {
    return std::bit_cast<int>(static_cast<unsigned>(a) * static_cast<unsigned>(b));
}
int negate(int value) {
    return std::bit_cast<int>(0u - static_cast<unsigned>(value));
}
} // namespace
void Navigation::stamp(unsigned shape, Vec3 position, std::span<const float> p, int amount, bool base) {
    const unsigned required = shape == 2 ? 5 : shape == 4 ? 3 : (shape == 1 || shape == 3) ? 2 : 0;
    if (p.size() < required)
        throw Error("Truncated navigation stamp parameters");
    const int map = base ? 2 : static_cast<int>(next_);
    if (!base)
        dirty_ = add(dirty_, 1);
    if (shape == 1) {
        const double hx = p[0] * 0.5, hy = p[1] * 0.5;
        const int x = integer((position.x - hx) * 0.1), y = integer((position.y - hy) * 0.1);
        const int w = add(add(1, negate(integer((position.x + hx) * -0.1))), negate(x)),
                  h = add(add(1, negate(integer((position.y + hy) * -0.1))), negate(y));
        if (w < 0 || h < 0)
            throw Error("Invalid native navigation rectangle dimensions");
        for (int row = 0; row < h; ++row)
            for (int column = w - 1; column >= 0; --column)
                write(map, static_cast<std::int64_t>(y + row) * 256 + x + column, amount, true);
    } else if (shape == 2) {
        const int x = std::max(integer((static_cast<double>(position.x) + p[0]) * 0.1), 0),
                  y = std::max(integer((static_cast<double>(position.y) + p[1]) * 0.1), 0);
        const int w = add(add(1, negate(integer((static_cast<double>(position.x) + p[3]) * -0.1))),
                          negate(x)),
                  h = add(add(1, negate(integer((static_cast<double>(position.y) + p[4]) * -0.1))),
                          negate(y));
        if (w <= 0 || h <= 0)
            return;
        const std::int64_t origin = static_cast<std::int64_t>(y) * 256 + x;
        for (int column = 0; column < w; ++column) {
            write(map, origin + column, amount >> 1, false);
            write(map, origin + static_cast<std::int64_t>(h - 1) * 256 + column, amount >> 1, false);
        }
        for (int row = h - 1; row >= 0; --row) {
            write(map, origin + static_cast<std::int64_t>(row) * 256, amount >> 1, false);
            write(map, origin + static_cast<std::int64_t>(row) * 256 + w - 1, amount >> 1, false);
        }
        for (int row = 1; row < h - 1; ++row)
            for (int column = w - 2; column > 0; --column)
                write(map, origin + static_cast<std::int64_t>(row) * 256 + column, amount, true);
    } else if (shape == 3) {
        const int inner = integer(p[0] * 0.1), outer = integer(p[1] * 0.1);
        if (outer < 0)
            return;
        const float inner_squared = static_cast<float>(multiply(inner, inner)),
                    outer_squared = static_cast<float>(multiply(outer, outer));
        const int cx = integer((position.x + 5.0) * 0.1), cy = integer((position.y + 5.0) * 0.1);
        for (int dy = std::max(-outer, -cy); dy <= std::min(outer, 255 - cy); ++dy)
            for (int dx = std::max(-outer, -cx); dx <= std::min(outer, 255 - cx); ++dx) {
                const float dy_squared = static_cast<float>(multiply(dy, dy));
                const double squared = static_cast<double>(multiply(dx, dx)) + dy_squared;
                const float stored_squared = static_cast<float>(squared);
                if (squared > outer_squared)
                    continue;
                int cost = amount;
                if (stored_squared > inner_squared) {
                    const float scale =
                        static_cast<float>(static_cast<double>(amount) / (outer_squared - inner_squared));
                    cost = integer((static_cast<double>(outer_squared) - stored_squared) * scale);
                }
                write(map, static_cast<std::int64_t>(cy + dy) * 256 + cx + dx, cost, true);
            }
    } else if (shape == 4) {
        const int radius = std::min(integer(p[2] * 0.1 + 0.5), 15);
        if (radius < 0)
            return;
        const int squared_radius = multiply(radius, radius);
        std::array<std::uint8_t, 1024> kernel{};
        unsigned index = 0;
        for (int y = -radius; y <= radius; ++y)
            for (int x = -radius; x <= radius; ++x) {
                const int squared = add(multiply(x, x), multiply(y, y));
                if (squared < squared_radius) {
                    const int factor =
                        256 - std::bit_cast<int>(static_cast<unsigned>(squared) << 8) / squared_radius;
                    kernel[index] =
                        static_cast<std::uint8_t>(static_cast<std::uint16_t>(multiply(amount, factor)) >> 8);
                }
                ++index;
            }
        int x = integer((position.x + 5.0) * 6553.6), y = integer((position.y + 5.0) * 6553.6);
        double length = std::sqrt(static_cast<double>(p[0]) * p[0] + static_cast<double>(p[1]) * p[1]);
        const int dx = integer(65536.0 / length * p[0]), dy = integer(65536.0 / length * p[1]);
        while (length >= -5) {
            const unsigned cell =
                ((static_cast<unsigned>(x) >> 16) & 255) | (static_cast<unsigned>(y) >> 8 & 0xff00);
            index = 0;
            for (int row = radius; row >= -radius; --row)
                for (int column = -radius; column <= radius; ++column) {
                    const auto at = static_cast<std::uint16_t>(cell + row * 256 + column);
                    write(map, at, kernel[index++], true);
                }
            length -= 10;
            x = add(x, dx);
            y = add(y, dy);
        }
    }
}
} // namespace sacrifice
