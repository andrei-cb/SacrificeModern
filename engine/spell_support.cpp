#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/fog.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/soul_actor.hpp"
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
            int fallback_amount, int fallback_type) {
    const int type = hit_type(spell, fallback_type);
    const int amount = hit_amount(spell, fallback_amount);
    if (splash)
        spell_damage(match, &spell, victim, type, amount, spell.data->radius, point);
    else
        spell_damage(match, &spell, victim, type, amount, spell.data->radius);
    attach_hit(match, spell, victim, point);
}
int support_flight(Match& match, SpellActor& spell, int ticks, bool fire) {
    if (!spell.stage) {
        spell.start_creature_projectile(match, static_cast<int>(spell.data->range / spell.data->speed), {}, 1,
                                        false);
        spell.motion.collision_mask = 0x114015; // ArachonShoot 0x41c760.
        spell.friendly_ignore_time = 0;
        auto& p = spell.particles(match);
        spell.particle_material(match, p, 0x1a012008,
                                fire ? FourCC::from_text("fbal")
                                     : FourCC{static_cast<unsigned>(spell.data->integer(
                                           FourCC::from_text("ttex"), FourCC::from_text("glo2").value))});
        const float size = fire ? .4f : 1.f, life = fire ? 1.f : .75f;
        p.set_property(129, &size, &life);
        if (fire) {
            // 0x41c880 / 0x4cb0dc: fire particles drift rather than forming the life trail.
            const float low[3]{-.01f, -.01f, -.01f}, high[3]{.01f, .01f, .01f};
            p.set_property(3, low, high);
        }
        p.set_position(spell.motion.position);
        spell.emission = fire ? 1.f : 2.f;
    }
    return spell.fly_creature_projectile(match, ticks);
}
void drain_support(SpellActor& spell) {
    spell.emission = 0;
    spell.motion.flags = spell.motion.collision_mask = 0;
    spell.motion.velocity = {};
    spell.stage = 240;
}
int run_arachon_life(Match& match, SpellActor& spell, int ticks) {
    // Stage 16: 0x438FF0 then 0x492300(5, pos, spread, 0x440A30).
    if (const int flying = support_flight(match, spell, ticks, false))
        return flying;
    attach_hit(match, spell, spell.impact_target, spell.motion.position);
    const float reach = spell.data->spread_limit;
    const auto heal_one = [&](MatchObject& object) {
        auto* actor = living_unit(&object);
        if (!actor)
            return 0;
        if (!actor->source_motion())
            return 0;
        if (spell_box_distance_squared(spell.motion.position, actor->motion, reach * reach) >
            static_cast<double>(reach) * reach)
            return 0;
        // 0x440a30 requests the base health amount, without a side filter.
        actor->heal(match, actor->stats.base[CreatureStats::health], 256, 0);
        return 0;
    };
    match.visit_group(0, 5, heal_one);
    match.visit_group(1, 5, heal_one);
    drain_support(spell);
    return 1;
}
int run_arachon_fire(Match& match, SpellActor& spell, int ticks) {
    if (const int flying = support_flight(match, spell, ticks, true))
        return flying;
    strike(match, spell, spell.impact_target, spell.motion.position, spell.data->radius > 0.f, 600, 20);
    drain_support(spell);
    return 1;
}
int run_cage_pull(Match& match, SpellActor& spell, int ticks) {
    // CagePullRun 0x41d990: extend, enter the attack animation, then pull on its release events.
    auto* victim = living_unit(spell.effect_target);
    auto* caster = dynamic_cast<CreatureActor*>(spell.owner);
    if (!spell.stage) {
        if (!victim || !caster)
            return remove_spell(spell);
        caster->orders.pop(true); // 0x46d8b0(1): consume this ability order, retaining queued orders.
        // 0x4355E0(spell, range, ttex 'ltng', fjit, 0.25 s, 0.5, 1 strand, width 0.5, 'web!' ? 10 : 3):
        // a web is normal-blended and untraced; other pulls are additive, retrace and carry the white light.
        const bool web = spell.data->integer(FourCC::from_text("ttyp"), FourCC::from_text("litn").value) ==
                         static_cast<int>(FourCC::from_text("web!").value);
        spell.beam.enabled = true;
        spell.beam.texture = FourCC{static_cast<unsigned>(
            spell.data->integer(FourCC::from_text("ttex"), FourCC::from_text("ltng").value))};
        spell.beam.material = web ? 0x0a011008 : 0x1a012008;
        spell.beam.deviation = spell.host.fade = spell.data->real(FourCC::from_text("fjit"), 0);
        if (!web) {
            caster->write_reference_point(spell.motion.position, 0, true);
            spell_target_point(spell.point, victim);
            // Flag 2 (0x44CD10) replaces +476 with whatever the ray meets first.
            if (auto* hit = spell_trace_ray(match, spell, spell.motion.position, spell.point, 0x4015);
                hit && hit != victim) {
                spell.effect_target = hit;
                victim = living_unit(hit);
            }
            spell.light = {true, spell.motion.position, {1, 1, 1}, 1, 1, 100};
        }
        spell.remaining = 3;
        spell.stage = 1;
        spell.ability.length = spell.ability.draw_back = 0;
        spell.sound_event(match, 2);
    }
    if (!victim || !caster || !caster->alive() || (caster->state_flags & 0x10))
        spell.stage = 240;
    if (caster && spell.stage < 240) {
        caster->write_reference_point(spell.motion.position, 0, true);
        spell_target_point(spell.point, victim);
    }
    switch (spell.stage) {
    case 1:
        spell.ability.draw_back = std::min(1.f, spell.ability.draw_back + ticks / 15.f);
        if (spell.ability.draw_back >= 1)
            spell.stage = caster->fsm().event(51) == 2 ? 16 : 240;
        break;
    case 16: {
        spell.sound_event(match, 4);
        auto& hold =
            launch_child(match, spell, SpellActor::Effect::web_pull_hold, &spell, spell.motion.position);
        hold.run(match, 0);
        spell.stage = 35;
        break;
    }
    case 32:
        spell.beam.deviation = 0; // Context +32 is the primitive's deviation (+56), not its start.
        caster->state_flags &= ~0x20000000u;
        spell.stage = (victim->motion.flags & 0x10000) || victim->named(FourCC::from_text("PROT")) ? 240 : 33;
        break;
    case 33:
        spell.sound_event(match, 8);
        victim->knock_down(1, 10.f);
        spell.stage = 34;
        spell.next_pulse = 15;
        [[fallthrough]];
    case 34:
        victim->motion.push_direction({spell.motion.position.x - victim->motion.position.x,
                                       spell.motion.position.y - victim->motion.position.y,
                                       spell.motion.position.z - victim->motion.position.z},
                                      .33636057f, 7);
        if ((spell.next_pulse -= ticks) <= 0) {
            spell.stage = spell.remaining-- <= 0 ? 240 : 35;
        }
        break;
    case 35: {
        const float goal = spell.host.fade * .25f, step = spell.host.fade * ticks * .0125f;
        spell.beam.deviation += std::clamp(goal - spell.beam.deviation, -step, step);
        if (caster->state_flags & 0x20000000)
            spell.stage = 32;
        break;
    }
    default:
        break;
    }
    if (spell.stage >= 240) {
        if (caster) {
            caster->fsm().event(52);
            spell.owner = nullptr;
        }
        spell.ability.length = std::min(spell.ability.draw_back, spell.ability.length + ticks / 15.f);
        if (spell.light.enabled) // 0x435840 dims the light by 0.1 per call while retracting.
            spell.light.intensity = std::max(0.f, spell.light.intensity - .1f);
        if (spell.ability.length >= spell.ability.draw_back)
            return remove_spell(spell);
    }
    const auto along = [&](float t) {
        return Vec3{spell.motion.position.x + (spell.point.x - spell.motion.position.x) * t,
                    spell.motion.position.y + (spell.point.y - spell.motion.position.y) * t,
                    spell.motion.position.z + (spell.point.z - spell.motion.position.z) * t};
    };
    // 0x435840: frames advance 0.5 per tick; the curve keeps its endpoints and draws [start, end].
    spell.beam.frame += ticks * .5f;
    if (const auto frames = match.texture_frames ? match.texture_frames(spell.beam.texture) : 1u;
        spell.beam.frame >= std::max(frames, 1u))
        spell.beam.frame = 0;
    if (spell.light.enabled)
        spell.light.position = along(spell.ability.draw_back);
    spell.beam.primitive.set_params({spell.motion.position, spell.point, .2f, spell.beam.deviation,
                                     spell.ability.length, spell.ability.draw_back, .5f, 16, 1},
                                    match.visual_random);
    return 1;
}
int run_bugzapper(Match& match, SpellActor& spell, int ticks) {
    // BugzapperRun 0x419e10: drain victim +2860 mana, then 0x44D7E0.
    auto* victim = living_unit(spell.effect_target);
    auto* caster = living_unit(spell.owner);
    if (!spell.stage) {
        if (!victim || !caster)
            return remove_spell(spell);
        // 0x419d60 traces one shot; 0x441C30(16, 0x1a) only sets network-sync bits, so the drain runs
        // on the next call.
        // The helper leaves the point unchanged without a SAXS body; start from the caster then.
        spell.ability.destination = caster->motion.position;
        caster->write_reference_point(spell.ability.destination, 0, true);
        spell_target_point(spell.point, victim);
        Vec3 direction{spell.point.x - spell.ability.destination.x,
                       spell.point.y - spell.ability.destination.y,
                       spell.point.z - spell.ability.destination.z};
        spell_scatter(direction, caster->stats.current[8], match.random());
        const float length = static_cast<float>(std::sqrt(distance_squared(direction, {})));
        if (length > 0) {
            const float scale = spell.data->range / length;
            spell.point = {spell.ability.destination.x + direction.x * scale,
                           spell.ability.destination.y + direction.y * scale,
                           spell.ability.destination.z + direction.z * scale};
        }
        spell.effect_target = spell_trace_ray(match, spell, spell.ability.destination, spell.point, 0x4015);
        spell.motion.position = spell.point;
        spell.motion.flags = 0;
        spell.sound_event(match, 2);
        spell.stage = 16;
        return 1;
    }
    if (spell.stage == 16) {
        // NATIVE QUIRK (BugzapperRun 0x419f6f): the original reads the victim's mana cap (+2852, 0xB24)
        // and divides it by 655360 (`imul 0x66666667; sar 18`), as if the cap were 16.16 fixed point.
        // For every stock unit that rounds to 0, so Drain Essence deals its damage but steals no mana.
        // To restore the evident intent (steal a tenth of current mana), set this to false.
        constexpr bool native_mana_steal = true;
        const int available = native_mana_steal ? victim ? victim->maximum_mana / 655360 : 0
                                                : victim ? victim->mana / 10 : 0;
        const int stolen = victim ? std::clamp(std::min(available, spell.data->amount / 2), 0, 100) : 0;
        if (victim)
            victim->change_mana_fixed(-stolen * 65536);
        spell.rate = stolen;
        if (spell.effect_target)
            spell_damage(match, &spell, spell.effect_target, hit_type(spell, 83), hit_amount(spell, 200),
                         spell.data->radius);
        const Vec3 delta{spell.ability.destination.x - spell.motion.position.x,
                         spell.ability.destination.y - spell.motion.position.y,
                         spell.ability.destination.z - spell.motion.position.z};
        const float distance = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
        const float speed = spell.data->speed;
        if (distance <= 0 || speed <= 0)
            return remove_spell(spell);
        spell.start_return_projectile(match, spell.ability.destination, speed);
        auto& p = spell.particles(match);
        const auto texture = FourCC{static_cast<unsigned>(spell.data->integer(
            FourCC::from_text(victim ? "tlif" : "tdef"), FourCC::from_text("elec").value))};
        spell.particle_material(match, p, 0x1a012008, texture);
        const float size = .3f, life = 1.f; // Native constants 0x4cb0bc / 0x4cb0c0.
        p.set_property(129, &size, &life);
        spell.emission = spell.data->real(FourCC::from_text(victim ? "flif" : "fdef"), 1);
        spell.remaining = static_cast<int>(distance / speed) + ticks;
        spell.stage = 17;
    }
    if (spell.stage == 17) {
        if ((spell.remaining -= ticks) <= 0) {
            if (caster)
                caster->change_mana_fixed(spell.rate * 65536);
            drain_support(spell);
        }
    }
    spell.age += ticks;
    for (auto& p : spell.particle_systems) {
        p.set_position(spell.motion.position);
        p.advance(spell.age, spell.emission);
    }
    if (spell.stage == 240 &&
        std::ranges::none_of(spell.particle_systems, [](const auto& p) { return p.alive(); }))
        return remove_spell(spell);
    return 1;
}
int run_oil_affect(Match& match, SpellActor& spell, int ticks) {
    auto* victim = living_unit(spell.effect_target);
    constexpr std::uintptr_t ignite = 0x4131d0;
    auto& p = spell.particles(match);
    if (!spell.stage) {
        if (!victim)
            return remove_spell(spell);
        victim->name_reference(FourCC::from_text("OILY"), &spell);
        spell.particle_material(match, p, 0x0a011008,
                                FourCC{static_cast<unsigned>(spell.data->integer(
                                    FourCC::from_text("ttex"), FourCC::from_text("oile").value))});
        const auto& low = victim->motion.box_min;
        const auto& high = victim->motion.box_max;
        const Vec3 half{(high.x - low.x) * .5f, (high.y - low.y) * .5f, (high.z - low.z) * .5f};
        spell_target_point(spell.motion.position, victim);
        const float size = std::clamp(std::sqrt(half.x * half.x + half.y * half.y + half.z * half.z) * .25f,
                                      .3f, 2.f),
                    life = spell.data->duration;
        p.set_property(132, &size, &life);
        const float a[]{-half.x, -half.y, -half.z}, b[]{half.x, half.y, half.z};
        p.set_property(2, a, b);
        const float cycles = std::bit_cast<float>(8);
        p.set_property(9, &cycles, nullptr);
        spell.rate = 1;
        match.functions.try_emplace(ignite, [](MatchObject& object, int) {
            auto& oil = static_cast<SpellActor&>(object);
            if (oil.stack.size() >= 2) {
                const int type = std::bit_cast<int>(oil.stack[0]);
                if ((type & 0x70) == 0x20 && (type & 7)) {
                    oil.stage = 16;
                    oil.stack[1] *= 2;
                }
            }
            return -3;
        });
        match.watch(*victim, 0, {&spell, spell.reference(), ignite});
        spell.stage = 1;
    }
    if (spell.stage == 1 && spell.rate) {
        spell.rate = 0;
        for (int n = 0; n < 32; ++n)
            p.emit(spell.age, 1);
        spell.remaining = static_cast<int>(spell.data->duration * 60);
    }
    if (!victim || (spell.stage == 1 && (spell.remaining -= ticks) <= 0))
        spell.stage = 240;
    if (victim) {
        spell_target_point(spell.motion.position, victim);
        if (spell.stage == 1) {
            victim->orders.clear();
            victim->stats.changed |= 0x10000;
            victim->stats.target[16] = 0;
            victim->fsm().event(8);
        } else {
            match.unwatch(*victim, 0, spell.reference(), &spell);
            victim->unname(FourCC::from_text("OILY"), &spell);
            if (spell.stage == 16) {
                SpellActor::launch_burning_limbs(match, *victim);
                victim->burn_near(spell.motion.position, 1, 120);
                spell_damage(match, &spell, victim, spell.data->damage_type, spell.data->amount, 0);
                spell.stage = 240;
            }
        }
    }
    if (spell.stage == 240) {
        p.fade(.25f, .5f);
        spell.stage = 241;
    }
    p.set_position({}); // Oil motes follow the victim in the host's local frame.
    spell.age += ticks;
    p.advance(spell.age, 0);
    return spell.stage == 241 && !p.alive() ? remove_spell(spell) : 1;
}
int run_oil(Match& match, SpellActor& spell, int ticks) {
    if (!spell.stage) {
        spell.sprite = {true,
                        FourCC{static_cast<unsigned>(
                            spell.data->integer(FourCC::from_text("ttex"), FourCC::from_text("oile").value))},
                        0x0a011008, 0xffffffff, 1.5f};
        spell.stage = 1;
        spell.sound_event(match, 1);
    }
    if (spell.stage == 1) {
        auto* caster = dynamic_cast<CreatureActor*>(spell.owner);
        if (!caster || (caster->state_flags & 0x10))
            return remove_spell(spell);
        caster->write_reference_point(spell.motion.position, 0, true);
        if (!(caster->state_flags & 0x20000000))
            return 1;
        spell.start_creature_projectile(match, static_cast<int>(spell.data->range * 180 / spell.data->speed));
        spell.sprite.size *= .75f;
        spell.motion.flags = 5;
        spell.motion.collision_mask = 0x114015;
        spell.friendly_ignore_time = 0;
        const int count = std::clamp(spell.data->count, 1, 64);
        for (int n = 1; n < count; ++n) {
            auto& child =
                launch_child(match, spell, SpellActor::Effect::viscous_oil, nullptr, spell.motion.position);
            child.sprite = spell.sprite;
            child.start_return_projectile(match, child.motion.position, 0);
            child.remaining = spell.remaining;
            child.stage = 17;
            child.motion.flags = 5;
            child.motion.collision_mask = 0x114015;
            child.friendly_ignore_time = 0;
            child.motion.velocity = spell.motion.velocity;
            spell_scatter(child.motion.velocity, 2000, match.random());
            const float spread = spell.data->speed * .2f;
            child.motion.velocity.x += (match.random().next() / 16384.f - 1) * spread;
            child.motion.velocity.y += (match.random().next() / 16384.f - 1) * spread;
            child.motion.velocity.z += (match.random().next() / 16384.f - 1) * spread;
        }
    }
    spell.sprite.frame += ticks * .25f;
    if (spell.stage >= 240) {
        spell.sprite.size = std::max(0.f, spell.sprite.size - ticks / 30.f);
        return spell.sprite.size == 0 ? remove_spell(spell) : 1;
    }
    if (const int flying = spell.fly_creature_projectile(match, ticks)) {
        // The native Icarus-family host shrinks expired oil bodies before retiring them.
        if (spell.stage == 240)
            spell.sprite.enabled = true;
        return flying;
    }
    attach_hit(match, spell, spell.impact_target, spell.motion.position);
    match.scar_requests.push_back({FourCC{static_cast<unsigned>(spell.data->integer(
                                       FourCC::from_text("tscr"), FourCC::from_text("scar").value))},
                                   spell.motion.position, match.clock().match, ++match.terrain_revision});
    const auto oil = [&](MatchObject& object) {
        auto* victim = living_unit(&object);
        if (!victim || spell_box_distance_squared(spell.motion.position, victim->motion,
                                                  spell.data->spread_limit * spell.data->spread_limit) >
                           spell.data->spread_limit * spell.data->spread_limit)
            return 0;
        if (auto* previous = dynamic_cast<SpellActor*>(victim->named(FourCC::from_text("OILY"))))
            ++previous->rate;
        else {
            auto& slick = launch_child(match, spell, SpellActor::Effect::viscous_oil_affect, victim,
                                       victim->motion.position);
            slick.run(match, 0);
        }
        return 0;
    };
    match.visit_group(0, 5, oil);
    match.visit_group(1, 5, oil);
    return remove_spell(spell);
}
int run_soul_eater(Match& match, SpellActor& spell, int ticks) {
    auto* caster = dynamic_cast<CreatureActor*>(spell.owner);
    auto* soul = dynamic_cast<SoulActor*>(spell.effect_target);
    if (!caster || !caster->alive())
        return remove_spell(spell);
    spell.motion.position = caster->motion.position;
    if (!spell.stage) {
        if (!soul || !caster->allegiance || !(soul->eligible_sides & caster->allegiance->identity))
            return remove_spell(spell);
        spell.rate = soul->count;
        spell.sound_event(match, 2);
        soul->collect(match, caster);
        spell.stage = 1;
    }
    if (spell.stage == 1) {
        if (!soul || soul->collector != caster)
            return remove_spell(spell);
        if (!(soul->soul_flags & 0xc0000000))
            return 1;
        const auto rescale = [&](unsigned index, int value) {
            const int old = caster->stats.base[index];
            const int next = std::clamp(value, 0, 32767);
            caster->stats.changed |= 1u << index;
            caster->stats.base[index] = static_cast<std::int16_t>(next);
            if (old) { // 0x41e1f0 truncates next * (value / old) in double precision.
                caster->stats.current[index] = static_cast<std::int16_t>(
                    static_cast<std::int64_t>(next * (static_cast<double>(caster->stats.current[index]) / old)));
                caster->stats.target[index] = static_cast<std::int16_t>(
                    static_cast<std::int64_t>(next * (static_cast<double>(caster->stats.target[index]) / old)));
            }
        };
        for (int i = 0; i < spell.rate; ++i) {
            rescale(5, caster->stats.base[5] + 500);
            rescale(6, (caster->stats.base[5] + caster->stats.base[6] * 3) / 4);
            for (unsigned index = 9; index <= 13; ++index)
                rescale(index, static_cast<int>(caster->stats.base[index] * .9));
            caster->heal(match, caster->stats.base[5], 256, 0);
        }
        spell.remaining = 100 * (3 * spell.rate + 9);
        spell.stage = 17; // SoulEaterRun stage 16 has no 0x438FF0 hit burst.
    }
    if (spell.stage == 17) {
        if (soul && !(soul->state_flags & MatchObject::removed_flag)) {
            // 0x44e660 / 0x476730: absorb at the animation's first reference point,
            // overriding the ordinary soul collector's collision-box centre.
            soul->collector = nullptr;
            caster->write_reference_point(soul->destination, 0, true);
            if ((match.random().next() & 63) < ticks)
                spell.sound_event(match, 8);
        } else
            spell.stage = 18;
    }
    for (unsigned bone = 0; bone < caster->bone_count(); ++bone)
        caster->request_bone_scale(bone, {1.5f, 1.5f, 1});
    return (spell.remaining -= ticks) <= 0 ? remove_spell(spell) : 1;
}
int run_resurrect(Match& match, SpellActor& spell, int ticks) {
    // GD_ResurrectRun 0x41bbb0 launches a separate revival host (0x41b8b0).
    // The travelling sprite fades independently while that host spins and restores the corpse.
    if (spell.revival_role == SpellActor::RevivalRole::streamer) {
        auto* corpse = dynamic_cast<CreatureActor*>(spell.effect_target);
        if (!spell.stage) {
            const auto* body = corpse ? corpse->source_motion() : nullptr;
            const Vec3 half = body ? Vec3{(body->box_max.x - body->box_min.x) * .3f,
                                          (body->box_max.y - body->box_min.y) * .3f,
                                          (body->box_max.z - body->box_min.z) * .3f}
                                   : Vec3{.3f, .3f, .3f};
            spell.initialize_spike_surface(match, half);
            spell.surface.textures[0] = FourCC{static_cast<unsigned>(
                spell.data->integer(FourCC::from_text("tray"), FourCC::from_text("rayz").value))};
            spell.surface.material =
                spell.data->integer(FourCC::from_text("iadd"), 1) ? 0x1a012009 : 0x1a010401;
            const auto channel = [&](const char* key, int fallback) {
                return static_cast<unsigned>(spell.data->integer(FourCC::from_text(key), fallback)) & 255;
            };
            spell.surface.color =
                0xff000000u | channel("ired", 255) | channel("igrn", 207) << 8 | channel("iblu", 63) << 16;
            spell.remaining = 40;
            spell.stage = 1;
        }
        if (!corpse || (spell.remaining -= ticks) <= 0)
            spell.stage = 240;
        spell.surface.opacity = spell.stage >= 240 ? std::max(0.f, spell.surface.opacity - ticks / 30.f)
                                                   : std::min(1.f, spell.surface.opacity + ticks / 30.f);
        if (corpse) {
            Vec3 at;
            spell_target_point(at, corpse);
            spell.motion.position = {spell.motion.position.x + (at.x - spell.motion.position.x) * .2f,
                                     spell.motion.position.y + (at.y - spell.motion.position.y) * .2f,
                                     spell.motion.position.z + (at.z - spell.motion.position.z) * .2f};
        }
        for (auto& ray : spell.surface.rays_data)
            ray.phase -= ticks * ray.speed;
        return spell.stage >= 240 && spell.surface.opacity == 0 ? remove_spell(spell) : 1;
    }
    if (spell.revival_role == SpellActor::RevivalRole::body) {
        auto* soul = dynamic_cast<SoulActor*>(spell.effect_target);
        auto* corpse = dynamic_cast<CreatureActor*>(spell.impact_target);
        auto& p = spell.particles(match);
        if (!spell.stage) {
            if (!soul || !corpse)
                return remove_spell(spell);
            spell.particle_material(match, p, 0x1a012008, FourCC::from_text("glo2"));
            const auto& low = corpse->motion.box_min;
            const auto& high = corpse->motion.box_max;
            const float x = (high.x - low.x) * .5f, y = (high.y - low.y) * .5f, z = (high.z - low.z) * .5f;
            const float size = std::clamp(std::sqrt(x * x + y * y + z * z) * .25f, .3f, 2.f), life = 1;
            p.set_property(129, &size, &life);
            const float gravity[]{0, 0, -.002725f};
            p.set_property(4, gravity, gravity);
            p.flags |= 0x10000;
            spell.remaining = static_cast<int>(spell.data->duration * 60);
            spell.cast_alpha = 1;
            spell.stage = 16;
        }
        if (!corpse)
            spell.stage = 240;
        if (spell.stage == 16)
            spell.stage = soul && soul->reconstitute(match, *corpse) ? 32 : 240;
        else if (spell.stage == 32) {
            spell.sound_event(match, 2);
            corpse->fsm().event(49);
            spell.stage = 33;
        } else if (spell.stage == 33) {
            spell.rate = std::min(120, spell.rate + ticks);
            const float phase = spell.rate / 120.f;
            corpse->motion.yaw += phase * ticks * .052359833f;
            corpse->motion.velocity.z = std::sin(phase * 3.14159f) * .03f;
            if ((spell.remaining -= ticks) <= 0)
                spell.stage = 128;
        } else if (spell.stage == 128) {
            spell.sound_event(match, 4);
            corpse->fsm().event(50);
            spell.stage = 240;
        }
        if (spell.stage >= 240)
            spell.cast_alpha = std::max(0.f, spell.cast_alpha - ticks / 60.f);
        if (corpse) {
            spell_target_point(spell.motion.position, corpse);
            if (spell.remaining <= 30 && !spell.had_target) {
                auto& rays = launch_child(match, spell, SpellActor::Effect::gd_resurrect, corpse,
                                          spell.motion.position);
                rays.revival_role = SpellActor::RevivalRole::streamer;
                rays.run(match, 0);
                spell.had_target = true;
            }
            if (spell.stage < 240 && corpse->bone_count())
                for (int n = 0; n < 2 * ticks; ++n) {
                    Vec3 point;
                    const auto bone = match.random().next() % corpse->bone_count();
                    if (corpse->write_bone_point(point, bone, &match.random())) {
                        p.set_position(point);
                        p.emit(spell.age, 1);
                    }
                }
        }
        spell.age += ticks;
        p.advance(spell.age, 0);
        return spell.stage >= 240 && spell.cast_alpha == 0 && !p.alive() ? remove_spell(spell) : 1;
    }
    if (!spell.stage) {
        auto* soul = dynamic_cast<SoulActor*>(spell.effect_target);
        auto* corpse = soul ? dynamic_cast<CreatureActor*>(soul->anchor)
                            : dynamic_cast<CreatureActor*>(spell.effect_target);
        if (!soul && corpse)
            match.visit_group(3, 2, [&](MatchObject& object) {
                if (auto* found = dynamic_cast<SoulActor*>(&object); found && found->anchor == corpse)
                    soul = found;
                return 0;
            });
        if (!soul || !corpse)
            return remove_spell(spell);
        spell.effect_target = soul;
        spell.impact_target = corpse;
        if (auto* caster = dynamic_cast<CreatureActor*>(spell.owner))
            caster->write_reference_point(spell.motion.position, 0, true);
        spell.ability.origin = spell.motion.position;
        spell_target_point(spell.point, corpse);
        spell.ability.length = std::max(
            1.f, static_cast<float>(std::sqrt(distance_squared(spell.point, spell.motion.position))));
        spell.sprite = {true,
                        FourCC{static_cast<unsigned>(
                            spell.data->integer(FourCC::from_text("tspr"), FourCC::from_text("drgn").value))},
                        0x1a012008, 0xffffffff, spell.data->real(FourCC::from_text("fspR"), 1.5f)};
        auto& trail = spell.particles(match);
        spell.particle_material(match, trail, 0x1a012008,
                                FourCC{static_cast<unsigned>(spell.data->integer(
                                    FourCC::from_text("ttrl"), FourCC::from_text("brth").value))});
        const float size = spell.sprite.size * .25f, life = spell.sprite.size * .6667f;
        trail.set_property(129, &size, &life);
        spell.scale_target = spell.sprite.size;
        spell.cast_alpha = 0;
        spell.stage = 1;
        spell.sound_event(match, 1);
    }
    auto* corpse = dynamic_cast<CreatureActor*>(spell.impact_target);
    if (!corpse)
        spell.stage = 240;
    if (spell.stage == 1) {
        spell.cast_alpha = std::min(1.f, spell.cast_alpha + ticks * spell.data->speed / spell.ability.length);
        spell_target_point(spell.point, corpse);
        spell.motion.position = {
            spell.ability.origin.x + (spell.point.x - spell.ability.origin.x) * spell.cast_alpha,
            spell.ability.origin.y + (spell.point.y - spell.ability.origin.y) * spell.cast_alpha,
            spell.ability.origin.z + (spell.point.z - spell.ability.origin.z) * spell.cast_alpha};
        if (spell.cast_alpha >= 1) {
            if (spell.effect_target) {
                auto& revival = launch_child(match, spell, SpellActor::Effect::gd_resurrect,
                                             spell.effect_target, spell.motion.position);
                revival.revival_role = SpellActor::RevivalRole::body;
                revival.impact_target = corpse;
                revival.run(match, 0);
            }
            spell.stage = 240;
            spell.effect_target = spell.impact_target = nullptr;
        }
    }
    if (spell.stage >= 240)
        spell.sprite.size = std::max(0.f, spell.sprite.size - ticks / 30.f);
    spell.age += ticks;
    for (auto& trail : spell.particle_systems) {
        trail.set_position(spell.motion.position);
        trail.advance(spell.age, spell.stage < 240 ? 1.f : 0.f);
    }
    spell.sprite.frame += ticks * .26666667f;
    return spell.stage >= 240 && spell.sprite.size == 0 &&
                   std::ranges::none_of(spell.particle_systems, [](const auto& p) { return p.alive(); })
               ? remove_spell(spell)
               : 1;
}
int run_scry(Match& match, SpellActor& spell, int ticks) {
    if (!spell.stage) {
        auto* caster = dynamic_cast<CreatureActor*>(spell.owner);
        if (!caster)
            return remove_spell(spell);
        spell.motion.position = {caster->motion.position.x + caster->motion.orientation[3] * 2,
                                 caster->motion.position.y + caster->motion.orientation[4] * 2,
                                 caster->motion.position.z + 3};
        spell.point = {caster->motion.position.x + caster->motion.orientation[3] * spell.data->range,
                       caster->motion.position.y + caster->motion.orientation[4] * spell.data->range,
                       caster->motion.position.z + 3};
        spell.effect_target = nullptr;
        spell.launch_projectile(match);
        spell.motion.collision_mask = 0;
        spell.sprite = {true,
                        FourCC{static_cast<unsigned>(
                            spell.data->integer(FourCC::from_text("ttex"), FourCC::from_text("scri").value))},
                        0x0a011008, 0xffffffff, 0};
        spell.remaining = static_cast<int>(spell.data->duration * 60);
        spell.cast_alpha = 0;
        spell.sound_event(match, 2);
        spell.stage = 32;
    }
    if (spell.stage < 240 && (spell.remaining -= ticks) <= 0)
        spell.stage = 240;
    spell.cast_alpha = std::clamp(spell.cast_alpha + (spell.stage < 240 ? ticks : -ticks) / 60.f, 0.f, 1.f);
    spell.sprite.size = 2 * spell.cast_alpha;
    spell.sprite.color = (static_cast<unsigned>(spell.cast_alpha * 255) << 24) | 0xffffff;
    bool arrived = false;
    if (spell.stage < 240) {
        // 0x41c1e0 stage 32: once a target has been held (+56), losing it never re-searches (0x41c180).
        if (living_unit(spell.effect_target))
            spell.host.initialized = true;
        else if (spell.host.initialized)
            spell.effect_target = nullptr;
        else {
            spell.effect_target = nullptr;
            double nearest = spell.data->range * spell.data->range;
            for (unsigned group : {0u, 1u})
                match.visit_group(group, 5, [&](MatchObject& object) {
                    if (auto* actor = living_unit(&object);
                        actor && actor != spell.owner && actor->enemy(&spell)) {
                        const double distance =
                            distance_squared(actor->motion.position, spell.motion.position);
                        if (distance < nearest) {
                            nearest = distance;
                            spell.effect_target = actor;
                        }
                    }
                    return 0;
                });
        }
        if (spell.effect_target) {
            spell_target_point(spell.point, spell.effect_target);
            spell.point.z += 3;
        }
        // Steer while the planar gap is at least sqrt(10); inside it, 0x41c1e0 damps the velocity to 1%.
        const float dx = spell.motion.position.x - spell.point.x, dy = spell.motion.position.y - spell.point.y;
        if (dx * dx + dy * dy >= 10)
            spell_approach_velocity(spell.motion, spell.point, ticks * spell.data->acceleration,
                                    spell.data->speed);
        else
            arrived = true;
    }
    if (spell.stage >= 240 || arrived)
        spell.motion.velocity = {spell.motion.velocity.x * .01f, spell.motion.velocity.y * .01f,
                                 spell.motion.velocity.z * .01f};
    if (match.terrain) {
        const float floor = static_cast<float>(terrain_surface(*match.terrain, spell.motion.position.x,
                                                               spell.motion.position.y)) +
                            3;
        spell.motion.position.z = std::max(spell.motion.position.z, floor);
    }
    // ScryRun calls 0x46b150 with a full-circle view and spread * current fade.
    if (match.terrain && spell.allegiance && (spell.host.ticks -= ticks) <= 0) {
        spell.host.ticks = 16;
        const float range = spell.data->spread_limit * spell.cast_alpha;
        for (auto* side : match.side_order) {
            if (side != spell.allegiance &&
                !(match.shared_vision && ((side->allies & spell.allegiance->identity) ||
                                          (spell.allegiance->allies & side->identity))))
                continue;
            if (side->fog)
                side->fog->reveal(*match.terrain, spell.motion.position, 0, 3.14159f, 1, range);
            for (const auto& object : match.objects()) {
                const auto* body = object->source_motion();
                if (!(object->kind_flags & 23) || (object->state_flags & MatchObject::removed_flag) || !body)
                    continue;
                if (const auto* actor = dynamic_cast<const CreatureActor*>(object.get());
                    actor &&
                    (!actor->alive() || (actor->fsm().state() && (actor->fsm().state()->flags & 0x4000))))
                    continue;
                if (const auto* building = dynamic_cast<const BuildingActor*>(object.get());
                    building && building->health <= 0)
                    continue;
                const Vec3 delta{body->position.x - spell.motion.position.x,
                                 body->position.y - spell.motion.position.y,
                                 body->position.z + body->box_max.z - spell.motion.position.z};
                const float planar = delta.x * delta.x + delta.y * delta.y;
                if (planar > range * range)
                    continue;
                Vec3 hit;
                const float contact =
                    terrain_intercept(*match.terrain, spell.motion.position, delta, hit, planar, true);
                if (contact <= 0 || contact >= 1)
                    side->see(*object);
            }
        }
    }
    spell.sprite.frame += ticks * spell.data->real(FourCC::from_text("ffps"), 10) / 60;
    return spell.stage >= 240 && spell.cast_alpha == 0 ? remove_spell(spell) : 1;
}
} // namespace
int SpellActor::run_support(Match& match, int ticks) {
    if (!data)
        throw Error("Creature support ability requires its SPEL definition");
    switch (effect) {
    case Effect::arachon_life:
        return run_arachon_life(match, *this, ticks);
    case Effect::arachon_fire:
        return run_arachon_fire(match, *this, ticks);
    case Effect::cage_pull:
        return run_cage_pull(match, *this, ticks);
    case Effect::bugzapper:
        return run_bugzapper(match, *this, ticks);
    case Effect::viscous_oil:
        return run_oil(match, *this, ticks);
    case Effect::soul_eater:
        return run_soul_eater(match, *this, ticks);
    case Effect::gd_resurrect:
        return run_resurrect(match, *this, ticks);
    case Effect::scry:
        return run_scry(match, *this, ticks);
    case Effect::web_pull_hold: {
        // 0x41DF10: when the pull ends, 0x438FF0 bursts the web ('tSPK') on the victim, then retires.
        const auto* parent = effect_target && !(effect_target->state_flags & removed_flag)
                                 ? dynamic_cast<SpellActor*>(effect_target)
                                 : nullptr;
        if (!parent || parent->stage >= 240 || !parent->effect_target) {
            auto& burst = launch_sacrifice_host(match, *this, Effect::spell_hit);
            burst.effect_target = parent ? parent->effect_target : nullptr;
            burst.motion.position = motion.position;
            burst.run(match, 0);
            return remove_spell(*this);
        }
        {
            spell_target_point(motion.position, parent->effect_target);
            if (const auto* body = parent->effect_target->source_motion())
                shockwave.axes = {(body->box_max.x - body->box_min.x) * .5f,
                                  (body->box_max.y - body->box_min.y) * .5f,
                                  (body->box_max.z - body->box_min.z) * .5f};
        }
        cast_alpha = std::min(1.f, cast_alpha + 1.f / 15);
        shockwave.texture = FourCC{
            static_cast<unsigned>(data->integer(FourCC::from_text("tstx"), FourCC::from_text("webc").value))};
        shockwave.material = data->integer(FourCC::from_text("iadd"), 0) ? 0x1a012008 : 0x02010008;
        shockwave.radius = cast_alpha;
        shockwave.color = 0xffffffu | static_cast<unsigned>(cast_alpha * 255) << 24;
        shockwave.frame += ticks * data->real(FourCC::from_text("ffps"), 15) / 60;
        return 1;
    }
    case Effect::viscous_oil_affect:
        return run_oil_affect(match, *this, ticks);
    default:
        throw Error("Invalid creature support ability");
    }
}
} // namespace sacrifice
