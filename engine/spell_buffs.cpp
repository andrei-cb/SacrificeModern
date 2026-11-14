#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/spatial.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_data.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
constexpr FourCC speedup_name{1397769540u}; // 'SPED' on the target while the effect runs.

// 0x453340: keep the point when it is on land and free for `mask`, otherwise up to 32 random points within
// `radius` of `centre` (the shared rand(), as in summon placement).
bool place(Match& match, Vec3& point, Vec3 centre, float radius, unsigned mask) {
    if (!match.terrain || !match.spatial)
        throw Error("Spell placement requires terrain and spatial services");
    const auto free = [&] {
        const double height = terrain_surface(*match.terrain, point.x, point.y);
        point.z = static_cast<float>(height);
        return height > 0 && !match.spatial->occupied(point, mask);
    };
    int remaining = 32;
    while (!free()) {
        if (remaining-- == 0)
            break;
        const float angle =
            static_cast<float>(match.random().next() * 6.2831802 * 0.000030517578125 - 3.141590118408203);
        const double distance = match.random().next() * static_cast<double>(radius) * 0.000030517578125;
        point.x = static_cast<float>(std::sin(static_cast<double>(angle)) * distance + centre.x);
        point.y = static_cast<float>(std::cos(static_cast<double>(angle)) * distance + centre.y);
    }
    return remaining > 0;
}
Vec3 centre_of(const MatchObject& object) { // 0x44e020.
    const auto* source = object.source_motion();
    if (!source)
        return {};
    return {source->position.x + source->reference_center.x, source->position.y + source->reference_center.y,
            source->position.z + source->reference_center.z};
}
double delta(float a, float b) {
    return static_cast<double>(a) - b;
}
std::int16_t scaled(std::int16_t value, double factor) {
    const double result = factor * value;
    return static_cast<std::int16_t>(static_cast<std::int64_t>(result <= 0 ? 0 : result));
}
} // namespace
bool open_point(Match& match, Vec3& point, Vec3 centre, float radius, unsigned mask) {
    return place(match, point, centre, radius, mask);
}

SpellActor& SpellActor::launch_spell(Match& match, CreatureActor& actor, Effect effect) {
    auto& object = match.create(
        MatchObject::spell, [] { return std::make_unique<SpellActor>(); }, &actor,
        [&](MatchObject& object) {
            auto* spell = dynamic_cast<SpellActor*>(&object);
            if (!spell)
                throw Error("Spell pool object has an incompatible host type");
            spell->effect = effect;
            spell->data = actor.casting.data;
        });
    auto& spell = static_cast<SpellActor&>(object);
    spell.spell_id = actor.casting.definition;
    spell.owner = actor.owner;
    spell.effect_target = actor.effect_target;
    spell.point = actor.casting.point;
    spell.motion.position = actor.motion.position;
    spell.motion.yaw = actor.motion.yaw;
    spell.motion.orientation = actor.motion.orientation;
    spell.object_flags |= 0x10000;
    if (actor.allegiance)
        actor.allegiance->attach(spell);
    spell.run(match, 0);
    return spell;
}

// SpeedupStart (0x441870) configures the green light, the glows over the target's box and the position trail,
// then SpeedupRun (0x441600) multiplies the target's movement stats every tick while the duration lasts.
int SpellActor::run_speedup(Match& match, int ticks) {
    if (!data)
        throw Error("SpeedupStart requires a spell definition");
    auto& p = particles(match);
    auto& s = speedup;
    if (!stage) {
        if (!effect_target) {
            state_flags |= removed_flag;
            return -2;
        }
        const int count = std::clamp(data->integer(FourCC::from_text("inum"), 4), 1, 4);
        const int gap = std::clamp(data->integer(FourCC::from_text("igap"), 3), 1, 3);
        s.factor = data->real(FourCC::from_text("fmul"), 2) - 1;
        light = {true, motion.position, {0, 1, 0}, 0, 1, 25}; // 0x44ca20(unk_4CB910, 0, 1, 25).
        particle_material(match, p, 0x1a012008,
                          FourCC{static_cast<std::uint32_t>(
                              data->integer(FourCC::from_text("ttex"), FourCC::from_text("glo2").value))});
        const float size = .5f, life = 1.5f;
        p.set_property(129, &size, &life);
        const auto& box = *effect_target->source_motion();
        const float low[3]{box.box_min.x * .5f, box.box_min.y * .5f, box.box_min.z},
            high[3]{box.box_max.x * .5f, box.box_max.y * .5f, box.box_max.z};
        p.set_property(2, low, high);
        host.time = 0;
        host.rate = .5f;
        p.advance(host.time, host.rate);
        motion.orientation = box.orientation;
        motion.position = box.position;
        p.set_position(motion.position, &motion.position);
        // A 'sped' definition derives its time from the target's speed; the stock 'spup' lasts 60 x duration.
        s.duration = static_cast<int>(static_cast<std::int64_t>(60.0 * data->duration));
        sound_event(match, 2);
        s.trail.assign(static_cast<std::size_t>(gap * (count + 1)), box.position);
        s.head = 0;
        if (auto* target = dynamic_cast<CreatureActor*>(effect_target))
            for (int i = 0; i < count; ++i) {
                auto& child = static_cast<SpellActor&>(match.create(
                    MatchObject::spell, [] { return std::make_unique<SpellActor>(); }, this,
                    [&](MatchObject& object) {
                        auto& echo = static_cast<SpellActor&>(object);
                        echo.effect = Effect::speedup_echo;
                        echo.data = data;
                    }));
                child.effect_target = this;
                child.summon_id = target->definition().id;
                child.echo.lag = i * gap;
                child.echo.length = static_cast<unsigned>(s.trail.size());
                child.run(match, 0);
                if (match.create_summon_preview)
                    match.create_summon_preview(child);
            }
        effect_target->name_reference(speedup_name, this);
        stage = 1;
        return 1;
    }
    host.time += static_cast<float>(ticks);
    p.advance(host.time, host.rate);
    if (!effect_target) {
        host.rate = 0;
        light.intensity -= ticks * .01f;
        if (light.intensity <= 0) {
            state_flags |= removed_flag;
            return -2;
        }
        return 1;
    }
    if (light.intensity < 1)
        light.intensity = std::min(light.intensity + ticks * .01f, 1.0f);
    const auto& box = *effect_target->source_motion();
    motion.orientation = box.orientation;
    motion.position = box.position;
    light.position = motion.position;
    s.head = (s.head + 1) % static_cast<unsigned>(s.trail.size());
    s.trail[s.head] = box.position;
    p.set_position(motion.position);
    auto* actor = effect_target->kind_flags & 5 ? dynamic_cast<CreatureActor*>(effect_target) : nullptr;
    s.duration -= ticks;
    if (!actor || s.duration <= 0) {
        sound_event(match, 16);
        effect_target->unname(speedup_name, this);
        effect_target = nullptr;
        return 1;
    }
    actor->stats.changed |= 8;
    actor->stats.target[3] = scaled(actor->stats.target[3], s.factor + 1);
    actor->stats.changed |= 0x10;
    actor->stats.target[4] = scaled(actor->stats.target[4], s.factor * .5 + 1);
    return 1;
}

