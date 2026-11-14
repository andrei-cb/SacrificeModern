#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include "sacrifice/spell_registry.hpp"
#include <algorithm>
#include <cmath>
#include <format>

namespace sacrifice {
SpellActor& SpellActor::launch_structure(Match& match, CreatureActor& actor) {
    auto& spell = static_cast<SpellActor&>(match.create(
        MatchObject::spell, [] { return std::make_unique<SpellActor>(); }, &actor,
        [](MatchObject& object) {
            auto& spell = static_cast<SpellActor&>(object);
            spell.effect = Effect::structure;
            spell.data.reset();
        }));
    spell.structure.definition = actor.casting.definition;
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
// 0x43c370: reserves the mana fountain, creates the god's structure there, and reveals it with the
// caster's progress. The cast timer is held at its total until the building exists.
int SpellActor::run_structure(Match& match, int ticks) {
    auto* caster = dynamic_cast<CreatureActor*>(owner);
    auto& s = structure;
    const int entry_stage = stage;
    struct TraceStage {
        SpellActor& spell;
        const int& stage;
        int entry;
        ~TraceStage() {
            if (trace_ai() && stage != entry)
                log(std::format("Structure {} side {} stage {} -> {}", spell.structure.definition.text(),
                                spell.allegiance ? spell.allegiance->identity : 0u, entry, stage));
        }
    } trace_stage{*this, stage, entry_stage};
    if (stage > 0) {
        spell_exclusion_field(match, motion.position, 10, 15, std::bit_cast<float>(0x3DAAAAABu), 3, 5);
        if (dynamic_cast<BuildingActor*>(effect_target) && caster) {
            const auto [state, alpha] = caster->cast_state();
            s.progress = alpha;
            if (state == 16)
                stage = -1;
            else if (state == 0x20000000)
                stage = 112;
            if (s.progress > 0.999f)
                s.progress = 0.999f;
        } else
            stage = -1;
    }
    bool hold = false;
    switch (stage) {
    case -16:
        if (!live_particles) {
            state_flags |= removed_flag;
            return -2;
        }
        break;
    case -1:
        if (caster)
            caster->fsm().event(7);
        s.parameter = 0;
        stage = -2;
        [[fallthrough]];
    case -2:
        if (s.building)
            s.building->destroy(match);
        else
            stage = -16;
        break;
    case 0: {
        if (effect_target)
            if (const auto* target = effect_target->source_motion())
                motion.position = target->position;
        motion.orientation = motion_yaw_pitch(0, 1.5707951f);
        s.time = 0;
        s.parameter = 0;
        s.god = caster && caster->progression.stats[18] ? caster->progression.stats[18] : 1;
        visual_requests.push_back({VisualRequest::Kind::structure_cast});
        particle_material(match, particles(match), 0x1a012008, "brng");
        launch_god_cast(match, *this, s.god, 3); // 0x434ab0: hand sparks and the caster's column.
        stage = 1;
        break;
    }
    case 1:
        if (auto* site = dynamic_cast<BuildingActor*>(effect_target)) {
            auto* reserved = site->reserve(this, 1);
            if (reserved && reserved != this)
                stage = -1;
            else {
                if (reserved == this)
                    stage = 2;
                hold = true;
            }
        }
        break;
    case 2:
        stage = 16;
        hold = true;
        break;
    case 16: {
        stage = 17;
        const auto* definition =
            match.spell_registry ? match.spell_registry->find(s.definition) : nullptr;
        auto* site = dynamic_cast<BuildingActor*>(effect_target);
        if (definition && site && match.create_building) {
            const int god = s.god <= 1 ? 0 : s.god >= 5 ? 4 : s.god - 1;
            s.building = match.create_building(definition->structures[god], site->motion.position, 0, 0,
                                               0x60000, caster, site);
            if (s.building)
                stage = 32;
        }
        hold = true;
        break;
    }
    case 17:
        hold = true;
        break;
    case 32:
        stage = 33;
        break;
    case 112:
        s.progress = 1;
        s.parameter = 0;
        stage = -16;
        break;
    default:
        break;
    }
    if (hold && caster)
        caster->casting.remaining = caster->casting.total;
    if (auto* building = s.building) {
        const auto& lo = building->motion.box_min;
        const auto& hi = building->motion.box_max;
        if (!s.configured) {
            const double dx = static_cast<double>(hi.x) - lo.x, dy = static_cast<double>(hi.y) - lo.y;
            if (std::sqrt(dx * dx + dy * dy) * 0.5 > 0) {
                visual_requests.push_back({VisualRequest::Kind::structure_particles});
                // 0x43c370 passes the building's XY half diagonal to preset 134. Its separate
                // 0x44ca80 scale is for the god particles, not the blue construction ring.
                auto& system = particles(match);
                const float scale = static_cast<float>(std::sqrt(dx * dx + dy * dy) * 0.5),
                            one = 1;
                system.set_property(134, &scale, &one);
                system.set_frame({motion.orientation, motion.position});
                const float end_low = 0.1f, end_high = 0.25f;
                system.set_property(6, &end_low, &end_high);
                const float box_low[3]{0, 0, -1}, box_high[3]{0, 0, 1};
                system.set_property(2, box_low, box_high);
                s.parameter = 0.3f;
                system.advance(s.time, s.parameter);
                s.configured = true;
            }
        }
        motion.position.z = static_cast<float>((static_cast<double>(hi.z) - lo.z) * s.progress +
                                               building->motion.position.z + lo.z);
        if (!particle_systems.empty())
            particle_systems.front().set_position(motion.position);
        building->reveal = s.progress; // 0x4651c0.
    }
    s.time += static_cast<float>(ticks);
    if (!particle_systems.empty())
        particle_systems.front().advance(s.time, s.parameter);
    return 1;
}
} // namespace sacrifice
