#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
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
FourCC prot() {
    return cc("PROT");
}
int remove_spell(SpellActor& spell) {
    spell.state_flags |= MatchObject::removed_flag;
    return -2;
}
// 0x441d20: a wizard owner reports release 0x20000000 or abort; any other owner, or none (0x453600),
// aborts. Effects listed by casterless_skips_cast start past the wait in native, so they release.
unsigned form_cast(SpellActor& spell, unsigned released, unsigned aborted) {
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
CreatureActor* form_target(SpellActor& spell) {
    if (auto* actor = dynamic_cast<CreatureActor*>(spell.effect_target))
        return actor;
    if (auto* actor = dynamic_cast<CreatureActor*>(spell.owner)) {
        spell.effect_target = actor;
        return actor;
    }
    return nullptr;
}
void follow_target(SpellActor& spell) {
    spell_target_point(spell.motion.position, spell.effect_target);
    if (const auto* motion = spell.effect_target ? spell.effect_target->source_motion() : nullptr)
        spell.motion.orientation = motion->orientation;
}
void ease(float& value, float target, float step) {
    if (value < target)
        value = std::min(target, value + step);
    else if (value > target)
        value = std::max(target, value - step);
}
// 0x458830: write resistance slots 9..13 and mark them dirty so 0x45a670 eases toward the new target.
void scale_resistances(CreatureActor& actor, float factor, bool current) {
    auto& stats = actor.stats;
    for (unsigned i = 9; i <= 13; ++i) {
        stats.changed |= 1u << i;
        auto& slot = current ? stats.current[i] : stats.target[i];
        const double value = static_cast<double>(slot) * factor;
        slot = static_cast<std::int16_t>(value <= 0 ? 0 : static_cast<std::int64_t>(value));
    }
}
void name_form(CreatureActor& actor, SpellActor& spell) {
    if (auto* previous = actor.named(prot()))
        actor.unname(prot(), previous);
    actor.name_reference(prot(), &spell);
}
void drop_form(Match& match, CreatureActor* actor, SpellActor& spell) {
    if (!actor)
        return;
    if (spell.effect == SpellActor::Effect::skin_of_stone && actor->named(prot()) == &spell)
        actor->grayscale = false;
    actor->unname(prot(), &spell);
    match.unwatch(*actor, 0, spell.reference(), &spell);
    match.unwatch(*actor, 2, spell.reference(), &spell);
}
void watch_damage(Match& match, CreatureActor& actor, SpellActor& spell, std::uintptr_t handler) {
    match.watch(actor, 0, {&spell, spell.reference(), handler});
}
void fireform_burn(Match& match, SpellActor& spell, int ticks) {
    auto* source = form_target(spell);
    if (!source)
        return;
    const float spread = spell.data->spread_limit;
    match.visit_group(0, 5, [&](MatchObject& object) {
        auto* actor = dynamic_cast<CreatureActor*>(&object);
        if (!actor || actor == source || !actor->alive() || !actor->enemy(source))
            return 0;
        if (actor->motion.collision_boxes.empty() || source->motion.collision_boxes.empty())
            return 0;
        if (spell_box_separation(actor->motion, source->motion) >= spread)
            return 0;
        actor->accumulate_damage({spell.data->damage_type, spell.data->amount, source->reference()}, ticks);
        actor->burn_near(source->motion.position, 10.f, 60);
        return 0;
    });
    match.visit_group(1, 5, [&](MatchObject& object) {
        auto* actor = dynamic_cast<CreatureActor*>(&object);
        if (!actor || actor == source || !actor->alive() || !actor->enemy(source))
            return 0;
        if (actor->motion.collision_boxes.empty() || source->motion.collision_boxes.empty())
            return 0;
        if (spell_box_separation(actor->motion, source->motion) >= spread)
            return 0;
        actor->accumulate_damage({spell.data->damage_type, spell.data->amount, source->reference()}, ticks);
        actor->burn_near(source->motion.position, 10.f, 60);
        return 0;
    });
}
int swarm_sting(Match& match, SpellActor& spell) {
    auto* source = form_target(spell);
    if (!source)
        return 0;
    int hits = 0;
    const float spread = spell.data->spread_limit;
    const float limit = spread * spread;
    const auto sting = [&](MatchObject& object) {
        auto* actor = dynamic_cast<CreatureActor*>(&object);
        if (!actor || actor == source || !actor->enemy(source))
            return 0;
        const auto& at = actor->motion.position;
        const double dx = static_cast<double>(at.x) - spell.motion.position.x,
                     dy = static_cast<double>(at.y) - spell.motion.position.y,
                     dz = static_cast<double>(at.z) - spell.motion.position.z;
        if (dx * dx + dy * dy + dz * dz > limit)
            return 0;
        ++hits;
        spell_damage(match, &spell, actor, spell.data->damage_type, spell.data->amount, spell.data->radius);
        return 0;
    };
    match.visit_group(0, 5, sting);
    match.visit_group(1, 5, sting);
    return hits;
}
SpellActor& spawn_form_child(Match& match, SpellActor& source, SpellActor::Effect effect,
                             MatchObject* target) {
    auto& child = SpellActor::launch_sacrifice_host(match, source, effect);
    child.effect_target = target;
    child.owner = source.owner;
    child.data = source.data;
    if (effect == SpellActor::Effect::spinny_particles)
        child.impact_target = &source;
    if (source.allegiance)
        source.allegiance->attach(child);
    child.run(match, 0);
    return child;
}
void skin_fragments(Match& match, SpellActor& spell, CreatureActor& target, int count) {
    auto& p = spell.particles(match);
    const auto& body = target.motion;
    p.set_frame({body.orientation, body.position});
    int emitted = 0;
    for (int n = 0; n < count; ++n) {
        Vec3 at;
        const unsigned bones = target.bone_count();
        if (!bones ||
            !target.write_bone_point(at, match.visual_random.next() % bones, &match.visual_random)) {
            const auto sample = [&](float low, float high) {
                return low + (high - low) * match.visual_random.next() / 32768.f;
            };
            const Vec3 local{sample(body.box_min.x, body.box_max.x), sample(body.box_min.y, body.box_max.y),
                             sample(body.box_min.z, body.box_max.z)};
            const auto& o = body.orientation;
            at = {body.position.x + local.x * o[0] + local.y * o[3] + local.z * o[6],
                  body.position.y + local.x * o[1] + local.y * o[4] + local.z * o[7],
                  body.position.z + local.x * o[2] + local.y * o[5] + local.z * o[8]};
        }
        p.set_position(at, &at);
        if (!p.emit(0, 1))
            break;
        ++emitted;
    }
    if (emitted)
        spell.sound_event(match, 4);
}
int run_skin_casting(Match& match, SpellActor& spell, int ticks) {
    // SkinOfStoneCasting 0x446a90: a narrowing ring rises from the feet through the wizard.
    auto& p = spell.particles(match);
    auto* target = form_target(spell);
    if (!spell.stage) {
        spell.particle_material(
            match, p, 0x0a011008,
            FourCC{static_cast<unsigned>(spell.data ? spell.data->integer(cc("ttex"), cc("dirt").value)
                                                    : cc("dirt").value)});
        const float half = .5f, life = 1.f;
        p.set_property(132, &half, &life);
        p.set_flags(0x400, 0x400000); // Native horizontal ring, rather than a spherical box emitter.
        spell.remaining = 60;
        spell.emission = 2;
        spell.stage = 1;
    }
    if (spell.stage == 1) {
        if (const auto next = form_cast(spell, 16, 32))
            spell.stage = static_cast<int>(next);
    }
    ease(spell.age, spell.cast_alpha, ticks / 60.f);
    if (spell.stage == 16 && spell.age == spell.cast_alpha)
        spell.stage = 32;
    if (spell.stage == 32)
        spell.emission = 0;
    if (target && target->source_motion()) {
        const auto& box = *target->source_motion();
        spell.motion.position = box.position;
        spell.motion.orientation = box.orientation;
        const float progress = spell.age * spell.age, grow = 1.f - progress;
        const float half =
            std::sqrt(std::max(0.f, (box.box_max.x - box.box_min.x) * (box.box_max.x - box.box_min.x) +
                                        (box.box_max.y - box.box_min.y) * (box.box_max.y - box.box_min.y))) *
                .35355f * .1f +
            grow + grow;
        const float height = box.box_max.z * progress;
        const float low[3]{-half, -half, height}, high[3]{half, half, height};
        p.set_property(2, low, high);
    }
    p.set_position({}); // Preset 132 is drawn in the followed host's frame.
    spell.host.time += ticks;
    p.advance(spell.host.time, spell.emission);
    if (spell.stage == 32 && !p.alive())
        return remove_spell(spell);
    return 1;
}
int run_spinny(Match& match, SpellActor& spell, int ticks) {
    // SpawnSpinnyParticles 0x437090 / INERT_RunSpinnyParticles 0x437120.
    auto& p = spell.particles(match);
    auto* target = form_target(spell);
    if (!spell.stage) {
        spell.particle_material(match, p, 0x1a012008, "pash");
        const float half = .5f, life = 1.f;
        p.set_property(132, &half, &life);
        spell.emission = 1;
        spell.stage = 1;
    }
    auto* parent = dynamic_cast<SpellActor*>(spell.impact_target);
    if (!target || !target->alive() || !parent || parent->stage >= 240 ||
        (parent->state_flags & MatchObject::removed_flag)) {
        spell.emission = 0;
        spell.stage = 240;
    } else if (target->source_motion()) {
        const auto& box = *target->source_motion();
        spell.motion.position = box.position;
        const float radius =
            std::sqrt(std::max(0.f, (box.box_max.x - box.box_min.x) * (box.box_max.x - box.box_min.x) +
                                        (box.box_max.y - box.box_min.y) * (box.box_max.y - box.box_min.y))) *
            .35355f * .4f;
        const float low[3]{-radius, -radius, box.box_min.z}, high[3]{radius, radius, box.box_max.z};
        p.set_property(2, low, high);
    }
    spell.age += ticks * .1570795f;
    spell.motion.orientation = spell_orientation({std::sin(spell.age), std::cos(spell.age), 0});
    p.set_position({}); // Orbit offsets are local; Draw applies the host transform.
    spell.host.time += ticks;
    p.advance(spell.host.time, spell.emission);
    if (spell.stage == 240 && !p.alive())
        return remove_spell(spell);
    return 1;
}
} // namespace
int SpellActor::run_forms(Match& match, int ticks) {
    if (!data)
        throw Error("God form requires its SPEL definition");
    auto* target = form_target(*this);
    switch (effect) {
    case Effect::ethereal_form: {
        auto& p = particles(match);
        if (!stage) {
            if (!target)
                return remove_spell(*this);
            launch_cast_hands(match, *this);
            particle_material(match, p, 0x1a012008,
                              FourCC{static_cast<unsigned>(data->integer(cc("ttex"), cc("glo2").value))});
            const float scale = .35f, life = 1.f;
            p.set_property(129, &scale, &life);
            remaining = static_cast<int>(data->duration * 60.f);
            light = {true, motion.position, {1, 1, 0}, 0, 1, 50}; // 0x42b5b0: 0x44ca20 with unk_4CB8EC.
            age = 0;
            emission = 0;
            stage = 1;
        }
        if (stage == 1) {
            if (const auto next = form_cast(*this, 2, 240))
                stage = static_cast<int>(next);
        }
        if (stage == 2) {
            if (!target)
                return remove_spell(*this);
            sound_event(match, 2);
            name_form(*target, *this);
            auto& burst = launch_sacrifice_host(match, *this, Effect::spell_hit);
            burst.effect_target = target;
            burst.run(match, 0);
            emission = 1;
            target->fade = .74f;
            scale_resistances(*target, 0, true);
            stage = 3;
        }
        if (stage == 3) {
            if (!target || !target->alive())
                stage = 240;
            else {
                scale_resistances(*target, 0, false);
                target->fade_target = .75f;
                target->fade_step = 1.f / 60.f;
                remaining -= ticks;
                if (remaining <= 0)
                    stage = 240;
            }
        }
        if (stage == 240) {
            sound_event(match, 16);
            emission = 0;
            drop_form(match, target, *this);
            stage = 241;
        }
        if (target && target->alive())
            follow_target(*this);
        else if (stage < 240)
            stage = 240;
        ease(age, stage >= 240 ? 0.f : 1.f, ticks / 60.f);
        light.position = motion.position;
        light.intensity = age;
        if (const auto* body = target && target->source_motion() ? target->source_motion() : nullptr) {
            const float low[3]{body->box_min.x, body->box_min.y, body->box_min.z},
                high[3]{body->box_max.x, body->box_max.y, body->box_max.z};
            p.set_property(2, low, high);
        }
        p.set_position(motion.position);
        host.time += ticks;
        p.advance(host.time, emission);
        if (stage == 241 && age <= 0 && !p.alive())
            return remove_spell(*this);
        return 1;
    }
    case Effect::fireform: {
        if (auto* caster = dynamic_cast<CreatureActor*>(owner)) {
            target = caster;
            effect_target = caster;
        }
        auto& p = particles(match);
        if (!stage) {
            if (!target)
                return remove_spell(*this);
            effect_target = target;
            launch_cast_hands(match, *this);
            particle_material(match, p, 0x1a012008,
                              FourCC{static_cast<unsigned>(data->integer(cc("ttex"), cc("firy").value))});
            const float scale = 2.f;
            const auto& body = target->motion;
            const float x = (body.box_max.x - body.box_min.x) * .55f,
                        y = (body.box_max.y - body.box_min.y) * .55f,
                        z = (body.box_max.z - body.box_min.z) * .55f;
            const float box[]{-x, -y, -z, x, y, z};
            p.set_property(139, &scale, box);
            name_form(*target, *this);
            sound_event(match, 2);
            remaining = static_cast<int>(data->duration * 60.f);
            light = {true, motion.position, {1.f, .5f, 0.f}, 0, 1, 25}; // 0x427470: dword_4CB8E0.
            age = 0;
            emission = 0;
            stage = 1;
        }
        if (stage == 1) {
            if (const auto next = form_cast(*this, 16, 240))
                stage = static_cast<int>(next);
            emission = cast_alpha;
        }
        if (stage == 16) {
            emission = 1;
            stage = 17;
        }
        if (stage == 17) {
            if (!target || !target->alive())
                stage = 240;
            else {
                fireform_burn(match, *this, ticks);
                remaining -= ticks;
                if (remaining <= 0)
                    stage = 240;
            }
        }
        if (stage == 240) {
            drop_form(match, target, *this);
            emission = 0;
        }
        if (target && target->alive())
            follow_target(*this);
        else if (stage < 240)
            stage = 240;
        ease(age, emission + emission, .06666667f);
        if (target) {
            const auto& body = target->motion;
            const float half[]{(body.box_max.x - body.box_min.x) * .55f,
                               (body.box_max.y - body.box_min.y) * .55f,
                               (body.box_max.z - body.box_min.z) * .55f};
            const float low[]{-half[0], -half[1], -half[2]}, high[]{half[0], half[1], half[2]};
            p.set_property(2, low, high);
        }
        light.position = motion.position;
        light.intensity = age * .5f;
        // Preset 139 lives in this host's frame; Draw adds motion.position once.
        p.set_position({});
        host.time += ticks;
        p.advance(host.time, age);
        if (stage >= 240 && age <= 0 && !p.alive())
            return remove_spell(*this);
        return 1;
    }
    case Effect::skin_of_stone: {
        auto& p = particles(match);
        if (!stage) {
            if (!target)
                return remove_spell(*this);
            launch_cast_hands(match, *this);
            particle_material(match, p, 0x0a011008,
                              FourCC{static_cast<unsigned>(data->integer(cc("tsha"), cc("rock").value))});
            const float scale = .5f, life = 1.5f;
            p.set_property(131, &scale, &life);
            const float slow[3]{-.05f, -.05f, -.05f}, fast[3]{.05f, .05f, .05f};
            p.set_property(3, slow, fast);
            remaining = static_cast<int>(data->duration * 60.f);
            linger = 0;
            spawn_form_child(match, *this, Effect::skin_of_stone_casting,
                             target); // 0x44E850 SkinOfStoneCasting.
            stage = 1;
        }
        if (stage == 1) {
            if (const auto next = form_cast(*this, 16, 240))
                stage = static_cast<int>(next);
        }
        if (stage == 16) {
            if (!target)
                return remove_spell(*this);
            name_form(*target, *this);
            watch_damage(match, *target, *this, skin_absorb_function);
            target->grayscale = true;
            match.watch(*target, 2, {this, reference(), skin_death_function});
            sound_event(match, 2);
            stage = 17;
        }
        if (stage == 17) {
            if (linger > 0) {
                if (target)
                    skin_fragments(match, *this, *target, 16);
                linger = 0;
            }
            if (!target || !target->alive())
                stage = 240;
            else {
                remaining -= ticks;
                if (remaining < 0)
                    stage = 240;
            }
        }
        if (stage == 240) {
            drop_form(match, target, *this);
            if (target)
                skin_fragments(match, *this, *target, data->integer(cc("inum"), 48));
            sound_event(match, 16);
            stage = 241;
        }
        if (target && target->alive())
            follow_target(*this);
        else if (stage < 240)
            stage = 240;
        p.set_position(motion.position);
        host.time += ticks;
        p.advance(host.time, 0);
        if (stage == 241 && !p.alive())
            return remove_spell(*this);
        return 1;
    }
    case Effect::air_shield: {
        auto& cloud = altar_cloud;
        if (!stage) {
            if (!target)
                return remove_spell(*this);
            remaining = static_cast<int>(data->duration * 60.f);
            emission = 1;
            age = 0;
            host.rate = .01f;
            cloud.enabled = true;
            cloud.material = 0x02011008;
            cloud.texture = FourCC{static_cast<unsigned>(data->integer(cc("ttex"), cc("icec").value))};
            if (!match.cloud_noise)
                match.cloud_noise = std::make_shared<CloudNoise>(match.visual_random);
            cloud.noise = match.cloud_noise;
            launch_cast_hands(match, *this);
            if (effect_target) {
                const auto& box = target->motion.box_max;
                const auto texture =
                    FourCC{static_cast<unsigned>(data->integer(cc("tcst"), cc("cst0").value))}.text();
                launch_glow(match, *this, 0x12012008, texture.c_str(), 2 * std::hypot(box.x, box.y, box.z), 0,
                            .5f, 1, effect_target, 1);
            }
            spawn_form_child(match, *this, Effect::spinny_particles, target); // 0x437090 / 0x437120.
            sound_event(match, 2);
            sound_event(match, 1);
            stage = 1;
        }
        if (stage == 1) {
            if (const auto next = form_cast(*this, 2, 240))
                stage = static_cast<int>(next);
        }
        if (stage == 2) {
            if (target) {
                watch_damage(match, *target, *this, air_shield_absorb_function);
                name_form(*target, *this);
            }
            emission = 1.5f;
            stage = 3;
        }
        if (stage == 3) {
            if (age == emission)
                emission = emission > 1.25f ? 1.25f : 1.5f;
            if (target && target->alive())
                spell_exclusion_field(match, motion.position, 1.f, data->spread_limit, 1.f / 12.f, 3, 5,
                                      target, 1);
            remaining -= ticks;
            if (remaining <= 0 || !target || !target->alive())
                stage = 240;
        }
        if (stage == 240) {
            stop_sound_event(1);
            drop_form(match, target, *this);
            emission = 0;
            host.rate = .05f;
            stage = 241;
        }
        if (target && target->alive())
            follow_target(*this);
        else if (stage < 240)
            stage = 240;
        ease(age, emission, ticks * host.rate);
        if (target) {
            const auto& box = target->motion;
            cloud.radii = {(box.box_max.x - box.box_min.x) * .5f * age,
                           (box.box_max.y - box.box_min.y) * .5f * age,
                           (box.box_max.z - box.box_min.z) * .5f * age};
        }
        cloud.opacity = age * (128.f / 255);
        cloud.phase.x += ticks * 8;
        cloud.phase.y += ticks * 8;
        cloud.phase.z += ticks * 8;
        cloud.frame += ticks * .25f;
        if (stage == 241 && age <= 0)
            return remove_spell(*this);
        return 1;
    }
    case Effect::protective_swarm: {
        auto& p = particles(match);
        if (!stage && remaining == 0) {
            if (!target)
                return remove_spell(*this);
            remaining = static_cast<int>(data->duration * 60.f);
            particle_material(match, p, 0x0a011008,
                              FourCC{static_cast<unsigned>(data->integer(cc("ttex"), cc("fb_g").value))});
            const float count = 0; // Preset 141's count is the integer bits; launch emits the 64 bugs later.
            const float swarm[7]{.2f, .5f, .5f, .5f, .3f, .2f, .05f};
            p.set_property(141, &count, swarm);
            sound_event(match, 1);
            if (target)
                name_form(*target, *this);
            match.assign(*this, spell_function_id("ProtectiveSwarmRun"));
            return 0;
        }
        if (!target || !target->alive()) {
            if (stage < 6)
                stage = 6;
        } else
            follow_target(*this);
        if (stage == 0) {
            // 0x4313F0(gather 1): one bug a tick, up to 64, at a random caster hand in the swarm's own frame.
            if (auto* caster = dynamic_cast<CreatureActor*>(owner))
                for (int i = 0; i < ticks && p.count() < 64; ++i) {
                    auto* bug = p.emit(0, 1);
                    if (!bug)
                        break;
                    Vec3 hand;
                    caster->write_reference_point(hand, match.random().next() & 1, true);
                    const auto& m = motion.orientation;
                    const Vec3 d{hand.x - motion.position.x, hand.y - motion.position.y, hand.z - motion.position.z};
                    bug->position = {m[0] * d.x + m[1] * d.y + m[2] * d.z, m[3] * d.x + m[4] * d.y + m[5] * d.z,
                                     m[6] * d.x + m[7] * d.y + m[8] * d.z};
                    bug->velocity = {};
                }
            if (const auto next = form_cast(*this, 1, 6))
                stage = static_cast<int>(next);
        }
        if (stage == 1) {
            if (target)
                watch_damage(match, *target, *this, protective_swarm_absorb_function);
            for (int n = static_cast<int>(p.count()); n < 64; ++n)
                if (!p.emit(0, 1))
                    break;
            next_pulse = 0;
            stage = 2;
        }
        if (stage == 2) {
            if (host.fade > 0) {
                // Stage 4: 24 bugs on a disc across the attacker's line at the hit point, fading over
                // .5..1.5 s. Native writes the world point into this model-frame system; ours keeps it local.
                sound_event(match, 4);
                const auto& m = motion.orientation;
                const Vec3 w{point.x - motion.position.x, point.y - motion.position.y, point.z - motion.position.z};
                const Vec3 hit{m[0] * w.x + m[1] * w.y + m[2] * w.z, m[3] * w.x + m[4] * w.y + m[5] * w.z,
                               m[6] * w.x + m[7] * w.y + m[8] * w.z};
                const float length = std::sqrt(hit.x * hit.x + hit.y * hit.y + hit.z * hit.z);
                const Vec3 d = length != 0 ? Vec3{hit.x / length, hit.y / length, hit.z / length} : Vec3{};
                for (int n = 0; n < 24; ++n)
                    if (auto* bug = p.emit(host.time, 1)) {
                        const Vec3 r{(match.random().next() - 0x4000) / 16384.f,
                                     (match.random().next() - 0x4000) / 16384.f,
                                     (match.random().next() - 0x4000) / 16384.f};
                        const float along = r.x * d.x + r.y * d.y + r.z * d.z;
                        bug->position = {hit.x - along * d.x + r.x, hit.y - along * d.y + r.y,
                                         hit.z - along * d.z + r.z};
                        bug->velocity = {};
                        bug->life = 30 + match.visual_random.next() * (60.f / 32768);
                        bug->color_rate[3] = -bug->color[3] / bug->life;
                    }
                host.fade = 0;
            }
            remaining -= ticks;
            if (remaining <= 0)
                stage = 6;
            next_pulse += ticks;
            if (next_pulse >= 64) {
                if (swarm_sting(match, *this))
                    sound_event(match, 4);
                next_pulse = 0;
            }
        }
        if (stage == 6) {
            drop_form(match, target, *this);
            stop_sound_event(255);
            p.fade(.25f, 1);
            stage = 7;
        }
        host.time += ticks;
        p.advance(host.time, 0);
        if (stage == 7 && !p.alive())
            return remove_spell(*this);
        return 1;
    }
    case Effect::turn_to_stone: {
        // TurnToStoneRun 0x4478b0, the Basilisk's casterless @stn on its victim.
        auto& p = particles(match);
        const auto para = cc("PARA");
        if (!stage) {
            if (!target)
                return remove_spell(*this);
            // 0x446750: the Skin of Stone chip emitter, idle until 0x446820 bursts it.
            particle_material(match, p, 0x0a011008,
                              FourCC{static_cast<unsigned>(data->integer(cc("tsha"), cc("rock").value))});
            const float scale = .5f, life = 1.5f;
            p.set_property(131, &scale, &life);
            const float slow[3]{-.05f, -.05f, -.05f}, fast[3]{.05f, .05f, .05f};
            p.set_property(3, slow, fast);
            linger = 0;
            // 0x446990: death watcher (slot 2), damage interrupt (slot 0), 'PARA' name, grayscale, sound 2.
            match.watch(*target, 2, {this, reference(), skin_death_function});
            watch_damage(match, *target, *this, stone_damage_function);
            target->name_reference(para, this);
            target->grayscale = true;
            sound_event(match, 2);
            // 0x4215b0: creatures with more than 1000 base life recover sooner; wizards stay a quarter.
            double factor = 1;
            if (target->kind_flags & 4)
                factor = target->stats.base[5] > 0 ? std::min(1.0, 1000.0 / target->stats.base[5]) : 1.0;
            if (target->kind_flags & 1)
                factor = .25;
            remaining = static_cast<int>(factor * data->duration * 60.0);
            stage = 16; // 0x441C30 only schedules stage 16; the first hold check runs on the next call.
        } else if (stage == 16) {
            // 0x421630: hold while the timer runs, the victim lives, and its FSM is (or enters, event 24)
            // the petrified state with flag 0x20.
            const bool hold = (remaining -= ticks) > 0 && target->alive() && target->fsm().state() &&
                              ((target->fsm().state()->flags & 0x20) || target->fsm().event(24) == 2);
            if (!hold)
                stage = 240;
            else if (linger) {
                skin_fragments(match, *this, *target, 16);
                linger = 0;
            }
        }
        if (stage == 240) {
            // 0x446900, then FSM events 25 and 7 release the pose.
            if (target->named(para) == this)
                target->grayscale = false;
            target->unname(para, this);
            match.unwatch(*target, 0, reference(), this);
            match.unwatch(*target, 2, reference(), this);
            skin_fragments(match, *this, *target, data->integer(cc("inum"), 48));
            sound_event(match, 16);
            if (target->alive()) {
                target->fsm().event(25);
                target->fsm().event(7);
            }
            stage = 241;
        }
        follow_target(*this);
        p.set_position(motion.position);
        host.time += ticks;
        p.advance(host.time, 0);
        if (stage == 241 && !p.alive())
            return remove_spell(*this);
        return 1;
    }
    case Effect::skin_of_stone_casting:
        return run_skin_casting(match, *this, ticks);
    case Effect::spinny_particles:
        return run_spinny(match, *this, ticks);
    default:
        throw Error("Invalid god form");
    }
}
} // namespace sacrifice