// TeleportSpell (0x442b80): the destination is 30 units from the target building towards the caster
// (0x442ad0). After release a circle around the caster grows to the spread limit over one second, and each
// own creature or wizard it reaches (0x492300 mask 5) moves by the same offset (0x4429e0), once.
int SpellActor::run_teleport(Match& match, int ticks) {
    if (!data)
        throw Error("TeleportSpell requires a spell definition");
    const auto remove = [&] {
        state_flags |= removed_flag;
        return -2;
    };
    if (!stage) {
        if (!effect_target || !effect_target->source_motion())
            return remove();
        sound_event(match, 1);
        teleport_radius = data->spread_limit;
        age = 0;
        stage = 1;
        const auto target = effect_target->source_motion()->position;
        Vec3 offset{motion.position.x - target.x, motion.position.y - target.y, motion.position.z - target.z};
        const double distance =
            std::sqrt(static_cast<double>(offset.x) * offset.x + static_cast<double>(offset.y) * offset.y +
                      static_cast<double>(offset.z) * offset.z);
        if (distance != 0)
            offset = {static_cast<float>(30.0 / distance * offset.x),
                      static_cast<float>(30.0 / distance * offset.y),
                      static_cast<float>(30.0 / distance * offset.z)};
        point = {offset.x + target.x, offset.y + target.y, offset.z + target.z};
        place(match, point, target, 50, 16);
    }
    if (stage == 1) {
        const auto* caster = owner && (owner->kind_flags & 1) ? dynamic_cast<CreatureActor*>(owner) : nullptr;
        if (!owner)
            stage = 16;
        else if (!caster)
            stage = 240;
        else if (const auto state = caster->cast_state().first)
            stage = state == 0x20000000 ? 16 : 240;
        if (stage == 1)
            return 1;
    }
    if (stage == 16) {
        sound_event(match, 2);
        if (owner)
            motion.position = centre_of(*owner);
        stage = 17;
        launch_teleport_portals(match);
    }
    if (stage == 17) {
        age = std::min(1.0f, static_cast<float>(ticks * 0.01666666666666667 + age));
        const double radius = static_cast<double>(age) * teleport_radius;
        if (allegiance)
            for (unsigned group : {0u, 1u}) { // Native groups whose masks meet 5.
                const std::vector<MatchObject*> members(allegiance->objects[group].begin(),
                                                        allegiance->objects[group].end());
                for (auto* object : members) {
                    const auto* source = object->source_motion();
                    if (!source || !(object->kind_flags & 5))
                        continue;
                    const double x = delta(motion.position.x, source->position.x),
                                 y = delta(motion.position.y, source->position.y),
                                 z = delta(motion.position.z, source->position.z);
                    if (x * x + y * y + z * z < radius * radius)
                        teleport_unit(match, *object);
                }
            }
        if (age != 1)
            return 1;
        stage = 240;
    }
    return remove();
}
void SpellActor::teleport_unit(Match& match, MatchObject& object) {
    auto* actor = dynamic_cast<CreatureActor*>(&object);
    if (!actor || !actor->network_identity ||
        (actor->fsm().state() && (actor->fsm().state()->flags & 0x4000)) || !actor->alive() ||
        std::ranges::any_of(actor->named_references, [&](const auto& entry) { return entry.object == this; }))
        return;
    const auto& from = actor->motion.position;
    Vec3 destination{from.x - motion.position.x + point.x, from.y - motion.position.y + point.y,
                     from.z - motion.position.z + point.z};
    if (place(match, destination, point, data->spread_limit, 21) && actor->relocate(match, destination))
        actor->name_reference(spell_id, this);
}
} // namespace sacrifice
