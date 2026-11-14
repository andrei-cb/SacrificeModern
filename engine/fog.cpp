#include "sacrifice/fog.hpp"
#include <cmath>

namespace sacrifice {
namespace {
constexpr double tau = 6.28318;
constexpr float maximum_cells = 30.5f; // 0x45d6d0(300): (300 + 5) / 10.
}
FogOfWar::FogOfWar(unsigned width, unsigned height)
    : width_(width), height_(height), cells_(static_cast<std::size_t>(width) * (height + 2)) {
    // 0x45d490: one annulus per rounded ten-unit radius, sorted around the observer.
    // Each sample inherits the nearest angular sample of the previous ring. Two horizon
    // buffers alternate, with indices quantized at the outermost ring's angular density.
    rings_.reserve(30);
    for (int i = 0; i < 30; ++i) {
        Ring ring;
        const float radius = i + .5f;
        ring.distance = radius * 10;
        ring.inverse = 1 / ring.distance;
        for (int y = -i; y <= i; ++y)
            for (int x = -i; x <= i; ++x) {
                const double distance = std::sqrt(static_cast<double>(x * x + y * y));
                if (distance > radius || distance <= radius - 1)
                    continue;
                float angle = static_cast<float>(std::atan2(static_cast<double>(-x), static_cast<double>(y)));
                if (angle < 0)
                    angle = static_cast<float>(angle + tau);
                ring.entries.push_back({x + y * static_cast<int>(width),
                                        static_cast<unsigned>(maximum_cells * angle), 0, angle});
            }
        std::ranges::sort(ring.entries, {}, &Entry::angle);
        if (!rings_.empty())
            for (auto& entry : ring.entries) {
                float best = 100000;
                for (const auto& previous : rings_.back().entries) {
                    const float difference = entry.angle - previous.angle;
                    if (difference * difference < best) {
                        best = difference * difference;
                        entry.previous = previous.current;
                    }
                }
            }
        rings_.push_back(std::move(ring));
    }
    for (auto& horizon : horizons_)
        horizon.resize(static_cast<std::size_t>((maximum_cells + maximum_cells) * 3.14159));
}
void FogOfWar::reveal(const Terrain& terrain, Vec3 eye, float yaw, float half_angle,
                      float concealment, float range) {
    if (terrain.width != width_ || terrain.height != height_ || terrain.heights.empty() ||
        !std::isfinite(range) || range <= 0 || !std::isfinite(yaw))
        return;
    const int x = static_cast<int>((eye.x + 5.0) * .1), y = static_cast<int>((eye.y + 5.0) * .1);
    const int centre = x + y * static_cast<int>(width_);
    if (centre < 0 || centre >= static_cast<int>(width_ * height_))
        return;
    half_angle = std::clamp(half_angle, .1f, 3.1415f);
    range = std::min(range, maximum_cells * 10);
    double direction = yaw;
    while (direction < 0)
        direction += tau;
    while (direction >= tau)
        direction -= tau;
    for (auto& horizon : horizons_)
        std::ranges::fill(horizon, 10000000.0f);
    horizons_[0][0] = horizons_[1][0] = -10000000000.0f;
    eye.z += 2;
    unsigned current = 0, previous = 1;
    const int count = std::min(static_cast<int>(range * .1), static_cast<int>(rings_.size()));
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        const auto& ring = rings_[i];
        const int size = static_cast<int>(ring.entries.size());
        int first = 0, samples = size;
        const float angle = ((range * ring.inverse - 1) / concealment + 1) * half_angle;
        if (angle < 3.14159f) {
            double low = direction - angle, high = direction + angle;
            while (low < 0)
                low += tau, high += tau;
            if (high < tau * 2) {
                first = std::max(0, static_cast<int>(size * low / tau - 1));
                samples = std::min(size, static_cast<int>(size * high / tau + 1) - first);
            }
        }
        for (int n = 0; n < samples; ++n) {
            const auto& entry = ring.entries[(first + n) % size];
            const int cell = centre + entry.offset;
            // Native uses a linear index (including its first padded sample), not separate x/y
            // clipping. Retain that border behavior without reading past the height buffer.
            if (cell < 0 || cell > static_cast<int>(width_ * height_))
                continue;
            const auto raw = terrain.heights[static_cast<std::size_t>(cell) % terrain.heights.size()];
            float horizon = horizons_[previous][entry.previous];
            const float slope = (static_cast<float>((raw & 0x7fff) * .1) + 2 - eye.z) * ring.inverse;
            if ((raw & 0x8000) || slope >= horizon) {
                changed |= (cells_[cell] & 15) != 15;
                cells_[cell] |= 15;
                if (!(raw & 0x8000))
                    horizon = std::max(horizon, slope - 2 * ring.inverse);
            }
            horizons_[current][entry.current] = horizon;
        }
        std::swap(current, previous);
    }
    if (changed)
        ++revision_;
}
void FogOfWar::age() {
    bool changed = false;
    for (auto& cell : cells_) {
        // 0x45d6d0's byte LUT. Zero stays undiscovered; the low nibble fades to 1.
        // Bits 4/5 advance as the original's discovery history, even though the terrain
        // texture currently consumes only the low nibble.
        unsigned value = (cell + 15) & 15;
        if (value == 15)
            value = 0;
        else if (!value)
            value = 1;
        if (cell)
            value |= (cell & 0x30) ? 0x30 : (cell + 16) & 0x30;
        changed |= value != cell;
        cell = static_cast<std::uint8_t>(value);
    }
    if (changed)
        ++revision_;
}
bool FogOfWar::known(Vec3 point) const {
    const int x = static_cast<int>((point.x + .5) * .1), y = static_cast<int>((point.y + .5) * .1);
    return x >= 0 && y >= 0 && x < static_cast<int>(width_) && y < static_cast<int>(height_) &&
           (cells_[static_cast<std::size_t>(y) * width_ + x] & 15);
}
} // namespace sacrifice
