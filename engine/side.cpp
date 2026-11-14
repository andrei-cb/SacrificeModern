#include "sacrifice/side.hpp"
#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/fog.hpp"
#include "sacrifice/match.hpp"
#include "sacrifice/side_ai.hpp"
#include <algorithm>

namespace sacrifice {
std::vector<SideDefinition> SideDefinition::decode(ByteView bytes) {
    Reader r(bytes, "SIDS");
    const auto count = std::bit_cast<std::int32_t>(r.u32());
    if (count <= 0)
        return {};
    if (r.remaining() < static_cast<std::uint64_t>(count) * 20)
        throw Error("Invalid SIDS record count/size");
    std::vector<SideDefinition> result;
    for (int i = 0; i < count; ++i) {
        SideDefinition definition{r.u32(), r.u32(), r.u32(), r.u32(), r.u32()};
        if (definition.index >= 32)
            throw Error("Invalid SIDS side index");
        result.push_back(definition);
    }
    return result;
}
MatchSide::MatchSide(SideDefinition value) : definition(value), enemies(value.enemies), flags(value.flags) {
    if (value.index >= 32)
        throw Error("Invalid runtime side index");
    identity = 1u << value.index;
    allies = value.allies | identity;
}
MatchSide::~MatchSide() = default;
void MatchSide::event(MatchObject& target, MatchObject* source, int code, int value) {
    if (event_handler)
        event_handler(target, source, code, value);
    if (code == 7)
        change_souls(value);
    if (code == 8)
        summoned_creatures = std::bit_cast<int>(static_cast<unsigned>(summoned_creatures) + 1u);
    // 0x472920: a manalith is a primary part with all of flags 0x55a0, a shrine has flag 2 without 1.
    const auto structure = [](const MatchObject* object) {
        const auto* building = dynamic_cast<const BuildingActor*>(object);
        if (!building)
            return 0;
        if (!building->primary && (building->building_flags & 0x55a0) == 0x55a0)
            return 1;
        return (building->building_flags & 3) == 2 ? 2 : 0;
    };
    if (code == 9 && value > 0) {
        souls_harvested += value;
        flags |= 0x10000;
    } else if (code == 24)
        ++banishments;
    else if (code == 14 && (target.kind_flags & 0x10)) {
        const auto kind = structure(&target);
        manaliths_created += kind == 1;
        shrines_created += kind == 2;
        if (kind)
            flags |= 0x10000;
    } else if (code == 25) { // The destroying side; `source` is the building.
        const auto kind = structure(source);
        manaliths_destroyed += kind == 1;
        shrines_destroyed += kind == 2;
    } else if (code == 16 && source && source->allegiance)
        source->allegiance->event(*source, &target, 25);
    // 0x472b80: events touching the local side raise the battle music; friendly 4/10/15 are ignored.
    // Code 1 (a seen creature with a cast word, 5 when this is the local side) awaits its vtable +80 test.
    if (match && match->local_side &&
        !((code == 4 || code == 10 || code == 15) && source && source->allegiance == target.allegiance) &&
        (target.allegiance == match->local_side || (source && source->allegiance == match->local_side))) {
        const float weight = code == 4 || code == 5    ? 3.0f
                             : code == 6 || code == 13 ? 5.0f
                             : code == 10 || code == 15 || code == 23 ? 1.0f
                             : code == 12                 ? 12.0f
                             : code == 16                 ? 6.0f
                                                          : 0.0f;
        if (weight != 0)
            match->music(1, weight);
    }
    if (!source || !source->allegiance)
        return;
    if (code == 3)
        if (auto* actor = dynamic_cast<CreatureActor*>(&target))
            actor->friendly_exception_mask |= source->allegiance->identity;
    if (code == 3 || code == 4 || code == 5 || code == 10 || code == 15 || code == 16)
        incoming_mask |= source->allegiance->identity;
}
void MatchSide::tally() {
    int kills = 0, level = 0;
    for (auto* object : objects[1])
        if (const auto* wizard = dynamic_cast<const CreatureActor*>(object)) {
            kills += wizard->credit_count_80 + wizard->credit_count_20;
            level = wizard->progression.level; // The last listed wizard's +4900.
        }
    peak_kills = std::max(peak_kills, kills);
    peak_level = std::max(peak_level, level);
}
void MatchSide::change_souls(int delta) {
    soul_count = std::bit_cast<int>(static_cast<unsigned>(soul_count) + static_cast<unsigned>(delta));
    flags |= 0x10000;
}
void MatchSide::see(MatchObject& object) {
    const auto* creature = dynamic_cast<const CreatureActor*>(&object);
    // Vtable +56 / +64; the 0x453b00 named-reference exception is not reconstructed.
    if (creature && (!creature->alive() || (creature->fsm().state() && (creature->fsm().state()->flags & 0x4000))))
        return;
    int value = stamp;
    if (object.kind_flags & 0x12)
        value += 1000000;
    else if (object.allegiance == this && !(object.state_flags & 0x2000))
        return;
    auto it = std::ranges::find_if(known, [&](const Known& k) { return k.object == &object; });
    if (it == known.end())
        it = known.insert(known.begin(), {&object, object.reference(), value});
    it->reference = object.reference();
    it->stamp = value;
    object.seen_by |= identity;
    object.hidden_from &= ~identity;
    // First-sighting statistics (0x472920 codes 1 and 2) await the statistics host.
}
void MatchSide::schedule(MatchObject& object, int delay) {
    work.remove(&object);
    if (object.allegiance != this || !(object.kind_flags & 5)) // Vtable +92 holds for creatures.
        return;
    int value = delay;
    const int capped = std::min(delay, 16);
    if (capped >= object.countdown)
        value = object.countdown;
    else if (delay >= 16)
        value = 16;
    object.countdown = value;
    auto it = work.begin();
    while (it != work.end() && (*it)->countdown <= value)
        ++it;
    work.insert(it, &object);
}
void MatchSide::tick(Match& match, int ticks) {
    if (!fog && this == match.local_side && match.terrain)
        fog = std::make_unique<FogOfWar>(match.terrain->width, match.terrain->height);
    if (flags & 0x10000) { // 0x471fb0; 0x472080's counts are kept elsewhere by the port.
        tally();
        flags &= ~0x10000u;
    }
    period -= ticks;
    for (; period < 0; ++stamp) { // 0x466cc0 every 32 ticks.
        period += 32;
        if (fog)
            fog->age();
        const int oldest = stamp - 80;
        std::erase_if(known, [&](const Known& k) {
            const bool stale = k.stamp < oldest || match.resolve(k.reference) != k.object;
            if (stale && match.resolve(k.reference) == k.object) {
                k.object->seen_by &= ~identity;
                k.object->hidden_from &= ~identity;
            } else if (!stale && k.stamp - oldest < 72)
                // 0x4939e0(clock - 80, identity, 72): sightings older than 8 periods are marked hidden so
                // the next sight scan (0x4724e0) refreshes them; otherwise a unit in plain view would keep
                // its first stamp and lose its overhead arrow 12 periods later.
                k.object->hidden_from |= identity;
            return stale;
        });
    }
    if (controller)
        controller->tick(ticks);
    // Orders changed through 0x46de40 reschedule their creature's side work at -15.
    for (auto& group : objects)
        for (auto* object : group)
            if (auto* creature = dynamic_cast<CreatureActor*>(object)) {
                bool urgent = false;
                for (const auto& request : creature->order_requests)
                    urgent |= request.schedule;
                creature->order_requests.clear();
                if (urgent)
                    schedule(*object, -15);
            }
    // 0x473130: run due heads (vtable +88), each returning to the tail with countdown 16.
    if (work.empty())
        return;
    auto* tail = work.back();
    int budget = ticks;
    while (!work.empty() && budget) {
        auto* object = work.front();
        if (object->countdown >= 0)
            break;
        if (auto* creature = dynamic_cast<CreatureActor*>(object); creature && creature->think(match))
            --budget;
        object->countdown = 16;
        if (object->allegiance == this && object != tail && !work.empty() && work.front() == object) {
            work.pop_front();
            work.push_back(object);
            tail = object;
        } else if (!work.empty() && work.front() == object)
            break;
    }
}
MatchObject* MatchSide::wizard() const {
    return objects[1].empty() ? nullptr : objects[1].front();
}
void MatchSide::attach(MatchObject& object) {
    if (object.allegiance == this) {
        object.controller = wizard();
        return;
    }
    if (object.allegiance)
        object.allegiance->detach(object);
    if (const auto* creature = dynamic_cast<const CreatureActor*>(&object))
        change_souls(creature->soul_count); // Creature side assignment 0x469540, before group linking.
    object.allegiance = this;
    objects[MatchObject::group(object.type)].push_front(&object);
    flags |= 0x10000;
    object.controller = wizard(); // 0x4694f0 obtains +324 after linking the object.
    schedule(object, object.countdown); // 0x4932f0 queues side work with the object's countdown.
    event(object, nullptr, 18); // 0x4723a0: notify the controller that its side roster has changed.
}
void MatchSide::detach(MatchObject& object) {
    if (object.allegiance != this)
        throw Error("Removing object from a different side");
    if (controller)
        controller->drop(object); // 0x472340 drops the old side's AI record before clearing allegiance.
    if (const auto* creature = dynamic_cast<const CreatureActor*>(&object))
        change_souls(std::bit_cast<int>(0u - static_cast<unsigned>(creature->soul_count)));
    objects[MatchObject::group(object.type)].remove(&object);
    work.remove(&object);
    if (object.allegiance == this)
        object.allegiance = nullptr;
    flags |= 0x10000;
}
void MatchSide::normalize(const MatchSides& sides) {
    for (int i = 0; i < 32; ++i) {
        auto* other = sides.get(i);
        if (!other || other->definition.index != static_cast<unsigned>(i)) {
            allies &= ~(1u << i);
            enemies &= ~(1u << i);
            continue;
        }
        if (allies & other->identity) {
            allies |= other->identity;
            other->allies |= identity;
            enemies &= ~other->identity;
            other->enemies &= ~identity;
        } else {
            allies &= ~other->identity;
            other->allies &= ~identity;
        }
        if (enemies & other->identity) {
            enemies |= other->identity;
            other->enemies |= identity;
            allies &= ~other->identity;
            other->allies &= ~identity;
        } else {
            enemies &= ~other->identity;
            other->enemies &= ~identity;
        }
    }
}
int MatchSide::end_game(Match& match, int result, int mode) {
    if (outcome)
        return outcome;
    const int requested = result;
    if (mode == 1 && result < 0) // Offline sides keep network slot -1 (+352), so this branch always runs.
        result = -1;
    if (result != 1 && result != -1)
        throw Error("SIDE::EndGame must have 1 or -1 as parameter");
    outcome = result;
    tally();
    bool wizard_lost = false;
    if (result == -1) {
        if (auto* own = wizard()) { // +168, the side's wizard count from 0x472080.
            own->state_flags |= 0x40000;
            wizard_lost = true;
        } else {
            // 0x471dc0; its side event 19 for the controller awaits that host.
            for (auto* other : match.side_order) {
                allies &= ~other->identity;
                other->allies &= ~identity;
                enemies &= ~other->identity;
                other->enemies &= ~identity;
            }
            for (unsigned group : {0u, 1u, 2u}) { // Groups whose masks meet 0x15.
                const std::vector<MatchObject*> members(objects[group].begin(), objects[group].end());
                for (auto* object : members) {
                    match.notify_reference(*object, 14);
                    if (auto* actor = dynamic_cast<CreatureActor*>(object); actor && (object->kind_flags & 5))
                        actor->orders.clear();
                }
            }
        }
    } else {
        for (auto* other : match.side_order)
            if (other != this && ((other->enemies & identity) || (other->identity & enemies)) &&
                other->identity != identity)
                other->end_game(match, -5, 1);
    }
    for (unsigned group : {0u, 1u, 2u}) {
        const std::vector<MatchObject*> members(objects[group].begin(), objects[group].end());
        for (auto* object : members) {
            if (requested == 1)
                object->common_status |= 7;
            if (auto* actor = dynamic_cast<CreatureActor*>(object); actor && (object->kind_flags & 5)) {
                actor->orders.clear();
                if (wizard_lost)
                    actor->state_flags |= 0x40000;
            }
        }
    }
    controller.reset(); // 0x471f90.
    return outcome;
}
MatchSides::MatchSides(std::span<const SideDefinition> definitions) {
    for (auto definition : definitions) {
        if (definition.index >= 32)
            throw Error("Invalid runtime side index");
        auto side = std::make_unique<MatchSide>(definition);
        table_[definition.index] = side.get();
        instances_.push_back(std::move(side));
    }
    for (auto& side : table_)
        if (!side)
            side = table_[31];
}
MatchSide* MatchSides::get(int index) const {
    return table_[std::clamp(index, 0, 31)];
}
} // namespace sacrifice
