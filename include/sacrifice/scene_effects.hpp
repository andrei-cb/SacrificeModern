#pragma once
#include "match.hpp"
#include "renderer.hpp"
#include <functional>
namespace sacrifice {
struct EffectTexture {
    TextureHeader header;
    void* view{};
};
using EffectTextureResolver = std::function<EffectTexture(FourCC)>;
// Resolver ids outside TXTR names: a land cell's current map texture (LAND::GetMapTextureAt, cell in the
// low bits) and the environment's land edge texture (LAND::GetEdgeTexture).
inline constexpr std::uint32_t effect_land_tile = 0x80000000u, effect_land_edge = 0xc0000000u;
// The caller retains texture owners until the frame has been submitted.
class SoulActor;
// A soul's coloured point light (0x475c90 / 0x475ee0).
PointLight soul_light(const SoulActor& soul);
// Creature planar shadows (0x459bb0 hands SAXSINSTANCE the ground plane under a creature that is not
// fading and lacks state bit 4; SetupShadow skips models drawn below 0.3 of full detail): opaque black,
// two-sided, depth tested, in one batch.
void append_creature_shadows(Match& match, const Camera& camera, const RenderDetail& detail,
                             std::vector<ParticleBatch>& batches);
// With `remastered`, weather draws every simulated drop plus drawing-only copies (remaster_tuning.hpp).
void append_match_effects(Match& match, const Camera& camera, const EffectTextureResolver& texture_for,
                          std::vector<ParticleBatch>& batches, std::vector<PointLight>& lights,
                          bool remastered = false);
// Remastered Look: ground wetness and settled snow build up and fade from the match's weather intensities,
// ash and the lightning flash follow them directly. Drawing only; the match is read, never changed.
void update_weather_look(WeatherLook& look, const Match& match);
} // namespace sacrifice
