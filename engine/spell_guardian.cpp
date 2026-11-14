#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_data.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
void BuildingActor::add_guard(Match& match, CreatureActor& actor) {
    if (std::ranges::find(guards, &actor) != guards.end())
        return;
    actor.ritual_building = this;
    actor.issue_order({5, this, motion.position});
    SpellActor::launch_guardian_beam(match, actor);
    guards.insert(guards.begin(), &actor);
}
void BuildingActor::remove_guard(CreatureActor& actor) {
    std::erase(guards, &actor);
    if (actor.ritual_building == this)
        actor.ritual_building = nullptr;
}
void CreatureActor::set_guard(Match& match, BuildingActor* building) {
    if (auto* previous = dynamic_cast<BuildingActor*>(ritual_building))
        previous->remove_guard(*this);
    ritual_building = nullptr;
    if (building)
        building->add_guard(match, *this);
}
// 0x45e980: stat targets are refreshed each pass, before the normal stat easing/reset.
void CreatureActor::run_guard(Match& match) {
    auto* building = dynamic_cast<BuildingActor*>(ritual_building);
    if (!building || building->allegiance != allegiance || !alive()) {
        set_guard(match, nullptr);
        return;
    }
    const double factor = building->guards.size() > 3 ? 3.0 / building->guards.size() : 1.0;
    const auto scale = [&](unsigned index, double multiplier) {
        stats.changed |= 1u << index;
        const double value = stats.target[index] * multiplier;
        stats.target[index] = static_cast<std::int16_t>(static_cast<std::int64_t>(std::max(value, 0.0)));
    };
    stats.changed |= 0x40;
    stats.target[6] = static_cast<std::int16_t>(std::max(0, stats.target[6] + static_cast<int>(factor * 1000)));
    scale(7, 1 + factor);
    scale(8, 1 - factor);
    scale(12, 1 - factor * .75);
    scale(13, 1 - factor * .75);
    scale(10, 1 - factor * .5);
    scale(11, 1 - factor * .5);
    const auto& p = building->motion.position;
    const double x = static_cast<double>(motion.position.x) - p.x,
                 y = static_cast<double>(motion.position.y) - p.y,
                 z = static_cast<double>(motion.position.z) - p.z;
    if (x * x + y * y + z * z > 2500 && (orders.kind() != 5 || orders.current().target != building))
        issue_order({5, building, p});
}

// GuardianSpellRun 0x45ef10. The target is a creature, and 0x4821e0 finds its nearest own altar/manalith
// in the same navigation region, with a strict 5000-unit maximum and activation-group tie order.
int SpellActor::run_guardian(Match& match, int) {
    // GuardianSpellRun 0x45ef10: 0x44C0D0 enters stage 1 with a caster and stage 16 without one.
    if (!stage) {
        if (!owner)
            stage = 16;
        else {
            launch_cast_hands(match, *this);
            stage = 2;
        }
    }
    if (stage == 2) {
        // 0x441d20: only a wizard caster reports release (0x20000000) or abort; another owner aborts.
        auto* caster = dynamic_cast<CreatureActor*>(owner);
        if (!caster || !(caster->kind_flags & 1))
            stage = 240;
        else {
            const auto [state, alpha] = caster->cast_state();
            cast_alpha = alpha;
            if (state)
                stage = state == 0x20000000 ? 16 : 240;
        }
        return 1; // The new stage runs on the next call.
    }
    if (stage == 16) {
        auto* target = dynamic_cast<CreatureActor*>(effect_target);
        if (target && (target->kind_flags & 4) && target->allegiance == allegiance && target->alive()) {
            BuildingActor* nearest = nullptr;
            double best = 25000000;
            match.visit_group(2, MatchObject::building, [&](MatchObject& object) {
                auto* b = dynamic_cast<BuildingActor*>(&object);
                if (!b || b->reveal != 1 || (b->primary && b->primary != b) || b->allegiance != allegiance ||
                    (!(b->building_flags & 2) && (b->primary || (b->building_flags & 0x55a0) != 0x55a0)) ||
                    (match.navigation && !match.navigation->grid().connected(target->motion.position, b->motion.position)))
                    return 0;
                const auto p = target->motion.position, q = b->motion.position;
                const double x = static_cast<double>(p.x) - q.x, y = static_cast<double>(p.y) - q.y,
                             z = static_cast<double>(p.z) - q.z;
                const double distance = x * x + y * y + z * z;
                if (distance < best)
                    best = distance, nearest = b;
                return 0;
            });
            if (nearest)
                target->set_guard(match, nearest);
        }
    }
    state_flags |= removed_flag;
    return -2;
}
SpellActor& SpellActor::launch_guardian_beam(Match& match, CreatureActor& guard) {
    auto& result = static_cast<SpellActor&>(match.create(
        MatchObject::spell, [] { return std::make_unique<SpellActor>(); }, &guard,
        [](MatchObject& object) {
            auto& spell = static_cast<SpellActor&>(object);
            spell.effect = Effect::guardian_beam;
            spell.data.reset();
        }));
    result.effect_target = &guard;
    result.run(match, 0);
    return result;
}
int SpellActor::run_guardian_beam(Match& match, int ticks) {
    const auto remove = [&] { state_flags |= removed_flag; return -2; };
    if (!stage) {
        auto* guard = dynamic_cast<CreatureActor*>(effect_target);
        auto* building = guard ? dynamic_cast<BuildingActor*>(guard->ritual_building) : nullptr;
        if (!guard || !(guard->kind_flags & 4) || !building || !(building->kind_flags & 16))
            return remove();
        owner = guard;
        effect_target = building;
        beam.enabled = true;
        beam.texture = FourCC::from_text("vtx1");
        if (data)
            beam.texture.value = data->integer(FourCC::from_text("ttex"), beam.texture.value);
        guard->name_reference(FourCC::from_text("grdn"), this);
        stage = 1;
    }
    auto* guard = dynamic_cast<CreatureActor*>(owner);
    if (stage == 1) {
        emission = std::min(1.0f, emission + ticks * .01f);
        if (!guard || !effect_target || guard->ritual_building != effect_target)
            stage = -1;
    } else {
        if (stage == -1) {
            if (guard)
                guard->unname(FourCC::from_text("grdn"), this);
            stage = -2;
        }
        age += ticks * .075f;
        if (age >= emission - .05f)
            return remove();
    }
    if (auto* building = dynamic_cast<BuildingActor*>(effect_target))
        point = building->effect_point();
    if (guard) {
        const auto& m = guard->motion;
        motion.position = {m.position.x + m.reference_center.x, m.position.y + m.reference_center.y,
                           m.position.z + m.reference_center.z};
    }
    const double x = static_cast<double>(point.x) - motion.position.x,
                 y = static_cast<double>(point.y) - motion.position.y,
                 z = static_cast<double>(point.z) - motion.position.z;
    const auto frames = std::max(match.texture_frames ? match.texture_frames(beam.texture) : 1u, 1u);
    LightningPrimitive::Parameters p;
    p.source = motion.position;
    p.target = point;
    p.step = .033333335f;
    p.deviation = static_cast<float>(std::clamp(std::sqrt(x * x + y * y + z * z) * .02, .1, 2.0));
    p.start = age;
    p.end = emission;
    p.width = static_cast<float>(std::abs(std::sin(beam.frame * 3.14159 / frames)) * .3 + .8);
    beam.frame += ticks * .5f;
    if (beam.frame >= frames)
        beam.frame = 0;
    beam.primitive.set_params(p, match.visual_random);
    return 1;
}
} // namespace sacrifice
