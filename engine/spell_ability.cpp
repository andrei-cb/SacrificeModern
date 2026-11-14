#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include "sacrifice/spell_data.hpp"
#include <algorithm>
#include <bit>
#include <cmath>

namespace sacrifice {
namespace {
int remove_spell(SpellActor& spell) {
    spell.state_flags |= MatchObject::removed_flag;
    return -2;
}
CreatureActor* living_unit(MatchObject* object) {
    auto* actor = dynamic_cast<CreatureActor*>(object);
    return actor && (actor->kind_flags & 5) && actor->alive() ? actor : nullptr;
}
double distance_squared(Vec3 a, Vec3 b) {
    const double dx = static_cast<double>(a.x) - b.x, dy = static_cast<double>(a.y) - b.y,
                 dz = static_cast<double>(a.z) - b.z;
    return dx * dx + dy * dy + dz * dz;
}
void aim_point(SpellActor& spell) {
    if (spell.effect_target)
        spell_target_point(spell.motion.position, spell.effect_target);
    else
        spell.motion.position = spell.point;
}
FourCC cc(std::string_view text) {
    return FourCC::from_text(text);
}
FourCC hop_name(const SpellActor& spell) {
    if (spell.spell_id.value)
        return spell.spell_id;
    switch (spell.effect) {
    case SpellActor::Effect::fire_bow:
        return FourCC::from_text("fbow");
    case SpellActor::Effect::air_clawpult:
        return FourCC::from_text("abac");
    case SpellActor::Effect::fire_clawpult:
        return FourCC::from_text("abfc");
    case SpellActor::Effect::rock_clawpult:
        return FourCC::from_text("abrc");
    case SpellActor::Effect::poison_dart:
        return FourCC::from_text("adfl");
    case SpellActor::Effect::pyromaniac_rocket:
        return FourCC::from_text("affl");
    case SpellActor::Effect::gnome_shoot:
        return FourCC::from_text("abkf");
    case SpellActor::Effect::tick_vortex_missile:
        return FourCC::from_text("atic");
    case SpellActor::Effect::baby_chuck:
        return FourCC::from_text("tbab");
    case SpellActor::Effect::gut_chuck:
        return FourCC::from_text("tgut");
    case SpellActor::Effect::hail_of_earth:
        return FourCC::from_text("abhe");
    default:
        return FourCC::from_text("cabi");
    }
}
void attach_hit(Match& match, SpellActor& spell, MatchObject* victim, Vec3 point) {
    auto& hit = SpellActor::launch_sacrifice_host(match, spell, SpellActor::Effect::spell_hit);
    hit.effect_target = victim;
    hit.motion.position = point;
    hit.run(match, 0);
}
SpellActor& launch_child(Match& match, SpellActor& source, SpellActor::Effect effect, MatchObject* target,
                         Vec3 position) {
    auto& child = SpellActor::launch_sacrifice_host(match, source, effect);
    child.owner = source.owner;
    child.effect_target = target;
    child.motion.position = position;
    child.point = position;
    child.spell_id = hop_name(source);
    child.data = source.data;
    if (source.allegiance)
        source.allegiance->attach(child);
    return child;
}
void impact_damage(Match& match, SpellActor& spell, MatchObject* victim, Vec3 point, bool splash) {
    if (splash)
        spell_damage(match, &spell, victim, spell.data->damage_type, spell.data->amount, spell.data->radius,
                     point);
    else
        spell_damage(match, &spell, victim, spell.data->damage_type, spell.data->amount, spell.data->radius);
}
bool splash(const SpellActor& spell) {
    return spell.data && spell.data->radius > 0.f;
}
void follow_particles(SpellActor& spell) {
    if (spell.particle_systems.empty())
        return;
    if (spell.effect == SpellActor::Effect::fire_clawpult)
        spell.particle_systems.front().set_frame({});
    else
        spell.particle_systems.front().set_frame({spell.motion.orientation, spell.motion.position});
}
int hold_particles(SpellActor& spell, int ticks) {
    if (!spell.particle_systems.empty()) {
        spell.age += static_cast<float>(ticks);
        auto& system = spell.particle_systems.front();
        system.set_position(spell.motion.position);
        system.advance(spell.age, spell.emission);
        if (system.alive() && spell.remaining <= 0)
            return 1;
    }
    return spell.remaining > 0 ? 1 : remove_spell(spell);
}
void vacuum_particles(Match& match, SpellActor& spell) {
    // VacuumEffectRun 0x4187f0: tsha/shrd, preset 129, 32 motes, then 0x439E50.
    auto& p = spell.particles(match);
    const auto tex = FourCC{static_cast<unsigned>(
        spell.data ? spell.data->integer(cc("tsha"), cc("shrd").value) : cc("shrd").value)};
    spell.particle_material(match, p, 0x0a011008u, tex);
    float radius = 0.5f, life = 1.f;
    p.set_property(129, &radius, &life);
    const float lo[3]{-0.5f, -0.5f, -0.5f}, hi[3]{0.5f, 0.5f, 0.5f};
    p.set_property(2, lo, hi);
    p.set_flags(0x600, 0);
    const float vel[3]{20.f, 0, 0};
    p.set_property(3, vel, vel);
    p.set_position(spell.motion.position);
    for (int i = 0; i < 32; ++i)
        p.emit(0, 1);
    p.set_flags(0x80000, 0x600);
    const float spread = spell.data->spread_limit;
    const float low[]{-spread * .5f, -spread * .5f, -spread * .25f},
        high[]{spread * .5f, spread * .5f, spread * .25f};
    p.set_property(2, low, high);
    spell.emission = 2;
    spell.sound_event(match, 4);
    auto& wave = SpellActor::launch_shockwave(match, spell, 5 / std::max(spread, .01f));
    auto& sphere = wave.shockwave;
    sphere.texture = FourCC{static_cast<unsigned>(spell.data->integer(cc("tsph"), cc("icec").value))};
    sphere.material = 0x02011008;
    sphere.axes = {spread, spread, spread};
    sphere.start_alpha = 96; // 0x439E50 a7.
    sphere.frames = match.texture_frames ? match.texture_frames(sphere.texture) : 1;
    wave.host.rate = 1.f / 90;
}
void blood_particles(Match& match, SpellActor& spell, float scale, int count = 0) {
    auto& p = spell.particles(match);
    spell.particle_material(match, p, 0x0a011008, "blud");
    p.set_position(spell.motion.position, &spell.motion.position);
    const float blood[]{scale, std::bit_cast<float>(count), std::bit_cast<float>(0)};
    p.set_property(133, blood, blood);
}
void gut_impact(Match& match, SpellActor& spell) {
    // GutChuckRun 0x419810 -> 0x439d00: eight thrown blood emitters and a blood cloud.
    for (int i = 0; i < 8; ++i) {
        auto& gib =
            launch_child(match, spell, SpellActor::Effect::gut_chuck_drip, nullptr, spell.motion.position);
        blood_particles(match, gib, .25f);
        const float low[]{-.01f, -.01f, -.01f}, high[]{.01f, .01f, .01f};
        gib.particle_systems.front().set_property(3, low, high);
        gib.motion.velocity = {(match.random().next() - 16384) * .00001516622179f,
                               (match.random().next() - 16384) * .00001516622179f,
                               match.random().next() * .00000457763671875f};
        gib.ability.clock = 1;
        gib.stage = 1;
        gib.emission = 1;
    }
    auto& cloud =
        launch_child(match, spell, SpellActor::Effect::gut_chuck_drip, nullptr, spell.motion.position);
    blood_particles(match, cloud, 1.2f);
    auto& p = cloud.particle_systems.front();
    const float size = 1.2f, life = 3, low[]{-.0005f, -.0005f, -.001f}, high[]{.0005f, .0005f, 0},
                box_low[]{-2, -2, -2}, box_high[]{2, 2, 2};
    p.set_property(129, &size, &life);
    p.set_property(3, low, high);
    p.set_property(2, box_low, box_high);
    for (int i = 0; i < 50; ++i)
        p.emit(0, 1);
    cloud.ability.clock = 2;
    cloud.stage = 1;
    cloud.emission = .1f;
    cloud.sound_requests.push_back({cc("Xgib"), cc("Xgib"), cloud.reference(), 0x40004});
}
int run_linger(Match& match, SpellActor& spell, int ticks) {
    if (!spell.stage) {
        if (spell.effect == SpellActor::Effect::vacuum_effect)
            vacuum_particles(match, spell);
        else if (spell.effect == SpellActor::Effect::gut_chuck_drip) {
            blood_particles(match, spell, .35f);
            spell.emission = .5f;
        }
        if (spell.remaining <= 0)
            spell.remaining = 90;
        spell.stage = 1;
    }
    if (spell.effect == SpellActor::Effect::gut_chuck_drip) {
        auto& p = spell.particle_systems.front();
        if (spell.ability.clock == 1) {
            // The eight fragments shed blood during their ballistic flight, then splatter on LAND.
            for (int i = 0; i < ticks && spell.emission > 0; ++i) {
                spell.motion.velocity.z -= .002725f;
                spell.motion.position.x += spell.motion.velocity.x;
                spell.motion.position.y += spell.motion.velocity.y;
                spell.motion.position.z += spell.motion.velocity.z;
                if (match.terrain) {
                    const float ground = static_cast<float>(
                        terrain_surface(*match.terrain, spell.motion.position.x, spell.motion.position.y));
                    if (spell.motion.position.z <= ground) {
                        spell.motion.position.z = ground;
                        if (spell.emission > .25f) {
                            const float low[]{-.05f, -.05f, 0}, high[]{.05f, .05f, .05f};
                            p.set_property(3, low, high);
                            blood_particles(match, spell, .25f, static_cast<int>(spell.emission * 32));
                        }
                        spell.emission = 0;
                    }
                }
                spell.emission = std::max(0.f, spell.emission - .01f);
            }
        } else if (spell.ability.clock == 2)
            spell.emission = std::max(0.f, spell.emission - ticks * .005f);
        else if (spell.effect_target && !(spell.effect_target->state_flags & MatchObject::removed_flag))
            spell.motion.position = spell.effect_target->source_motion()->position;
        else
            spell.emission = 0;
        spell.age += ticks;
        p.set_position(spell.motion.position);
        p.advance(spell.age, spell.emission);
        // 0x4194c0 enumerates only the projectile's dripping blood, not the impact cloud/fragments.
        if (!spell.ability.clock)
            p.enumerate([&](ParticleSystem::Particle& particle) {
                bool hit = false;
                for (unsigned group : {0u, 1u})
                    match.visit_group(group, 5, [&](MatchObject& object) {
                        auto* actor = living_unit(&object);
                        if (!hit && actor && !actor->friendly(&spell) &&
                            spell_box_distance_squared(particle.position, actor->motion,
                                                       particle.size * particle.size) <=
                                particle.size * particle.size) {
                            actor->damage(match, {spell.data->damage_type, 100, spell.reference()});
                            hit = true;
                        }
                        return 0;
                    });
                return hit ? 2 : 0;
            });
        return spell.emission > 0 || p.alive() ? 1 : remove_spell(spell);
    }
    spell.remaining -= ticks;
    if (spell.effect == SpellActor::Effect::vacuum_effect) {
        spell.cast_alpha = std::min(1.f, spell.cast_alpha + ticks / 30.f);
        const float radius = spell.data->spread_limit * spell.cast_alpha;
        if (spell.remaining > 0)
            for (unsigned group : {0u, 1u})
                match.visit_group(group, 5, [&](MatchObject& object) {
                    auto* actor = living_unit(&object);
                    if (!actor)
                        return 0;
                    Vec3 centre;
                    spell_target_point(centre, actor);
                    Vec3 direction{spell.motion.position.x - centre.x, spell.motion.position.y - centre.y,
                                   spell.motion.position.z - centre.z};
                    const float distance = std::sqrt(distance_squared(direction, {}));
                    if (distance > 0 && distance <= radius) {
                        direction = {direction.x / distance, direction.y / distance, direction.z / distance};
                        actor->motion.push_direction(direction, .15f, 7);
                    }
                    return 0;
                });
        else
            spell.emission = 0;
    }
    return hold_particles(spell, ticks);
}
template <typename Fn> void visit_foes(Match& match, MatchObject* source, Fn&& fn) {
    const auto visit = [&](MatchObject& object) {
        auto* actor = living_unit(&object);
        if (!actor || actor == source)
            return 0;
        if (!actor->enemy(source))
            return 0;
        fn(*actor);
        return 0;
    };
    match.visit_group(0, 5, visit);
    match.visit_group(1, 5, visit);
}
void end_flight(SpellActor& spell) {
    spell.emission = 0;
    spell.motion.flags = spell.motion.collision_mask = 0;
    spell.motion.velocity = {};
    spell.crossline.enabled = spell.sprite.enabled = false;
    spell.model_scale = 0;
    if (spell.effect != SpellActor::Effect::air_clawpult)
        spell.surface.opacity = 0;
    spell.light.enabled = false;
    spell.stage = 240;
}
void line_setup(SpellActor& spell, const char* fallback, float width, float length, bool additive = false) {
    auto& line = spell.crossline;
    line.enabled = true;
    line.texture = FourCC{static_cast<unsigned>(spell.data->integer(cc("ttex"), cc(fallback).value))};
    line.material = additive ? 0x1a012008 : 0x0a011008;
    line.points = {{spell.motion.position, width, true}, {spell.motion.position, width, false}};
    spell.ability.length = length;
}
void line_follow(Match& match, SpellActor& spell, int ticks) {
    auto& line = spell.crossline;
    if (!line.enabled || line.points.size() < 2)
        return;
    const auto v = spell.motion.velocity;
    const double length = std::sqrt(distance_squared(v, {}));
    const double scale = length > 0 ? -spell.ability.length / length : 0;
    line.points[0].position = spell.motion.position;
    line.points[1].position = {spell.motion.position.x + static_cast<float>(v.x * scale),
                               spell.motion.position.y + static_cast<float>(v.y * scale),
                               spell.motion.position.z + static_cast<float>(v.z * scale)};
    const unsigned frames = match.texture_frames ? std::max(match.texture_frames(line.texture), 1u) : 1u;
    line.frame = std::fmod(line.frame + ticks * .5f, static_cast<float>(frames));
}
int run_fire_bow(Match& match, SpellActor& spell, int ticks) {
    // 0x415C10/0x415A00: a five-point bow on the animated bones, then a two-point ballistic arrow.
    if (!spell.stage) {
        line_setup(spell, "wing", .025f, 1, true);
        spell.crossline.points.resize(5);
        spell.stage = 1;
    }
    if (spell.stage == 1) {
        auto* caster = dynamic_cast<CreatureActor*>(spell.owner);
        if (!caster || (caster->state_flags & 0x10))
            return remove_spell(spell);
        auto& points = spell.crossline.points;
        static constexpr unsigned bones[5]{15, 11, 16, 11, 17};
        static constexpr Vec3 offsets[5]{{0, -.25f, 0}, {}, {0, -.5f, 0}, {}, {0, .5f, 0}};
        for (unsigned i = 0; i < 5; ++i) {
            points[i] = {caster->motion.position, i == 1 || i == 3 ? .1f : .025f, i != 1 && i != 4};
            caster->write_bone_point(points[i].position, bones[i], nullptr, &offsets[i]);
        }
        if (!(caster->state_flags & 0x20000000))
            return 1;
        spell.motion.position = points.front().position;
        spell.ability.length = std::max(
            .5f, static_cast<float>(std::sqrt(distance_squared(points[0].position, points[1].position))));
        points.resize(2);
        points[0].connected = true;
        points[1].connected = false;
        spell.start_creature_projectile(match, 600, {}, 1, true, false);
    }
    line_follow(match, spell, ticks);
    if (const int flying = spell.fly_creature_projectile(match, ticks))
        return flying;
    if (spell.impact_target)
        spell_damage(match, &spell, spell.impact_target, spell.data->damage_type, spell.data->amount,
                     spell.data->radius);
    return remove_spell(spell);
}
void clawpult_particles(Match& match, SpellActor& spell) {
    if (spell.effect == SpellActor::Effect::air_clawpult) {
        // 0x418660: 24 rotating SPIKERAYS, with equal inner/outer tip lengths.
        spell.initialize_spike_surface(match, {.3f, .3f, .3f});
        spell.surface.parameters[1] = spell.surface.parameters[2] = spell.surface.parameters[0] + .9f;
        spell.surface.parameters[3] = 0;
        spell.surface.textures[0] =
            FourCC{static_cast<unsigned>(spell.data->integer(cc("ttex"), cc("iplt").value))};
        spell.surface.material = 0x0a011008;
        spell.surface.opacity = 1;
        spell.motion.angular_velocity = {.010471967f + match.visual_random.next() * .015707951f / 32768,
                                         .0052359835f + match.visual_random.next() * .0052359835f / 32768,
                                         .0052359835f + match.visual_random.next() * .0052359835f / 32768};
        spell.motion.flags |= 56;
        return;
    }
    auto& p = spell.particles(match);
    const auto tex = FourCC{static_cast<unsigned>(
        spell.data ? spell.data->integer(cc("ttex"), cc("iplt").value) : cc("iplt").value)};
    spell.particle_material(match, p, 0x0a011008u, tex);
    float radius = 0.5f, life = 1.5f;
    p.set_property(129, &radius, &life);
    p.set_frame({spell.motion.orientation, spell.motion.position});
    spell.emission = 1;
    for (int i = 0; i < 8; ++i)
        p.emit(0, 1);
}
int run_clawpult(Match& match, SpellActor& spell, int ticks) {
    // All five native bodies share 0x418420 (draw-back), 0x418520 (ballistic launch), 0x4185F0 (flight).
    if (spell.effect == SpellActor::Effect::baby_chuck && spell.stage == 241) {
        spell.host.time += ticks;
        auto& p = spell.particle_systems.front();
        p.advance(spell.host.time, spell.emission);
        spell.emission = std::max(0.f, spell.emission - ticks * spell.host.rate);
        return spell.emission > 0 || p.alive() ? 1 : remove_spell(spell);
    }
    if (!spell.stage) {
        if (spell.effect == SpellActor::Effect::rock_clawpult) {
            spell.set_projectile_model(
                match, FourCC{static_cast<unsigned>(spell.data->integer(cc("cmod"), cc("rokm").value))}, .5f);
        } else if (spell.effect == SpellActor::Effect::baby_chuck ||
                   spell.effect == SpellActor::Effect::gut_chuck) {
            spell.sprite.enabled = true;
            spell.sprite.texture = FourCC{static_cast<unsigned>(spell.data->integer(
                cc("ttex"), cc(spell.effect == SpellActor::Effect::baby_chuck ? "gfrg" : "guts").value))};
            spell.sprite.size = 1.5f;
            if (spell.effect == SpellActor::Effect::gut_chuck) {
                auto& drip = launch_child(match, spell, SpellActor::Effect::gut_chuck_drip, &spell,
                                          spell.motion.position);
                drip.remaining = 600;
                drip.run(match, 0);
            }
        } else {
            clawpult_particles(match, spell);
            if (spell.effect == SpellActor::Effect::fire_clawpult) {
                auto& p = spell.particle_systems.front();
                p.clear(128);
                spell.particle_material(
                    match, p, spell.data->integer(cc("iadd"), 1) ? 0x1a012008 : 0x0a011008,
                    FourCC{static_cast<unsigned>(spell.data->integer(cc("ttex"), cc("roar").value))});
                const float size = .5f, life = 1.5f;
                p.set_property(132, &size, &life);
                p.set_position({}); // The burning ball is local to the moving clawpult shot.
                spell.light = {true, spell.motion.position, {1, .5f, 0}, 0, 1, 15};
            }
        }
        spell.stage = 1;
        spell.sound_event(match, 1);
    }
    if (spell.effect == SpellActor::Effect::air_clawpult) {
        auto& surface = spell.surface;
        const float goal = spell.stage >= 240 ? 0.f : spell.stage == 1 ? .5f : 1.f;
        surface.parameters[3] += std::clamp(goal - surface.parameters[3], -ticks / 30.f, ticks / 30.f);
        surface.frame += ticks * .26666667f;
        for (auto& ray : surface.rays_data)
            ray.phase -= ticks * ray.speed;
        if (spell.stage >= 240)
            return surface.parameters[3] > 0 ? 1 : remove_spell(spell);
    }
    if (spell.stage == 1) {
        auto* caster = dynamic_cast<CreatureActor*>(spell.owner);
        if (!caster || (caster->state_flags & 0x10)) {
            end_flight(spell);
            if (spell.effect == SpellActor::Effect::air_clawpult)
                return 1;
        } else {
            Vec3 from = caster->motion.position, to = from;
            caster->write_reference_point(from, 0, true);
            caster->write_reference_point(to, 1, true);
            const float t = spell.ability.draw_back;
            spell.motion.position = {from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t,
                                     from.z + (to.z - from.z) * t};
            spell.ability.draw_back = std::min(1.f, t + .05f);
            if (spell.model_id.value)
                spell.model_scale = t * 1.5f + .5f;
            follow_particles(spell);
            spell.age += ticks;
            if (!spell.particle_systems.empty())
                spell.particle_systems.front().advance(spell.age, spell.emission);
            if (caster->state_flags & 0x20000000)
                spell.stage = 16;
            return 1;
        }
    }
    if (spell.stage == 16) {
        spell.start_creature_projectile(match, 600, spell.model_id, 2, true, false);
        if (spell.effect == SpellActor::Effect::rock_clawpult)
            // 0x418d70 stage 16: model scale 2, then 0x4380A0(emission 1.25, life .75) tRT1 trail.
            SpellActor::launch_model_trail(
                match, spell, 0x0a011008,
                FourCC{static_cast<unsigned>(spell.data->integer(cc("tRT1"), cc("dust").value))}, 1.25f, .75f);
    }
    spell.sprite.frame += ticks * .26666667f;
    spell.light.position = spell.motion.position;
    spell.light.intensity = spell.stage == 81 ? std::max(0.f, spell.light.intensity - ticks / 30.f)
                                              : std::min(1.f, spell.light.intensity + ticks / 30.f);
    if (spell.stage == 81) {
        // FireClawpult 0x419000: burn/damage continues around the actual impact, for duration * 60.
        const float reach = spell.data->spread_limit * 1.25f;
        for (unsigned group : {0u, 1u, 2u})
            match.visit_group(group, 21, [&](MatchObject& object) {
                const auto* body = object.source_motion();
                if (!body ||
                    spell_box_distance_squared(spell.motion.position, *body, reach * reach) > reach * reach)
                    return 0;
                if (auto* actor = living_unit(&object)) {
                    actor->accumulate_damage({spell.data->damage_type, 75, spell.reference()}, ticks);
                    actor->burn_near(spell.motion.position, spell.data->spread_limit, 60);
                } else if (auto* building = dynamic_cast<BuildingActor*>(&object))
                    building->accumulate_damage(75, ticks, spell.reference());
                return 0;
            });
        spell.age += ticks;
        spell.particles(match).advance(spell.age, 0);
        if ((spell.remaining -= ticks) <= 0)
            end_flight(spell);
        return 1;
    }
    if (const int flying = spell.fly_creature_projectile(match, ticks))
        return flying;
    auto* victim = spell.impact_target;
    if (spell.effect == SpellActor::Effect::air_clawpult || spell.effect == SpellActor::Effect::baby_chuck ||
        spell.effect == SpellActor::Effect::gut_chuck)
        spell_damage(match, &spell, victim, spell.data->damage_type, spell.data->amount, spell.data->radius);
    else // The rock and fire clawpult impacts (0x418d70 / 0x419050 stage 0x50) spawn no 0x438FF0 hit burst.
        impact_damage(match, spell, victim, spell.motion.position, splash(spell));
    switch (spell.effect) {
    case SpellActor::Effect::gut_chuck:
        gut_impact(match, spell);
        break;
    case SpellActor::Effect::air_clawpult: {
        // AirClawpultStart 0x418aa0 stage 0x50: 0x44D7E0, 0x439400 shard burst, then VacuumEffectRun.
        SpellActor::launch_fragment_burst(match, spell, cc("shrd"), 64, 10);
        auto& vacuum =
            launch_child(match, spell, SpellActor::Effect::vacuum_effect, victim, spell.motion.position);
        vacuum.remaining = 90;
        vacuum.run(match, 0);
        break;
    }
    case SpellActor::Effect::rock_clawpult: {
        // 0x418d70 stage 0x50: 0x492230(mask 5, spread) knocks every unit down (0x418D40 -> 0x4591A0(0, 10)).
        const float reach = spell.data->spread_limit;
        for (unsigned group : {0u, 1u})
            match.visit_group(group, 5, [&](MatchObject& object) {
                if (auto* actor = living_unit(&object);
                    actor && distance_squared(actor->motion.position, spell.motion.position) <= reach * reach)
                    actor->knock_down(0, 10);
                return 0;
            });
        match.shake(spell.motion.position, 16, 100);
        SpellActor::launch_rock_shatter(match, spell);
        break;
    }
    case SpellActor::Effect::fire_clawpult: {
        // FireClawpultStart 0x419050 stage 0x50: 0x439F10 shockwave, then 0x437540(0.75, spell,
        // spread * 0.5, spread, duration, rate 1.5), i.e. 0x4375A0 'firy' with size
        // clamp(spread * 0.15, 0.25, 2) (0x44CB00), height spread, life 2 and a spread * 0.5 box. The shot's
        // own fireball stops emitting and its light fades; stage 0x51 burns the area (0x419000).
        SpellActor::launch_shockwave(match, spell, spell.data->spread_limit);
        const float spread = spell.data->spread_limit;
        SpellActor::launch_inert_fire(match, spell, cc("firy"), std::clamp(spread * .15f, .25f, 2.f), spread, 2,
                                      1.5f, spell.data->duration, spread * .5f);
        spell.particles(match);
        spell.motion.flags = spell.motion.collision_mask = 0;
        spell.motion.velocity = {};
        spell.remaining = static_cast<int>(spell.data->duration * 60);
        spell.emission = 0;
        spell.stage = 81;
        return 1;
    }
    case SpellActor::Effect::baby_chuck: {
        // BabyChuckRun 0x419ad0 -> 0x439690: a fading splatter emitter, not a fire sphere.
        auto& p = spell.particles(match);
        spell.particle_material(
            match, p, 0x0a011008,
            FourCC{static_cast<unsigned>(spell.data->integer(cc("txpl"), cc("splt").value))});
        const float radius = spell.data->real(cc("fxpr"), 2),
                    duration = std::max(.01f, spell.data->real(cc("fxpt"), 5)),
                    // 0x44CA80(box, .3, 2, .25): half of .3 times the +-radius box diagonal, clamped.
                    size = std::clamp(2 * radius * std::sqrt(3.f) * .15f, .25f, 2.f), life = duration * .7f;
        p.set_property(129, &size, &life);
        const float low[]{-radius, -radius, -radius}, high[]{radius, radius, radius};
        p.set_property(2, low, high);
        p.set_position(spell.motion.position);
        spell.motion.flags = spell.motion.collision_mask = 0;
        spell.motion.velocity = {};
        spell.sprite.enabled = false;
        spell.host.time = 0;
        spell.host.rate = 1 / (duration * .3f * 60);
        spell.emission = 1;
        spell.stage = 241;
        return 1;
    }
    default:
        break;
    }
    return remove_spell(spell);
}
int run_poison_dart(Match& match, SpellActor& spell, int ticks) {
    // Stage 32: vtable +144 poison, then 0x44D7A0.
    if (!spell.stage) {
        spell.start_creature_projectile(match, static_cast<int>(spell.data->range / spell.data->speed) + 30,
                                        {}, 1, false);
        spell.motion.collision_mask = 0x114015;
        spell.friendly_ignore_time = 0;
        line_setup(spell, "lzr2", spell.data->real(cc("fwid"), .4f), spell.data->real(cc("flen"), 1));
    }
    line_follow(match, spell, ticks);
    if (const int flying = spell.fly_creature_projectile(match, ticks))
        return flying;
    auto* victim = living_unit(spell.impact_target);
    if (victim)
        SpellActor::poison(match, *victim, spell.owner, *spell.data);
    return remove_spell(spell);
}
int run_rocket(Match& match, SpellActor& spell, int ticks) {
    // Stage 32: 0x45CB90 burn for duration * 60, then remaining countdown.
    if (!spell.stage) {
        spell.start_creature_projectile(match, static_cast<int>(spell.data->range / spell.data->speed) + 30,
                                        {}, 1, false);
        line_setup(spell, "lzr2", spell.data->real(cc("fwid"), .4f), spell.data->real(cc("flen"), 1));
        SpellActor::launch_generic_trail(
            match, spell, 0x0a011008,
            FourCC{static_cast<unsigned>(spell.data->integer(cc("tRT1"), cc("smok").value))}, .5f, 1, 1, 0,
            0);
        spell.light = {true, spell.motion.position, {1, .5f, 0}, 1, 1, 25};
    }
    if (spell.stage == 33) {
        auto* victim = living_unit(spell.effect_target);
        if (victim && spell.data->duration > 0) {
            victim->accumulate_damage({spell.data->damage_type,
                                       static_cast<int>(spell.data->amount / spell.data->duration),
                                       spell.reference()},
                                      ticks);
        }
        if (!victim || (spell.remaining -= ticks) <= 0)
            end_flight(spell);
        return 1;
    }
    line_follow(match, spell, ticks);
    spell.light.position = spell.motion.position;
    if (const int flying = spell.fly_creature_projectile(match, ticks))
        return flying;
    spell.effect_target = spell.impact_target;
    if (auto* victim = living_unit(spell.effect_target))
        victim->burn_near(spell.motion.position, 5, static_cast<int>(spell.data->duration * 60));
    // 0x415d90 stage 0x20: 0x4375A0(0.25, spell, 'firy', 0.35, 2, 0.75, rate 2, 0.5 s).
    SpellActor::launch_inert_fire(match, spell, cc("firy"), .35f, 2, .75f, 2, .5f);
    spell.crossline.enabled = false;
    spell.motion.flags = spell.motion.collision_mask = 0;
    spell.remaining = static_cast<int>(spell.data->duration * 60);
    spell.stage = 33;
    return 1;
}
int run_gnome(Match& match, SpellActor& spell, int ticks) {
    // 0x4164A0: GenericShoot's aim/scatter, but trace 0x44CD10 and disable motion; no flying pebble.
    if (!spell.stage) {
        spell.start_creature_projectile(match, 1, {}, 1, false);
        const float factor = spell.data->range / spell.data->speed;
        spell.point = {spell.motion.position.x + spell.motion.velocity.x * factor,
                       spell.motion.position.y + spell.motion.velocity.y * factor,
                       spell.motion.position.z + spell.motion.velocity.z * factor};
        spell.impact_target = spell_trace_ray(match, spell, spell.motion.position, spell.point);
        spell.motion.flags = spell.motion.collision_mask = 0;
        spell.stage = 1;
        // 0x438BC0 always spawns the type-60 muzzle flash (0x438AD0 / 0x438A40): 'tMFL' (default 'mfls'),
        // additive unless iMF+ is 0, an fMFx-wide MUZZLE square facing the shot, stepping its frames over fMFt
        // seconds without fading.
        auto& line = spell.crossline;
        line.enabled = line.muzzle = true;
        line.texture = FourCC{static_cast<unsigned>(spell.data->integer(cc("tMFL"), cc("mfls").value))};
        line.material = spell.data->integer(cc("iMF+"), 1) ? 0x1a012008 : 0x0a011008;
        line.points = {{spell.motion.position, spell.data->real(cc("fMFx"), 2), false},
                       {spell.point, 0, false}};
        spell.remaining = std::max(1, static_cast<int>(spell.data->real(cc("fMFt"), .5f) * 60));
        return 1;
    }
    if (spell.crossline.enabled) {
        spell.host.time += ticks;
        const auto frames =
            std::max(match.texture_frames ? match.texture_frames(spell.crossline.texture) : 1u, 1u);
        spell.crossline.frame =
            std::min(static_cast<float>(frames - 1), spell.host.time * frames / spell.remaining);
    }
    if (spell.stage == 240)
        return spell.host.time < spell.remaining ? 1 : remove_spell(spell);
    if (spell.stage == 1) {
        spell.stage = 32;
        return 1;
    }
    spell.motion.position = spell.point;
    spell.sound_event(match, 4);
    // 0x4164A0 stage 16: 0x44D7E0, then the 0x438FF0 burst only on a unit or where the ray met the ground.
    auto* victim = spell.impact_target;
    if (splash(spell))
        spell_damage(match, &spell, victim, spell.data->damage_type, spell.data->amount, spell.data->radius,
                     spell.motion.position);
    else
        spell_damage(match, &spell, victim, spell.data->damage_type, spell.data->amount, spell.data->radius);
    if (victim ||
        (match.terrain && std::abs(terrain_surface(*match.terrain, spell.point.x, spell.point.y) - spell.point.z) < 1))
        attach_hit(match, spell, victim, spell.motion.position);
    spell.stage = 240;
    return 1;
}
int run_tick_missile(Match& match, SpellActor& spell, int ticks) {
    // Stage 16: 0x44D7E0, then TickVortex 0x41d860 via 0x44E850.
    if (!spell.stage) {
        spell.start_creature_projectile(match, static_cast<int>(spell.data->range / spell.data->speed), {}, 1,
                                        false);
        auto& p = spell.particles(match);
        spell.particle_material(
            match, p, 0x1a012008,
            FourCC{static_cast<unsigned>(spell.data->integer(cc("ttex"), cc("tube").value))});
        const float size = .3f, life = 2;
        p.set_property(134, &size, &life);
        p.set_frame({spell.motion.orientation, spell.motion.position});
        spell.emission = 1;
    }
    if (const int flying = spell.fly_creature_projectile(match, ticks))
        return flying;
    auto* victim = spell.impact_target;
    impact_damage(match, spell, victim, spell.motion.position, splash(spell)); // 0x41D420 stage 16: no hit burst.
    auto& vortex = launch_child(match, spell, SpellActor::Effect::tick_vortex, victim, spell.motion.position);
    vortex.remaining = 90;
    vortex.next_pulse = 16;
    vortex.run(match, 1);
    end_flight(spell);
    return 1;
}
int run_tick_vortex(Match& match, SpellActor& spell, int ticks) {
    // TickVortex 0x41d860: 0x492230 visit as the radius grows.
    if (!spell.stage) {
        auto& p = spell.particles(match);
        spell.particle_material(match, p, 0x1a012008, "cst0");
        const float size = 1, life = 1.5f;
        p.set_property(132, &size, &life);
        p.set_flags(0x400, 0x400000);
        spell.emission = 1;
        spell.motion.angular_velocity.x = .0523598f;
        spell.motion.flags = 8;
        spell.stage = 1;
    }
    spell.age += ticks;
    const float t = std::min(1.f, spell.age / 60.f);
    const float radius = spell.data->spread_limit * (1 + t * t * t), height = 5 * t;
    if (t < 1) {
        for (unsigned group = 0; group < 2; ++group)
            match.visit_group(group, 5, [&](MatchObject& object) {
                auto* actor = living_unit(&object);
                if (!actor || (actor->motion.flags & 0x10000) ||
                    distance_squared(actor->motion.position, spell.motion.position) >
                        (radius + 2) * (radius + 2))
                    return 0;
                const float x = actor->motion.position.x - spell.motion.position.x,
                            y = actor->motion.position.y - spell.motion.position.y;
                const float distance = std::sqrt(x * x + y * y);
                if (distance > 0) {
                    const float c = std::cos(.0523598f), s = std::sin(.0523598f);
                    const Vec3 target{spell.motion.position.x + (x * c - y * s) * radius / distance,
                                      spell.motion.position.y + (x * s + y * c) * radius / distance,
                                      spell.motion.position.z + height + .5f};
                    spell_approach_velocity(actor->motion, target, ticks * .05f, .2f);
                    actor->motion.angular_velocity.x = .0523598f;
                }
                return 0;
            });
    } else {
        spell.emission = 0;
    }
    auto& p = spell.particle_systems.front();
    const float low[3]{-radius, -radius, height}, high[3]{radius, radius, height};
    p.set_property(2, low, high);
    p.set_frame({}); // TickVortex's local particles inherit the spinning host at draw.
    p.advance(spell.age, spell.emission);
    return spell.emission > 0 || p.alive() ? 1 : remove_spell(spell);
}
void hail_particles(Match& match, SpellActor& spell) {
    // HailOfEarth 0x41A400: ttex/rock, preset 131, 16 NewParticle, velocity along the caster frame.
    auto& p = spell.particles(match);
    const auto tex = FourCC{static_cast<unsigned>(spell.data->integer(cc("ttex"), cc("rock").value))};
    const auto additive = spell.data->integer(cc("iadd"), 0);
    spell.particle_material(match, p, additive ? 0x1a012008u : 0x0a011008u, tex);
    const float size = spell.data->real(cc("fsiz"), 0.35f);
    const float speed = spell.data->speed > 0.f ? spell.data->speed : 0.6f;
    const float range = spell.data->range > 0.f ? spell.data->range : 40.f;
    const float life = range * 1.2f / (speed * 60.f);
    p.set_property(131, &size, &life);
    const float end = spell.data->real(cc("fdsz"), 1.f);
    const float end_lo = end * 0.9f, end_hi = end * 1.1f;
    p.set_property(6, &end_lo, &end_hi);
    const float white = 255.f;
    p.set_property(14, &white, &white);
    const float fang = spell.data->real(cc("fang"), 5.f);
    const float spread = speed * std::sin(fang * 0.017453278f);
    const float vmin[3]{-spread, speed * 0.9f, -spread}, vmax[3]{spread, speed, spread};
    p.set_property(3, vmin, vmax);
    p.set_flags(0x20000, 0x20);
    if (auto* caster = dynamic_cast<CreatureActor*>(spell.owner)) {
        caster->write_reference_point(spell.motion.position, 0, true);
        if (match.terrain)
            spell_lead_target(spell.point, spell.effect_target, speed, spell.motion, *match.terrain);
        else
            spell_target_point(spell.point, spell.effect_target);
        Vec3 direction{spell.point.x - spell.motion.position.x, spell.point.y - spell.motion.position.y,
                       spell.point.z - spell.motion.position.z};
        if (distance_squared(direction, {}) > 0) {
            spell_scatter(direction, caster->stats.current[8], match.random());
            spell.motion.orientation = spell_orientation(direction); // 0x41A310: aim in all three axes.
        }
    }
    p.set_frame({spell.motion.orientation, spell.motion.position});
    spell.remaining = std::max(static_cast<int>(life * 60.f), 1);
    spell.emission = 1;
    spell.host.rate = spell.data->real(cc("fvig"), 0.05f);
    spell.host.fade = std::sin(fang * 0.017453278f);
    spell.host.time = 0; // Distance travelled by the cone front, +64 in 0x41A750.
    for (int i = 0; i < 16; ++i)
        p.emit(0, 1);
    p.advance(0, spell.emission);
}
int run_hail(Match& match, SpellActor& spell, int ticks) {
    // HailOfEarth 0x41a7e0 / 0x41A750: spray from the caster at unmarked foes in range.
    if (!spell.stage && spell.owner && spell.owner->source_motion()) {
        spell.motion.position = spell.owner->source_motion()->position;
        spell.motion.orientation = spell.owner->source_motion()->orientation;
    } else if (!spell.stage)
        aim_point(spell);
    if (!spell.stage) {
        hail_particles(match, spell);
        spell.sound_event(match, 2);
        spell.stage = 1;
    }
    if (ticks > 0) {
        const float speed = spell.data->speed > 0 ? spell.data->speed : .6f;
        const float step = ticks * speed * 1.2f;
        spell.host.time += step;
        const auto& frame = spell.motion.orientation;
        // 0x492550: a moving circular slab, tested against each collision record's bounding sphere.
        for (unsigned group = 0; group < 3; ++group)
            match.visit_group(group, 21, [&](MatchObject& object) {
                if (&object == spell.owner || object.named(hop_name(spell)) || !object.source_motion())
                    return 0;
                if (match.projectile_friendly_fire && spell.allegiance &&
                    object.allegiance == spell.allegiance && &object != spell.effect_target)
                    return 0;
                const auto& m = *object.source_motion();
                bool intersects = false;
                for (const auto& box : m.collision_boxes) {
                    const Vec3 center{m.position.x + (box.minimum.x + box.maximum.x) * .5f,
                                      m.position.y + (box.minimum.y + box.maximum.y) * .5f,
                                      m.position.z + (box.minimum.z + box.maximum.z) * .5f};
                    const Vec3 d{center.x - spell.motion.position.x, center.y - spell.motion.position.y,
                                 center.z - spell.motion.position.z};
                    const double along = d.x * frame[3] + d.y * frame[4] + d.z * frame[5];
                    const double radius = std::sqrt(distance_squared(box.minimum, box.maximum)) * .35355;
                    const double radial = std::max(0., distance_squared(d, {}) - along * along);
                    const double width = radius + spell.host.time * spell.host.fade;
                    if (std::abs(along - spell.host.time) <= radius + step && radial <= width * width) {
                        intersects = true;
                        break;
                    }
                }
                if (!intersects)
                    return 0;
                Vec3 center;
                spell_target_point(center, &object);
                if (match.terrain) {
                    Vec3 hit;
                    const Vec3 d{center.x - spell.motion.position.x, center.y - spell.motion.position.y,
                                 center.z - spell.motion.position.z};
                    const float fraction = terrain_intercept(*match.terrain, spell.motion.position, d, hit);
                    if (fraction > 0 && fraction < 1)
                        return 0; // 0x44DF90: land occludes the spray.
                }
                object.name_reference(hop_name(spell), &spell);
                if (spell.data->integer(cc("thit"), cc("dirt").value) == static_cast<int>(cc("burn").value))
                    if (auto* actor = dynamic_cast<CreatureActor*>(&object))
                        actor->burn_near(center, 1, 30); // Pyrodactyl, 0x41A660.
                spell_damage(match, &spell, &object, spell.data->damage_type, spell.data->amount,
                             spell.data->radius, center);
                return 0;
            });
    }
    if (!spell.particle_systems.empty()) {
        auto& p = spell.particle_systems.front();
        p.set_frame({spell.motion.orientation, spell.motion.position});
        spell.age += static_cast<float>(ticks);
        p.advance(spell.age, spell.emission);
        spell.emission = std::max(0.f, spell.emission - static_cast<float>(ticks) * spell.host.rate);
    }
    spell.remaining -= ticks;
    if (spell.remaining <= 0 && (spell.particle_systems.empty() || !spell.particle_systems.front().alive()))
        return remove_spell(spell);
    return 1;
}
} // namespace
int SpellActor::run_ability(Match& match, int ticks) {
    if (!data)
        throw Error("Creature ability requires its SPEL definition");
    switch (effect) {
    case Effect::fire_bow:
        return run_fire_bow(match, *this, ticks);
    case Effect::air_clawpult:
    case Effect::fire_clawpult:
    case Effect::rock_clawpult:
    case Effect::baby_chuck:
    case Effect::gut_chuck:
        return run_clawpult(match, *this, ticks);
    case Effect::poison_dart:
        return run_poison_dart(match, *this, ticks);
    case Effect::pyromaniac_rocket:
        return run_rocket(match, *this, ticks);
    case Effect::gnome_shoot:
        return run_gnome(match, *this, ticks);
    case Effect::tick_vortex_missile:
        return run_tick_missile(match, *this, ticks);
    case Effect::hail_of_earth:
        return run_hail(match, *this, ticks);
    case Effect::tick_vortex:
        return run_tick_vortex(match, *this, ticks);
    case Effect::vacuum_effect:
    case Effect::gut_chuck_drip:
        return run_linger(match, *this, ticks);
    default:
        throw Error("Invalid creature projectile ability");
    }
}
} // namespace sacrifice
