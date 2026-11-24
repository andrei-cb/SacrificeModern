#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/spatial.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
FourCC cc(std::string_view text) { return FourCC::from_text(text); }
FourCC texture(const SpellData& data, const char* key, const char* fallback) {
    return FourCC{static_cast<unsigned>(data.integer(cc(key), cc(fallback).value))};
}
Vec3 difference(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double length(Vec3 p) { return std::sqrt(static_cast<double>(p.x) * p.x + static_cast<double>(p.y) * p.y + static_cast<double>(p.z) * p.z); }
int remove(SpellActor& spell) { spell.state_flags |= MatchObject::removed_flag; return -2; }
unsigned cast_state(SpellActor& spell) {
    auto* caster = dynamic_cast<CreatureActor*>(spell.owner);
    // 0x441d20: no owner (a 0x453600 side cast) aborts unless the handler skips the wait in native.
    if (!spell.owner)
        return SpellActor::casterless_skips_cast(spell.effect) ? 0x20000000 : 0x10;
    if (!caster || !(caster->kind_flags & 1))
        return 0x10;
    const auto [state, alpha] = caster->cast_state();
    spell.cast_alpha = alpha;
    return state;
}
Vec3 attachment(const MatchObject* object, Vec3 offset) {
    if (!object || !object->source_motion())
        return {};
    const auto& m = *object->source_motion();
    const auto& o = m.orientation;
    return {m.position.x + o[0] * offset.x + o[3] * offset.y + o[6] * offset.z,
            m.position.y + o[1] * offset.x + o[4] * offset.y + o[7] * offset.z,
            m.position.z + o[2] * offset.x + o[5] * offset.y + o[8] * offset.z};
}
int run_rock_fragments(Match& match, SpellActor& spell, int ticks) {
    // 0x4388B0 fragments (no emission) and 0x4389D0 dust (emission draining at host.rate) both retire once
    // their emission is spent and the last particle has died.
    auto& p = spell.particles(match);
    spell.host.time += ticks;
    p.advance(spell.host.time, spell.emission);
    spell.emission = std::max(0.f, spell.emission + ticks * spell.host.rate);
    if (spell.emission <= 0 && !p.alive()) {
        spell.state_flags |= MatchObject::removed_flag;
        return -2;
    }
    return 1;
}
float burst_size(Vec3 low, Vec3 high) { // 0x44CA80(box, .3, 2, .25).
    return std::clamp(static_cast<float>(length(difference(high, low)) * .3 * .5), .25f, 2.f);
}
void launch_fragments(Match& match, SpellActor& spell, Vec3) { SpellActor::launch_rock_shatter(match, spell); }
void hit_particles(Match& match, SpellActor& spell, MatchObject* victim, Vec3 point) {
    auto& hit = SpellActor::launch_sacrifice_host(match, spell, SpellActor::Effect::spell_hit);
    hit.effect_target = victim;
    hit.motion.position = point;
    hit.run(match, 0);
}
}
void SpellActor::launch_dust_splatter(Match& match, SpellActor& source, std::uint32_t material, FourCC texture,
                                      Vec3 centre, Vec3 half, float seconds, float fraction) {
    // 0x439830 -> 0x439690: a GenericDecayingPsysRun host in a +-half box, preset 129 sized by 0x44CA80,
    // life (1 - fraction) * seconds, whose emission drains over fraction * seconds.
    auto& cloud = launch_sacrifice_host(match, source, Effect::rock_fragments);
    cloud.data = source.data;
    cloud.motion.position = centre;
    auto& p = cloud.particles(match);
    cloud.particle_material(match, p, material, texture);
    const float size = burst_size({-half.x, -half.y, -half.z}, half), life = (1 - fraction) * seconds;
    p.set_property(129, &size, &life);
    const float box_low[3]{-half.x, -half.y, -half.z}, box_high[3]{half.x, half.y, half.z};
    p.set_property(2, box_low, box_high);
    p.set_position(centre);
    cloud.emission = 1;
    cloud.host.rate = -1.f / std::max(seconds * fraction * 60, 1.f);
}
void SpellActor::launch_rock_shatter(Match& match, SpellActor& source) {
    // 0x439270 -> 0x439380: tRS4 ground scorch within 5 of the land (0x44CB50), a tRS3 dust splatter over the
    // body box grown 1.5x (0x4397A0 -> 0x439690, 3 s, .5), then iRS2 tRS1 fragments flung from the box at
    // fRS5 under gravity (0x439440 flags 14: sound 4, opaque, shrinking to nothing).
    const auto* data = source.data.get();
    const auto key = [&](const char* name, const char* fallback) {
        return FourCC{static_cast<std::uint32_t>(data ? data->integer(cc(name), cc(fallback).value) : cc(fallback).value)};
    };
    const auto at = source.motion.position;
    const Vec3 low = source.motion.box_min, high = source.motion.box_max;
    if (const auto scorch = key("tRS4", "gscr"); scorch.value && match.terrain &&
        std::abs(terrain_surface(*match.terrain, at.x, at.y) - at.z) < 5)
        match.scar_requests.push_back({scorch, at, match.clock().match, ++match.terrain_revision});
    if (const auto dust = key("tRS3", "dirt"); dust.value)
        launch_dust_splatter(match, source, 0x0a011008, dust,
                             {at.x + (low.x + high.x) * .5f, at.y + (low.y + high.y) * .5f,
                              at.z + (low.z + high.z) * .5f},
                             {(high.x - low.x) * .75f, (high.y - low.y) * .75f, (high.z - low.z) * .75f}, 3, .5f);
    launch_fragment_burst(match, source, key("tRS1", "rock"), data ? data->integer(FourCC{1767002930}, 64) : 64,
                          data ? data->real(cc("fRS5"), 10) : 10); // iRS2.
}
void SpellActor::launch_fragment_burst(Match& match, SpellActor& source, FourCC texture, int count, float speed,
                                       unsigned flags, const Vec3* box_low_override, const Vec3* box_high_override) {
    // 0x439440: count particles thrown from the body box at `speed` under gravity. Its sound is left to the
    // caller, which already plays the shot's own sound.
    if (count <= 0 || !texture.value)
        return;
    const Vec3 low = box_low_override ? *box_low_override : source.motion.box_min,
               high = box_high_override ? *box_high_override : source.motion.box_max;
    auto& bits = launch_sacrifice_host(match, source, Effect::rock_fragments);
    bits.data = source.data;
    bits.motion.position = source.motion.position;
    auto& p = bits.particles(match);
    bits.particle_material(match, p, 0x0a011008, texture);
    const float size = burst_size(low, high), life = 1.5f;
    p.set_property(129, &size, &life);
    const float box_low[3]{low.x, low.y, low.z}, box_high[3]{high.x, high.y, high.z};
    p.set_property(2, box_low, box_high);
    const float fling[3]{speed, 0, 0}, still[3]{};
    p.set_property(3, fling, still);
    p.set_flags(flags & 2 ? 0x600 : 0x200, flags & 1 ? 0x600000 : 0);
    const float gravity[3]{0, 0, -.002725f};
    p.set_property(4, gravity, gravity);
    const float opaque = 255, none = 0;
    if (flags & 4)
        p.set_property(14, &opaque, &opaque);
    if (flags & 8)
        p.set_property(6, &none, &none);
    p.set_position(bits.motion.position);
    for (int i = 0; i < count; ++i)
        p.emit(0, 1);
}
void SpellActor::launch_ground_ring(Match& match, SpellActor& source, std::uint32_t material, FourCC texture,
                                    float radius, float width, float spacing, float low, float high, float life,
                                    int least, int most) {
    if (!texture.value || spacing <= 0 || life <= 0)
        return;
    const int count = static_cast<int>(std::clamp(radius * 6.28318f / spacing, static_cast<float>(least),
                                                  static_cast<float>(most)));
    auto& ring = launch_sacrifice_host(match, source, Effect::rock_fragments);
    ring.data = source.data;
    ring.motion.position = source.motion.position;
    auto& p = ring.particles(match);
    ring.particle_material(match, p, material, texture);
    const float size = std::clamp(width * .3f, .25f, 2.f); // 0x44CB00(width, .3, 2, .25).
    p.set_property(129, &size, &life);
    const float box_low[3]{-width, -width, 0}, box_high[3]{width, width, 0};
    p.set_property(2, box_low, box_high);
    const float rise_low[3]{0, 0, low / (life * 60)}, rise_high[3]{0, 0, high / (life * 60)};
    p.set_property(3, rise_low, rise_high);
    for (int i = 0; i < count; ++i) {
        const float angle = i * 6.28318f / count;
        Vec3 at{ring.motion.position.x + std::sin(angle) * radius, ring.motion.position.y + std::cos(angle) * radius,
                ring.motion.position.z};
        if (match.terrain)
            at.z = static_cast<float>(terrain_surface(*match.terrain, at.x, at.y)); // 0x44ECB0.
        p.set_position(at);
        p.emit(0, 1);
    }
}
int SpellActor::run_wizard_offense(Match& match, int ticks) {
    if (!data)
        throw Error("Wizard offensive spell requires its SPEL definition");
    switch (effect) {
    case Effect::lightning_cast:
    case Effect::lightning_bolt: return run_lightning(match, ticks);
    case Effect::rock: return run_rock(match, ticks);
    case Effect::flash_orb: return run_flash_orb(match, ticks);
    case Effect::insect_swarm: return run_insect_swarm(match, ticks);
    case Effect::rock_fragments: return run_rock_fragments(match, *this, ticks);
    default: throw Error("Invalid wizard offensive spell handler");
    }
}
void SpellActor::capture_collision(Match& match) {
    impact_target = nullptr;
    if (motion.collided)
        for (const auto& object : match.objects())
            if (object->source_motion() == motion.collided) {
                impact_target = object.get();
                break;
            }
    state_flags &= ~3u;
}
// 0x441da0; collision metadata comes from the native particle/model box supplied by the caller.
void SpellActor::launch_projectile(Match& match) {
    spell_target_point(point, effect_target);
    motion.velocity = difference(point, motion.position);
    const auto distance = length(motion.velocity);
    if (distance) {
        const auto factor = data->speed / distance;
        motion.velocity = {static_cast<float>(motion.velocity.x * factor), static_cast<float>(motion.velocity.y * factor),
                           static_cast<float>(motion.velocity.z * factor)};
    }
    motion.flags |= 1;
    motion.collision_mask |= 0x114015;
    friendly_ignore_time = match.projectile_friendly_fire ? 2.0f : 0; // 0x474fb0(2.0).
    motion.collision_mask |= 0x20000;
    bind_world(match);
}
void SpellActor::steer_projectile(Match& match, int ticks) {
    if (had_target && !effect_target) {
        had_target = false;
        TerrainPlane plane;
        if (!terrain_plane(*match.terrain, point.x, point.y, plane) || !plane.solid)
            state_flags |= 2;
        point.z = static_cast<float>(terrain_true_height(*match.terrain, point.x, point.y));
    }
    spell_lead_target(point, effect_target, data->speed, motion, *match.terrain);
    spell_approach_velocity(motion, point, ticks * data->acceleration, data->speed);
    const float ground = static_cast<float>(terrain_surface(*match.terrain, motion.position.x, motion.position.y));
    if (ground > 0) {
        const float distance = static_cast<float>(length(difference(motion.position, point)));
        const double clearance = distance < 3 ? 0 : distance >= 10 ? 1 : (distance - 3.0) / 7;
        if (motion.position.z - ground < clearance)
            motion.position.z = static_cast<float>(ground + clearance);
    }
}

