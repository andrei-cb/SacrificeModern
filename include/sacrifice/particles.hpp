#pragma once
#include "motion.hpp"
#include <deque>
#include <functional>

namespace sacrifice {
struct CrtRandom;
// game3d.dll PARTICLESYSTEM (0x1601e1c0..0x16021220): the effect host behind every spell, building and
// creature particle visual. Native times are game ticks (the callers pass the summed ticks to SetParams);
// lifetimes are seconds × 60. The native draws share the game's MSVCRT rand(); here a separate visual
// stream keeps the simulation independent of what is drawn.
class ParticleSystem {
  public:
    // A 48-byte frame: row-vector rotation then position (object +404/+440 layout, property 1).
    struct Frame {
        Orientation rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
        Vec3 position;
    };
    struct Particle {
        Vec3 position, velocity, acceleration;  // +8, +20, +32 per tick.
        std::array<float, 4> color{}, color_rate{}; // +44 R, G, B, A in 0..255; +60 per tick.
        float frame{}, frame_rate{};            // +76, +80.
        float size{}, size_rate{};              // +84 half extent, +88.
        float detail{};                         // +92 in 0.15..1, skipped above the view quality.
        float life{};                           // +96 remaining ticks.
        float lag{};                            // +100 ticks between its birth and the last run.
    };
    // Optional LAND access for flags 0x1000 (spawn on the ground) and 0x10000 (ground collision).
    using Ground = std::function<float(const Vec3& point, Vec3* normal)>;

    explicit ParticleSystem(CrtRandom& random, int capacity = 128);
    void clear(int capacity);                                   // 0x1601e230.
    void set_material(std::uint32_t flags, FourCC texture);     // PRIMITIVE::SetMaterial slot 0.
    void set_frames(unsigned frames) { frames_ = frames ? frames : 1; } // TEXTURE frame count (vtable +36).
    void set_ground(Ground ground) { ground_ = std::move(ground); }
    void set_scar(std::function<void(Vec3)> scar) { scar_ = std::move(scar); }
    // 0x1601e5b0. Pointers follow the native (a3, a4) pair; null where the caller passes none.
    void set_property(int id, const float* a, const float* b);
    void set_position(const Vec3& position, const Vec3* previous = nullptr); // Property 0.
    void set_frame(const Frame& frame, const Frame* previous = nullptr);     // Property 1.
    void set_flags(std::uint32_t set, std::uint32_t clear) { flags = ~clear & (set | flags); } // Property 11.
    // SetParams (0x1601e490): emit rate × elapsed particles up to `time`, then run or prune.
    void advance(float time, float rate);
    void run();                                   // RunParticles 0x1601fb40.
    void fade(float minimum, float maximum);      // Property 10: every particle ends within the range.
    void enumerate(const std::function<int(Particle&)>& visitor); // Return 2 to delete a particle.
    bool alive() const { return !particles_.empty(); } // IsStillAlive.
    std::size_t count() const { return particles_.size(); }
    const std::deque<Particle>& particles() const { return particles_; }
    std::uint32_t material_flags() const { return material_flags_; }
    FourCC texture() const { return texture_; }
    unsigned frames() const { return frames_; }
    float time() const { return time_; }
    float elapsed() const { return time_ - run_time_; }
    Particle* emit(float time, float fraction, const float* angles = nullptr); // NewParticle 0x160205c0.

    std::uint32_t flags{};      // +252.
    int cycles{1};              // +256: texture frame cycles over a life.
    std::array<float, 2> life{2, 2}, size{1, 1}, size_end{}; // +264, +272, +280.
    std::array<float, 4> color_start{}, color_end{};         // +288, +304.
    Vec3 velocity_min, velocity_max;                          // +320, +332.
    Vec3 acceleration_min, acceleration_max;                  // +344, +356.
    Frame previous, current;                                  // +368, +416.
    Vec3 box_min, box_max;                                    // +464, +476.
    Vec3 swarm_scale;                                         // +488.
    float swarm_125{}, swarm_126{}, swarm_jitter{};           // +500, +504, +508.
    Vec3 swarm_center;                                        // +516.

  private:
    float uniform(float low, float high);
    void reset_properties(bool identity);
    void bound();
    void prune();
    CrtRandom* random_;
    std::deque<Particle> particles_; // Head is the newest, as the native list inserts at its head.
    int capacity_{};
    std::uint32_t material_flags_{};
    FourCC texture_;
    unsigned frames_{1};
    float time_{}, rate_{}, run_time_{};
    bool positioned_{};
    Ground ground_;
    std::function<void(Vec3)> scar_;
};
} // namespace sacrifice
