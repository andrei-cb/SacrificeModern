#include "sacrifice/creature_actor.hpp"
#include "sacrifice/motion.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/soul_actor.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include "sacrifice/spell_data.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
FourCC cc(std::string_view text) {
    return FourCC::from_text(text);
}
FourCC para() {
    return FourCC{0x50415241u}; // PARA, 0x4692a0 from 0x4215b0.
}
int remove_spell(SpellActor& spell) {
    spell.state_flags |= MatchObject::removed_flag;
    return -2;
}
double distance_squared(Vec3 a, Vec3 b) {
    const double x = static_cast<double>(a.x) - b.x, y = static_cast<double>(a.y) - b.y,
                 z = static_cast<double>(a.z) - b.z;
    return x * x + y * y + z * z;
}
// 0x441d20: a wizard owner reports release 0x20000000 or abort; any other owner, or none (0x453600),
// aborts. Effects listed by casterless_skips_cast start past the wait in native, so they release.
unsigned control_cast(SpellActor& spell, unsigned released, unsigned aborted) {
    auto* caster = dynamic_cast<CreatureActor*>(spell.owner);
    if (!spell.owner)
        return SpellActor::casterless_skips_cast(spell.effect) ? released : aborted;
    if (!caster)
        return aborted;
    if (!(caster->kind_flags & 1))
        return aborted;
    const auto [state, alpha] = caster->cast_state();
    spell.cast_alpha = alpha;
    if (!state)
        return 0;
    return state == 0x20000000 ? released : aborted;
}
CreatureActor* living_unit(MatchObject* object) {
    auto* actor = dynamic_cast<CreatureActor*>(object);
    return actor && (actor->kind_flags & 5) && actor->alive() ? actor : nullptr;
}
void follow_target(SpellActor& spell) {
    spell_target_point(spell.motion.position, spell.effect_target);
    if (const auto* motion = spell.effect_target ? spell.effect_target->source_motion() : nullptr)
        spell.motion.orientation = motion->orientation;
}
void name_para(CreatureActor& actor, SpellActor& spell) {
    actor.name_reference(para(), &spell);
    if (spell.spell_id.value)
        actor.name_reference(spell.spell_id, &spell);
}
void drop_para(CreatureActor* actor, SpellActor& spell) {
    if (!actor)
        return;
    actor->unname(para(), &spell);
    if (spell.spell_id.value)
        actor->unname(spell.spell_id, &spell);
}
// 0x4215b0: seconds × 60, scaled by min(1, 1000 / base health) on creatures and 0.25 on wizards.
int control_duration(SpellActor& spell, float seconds, CreatureActor* target) {
    double factor = 1;
    if (target) {
        name_para(*target, spell);
        if (target->kind_flags & MatchObject::creature) {
            const auto health = target->stats.base[CreatureStats::health];
            factor = health > 0 ? 1000.0 / health : 1;
            if (factor >= 1)
                factor = 1;
        }
        if (target->kind_flags & MatchObject::wizard)
            factor = 0.25;
    }
    return static_cast<int>(static_cast<std::int64_t>(factor * seconds * 60.0));
}
void pin_unit(CreatureActor& actor, const Vec3& point) {
    actor.motion.position = point;
    actor.state_flags |= 0x80000;
}
void scale_stat(CreatureActor& actor, unsigned slot, double factor) {
    actor.stats.changed |= 1u << slot;
    const double value = static_cast<double>(actor.stats.target[slot]) * factor;
    actor.stats.target[slot] = static_cast<std::int16_t>(value <= 0 ? 0 : static_cast<std::int64_t>(value));
}
void add_stat(CreatureActor& actor, unsigned slot, int delta) {
    actor.stats.changed |= 1u << slot;
    const int value = static_cast<int>(actor.stats.target[slot]) + delta;
    actor.stats.target[slot] = static_cast<std::int16_t>(value <= 0 ? 0 : value > 32767 ? 32767 : value);
}
// 0x4321f0: movement slots 0/1/3 × 0.2, resistance slots 9..13 + 200, dirty bits, animation × 0.2.
SpellActor& spawn_control_child(Match& match, SpellActor& source, SpellActor::Effect effect,
                                MatchObject* target, Vec3 position) {
    auto& child = SpellActor::launch_sacrifice_host(match, source, effect);
    child.effect_target = target;
    child.owner = source.owner;
    child.data = source.data;
    child.motion.position = position;
    child.remaining = source.remaining;
    if (source.allegiance)
        source.allegiance->attach(child);
    child.run(match, 0);
    return child;
}
int run_vines_visual(Match& match, SpellActor& spell, int ticks) {
    // 0x42bd10: eight growing VINES rooted around the victim, not billboards.
    if (!spell.stage) {
        spell.surface.kind = SpellActor::CurvedSurface::vines;
        spell.surface.material = 0x00010001;
        spell.surface.textures[0] = FourCC{static_cast<unsigned>(
            spell.data ? spell.data->integer(cc("ttex"), cc("vine").value) : cc("vine").value)};
        auto* target = living_unit(spell.effect_target);
        if (!target)
            return remove_spell(spell);
        const auto& box = target->motion;
        Vec3 tip;
        spell_target_point(tip, target);
        const float radius =
            std::max(std::hypot(box.box_min.x, box.box_min.y), std::hypot(box.box_max.x, box.box_max.y));
        const float width = std::clamp(radius * .1f, .1f, .5f);
        for (unsigned n = 0; n < 8; ++n) {
            const float x = box.box_min.x +
                            (box.box_max.x - box.box_min.x) * match.visual_random.next() / 32768,
                        y = box.box_min.y +
                            (box.box_max.y - box.box_min.y) * match.visual_random.next() / 32768;
            Vec3 root{tip.x + x * 1.5f, tip.y + y * 1.5f, spell.motion.position.z};
            if (match.terrain)
                root.z = static_cast<float>(terrain_surface(*match.terrain, root.x, root.y));
            const Vec3 end{tip.x + x, tip.y + y,
                           tip.z + (tip.z - spell.motion.position.z) *
                                       (match.visual_random.next() * 2.25f / 32768 - .25f)};
            spell.add_vine(root, end, width);
        }
        spell.stage = 1;
    }
    spell.remaining -= ticks;
    const bool active = spell.remaining > 0 && living_unit(spell.effect_target);
    const bool visible = spell.advance_vines(ticks, active ? 1.f : 0.f);
    if (!active && !visible)
        return remove_spell(spell);
    return 1;
}
int run_freeze_ice(Match& match, SpellActor& spell, int ticks) {
    // 0x421700: type 36 is a CUBOID, not a particle emitter.
    auto* target = living_unit(spell.effect_target);
    if (!spell.stage) {
        spell.surface.kind = SpellActor::CurvedSurface::cuboid;
        spell.surface.textures[0] =
            FourCC{static_cast<unsigned>(spell.data->integer(cc("ttex"), cc("icec").value))};
        spell.surface.material = 0x0a011008;
        spell.stage = 1;
    }
    if (target && target->source_motion()) {
        follow_target(spell);
        const auto& box = *target->source_motion();
        if (spell.remaining > 0)
            spell.cast_alpha = std::min(1.f, spell.cast_alpha + ticks * .05f);
        spell.surface.parameters[0] = (box.box_max.x - box.box_min.x) * .8f * spell.cast_alpha;
        spell.surface.parameters[1] = (box.box_max.y - box.box_min.y) * .8f * spell.cast_alpha;
        spell.surface.parameters[2] = (box.box_max.z - box.box_min.z) * .8f * spell.cast_alpha;
    } else
        spell.remaining = 0;
    spell.remaining -= ticks;
    if (spell.remaining <= 0)
        spell.cast_alpha = std::max(0.f, spell.cast_alpha - ticks * .075f);
    spell.surface.opacity = spell.cast_alpha * spell.data->real(cc("falf"), 128) / 255;
    if (spell.remaining <= 0 && spell.cast_alpha == 0)
        return remove_spell(spell);
    return 1;
}
void apply_lethargy(CreatureActor& actor) {
    scale_stat(actor, 0, 0.2);
    scale_stat(actor, 1, 0.2);
    scale_stat(actor, 3, 0.2);
    for (unsigned i = 9; i <= 13; ++i)
        add_stat(actor, i, 200);
    actor.animation_multiplier *= 0.2f;
}
template <class Play> int run_vines(Match& match, SpellActor& spell, int ticks, Play play) {
    auto* target = living_unit(spell.effect_target);
    if (!spell.stage) {
        if (!target)
            return remove_spell(spell);
        play(2);
        // 0x42c2f0 stage 0: 0x436DF0(target, 'tdrt' default 'dirt', blended, box .5, size .75, 2 s), then
        // the 0x435EC0 cast hands.
        SpellActor::launch_dirt_spray(
            match, spell, target,
            FourCC{static_cast<unsigned>(spell.data->integer(cc("tdrt"), cc("dirt").value))}, true, .5f, .75f, 2);
        SpellActor::launch_cast_hands(match, spell);
        spell.stage = 2;
    }
    if (spell.stage == 2) {
        if (const auto next = control_cast(spell, 16, 240))
            spell.stage = static_cast<int>(next);
        if (spell.stage != 16)
            return spell.stage >= 240 ? remove_spell(spell) : 1;
    }
    if (spell.stage == 16) {
        if (!target)
            return remove_spell(spell);
        Vec3 at = target->motion.position;
        if (match.terrain) {
            const double height = terrain_surface(*match.terrain, at.x, at.y);
            if (height <= 0)
                return remove_spell(spell);
            at.z = static_cast<float>(height);
        }
        spell.point = {target->motion.position.x - at.x, target->motion.position.y - at.y,
                       target->motion.position.z - at.z};
        constexpr float lift_limit = 5.f; // Start 0x42c2f0 writes 5.0 at +64.
        if (spell.point.z > lift_limit) {
            spell.effect_target = nullptr;
            target = nullptr;
        }
        spell.motion.position = at;
        spell.remaining = target ? control_duration(spell, spell.data->duration, target) : 0;
        if (!target)
            return remove_spell(spell);
        spawn_control_child(match, spell, SpellActor::Effect::vines_visual, target, at);
        play(4); // 0x42bd10 after AddVine.
        spell.stage = 1;
        match.assign(spell, spell_function_id("VinesHoldRun"));
        return 0;
    }
    if (spell.stage == 1) {
        target = living_unit(spell.effect_target);
        if (target) {
            pin_unit(*target,
                     {spell.motion.position.x + spell.point.x, spell.motion.position.y + spell.point.y,
                      spell.motion.position.z + spell.point.z});
            if (match.terrain)
                spell.motion.position.z = static_cast<float>(
                    terrain_surface(*match.terrain, spell.motion.position.x, spell.motion.position.y));
        }
        spell.remaining -= ticks;
        if (spell.remaining <= 0 || !target)
            spell.stage = 240;
    }
    if (spell.stage >= 240) {
        drop_para(dynamic_cast<CreatureActor*>(spell.effect_target), spell);
        return remove_spell(spell);
    }
    return 1;
}
std::array<float, 6> chains_ring(Match& match, const CreatureActor& target) {
    const auto& box = target.motion;
    const float middle = (box.box_min.z + box.box_max.z) * .5f;
    const float height = middle + (box.box_max.z - middle) * match.visual_random.next() / 32768.f;
    return {box.box_min.x * 1.2f, box.box_min.y * 1.2f, height,
            box.box_max.x * 1.2f, box.box_max.y * 1.2f, height};
}
template <class Play> int run_chains(Match& match, SpellActor& spell, int ticks, Play play) {
    auto* target = living_unit(spell.effect_target);
    if (!spell.stage) {
        if (!target)
            return remove_spell(spell);
        SpellActor::launch_cast_hands(match, spell);
        control_duration(spell, spell.data->duration, target); // Names PARA; duration is then overwritten.
        spell.remaining = static_cast<int>(spell.data->duration * 60.f);
        spell.next_pulse = 32;
        auto& p = spell.particles(match);
        spell.particle_material(
            match, p, 0x1a012008,
            FourCC{static_cast<unsigned>(spell.data->integer(cc("ttex"), cc("firy").value))});
        const auto bounds = chains_ring(match, *target);
        const float scale = 2;
        p.set_property(139, &scale, bounds.data());
        p.set_flags(0x80002, 0x400000);
        const float diagonal =
            std::hypot(bounds[3] - bounds[0], bounds[4] - bounds[1], bounds[5] - bounds[2]);
        const auto glow =
            FourCC{static_cast<unsigned>(spell.data->integer(cc("tcst"), cc("firy").value))}.text();
        SpellActor::launch_glow(match, spell, 0x1a012008, glow.c_str(), diagonal * .75f, 0,
                                std::clamp(diagonal * .15f, .25f, 2.f), 1, target, 1);
        spell.stage = 1; // 0x4258A0 falls through into the cast wait.
    }
    if (spell.stage == 1) {
        if (const auto next = control_cast(spell, 16, 240))
            spell.stage = static_cast<int>(next);
    }
    if (target && spell.stage < 240) {
        spell.motion.position = target->motion.position;
        spell.motion.orientation = target->motion.orientation;
        spell.particles(match).set_position({});
    }
    if (spell.stage == 16) {
        // 0x4258A0 stage 16: the (1,.5,0) light and sound 2 come on when the cast is released.
        spell.light = {true, spell.motion.position, {1, .5f, 0}, 0, 1, 25};
        play(2);
        spell.stage = 17;
    }
    if (spell.stage == 17) {
        target = living_unit(spell.effect_target);
        if (target) {
            target->accumulate_damage({spell.data->damage_type, spell.data->amount, spell.reference()},
                                      ticks);
            spell.next_pulse += ticks;
            if (spell.next_pulse >= 32) {
                auto& p = spell.particles(match);
                const auto bounds = chains_ring(match, *target);
                p.set_property(2, bounds.data(), bounds.data() + 3);
                float angles[2]{};
                for (int n = 0; n < 32; ++n) {
                    p.emit(0, 1, angles);
                    angles[0] += .19634938f;
                }
                spell.next_pulse = 0;
            }
        }
        spell.remaining -= ticks;
        if (spell.remaining <= 0 || !target)
            spell.stage = 240;
    }
    if (spell.stage == 240) {
        drop_para(dynamic_cast<CreatureActor*>(spell.effect_target), spell);
        spell.stop_sound_event(2);
        spell.stage = 241;
    }
    const float goal = spell.stage >= 240 ? 0.f : spell.stage == 1 ? spell.cast_alpha : 1.f;
    spell.age += std::clamp(goal - spell.age, -ticks / 30.f, ticks / 30.f);
    spell.light.position = spell.motion.position;
    spell.light.intensity = spell.age; // 0x4685C0 light, vfunc 56.
    spell.host.time += ticks;
    auto& p = spell.particles(match);
    p.advance(spell.host.time, 0);
    if (spell.stage >= 240 && spell.age == 0 && !p.alive())
        return remove_spell(spell);
    return 1;
}
template <class Play> int run_freeze(Match& match, SpellActor& spell, int ticks, Play play) {
    CreatureActor* target = nullptr;
    if (!spell.stage) {
        // FreezeSpellStart 0x421B00: a 'tcst' glow on the caster (its box radius, speed 2) and, at twice its
        // radius, on the target (0x4367C0 scale .5, life 1).
        const auto radius = [](const MatchObject* object) {
            const auto* motion = object ? object->source_motion() : nullptr;
            if (!motion)
                return 0.f;
            const float dx = motion->box_max.x - motion->box_min.x, dy = motion->box_max.y - motion->box_min.y;
            return std::sqrt(dx * dx + dy * dy) * .5f;
        };
        if (const auto texture = spell.data->integer(cc("tcst"), 0)) {
            const auto name = FourCC{static_cast<unsigned>(texture)}.text();
            SpellActor::launch_glow(match, spell, 0x12010808, name.c_str(), radius(spell.owner), 2, .5f, 1,
                                    spell.owner, 1);
            SpellActor::launch_cast_hands(match, spell);
            if (spell.effect_target)
                SpellActor::launch_glow(match, spell, 0x12010808, name.c_str(), radius(spell.effect_target) * 2,
                                        0, .5f, 1, spell.effect_target, 1);
        } else
            SpellActor::launch_cast_hands(match, spell);
        play(4);
        spell.stage = 1;
    }
    if (spell.stage == 1) {
        if (const auto next = control_cast(spell, 16, 240))
            spell.stage = static_cast<int>(next);
        if (spell.stage == 1)
            return 1;
    }
    if (spell.stage == 16) {
        target = living_unit(spell.effect_target);
        if (!target)
            return remove_spell(spell);
        spell.remaining = control_duration(spell, spell.data->duration, target);
        spell.age = 0;
        spell.emission = 1;
        spawn_control_child(match, spell, SpellActor::Effect::freeze_ice, target, target->motion.position);
        match.assign(spell, spell_function_id("FreezeSpellRun"));
        spell.stage = 2;
        return 0;
    }
    if (spell.stage == 2) {
        target = living_unit(spell.effect_target);
        if (target) {
            follow_target(spell);
            pin_unit(*target, target->motion.position);
            spell.age = std::min(1.f, spell.age + ticks * .05f);
        }
        spell.remaining -= ticks;
        if (spell.remaining <= 0 || !target)
            spell.stage = 32;
    }
    if (spell.stage == 32) {
        // FreezeSpellRun 0x4217C0 stage 32: 0x44D7E0 damage, then the ice shatters at once into 'tsha'
        // ('shrd') bits, 'inum' (48) of them at speed 3 from the ice box (0x439440 flags 1, sound 16).
        target = living_unit(spell.effect_target);
        if (target)
            spell_damage(match, &spell, target, spell.data->damage_type, spell.data->amount,
                         spell.data->radius);
        if (const auto* box = spell.effect_target ? spell.effect_target->source_motion() : nullptr) {
            spell.motion.position = box->position;
            const Vec3 low{box->box_min.x * .8f, box->box_min.y * .8f, box->box_min.z * .8f},
                       high{box->box_max.x * .8f, box->box_max.y * .8f, box->box_max.z * .8f};
            SpellActor::launch_fragment_burst(
                match, spell, FourCC{static_cast<unsigned>(spell.data->integer(cc("tsha"), cc("shrd").value))},
                spell.data->integer(cc("inum"), 48), 3, 1, &low, &high);
        }
        for (const auto& object : match.objects())
            if (auto* ice = dynamic_cast<SpellActor*>(object.get());
                ice && ice->effect == SpellActor::Effect::freeze_ice && ice->effect_target == spell.effect_target &&
                ice->owner == spell.owner && !(ice->state_flags & MatchObject::removed_flag))
                remove_spell(*ice);
        drop_para(dynamic_cast<CreatureActor*>(spell.effect_target), spell);
        play(16);
        return remove_spell(spell);
    }
    if (spell.stage >= 240)
        return remove_spell(spell);
    return 1;
}
template <class Play> int run_lethargy(Match& match, SpellActor& spell, int ticks, Play play) {
    // LethargyRun 0x432550.
    auto* target = living_unit(spell.effect_target);
    auto& cloud = spell.altar_cloud;
    const auto flash = [&] { // 0x438FF0(spell, target, position, 1).
        auto& hit = SpellActor::launch_sacrifice_host(match, spell, SpellActor::Effect::spell_hit);
        hit.effect_target = spell.effect_target;
        hit.motion.position = spell.motion.position;
        hit.run(match, 0);
    };
    if (!spell.stage) {
        if (!target)
            return remove_spell(spell);
        // 0x432400: sound 2, cast hands, a 'tcst' glow on the caster (0x4367C0 1, 2, .25, 1) and the 'ttex'
        // cloud .15 x the target's box diagonal across, starting 15 units up; then sound 1.
        play(2);
        SpellActor::launch_cast_hands(match, spell);
        const auto glow = FourCC{static_cast<unsigned>(spell.data->integer(cc("tcst"), cc("cst0").value))}.text();
        SpellActor::launch_glow(match, spell, 0x1a010808, glow.c_str(), 1, 2, .25f, 1, spell.owner, 1);
        cloud.enabled = true;
        cloud.material = 0x10000;
        cloud.texture = FourCC{static_cast<unsigned>(spell.data->integer(cc("ttex"), cc("lthg").value))};
        cloud.noise = std::make_shared<CloudNoise>(match.visual_random);
        const auto& low = target->motion.box_min;
        const auto& high = target->motion.box_max;
        const float x = high.x - low.x, y = high.y - low.y, z = high.z - low.z;
        spell.scale_target = std::sqrt(x * x + y * y + z * z) * .15f;
        spell.age = 1;
        play(1);
        spell.stage = 1;
    }
    if (spell.stage == 1) {
        if (const auto next = control_cast(spell, 16, 240)) {
            spell.stage = static_cast<int>(next);
            if (spell.stage == 16) {
                play(2);
                spell.cast_alpha = 1;
            }
        }
    }
    if (spell.stage == 16) {
        // Stage 17: the cloud sinks onto the target over half a second.
        target = living_unit(spell.effect_target);
        if (!target)
            spell.stage = 240;
        else {
            spell.age = std::max(0.f, spell.age - ticks / 30.f);
            if (spell.age == 0)
                spell.stage = 32;
        }
    } else if (spell.stage == 32) {
        if (!target)
            spell.stage = 240;
        else {
            // The cloud model goes (0x4691F0); a hit flash, the 'ttex' skin and the 0x43ABD0 'tgoo' drip
            // emitter (preset 133 at .25, .1 drops a tick from random body points) take over.
            play(4);
            flash();
            spell.remaining = control_duration(spell, spell.data->duration, target);
            target->texture_override = cloud.texture;
            cloud.enabled = false;
            auto& goo = spell.particles(match);
            spell.particle_material(
                match, goo, 0x0a011008,
                FourCC{static_cast<unsigned>(spell.data->integer(cc("tgoo"), cc("lth2").value))});
            const float preset[3]{.25f, 0, 0};
            goo.set_property(133, preset, preset);
            spell.stage = 33;
        }
    }
    if (spell.stage == 33) {
        target = living_unit(spell.effect_target);
        if (target) {
            follow_target(spell);
            apply_lethargy(*target);
            auto& goo = spell.particle_systems.front();
            for (float credit = ticks * .1f * 32768; credit > 0 && target->bone_count(); credit -= 32768) {
                if (credit < 32768 && match.visual_random.next() >= credit)
                    continue;
                Vec3 at;
                if (target->write_bone_point(at, match.visual_random.next() % target->bone_count(),
                                             &match.visual_random)) {
                    goo.set_position(at, &at);
                    goo.emit(spell.host.time, 1);
                }
            }
        }
        spell.remaining -= ticks;
        if (spell.remaining <= 0 || !target)
            spell.stage = 48;
    }
    if (spell.stage == 48) {
        play(16);
        if (auto* actor = dynamic_cast<CreatureActor*>(spell.effect_target);
            actor && actor->texture_override == cloud.texture)
            actor->texture_override = {};
        flash();
        drop_para(dynamic_cast<CreatureActor*>(spell.effect_target), spell);
        spell.effect_target = nullptr;
        spell.stage = 49; // The drip emitter stops but its drops fall out.
    }
    if (spell.stage == 240) {
        // An aborted cast lets go of the target and shrinks the cloud away over a second.
        drop_para(dynamic_cast<CreatureActor*>(spell.effect_target), spell);
        spell.effect_target = nullptr;
        target = nullptr;
        spell.stage = 241;
    }
    if (spell.stage == 241) {
        spell.cast_alpha -= ticks / 60.f;
        if (spell.cast_alpha <= 0)
            return remove_spell(spell);
    }
    if (cloud.enabled) {
        if (target) {
            follow_target(spell);
            spell.motion.position.z += spell.age * 15;
        }
        cloud.radii = {spell.scale_target * spell.cast_alpha, spell.scale_target * spell.cast_alpha,
                       spell.scale_target * spell.cast_alpha};
        cloud.opacity = 1;
        cloud.phase.x += ticks * 8;
        cloud.phase.y += ticks * 8;
        cloud.phase.z += ticks * 8;
    }
    spell.host.time += ticks;
    for (auto& goo : spell.particle_systems)
        goo.advance(spell.host.time, 0);
    if (spell.stage == 49 && (spell.particle_systems.empty() || !spell.particle_systems.front().alive()))
        return remove_spell(spell);
    return 1;
}
template <class Play>
int run_decompose(Match& match, SpellActor& spell, int ticks, Play play, bool wind = false) {
    auto* soul = dynamic_cast<SoulActor*>(spell.effect_target);
    if (!spell.stage) {
        if (!soul || !spell.owner || !spell.owner->source_motion())
            return remove_spell(spell);
        SpellActor::launch_cast_hands(match, spell);
        spell.ability.origin = spell.owner->source_motion()->position;
        spell_target_point(spell.point, soul);
        const Vec3 delta{spell.point.x - spell.ability.origin.x, spell.point.y - spell.ability.origin.y,
                         spell.point.z - spell.ability.origin.z};
        spell.motion.orientation = spell_orientation(delta);
        spell.ability.length = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z) * .25f;
        if (!wind)
            spell.set_projectile_model(
                match, FourCC{static_cast<unsigned>(spell.data->integer(cc("tmod"), cc("rock").value))}, 1);
        if (!wind) {
            SpellActor::launch_ground_trail(
                match, spell, 0x0a011008,
                FourCC{static_cast<unsigned>(spell.data->integer(cc("ttrl"), cc("dust").value))},
                {-.75f, -.75f, 0}, {.75f, .75f, .25f}, .75f, 2);
            SpellActor::launch_generic_trail(
                match, spell, 0x0a011008,
                FourCC{static_cast<unsigned>(spell.data->integer(cc("tbit"), cc("rock").value))}, .15f, .25f,
                1, .15f, .78539753f);
        }
        spell.age = 0;
        spell.next_pulse = 0;
        play(1);
        spell.stage = 1;
    }
    // DecomposeStart 0x444060: the mole already travels during the cast, at the cast alpha's speed.
    if (spell.stage == 1) {
        if (const auto next = control_cast(spell, 16, 240))
            spell.stage = static_cast<int>(next);
        if (spell.stage == 1 && wind)
            return 1;
    }
    if (!soul || !spell.owner || !spell.owner->source_motion())
        spell.stage = 240;
    if (spell.stage == 1 || spell.stage == 16 || spell.stage == 32 || spell.stage == 48) {
        if (spell.stage == 16)
            spell_target_point(spell.point, soul);
        const float speed = spell.stage == 1 ? spell.cast_alpha : 1;
        spell.age = std::min(spell.stage == 48 ? 2.f : 1.f, spell.age + ticks * speed / 240.f);
        const bool returning = spell.age > 1;
        const float t = returning ? spell.age - 1 : spell.age, u = 1 - t;
        const Vec3 from = returning ? spell.point : spell.ability.origin,
                   to = returning ? spell.owner->source_motion()->position : spell.point;
        const auto& o = spell.motion.orientation;
        // Out: origin, 40% toward the soul, the soul pushed along the heading, the soul. Back (0x444060's
        // second basis): the soul, pulled back along the heading, 40% from the caster, the caster.
        const float offset = spell.ability.length * (returning ? -1 : 1);
        const Vec3 near{from.x + (to.x - from.x) * .4f, from.y + (to.y - from.y) * .4f,
                        from.z + (to.z - from.z) * .4f},
            far{to.x + (from.x - to.x) * .4f, to.y + (from.y - to.y) * .4f, to.z + (from.z - to.z) * .4f},
            heading{spell.point.x + o[0] * offset, spell.point.y + o[1] * offset, spell.point.z + o[2] * offset};
        const Vec3 a = returning ? heading : near, b = returning ? far : heading;
        const auto blend = [&](auto member) {
            return u * u * u * (from.*member) + 3 * u * u * t * (a.*member) + 3 * u * t * t * (b.*member) +
                   t * t * t * (to.*member);
        };
        spell.motion.position = {blend(&Vec3::x), blend(&Vec3::y), blend(&Vec3::z)};
        if (match.terrain)
            spell.motion.position.z = static_cast<float>(
                terrain_surface(*match.terrain, spell.motion.position.x, spell.motion.position.y));
        if (spell.stage == 16 && spell.age >= 1)
            spell.stage = 32;
        // Stage 32 waits while the soul is flagged (0xC0000000), then takes it from its corpse.
        if (spell.stage == 32 && !(soul->soul_flags & 0xc0000000)) {
            play(4);
            if (auto* corpse = dynamic_cast<CreatureActor*>(soul->anchor)) {
                SpellActor::launch_corpse_fade(match, *corpse);
                corpse->state_flags |= MatchObject::removed_flag;
            }
            soul->anchor = nullptr;
            spell.stage = 48;
        }
        if (spell.stage == 48) {
            soul->motion.position = spell.motion.position;
            soul->motion.position.z += wind ? 1 : 2;
            if (spell.age >= 2)
                spell.stage = 240;
        }
        const float limit = spell.data->spread_limit * spell.data->spread_limit;
        const bool pulse = wind && (spell.next_pulse -= ticks) <= 0 && spell.age < 1.95f;
        if (pulse)
            spell.next_pulse = 32;
        // 0x443FD0 knocks down units near the mole from release on, except the caster.
        for (unsigned group = 0; spell.stage != 1 && group < 2; ++group)
            match.visit_group(group, 5, [&](MatchObject& object) {
                auto* actor = dynamic_cast<CreatureActor*>(&object);
                if (!actor || actor == spell.owner ||
                    distance_squared(actor->motion.position, spell.motion.position) > limit)
                    return 0;
                if (wind && pulse && actor->alive() && actor->enemy(&spell))
                    spell_damage(match, &spell, actor, spell.data->damage_type, spell.data->amount,
                                 spell.data->radius);
                else if (!wind && (actor->state_flags & 2))
                    actor->knock_down(0, 10);
                return 0;
            });
    }
    if (spell.stage >= 240) {
        spell.stop_sound_event(1);
        spell.effect_target = nullptr;
        return remove_spell(spell);
    }
    return 1;
}
} // namespace
SpellActor* SpellActor::launch_dirt_spray(Match& match, SpellActor& source, MatchObject* target, FourCC texture,
                                          bool blend, float box, float size, float seconds) {
    // 0x436DF0: a4 picks 0x0A011008 over additive 0x1A012008; a8 = 0 waits on the owner's cast.
    auto& child = launch_sacrifice_host(match, source, Effect::dirt_spray);
    child.owner = source.owner;
    child.effect_target = target;
    child.data = source.data;
    child.motion.position = source.motion.position;
    child.remaining = static_cast<int>(seconds * 60);
    auto& system = child.particles(match);
    child.particle_material(match, system, blend ? 0x0a011008u : 0x1a012008u, texture);
    const float life = 1.5f;
    system.set_property(129, &size, &life);
    const float low[3]{-box, -box, 0}, high[3]{box, box, 0};
    system.set_property(2, low, high);
    system.set_flags(0x1000, 0x400000);
    child.emission = 1;
    child.stage = 1;
    return &child;
}
int SpellActor::run_dirt_spray(Match& match, int ticks) {
    // 0x436B40: sit on the ground under the target, emitting only where there is land; 0x441D20 sends a
    // release to the timed tail (240) and an abort straight to the drain (241).
    if (const auto* motion = effect_target ? effect_target->source_motion() : nullptr)
        this->motion.position = motion->position;
    float ground = this->motion.position.z;
    if (match.terrain)
        ground = static_cast<float>(terrain_surface(*match.terrain, this->motion.position.x, this->motion.position.y));
    this->motion.position.z = ground;
    emission = ground > 0 ? 1.f : 0.f;
    if (stage == 1) {
        if (const auto next = control_cast(*this, 240, 241))
            stage = static_cast<int>(next);
    } else if (stage == 240 && (remaining -= ticks) <= 0)
        stage = 241;
    auto& system = particles(match);
    if (stage == 241) {
        emission = 0;
        if (!system.alive())
            return remove_spell(*this);
    }
    system.set_position(this->motion.position);
    host.time += ticks;
    system.advance(host.time, emission);
    return 1;
}
int SpellActor::run_control(Match& match, int ticks) {
    if (!data)
        throw Error("God control spell requires its SPEL definition");
    const auto play = [&](unsigned mask) { sound_event(match, mask); };
    switch (effect) {
    case Effect::grasping_vines:
        return run_vines(match, *this, ticks, play);
    case Effect::chains_of_fire:
        return run_chains(match, *this, ticks, play);
    case Effect::freeze:
        return run_freeze(match, *this, ticks, play);
    case Effect::lethargy:
        return run_lethargy(match, *this, ticks, play);
    case Effect::decompose:
        return run_decompose(match, *this, ticks, play);
    case Effect::vines_visual:
        return run_vines_visual(match, *this, ticks);
    case Effect::freeze_ice:
        return run_freeze_ice(match, *this, ticks);
    default:
        throw Error("Invalid god control spell");
    }
}
} // namespace sacrifice