// LightningSpellStart 0x422a00 and LightningSpellRun 0x422cb0.
int SpellActor::run_lightning(Match& match, int ticks) {
    if (effect == Effect::lightning_cast) {
        auto& p = particles(match);
        if (!stage) {
            launch_cast_hands(match, *this);
            particle_material(match, p, 0x1a012008, "cst0");
            const float size = .6f, life = 1, low[3]{-.5f, -.5f, -.5f}, high[3]{.5f, .5f, .5f};
            p.set_property(132, &size, &life);
            p.set_property(2, low, high);
            host.rate = .1f;
            stage = 1;
        }
        if (stage == 1) {
            host.rate = std::min(1.0f, host.rate + ticks / 60.0f);
            if (const auto state = cast_state(*this))
                stage = state == 0x20000000 ? 16 : 240;
        } else if (stage == 16) {
            auto& bolt = static_cast<SpellActor&>(match.create(MatchObject::spell,
                [] { return std::make_unique<SpellActor>(); }, this, [&](MatchObject& object) {
                    auto& spell = static_cast<SpellActor&>(object);
                    spell.effect = Effect::lightning_bolt;
                    spell.data = data;
                }));
            bolt.owner = owner;
            bolt.effect_target = effect_target;
            bolt.point = point;
            bolt.motion.position = motion.position;
            if (allegiance)
                allegiance->attach(bolt);
            bolt.run(match, 0);
            stage = 240;
        }
        if (stage == 240) {
            host.rate = 0;
            if (!p.alive())
                return remove(*this);
        }
        if (owner)
            motion.position = attachment(owner, {0, 1, 2}); // unk_4CB134.
        host.time += ticks;
        p.advance(host.time, host.rate);
        return 1;
    }
    if (!stage) {
        spell_target_point(point, effect_target);
        if (!owner) {
            // 0x422cb0 -> 0x44c0d0(state, spell, 38, 2, 1): only ownerless bolts strike from the sky.
            motion.position.x = static_cast<float>(match.random().next() * 60.0 / 32768 - 30 + motion.position.x);
            motion.position.y = static_cast<float>(match.random().next() * 60.0 / 32768 - 30 + motion.position.y);
            motion.position.z += 100;
        }
        // 0x4355e0 with flag 2 traces at least the range, and substitutes the nearest object or land hit.
        const auto delta = difference(point, motion.position);
        const auto distance = length(delta);
        const auto scale = distance ? std::max<double>(data->range, distance) / distance : 0;
        const Vec3 ray{static_cast<float>(delta.x * scale), static_cast<float>(delta.y * scale), static_cast<float>(delta.z * scale)},
                   end{motion.position.x + ray.x, motion.position.y + ray.y, motion.position.z + ray.z};
        double nearest = 1.01;
        MatchObject* hit = nullptr;
        if (match.spatial)
            match.spatial->visit_ray(motion.position, end, 21, [&](const ObjectMotion& other) {
                if (&other == &motion || (owner && owner->source_motion() == &other))
                    return 0;
                const auto relative = difference(motion.position, other.position);
                for (const auto& box : other.collision_boxes) {
                    const auto fraction = motion_segment_contact(relative, ray, box, 0);
                    if (fraction >= 0 && fraction < nearest) {
                        nearest = fraction;
                        for (const auto& object : match.objects())
                            if (object->source_motion() == &other)
                                hit = object.get();
                    }
                }
                return 0;
            });
        Vec3 ground;
        const auto intercept = match.terrain ? terrain_intercept(*match.terrain, motion.position, ray, ground, 1000000, true) : -1;
        if (intercept > 0 && intercept < nearest)
            point = ground, hit = nullptr;
        else if (nearest <= 1)
            point = {motion.position.x + static_cast<float>(ray.x * nearest), motion.position.y + static_cast<float>(ray.y * nearest),
                     motion.position.z + static_cast<float>(ray.z * nearest)};
        else
            point = end;
        effect_target = hit;
        beam.enabled = true;
        beam.texture = cc("ltng");
        beam.hold = .5f;
        light = {true, motion.position, {1, 1, 1}, 1, 1, 100};
        sound_event(match, 2);
        stage = 1;
    }
    const auto target = point; // Draw uses the previous target point, refreshed for the next call.
    spell_target_point(point, effect_target);
    if (emission < 1) {
        emission = std::min(1.0f, emission + ticks / 15.0f);
        if (emission == 1) {
            sound_event(match, 4);
            const auto origin = motion.position;
            motion.position = point;
            spell_damage(match, this, effect_target, data->damage_type, data->amount, data->radius, point);
            motion.position = origin;
            hit_particles(match, *this, effect_target, point);
            visual_requests.push_back({VisualRequest::Kind::remove_widgets});
        }
    } else {
        beam.hold -= ticks / 60.0f;
        if (beam.hold < 0) {
            age += ticks / 15.0f;
            if (age >= 1)
                return remove(*this);
            light.intensity = std::max(0.0f, light.intensity - .1f);
        }
    }
    light.position = {motion.position.x + (point.x - motion.position.x) * emission,
                       motion.position.y + (point.y - motion.position.y) * emission,
                       motion.position.z + (point.z - motion.position.z) * emission};
    LightningPrimitive::Parameters p;
    p.source = motion.position;
    p.target = target;
    p.step = .2f;
    p.deviation = 1.5f;
    p.width = .5f;
    p.strands = 2;
    p.start = age;
    p.end = emission;
    beam.primitive.set_params(p, match.visual_random);
    beam.frame += ticks * .5f;
    const auto frames = std::max(match.texture_frames ? match.texture_frames(beam.texture) : 1u, 1u);
    if (beam.frame >= frames)
        beam.frame = 0;
    return 1;
}

