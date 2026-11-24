#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/spatial.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_registry.hpp"
#include <cmath>

namespace sacrifice {
bool CreatureActor::find_summon_point(Match& match, Vec3& point) const {
    return find_creature_point(match, point, motion.position, motion.orientation);
}
bool find_creature_point(Match& match, Vec3& point, Vec3 centre, const Orientation& orientation,
                         bool try_centre) {
    if (!match.terrain || !match.spatial)
        throw Error("Summon placement requires terrain and spatial services");
    const auto free = [&](unsigned mask) {
        const double height = terrain_surface(*match.terrain, point.x, point.y);
        point.z = static_cast<float>(height);
        return height > 0 && !match.spatial->occupied(point, mask);
    };
    if (try_centre) {
        point = centre;
        if (free(31))
            return true;
    }
    // Native ordered offsets at 0x4cf8e8, transformed by the wizard's current matrix.
    static constexpr std::array<std::array<float, 2>, 25> offsets{
        {{0, 6},   {-3, 6},  {3, 6},   {-2, 3},  {2, 3},   {-5, 3}, {5, 3},  {-2, 9}, {2, 9},
         {5, 9},   {-5, 9},  {-8, 12}, {-5, 12}, {-2, 12}, {2, 12}, {5, 12}, {8, 12}, {-11, 15},
         {-8, 15}, {-5, 15}, {-2, 15}, {2, 15},  {5, 15},  {8, 15}, {11, 15}}};
    const auto& o = orientation;
    for (const auto& offset : offsets) {
        point = {static_cast<float>(static_cast<double>(offset[0]) * o[0] +
                                    static_cast<double>(offset[1]) * o[3] + centre.x),
                 static_cast<float>(static_cast<double>(offset[0]) * o[1] +
                                    static_cast<double>(offset[1]) * o[4] + centre.y),
                 static_cast<float>(static_cast<double>(offset[0]) * o[2] +
                                    static_cast<double>(offset[1]) * o[5] + centre.z)};
        if (free(31))
            return true;
    }
    // 0x453340 rechecks the last point with mask 21 before drawing random alternatives.
    return open_point(match, point, centre, 20, 21);
}
void CreatureActor::change_souls(int delta) {
    soul_count = std::bit_cast<int>(static_cast<unsigned>(soul_count) + static_cast<unsigned>(delta));
    if (allegiance)
        allegiance->change_souls(delta);
}
void CreatureActor::follow_and_select(Match& match) {
    // 0x45d300: follow the controlling wizard and, for the local player's wizard, select and
    // place the creature in group 0.
    issue_order({5, controller});
    if (controller && match.select_interface)
        match.select_interface(*this);
}
bool CreatureActor::summon_creature(Match& match, FourCC id, Vec3 point) {
    if (!network_identity)
        return false;
    const auto* definition = match.spell_registry ? match.spell_registry->find(id) : nullptr;
    if (!definition || definition->kind != 2)
        throw Error("Summon release has no creature definition");
    if (soul_count < definition->required_souls)
        return false;
    if (!match.create_creature)
        throw Error("Summon release requires a creature factory");
    change_souls(std::bit_cast<int>(0u - static_cast<unsigned>(definition->required_souls)));
    if (auto* actor = match.create_creature(*this, id, point, this))
        actor->follow_and_select(match);
    return true; // 0x481900 reports success after payment even if the factory returns null.
}
SpellActor& SpellActor::launch_summon(Match& match, CreatureActor& actor) {
    auto& spell = static_cast<SpellActor&>(match.create(
        MatchObject::spell, [] { return std::make_unique<SpellActor>(); }, &actor,
        [](MatchObject& object) {
            auto& spell = static_cast<SpellActor&>(object);
            spell.effect = Effect::summon;
            spell.data.reset();
        }));
    spell.summon_id = actor.casting.definition;
    spell.owner = actor.owner;
    spell.effect_target = actor.effect_target;
    spell.point = actor.casting.point;
    spell.motion.position = spell.point;
    spell.motion.orientation = actor.motion.orientation;
    spell.motion.yaw = actor.motion.yaw;
    spell.object_flags |= 0x10000;
    if (actor.allegiance)
        actor.allegiance->attach(spell);
    spell.run(match, 0);
    return spell;
}
int SpellActor::run_summon(Match& match, int ticks) {
    const auto remove = [&] {
        state_flags |= removed_flag;
        return -2;
    };
    switch (stage) {
    case 0:
        if (!match.prepare_creature || !match.prepare_creature(summon_id))
            return remove();
        if (match.create_summon_preview)
            match.create_summon_preview(*this);
        if (const auto* definition =
                match.creature_definition ? match.creature_definition(summon_id) : nullptr)
            summon_god = definition->stats[18];
        else
            summon_god = 0;
        if (summon_god < 1 || summon_god > 5) {
            const auto* caster = dynamic_cast<CreatureActor*>(owner);
            summon_god = caster ? caster->stats.current[18] : 1;
        }
        if (summon_god < 1 || summon_god > 5)
            summon_god = 1;
        launch_god_cast(match, *this, summon_god, 7);
        visual_requests.push_back({VisualRequest::Kind::summon_model});
        visual_requests.push_back({VisualRequest::Kind::summon_effects});
        cast_alpha = 0;
        stage = 1;
        return 0;
    case 1:
        if (auto* caster = dynamic_cast<CreatureActor*>(owner); caster && (caster->kind_flags & 1)) {
            const auto [state, alpha] = caster->cast_state();
            cast_alpha = alpha;
            if (state)
                stage = state == 0x20000000 ? 16 : 240;
        } else
            stage = 240;
        break;
    case 16:
        if (auto* caster = dynamic_cast<CreatureActor*>(owner)) {
            if (caster->network_identity)
                stage = caster->summon_creature(match, summon_id, motion.position) ? 32 : 240;
        } else
            stage = 240;
        break;
    case 32:
        return remove();
    case 240:
        cast_alpha = static_cast<float>(cast_alpha - ticks * 0.01);
        if (cast_alpha <= 0)
            return remove();
        break;
    default:
        break;
    }
    if (match.terrain)
        motion.position.z =
            static_cast<float>(terrain_surface(*match.terrain, motion.position.x, motion.position.y));
    return 1;
}
} // namespace sacrifice
