#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_data.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
int add(int a, int b) {
    return std::bit_cast<int>(static_cast<unsigned>(a) + static_cast<unsigned>(b));
}
int integer(double value) {
    if (!std::isfinite(value) || value >= 9223372036854775808.0 || value < -9223372036854775808.0)
        return 0;
    return std::bit_cast<int>(static_cast<unsigned>(static_cast<std::int64_t>(value)));
}
double distance(const Vec3& a, const Vec3& b) {
    const double dx = static_cast<double>(a.x) - b.x, dy = static_cast<double>(a.y) - b.y,
                 dz = static_cast<double>(a.z) - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
bool allied(const MatchSide& a, const MatchSide& b) {
    return (a.allies & b.identity) || (a.identity & b.allies) || a.identity == b.identity;
}
} // namespace
// ManahoarManaRun 0x491730: the hoar names itself as the context target (+476) and starts the periodic
// 0x4911c0 object, then clears its own handler.
SpellActor& SpellActor::launch_mana_relay(Match& match, CreatureActor& hoar, std::shared_ptr<const SpellData> data) {
    auto& object = match.create(
        MatchObject::spell, [] { return std::make_unique<SpellActor>(); }, &hoar,
        [&](MatchObject& object) {
            auto* spell = dynamic_cast<SpellActor*>(&object);
            if (!spell)
                throw Error("Spell pool object has an incompatible host type");
            spell->effect = Effect::mana_relay;
            spell->data = std::move(data);
        });
    auto& spell = static_cast<SpellActor&>(object);
    spell.owner = &hoar; // +460, taken from +476 on the first run.
    spell.motion.position = hoar.motion.position;
    spell.object_flags |= 0x10000;
    if (hoar.allegiance)
        hoar.allegiance->attach(spell);
    return spell;
}
// 0x4911c0: every 16 ticks the manalith supply around the hoar (0x490f80) is shared between the own
// side's units in range by their missing mana (0x4910c0), with side-coloured particles and light.
int SpellActor::run_mana_relay(Match& match, int ticks) {
    auto* hoar = dynamic_cast<CreatureActor*>(owner);
    if (!hoar) {
        state_flags |= removed_flag;
        return -2;
    }
    auto& system = particles(match);
    if (!relay.initialized) {
        relay.initialized = true;
        particle_material(match, system, 0x1a012009, "fb_g");
        const float preset[3]{0.35f, 2, 1};
        system.set_property(128, preset, preset);
        const float low[3]{-0.15f, -0.15f, 0}, high[3]{0.15f, 0.15f, 0};
        system.set_property(2, low, high);
    }
    // The hoar's side colour (light, and particles at least 64 per channel) follows side changes (+196).
    if (const auto color = hoar->allegiance ? hoar->allegiance->definition.color : 0xffffffu;
        !light.outer || color != relay.color) {
        relay.color = color;
        light.color = {static_cast<float>((color >> 16 & 255) * 0.0039215689),
                       static_cast<float>((color >> 8 & 255) * 0.0039215689),
                       static_cast<float>((color & 255) * 0.0039215689)};
        light.inner = 1;
        light.outer = 15;
        const float tint[3]{std::max(light.color.x * 255.0f, 64.0f), std::max(light.color.y * 255.0f, 64.0f),
                            std::max(light.color.z * 255.0f, 64.0f)};
        system.set_property(12, tint, tint);
    }
    motion.position = hoar->motion.position;
    relay.ticks = add(relay.ticks, ticks);
    if (!hoar->alive()) {
        relay.ticks = 0;
        relay.supply = 0;
    } else if (relay.ticks >= 16) {
        int supply = 0;
        const auto* own = hoar->allegiance;
        const auto at = hoar->motion.position;
        for (auto* side : match.side_order) {
            if (!own || !allied(*side, *own))
                continue;
            for (auto* object : side->objects[2]) {
                auto* building = dynamic_cast<BuildingActor*>(object);
                const auto flags = building ? building->building_flags : 0;
                // Vtable +112 (the per-object acceptance without 0x800) is taken as passing for allied sides.
                if (!building || building->primary || !(flags & 0x4000) || building->reserved ||
                    building->reveal != 1 || !(flags & 0xc00) || !building->mana_rate)
                    continue;
                const double d = distance(building->motion.position, at);
                const double weight = d < 50 ? d * 0.02 : d < 1000 ? (1000 - d) * 0.0010526315 : 0;
                supply = integer(static_cast<double>(building->mana_rate) * weight * 0.6 + supply);
            }
        }
        relay.supply = 0;
        if (supply > 0 && own) {
            std::vector<CreatureActor*> recipients;
            int missing = 0;
            const double range = data ? data->range : 0;
            for (const unsigned group : {0u, 1u})
                for (auto* object : own->objects[group]) {
                    auto* unit = dynamic_cast<CreatureActor*>(object);
                    if (!unit || !(unit->kind_flags & 5) || distance(unit->motion.position, at) >= range)
                        continue;
                    const int need = unit->maximum_mana - unit->mana;
                    if (need <= 0)
                        continue;
                    recipients.push_back(unit);
                    missing = add(missing, need);
                }
            relay.supply = static_cast<float>(supply);
            if (missing)
                for (auto* unit : recipients)
                    unit->change_mana_fixed(integer(static_cast<double>(unit->maximum_mana - unit->mana) / missing *
                                                    relay.ticks * relay.supply));
        }
        relay.ticks = 0;
    }
    relay.beam = std::clamp(relay.supply * 0.0000274658203125f, 0.0f, 1.0f); // a1[7].
    // 0x44e660(hoar, 1): the emitter and light follow the hoar's reference point 1.
    Vec3 anchor = hoar->motion.position;
    hoar->write_reference_point(anchor, 1, false);
    motion.position = anchor;
    const float position[3]{anchor.x, anchor.y, anchor.z};
    system.set_property(0, position, nullptr);
    light.position = anchor;
    if (relay.shown != relay.beam) {
        if (relay.shown == 0)
            light.enabled = true; // 'mama' is named on the hoar here; its consumer is not recovered.
        relay.shown = relay.shown < relay.beam ? std::min(relay.shown + 0.01f, relay.beam)
                                               : std::max(relay.shown - 0.01f, relay.beam);
        light.intensity = relay.shown * 0.5f;
        if (relay.shown == 0)
            light.enabled = false;
    }
    relay.time += static_cast<float>(ticks);
    system.advance(relay.time, relay.beam);
    return 1;
}
} // namespace sacrifice
