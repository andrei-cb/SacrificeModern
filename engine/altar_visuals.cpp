#include "sacrifice/creature_actor.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include "sacrifice/spell_data.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
FourCC cc(std::string_view text) { return FourCC::from_text(text); }
int integer(const SpellActor& spell, const char* key, int fallback) {
    return spell.data ? spell.data->integer(cc(key), fallback) : fallback;
}
float real(const SpellActor& spell, const char* key, float fallback) {
    return spell.data ? spell.data->real(cc(key), fallback) : fallback;
}
Vec3 control(CrtRandom& random) { // 0x44c330, range at 0x4ce5e0.
    return {static_cast<float>(random.next() * 30.0 / 32768 - 15),
            static_cast<float>(random.next() * 30.0 / 32768 - 15),
            static_cast<float>(random.next() * 20.0 / 32768 - 10)};
}
void frame(Match& match, FourCC texture, float& value, int ticks, float rate) {
    value += ticks * rate;
    const auto count = std::max(match.texture_frames ? match.texture_frames(texture) : 1u, 1u);
    if (value >= count)
        value = 0;
}
}
// 0x448dd0: fixed cloud above the altar, fading after the victim reference disappears.
int SpellActor::run_altar_cloud(Match& match, int ticks) {
    auto& cloud = altar_cloud;
    if (!stage) {
        motion.position.z += 50;
        cloud.enabled = true;
        cloud.texture = FourCC{static_cast<std::uint32_t>(integer(*this, "tcld", cc("clou").value))};
        cloud.phase = motion.position;
        if (!match.cloud_noise)
            match.cloud_noise = std::make_shared<CloudNoise>(match.visual_random);
        cloud.noise = match.cloud_noise;
        stage = 1;
    }
    const bool fading = stage == 240;
    if (stage == 1) {
        if (!effect_target)
            stage = 240;
        else {
            cloud.lightning_ticks -= ticks;
            if (cloud.lightning_ticks <= 0) {
                const auto angle = match.visual_random.next() * 6.2831802 / 32768;
                const auto radius = match.visual_random.next() * 30.0 / 32768;
                auto& bolt = launch_sacrifice_host(match, *this, Effect::altar_cloud_bolt);
                bolt.motion.position = {static_cast<float>(motion.position.x + std::sin(angle) * radius),
                                        static_cast<float>(motion.position.y + std::cos(angle) * radius),
                                        motion.position.z - 10};
                cloud.lightning_ticks = (match.visual_random.next() & 127) + 100;
                match.shake(motion.position, 12, 200);
            }
        }
    } else if (stage == 240 && cloud.opacity == 0) {
        state_flags |= removed_flag;
        return -2;
    }
    frame(match, cloud.texture, cloud.frame, ticks, .25f);
    // Reference loss switches stage first; the native fade begins on the next call.
    const auto target = fading ? 0.0f : 1.0f;
    cloud.opacity = target > cloud.opacity ? std::min(target, cloud.opacity + ticks / 120.0f)
                                          : std::max(target, cloud.opacity - ticks / 120.0f);
    const auto scale = (cloud.opacity + 1) * .5f;
    cloud.radii = {70 * scale, 70 * scale, 25 * scale};
    cloud.phase.x += ticks;
    cloud.phase.y += ticks;
    cloud.phase.z += ticks;
    return 1;
}
int SpellActor::run_altar_streamer(Match& match, int ticks) {
    auto& streamer = altar_streamer;
    if (!stage) {
        streamer.enabled = true;
        streamer.texture = FourCC{static_cast<std::uint32_t>(integer(*this, "tStx", cc("rift").value))};
        const auto blend = integer(*this, "iSad", 1);
        streamer.material = blend == 0 ? 0x0a011008 : blend == 1 ? 0x1a012008 : 0x1a010401;
        streamer.primitive.configure(integer(*this, "iSln", 45), real(*this, "fSrd", 1.5f));
        for (auto& point : streamer.controls)
            point = control(match.visual_random);
        stage = 1;
    }
    const Vec3* position = nullptr;
    if (const auto* target = effect_target ? effect_target->source_motion() : nullptr) {
        const auto previous = motion.position;
        point = target->position;
        streamer.phase += ticks * .033333335f;
        while (streamer.phase >= 1) {
            for (unsigned i = 0; i < 3; ++i)
                streamer.controls[i] = streamer.controls[i + 1];
            streamer.controls[3] = control(match.visual_random);
            streamer.phase -= 1;
        }
        // 0x423980 with the Catmull-Rom basis at 0x4caf40, then add the target's position.
        const auto coordinate = [&](auto member) {
            const auto a = streamer.controls[0].*member, b = streamer.controls[1].*member,
                       c = streamer.controls[2].*member, d = streamer.controls[3].*member;
            const double t = streamer.phase;
            return static_cast<float>((((-.5 * a + 1.5 * b - 1.5 * c + .5 * d) * t +
                                         a - 2.5 * b + 2 * c - .5 * d) * t + (-.5 * a + .5 * c)) * t + b);
        };
        motion.position = {point.x + coordinate(&Vec3::x), point.y + coordinate(&Vec3::y),
                           point.z + coordinate(&Vec3::z)};
        if (ticks)
            motion.velocity = {(motion.position.x - previous.x) / ticks, (motion.position.y - previous.y) / ticks,
                               (motion.position.z - previous.z) / ticks};
        position = &motion.position;
    }
    streamer.ticks += ticks;
    const auto count = match.texture_frames ? match.texture_frames(streamer.texture) : 1u;
    if (!streamer.primitive.add_point(position, streamer.ticks, count)) {
        state_flags |= removed_flag;
        return -2;
    }
    return 1;
}
SpellActor& SpellActor::launch_inert_lightning(Match& match, SpellActor& source, Vec3 from, Vec3 to,
                                               MatchObject* target, float deviation, float seconds, float hold,
                                               int strands, float width, unsigned flags) {
    auto& bolt = launch_sacrifice_host(match, source, Effect::altar_cloud_bolt);
    bolt.data = source.data;
    bolt.motion.position = from;
    bolt.point = to;
    bolt.effect_target = target;
    bolt.inert_bolt = {deviation, width, 1 / (std::max(seconds, 1e-3f) * 60), hold, strands, flags};
    return bolt;
}
void SpellActor::caster_sparks(Match& match, SpellActor& spell, float chance) {
    auto* caster = dynamic_cast<CreatureActor*>(spell.owner);
    if (!caster || !caster->source_motion())
        return;
    const auto& v = caster->motion.velocity;
    if (!(chance * 546.1333333f * std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z) > match.random().next()))
        return;
    const auto& body = *caster->source_motion();
    const auto random = [&](float low, float high) { return match.random().next() * (high - low) / 32768 + low; };
    Vec3 from{random(body.box_min.x, body.box_max.x) * .8f + body.position.x,
              random(body.box_min.y, body.box_max.y) * .8f + body.position.y, body.position.z};
    if (match.terrain)
        from.z = static_cast<float>(terrain_surface(*match.terrain, from.x, from.y));
    const float low = body.box_min.z, high = body.box_max.z * 1.5f;
    const Vec3 to{from.x, from.y, (high - low) * ((match.random().next() + 0x8000) / 65536.f) + from.z + low};
    launch_inert_lightning(match, spell, from, to, nullptr, .15f, .5f, 0, 1, .2f, 0);
}
int SpellActor::run_altar_cloud_bolt(Match& match, int ticks) {
    const auto parameters = [&] {
        LightningPrimitive::Parameters p;
        p.source = motion.position;
        p.target = point;
        p.step = .2f;
        p.deviation = inert_bolt.deviation;
        p.start = age;
        p.end = emission;
        p.width = inert_bolt.width;
        p.strands = inert_bolt.strands;
        return p;
    };
    if (!stage) {
        spell_target_point(point, effect_target);
        beam.enabled = true;
        beam.texture = FourCC{static_cast<std::uint32_t>(integer(*this, "tltn", cc("ltng").value))};
        beam.material = inert_bolt.flags & 8 ? 0x0a011008 : inert_bolt.flags & 4 ? 0x1a010401 : 0x1a012008;
        beam.hold = inert_bolt.hold;
        beam.primitive.set_params(parameters(), match.visual_random);
        if (inert_bolt.flags & 1)
            light = {true, motion.position, {1, 1, 1}, 1, 1, 100}; // 0x44CA20.
        stage = 1;
    }
    auto p = parameters(); // The native primitive uses the previous target point on this call.
    spell_target_point(point, effect_target);
    if (emission < 1) {
        emission = std::min(1.0f, emission + ticks * inert_bolt.rate);
        if (emission == 1) {
            if (data)
                sound_event(match, 4);
            if (inert_bolt.flags & 16) {
                auto& hit = launch_sacrifice_host(match, *this, Effect::spell_hit);
                hit.motion.position = p.target;
            }
        }
    } else {
        beam.hold -= ticks / 60.0f;
        if (beam.hold < 0) {
            age += ticks * inert_bolt.rate;
            if (age >= 1) {
                state_flags |= removed_flag;
                return -2;
            }
            light.intensity = std::max(0.f, light.intensity - .1f);
        }
    }
    if (light.enabled)
        light.position = {motion.position.x + (point.x - motion.position.x) * emission,
                          motion.position.y + (point.y - motion.position.y) * emission,
                          motion.position.z + (point.z - motion.position.z) * emission};
    p.start = age;
    p.end = emission;
    frame(match, beam.texture, beam.frame, ticks, .5f);
    beam.primitive.set_params(p, match.visual_random);
    return 1;
}
int SpellActor::run_spell_hit(Match& match, int ticks) {
    auto& p = particles(match);
    if (!stage) { // 0x438e30 / 0x438f10; the hit is a burst, with no gameplay damage.
        const auto blend = integer(*this, "iSP+", 1);
        particle_material(match, p, blend == 2 ? 0x1a010401 : blend ? 0x1a012008 : 0x0a011008,
                          FourCC{static_cast<std::uint32_t>(integer(*this, "tSPK", cc("sprk").value))});
        const auto size = real(*this, "fSPs", 2);
        Vec3 radius{size, size, size};
        if (const auto* target = effect_target ? effect_target->source_motion() : nullptr) {
            const auto& low = target->box_min;
            const auto& high = target->box_max;
            motion.position = {target->position.x + (low.x + high.x) * .5f,
                               target->position.y + (low.y + high.y) * .5f,
                               target->position.z + (low.z + high.z) * .5f};
            radius = {(high.x - low.x) * .5f, (high.y - low.y) * .5f, (high.z - low.z) * .5f};
        }
        const float life[2]{1.5f, real(*this, "fSPa", 0)},
                    bounds[6]{-radius.x, -radius.y, -radius.z, radius.x, radius.y, radius.z};
        p.set_property(140, life, bounds);
        p.set_position(motion.position);
        for (int i = std::max(integer(*this, "iSP#", 64), 0); i > 0; --i)
            p.emit(0, 1);
        stage = 1;
    }
    host.time += ticks;
    p.advance(host.time, 0);
    if (!p.alive()) {
        state_flags |= removed_flag;
        return -2;
    }
    return 1;
}
} // namespace sacrifice
