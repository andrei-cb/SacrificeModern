#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/soul_actor.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include "sacrifice/spell_data.hpp"
#include <algorithm>
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
int run_klappon_rock(Match& match, SpellActor& spell, int ticks) {
    // KlapponStart 0x41ada0 assigns KlapponRun / BoulderSquatSpawn 0x41acf0.
    if (!spell.stage) {
        spell.start_creature_projectile(match, 1200, FourCC::from_text("roks"));
        const auto angular = [&] {
            return static_cast<float>((match.random().next() - 0x4000) * .00000152587890625);
        };
        spell.motion.angular_velocity = {angular(), angular(), angular()};
        spell.motion.flags |= 0x38;
        spell.motion.collision_mask |= 0x80000;
        // 0x4380A0(emission 1, life .5) -> 0x438050: tRT1 (default 'dust') body-sized trail.
        SpellActor::launch_model_trail(
            match, spell, 0x0a011008,
            FourCC{static_cast<unsigned>(spell.data->integer(FourCC::from_text("tRT1"), FourCC::from_text("dust").value))},
            1, .5f);
    }
    if (const int flying = spell.fly_creature_projectile(match, ticks))
        return flying;
    // KlapponRun stage 32: 0x439270 shatter and 0x44D7E0 damage only; no 0x438FF0 hit burst.
    SpellActor::launch_rock_shatter(match, spell);
    spell_damage(match, &spell, spell.impact_target, hit_type(spell, 51), hit_amount(spell, 120), spell.data->radius);
    return remove_spell(spell);
}
int run_klappon_fire(Match& match, SpellActor& spell, int ticks) {
    // KlapponFireRun 0x41aea0: 0x44D7E0 then 0x4375A0.
    if (!spell.stage) {
        spell.start_creature_projectile(match, 1200);
        auto& p = spell.particles(match);
        spell.particle_material(match, p, 0x1a012008,
                                FourCC{static_cast<unsigned>(spell.data->integer(
                                    FourCC::from_text("ttex"), FourCC::from_text("firy").value))});
        const float size = .4f, life = .75f;
        p.set_property(131, &size, &life);
        const float low[3]{-.05f, 0, 0}, high[3]{.05f, 0, 0};
        p.set_property(3, low, high);
        spell.emission = 2;
    }
    if (const int flying = spell.fly_creature_projectile(match, ticks))
        return flying;
    auto* victim = spell.impact_target;
    // Contact only: 0x44D7E0 damage, then the INERT_RunFire burst. KlapponFireRun has no 0x45CB90 burn.
    strike(match, spell, victim, spell.motion.position, false, 160, 35);
    SpellActor::launch_inert_fire(match, spell, FourCC::from_text("firy"), .35f, 2, .75f, 2, .5f);
    spell.emission = 0;
    spell.stage = 240;
    return 1;
}
int run_death_klappon(Match& match, SpellActor& spell, int ticks) {
    // DeathKlapponStart 0x413a50 assigns InsectSwarmRun 0x4315a0 stage 4: 0x438FF0 + 0x44D7E0.
    if (!spell.host.initialized) {
        spell.start_creature_projectile(match, 800);
        spell.motion.collision_mask = 0x114015;
        spell.friendly_ignore_time = 0;
        auto& p = spell.particles(match);
        spell.particle_material(match, p, 0x0a011008,
                                FourCC{static_cast<unsigned>(spell.data->integer(
                                    FourCC::from_text("ttex"), FourCC::from_text("fb_g").value))});
        static constexpr float swarm[8]{.1f, .2f, .2f, .2f, .2f, .05f, .05f, .1f};
        const float count = std::bit_cast<float>(spell.data->integer(FourCC::from_text("inum"), 32));
        p.set_position(spell.motion.position, &spell.motion.position);
        p.set_property(136, &count, swarm);
        spell.host.initialized = true;
        spell.flight_remaining = 800;
        spell.generic.collision_radius = .2f;
        spell.stage = 3; // Native DeathKlapponStart rebinds to the ballistic InsectSwarmRun stage.
    }
    return spell.run_insect_swarm(match, ticks);
}
// 0x416a50: shared GunheadFuse/GunheadTorch emitter. ttyp 'flam' uses the flame preset at rate 1;
// otherwise a small rising spark preset at rate .5.
void gunhead_flame(Match& match, SpellActor& spell) {
    auto& p = spell.particles(match);
    spell.particle_material(match, p,
                            spell.data->integer(FourCC::from_text("iadd"), 1) ? 0x1a012008 : 0x1a010401,
                            FourCC{static_cast<unsigned>(spell.data->integer(
                                FourCC::from_text("tptx"), FourCC::from_text("firy").value))});
    const bool flame = spell.data->integer(FourCC::from_text("ttyp"), FourCC::from_text("flam").value) ==
                       static_cast<int>(FourCC::from_text("flam").value);
    const float fire[]{.3f, 1, .5f}, size = .15f, life = .5f;
    if (flame)
        p.set_property(128, fire, fire);
    else {
        p.set_property(131, &size, &life);
        const float low[]{-.02f, -.02f, 0}, high[]{.02f, .02f, .075f}; // 0x4cb04c / 0x4cb058.
        p.set_property(3, low, high);
    }
    spell.emission = flame ? 1.f : .5f;
}
int run_fuse(Match& match, SpellActor& spell, int ticks) {
    // GunheadFuseRun 0x416c90 remaining 150 then 0x416C30 at the caster.
    if (!spell.stage) {
        gunhead_flame(match, spell);
        spell.remaining = 150;
        spell.stage = 1;
        spell.sound_event(match, 1);
    }
    if (spell.stage == 240) {
        spell.age += ticks;
        auto& p = spell.particle_systems.front();
        p.advance(spell.age, 0);
        return p.alive() ? 1 : remove_spell(spell);
    }
    auto* caster = dynamic_cast<CreatureActor*>(spell.owner);
    if (!caster || (caster->state_flags & 0x10)) {
        spell.stop_sound_event(1);
        spell.emission = 0;
        spell.stage = 240;
        return 1;
    }
    spell.remaining -= ticks;
    Vec3 from = caster->motion.position, to = from;
    const float t = std::max(0.f, spell.remaining / 60.f);
    const Vec3 offset{0, .5f, -.5f};
    caster->write_bone_point(from, 11, nullptr, t > .5f ? &offset : nullptr);
    caster->write_bone_point(to, t <= .5f ? 10 : 11);
    const float factor = t <= .5f ? t * 2 : t * 2 - 1;
    spell.motion.position = {to.x + (from.x - to.x) * factor, to.y + (from.y - to.y) * factor,
                             to.z + (from.z - to.z) * factor};
    spell.age += ticks;
    auto& p = spell.particle_systems.front();
    p.set_position(spell.motion.position);
    p.advance(spell.age, spell.emission);
    if (!(caster->state_flags & 0x20000000))
        return 1;
    // 0x416C30 launches a aimed beam (or a counted gatling burst), not an explosion at the caster.
    auto& shot = SpellActor::launch_sacrifice_host(match, spell, SpellActor::Effect::gunhead_lightning);
    shot.effect_target = spell.effect_target;
    shot.point = spell.point;
    if (spell.allegiance)
        spell.allegiance->attach(shot);
    shot.run(match, 0);
    spell.stop_sound_event(1);
    spell.emission = 0;
    spell.stage = 240;
    return 1;
}
int run_torch(Match& match, SpellActor& spell, int ticks) {
    // GunheadTorchRun 0x416b60: follow caster bone 6.
    if (!spell.stage) {
        gunhead_flame(match, spell);
        spell.stage = 1;
    }
    auto* caster = dynamic_cast<CreatureActor*>(spell.owner);
    const bool active = caster && caster->alive();
    if (active) {
        spell.motion.position = caster->motion.position;
        const Vec3 offset{0, -1, 0};
        caster->write_bone_point(spell.motion.position, 6, nullptr, &offset);
    }
    auto& p = spell.particle_systems.front();
    p.set_position(spell.motion.position);
    spell.age += ticks;
    p.advance(spell.age, active ? spell.emission : 0.f);
    return active || p.alive() ? 1 : remove_spell(spell);
}
int run_gunhead_lightning(Match& match, SpellActor& spell, int ticks) {
    const bool gatling = spell.data->integer(FourCC::from_text("tmov"), 0) ==
                         static_cast<int>(FourCC::from_text("gatl").value);
    if (gatling && !spell.stage) {
        auto& line = spell.crossline;
        line.enabled = line.muzzle = true;
        line.texture = FourCC{static_cast<unsigned>(
            spell.data->integer(FourCC::from_text("tMFL"), FourCC::from_text("mfls").value))};
        line.material = spell.data->integer(FourCC::from_text("iMF+"), 1) ? 0x1a012008 : 0x0a011008;
        spell.rate = spell.data->count;
        spell.age = 0;
        spell.stage = 1;
        spell.sound_event(match, 2);
    }
    if (!spell.stage) {
        spell_target_point(spell.point, spell.effect_target);
        spell.ability.origin = spell.motion.position;
        spell.rate = spell.data->count;
        spell.age = 0;
        // 0x416e90: 0x4355A0(spell, length, deviation 0.5, iadd ? 2 : 1 strands, iadd ? 1 : 4). Neither
        // flag set traces a replacement target, so Rend keeps the selected soul beside other creatures.
        const bool additive = spell.data->integer(FourCC::from_text("iadd"), 1) != 0;
        spell.start_beam(match, spell.motion.position, spell.point, .5f, additive ? 2 : 1, additive ? 1u : 4u);
        spell.stage = 1;
    }
    if (gatling && spell.stage == 1) {
        if (!spell.owner || (spell.owner->state_flags & MatchObject::removed_flag))
            spell.rate = 0;
        if (auto* caster = dynamic_cast<CreatureActor*>(spell.owner))
            caster->write_reference_point(spell.motion.position, 0, true);
        // MUZZLE square at the gun, facing the shots' target (fMFz only bounds the native primitive).
        Vec3 aim = spell.point;
        spell_target_point(aim, spell.effect_target);
        spell.crossline.points = {{spell.motion.position, spell.data->real(FourCC::from_text("fMFx"), 2), false},
                                  {aim, 0, false}};
        const auto frames =
            std::max(match.texture_frames ? match.texture_frames(spell.crossline.texture) : 1u, 1u);
        spell.crossline.frame = std::fmod(spell.crossline.frame + ticks * .5f, static_cast<float>(frames));
        spell.age += ticks;
        while (spell.age >= 1.5f && spell.rate > 0) {
            spell.age -= 1.5f;
            --spell.rate;
            if (auto* caster = dynamic_cast<CreatureActor*>(spell.owner))
                caster->write_reference_point(spell.motion.position, 0, true);
            Vec3 endpoint = spell.point;
            spell_target_point(endpoint, spell.effect_target);
            Vec3 direction{endpoint.x - spell.motion.position.x, endpoint.y - spell.motion.position.y,
                           endpoint.z - spell.motion.position.z};
            if (auto* caster = dynamic_cast<CreatureActor*>(spell.owner))
                spell_scatter(direction, caster->stats.current[8], match.random());
            const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y +
                                           direction.z * direction.z),
                        scale = length > 0 ? spell.data->range * 1.5f / length : 0;
            endpoint = {spell.motion.position.x + direction.x * scale,
                        spell.motion.position.y + direction.y * scale,
                        spell.motion.position.z + direction.z * scale};
            auto* hit = spell_trace_ray(match, spell, spell.motion.position, endpoint);
            spell_damage(match, &spell, hit, spell.data->damage_type, spell.data->amount, spell.data->radius,
                         endpoint);
            attach_hit(match, spell, hit, endpoint);
        }
        if (spell.rate <= 0) {
            spell.stop_sound_event(2);
            return remove_spell(spell);
        }
        return 1;
    }
    const int state = spell.advance_beam(match, ticks);
    // 0x416e90: stage 1 → 16 once the beam connects. 0x441C30's last argument (0x18) is a network sync
    // mask, not a delay, so stage 16 strikes on the next call. Rend first validates and collects the soul
    // in stage 16 and strikes from stage 32 on the call after.
    if (gatling)
        return state ? 1 : remove_spell(spell);
    const auto hit = FourCC{static_cast<unsigned>(
        spell.data->integer(FourCC::from_text("thit"), FourCC::from_text("damg").value))};
    if (spell.stage == 1) {
        if (state == 2 || state == 0)
            spell.stage = 16;
        return state ? 1 : remove_spell(spell);
    }
    if (spell.stage == 16 && hit == FourCC::from_text("rend")) {
        auto* soul = dynamic_cast<SoulActor*>(spell.effect_target);
        if (soul && spell.allegiance && (soul->eligible_sides & spell.allegiance->identity)) {
            soul->collect(match, nullptr);
            spell.stage = 32;
        } else
            spell.stage = 240;
        return state ? 1 : remove_spell(spell);
    }
    if (spell.stage == 16 || spell.stage == 32) {
        auto* victim = spell.effect_target;
        if (hit == FourCC::from_text("rend")) {
            victim = nullptr;
            match.shake(spell.ability.destination, 16, 100);
        }
        spell.sound_event(match, 4);
        spell_damage(match, &spell, victim, spell.data->damage_type, spell.data->amount, spell.data->radius,
                     spell.ability.destination);
        auto& wave = SpellActor::launch_shockwave(match, spell, spell.data->spread_limit * 1.25f);
        wave.motion.position = spell.ability.destination;
        match.scar_requests.push_back({FourCC::from_text("scar"), spell.ability.destination, // 0x44CB50.
                                      match.clock().match, ++match.terrain_revision});
        attach_hit(match, spell, victim, spell.ability.destination);
        if (hit == FourCC::from_text("mana"))
            if (auto* target = living_unit(victim))
                target->change_mana_fixed(-32768 * target->maximum_mana);
        spell.stage = 240;
    }
    return state ? 1 : remove_spell(spell);
}
int run_firewalk(Match& match, SpellActor& spell, int ticks) {
    auto* caster = dynamic_cast<CreatureActor*>(spell.owner);
    auto& p = spell.particles(match);
    if (!spell.stage) {
        spell.particle_material(match, p, 0x1a012008, "firy");
        const float flame[]{.6f, 2, 2};
        p.set_property(128, flame, flame);
        const float low[]{-.5f, -.5f, 0}, high[]{.5f, .5f, 0};
        p.set_property(2, low, high);
        spell.emission = 1.5f;
        spell.stage = 1;
        spell.sound_event(match, 1);
    }
    if (!caster || !caster->alive() || (caster->state_flags & 0x10))
        spell.stage = 240;
    if (spell.stage == 1) {
        const Vec3 offset{0, -1, 0};
        caster->write_bone_point(spell.motion.position, 6, nullptr, &offset);
        if (caster->state_flags & 0x20000000) {
            spell.ability.origin = caster->motion.position;
            if (spell.effect_target)
                spell_target_point(spell.point, spell.effect_target);
            const Vec3 d{spell.point.x - spell.ability.origin.x, spell.point.y - spell.ability.origin.y,
                         spell.point.z - spell.ability.origin.z};
            const float distance = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z),
                        scale = distance > 0 ? std::min(distance * .5f, 15.f) / distance : 0;
            spell.motion.velocity = {d.x * scale, d.y * scale, d.z * scale};
            spell.age = 0;
            spell.stage = 17;
            spell.stop_sound_event(1);
            spell.sound_event(match, 2);
            caster->fsm().event(55);
        }
    }
    if (spell.stage == 17) {
        spell.age = std::min(1.f, spell.age + ticks / 60.f);
        const bool returning = spell.age >= .5f;
        const float t = returning ? spell.age * 2 - 1 : spell.age * 2;
        const Vec3 from = returning ? Vec3{spell.point.x - spell.motion.velocity.x,
                                           spell.point.y - spell.motion.velocity.y,
                                           spell.point.z - spell.motion.velocity.z}
                                    : spell.ability.origin,
                   to = returning ? spell.point
                                  : Vec3{spell.ability.origin.x + spell.motion.velocity.x,
                                         spell.ability.origin.y + spell.motion.velocity.y,
                                         spell.ability.origin.z + spell.motion.velocity.z};
        spell.motion.position = {from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t,
                                 from.z + (to.z - from.z) * t};
        spell.emission = std::sqrt(returning ? t : 1 - t) * 1.5f;
        if (match.terrain)
            spell.motion.position.z = static_cast<float>(
                terrain_surface(*match.terrain, spell.motion.position.x, spell.motion.position.y));
        if (spell.age >= 1) {
            caster->orders.clear();
            caster->motion.position = spell.motion.position;
            caster->fsm().event(2);
            spell.stage = 240;
        }
    }
    p.set_position(spell.motion.position);
    spell.host.time += ticks;
    p.advance(spell.host.time, spell.stage == 240 ? 0 : spell.emission);
    return spell.stage == 240 && !p.alive() ? remove_spell(spell) : 1;
}
} // namespace
int SpellActor::run_klappon(Match& match, int ticks) {
    if (!data)
        throw Error("Creature klappon ability requires its SPEL definition");
    switch (effect) {
    case Effect::klappon:
        return run_klappon_rock(match, *this, ticks);
    case Effect::klappon_fire:
        return run_klappon_fire(match, *this, ticks);
    case Effect::death_klappon:
        return run_death_klappon(match, *this, ticks);
    case Effect::gunhead_fuse:
        return run_fuse(match, *this, ticks);
    case Effect::gunhead_lightning:
        return run_gunhead_lightning(match, *this, ticks);
    case Effect::gunhead_torch:
        return run_torch(match, *this, ticks);
    case Effect::firewalk:
        return run_firewalk(match, *this, ticks);
    default:
        throw Error("Invalid creature klappon ability");
    }
}
} // namespace sacrifice