// FlashOrbRun 0x42bad0 / 0x42b9f0 / 0x42b980.
int SpellActor::run_flash_orb(Match& match, int ticks) {
    auto& p = particles(match);
    if (!stage) {
        launch_cast_hands(match, *this);
        had_target = effect_target != nullptr;
        stage = 1;
    }
    if (stage == 1) {
        if (const auto state = cast_state(*this))
            stage = state == 0x20000000 ? 16 : 255;
        return 1;
    }
    if (stage == 16) {
        particle_material(match, p, 0x1a012008, texture(*data, "ttex", "flao"));
        const float size = .5f, life = 1;
        p.set_property(129, &size, &life);
        host.rate = 1;
        light = {true, motion.position, {1, 1, 1}, 1, 1, 25};
        if (auto* actor = dynamic_cast<CreatureActor*>(owner))
            actor->write_reference_point(motion.position, 0, true);
        generic.initialized = true;
        generic.collision_radius = .1f;
        flight_remaining = 800;
        launch_projectile(match);
        sound_event(match, 2);
        stage = 17;
    }
    if (stage == 17) {
        steer_projectile(match, ticks);
        if (state_flags & 3) {
            capture_collision(match);
            stage = 32;
        } else if ((flight_remaining -= ticks) <= 0)
            stage = 255;
    } else if (stage == 32) {
        sound_event(match, 4);
        auto& ring = launch_sacrifice_host(match, *this, Effect::ruin_ring);
        ring.effect_target = nullptr;
        ring.host.initialized = true;
        ring.altar_ring.enabled = true;
        ring.altar_ring.texture = texture(*data, "trng", "forb");
        ring.altar_ring.radius = 2;
        ring.altar_ring.height = .5f;
        ring.host.rate = (20.0f - 2) / 60;
        ring.remaining = 60;
        ring.linger = 60;
        launch_fireball_visual(match, *this, Effect::fireball_blast);
        spell_damage(match, this, impact_target, data->damage_type, data->amount, data->radius);
        light.enabled = false;
        stage = 255;
    }
    if (stage == 255) {
        host.rate = 0;
        motion.flags = motion.collision_mask = 0;
        if (!p.alive())
            return remove(*this);
    }
    p.set_position(motion.position);
    host.time += ticks;
    p.advance(host.time, host.rate);
    light.position = motion.position;
    return 1;
}

