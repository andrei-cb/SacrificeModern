#include "sacrifice/spell_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/spell_data.hpp"
#include "sacrifice/building_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
Vec3 normalize(Vec3 v) {
    const double length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return length ? Vec3{static_cast<float>(v.x / length), static_cast<float>(v.y / length),
                         static_cast<float>(v.z / length)} : v;
}
Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
// 0x16012c90 / 0x16012370: face the eye, blending the preferred up towards world Y near parallel.
Orientation facing(Vec3 from, Vec3 to, Vec3 up) {
    const auto forward = normalize({to.x - from.x, to.y - from.y, to.z - from.z});
    const double dot = forward.x * up.x + forward.y * up.y + forward.z * up.z;
    const double blend = dot * dot <= .75 ? 0 : std::pow((dot * dot - .75) * 4, 2);
    up = {static_cast<float>(up.x * (1 - blend)), static_cast<float>(up.y + (1 - up.y) * blend),
          static_cast<float>(up.z * (1 - blend))};
    const auto right = normalize(cross(forward, up));
    up = cross(right, forward);
    return {right.x, right.y, right.z, forward.x, forward.y, forward.z, up.x, up.y, up.z};
}
}
// 0x44a5b0: build SACPRIM parameters before approaching the cast opacity, then update its red light.
void SpellActor::update_orifice_visuals(Match& match, int ticks) {
    if (!portal.enabled) {
        portal.enabled = true;
        if (const auto* target = effect_target ? effect_target->source_motion() : nullptr)
            motion.orientation = facing(motion.position, target->position, {0, 0, 1});
    }
    auto& p = portal.parameters;
    p[0] = 2.5f * cast_alpha * cast_alpha;
    p[1] = 3 * cast_alpha;
    const double a = portal.phase_a, b = portal.phase_b;
    p[2] = static_cast<float>(std::cos(b) * std::sin(a) * .1 + .8);
    p[3] = static_cast<float>(std::sin(b) * std::cos(a) * .1 + .8);
    p[4] = static_cast<float>(std::cos(a - b) * std::sin(a + b) * .1 + .6);
    p[5] = static_cast<float>(std::sin(a - b) * std::cos(a + b) * .1 + .6);
    p[6] += ticks * .2f;
    p[7] += ticks * .3f;
    for (unsigned i = 0; i < 2; ++i) {
        const auto texture = FourCC::from_text(i ? "vtx2" : "vtx1");
        const auto frames = std::max(match.texture_frames ? match.texture_frames(texture) : 1u, 1u);
        if (p[6 + i] >= frames)
            p[6 + i] -= frames * static_cast<int>(p[6 + i] / frames);
    }
    portal.phase_a += ticks * .01f;
    portal.phase_b += ticks * .05f;
    portal.opacity = static_cast<int>(cast_alpha * 255) / 255.0f;
    const double step = ticks / 60.0;
    cast_alpha = static_cast<float>(cast_alpha < sacrifice.alpha
        ? std::min(cast_alpha + step, static_cast<double>(sacrifice.alpha))
        : std::max(cast_alpha - step, static_cast<double>(sacrifice.alpha)));
    light = {true, motion.position, {1, 0, 0}, cast_alpha, 5, cast_alpha * 45 + 5};
    if (cast_alpha > .5f && !portal.sparks) {
        launch_sacrifice_host(match, *this, Effect::doctor_sparks);
        portal.sparks = true;
    }
}
Mesh SpellActor::portal_mesh(unsigned material, const TextureHeader& texture, Vec3 eye, Vec3 up) const {
    Mesh mesh;
    const auto& p = portal.parameters;
    const auto basis = facing(motion.position, eye, up);
    const auto frame = static_cast<unsigned>(p[6 + material]) % std::max(texture.frames, 1u);
    const auto columns = std::max(texture.width / std::max(texture.frame_width, 1u), 1u);
    const float su = static_cast<float>(texture.frame_width) / std::max(texture.width, 1u),
                sv = static_cast<float>(texture.frame_height) / std::max(texture.height, 1u);
    const Vec2 uv0{(frame % columns) * su, (frame / columns) * sv}, uv1{uv0.x + su, uv0.y + sv};
    const Vec2 center{uv0.x + su * .5f, uv0.y + sv * .5f};
    const auto color = 0xffffffu | static_cast<std::uint32_t>(portal.opacity * 255) << 24;
    const auto vertex = [&](float x, float z, Vec2 uv) {
        const auto& at = motion.position;
        mesh.vertices.push_back({{at.x + basis[0] * x + basis[6] * z,
                                  at.z + basis[2] * x + basis[8] * z,
                                  at.y + basis[1] * x + basis[7] * z}, {}, uv, color});
    };
    // SACPRIM::Draw: twelve groups of outer/middle/inner/middle vertices, then the fill centre.
    double angle = 0;
    for (unsigned i = 0; i < 12; ++i, angle += .52358335) {
        const float sine = static_cast<float>(std::sin(angle)), cosine = static_cast<float>(std::cos(angle));
        const float x = sine * p[0], z = cosine * p[1], u = i & 1 ? uv1.x : uv0.x;
        vertex(x, z, {u, uv0.y});
        vertex(x * p[2], z * p[3], {u, uv1.y});
        vertex(x * p[4], z * p[5], {u, uv0.y});
        vertex(x * p[2], z * p[3], {center.x + sine * su * .5f, center.y + cosine * sv * .5f});
    }
    vertex(0, 0, center);
    for (unsigned i = 0; i < 12; ++i) {
        const auto a = i * 4, b = ((i + 1) % 12) * 4;
        if (material == 1)
            mesh.indices.insert(mesh.indices.end(), {48, a + 3, b + 3});
        else
            for (unsigned ring = 0; ring < 2; ++ring)
                mesh.indices.insert(mesh.indices.end(), {a + ring, a + ring + 1, b + ring,
                                                        a + ring + 1, b + ring + 1, b + ring});
    }
    return mesh;
}
void SpellActor::update_altar_ring(Match& match, int ticks) {
    altar_ring.radius = altar_ring.end_radius = sacrifice.alpha;
    if (!altar_ring.enabled) {
        altar_ring.enabled = true;
        const auto fallback = FourCC::from_text("safr").value;
        altar_ring.texture = FourCC{static_cast<std::uint32_t>(
            data ? data->integer(FourCC::from_text("tcyl"), fallback) : fallback)};
    }
    if (sacrifice.flags & 16) {
        altar_ring.frame += ticks * .5f;
        const auto frames = std::max(match.texture_frames ? match.texture_frames(altar_ring.texture) : 1u, 1u);
        if (altar_ring.frame >= frames)
            altar_ring.frame = 0;
    }
}
// RINGEXP::Draw 0x16023330: five rings and seventeen columns (duplicated angular seam).
Mesh SpellActor::altar_ring_mesh(const TextureHeader& texture) const {
    return ring_exp_mesh(altar_ring.radius, altar_ring.height, altar_ring.end_radius,
                         motion.position, motion.orientation, texture, static_cast<unsigned>(altar_ring.frame));
}
// RINGEXP::Draw material 1: two flat rows at the centre height, the radius (frame bottom V, model colour)
// and the inner radius (frame top V, colour with zero alpha), drawn as one 2 x 17 strip.
Mesh SpellActor::altar_ring_band_mesh(const TextureHeader& texture) const {
    Mesh mesh;
    const auto frame = static_cast<unsigned>(altar_ring.frame) % std::max(texture.frames, 1u);
    const auto columns = std::max(texture.width / std::max(texture.frame_width, 1u), 1u);
    const float su = static_cast<float>(texture.frame_width) / std::max(texture.width, 1u),
                sv = static_cast<float>(texture.frame_height) / std::max(texture.height, 1u);
    const float u0 = (frame % columns) * su, v0 = (frame / columns) * sv;
    const float radii[2]{altar_ring.radius, altar_ring.inner}, v[2]{v0 + sv, v0};
    const std::uint32_t colors[2]{0xffffffffu, 0x00ffffffu};
    const auto& o = motion.orientation;
    const auto& at = motion.position;
    for (unsigned ring = 0; ring < 2; ++ring)
        for (unsigned j = 0; j < 17; ++j) {
            const double angle = -6.28318 * j / 16;
            const float x = static_cast<float>(std::sin(angle) * radii[ring]),
                        y = static_cast<float>(std::cos(angle) * radii[ring]);
            mesh.vertices.push_back({{at.x + o[0] * x + o[3] * y, at.z + o[2] * x + o[5] * y,
                                      at.y + o[1] * x + o[4] * y}, {}, {u0 + (j & 1 ? su : 0), v[ring]},
                                     colors[ring]});
        }
    for (unsigned j = 0; j < 16; ++j)
        mesh.indices.insert(mesh.indices.end(), {j, j + 1, j + 17, j + 1, j + 18, j + 17});
    return mesh;
}
Mesh ring_exp_mesh(float radius, float height, float end_radius, Vec3 origin, const Orientation& orientation,
                   const TextureHeader& texture, unsigned frame) {
    Mesh mesh;
    frame %= std::max(texture.frames, 1u);
    const auto columns = std::max(texture.width / std::max(texture.frame_width, 1u), 1u);
    const float su = static_cast<float>(texture.frame_width) / std::max(texture.width, 1u),
                sv = static_cast<float>(texture.frame_height) / std::max(texture.height, 1u);
    const float u0 = (frame % columns) * su, v0 = (frame / columns) * sv;
    const auto quarter_radius = static_cast<float>((end_radius - radius) * .1 + radius);
    const float heights[5]{-height * .5f, -height * .25f, 0, height * .25f, height * .5f},
                radii[5]{end_radius, quarter_radius, radius, quarter_radius, end_radius},
                v[5]{v0, v0 + sv * .5f, v0 + sv, v0 + sv * .5f, v0};
    for (unsigned ring = 0; ring < 5; ++ring)
        for (unsigned j = 0; j < 17; ++j) {
            const double angle = -6.28318 * j / 16;
            const float x = static_cast<float>(std::sin(angle) * radii[ring]),
                        y = static_cast<float>(std::cos(angle) * radii[ring]), z = heights[ring];
            const auto& o = orientation;
            const auto& at = origin;
            mesh.vertices.push_back({{at.x + o[0] * x + o[3] * y + o[6] * z,
                                      at.z + o[2] * x + o[5] * y + o[8] * z,
                                      at.y + o[1] * x + o[4] * y + o[7] * z}, {},
                                     {u0 + (j & 1 ? su : 0), v[ring]}, 0xffffffffu});
        }
    for (unsigned ring = 0; ring < 4; ++ring)
        for (unsigned j = 0; j < 16; ++j) {
            const auto a = ring * 17 + j, b = a + 17;
            mesh.indices.insert(mesh.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    return mesh;
}
int SpellActor::run_desecration_globe(Match& match, int ticks) {
    if (!stage) {
        if (!sacrifice.altar) {
            state_flags |= removed_flag;
            return -2;
        }
        effect_target = sacrifice.altar->controller;
        sacrifice.alpha = 1;
        stage = 33;
    } else if (stage == 240) {
        sacrifice.alpha = 0;
        if (cast_alpha == 0) {
            state_flags |= removed_flag;
            return -2;
        }
    }
    // 0x44b350 approaches once here, and 0x44a5b0 approaches a second time below.
    const double step = ticks / 60.0;
    cast_alpha = static_cast<float>(cast_alpha < sacrifice.alpha
        ? std::min(cast_alpha + step, static_cast<double>(sacrifice.alpha))
        : std::max(cast_alpha - step, static_cast<double>(sacrifice.alpha)));
    if (sacrifice.altar) {
        if (!effect_target || !sacrifice.altar->ritual.victim)
            sacrifice.altar = nullptr;
        else {
            point = effect_target->source_motion()->position;
            point.z += 15;
            if (match.terrain)
                point.z = std::max(point.z, static_cast<float>(terrain_surface(*match.terrain, point.x, point.y) + 15));
            if (!host.ticks) {
                motion.position = point;
                host.ticks = 1;
            }
            if (sacrifice.altar->ritual.fraction) {
                if (host.ticks == 1) {
                    host.ticks = 2;
                    launch_sacrifice_host(match, *this, Effect::desecration_beam);
                }
            } else
                host.ticks = 1;
        }
    } else
        stage = 240;
    const Vec3 delta{motion.position.x - point.x, motion.position.y - point.y, motion.position.z - point.z};
    const auto distance = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    if (distance > 15) {
        const float scale = 15 / distance;
        motion.position = {point.x + delta.x * scale, point.y + delta.y * scale, point.z + delta.z * scale};
    }
    update_orifice_visuals(match, ticks);
    return 1;
}
int SpellActor::run_desecration_beam(Match& match, int ticks) {
    if (!stage) {
        auto* globe = dynamic_cast<SpellActor*>(effect_target);
        if (!globe) {
            state_flags |= removed_flag;
            return -2;
        }
        impact_target = globe;
        effect_target = globe->effect_target;
        motion.position = globe->motion.position;
        spell_target_point(point, effect_target);
        beam.enabled = true;
        beam.texture = FourCC::from_text("ltng");
        beam.hold = .5f;
        stage = 1;
        // 0x4355e0 sets the initial primitive before 0x435840 runs it in this same call.
        LightningPrimitive::Parameters initial;
        initial.source = motion.position;
        initial.target = point;
        initial.step = .2f;
        initial.deviation = 3;
        initial.strands = 3;
        beam.primitive.set_params(initial, match.visual_random);
    }
    const auto* globe = dynamic_cast<const SpellActor*>(impact_target);
    const bool active = globe && effect_target && (effect_target->kind_flags & 1) && globe->sacrifice.altar &&
                        globe->sacrifice.altar->ritual.fraction;
    if (active) {
        motion.position = globe->motion.position;
        spell_target_point(point, effect_target);
    } else
        impact_target = nullptr;
    // 0x44b210 writes context +76 (object +100): the hold duration, not the strand count.
    // It is refreshed while the altar's processing fraction is nonzero.
    beam.hold = active ? 10.0f : 0.0f;
    const auto target = point;
    spell_target_point(point, effect_target);
    if (emission < 1)
        emission = std::min(1.0f, emission + ticks / 15.0f);
    else {
        beam.hold = static_cast<float>(beam.hold - ticks / 60.0);
        if (beam.hold < 0) {
            age += ticks / 15.0f;
            if (age >= 1) {
                state_flags |= removed_flag;
                return -2;
            }
        }
    }
    LightningPrimitive::Parameters parameters;
    parameters.source = motion.position;
    parameters.target = target;
    parameters.step = .2f;
    parameters.deviation = 3;
    parameters.start = age;
    parameters.end = emission;
    parameters.strands = 3;
    beam.primitive.set_params(parameters, match.visual_random);
    beam.frame += ticks * .5f;
    const auto frames = std::max(match.texture_frames ? match.texture_frames(beam.texture) : 1u, 1u);
    if (beam.frame >= frames)
        beam.frame = 0;
    return 1;
}
int SpellActor::run_sacrifice_host(Match& match, int ticks) {
    auto& p = particles(match);
    if (!host.initialized) {
        host.initialized = true;
        if (effect == Effect::doctor_sparks) { // 0x44aa90.
            particle_material(match, p, 0x1a012008, "vtx2");
            const float scale = .75f, life = 1.5f;
            p.set_property(131, &scale, &life);
            const float low[3]{-1.5f, -1.5f, -2.5f}, high[3]{1.5f, 1.5f, -.5f};
            p.set_property(2, low, high);
            const ParticleSystem::Frame frame{motion.orientation, motion.position};
            p.set_frame(frame, &frame);
            host.rate = 1;
        } else if (effect == Effect::doctor_beam_sparks) { // 0x449770.
            particle_material(match, p, 0x1a012008, "cst0");
            const float scale = .5f, life = 1.5f;
            p.set_property(131, &scale, &life);
            const float low[3]{-.06666667f, .016666668f, -.06666667f},
                        high[3]{.06666667f, .05f, .06666667f};
            p.set_property(3, low, high);
            host.rate = .5f;
        } else { // 0x4490d0: skulls around the raised victim, emitted manually once per tick.
            const auto texture = data ? data->integer(FourCC::from_text("tskl"), FourCC::from_text("skul").value)
                                      : FourCC::from_text("skul").value;
            particle_material(match, p, 0x1a012008, FourCC{static_cast<std::uint32_t>(texture)});
            const float scale = 2, life = 1.5f, alpha = 255;
            p.set_property(132, &scale, &life);
            p.set_property(14, &alpha, &alpha);
            host.fade = std::bit_cast<float>(1009488541u);
        }
    }
    if (effect == Effect::doctor_beam_sparks && !stage) {
        const auto* source = dynamic_cast<const SpellActor*>(effect_target);
        if (source && source->age <= 0) {
            motion.position = source->motion.position;
            motion.orientation = facing(motion.position, source->point, {0, 0, 1});
            const ParticleSystem::Frame frame{motion.orientation, motion.position};
            p.set_frame(frame, &frame);
        } else
            stage = 240;
    }
    const auto* target = effect_target ? effect_target->source_motion() : nullptr;
    if (target && !stage) {
        motion.position = target->position;
        if (effect == Effect::doctor_sparks)
            p.set_position(motion.position);
        else if (effect == Effect::altar_motes) {
            motion.yaw = static_cast<float>(motion.yaw + ticks * motion.angular_velocity.z);
            motion.orientation = motion_orientation(motion.yaw);
            for (int i = 0; i < ticks; ++i) {
                const double angle = match.visual_random.next() * 6.2831802 / 32768 - 3.141590118408203;
                const float low[3]{static_cast<float>(std::sin(angle) * 15 - 5),
                                   static_cast<float>(std::cos(angle) * 15 - 5), -10},
                            high[3]{low[0] + 5, low[1] + 5, 5};
                p.set_property(2, low, high);
                p.emit(0, 1);
            }
            if (motion.angular_velocity.z == host.fade)
                host.fade = -host.fade;
        }
    } else {
        stage = 240;
        host.rate = host.fade = 0;
        if (!p.alive()) {
            state_flags |= removed_flag;
            return -2;
        }
    }
    if (effect == Effect::altar_motes)
        motion_approach(motion.angular_velocity.z, host.fade, ticks * std::bit_cast<float>(951517898u),
                        ticks * std::bit_cast<float>(951517898u));
    host.time += ticks;
    p.advance(host.time, host.rate);
    return 1;
}
} // namespace sacrifice
