#include "sacrifice/creature_actor.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_registry.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
SpellActor& child(Match& match, MatchObject& parent, SpellActor::Effect effect,
                  std::shared_ptr<const SpellData> data) {
    return static_cast<SpellActor&>(match.create(
        MatchObject::spell, [] { return std::make_unique<SpellActor>(); }, &parent,
        [&](MatchObject& object) {
            auto& spell = static_cast<SpellActor&>(object);
            spell.effect = effect;
            spell.data = data;
        }));
}
FourCC texture(const SpellData* data, const char* key, const char* fallback) {
    const auto value = FourCC::from_text(fallback).value;
    return FourCC{static_cast<std::uint32_t>(data ? data->integer(FourCC::from_text(key), value) : value)};
}
}
// 0x441500 -> 0x441310: copy the current integer animation pose and orientation, but sample an older
// position from Speedup's circular buffer. The coincident first echo is suppressed, even when moving.
int SpellActor::run_speedup_echo(Match&, int ticks) {
    auto* source = dynamic_cast<SpellActor*>(effect_target);
    auto* actor = source ? dynamic_cast<CreatureActor*>(source->effect_target) : nullptr;
    if (actor && echo.length && source->speedup.trail.size() == echo.length) {
        const auto clip = actor->clip_handle();
        const auto frame = static_cast<unsigned>(actor->frame());
        if (clip != echo.clip || frame != echo.frame) {
            echo.clip = clip;
            echo.frame = frame;
            ++echo.revision;
        }
        const auto index = (source->speedup.head + echo.length - echo.lag) % echo.length;
        motion.orientation = actor->motion.orientation;
        motion.yaw = actor->motion.yaw;
        motion.position = source->speedup.trail[index];
        const auto& p = actor->motion.position;
        echo.hidden = motion.position.x == p.x && motion.position.y == p.y && motion.position.z == p.z;
        if (!echo.hidden || !stage)
            echo.alpha = static_cast<int>((echo.length - echo.lag) *
                static_cast<std::int64_t>(data ? data->integer(FourCC::from_text("ialf"), 255) : 255) / echo.length);
        if (!stage)
            echo.revision = 1;
        stage = 1;
    } else {
        echo.alpha -= 10 * ticks;
        if (echo.alpha <= 0 || !stage) {
            state_flags |= removed_flag;
            return -2;
        }
    }
    cast_alpha = static_cast<float>(echo.alpha) / 255;
    return 1;
}
void SpellActor::launch_teleport_portals(Match& match) {
    for (const auto& position : {motion.position, point}) {
        auto& portal = child(match, *this, Effect::teleport_portal, data);
        portal.motion.position = position;
        portal.teleport_radius = teleport_radius;
        portal.cylinder.enabled = true;
        portal.cylinder.texture = texture(data.get(), "tcyl", "safr");
        portal.run(match, 0);
    }
}
// 0x442800/0x4428d0: a five-second box emitter and a half-second sweep at the unit's old/new position.
SpellActor& SpellActor::launch_teleport_burst(Match& match, CreatureActor& actor, bool arrival) {
    const auto* definition = match.spell_registry ? match.spell_registry->find(FourCC::from_text(arrival ? "@tou" : "@tin")) : nullptr;
    auto& burst = child(match, actor, Effect::teleport_burst, definition ? definition->spell : nullptr);
    const auto& m = actor.motion;
    const float scale = arrival ? 1.5f : 1;
    const Vec3 half{(m.box_max.x - m.box_min.x) * .5f * scale,
                    (m.box_max.y - m.box_min.y) * .5f * scale,
                    (m.box_max.z - m.box_min.z) * .5f * scale};
    burst.motion.position = {m.position.x + (m.box_min.x + m.box_max.x) * .5f,
                              m.position.y + (m.box_min.y + m.box_max.y) * .5f,
                              m.position.z + (m.box_min.z + m.box_max.z) * .5f};
    auto& p = burst.particles(match);
    burst.particle_material(match, p, 0x1a012008, texture(burst.data.get(), "tcld", "glo2"));
    const float size = static_cast<float>(std::clamp(std::sqrt(static_cast<double>(half.x) * half.x +
                          static_cast<double>(half.y) * half.y + static_cast<double>(half.z) * half.z) * .3, .25, 2.0)),
                life = 2.5f;
    p.set_property(129, &size, &life);
    const float low[3]{-half.x, -half.y, -half.z}, high[3]{half.x, half.y, half.z};
    p.set_property(2, low, high);
    p.set_position(burst.motion.position, &burst.motion.position);
    burst.host.rate = 1;
    burst.stage = 1;
    auto& sweep = child(match, actor, Effect::teleport_sweep, burst.data);
    sweep.motion.position = burst.motion.position;
    sweep.motion.orientation = motion_orientation(0, arrival ? -1.570795f : 1.570795f);
    sweep.model_scale = (m.box_max.z - m.box_min.z) * .5f;
    sweep.had_target = arrival;
    auto& q = sweep.particles(match);
    sweep.particle_material(match, q, 0x1a012008, texture(sweep.data.get(), "trng", "brng"));
    const float radius = static_cast<float>(std::hypot(static_cast<double>(m.box_min.x) - m.box_max.x,
                                                      static_cast<double>(m.box_min.y) - m.box_max.y) * .5),
                seconds = 1, end_size = .2f;
    q.set_property(134, &radius, &seconds);
    q.set_property(6, &end_size, &end_size);
    const ParticleSystem::Frame frame{sweep.motion.orientation, sweep.motion.position};
    q.set_frame(frame, &frame);
    sweep.run(match, 1); // The native sweep starts with one tick, before relocation continues.
    return burst;
}
int SpellActor::run_teleport_visual(Match& match, int ticks) {
    if (effect == Effect::teleport_portal) {
        age += ticks / 60.0f;
        if (age >= 1) {
            state_flags |= removed_flag;
            return -2;
        }
        // Native 0x43a2e0 stores its start radius twice, ignoring the end-radius argument. Use that
        // supplied endpoint here so the Teleport cylinder can expand instead of being zero-width.
        cylinder.radius = teleport_radius * age;
        cylinder.alpha = static_cast<int>((1 - age) * 255) / 255.0f;
        cylinder.frame += ticks * .5f;
        const auto frames = std::max(match.texture_frames ? match.texture_frames(cylinder.texture) : 1u, 1u);
        if (cylinder.frame >= frames)
            cylinder.frame = 0;
        motion.yaw = .10471967f * age;
        motion.orientation = motion_orientation(motion.yaw);
        return 1;
    }
    auto& p = particles(match);
    host.time += ticks;
    if (effect == Effect::teleport_burst) {
        p.advance(host.time, host.rate);
        host.rate -= ticks / 150.0f;
        if (host.rate <= 0 && !p.alive()) {
            state_flags |= removed_flag;
            return -2;
        }
    } else {
        if (stage != 240) {
            age += 1 / 30.0f; // 0x43a370 advances once per call, independently of ticks.
            if (age > 1)
                age = 1, stage = 240;
        }
        const float z = ((had_target ? 1 - age : age) * 2 - 1) * model_scale;
        const float point[3]{0, 0, z};
        p.set_property(2, point, point);
        p.advance(host.time, stage == 240 ? 0 : .5f);
        if (stage == 240 && !p.alive()) {
            state_flags |= removed_flag;
            return -2;
        }
    }
    return 1;
}
// CYLINDER::Draw (0x16024080): sixteen two-vertex strips, alternating U at each edge.
Mesh SpellActor::cylinder_mesh(const TextureHeader& texture) const {
    Mesh mesh;
    if (cylinder.radius <= 0)
        return mesh;
    const auto frames = std::max(texture.frames, 1u), columns = std::max(texture.width / std::max(texture.frame_width, 1u), 1u);
    const auto frame = static_cast<unsigned>(cylinder.frame) % frames;
    const float su = static_cast<float>(texture.frame_width) / std::max(texture.width, 1u),
                sv = static_cast<float>(texture.frame_height) / std::max(texture.height, 1u),
                u = (frame % columns) * su, v = (frame / columns) * sv;
    const auto color = 0xffffffu | static_cast<std::uint32_t>(std::clamp(cylinder.alpha * 255, 0.0f, 255.0f)) << 24;
    for (unsigned row = 0; row < 2; ++row)
        for (unsigned j = 0; j < 16; ++j) {
            const auto angle = -6.28318 * j / 16;
            const float x = static_cast<float>(std::sin(angle) * cylinder.radius),
                        y = static_cast<float>(std::cos(angle) * cylinder.radius),
                        z = row ? cylinder.top : cylinder.bottom;
            const auto& o = motion.orientation;
            mesh.vertices.push_back({{motion.position.x + o[0] * x + o[3] * y + o[6] * z,
                                      motion.position.z + o[2] * x + o[5] * y + o[8] * z,
                                      motion.position.y + o[1] * x + o[4] * y + o[7] * z}, {},
                                     {u + (j & 1 ? su : 0), v + (row ? sv : 0)}, color});
        }
    for (unsigned j = 0; j < 16; ++j) {
        const auto next = (j + 1) % 16;
        mesh.indices.insert(mesh.indices.end(), {j, next, j + 16, next, next + 16, j + 16});
    }
    return mesh;
}
} // namespace sacrifice
