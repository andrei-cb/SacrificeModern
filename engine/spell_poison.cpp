#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/spatial.hpp"
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
SpellActor* SpellActor::poison(Match& match, CreatureActor& actor, MatchObject* source,
                               const SpellData& data) {
    return poison(match, actor, source, data.duration, data.amount, data.damage_type,
                  data.integer(cc("ires"), 0), data.real(cc("freg"), 1), data.integer(cc("iacc"), 0),
                  data.spread_limit);
}
SpellActor* SpellActor::poison(Match& match, CreatureActor& actor, MatchObject* source, float seconds,
                               int amount, int damage_type, int resistance_delta, float regeneration,
                               int accuracy_delta, float spread_limit) {
    if (!actor.alive())
        return nullptr;
    auto* effect = dynamic_cast<SpellActor*>(actor.named(cc("POIS")));
    if (actor.named(cc("POIS")) && !effect)
        throw Error("POIS reference has an incompatible host type");
    if (!effect) {
        auto& object = match.create(
            MatchObject::spell, [] { return std::make_unique<SpellActor>(); }, &actor,
            [](MatchObject& object) {
                auto* spell = dynamic_cast<SpellActor*>(&object);
                if (!spell)
                    throw Error("Spell pool object has an incompatible host type");
                spell->effect = Effect::poison;
                spell->data.reset();
            });
        effect = static_cast<SpellActor*>(&object);
        effect->owner = actor.owner;
        effect->effect_target = &actor;
        effect->motion.position = actor.motion.position;
        effect->motion.orientation = actor.motion.orientation;
        effect->motion.yaw = actor.motion.yaw;
        effect->object_flags |= 0x10000;
        if (actor.allegiance)
            actor.allegiance->attach(*effect);
        effect->run(match, 1); // Initial 60-tick dose runs once before the caller's parameters are merged.
    }
    effect->owner = source;
    if (source && (source->kind_flags & 5))
        amount = source->damage_dealt(match, actor, damage_type, amount);
    auto& state = effect->poison_state;
    if (effect->remaining <= 0) {
        effect->remaining = integer(seconds * 60.0);
        state = {resistance_delta, accuracy_delta,   integer(amount / static_cast<double>(seconds)),
                 damage_type,      state.scan_clock, regeneration,
                 spread_limit};
    } else {
        const int old_amount = integer(multiply(effect->remaining, state.damage_rate) * (1.0 / 60.0));
        if (amount > old_amount)
            state.damage_type = damage_type;
        const double duration = std::max(effect->remaining * (1.0 / 60.0), static_cast<double>(seconds));
        effect->remaining = integer(duration * 60.0);
        state.resistance_delta = std::min(state.resistance_delta, resistance_delta);
        state.regeneration = std::min(state.regeneration, regeneration);
        state.accuracy_delta = std::max(state.accuracy_delta, accuracy_delta);
        state.spread_limit = std::max(state.spread_limit, spread_limit);
        state.damage_rate = integer(std::max(add(old_amount, amount) / duration,
                                             static_cast<double>(static_cast<float>(state.damage_rate))));
    }
    return effect;
}
int SpellActor::run_poison(Match& match, int ticks) {
    auto* actor = effect_target && (effect_target->kind_flags & 5)
                      ? dynamic_cast<CreatureActor*>(effect_target)
                      : nullptr;
    if (!stage) {
        if (!actor) {
            state_flags |= removed_flag;
            return -2;
        }
        age = emission = 0;
        stage = 1;
        poison_state = {};
        remaining = 60;
        spell_target_point(motion.position, actor);
        motion.flags = 1;
        visual_requests.push_back({VisualRequest::Kind::poison_particles});
        // A burst of 16 'pois' motes inside the victim's box (preset 132, in the effect's frame).
        auto& system = particles(match);
        const auto& box = actor->motion;
        const Vec3 half{(box.box_max.x - box.box_min.x) * 0.5f, (box.box_max.y - box.box_min.y) * 0.5f,
                        (box.box_max.z - box.box_min.z) * 0.5f};
        // 0x44ca80(box, 0.3, 2, 0.25): the box diagonal × 0.15 within 0.25..2.
        const double diagonal = std::sqrt(4.0 * half.x * half.x + 4.0 * half.y * half.y + 4.0 * half.z * half.z);
        const float scale = static_cast<float>(std::clamp(diagonal * 0.3 * 0.5, 0.25, 2.0)), life = 5;
        particle_material(match, system, 0x0a011008, "pois");
        system.set_property(132, &scale, &life);
        const float drift_low[3]{-0.0005f, -0.0005f, -0.001f}, drift_high[3]{0.0005f, 0.0005f, 0};
        system.set_property(3, drift_low, drift_high);
        const float low[3]{-half.x, -half.y, -half.z}, high[3]{half.x, half.y, half.z};
        system.set_property(2, low, high);
        for (int i = 0; i < 16; ++i)
            system.emit(0, 1);
        live_particles = static_cast<unsigned>(system.count());
        actor->name_reference(cc("POIS"), this);
    }
    bool active = false;
    if (actor) {
        if (!actor->alive()) {
            actor->unname(cc("POIS"), this);
            effect_target = nullptr;
            actor = nullptr;
        } else if (remaining > 0) {
            active = true;
            const auto modify = [&](unsigned index, int delta) {
                actor->stats.changed |= 1u << index;
                actor->stats.target[index] =
                    static_cast<std::int16_t>(std::max(add(actor->stats.target[index], delta), 0));
            };
            if (poison_state.resistance_delta)
                for (unsigned i = 9; i <= 13; ++i)
                    modify(i, poison_state.resistance_delta);
            actor->stats.target[6] = actor->stats.target[20] = 0;
            actor->stats.changed |= (1u << 6) | (1u << 20);
            modify(8, poison_state.accuracy_delta);
            actor->accumulate_damage(
                {poison_state.damage_type, multiply(3, poison_state.damage_rate) / 2, reference()}, ticks);
            remaining = add(remaining, -ticks);
            poison_state.scan_clock = add(poison_state.scan_clock, -ticks);
            emission = 0.10666667f;
            if (poison_state.scan_clock <= 0) {
                poison_state.scan_clock = 16;
                if (remaining > 60 && poison_state.spread_limit > 0) {
                    if (!match.spatial)
                        throw Error("Spreading poison requires a spatial service");
                    match.spatial->visit_members(actor->motion, 5, [&](const ObjectMotion& motion) {
                        CreatureActor* candidate = nullptr;
                        for (const auto& object : match.objects())
                            if (object->source_motion() == &motion) {
                                candidate = dynamic_cast<CreatureActor*>(object.get());
                                break;
                            }
                        if (!candidate || candidate == actor || candidate->named(cc("HEAL")) ||
                            candidate->named(cc("POIS")))
                            return 0;
                        if (spell_box_separation(actor->motion, motion) > poison_state.spread_limit)
                            return 0;
                        const float seconds = static_cast<float>(remaining * (1.0 / 60.0));
                        poison(match, *candidate, owner, seconds,
                               integer(poison_state.damage_rate * static_cast<double>(seconds)),
                               poison_state.damage_type, poison_state.resistance_delta,
                               poison_state.regeneration, poison_state.accuracy_delta,
                               poison_state.spread_limit);
                        return 0;
                    });
                }
            }
        }
    }
    if (!active) {
        emission = 0;
        if (!live_particles) {
            state_flags |= removed_flag;
            return -2;
        }
    }
    if (effect_target) {
        spell_target_point(point, effect_target); // A fading effect still follows a kind-changed corpse.
        for (int i = 0; i < ticks; ++i) {
            motion.position.x = static_cast<float>((point.x - motion.position.x) * 0.2 + motion.position.x);
            motion.position.y = static_cast<float>((point.y - motion.position.y) * 0.2 + motion.position.y);
            motion.position.z = static_cast<float>((point.z - motion.position.z) * 0.2 + motion.position.z);
        }
    }
    age = static_cast<float>(static_cast<double>(age) + ticks);
    if (!particle_systems.empty())
        particle_systems.front().advance(age, emission);
    return 1;
}
} // namespace sacrifice
