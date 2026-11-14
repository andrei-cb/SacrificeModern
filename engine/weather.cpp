#include "sacrifice/weather.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
// Original ATMOS parameter table, 0x160572c0 + kind * 136; serialized resource IDs/constants.
constexpr LandscapeEffects::Parameters table[]{
    {FourCC{0u}, 0x8001000u, 0.0799999982f, 0.0799999982f, {0.0f, 0.0f, -0.5f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 50.0f, 32, 768, 1.0f, 1.0f, 100.0f, 500.0f, {16.0f, 16.0f, 16.0f}},
    {FourCC{1936617335u}, 0x8011008u, 0.100000001f, 0.100000001f, {0.0f, 0.0f, -0.0500000007f}, {0.00999999978f, 0.00999999978f, 0.00499999989f}, {0.949999988f, 0.949999988f, 0.949999988f}, 50.0f, 255, 768, 0.0f, 0.0f, 100.0f, 500.0f, {16.0f, 16.0f, 16.0f}},
    {FourCC{1751214444u}, 0x8011008u, 0.100000001f, 0.100000001f, {0.0f, 0.0f, -0.5f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 50.0f, 255, 768, 0.5f, 1.0f, 100.0f, 500.0f, {16.0f, 16.0f, 16.0f}},
    {FourCC{1701667442u}, 0x8011008u, 0.100000001f, 0.100000001f, {0.0f, 0.0f, -0.0500000007f}, {0.00999999978f, 0.00999999978f, 0.00499999989f}, {0.949999988f, 0.949999988f, 0.949999988f}, 50.0f, 255, 768, 0.0f, 0.0f, 100.0f, 500.0f, {16.0f, 16.0f, 16.0f}},
    {FourCC{1634953311u}, 0x8011008u, 0.100000001f, 0.100000001f, {0.0f, 0.0f, -0.0500000007f}, {0.00999999978f, 0.00999999978f, 0.00499999989f}, {0.949999988f, 0.949999988f, 0.949999988f}, 50.0f, 255, 768, 0.0f, 0.0f, 100.0f, 500.0f, {16.0f, 16.0f, 16.0f}},
    {FourCC{1937206577u}, 0x8010400u, 0.100000001f, 0.100000001f, {0.0f, 0.0f, 0.0f}, {0.0299999993f, 0.0299999993f, 0.0299999993f}, {0.975000024f, 0.975000024f, 0.975000024f}, 25.0f, 255, 256, 0.0f, 0.0f, 100.0f, 500.0f, {16.0f, 16.0f, 16.0f}},
};
float approach(float a, float b, float step) { return a < b ? std::min(a + step, b) : std::max(a - step, b); }
}
const LandscapeEffects::Parameters& LandscapeEffects::parameters(unsigned kind) {
    if (kind < 1 || kind > 6) throw Error("Invalid landscape effect");
    return table[kind - 1];
}
void LandscapeEffects::set(unsigned kind, float intensity, float seconds, float duration, bool replace) {
    const auto& p = parameters(kind);
    if (std::ranges::find(order_, kind - 1) == order_.end()) order_.insert(order_.begin(), kind - 1);
    auto& e = effects[kind - 1];
    const float rate = seconds > 0 ? 1.0f / (seconds * 60) : 1;
    if (replace || e.target == 0) { e.target = intensity; e.rate = rate; e.duration = duration; }
    else {
        e.target = std::max(e.target, intensity);
        e.rate = std::max(e.rate, rate);
        if (e.duration == 0 || (duration != 0 && duration <= e.duration)) return;
        e.duration = duration;
    }
    e.drops.reserve(p.maximum);
}
void LandscapeEffects::advance(CrtRandom& random, int ticks, const std::function<void(int, FourCC, float)>& sound) {
    const auto uniform = [&] { return random.next() * .000030517578125; };
    // ATMOS inserts newly created effects at the head; all six are allocated lazily by the host.
    for (const unsigned n : order_) {
        auto& e = effects[n];
        const auto& p = table[n];
        if (e.duration > 0) {
            e.duration -= ticks * .016666668f;
            if (e.duration <= 0) { e.target = 0; e.rate = 1.0f / 300; }
        }
        e.intensity = approach(e.intensity, e.target, ticks * e.rate);
        while (e.drops.size() < static_cast<std::size_t>(p.maximum) && p.maximum * e.intensity > e.drops.size()) {
            const auto threshold = static_cast<float>(uniform());
            if (threshold >= e.intensity) break;
            Drop drop;
            drop.threshold = threshold;
            drop.size = static_cast<float>(uniform() * .40000004 + .800000011920929);
            drop.point = {static_cast<float>(uniform() * 100 - 50), static_cast<float>(uniform() * 100 - 50),
                          static_cast<float>(uniform() * 100 - 50)};
            drop.velocity = p.velocity;
            e.drops.insert(e.drops.begin(), drop);
        }
        if (n == 0 && sound) {
            const float gain = std::sqrt(e.intensity) * .5f;
            if (e.sound_active && gain == 0) { sound(0, FourCC::from_text("rain"), 0); e.sound_active = false; }
            else if (gain > 0) {
                if (!e.sound_active) { sound(2, FourCC::from_text("rain"), gain); e.sound_active = true; }
                sound(3, FourCC::from_text("rain"), gain);
            }
        }
        for (auto it = e.drops.begin(); it != e.drops.end();) {
            auto& d = *it;
            d.alpha = approach(d.alpha, e.intensity < d.threshold ? 0.0f : 1.0f, ticks * .016666668f);
            if (e.intensity < d.threshold && d.alpha <= 0) { it = e.drops.erase(it); continue; }
            if (n != 0) {
                d.velocity.x = static_cast<float>((d.velocity.x + uniform() * (2 * p.jitter.x) - p.jitter.x) * p.damping.x);
                d.velocity.y = static_cast<float>((d.velocity.y + uniform() * (2 * p.jitter.y) - p.jitter.y) * p.damping.y);
                d.velocity.z = static_cast<float>((d.velocity.z + uniform() * (2 * p.jitter.z) - p.jitter.z - p.velocity.z) * p.damping.z + p.velocity.z);
            }
            d.point.x += ticks * d.velocity.x;
            d.point.y += ticks * d.velocity.y;
            d.point.z += ticks * d.velocity.z;
            ++it;
        }
        if (n == 0) {
            const auto threshold = static_cast<int>((e.intensity - .2) * 512);
            if (e.flash != 0 || e.thunder.size() >= 4 || random.next() >= threshold)
                e.flash = std::max(0, e.flash - ticks);
            else { e.flash = 15; e.thunder.push_back(static_cast<int>((2.5 - 2 * e.intensity) * 60)); }
            if (!e.thunder.empty()) {
                e.thunder.front() -= ticks;
                if (e.thunder.front() <= 0) {
                    e.thunder.erase(e.thunder.begin());
                    if (sound) sound(1, FourCC::from_text("Xthn"), 1);
                }
            }
        }
    }
}
void LandscapeEffects::fog(float& start, float& end, std::uint32_t& color) const {
    const Vec3 original{static_cast<float>((color >> 16) & 255), static_cast<float>((color >> 8) & 255), static_cast<float>(color & 255)};
    float weight = 0, s = 0, e = 0;
    Vec3 c;
    for (const unsigned n : order_) {
        const auto& effect = effects[n];
        const auto& p = table[n];
        if (n == 0 && effect.flash) { weight += 1000; s += 2000000; e += 2000000; c.x += 255000; c.y += 255000; c.z += 255000; continue; }
        const float w = p.fog_weight * effect.intensity * effect.intensity, blend = effect.intensity * p.fog_blend;
        weight += w;
        s += (start + (p.fog_start - start) * blend) * w;
        e += (end + (p.fog_end - end) * blend) * w;
        c.x += (original.x + (p.fog_color.x - original.x) * blend) * w;
        c.y += (original.y + (p.fog_color.y - original.y) * blend) * w;
        c.z += (original.z + (p.fog_color.z - original.z) * blend) * w;
    }
    if (weight <= 0) return;
    start = s / weight; end = e / weight;
    const auto byte = [&](float v) { return static_cast<unsigned>(std::clamp(std::lround(v / weight), 0L, 255L)); };
    color = 0xff000000u | byte(c.x) << 16 | byte(c.y) << 8 | byte(c.z);
}
Vec3 LandscapeEffects::wrapped(Vec3 point, Vec3 eye, float radius) {
    const float scale = 65535.0f / (2 * radius), step = (2 * radius) * .00001525902189669642f;
    const auto wrap = [&](float v, float origin) {
        const auto cell = static_cast<std::uint16_t>(static_cast<std::int64_t>((v - origin + radius) * scale));
        return cell * step - radius + origin;
    };
    return {wrap(point.x, eye.x), wrap(point.y, eye.y), wrap(point.z, eye.z)};
}
} // namespace sacrifice
