#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/soul_actor.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include "sacrifice/spell_data.hpp"
#include "sacrifice/spell_registry.hpp"
#include <cmath>

namespace sacrifice {
namespace {
constexpr unsigned cinematic_ritual = 0x100; // Native altar-process context +92, separate from its visual flags.
FourCC cc(std::string_view text) {
    return FourCC::from_text(text);
}
int add(int a, int b) {
    return std::bit_cast<int>(static_cast<unsigned>(a) + static_cast<unsigned>(b));
}
int product(int a, int b) {
    return std::bit_cast<int>(static_cast<unsigned>(a) * static_cast<unsigned>(b));
}
int event(CreatureActor& actor, std::string_view name) {
    return actor.fsm().event(std::bit_cast<int>(cc(name).value));
}
bool valid(const SpellActor& spell) {
    if (!spell.effect_target)
        return false;
    if (!spell.network_identity)
        return true;
    if (!spell.sacrifice.altar || !spell.owner)
        return false;
    if (spell.sacrifice.action == cc("sacu"))
        return (spell.effect_target->kind_flags & 4) &&
               spell.effect_target->allegiance == spell.owner->allegiance;
    const auto* soul = dynamic_cast<const SoulActor*>(spell.effect_target);
    const auto* wizard = dynamic_cast<const CreatureActor*>(spell.owner);
    return !soul || !wizard || !wizard->alive() ||
           (!(wizard->common_status & 0x40) &&
            (!wizard->allegiance || !(soul->eligible_sides & wizard->allegiance->identity)));
}
// 0x44a8e0 / 0x44a7f0. The orifice searches two radii on both sides of the caster-target vector.
Vec3 orifice_position(Match& match, const CreatureActor& wizard, const MatchObject& target) {
    const auto to = target.source_motion()->position;
    Vec3 offset{to.x - wizard.motion.position.x, to.y - wizard.motion.position.y, 0}, result = to;
    bool found = false;
    for (float radius = 25; radius > 10 && !found; radius = static_cast<float>(radius - 7.5)) {
        const double length =
            std::sqrt(static_cast<double>(offset.x) * offset.x + static_cast<double>(offset.y) * offset.y);
        if (length != 0) {
            offset.x = static_cast<float>(offset.x * radius / length);
            offset.y = static_cast<float>(offset.y * radius / length);
        }
        for (float angle = 0; angle < 3.14159 && !found;
             angle = static_cast<float>(angle + .1745327777777778)) {
            const float sine = static_cast<float>(std::sin(angle)),
                        cosine = static_cast<float>(std::cos(angle));
            for (const float sign : {1.0f, -1.0f}) {
                Vec3 candidate{static_cast<float>(offset.y * sine * sign + cosine * offset.x + to.x),
                               static_cast<float>(to.y - offset.x * sine * sign + cosine * offset.y), 0};
                if (match.navigation && !match.navigation->grid().connected(candidate, to))
                    continue;
                candidate.z = static_cast<float>(terrain_surface(*match.terrain, candidate.x, candidate.y));
                if (candidate.z <= 0)
                    continue;
                bool clear = true;
                for (float fraction = .25f; fraction < .9; fraction = static_cast<float>(fraction + .25)) {
                    const Vec3 p{static_cast<float>((to.x - candidate.x) * fraction + candidate.x),
                                 static_cast<float>((to.y - candidate.y) * fraction + candidate.y), 0};
                    if (terrain_surface(*match.terrain, p.x, p.y) <= 0) {
                        clear = false;
                        break;
                    }
                }
                if (clear) {
                    result = candidate;
                    found = true;
                    break;
                }
            }
        }
    }
    result.z = static_cast<float>(result.z + 15.0);
    return result;
}
SpellActor& child(Match& match, CreatureActor& actor, SpellActor::Effect effect) {
    auto& spell = static_cast<SpellActor&>(match.create(
        MatchObject::spell, [] { return std::make_unique<SpellActor>(); }, &actor,
        [&](MatchObject& object) {
            auto& spell = static_cast<SpellActor&>(object);
            spell.effect = effect;
            spell.data.reset();
        }));
    spell.owner = &actor;
    spell.motion.position = actor.motion.position;
    spell.motion.orientation = actor.motion.orientation;
    spell.motion.yaw = actor.motion.yaw;
    spell.object_flags |= 0x10000;
    if (actor.allegiance)
        actor.allegiance->attach(spell);
    return spell;
}
} // namespace
SpellActor& SpellActor::launch_doctor_orifice(Match& match, CreatureActor& actor) {
    auto& spell = child(match, actor, Effect::doctor_orifice);
    spell.owner = actor.owner;
    spell.effect_target = actor.effect_target;
    spell.data = actor.casting.data;
    spell.sacrifice.action = actor.casting.definition;
    if (spell.sacrifice.action == cc("sacc")) {
        SoulActor* soul = dynamic_cast<SoulActor*>(spell.effect_target);
        if (!soul)
            match.visit_group(3, 2, [&](MatchObject& object) {
                auto* candidate = dynamic_cast<SoulActor*>(&object);
                if (candidate && candidate->anchor == spell.effect_target) {
                    soul = candidate;
                    return -1;
                }
                return 0;
            });
        spell.effect_target = soul; // SacSpellStart resolves a corpse to its soul before creating the child.
    }
    spell.sacrifice.altar = dynamic_cast<BuildingActor*>(
        actor.casting.requirements[spell.sacrifice.action == cc("sacc") ? 1 : 0]);
    spell.run(match, 0);
    return spell;
}
int SpellActor::run_doctor_orifice(Match& match, int ticks) {
    const auto remove = [&] {
        state_flags |= removed_flag;
        return -2;
    };
    switch (stage) {
    case 0:
        visual_requests.push_back({VisualRequest::Kind::doctor_orifice});
        if (auto* wizard = dynamic_cast<CreatureActor*>(owner); wizard && effect_target)
            motion.position = orifice_position(match, *wizard, *effect_target);
        if (const auto* wizard = dynamic_cast<CreatureActor*>(owner)) {
            const auto god = wizard->stats.current[18];
            launch_god_cast(match, *this, god >= 1 && god <= 5 ? god : 1, 3);
        }
        stage = 16;
        break;
    case 16:
        if (auto* wizard = dynamic_cast<CreatureActor*>(owner)) {
            const auto [state, alpha] = wizard->cast_state();
            sacrifice.alpha = alpha;
            if (state == 0x10)
                stage = 240;
            else if (state == 0x20000000) {
                stage = 17;
                sacrifice.alpha = 1;
            }
        } else
            stage = 240;
        break;
    case 17:
        if (cast_alpha >= 1)
            stage = 32;
        break;
    case 32:
        cast_alpha = 1;
        if (auto* wizard = dynamic_cast<CreatureActor*>(owner); wizard && network_identity && effect_target) {
            if (auto* doctor = match.create_creature(*wizard, cc("sacd"), motion.position, wizard))
                doctor->initialize_doctor(effect_target, sacrifice.altar, sacrifice.action,
                                          motion.position.z);
        }
        remaining = 120;
        stage = 33;
        [[fallthrough]];
    case 33:
        remaining = add(remaining, -ticks);
        if (remaining < 0)
            stage = 240;
        break;
    case 240:
        sacrifice.alpha = 0;
        if (cast_alpha <= 0)
            return remove();
        break;
    }
    if (!valid(*this)) {
        effect_target = nullptr;
        stage = 240;
    }
    update_orifice_visuals(match, ticks);
    return 1;
}
SpellActor& SpellActor::launch_doctor_beam(Match& match, CreatureActor& actor) {
    auto& spell = child(match, actor, Effect::doctor_beam);
    spell.effect_target = actor.effect_target;
    // 0x449590 creates this in mode 2; initialize when the child is reached by the object pass.
    return spell;
}
int SpellActor::run_doctor_beam(Match& match, int ticks) {
    if (!stage) {
        visual_requests.push_back({VisualRequest::Kind::doctor_beam});
        beam.enabled = true;
        beam.texture = cc("ltn2");
        launch_sacrifice_host(match, *this, Effect::doctor_beam_sparks);
        stage = 1;
    } else if (stage == 1) {
        auto* doctor = dynamic_cast<CreatureActor*>(owner);
        effect_target = doctor ? doctor->ritual_abort_target : nullptr;
        if (effect_target) {
            emission = static_cast<float>(emission + ticks * .05);
            if (emission >= 1) {
                emission = 1;
                doctor->doctor_beam_ready = 1;
            }
        } else
            stage = -1;
    }
    if (stage == -1) {
        age = static_cast<float>(age + ticks * .075);
        if (age >= emission - .05) {
            state_flags |= removed_flag;
            return -2;
        }
    }
    spell_target_point(point, effect_target);
    if (auto* doctor = dynamic_cast<CreatureActor*>(owner))
        doctor->write_reference_point(motion.position, 1, true);
    const auto frames = std::max(match.texture_frames ? match.texture_frames(beam.texture) : 1u, 1u);
    beam.frame += ticks * .5f;
    if (beam.frame >= frames)
        beam.frame = 0;
    LightningPrimitive::Parameters parameters;
    parameters.source = motion.position;
    parameters.target = point;
    parameters.start = age;
    parameters.end = emission;
    beam.primitive.set_params(parameters, match.visual_random);
    return 1;
}
SpellActor* SpellActor::launch_altar_process(Match& match, CreatureActor& wizard, BuildingActor& altar) {
    if (!altar.ritual.action.value || altar.ritual.wizard != &wizard || !altar.ritual.victim)
        return nullptr;
    auto& spell = child(match, wizard, Effect::altar_process);
    spell.sacrifice.altar = &altar;
    spell.sacrifice.action = altar.ritual.action;
    if (const auto* definition = match.spell_registry ? match.spell_registry->find(altar.ritual.action) : nullptr)
        spell.data = definition->spell;
    spell.effect_target = altar.ritual.victim;
    spell.motion.position = altar.sacrifice_point();
    spell.motion.orientation = motion_orientation(0);
    if (altar.ritual.action == cc("sacu") && altar.controller) {
        auto& globe = launch_sacrifice_host(match, spell, Effect::desecration_globe);
        globe.sacrifice.altar = &altar;
        globe.effect_target = &altar;
    }
    spell.run(match, 0);
    return &spell;
}
int SpellActor::run_altar_process(Match& match, int ticks) {
    auto& state = sacrifice;
    auto* wizard = dynamic_cast<CreatureActor*>(owner);
    auto* victim = dynamic_cast<CreatureActor*>(effect_target);
    auto* altar = state.altar;
    const bool suspended = match.cinematic && !(state.flags & cinematic_ritual);
    const auto doctors = [&](auto&& fn) {
        for (auto* doctor : state.doctors)
            if (doctor)
                fn(*doctor);
    };
    const auto interrupt = [&] {
        for (auto* doctor : state.doctors)
            if (doctor && !(doctor->fsm().state()->flags & 0x200)) {
                stage = 241;
                return;
            }
    };
    const auto prod = [&](bool start) {
        // Native helpers iterate backwards when setting the beam target and sending the event.
        for (auto i = state.doctors.rbegin(); i != state.doctors.rend(); ++i)
            if (auto* doctor = *i) {
                doctor->ritual_abort_target = start ? victim : nullptr;
                doctor->doctor_beam_ready = 0;
            }
        for (auto i = state.doctors.rbegin(); i != state.doctors.rend(); ++i)
            if (*i)
                event(**i, start ? "prod" : "tend");
    };
    if (stage > 0 && stage < 240 && (!victim || !wizard))
        stage = 241;
    switch (stage) {
    case 0:
        if (!altar || !wizard || !victim || altar->ritual.wizard != wizard) {
            state_flags |= removed_flag;
            return -2;
        }
        state.victim_origin = victim->motion.position;
        if (match.cinematic)
            state.flags |= cinematic_ritual;
        visual_requests.push_back({VisualRequest::Kind::altar_ritual});
        stage = 16;
        break;
    case 16:
        for (auto& doctor : state.doctors)
            doctor = match.create_creature(*wizard, cc("sacd"), motion.position, this);
        stage = 32;
        [[fallthrough]];
    case 32: {
        float angle = .78539753f;
        doctors([&](CreatureActor& doctor) {
            doctor.common_status |= 1;
            doctor.effect_target = victim;
            doctor.doctor_altar = altar;
            doctor.motion.position = {static_cast<float>(motion.position.x - std::sin(angle) * 20),
                                      static_cast<float>(motion.position.y + std::cos(angle) * 20), 0};
            doctor.motion.position.z = static_cast<float>(
                terrain_surface(*match.terrain, doctor.motion.position.x, doctor.motion.position.y));
            doctor.motion.yaw = static_cast<float>(angle + 3.14159);
            doctor.motion.orientation = motion_orientation(doctor.motion.yaw);
            doctor.state_flags |= 0x1800;
            doctor.doctor_circle = {motion.position, 15, 20, angle, angle, 20, true, true};
            doctor.fade = 1;
            doctor.fade_target = 0;
            doctor.fade_step = std::bit_cast<float>(1015580809u);
            match.assign(doctor, spell_function_id("SACDOCRunSacrifice"));
            angle = static_cast<float>(angle + 1.5707951);
        });
        state.flags |= 0x85;
        stage = 33;
        break;
    }
    case 33:
        if (!(state.flags & 1))
            stage = 48;
        break;
    case 48:
        state.flags |= 0x58;
        launch_sacrifice_host(match, *this, Effect::altar_cloud);
        launch_sacrifice_host(match, *this, Effect::altar_motes);
        for (unsigned i = 0; i < 6; ++i)
            launch_sacrifice_host(match, *this, Effect::altar_streamer);
        stage = 49;
        break;
    case 50:
        prod(false);
        stage = 49;
        [[fallthrough]];
    case 49: {
        bool ready = true;
        doctors([&](CreatureActor& doctor) {
            if (!(doctor.fsm().state()->flags & 1))
                ready = false;
        });
        if (ready)
            stage = 64;
        else
            interrupt();
        break;
    }
    case 64:
        doctors([](CreatureActor& doctor) {
            doctor.doctor_circle.angle = static_cast<float>(doctor.doctor_circle.current_angle + 1.5707951);
            doctor.doctor_circle.arrived = false;
        });
        stage = 65;
        break;
    case 65: {
        bool ready = true;
        doctors([&](CreatureActor& doctor) {
            if (!doctor.doctor_circle.arrived)
                ready = false;
        });
        if (ready)
            stage = 80;
        else
            interrupt();
        break;
    }
    case 80:
        prod(true);
        stage = 81;
        break;
    case 81: {
        bool ready = false;
        doctors([&](CreatureActor& doctor) {
            if (doctor.doctor_beam_ready == 1)
                ready = true;
        });
        if (ready)
            stage = 96;
        else
            interrupt();
        break;
    }
    case 96:
        interrupt();
        if (stage == 241 || suspended)
            break;
        if (altar) {
            auto& ritual = altar->ritual;
            if (state.action == cc("sacc")) {
                ritual.fraction = std::min(ritual.remaining, 240);
                ritual.remaining = add(ritual.remaining, -ritual.fraction);
                if (ritual.remaining < 60)
                    ritual.remaining = 0;
                wizard->earn_credit(ritual.initial_health, 2);
            } else
                ritual.fraction = 240;
        }
        stage = 97;
        [[fallthrough]];
    case 97: {
        // 0x448320 (at 0x60 and on each pass that ends in LABEL_67): the ritual's music for the local wizard.
        if (altar && wizard && match.local_side) {
            const auto* local = match.local_side->wizard();
            if (state.action == cc("sacc") && wizard == local) {
                const auto& a = altar->motion.position;
                const auto& b = wizard->motion.position;
                const double x = static_cast<double>(a.x) - b.x, y = static_cast<double>(a.y) - b.y,
                             z = static_cast<double>(a.z) - b.z;
                const auto d = static_cast<float>(std::sqrt(x * x + y * y + z * z));
                match.music(2, d <= 40 ? 1.0f : d < 80 ? (80 - d) * .025f : 0.0f);
            } else if (state.action == cc("sacu")) {
                if (wizard == local)
                    match.music(3, 1);
                else if (altar->controller && altar->controller == local)
                    match.music(4, 1);
            }
        }
        // 0x448190: suspend gameplay draining, but let the doctors finish their current cycle.
        bool active = false;
        if (victim && altar && !suspended) {
            auto& ritual = altar->ritual;
            if (state.action == cc("sacu")) {
                auto* target = dynamic_cast<CreatureActor*>(altar->controller);
                if (target) {
                    if (target->alive()) {
                        target->accumulate_damage({1, ritual.damage_rate, reference()}, ticks);
                        target->earn_credit(product(-ticks, ritual.mana_rate), 2);
                        target->change_mana_fixed(product(-ticks, ritual.fixed_rate));
                    } else
                        ritual.fraction = 60;
                } else
                    ritual.remaining = 0;
            }
            ritual.fraction = std::max(add(ritual.fraction, -ticks), 0);
            active = ritual.fraction != 0;
        }
        if (active)
            interrupt();
        else if (!altar || altar->ritual.remaining <= 0) {
            state.outcome = state.action == cc("sacu") ? -1 : victim ? victim->soul_count : 0;
            stage = 240;
        } else
            stage = 50;
        break;
    }
    case 240:
    case 241:
        if (stage == 240)
            prod(false);
        else
            for (auto i = state.doctors.rbegin(); i != state.doctors.rend(); ++i)
                if (*i) {
                    (*i)->ritual_abort_target = nullptr;
                    (*i)->doctor_beam_ready = 0;
                }
        if (victim) {
            if (stage == 240) {
                if (wizard && wizard->allegiance && state.outcome)
                    wizard->allegiance->event(*wizard, state.outcome > 0 ? victim : nullptr,
                                              state.outcome > 0 ? 9 : 24,
                                              state.outcome > 0 ? state.outcome : 0);
                state.outcome = 0;
                if (state.action == cc("sacu") && altar)
                    altar->destroy(match);
                if (victim->type != MatchObject::wizard) {
                    victim->combat_requests.push_back({CreatureActor::CombatRequest::Kind::gibs, 8});
                    if (victim->network_identity && wizard)
                        wizard->change_souls(victim->soul_count);
                    victim->change_souls(std::bit_cast<int>(0u - static_cast<unsigned>(victim->soul_count)));
                    victim->state_flags |= removed_flag;
                }
            } else {
                if (altar)
                    altar->request_sacrifice({}, victim, nullptr, nullptr);
                event(*victim, "sacd");
                if (altar && altar->controller && altar->controller->source_motion()) {
                    const auto from = altar->controller->source_motion()->position;
                    const double dx = static_cast<double>(victim->motion.position.x) - from.x,
                                 dy = static_cast<double>(victim->motion.position.y) - from.y,
                                 dz = static_cast<double>(victim->motion.position.z) - from.z,
                                 length = std::sqrt(dz * dz + dy * dy + dx * dx);
                    victim->motion.velocity.x = static_cast<float>(victim->motion.velocity.x +
                                                                   (length != 0 ? dx * (-.5 / length) : dx));
                    victim->motion.velocity.y = static_cast<float>(victim->motion.velocity.y +
                                                                   (length != 0 ? dy * (-.5 / length) : dy));
                    victim->motion.velocity.z = 0;
                }
                match.notify_reference(*victim, 16);
            }
        } else {
            stage = 242;
            state.flags = 32;
        }
        break;
    case 242:
        if (!(state.flags & 32))
            stage = 243;
        break;
    case 243:
        state_flags |= removed_flag;
        return -2;
    }
    if (state.flags & 1) {
        state.lift_angle = static_cast<float>(ticks * state.angular_speed + state.lift_angle);
        if (state.lift_angle >= 1.570795) {
            state.lift_angle = 1.5707951f;
            state.angular_speed = 0;
            state.flags = (state.flags & ~7u) | 6;
        }
        if (victim) {
            victim->motion.position = {
                static_cast<float>((state.victim_origin.x - motion.position.x) * std::cos(state.lift_angle) +
                                   motion.position.x),
                static_cast<float>((state.victim_origin.y - motion.position.y) * std::cos(state.lift_angle) +
                                   motion.position.y),
                static_cast<float>((motion.position.z - state.victim_origin.z) * std::sin(state.lift_angle) +
                                   state.victim_origin.z)};
        }
    }
    // 0x44d9a0: horizontal exclusion field, native creature group before wizard group.
    if (state.flags & 0x80)
        spell_exclusion_field(match, motion.position, 20, 30, std::bit_cast<float>(1040263575u), 3, 5);
    if ((state.flags & 2) && victim)
        victim->motion.yaw = static_cast<float>(ticks * state.angular_speed + victim->motion.yaw);
    if (state.flags & 4) {
        if (state.angular_speed >= .01)
            state.flags &= ~4u;
        else
            state.angular_speed = static_cast<float>(ticks * .0001 + state.angular_speed);
    }
    if (state.flags & 8) {
        if (state.alpha >= 2)
            state.flags &= ~8u;
        else
            state.alpha = static_cast<float>(state.alpha + ticks * .05);
    } else if (state.flags & 32) {
        if (state.alpha > 0)
            state.alpha = static_cast<float>(state.alpha - ticks * .05);
        else
            state.flags &= ~32u;
    }
    update_altar_ring(match, ticks);
    return 1;
}
} // namespace sacrifice
