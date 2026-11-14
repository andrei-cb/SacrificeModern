#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include "sacrifice/spell_data.hpp"
#include "sacrifice/spell_registry.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
FourCC cc(std::string_view text) {
    return FourCC::from_text(text);
}
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
CreatureActor* caster_of(SpellActor& spell) {
    if (auto* actor = dynamic_cast<CreatureActor*>(spell.owner))
        return actor;
    return living_unit(spell.effect_target);
}
void scale_resistances(CreatureActor& actor, float factor, bool current = true) {
    // 0x458830 writes slots 9..13.
    for (unsigned i = 9; i <= 13; ++i) {
        actor.stats.changed |= 1u << i;
        auto& slot = current ? actor.stats.current[i] : actor.stats.target[i];
        const double value = static_cast<double>(slot) * factor;
        slot = static_cast<std::int16_t>(value <= 0 ? 0 : static_cast<std::int64_t>(value));
    }
}
int run_life_shield(Match& match, SpellActor& spell, int ticks) {
    // LifeShieldRun 0x415370: 0x4692A0 name, 0x458830(0.5), duration * 60.
    // Native +460 overrides +476 on entry, then the affected creature stays in +476.
    auto* caster = spell.stage ? living_unit(spell.effect_target) : caster_of(spell);
    if (!spell.stage) {
        if (!caster)
            return remove_spell(spell);
        caster->name_reference(cc("PROT"), &spell);
        spell.effect_target = caster;
        scale_resistances(*caster, .5f);
        auto& sphere = spell.shockwave;
        sphere.texture = FourCC{static_cast<unsigned>(spell.data ?
            spell.data->integer(cc("ttex"), cc("ent1").value) : cc("ent1").value)};
        sphere.material = 0x1a012008;
        sphere.frames = match.texture_frames ? match.texture_frames(sphere.texture) : 1;
        sphere.radius = 0;
        spell.sound_event(match, 2);
        spell.remaining =
            spell.data && spell.data->duration > 0.f ? static_cast<int>(spell.data->duration * 60.f) : 1200;
        spell.stage = 1;
    }
    if (spell.stage == 1 && (!caster || !caster->alive() || (spell.remaining -= ticks) < 0)) {
        if (caster)
            caster->unname(cc("PROT"), &spell);
        spell.sound_event(match, 16);
        spell.stop_sound_event(2);
        spell.stage = 33;
    }
    auto& sphere = spell.shockwave;
    if (spell.stage == 33) {
        sphere.radius = std::max(0.f, sphere.radius - ticks * .05f);
        if (sphere.radius <= 0)
            return remove_spell(spell);
    } else {
        sphere.radius = std::min(1.f, sphere.radius + ticks * .05f);
        scale_resistances(*caster, .5f, false);
    }
    if (caster) {
        const auto& m = caster->motion;
        spell.motion.position = {m.position.x + (m.box_min.x + m.box_max.x) * .5f,
                                 m.position.y + (m.box_min.y + m.box_max.y) * .5f,
                                 m.position.z + (m.box_min.z + m.box_max.z) * .5f};
        const float x = (m.box_max.x - m.box_min.x) * .5f, y = (m.box_max.y - m.box_min.y) * .5f;
        const float radius = std::sqrt((x*x + y*y) * .5f);
        sphere.axes = {radius, radius, (m.box_max.z - m.box_min.z) * .5f};
    }
    sphere.frame += ticks * .25f;
    if (sphere.frame >= sphere.frames)
        sphere.frame = 0;
    return 1;
}
int run_protector(Match& match, SpellActor& spell) {
    // ProtectorRun 0x41e180: 0x492300(4, pos, spread, 0x41E130).
    if (spell.owner && spell.owner->source_motion())
        spell.motion.position = spell.owner->source_motion()->position;
    const float reach = spell.data->spread_limit;
    const auto id = FourCC{static_cast<unsigned>(spell.data->integer(cc("tspl"), cc("abah").value))};
    const auto* definition = match.spell_registry ? match.spell_registry->find(id) : nullptr;
    const auto protect = [&](MatchObject& object) {
        auto* actor = living_unit(&object);
        if (!actor)
            return 0;
        if (!actor->source_motion())
            return 0;
        if (spell_box_distance_squared(spell.motion.position, actor->motion, reach * reach) >
            static_cast<double>(reach) * reach)
            return 0;
        if (definition && actor->allegiance)
            SpellActor::launch_side_cast(match, *actor->allegiance, *definition, actor,
                                         actor->motion.position);
        return 0;
    };
    match.visit_group(MatchObject::group(MatchObject::creature), MatchObject::creature, protect);
    return remove_spell(spell);
}
int run_away(Match& match, SpellActor& spell) {
    // RunAwayStart 0x415340: 0x44E850(..., SpeedupStart 0x441870).
    auto* caster = caster_of(spell);
    if (caster) {
        auto& speed = SpellActor::launch_sacrifice_host(match, spell, SpellActor::Effect::speedup);
        speed.data = spell.data;
        speed.owner = caster;
        speed.effect_target = caster;
        speed.motion.position = caster->motion.position;
        speed.run(match, 0);
    }
    return remove_spell(spell);
}
int run_stealth(Match& match, SpellActor& spell, int ticks) {
    auto* caster = caster_of(spell);
    if (!spell.stage) {
        if (caster) {
            caster->fsm().event(57); // 0x41C060: enter the native stealth state.
            caster->name_reference(cc("INVI"), &spell);
            spell.sound_event(match, 2);
        }
        spell.stage = 1;
    }
    (void)ticks;
    if (!caster || !caster->alive() || caster->ritual_building || !caster->fsm().state() ||
        !(caster->fsm().state()->flags & 0x10000)) {
        if (caster)
            caster->unname(cc("INVI"), &spell);
        spell.sound_event(match, 16);
        return remove_spell(spell);
    }
    caster->fade_target = .75f;
    caster->fade_step = 1.f / 60.f;
    return 1;
}
int run_rock_form(Match& match, SpellActor& spell, int ticks) {
    auto* caster = caster_of(spell);
    if (!spell.stage) {
        if (caster) {
            caster->fsm().event(47);
            caster->name_reference(cc("ROCK"), &spell);
            spell.sound_event(match, 2);
            const FourCC model{static_cast<unsigned>(spell.data->integer(cc("tmdl"), 0))};
            if (model.value)
                spell.set_projectile_model(match, model, 0);
        }
        spell.stage = 1;
    }
    if (caster) {
        spell.motion.position = caster->motion.position;
        spell.motion.position.z -= caster->motion.box_min.z * .75f; // 0x41aa00: target +604 * 0.75.
        if (spell.stage == 1)
            spell.model_scale = std::min(1.f, spell.model_scale + ticks / 30.f);
    }
    if (spell.stage == 17) {
        spell.model_scale = std::max(0.f, spell.model_scale - ticks / 30.f);
        return spell.model_scale <= 0 ? remove_spell(spell) : 1;
    }
    if (!caster || !caster->alive() || caster->ritual_building || !caster->fsm().state() ||
        !(caster->fsm().state()->flags & 0x10000)) {
        if (caster)
            caster->unname(cc("ROCK"), &spell);
        spell.stage = 17;
        return 1;
    }
    for (unsigned i = 9; i <= 13; ++i) {
        caster->stats.changed |= 1u << i;
        caster->stats.current[i] = caster->stats.target[i] = 50;
    }
    return 1;
}
int run_play_dead(Match& match, SpellActor& spell, int ticks) {
    auto* caster = caster_of(spell);
    if (!spell.stage) {
        if (caster) {
            caster->fsm().event(46);
            caster->name_reference(cc("DEAD"), &spell);
            caster->stats.changed |= 1u << CreatureStats::regeneration;
            auto& regeneration = caster->stats.current[CreatureStats::regeneration];
            regeneration = static_cast<std::int16_t>(std::max(0, regeneration + 1500));
        }
        spell.stage = 1;
    }
    (void)ticks;
    (void)match;
    if (!caster || !caster->alive() || caster->ritual_building || !caster->fsm().state() ||
        !(caster->fsm().state()->flags & 0x10000)) {
        if (caster)
            caster->unname(cc("DEAD"), &spell);
        return remove_spell(spell);
    }
    caster->stats.changed |= 1u << CreatureStats::regeneration;
    caster->stats.target[CreatureStats::regeneration] =
        static_cast<std::int16_t>(std::max(0, caster->stats.target[CreatureStats::regeneration] + 1500));
    return 1;
}
int run_icarus(Match& match, SpellActor& spell, int ticks) {
    const auto fade = [&] {
        spell.motion.flags = spell.motion.collision_mask = 0;
        spell.motion.velocity = {};
        spell.stage = 240;
    };
    if (!spell.stage) {
        spell.sprite = {
            true, FourCC{static_cast<unsigned>(spell.data->integer(FourCC::from_text("ttex"), 0))},
            spell.data->integer(FourCC::from_text("iadd"), 1) ? 0x12012008u : 0x02011008u, 0xffffffff, .75f};
        spell.stage = 1;
        spell.sound_event(match, 1);
    }
    if (spell.stage == 1) {
        auto* caster = dynamic_cast<CreatureActor*>(spell.owner);
        if (!caster || (caster->state_flags & 0x10)) {
            fade();
        } else {
            caster->write_reference_point(spell.motion.position, 0, true);
            if (!(caster->state_flags & 0x20000000))
                return 1;
            spell.start_creature_projectile(match, static_cast<int>(spell.data->range * 180 / spell.data->speed),
                                            {}, 1, true);
            // 0x412960 / IcarusSpawnedRun: stock aica/abil release ten bodies, not one.
            for (int n = 1; n < std::clamp(spell.data->count, 1, 64); ++n) {
                auto velocity = spell.motion.velocity;
                spell_scatter(velocity, 2000, match.random());
                const float spread = spell.data->speed * .2f;
                velocity.x += (match.random().next() / 16384.f - 1) * spread;
                velocity.y += (match.random().next() / 16384.f - 1) * spread;
                velocity.z += (match.random().next() / 16384.f - 1) * spread;
                auto& child = SpellActor::launch_sacrifice_host(match, spell, SpellActor::Effect::icarus);
                child.owner = spell.owner;
                child.effect_target = spell.effect_target;
                child.data = spell.data;
                child.sprite = spell.sprite;
                child.start_return_projectile(match, spell.motion.position, 0);
                child.motion.velocity = velocity;
                child.generic.initialized = true;
                child.generic.collision_radius = .2f;
                child.remaining = spell.remaining;
                child.stage = 17;
            }
        }
    }
    spell.sprite.frame += ticks * .13333334f;
    if (spell.stage == 17) {
        spell.sprite.size *= .75f;
        spell.motion.flags = 5;
        spell.motion.collision_mask = 0x114015;
        spell.friendly_ignore_time = 0; // This host does not start the generic friendly-fire grace timer.
        spell.stage = 18;
    }
    if (spell.stage == 18) {
        if (spell.state_flags & 3) {
            spell.capture_collision(match);
            spell.effect_target = spell.impact_target;
            if (!living_unit(spell.effect_target)) {
                if (!spell.effect_target && spell.rate++ < spell.data->integer(cc("ihop"), 0)) {
                    // 0x42d160: seek the nearest living unit within 15, regardless of side;
                    // otherwise hop toward a random XY point. The vertical launch is fixed.
                    CreatureActor* nearest = nullptr;
                    double best = 15 * 15;
                    const auto visit = [&](MatchObject& object) {
                        auto* actor = living_unit(&object);
                        if (actor) {
                            const double d = distance_squared(spell.motion.position, actor->motion.position);
                            if (d < best) best = d, nearest = actor;
                        }
                        return 0;
                    };
                    match.visit_group(0, 5, visit);
                    match.visit_group(1, 5, visit);
                    Vec3 target;
                    if (nearest)
                        spell_target_point(target, nearest);
                    else {
                        target = spell.motion.position;
                        target.x += match.random().next() * (30.f / 32768) - 15;
                        target.y += match.random().next() * (30.f / 32768) - 15;
                    }
                    const float x = target.x - spell.motion.position.x, y = target.y - spell.motion.position.y,
                                distance = std::hypot(x, y);
                    spell.motion.velocity = {distance > 0 ? .075f * x / distance : 0,
                                             distance > 0 ? .075f * y / distance : 0, .075f};
                    spell.state_flags &= ~3u;
                    if ((match.random().next() & 15) <= 3)
                        spell.sound_event(match, 8);
                    return 1;
                }
                fade();
            } else {
                spell.motion.flags = spell.motion.collision_mask = 0;
                spell.motion.velocity = {};
                spell.remaining = static_cast<int>(spell.data->duration * 60);
                spell.stage = 81;
                if (auto attachment = living_unit(spell.effect_target)->closest_bone(spell.motion.position))
                    spell.bone_attachment = {attachment->bone, attachment->offset, true};
                spell.sound_event(match, 4);
                return 1;
            }
        } else if ((spell.remaining -= ticks) <= 0)
            fade();
    }
    if (spell.stage == 81) {
        auto* victim = living_unit(spell.effect_target);
        if (!victim || (spell.remaining -= ticks) <= 0) {
            fade();
        } else {
            const auto& attachment = spell.bone_attachment;
            if (!attachment.bound || !victim->write_bone_point(spell.motion.position, attachment.bone, nullptr,
                                                              &attachment.offset))
                spell_target_point(spell.motion.position, victim);
            const float speed = spell.data->real(FourCC::from_text("fspd"), 1),
                        regen = spell.data->real(FourCC::from_text("freg"), 1);
            for (const unsigned i : {3u, 4u, 6u}) {
                victim->stats.changed |= 1u << i;
                victim->stats.target[i] = static_cast<std::int16_t>(
                    std::clamp(victim->stats.target[i] * (i == 6 ? regen : speed), 0.f, 32767.f));
            }
            if (spell.data->amount && spell.data->duration > 0)
                victim->accumulate_damage({spell.data->damage_type,
                                           static_cast<int>(spell.data->amount / spell.data->duration),
                                           spell.reference()},
                                          ticks);
        }
    }
    if (spell.stage == 240) {
        spell.sprite.size = std::max(0.f, spell.sprite.size - ticks / 30.f);
        if (spell.sprite.size <= 0)
            return remove_spell(spell);
    }
    return 1;
}
} // namespace
int SpellActor::run_defense(Match& match, int ticks) {
    if (!data)
        throw Error("Creature defense ability requires its SPEL definition");
    switch (effect) {
    case Effect::life_shield:
        return run_life_shield(match, *this, ticks);
    case Effect::protector:
        return run_protector(match, *this);
    case Effect::run_away:
        return run_away(match, *this);
    case Effect::stealth:
        return run_stealth(match, *this, ticks);
    case Effect::rock_form:
        return run_rock_form(match, *this, ticks);
    case Effect::play_dead:
        return run_play_dead(match, *this, ticks);
    case Effect::icarus:
        return run_icarus(match, *this, ticks);
    default:
        throw Error("Invalid creature defense ability");
    }
}
} // namespace sacrifice