// RockRun 0x446390: rise from the caster's forward ground point, then launch with the model's boxes.
int SpellActor::run_rock(Match& match, int ticks) {
    const auto follow = [&](bool ground) {
        if (owner && owner->source_motion()) {
            const auto& m = *owner->source_motion();
            motion.position.x = m.position.x + m.orientation[3] * 2;
            motion.position.y = m.position.y + m.orientation[4] * 2;
            if (ground)
                motion.position.z = static_cast<float>(terrain_true_height(*match.terrain, motion.position.x, motion.position.y));
        }
    };
    if (!stage) {
        model_id = texture(*data, "tmod", "rokm");
        const auto found = match.spell_models.find(model_id);
        if (found == match.spell_models.end())
            throw Error("Rock model is not bound:" + model_id.text());
        model_ = found->second;
        model_scale = 1;
        if (match.create_spell_model)
            match.create_spell_model(*this);
        spell_target_point(point, effect_target);
        follow(true);
        const auto angular = [&] { return static_cast<float>((match.random().next() - 0x4000) * .000001597086588541667); };
        motion.angular_velocity = {angular(), angular(), angular()};
        flight_remaining = static_cast<int>(data->duration * 60);
        launch_cast_hands(match, *this);
        // 0x4380A0(emission 1, life 1.25): the 'tRT1' (default 'dust') body-sized trail.
        launch_model_trail(match, *this, 0x0a011008,
                           FourCC{static_cast<unsigned>(data->integer(FourCC::from_text("tRT1"),
                                                                      FourCC::from_text("dust").value))},
                           1, 1.25f);
        sound_event(match, 1);
        stage = 1;
    }
    if (stage == 1) {
        if (const auto state = cast_state(*this))
            stage = state == 0x20000000 ? 16 : 240;
        follow(true);
        motion.position.z -= (1 - cast_alpha) * 1.5f;
        match.shake(motion.position, 8, 30);
        return 1;
    }
    if (stage == 16) {
        stop_sound_event(1);
        sound_event(match, 2);
        age = 0;
        motion.velocity = {0, 0, std::bit_cast<float>(1051372203u)};
        motion.flags = 57;
        bind_world(match);
        // 0x4380f0's projected ground effects also request a local shake and a native scar.
        Vec3 ground = motion.position;
        ground.z = static_cast<float>(terrain_true_height(*match.terrain, ground.x, ground.y));
        match.shake(ground, 16, 50);
        launch_earth_burst(match, *this, ground, 3, 24);
        match.scar_requests.push_back({texture(*data, "tRE3", "tstr"), ground, match.clock().match, ++match.terrain_revision});
        stage = 17;
    }
    if (stage == 17) {
        age += ticks / 15.0f;
        follow(false);
        if (age >= 1)
            stage = 32;
        return 1;
    }
    if (stage == 32) {
        motion.collision_mask |= 0x114015;
        motion.flags |= 1;
        stage = 33;
    }
    if (stage == 33) {
        spell_target_point(point, effect_target);
        Vec3 aim = point;
        const double x = static_cast<double>(point.x) - motion.position.x, y = static_cast<double>(point.y) - motion.position.y;
        if (x * x + y * y > 100) {
            aim.z += 5;
            motion.position.z = std::max(motion.position.z,
                static_cast<float>(terrain_surface(*match.terrain, motion.position.x, motion.position.y) + 5));
        }
        spell_approach_velocity(motion, aim, ticks * data->acceleration, data->speed);
        if (state_flags & 3) {
            capture_collision(match);
            stage = 48;
        } else if ((flight_remaining -= ticks) <= 0)
            stage = 48, impact_target = nullptr;
    } else if (stage == 48) {
        if (!impact_target)
            match.dent(motion.position, 7.5f, .25f); // 0x44d0b0(pos, 7.5, 0.25, 1).
        visual_requests.push_back({VisualRequest::Kind::rock_debris});
        launch_fragments(match, *this, motion.position); // 0x439270
        spell_damage(match, this, impact_target, data->damage_type, data->amount, data->radius);
        sound_event(match, 4);
        return remove(*this);
    } else if (stage == 240) {
        model_scale -= ticks * .05f;
        if (model_scale < 0)
            return remove(*this);
    }
    return 1;
}

