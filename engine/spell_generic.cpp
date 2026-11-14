#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include "sacrifice/spell_data.hpp"
#include <cmath>

namespace sacrifice {
namespace {
FourCC cc(std::string_view text) {
    return FourCC::from_text(text);
}
int add(int a, int b) {
    return std::bit_cast<int>(static_cast<unsigned>(a) + static_cast<unsigned>(b));
}
int multiply(int a, int b) {
    return std::bit_cast<int>(static_cast<unsigned>(a) * static_cast<unsigned>(b));
}
int integer(double value) {
    if (!std::isfinite(value) || value >= 9223372036854775808.0 || value < -9223372036854775808.0)
        return 0;
    return std::bit_cast<int>(static_cast<unsigned>(static_cast<std::int64_t>(value)));
}
} // namespace
SpellActor& SpellActor::launch_generic_shoot(Match& match, CreatureActor& actor) {
    auto& object = match.create(
        MatchObject::spell, [] { return std::make_unique<SpellActor>(); }, &actor,
        [&](MatchObject& object) {
            auto* spell = dynamic_cast<SpellActor*>(&object);
            if (!spell)
                throw Error("Spell pool object has an incompatible host type");
            spell->effect = Effect::generic_shoot;
            spell->data = actor.casting.data;
        });
    auto& spell = static_cast<SpellActor&>(object);
    spell.owner = actor.owner;
    spell.effect_target = actor.effect_target;
    spell.point = actor.casting.point;
    spell.motion.position = actor.motion.position;
    spell.motion.orientation = actor.motion.orientation;
    spell.motion.yaw = actor.motion.yaw;
    spell.motion.pitch = actor.motion.pitch;
    spell.motion.roll = actor.motion.roll;
    spell.object_flags |= 0x10000;
    spell.motion.flags |= 0x10000; // 0x44e850: child objects reject external pushes until impact.
    if (actor.allegiance)
        actor.allegiance->attach(spell);
    spell.run(match, 0);
    return spell;
}
int SpellActor::start_generic_shoot(Match& match) {
    if (!data)
        throw Error("GenericShootStart requires a spell definition");
    auto* caster = dynamic_cast<CreatureActor*>(owner);
    if (!caster)
        throw Error("GenericShootStart requires a creature owner");
    bind_world(match);
    generic.type = {static_cast<unsigned>(data->integer(cc("ttyp"), cc("norm").value))};
    generic.movement = {static_cast<unsigned>(data->integer(cc("tmov"), cc("norm").value))};
    generic.hit = {static_cast<unsigned>(data->integer(cc("thit"), cc("norm").value))};
    generic.angle_step = std::bit_cast<float>(0x3e567744u);
    if (generic.type == cc("tube") || generic.type == cc("heat"))
        generic.collision_radius = 0;
    else if (generic.type == cc("swrm"))
        generic.collision_radius = 0.2f;
    else
        generic.collision_radius = static_cast<float>(data->real(cc("frad"), 0.4f) * 0.2);
    generic.initialized = true;
    flight_remaining = add(integer(static_cast<double>(data->range) / data->speed), 30);
    caster->write_reference_point(motion.position, 0, true);
    spell_lead_target(point, effect_target, data->speed, motion, *match.terrain);
    motion.velocity = {point.x - motion.position.x, point.y - motion.position.y, point.z - motion.position.z};
    const double distance = std::sqrt(static_cast<double>(motion.velocity.x) * motion.velocity.x +
                                      static_cast<double>(motion.velocity.y) * motion.velocity.y +
                                      static_cast<double>(motion.velocity.z) * motion.velocity.z);
    if (distance != 0) {
        const double scale = data->speed / distance;
        motion.velocity = {static_cast<float>(motion.velocity.x * scale),
                           static_cast<float>(motion.velocity.y * scale),
                           static_cast<float>(motion.velocity.z * scale)};
    }
    spell_scatter(motion.velocity, caster->stats.current[8], match.random());
    motion.orientation = spell_orientation(motion.velocity);
    motion.flags |= 1;
    motion.collision_mask |= 0x114015;
    visual_requests.push_back({VisualRequest::Kind::generic_projectile});
    {
        // 0x412550: the projectile's own particles, keyed by 'ttyp' with the 'ttex' texture.
        auto& system = particles(match);
        const auto texture = FourCC{static_cast<unsigned>(data->integer(cc("ttex"), cc("fbal").value))};
        const auto blend = data->integer(cc("ibln"), 0) ? 0x0a011008u : 0x1a012008u;
        system.set_frame({motion.orientation, motion.position});
        emission = data->real(cc("fvig"), 1);
        if (generic.type == cc("swrm")) {
            particle_material(match, system, 0x1a010001, texture);
            const auto count = std::bit_cast<float>(data->integer(cc("inum"), 64));
            static constexpr float swarm[8]{0.1f, 0.2f, 0.2f, 0.2f, 0.2f, 0.05f, 0.05f, 0.1f}; // flt_4CB82C.
            system.set_property(136, &count, swarm);
            emission = 0;
        } else if (generic.type == cc("tube") || generic.type == cc("heat")) {
            const float radius = data->real(cc("frad"), 0.5f), life = data->real(cc("flif"), 2);
            const auto capacity = std::bit_cast<float>(0x80);
            system.set_property(8, &capacity, nullptr);
            particle_material(match, system, blend, texture);
            system.set_property(134, &radius, &life);
            system.set_frame({motion.orientation, motion.position});
            const float still[3]{};
            system.set_property(3, still, still);
        } else {
            const float radius = data->real(cc("frad"), 0.4f), life = data->real(cc("flif"), 0.75f);
            particle_material(match, system, generic.type == cc("blnd") ? 0x0a011008u : blend, texture);
            system.set_property(129, &radius, &life);
            static constexpr float low[3]{-0.01f, -0.01f, -0.01f}, high[3]{0.01f, 0.01f, 0.01f}; // flt_4CB0DC.
            system.set_property(3, low, high);
        }
        system.advance(0, emission);
    }
    if (generic.type == cc("heat")) {
        visual_requests.push_back({VisualRequest::Kind::generic_heat});
        // 0x4126fa: 0x44c230(projectile, 38, HeatBeamRun, 0). The type-38 LIGHTNING child starts at the
        // launch point and targets its parent (+476), drawing the red ray behind the heat projectile.
        auto& ray = static_cast<SpellActor&>(match.create(
            MatchObject::spell, [] { return std::make_unique<SpellActor>(); }, this, [](MatchObject& object) {
                auto* spell = dynamic_cast<SpellActor*>(&object);
                if (!spell)
                    throw Error("Spell pool object has an incompatible host type");
                spell->effect = Effect::heat_beam;
            }));
        ray.object_flags |= 0x10000;
        ray.owner = owner;
        ray.data = data;
        ray.effect_target = this;
        ray.motion.position = motion.position;
        ray.motion.orientation = motion.orientation;
        ray.point = motion.position;
    }
    stage = 0;
    age = 0;
    sound_event(match, 2);
    match.assign(*this, spell_function_id("GenericShootRun"));
    return 0;
}
int SpellActor::run_heat_beam(Match& match, int ticks) {
    // HeatBeamRun 0x41d180. Context: +4..+24 source/target, +28 step 0.2, +32 deviation 0, +36 start,
    // +40 end 1, +44 segments 16, +48 width 0.6, +64 frame, +68 frame count, +72 frame rate 0.5,
    // +104 hold ticks, +112 state.
    if (!stage) {
        beam.enabled = true;
        beam.texture = cc("vtx1");
        beam.material = 0x1a012008;
        beam.frame = 0;
        remaining = 20;
        age = 0; // Context +36: the retracting start of the ray.
        stage = 1;
    }
    switch (stage) {
    case 1: {
        // The ray follows the projectile while its motion flags (+480) are live; GenericShootRun clears
        // them on impact and expiry.
        const auto* projectile = dynamic_cast<const SpellActor*>(effect_target);
        if (projectile && !(projectile->state_flags & removed_flag) && projectile->motion.flags)
            point = projectile->motion.position;
        else
            stage = 2;
        break;
    }
    case 2:
        remaining = add(remaining, -ticks);
        if (remaining <= 0)
            stage = 3;
        break;
    case 3:
        age = static_cast<float>(ticks * 0.01666666666666667 + age);
        if (age >= 1) {
            beam.enabled = false;
            state_flags |= removed_flag;
            return -2;
        }
        break;
    default:
        break;
    }
    const auto frames = std::max(match.texture_frames ? match.texture_frames(beam.texture) : 1u, 1u);
    beam.frame = static_cast<float>(ticks * 0.5 + beam.frame);
    if (beam.frame >= static_cast<float>(frames))
        beam.frame = 0;
    LightningPrimitive::Parameters parameters;
    parameters.source = motion.position;
    parameters.target = point;
    parameters.step = .2f;
    parameters.deviation = 0;
    parameters.start = age;
    parameters.end = 1;
    parameters.segments = 16;
    parameters.width = .6f;
    parameters.strands = 1; // Context +52 stays zero; LIGHTNINGPRIM draws at least one strand.
    beam.primitive.set_params(parameters, match.visual_random);
    return 1;
}
bool SpellActor::generic_hit(Match& match, int allow_damage) {
    auto* victim = impact_target && (impact_target->kind_flags & 5)
                       ? dynamic_cast<CreatureActor*>(impact_target)
                       : nullptr;
    if (generic.hit == cc("heat")) {
        if (victim) {
            const int numerator =
                std::bit_cast<int>((0u - static_cast<unsigned>(victim->maximum_mana)) << 16);
            generic.mana_rate = integer(numerator / (data->duration * 120.0));
        }
        generic.hit = cc("burn");
    }
    if (generic.hit == cc("burn") || generic.hit == cc("fire")) {
        visual_requests.push_back({VisualRequest::Kind::generic_hit});
        // 0x411bb0 LABEL_20: 0x4375A0(0.25, spell, 'firy', 0.35, 2, 0.75, rate 2, 0.5 s, no sound, no box).
        launch_inert_fire(match, *this, cc("firy"), .35f, 2, .75f, 2, .5f);
        if (generic.hit == cc("burn") && victim) {
            remaining = integer(data->duration * 60.0);
            victim->burn_near(motion.position, 5, remaining);
            stage = 5;
            return false;
        }
        return true; // Native fiery impacts override idam, including hits on structures.
    }
    if (generic.hit == cc("ko__")) {
        if (victim)
            victim->knock_down(10, 10);
    } else if (generic.hit == cc("push")) {
        emission = 0;
        if (impact_target) {
            stage = 4;
            remaining = 30;
        }
    } else if (generic.hit == cc("swrm")) {
        visual_requests.push_back({VisualRequest::Kind::generic_swarm});
        // 0x411bb0 'swrm': re-seed preset 136 with the clinging swarm table flt_4CB848.
        if (!particle_systems.empty()) {
            const auto count = std::bit_cast<float>(data->integer(cc("inum"), 64));
            static constexpr float cling[8]{0.1f, 0.8f, 0.8f, 0.8f, 0.2f, 0.05f, 0.05f, 0.2f}; // flt_4CB848.
            particle_systems.front().set_property(136, &count, cling);
        }
        effect_target = impact_target;
        remaining = 120;
        stage = 2;
    } else if (generic.hit == cc("disz") || generic.hit == cc("psys")) {
        effect_target = impact_target;
        if (victim)
            poison(match, *victim, owner, *data);
        return false;
    } else if (generic.hit == cc("disr")) {
        int amount = data->amount;
        if (impact_target && (match.random().next() & 1)) {
            amount = add(amount, data->integer(cc("idam"), 15));
            if (victim)
                victim->fsm().event(7);
        }
        spell_damage(match, this, impact_target, data->damage_type, amount, data->radius);
        visual_requests.push_back({VisualRequest::Kind::generic_hit});
        // 0x411ca4: 0x43A5E0(spell, 0x1A012008, 'tube', position - velocity, position, speed 0, rise 1.5,
        // radius 0.3, end size 1.5, life 0.3, rate 0.5, 0.3 s).
        const Vec3 from{motion.position.x - motion.velocity.x, motion.position.y - motion.velocity.y,
                        motion.position.z - motion.velocity.z};
        launch_generic_moving(match, *this, 0x1a012008, cc("tube"), from, motion.position, 0, 1.5f, .3f, 1.5f,
                              .3f, .5f, .3f);
        return false;
    }
    return allow_damage != 0;
}
int SpellActor::run_generic_shoot(Match& match, int ticks) {
    if (!data)
        throw Error("GenericShootRun requires a spell definition");
    age = static_cast<float>(static_cast<double>(age) + ticks);
    if (!particle_systems.empty())
        particle_systems.front().advance(age, emission);
    generic.particle_position = motion.position;
    if (generic.movement == cc("spyn")) {
        const float x = static_cast<float>(std::sin(generic.angle) * generic.spiral_radius);
        const float z = static_cast<float>(std::cos(generic.angle) * generic.spiral_radius);
        const auto& o = motion.orientation;
        generic.particle_position = {static_cast<float>(x * static_cast<double>(o[0]) +
                                                        z * static_cast<double>(o[6]) + motion.position.x),
                                     static_cast<float>(x * static_cast<double>(o[1]) +
                                                        z * static_cast<double>(o[7]) + motion.position.y),
                                     static_cast<float>(x * static_cast<double>(o[2]) +
                                                        z * static_cast<double>(o[8]) + motion.position.z)};
        generic.spiral_radius = static_cast<float>(
            std::min(generic.spiral_radius + 0.05, static_cast<double>(generic.spiral_max)));
        generic.angle += generic.angle_step;
    }
    if (!particle_systems.empty())
        particle_systems.front().set_position(generic.particle_position);
    switch (stage) {
    case 1:
        sound_event(match, 4);
        motion.collision_mask = motion.flags = 0;
        generic.mana_rate = 0;
        stage = 3;
        if (generic_hit(match, data->integer(cc("idam"), 1)))
            spell_damage(match, this, impact_target, data->damage_type, data->amount, data->radius);
        break;
    case 2:
        spell_target_point(motion.position, effect_target);
        stop_sound_event(2);
        remaining = add(remaining, -ticks);
        if (remaining <= 0) {
            if (!particle_systems.empty()) {
                const float range[2]{0.25f, 1}; // Property 10: the swarm ends within 0.25..1 s.
                particle_systems.front().set_property(10, range, range);
            }
            stage = 3;
        }
        break;
    case 3:
        emission = 0;
        motion.collision_mask = motion.flags = 0;
        if (!live_particles) {
            state_flags |= removed_flag;
            return -2;
        }
        break;
    case 4:
        emission = 0;
        if (!impact_target) {
            stage = 3;
            break;
        }
        if (auto* actor = dynamic_cast<CreatureActor*>(impact_target))
            actor->motion.push_direction(motion.velocity,
                                         static_cast<float>(data->real(cc("fpsh"), data->real(cc("fshk"), 2)) * 0.05), 3);
        // Native structures have the same object motion prefix.
        else if (auto* target_motion = const_cast<ObjectMotion*>(impact_target->source_motion()))
            target_motion->push_direction(motion.velocity,
                                          static_cast<float>(data->real(cc("fpsh"), data->real(cc("fshk"), 2)) * 0.05), 3);
        remaining = add(remaining, -ticks);
        if (remaining <= 0)
            stage = 3;
        break;
    case 5:
        emission = 0;
        if (auto* victim = dynamic_cast<CreatureActor*>(impact_target)) {
            if (generic.mana_rate)
                victim->change_mana_fixed(multiply(ticks, generic.mana_rate));
            victim->accumulate_damage(
                {data->damage_type, integer(data->amount / static_cast<double>(data->duration)), reference()},
                ticks);
            remaining = add(remaining, -ticks);
            if (remaining <= 0)
                stage = 3;
        } else
            stage = 3;
        break;
    default:
        if (state_flags & 3) {
            stage = 1;
            impact_target = nullptr;
            if (motion.collided)
                for (const auto& object : match.objects())
                    if (object->source_motion() == motion.collided) {
                        impact_target = object.get();
                        break;
                    }
            state_flags &= ~3u;
        } else {
            flight_remaining = add(flight_remaining, -ticks);
            if (flight_remaining <= 0)
                stage = 3;
        }
        break;
    }
    return 1;
}
} // namespace sacrifice
