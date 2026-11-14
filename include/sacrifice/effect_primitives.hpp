#pragma once
#include "motion.hpp"
#include <span>

namespace sacrifice {
struct CrtRandom;
// game3d LIGHTNING / CROSSLINE: random piecewise curves sampled into three-finned strands.
class LightningPrimitive {
  public:
    struct Parameters {
        Vec3 source, target;
        float step{.083333336f}, deviation{2}, start{}, end{}, width{.5f};
        int segments{16}, strands{1};
    };
    struct Point {
        Vec3 position;
        float width{};
        bool connected{};
        std::uint8_t alpha{255};
    };
    void set_params(const Parameters& parameters, CrtRandom& random);
    Mesh mesh(const TextureHeader& texture, unsigned frame) const;
    const std::vector<Point>& points() const { return points_; }

  private:
    void new_curve(CrtRandom& random);
    bool initialized_{};
    float phase_{};
    std::array<std::array<Vec3, 8>, 4> curves_{};
    std::vector<Point> points_;
};
Mesh crossline_mesh(std::span<const LightningPrimitive::Point> points, const TextureHeader& texture,
                    unsigned frame, bool continuous);
// MUZZLE::Draw (game3d 0x16023db0): a flat square `width` across, centred on `at` and facing `target`.
Mesh muzzle_mesh(Vec3 at, Vec3 target, float width, const TextureHeader& texture, unsigned frame);

// STREAMER keeps at most one point per tick and interpolates skipped ticks before aging the tail.
class StreamerPrimitive {
  public:
    void configure(int lifetime, float width);
    bool add_point(const Vec3* position, int tick, unsigned texture_frames);
    Mesh mesh(const TextureHeader& texture) const;
    const std::vector<LightningPrimitive::Point>& points() const { return points_; }
    unsigned frame() const { return frame_; }

  private:
    int lifetime_{45}, last_tick_{};
    float width_{1.5f}, frame_fraction_{};
    unsigned frame_{};
    bool previous_active_{};
    Vec3 previous_;
    std::vector<LightningPrimitive::Point> points_;
    std::vector<int> stamps_;
};

// CLOUDPRIM's shared, periodic 32^3 scalar lattice (0x160437b0 / 0x16043800).
class CloudNoise {
  public:
    explicit CloudNoise(CrtRandom& random);
    double sample(Vec3 position) const;

  private:
    std::array<std::uint16_t, 32768> values_{};
};
Mesh cloud_mesh(const CloudNoise& noise, Vec3 radii, Vec3 phase, float opacity, Vec3 origin, Vec3 eye,
                const TextureHeader& texture, unsigned frame, float detail);
// RINGEXP material 0: five rows, with a transparent centre and interpolated quarter-height radii.
Mesh ring_exp_mesh(float radius, float height, float end_radius, Vec3 origin, const Orientation& orientation,
                   const TextureHeader& texture, unsigned frame);
} // namespace sacrifice