// InsectSwarmStart 0x431900 / InsectSwarmRun 0x4315a0, sharing the native particle swarm preset.
int SpellActor::run_insect_swarm(Match& match, int ticks) {
    auto& p = particles(match);
    static constexpr float gathering[7]{.1f, .3f, .3f, .3f, .2f, .05f, .05f},
                           flying[7]{.1f, .2f, .2f, .2f, .2f, .05f, .05f},
                           impact[7]{.1f, .8f, .8f, .8f, .2f, .05f, .05f};
    const float count = std::bit_cast<float>(0);
    if (!host.initialized) {
        host.initialized = true;
        flight_remaining = 800;
        if (auto* actor = dynamic_cast<CreatureActor*>(owner))
            actor->write_reference_point(motion.position, 0, false);
        motion.position.z += 1.5f;
        particle_material(match, p, 0x0a011008, texture(*data, "ttex", "fb_g"));
        p.set_position(motion.position, &motion.position);
        p.set_property(136, &count, gathering);
        sound_event(match, 1);
        had_target = effect_target != nullptr;
        generic.initialized = true;
        generic.collision_radius = .3f;
    }
    switch (stage) {
    case 0: {
        const auto state = cast_state(*this);
        if (state == 0x10) {
            stage = 6;
            break;
        }
        if (!owner) {
            stage = 1;
            break;
        }
        auto* caster = dynamic_cast<CreatureActor*>(owner);
        caster->write_reference_point(motion.position, 0, false);
        motion.position.z += 1.5f;
        for (int i = 0; i < ticks && p.count() < 64; ++i) {
            auto* particle = p.emit(0, 1);
            if (!particle)
                break;
            caster->write_reference_point(particle->position, match.random().next() & 1, true);
            particle->velocity = {};
        }
        if (state == 0x20000000)
            stage = 1;
        break;
    }
    case 1:
        launch_projectile(match);
        while (p.count() < 64)
            if (!p.emit(0, 1))
                break;
        p.set_property(136, &count, flying);
        generic.collision_radius = .2f;
        stage = 2;
        [[fallthrough]];
    case 2:
    case 3:
        if (stage == 2)
            steer_projectile(match, ticks);
        if (state_flags & 3) {
            capture_collision(match);
            stage = 4;
        } else if ((flight_remaining -= ticks) <= 0)
            stage = 6;
        break;
    case 4:
        sound_event(match, 4);
        p.set_property(136, &count, impact);
        effect_target = impact_target;
        hit_particles(match, *this, impact_target, motion.position);
        spell_damage(match, this, impact_target, data->damage_type, data->amount, data->radius);
        if (auto* victim = dynamic_cast<CreatureActor*>(impact_target))
            victim->change_mana_fixed(static_cast<int>(static_cast<std::int64_t>(
                -static_cast<double>(victim->maximum_mana) * data->real(cc("fman"), 0) * 65536)));
        remaining = 120;
        motion.flags = motion.collision_mask = 0;
        stage = 5;
        [[fallthrough]];
    case 5:
        spell_target_point(motion.position, effect_target);
        if ((remaining -= ticks) <= 0)
            stage = 6;
        break;
    case 6: {
        motion.flags = motion.collision_mask = 0;
        stop_sound_event(255);
        const float interval[2]{.25f, 1};
        p.set_property(10, interval, interval);
        stage = 7;
        [[fallthrough]];
    }
    case 7:
        if (!p.alive())
            return remove(*this);
        break;
    default: break;
    }
    p.set_position(motion.position);
    host.time += ticks;
    p.advance(host.time, 0);
    return 1;
}
} // namespace sacrifice
