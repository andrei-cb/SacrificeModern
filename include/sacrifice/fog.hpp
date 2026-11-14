#pragma once
#include "assets.hpp"

namespace sacrifice {
// The side's optional minimap discovery map (+316), 0x45d3a0..0x45dc90.
// Visibility comes from the creature work queue, independently of drawing the HUD.
class FogOfWar {
  public:
    FogOfWar(unsigned width, unsigned height);
    void reveal(const Terrain& terrain, Vec3 eye, float yaw, float half_angle,
                float concealment, float range);
    void age(); // 0x45d9c0, once per side period (32 match ticks).
    bool known(Vec3 point) const; // 0x472470, used by minimap ground input.
    std::span<const std::uint8_t> cells() const {
        return std::span(cells_).first(static_cast<std::size_t>(width_) * height_);
    }
    std::uint64_t revision() const { return revision_; }

  private:
    struct Entry {
        int offset{};
        unsigned current{}, previous{};
        float angle{};
    };
    struct Ring {
        float distance{}, inverse{};
        std::vector<Entry> entries;
    };
    unsigned width_{}, height_{};
    std::vector<std::uint8_t> cells_;
    std::vector<Ring> rings_;
    std::array<std::vector<float>, 2> horizons_;
    std::uint64_t revision_{};
};
} // namespace sacrifice
