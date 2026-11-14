#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include "sacrifice/spell_data.hpp"
#include "sacrifice/spell_registry.hpp"
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
    else if (spell.owner && spell.owner->source_motion())
        spell.motion.position = spell.owner->source_motion()->position;
    else
        spell.motion.position = spell.point;
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
    child.data = source.data;
    if (source.allegiance)
        source.allegiance->attach(child);
    return child;
}
int hit_amount(const SpellActor& spell, int fallback) {
    return spell.data && spell.data->amount ? spell.data->amount : fallback;
}
int hit_type(const SpellActor& spell, int fallback) {
    return spell.data && spell.data->damage_type ? spell.data->damage_type : fallback;
}
void strike(Match& match, SpellActor& spell, MatchObject* victim, Vec3 point, bool splash,
            int fallback_amount = 200, int fallback_type = 51) {
    const int type = hit_type(spell, fallback_type);
    const int amount = hit_amount(spell, fallback_amount);
    if (splash)
        spell_damage(match, &spell, victim, type, amount, spell.data->radius, point);
    else
        spell_damage(match, &spell, victim, type, amount, spell.data->radius);
    attach_hit(match, spell, victim, point);
}
FourCC boulder_trail(const SpellActor& spell) { // 0x4380A0: tRT1, default 'dust'.
    return FourCC{static_cast<unsigned>(spell.data->integer(FourCC::from_text("tRT1"), FourCC::from_text("dust").value))};
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
int run_icespray(Match& match, SpellActor& spell, int ticks) {
    // SilverbackIcespray 0x413dd0: preset 132, then GenericStaticPsysRun 0x438900.
    if (!spell.stage) {
        if (!spell.effect_target) {
            spell.state_flags |= MatchObject::removed_flag;
            return -2;
        }
        auto& p = spell.particles(match);
        const FourCC texture{static_cast<std::uint32_t>(
            spell.data ? spell.data->integer(FourCC::from_text("ttx2"), FourCC::from_text("cst0").value)
                       : FourCC::from_text("cst0").value)};
        spell.particle_material(match, p, 0x1a012008, texture);
        // 0x44C230 makes the breath (the parent) the spray's +476: copy its aim frame, reuse its particle life
        // (range + 5) / (speed * 60), and last .5 / rate ticks, i.e. until the sweep is half way out.
        const float range = spell.data->range + 5, speed = std::max(spell.data->speed, .001f);
        float scale = 0.35f, life = range / (speed * 60);
        if (const auto* body = spell.effect_target->source_motion()) {
            spell.motion.orientation = body->orientation;
            spell.motion.position = body->position;
        }
        p.set_property(132, &scale, &life);
        const float cone = speed * spell.data->spread_limit * .5f / std::max(spell.data->range, .001f);
        const float low[3]{-cone, speed * .9f, -cone}, high[3]{cone, speed * 1.2f, cone};
        p.set_property(3, low, high);
        // Native copies the breath's frame into the host model frame (+404, 0x30 bytes) and sets no
        // particle frame: the spray emits in that local frame, which the renderer applies.
        spell.remaining = static_cast<int>(.5f * range / speed);
        spell.emission = 1;
        spell.stage = 1;
        match.assign(spell, SpellActor::generic_static_function);
        return spell.run_generic_particles(match, ticks);
    }
    return spell.run_generic_particles(match, ticks);
}
int run_breath(Match& match, SpellActor& spell, int ticks) {
    auto& p = spell.particles(match);
    const float range = spell.data->range + 5, speed = spell.data->speed;
    if (!spell.stage) {
        if (auto* caster = dynamic_cast<CreatureActor*>(spell.owner))
            caster->write_reference_point(spell.motion.position, 0, true);
        if (match.terrain)
            spell_lead_target(spell.point, spell.effect_target, speed, spell.motion, *match.terrain);
        spell.motion.orientation = spell_orientation({spell.point.x - spell.motion.position.x,
                                                      spell.point.y - spell.motion.position.y,
                                                      spell.point.z - spell.motion.position.z});
        spell.particle_material(match, p, 0x1a012008,
                                FourCC{static_cast<unsigned>(spell.data->integer(
                                    FourCC::from_text("ttx1"), FourCC::from_text("tube").value))});
        const float size = .3f, life = range / (speed * 60), end = spell.data->spread_limit / size;
        p.set_property(131, &size, &life);
        const float velocity[]{0, speed, 0};
        p.set_property(3, velocity, velocity);
        p.set_property(6, &end, &end);
        p.set_flags(0, 0x10020);
        if (spell.owner && spell.owner->source_motion()) {
            auto& spray = launch_child(match, spell, SpellActor::Effect::silverback_icespray, &spell,
                                       spell.motion.position);
            spray.run(match, 0);
        }
        spell.age = 0;
        spell.emission = 1;
        spell.stage = 16;
        spell.sound_event(match, 1);
    }
    const float previous = spell.age;
    spell.age = std::min(1.f, spell.age + ticks * speed / range);
    const float width = .3f + (spell.data->spread_limit - .3f) * spell.age;
    if (previous < 1 && ticks > 0) {
        const auto& frame = spell.motion.orientation;
        const float along = spell.data->range * spell.age, slab = ticks * speed;
        for (unsigned group = 0; group < 3; ++group)
            match.visit_group(group, 21, [&](MatchObject& object) {
                if (&object == spell.owner || !object.source_motion())
                    return 0;
                if (match.projectile_friendly_fire && object.friendly(&spell) &&
                    &object != spell.effect_target)
                    return 0;
                bool contact = false;
                const auto& m = *object.source_motion();
                for (const auto& box : m.collision_boxes) {
                    const Vec3 d{
                        m.position.x + (box.minimum.x + box.maximum.x) * .5f - spell.motion.position.x,
                        m.position.y + (box.minimum.y + box.maximum.y) * .5f - spell.motion.position.y,
                        m.position.z + (box.minimum.z + box.maximum.z) * .5f - spell.motion.position.z};
                    const double y = d.x * frame[3] + d.y * frame[4] + d.z * frame[5],
                                 sphere = std::sqrt(distance_squared(box.minimum, box.maximum)) * .35355,
                                 radial = distance_squared(d, {}) - y * y;
                    if (std::abs(y - along) <= sphere + slab && radial <= (width + sphere) * (width + sphere))
                        contact = true;
                }
                if (!contact)
                    return 0;
                const bool struck = object.named(spell.spell_id);
                Vec3 end;
                spell_target_point(end, &object);
                if (match.terrain) {
                    Vec3 hit;
                    const float t =
                        terrain_intercept(*match.terrain, spell.motion.position,
                                          {end.x - spell.motion.position.x, end.y - spell.motion.position.y,
                                           end.z - spell.motion.position.z},
                                          hit);
                    if (t > 0 && t < 1)
                        return 0;
                }
                // 0x414070: every contact pushes along the aim (0x44DD30 .1, 3); only the first damages and freezes.
                if (auto* actor = living_unit(&object))
                    actor->motion.push_direction({frame[3], frame[4], frame[5]}, .1f, 3);
                if (struck)
                    return 0;
                object.name_reference(spell.spell_id, &spell);
                spell_damage(match, &spell, &object, spell.data->damage_type, spell.data->amount,
                             spell.data->radius, end);
                if (auto* actor = living_unit(&object)) {
                    if (const auto* freeze = match.spell_registry
                                                 ? match.spell_registry->find(FourCC::from_text("frez"))
                                                 : nullptr;
                        freeze && actor->allegiance)
                        SpellActor::launch_side_cast(match, *actor->allegiance, *freeze, actor,
                                                     actor->motion.position);
                }
                return 0;
            });
    }
    spell.host.time += ticks;
    p.set_frame({spell.motion.orientation, spell.motion.position});
    p.advance(static_cast<float>(spell.host.time), spell.age <= .25f ? 1.f : 0.f);
    if (spell.age >= 1 && !p.alive()) {
        spell.stop_sound_event(1);
        return remove_spell(spell);
    }
    return 1;
}
void rhinoc_spike(Match& match, SpellActor& spell, Vec3 root) {
    if (!match.terrain)
        return;
    root.z = static_cast<float>(terrain_surface(*match.terrain, root.x, root.y));
    if (root.z < 0)
        return;
    const auto random = [&](float low, float high) {
        return low + match.visual_random.next() * (high - low) / 32768;
    };
    spell.add_vine(root, {root.x + random(-5, 5), root.y + random(-5, 5), root.z + random(5, 10)},
                   random(2, 5));
    auto& spike = spell.surface.vines_data.back();
    spike.speed = random(.0628318f, .07853975f);
    spike.curve[1] = {}; // Per-spike age; independent of the moving ground-wave host.
}
int run_rhinoc(Match& match, SpellActor& spell, int ticks) {
    // 0x414b60 / 0x414f50: a traveling SPIKES trail, followed by rings every ten ticks.
    if (!spell.stage) {
        if (spell.owner && spell.owner->source_motion()) {
            const auto& m = *spell.owner->source_motion();
            spell.motion.position = {m.position.x + m.orientation[3] * 2, m.position.y + m.orientation[4] * 2,
                                     m.position.z};
        }
        spell.start_creature_projectile(match, static_cast<int>(spell.data->range / spell.data->speed) + 10,
                                        {}, 1, false, false);
        spell.motion.flags = spell.motion.collision_mask = 0;
        spell.surface.kind = SpellActor::CurvedSurface::spikes;
        spell.surface.textures[0] = FourCC{static_cast<unsigned>(
            spell.data->integer(FourCC::from_text("ttex"), FourCC::from_text("mud1").value))};
        spell.surface.material = 0x00010001;
        spell.surface.opacity = 1;
        spell.stage = 16;
        spell.host.rate = 0;
    }
    for (auto& spike : spell.surface.vines_data) {
        spike.curve[1].x += ticks * spike.speed;
        // Grow, briefly hold, and sink back into the ground.
        spike.progress = std::clamp(std::min(spike.curve[1].x, 2.5f - spike.curve[1].x), 0.f, 1.f);
    }
    std::erase_if(spell.surface.vines_data, [](const auto& spike) { return spike.curve[1].x >= 2.5f; });
    for (int tick = 0; tick < ticks && spell.stage < 240; ++tick) {
        if (spell.stage == 16) {
            const auto from = spell.motion.position;
            Vec3 to{from.x + spell.motion.velocity.x, from.y + spell.motion.velocity.y, from.z};
            if (match.terrain)
                to.z = static_cast<float>(terrain_surface(*match.terrain, to.x, to.y));
            Vec3 endpoint{to.x, to.y, to.z + .5f};
            auto* hit = spell_trace_ray(match, spell, {from.x, from.y, from.z + .5f}, endpoint, 21);
            if (hit && hit != spell.effect_target && hit->friendly(&spell))
                hit = nullptr;
            spell.motion.position = to;
            Vec3 root{to.x + (match.visual_random.next() / 8192.f - 2),
                      to.y + (match.visual_random.next() / 8192.f - 2), to.z};
            rhinoc_spike(match, spell, root);
            if (hit || --spell.remaining <= 0) {
                if (hit) {
                    spell.effect_target = hit;
                    spell.point = hit->source_motion()->position;
                }
                spell.motion.position = spell.point;
                spell.motion.velocity = {};
                spell.sound_event(match, 4);
                spell_damage(match, &spell, spell.effect_target, spell.data->damage_type, spell.data->amount,
                             spell.data->radius, spell.point);
                spell.next_pulse = 0;
                spell.stage = 33;
            }
        } else if (--spell.next_pulse <= 0) {
            spell.host.rate += 5;
            const float radius = spell.host.rate;
            const int count = static_cast<int>(std::clamp(radius * 1.256636f, 16.f, 32.f));
            for (int n = 0; n < count; ++n) {
                const float angle = n * 6.28318f / count,
                            r = radius + (match.visual_random.next() / 32768.f * 6 - 3);
                rhinoc_spike(match, spell,
                             {spell.motion.position.x + std::sin(angle) * r,
                              spell.motion.position.y + std::cos(angle) * r, 0});
            }
            for (unsigned group : {0u, 1u, 2u})
                match.visit_group(group, 21, [&](MatchObject& object) {
                    const auto* body = object.source_motion();
                    if (!body || (&object != spell.effect_target && object.friendly(&spell)))
                        return 0;
                    const float distance = std::hypot(body->position.x - spell.motion.position.x,
                                                      body->position.y - spell.motion.position.y);
                    if (std::abs(distance - radius) <= 3 && match.terrain &&
                        std::abs(body->position.z -
                                 terrain_surface(*match.terrain, body->position.x, body->position.y)) <= 2) {
                        rhinoc_spike(match, spell, body->position);
                        spell_damage(match, &spell, &object, spell.data->damage_type, spell.data->amount,
                                     spell.data->radius, body->position);
                    }
                    return 0;
                });
            spell.next_pulse = 10;
            if (radius > spell.data->spread_limit)
                spell.stage = 240;
        }
    }
    return spell.stage == 240 && spell.surface.vines_data.empty() ? remove_spell(spell) : 1;
}
int run_attached_fire(Match& match, SpellActor& spell, int ticks) {
    // 0x41EFC0 and 0x41E960 are persistent attachments, not timed area attacks.
    auto* caster = dynamic_cast<CreatureActor*>(spell.owner);
    auto& p = spell.particles(match);
    if (!spell.stage) {
        spell.particle_material(match, p, 0x1a012008, "firy");
        const float flame[]{.3f, 1, .5f};
        p.set_property(128, flame, flame);
        spell.stage = 1;
        if (caster && spell.effect != SpellActor::Effect::fire_stomper_head) {
            constexpr std::uintptr_t fire_damage = 0x41efa0;
            match.functions.try_emplace(fire_damage, [](MatchObject& object, int) {
                auto& fire = static_cast<SpellActor&>(object);
                if (fire.stack.size() >= 2)
                    fire.stack[1] = static_cast<unsigned>(fire.stack[1] * fire.cast_alpha);
                return 1;
            });
            match.watch(*caster, 1, {&spell, spell.reference(), fire_damage});
        }
    }
    const bool active = caster && caster->alive();
    if (active) {
        spell.motion.position = caster->motion.position;
        const Vec3 head_offset{0, .2f, -.8f};
        if (spell.effect == SpellActor::Effect::fire_stomper_head)
            caster->write_bone_point(spell.motion.position, 18, nullptr, &head_offset);
        else
            for (int tick = 0; tick < ticks; ++tick) {
                caster->write_bone_point(spell.motion.position, (spell.rate++ & 1) ? 6 : 10);
                p.set_position(spell.motion.position, &spell.motion.position);
                p.emit(spell.age + tick, 1);
            }
        if (spell.effect != SpellActor::Effect::fire_stomper_head) {
            const int base = caster->stats.base[CreatureStats::health],
                      health = caster->stats.current[CreatureStats::health];
            caster->stats.changed |= 1;
            caster->stats.target[0] =
                static_cast<std::int16_t>(std::clamp(caster->stats.target[0] + base - health, 0, 32767));
            spell.cast_alpha = base ? 1 + (base - health) * 1.5f / base : 1;
        }
    } else if (caster)
        match.unwatch(*caster, 1, spell.reference(), &spell);
    p.set_position(spell.motion.position);
    spell.age += ticks;
    p.advance(spell.age, active && spell.effect == SpellActor::Effect::fire_stomper_head ? 1.f : 0.f);
    if (!active && !p.alive())
        return remove_spell(spell);
    return 1;
}
int run_quadrok(Match&, SpellActor& spell) {
    // QuadrokAbility 0x41f0f0 → 0x41EF30 on the caster.
    auto* caster = dynamic_cast<CreatureActor*>(spell.owner);
    if (caster && caster->source_motion())
        spell.motion.position = caster->source_motion()->position;
    else
        aim_point(spell);
    if (!caster || !caster->alive())
        return remove_spell(spell);
    // 0x41EF30: strength and resistance scale with missing health; this ability does not strike a target.
    const int base = caster->stats.base[CreatureStats::health],
              missing = base - caster->stats.current[CreatureStats::health];
    caster->stats.changed |= 2;
    caster->stats.target[1] =
        static_cast<std::int16_t>(std::clamp(caster->stats.target[1] + missing, 0, 32767));
    const float factor = base ? 1 - missing * .75f / base : 1;
    for (unsigned i = 9; i <= 13; ++i) {
        caster->stats.changed |= 1u << i;
        caster->stats.target[i] = static_cast<std::int16_t>(std::max(0.f, caster->stats.target[i] * factor));
    }
    return 1;
}
int run_earth_stomper(Match& match, SpellActor& spell, int ticks) {
    if (!spell.stage) {
        if (spell.owner && spell.owner->source_motion())
            spell.motion.position = spell.owner->source_motion()->position;
        spell.sound_event(match, 2);
        // EarthStomperRun 0x41ed50: 0x439830(.75, 'dirt', box (7.5, 7.5, 5), 8 s, .3) beside the LAND ring.
        SpellActor::launch_dust_splatter(match, spell, 0x0a011008, FourCC::from_text("dirt"), spell.motion.position,
                                         {7.5f, 7.5f, 5}, 8, .3f);
        spell.age = 0;
        spell.next_pulse = 0;
        spell.stage = 1;
        if (match.terrain) {
            const auto& land = *match.terrain;
            const float reach = spell.data->spread_limit + 20;
            for (int y = std::max(0, static_cast<int>((spell.motion.position.y - reach) / 10));
                 y <= std::min(static_cast<int>(land.height) - 1,
                               static_cast<int>((spell.motion.position.y + reach) / 10));
                 ++y)
                for (int x = std::max(0, static_cast<int>((spell.motion.position.x - reach) / 10));
                     x <= std::min(static_cast<int>(land.width) - 1,
                                   static_cast<int>((spell.motion.position.x + reach) / 10));
                     ++x)
                    spell.terrain_cells.push_back({static_cast<float>(x), static_cast<float>(y), 0});
        }
    }
    // 0x440ED0/0x441010: expanding LAND ring and 0x44EAF0 knockdowns, without a direct damage pulse.
    spell.age = std::min(1.f, spell.age + ticks * 2.5f / std::max(spell.data->spread_limit, .01f));
    if ((spell.next_pulse -= ticks) <= 0) {
        const float radius = std::sqrt(spell.age) * spell.data->spread_limit,
                    width = std::sqrt(std::sqrt(spell.age)) * 20;
        if (match.terrain) {
            auto& land = *const_cast<Terrain*>(match.terrain);
            const float amplitude = std::min({spell.age * 5, (1 - spell.age) * 4, 1.f}) * 25;
            for (auto& cell : spell.terrain_cells) {
                auto& raw =
                    land.heights[static_cast<unsigned>(cell.y) * land.width + static_cast<unsigned>(cell.x)];
                const int base = (raw & 0x7fff) - static_cast<int>(cell.z);
                const float distance = std::abs(
                    std::hypot(cell.x * 10 - spell.motion.position.x, cell.y * 10 - spell.motion.position.y) -
                    radius);
                const int offset =
                    width > 0 && distance < width
                        ? static_cast<int>(amplitude * (1 + std::cos(distance * 3.14159f / width)) * .5f)
                        : 0;
                const int value = std::clamp(base + offset, 0, 32767);
                cell.z = static_cast<float>(value - base);
                raw = static_cast<std::uint16_t>((raw & 0x8000) | value);
            }
            ++match.terrain_revision;
        }
        for (unsigned group = 0; group < 2; ++group)
            match.visit_group(group, 5, [&](MatchObject& object) {
                auto* actor = living_unit(&object);
                if (!actor || !(actor->state_flags & 2))
                    return 0;
                const float x = actor->motion.position.x - spell.motion.position.x,
                            y = actor->motion.position.y - spell.motion.position.y;
                if (std::abs(radius - std::sqrt(x * x + y * y)) < width && match.random().next() < 8192)
                    actor->knock_down(0, 10);
                return 0;
            });
        spell.next_pulse = 5;
    }
    return spell.age >= 1 ? remove_spell(spell) : 1;
}
int run_hellmouth(Match& match, SpellActor& spell, int ticks) {
    auto& trail = spell.altar_streamer;
    const auto random_point = [&]() {
        const auto* body = spell.effect_target->source_motion();
        const auto coordinate = [&](float low, float high) {
            return (low + (high - low) * (match.random().next() / 32768.f)) * 1.5f;
        };
        return Vec3{coordinate(body->box_min.x, body->box_max.x),
                    coordinate(body->box_min.y, body->box_max.y),
                    coordinate(body->box_min.z, body->box_max.z)};
    };
    if (!spell.stage) {
        spell.start_creature_projectile(match, static_cast<int>(spell.data->range / spell.data->speed) + 30,
                                        {}, 1, false);
        spell.motion.collision_mask = 0x114015;
        spell.friendly_ignore_time = 0;
        trail.enabled = true;
        trail.texture = FourCC{static_cast<unsigned>(
            spell.data->integer(FourCC::from_text("ttex"), FourCC::from_text("rist").value))};
        const int blend = spell.data->integer(FourCC::from_text("iadd"), -1);
        trail.material = blend == 0 ? 0x0a011008 : blend == 1 ? 0x1a012008 : 0x1a010401;
        trail.primitive.configure(spell.data->integer(FourCC::from_text("ilen"), 45),
                                  spell.data->real(FourCC::from_text("frad"), .5f));
        spell.light = {true, spell.motion.position, {1, 1, 1}, 0, 1, 30};
    }
    if (spell.stage == 17 && (spell.state_flags & 3)) {
        spell.capture_collision(match);
        spell.effect_target = spell.impact_target;
        spell.sound_event(match, 4);
        if (spell.effect_target && spell.effect_target->source_motion()) {
            const auto at = spell.effect_target->source_motion()->position;
            trail.controls[1] = {spell.motion.position.x - at.x, spell.motion.position.y - at.y,
                                 spell.motion.position.z - at.z};
            const auto b = trail.controls[1], v = spell.motion.velocity;
            trail.controls[0] = {b.x - v.x * 8, b.y - v.y * 8, b.z - v.z * 8};
            trail.controls[2] = random_point();
            trail.controls[3] = random_point();
        } else
            spell.effect_target = nullptr;
        if (auto* victim = living_unit(spell.effect_target))
            SpellActor::poison(match, *victim, spell.owner, *spell.data);
        spell.motion.flags = spell.motion.collision_mask = 0;
        spell.remaining = 180;
        spell.stage = spell.effect_target ? 33 : 240;
    }
    if (spell.stage == 17 || spell.stage == 33) {
        if (spell.stage == 33) {
            if (!spell.effect_target || (spell.effect_target->state_flags & MatchObject::removed_flag))
                spell.stage = 240;
            else {
                trail.phase += ticks / 30.f;
                while (trail.phase >= 1) {
                    trail.phase -= 1;
                    for (int i = 0; i < 3; ++i)
                        trail.controls[i] = trail.controls[i + 1];
                    trail.controls[3] = random_point();
                }
                const auto a = trail.controls[0], b = trail.controls[1], c = trail.controls[2],
                           d = trail.controls[3], at = spell.effect_target->source_motion()->position;
                const float t = trail.phase;
                const auto spline = [t](float a, float b, float c, float d) {
                    return .5f * (2 * b + (-a + c) * t + (2 * a - 5 * b + 4 * c - d) * t * t +
                                  (-a + 3 * b - 3 * c + d) * t * t * t);
                };
                spell.motion.position = {at.x + spline(a.x, b.x, c.x, d.x), at.y + spline(a.y, b.y, c.y, d.y),
                                         at.z + spline(a.z, b.z, c.z, d.z)};
            }
        }
        if ((spell.remaining -= ticks) <= 0)
            spell.stage = 240;
    }
    if (ticks > 0 && spell.stage < 240 && (match.random().next() & 63) <= ticks)
        spell.sound_event(match, 8);
    spell.light.position = spell.motion.position;
    const float goal = spell.stage < 240 ? 1.f : 0.f;
    spell.light.intensity += std::clamp(goal - spell.light.intensity, -ticks / 60.f, ticks / 60.f);
    spell.altar_streamer.ticks += ticks;
    const auto frames = match.texture_frames ? match.texture_frames(spell.altar_streamer.texture) : 1u;
    return spell.altar_streamer.primitive.add_point(spell.stage == 240 ? nullptr : &spell.motion.position,
                                                    spell.altar_streamer.ticks, frames)
               ? 1
               : remove_spell(spell);
}
int run_basilisk(Match& match, SpellActor& spell, int ticks) {
    if (!spell.stage) {
        spell.start_creature_projectile(match, static_cast<int>(spell.data->range / spell.data->speed), {}, 1,
                                        false);
        auto& p = spell.particles(match);
        spell.motion.collision_mask = 0x114015;
        spell.friendly_ignore_time = 0;
        spell.particle_material(match, p, 0x1a012008,
                                FourCC{static_cast<unsigned>(spell.data->integer(
                                    FourCC::from_text("ttex"), FourCC::from_text("tube").value))});
        const float size = .3f, life = 2;
        p.set_property(134, &size, &life);
        spell.emission = 1;
        spell.ability.origin = spell.motion.position;
        if (auto* caster = dynamic_cast<CreatureActor*>(spell.owner)) {
            Vec3 other = spell.motion.position;
            caster->write_reference_point(other, 1, true);
            spell.ability.destination = {other.x - spell.motion.position.x, other.y - spell.motion.position.y,
                                         other.z - spell.motion.position.z};
        }
    }
    // Restore the primary's previous centre after the secondary emitter used this shared host.
    spell.particle_systems.front().set_position(spell.ability.origin);
    const int flying = spell.fly_creature_projectile(match, ticks);
    if (flying < 0)
        return flying;
    if (!flying) {
        if (auto* victim = living_unit(spell.impact_target))
            if (const auto* definition =
                    match.spell_registry ? match.spell_registry->find(FourCC{1081308270u}) : nullptr;
                definition && victim->allegiance)
                SpellActor::launch_side_cast(match, *victim->allegiance, *definition, victim,
                                             victim->motion.position);
        spell.emission = 0;
        spell.stage = 240;
    }
    if (ticks > 0) {
        // 0x41ce20 advances the same particle clock twice, once for each launch reference.
        auto& p = spell.particle_systems.front();
        Vec3 other{spell.motion.position.x + spell.ability.destination.x,
                   spell.motion.position.y + spell.ability.destination.y,
                   spell.motion.position.z + spell.ability.destination.z};
        Vec3 previous{spell.ability.origin.x + spell.ability.destination.x,
                      spell.ability.origin.y + spell.ability.destination.y,
                      spell.ability.origin.z + spell.ability.destination.z};
        p.set_position(other, &previous);
        spell.age += ticks;
        p.advance(spell.age, spell.stage >= 240 ? 0 : spell.emission);
        spell.ability.origin = spell.motion.position;
    }
    return flying ? flying : 1;
}
int run_boulder(Match& match, SpellActor& spell, int ticks) {
    // BoulderSquatRun 0x4169a0 assigns BoulderSquatSpawn 0x41acf0: 0x439270 then 0x44D7E0.
    if (!spell.stage) {
        spell.start_creature_projectile(match, static_cast<int>(spell.data->range / spell.data->speed) + 30,
                                        FourCC::from_text("roks"), 1, false);
        const auto spin = [&] {
            return static_cast<float>((match.random().next() - 0x4000) * .00000152587890625);
        };
        spell.motion.angular_velocity = {spin(), spin(), spin()};
        spell.motion.flags |= 0x38;
        spell.motion.collision_mask |= 0x80000;
        spell.ability.clock = static_cast<int>(
            std::sqrt(distance_squared(spell.motion.position, spell.point)) * .5 / spell.data->speed);
        SpellActor::launch_model_trail(match, spell, 0x0a011008, boulder_trail(spell), 1, .5f); // 0x416640.
    }
    if (spell.stage == 17 && spell.ability.clock >= 0 && (spell.ability.clock -= ticks) <= 0) {
        // 0x4168B0: three local destination vectors (-20,300,5), (20,300,5), (0,300,0).
        const auto o = spell_orientation(spell.motion.velocity);
        for (const float x : {-20.f, 20.f}) {
            auto& child =
                launch_child(match, spell, SpellActor::Effect::boulder_squat, nullptr, spell.motion.position);
            child.point = {spell.motion.position.x + o[0] * x + o[3] * 300 + o[6] * 5,
                           spell.motion.position.y + o[1] * x + o[4] * 300 + o[7] * 5,
                           spell.motion.position.z + o[2] * x + o[5] * 300 + o[8] * 5};
            child.start_creature_projectile(
                match, 30 + static_cast<int>(spell.data->range * .5f / spell.data->speed),
                FourCC::from_text("roks"), 1, false, false);
            child.ability.clock = -1;
            SpellActor::launch_model_trail(match, child, 0x0a011008, boulder_trail(spell), 1, .5f); // 0x416960.
        }
        spell.point = {spell.motion.position.x + o[3] * 300, spell.motion.position.y + o[4] * 300,
                       spell.motion.position.z + o[5] * 300};
        spell.ability.clock = -1;
    }
    if (const int flying = spell.fly_creature_projectile(match, ticks))
        return flying;
    // KlapponRun 0x41acf0 stage 32 (main and split boulders): 0x439270 shatter, then 0x44D7E0; no hit burst.
    SpellActor::launch_rock_shatter(match, spell);
    const int type = hit_type(spell, 51), amount = hit_amount(spell, 500);
    if (spell.data->radius > 0.f)
        spell_damage(match, &spell, spell.impact_target, type, amount, spell.data->radius, spell.motion.position);
    else
        spell_damage(match, &spell, spell.impact_target, type, amount, spell.data->radius);
    return remove_spell(spell);
}
int run_lightning_charge(Match& match, SpellActor& spell, int ticks) {
    auto* host = dynamic_cast<CreatureActor*>(spell.owner);
    constexpr std::uintptr_t absorb = 0x41f220, amplify = 0x41f2b0;
    if (!spell.stage) {
        if (!host)
            return remove_spell(spell);
        match.functions.try_emplace(absorb, [&match](MatchObject& object, int) {
            auto& charge = static_cast<SpellActor&>(object);
            if (charge.stack.size() >= 3) {
                const int type = std::bit_cast<int>(charge.stack[0]),
                          amount = std::bit_cast<int>(charge.stack[1]);
                auto* source = dynamic_cast<SpellActor*>(match.resolve(charge.stack[2]));
                if ((type & 7) && (type & 0x70) == 0x40 && !(type & 8) && source && source->beam.enabled) {
                    charge.remaining += amount * (charge.friendly(source) ? 4 : 2);
                    charge.stack[1] = static_cast<unsigned>(amount >> 2);
                }
            }
            return 1;
        });
        match.functions.try_emplace(amplify, [](MatchObject& object, int) {
            auto& charge = static_cast<SpellActor&>(object);
            if (charge.stack.size() >= 2 && !(charge.stack[0] & 7) && charge.remaining > 0)
                charge.stack[1] = static_cast<unsigned>(charge.stack[1] * 1.5);
            return 1;
        });
        match.watch(*host, 0, {&spell, spell.reference(), absorb});
        match.watch(*host, 1, {&spell, spell.reference(), amplify});
        spell.stage = 1;
        spell.remaining = 0;
        spell.cast_alpha = 0;
        auto& sparks = spell.particles(match);
        spell.particle_material(match, sparks, 0x1a012008,
                                FourCC{static_cast<unsigned>(spell.data->integer(
                                    FourCC::from_text("tSPK"), FourCC::from_text("sprk").value))});
    }
    // 0x41f410: without an owner the charge drains its sparks at stage 240 before removal.
    if (spell.stage != 240 && (!host || !host->alive())) {
        if (host) {
            match.unwatch(*host, 0, spell.reference(), &spell);
            match.unwatch(*host, 1, spell.reference(), &spell);
        }
        spell.stage = 240;
    }
    if (spell.stage == 240) {
        if (!spell.particle_systems.front().alive())
            return remove_spell(spell);
        spell.host.time += ticks;
        spell.particle_systems.front().advance(spell.host.time, 0);
        return 1;
    }
    spell_target_point(spell.motion.position, host);
    if (spell.remaining > 0) {
        spell.remaining = std::max(0, spell.remaining - ticks);
        spell.cast_alpha = std::min(1.f, spell.cast_alpha + ticks / 60.f);
        for (const unsigned index : {0u, 3u, 4u}) {
            host->stats.changed |= 1u << index;
            host->stats.target[index] =
                static_cast<std::int16_t>(std::clamp(host->stats.target[index] * 1.5f, 0.f, 32767.f));
        }
        if (spell.remaining >= spell.data->amount &&
            ticks * 546.13333f * spell.cast_alpha > match.random().next()) {
            // 0x41f350 -> 0x44d430: any nearby living unit (either side) or one of four ground slots
            // at least 5 units out; picking the owner itself skips the shot and keeps the build-up.
            std::vector<CreatureActor*> candidates;
            const float range = spell.data->range, range_squared = range * range;
            for (unsigned group : {0u, 1u})
                match.visit_group(group, 5, [&](MatchObject& object) {
                    if (auto* actor = living_unit(&object);
                        actor && candidates.size() < 256 &&
                        spell_box_distance_squared(spell.motion.position, actor->motion, range_squared) <=
                            range_squared)
                        candidates.push_back(actor);
                    return 0;
                });
            CreatureActor* victim = nullptr;
            Vec3 at = spell.motion.position;
            const auto selected = match.random().next() % (candidates.size() + 4);
            if (selected < candidates.size()) {
                victim = candidates[selected];
                spell_target_point(at, victim);
            } else {
                const float radius = match.random().next() * (range - 5) / 32768.f + 5;
                const float angle = match.random().next() * 6.2831802f / 32768.f;
                at.x += std::sin(angle) * radius;
                at.y += std::cos(angle) * radius;
                if (match.terrain)
                    at.z = static_cast<float>(terrain_true_height(*match.terrain, at.x, at.y));
            }
            if (victim != host) {
                // 0x44e850 child running LightningSpellRun 0x422cb0 as damage type 38 (data +72 = 73).
                auto& bolt =
                    launch_child(match, spell, SpellActor::Effect::lightning_bolt, victim, spell.motion.position);
                auto data = std::make_shared<SpellData>(*spell.data);
                data->damage_type = 73;
                bolt.data = data;
                bolt.point = at;
                bolt.run(match, 0);
                spell.remaining -= spell.data->amount / 2;
                spell.cast_alpha = 0;
            }
        }
        // 0x41f3e0 -> 0x438f10: short, irregular bursts inside the moving body's bounds (sparks in its box, not
        // arcs along bones).
        if (ticks > 0) {
            const int count = static_cast<int>(16.f * match.random().next() / (ticks * 546.13333f));
            if (count < 32) {
                auto& sparks = spell.particle_systems.front();
                const auto& m = host->motion;
                const float life[]{1.5f, spell.data->real(FourCC::from_text("fSPa"), 0)},
                    bounds[]{(m.box_min.x - m.box_max.x) * .5f, (m.box_min.y - m.box_max.y) * .5f,
                             (m.box_min.z - m.box_max.z) * .5f, (m.box_max.x - m.box_min.x) * .5f,
                             (m.box_max.y - m.box_min.y) * .5f, (m.box_max.z - m.box_min.z) * .5f};
                sparks.set_property(140, life, bounds);
                // 0x44e350: the centre of the host's box (+596..+616) about its position.
                const Vec3 centre{m.position.x + (m.box_min.x + m.box_max.x) * .5f,
                                  m.position.y + (m.box_min.y + m.box_max.y) * .5f,
                                  m.position.z + (m.box_min.z + m.box_max.z) * .5f};
                sparks.set_position(centre);
                for (int n = 0; n <= count; ++n)
                    sparks.emit(spell.host.time, 1);
                spell.sound_event(match, 8);
            }
        }
    }
    spell.host.time += ticks;
    spell.particle_systems.front().advance(spell.host.time, 0);
    return 1;
}
int run_call_lightning(Match& match, SpellActor& spell) {
    // CallLightningRun 0x41f1e0: 0x4535B0(self +456, 'litn', self, self +440, 1). Like an unpromoted
    // 0x41f410, it runs on the creature itself (no 0x441bf0 spell child), so Lightning targets the
    // caster; 0x450390 accepts it under the 0x2015 unit mask and the bolt feeds Lightning Charge.
    auto* caster = dynamic_cast<CreatureActor*>(spell.owner);
    if (caster && caster->allegiance)
        if (const auto* definition =
                match.spell_registry ? match.spell_registry->find(FourCC::from_text("litn")) : nullptr)
            SpellActor::launch_side_cast(match, *caster->allegiance, *definition, caster,
                                         caster->motion.position);
    return remove_spell(spell);
}
} // namespace
int SpellActor::run_melee(Match& match, int ticks) {
    if (!data)
        throw Error("Creature melee ability requires its SPEL definition");
    switch (effect) {
    case Effect::silverback:
        return run_breath(match, *this, ticks);
    case Effect::rhinoc:
        return run_rhinoc(match, *this, ticks);
    case Effect::quadrok:
        return run_quadrok(match, *this);
    case Effect::fire_troll:
        return run_attached_fire(match, *this, ticks);
    case Effect::red_dragon:
        if (auto* caster = dynamic_cast<CreatureActor*>(owner); caster && caster->alive()) {
            visit_foes(match, caster, [&](CreatureActor& actor) {
                if (!caster->motion.collision_boxes.empty() && !actor.motion.collision_boxes.empty() &&
                    spell_box_separation(caster->motion, actor.motion) <= 2) {
                    actor.accumulate_damage({34, 200, reference()}, ticks);
                    actor.burn_near(caster->motion.position, 2, 60);
                }
            });
        }
        return run_attached_fire(match, *this, ticks);
    case Effect::fire_stomper_head:
        return run_attached_fire(match, *this, ticks);
    case Effect::earth_stomper:
        return run_earth_stomper(match, *this, ticks);
    case Effect::basilisk:
        return run_basilisk(match, *this, ticks);
    case Effect::hellmouth:
        return run_hellmouth(match, *this, ticks);
    case Effect::boulder_squat:
        return run_boulder(match, *this, ticks);
    case Effect::lightning_charge:
        return run_lightning_charge(match, *this, ticks);
    case Effect::call_lightning:
        return run_call_lightning(match, *this);
    case Effect::silverback_icespray:
        return run_icespray(match, *this, ticks);
    default:
        throw Error("Invalid creature melee ability");
    }
}
} // namespace sacrifice
