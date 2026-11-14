#pragma once
#include "assets.hpp"
#include <array>
#include <functional>

namespace sacrifice {
// LAND/ATMOS runtime (0x16043eb0..0x16044f40). These effects use the shared CRT random stream.
class LandscapeEffects {
  public:
    struct Parameters {
        FourCC texture;
        std::uint32_t material{};
        float size{}, rain_size{};
        Vec3 velocity, jitter, damping;
        float radius{};
        int alpha{}, maximum{};
        float fog_weight{}, fog_blend{}, fog_start{}, fog_end{};
        Vec3 fog_color;
    };
    struct Drop { Vec3 point, velocity; float threshold{}, alpha{}, size{}; };
    struct Effect {
        float intensity{}, target{}, rate{}, duration{};
        std::vector<Drop> drops;
        bool sound_active{};
        int flash{};
        std::vector<int> thunder;
    };
    std::array<Effect, 6> effects;
    static const Parameters& parameters(unsigned kind);
    void set(unsigned kind, float intensity, float seconds, float duration = 0, bool replace = true);
    void advance(CrtRandom& random, int ticks, const std::function<void(int, FourCC, float)>& sound = {});
    void fog(float& start, float& end, std::uint32_t& color) const;
  private:
    std::vector<unsigned> order_; // ATMOS list head: most recently allocated effect first.
  public:
    static Vec3 wrapped(Vec3 point, Vec3 eye, float radius);
};
} // namespace sacrifice
