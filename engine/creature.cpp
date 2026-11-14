#include "sacrifice/creature.hpp"
#include <algorithm>
#include <format>

namespace sacrifice {
namespace {
std::int32_t signed_word(std::uint32_t value) {
    return std::bit_cast<std::int32_t>(value);
}
std::int16_t short_word(int value) {
    return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(value));
}
int wrapped_product(int a, int b) {
    return signed_word(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
}
void nonnegative_ticks(int ticks) {
    if (ticks < 0)
        throw Error("Negative creature tick count");
}
} // namespace
CreatureDefinition CreatureDefinition::decode(ByteView data) {
    Reader r(data, "CRE8/WIZD");
    CreatureDefinition result;
    result.name = {r.u32()};
    result.kind = r.u32();
    if ((result.kind != 2 && result.kind != 8) || data.size() != (result.kind == 2 ? 528u : 840u))
        throw Error("Invalid CRE8/WIZD kind or size");
    result.id = {r.u32()};
    result.icon = {r.u32()};
    r.take(8);
    result.value = static_cast<std::int32_t>(r.u32());
    r.take(12);
    result.rank = static_cast<std::uint16_t>(r.i16()); // Runtime +48: the selection leader (0x461c40).
    r.take(2);
    for (auto& stat : result.stats)
        stat = r.i16();
    for (auto& spell : result.spells)
        spell = {r.u32()};
    result.run_spell = {r.u32()};
    result.melee_damage_type = signed_word(r.u32());
    result.melee_damage = signed_word(r.u32());
    result.mana = signed_word(r.u32());
    result.behaviour_fsm = {r.u32()};
    result.animation_fsm = {r.u32()};
    // 0x44efa0 replaces an absent/bad FSM name with norm.
    if (!result.animation_fsm.value || result.animation_fsm == FourCC::from_text("----"))
        result.animation_fsm = FourCC::from_text("norm");
    result.body = {r.u32()};
    result.flags = r.u32();
    const auto count = r.u32();
    if (count > 4)
        throw Error("CRE8/WIZD sound count exceeds four slots");
    for (unsigned i = 0; i < 4; ++i) {
        const FourCC sound{r.u32()};
        if (i < count)
            result.sounds.push_back(sound);
    }
    for (auto& animation : result.animations)
        animation = {r.u32()};
    for (auto& spell : result.event_spells) {
        spell.event = signed_word(r.u32());
        spell.chance = r.u16();
        spell.flags = r.u16();
        spell.spell = {r.u32()};
    }
    return result;
}
void CreatureStats::reset(const std::array<std::int16_t, 24>& values) {
    current = base = target = values;
    changed = 0;
}
void CreatureStats::advance(int ticks) {
    nonnegative_ticks(ticks);
    changed &= ~0xc0020u; // 0x4cfa6c: health and slots 18/19 do not ease back.
    const auto step = static_cast<std::int64_t>(ticks) * 10;
    for (unsigned i = 0; i < current.size(); ++i) {
        if (!(changed & (1u << i)))
            continue;
        if (current[i] < target[i])
            current[i] = static_cast<std::int16_t>(std::min<std::int64_t>(current[i] + step, target[i]));
        else if (current[i] > target[i])
            current[i] = static_cast<std::int16_t>(std::max<std::int64_t>(current[i] - step, target[i]));
        if (current[i] == base[i])
            changed &= ~(1u << i);
        target[i] = base[i];
    }
}
bool CreatureSpellBook::add(FourCC id, std::uint16_t priority, bool enabled) {
    // 0x44ffd0 checks capacity before looking for a duplicate.
    if (entries.size() >= 16)
        return false;
    for (auto& entry : entries)
        if (entry.id == id) {
            if (enabled)
                entry.flags |= 1;
            return true;
        }
    const auto position =
        std::ranges::find_if(entries, [&](const auto& entry) { return entry.priority > priority; });
    entries.insert(position, Entry{id, priority, enabled ? 1u : 0u});
    return true;
}
void CreatureSpellBook::level(int value) {
    for (auto& entry : entries)
        if ((entry.flags & 1) || entry.priority / 100 <= value)
            entry.flags &= ~2u;
        else {
            entry.flags |= 2;
            entry.parameter = 0;
        }
}
void CreatureSpellBook::advance(int ticks) {
    nonnegative_ticks(ticks);
    for (auto& entry : entries)
        if (entry.duration) {
            entry.remaining =
                signed_word(static_cast<unsigned>(entry.remaining) - static_cast<unsigned>(ticks));
            if (entry.remaining <= 0) {
                entry.remaining = 0;
                entry.duration = 0;
            }
        }
}
void CreatureSpellBook::cooldown(FourCC id, int base, int extra, const std::function<int(FourCC)>& reload) {
    for (auto& entry : entries) {
        const auto ticks =
            signed_word(static_cast<unsigned>(extra) +
                        static_cast<unsigned>(entry.id == id ? std::max(reload(entry.id), base) : base));
        entry.remaining = std::max(entry.remaining, ticks);
        entry.duration = std::max(entry.duration, ticks);
    }
}
bool CreatureRegeneration::advance(CreatureStats& stats, int ticks, std::int32_t mana,
                                   std::int32_t maximum_mana, bool alive, bool allowed) {
    nonnegative_ticks(ticks);
    auto& health = stats.current[CreatureStats::health];
    const auto maximum = stats.base[CreatureStats::health];
    if (health == maximum || !health || !alive || !allowed) {
        fraction = 0; // 0x45a300 clears it when 0x45a210 returns zero.
        return false;
    }
    const auto ratio =
        maximum_mana ? signed_word(static_cast<std::uint32_t>(mana) << 16) / maximum_mana : 0x10000;
    const auto increment =
        wrapped_product(wrapped_product(ticks, ratio), stats.current[CreatureStats::regeneration]) / 3600;
    fraction = signed_word(static_cast<std::uint32_t>(fraction) + static_cast<std::uint32_t>(increment));
    if (fraction >= 0x10000) {
        health = short_word(health + (static_cast<std::uint32_t>(fraction) >> 16));
        fraction &= 0xffff;
        if (health > maximum)
            health = maximum;
    }
    return true;
}
CreatureFsm CreatureFsm::decode(ByteView data) {
    Reader r(data, "FSMC");
    CreatureFsm result;
    result.id = {r.u32()};
    result.name = r.fixed_string(32);
    result.initial = {{r.u32()}, {r.u32()}};
    const auto groups = r.u32();
    if (groups > r.remaining() / 48)
        throw Error("FSMC group count exceeds payload");
    for (unsigned g = 0; g < groups; ++g) {
        const auto start = r.position();
        const auto size = r.u32();
        if (size < 48 || size > data.size() - start)
            throw Error("Invalid FSMC group size");
        Reader group_reader(data.subspan(start + 4, size - 4), "FSMC group");
        Group group;
        group.id = {group_reader.u32()};
        group.name = group_reader.fixed_string(32);
        group.flags = group_reader.u32();
        const auto states = group_reader.u32();
        if (states > group_reader.remaining() / 148)
            throw Error("FSMC state count exceeds group");
        for (unsigned s = 0; s < states; ++s) {
            const auto state_size = group_reader.u32();
            if (state_size < 148 || state_size - 4 > group_reader.remaining())
                throw Error("Invalid FSMC state size");
            Reader state_reader(group_reader.take(state_size - 4), "FSMC state");
            State state;
            state.id = {state_reader.u32()};
            state.animation = signed_word(state_reader.u32());
            state.flags = state_reader.u32();
            state.name = state_reader.fixed_string(32);
            state.enter = state_reader.fixed_string(32);
            state.run = state_reader.fixed_string(32);
            state.exit = state_reader.fixed_string(32);
            if (state.animation < -1 || state.animation >= 64)
                throw Error("FSMC state animation outside CRE8 slots");
            const auto events = state_reader.u32();
            if (events * 48ull != state_reader.remaining())
                throw Error("Invalid FSMC event table size");
            for (unsigned e = 0; e < events; ++e) {
                Event event;
                event.id = signed_word(state_reader.u32());
                event.animation = signed_word(state_reader.u32());
                event.destination = {{state_reader.u32()}, {state_reader.u32()}};
                event.handler = state_reader.fixed_string(32);
                if (event.animation < -1 || event.animation >= 64)
                    throw Error("FSMC event animation outside CRE8 slots");
                result.event_ids.push_back(event.id);
                state.events.push_back(std::move(event));
            }
            group.states.push_back(std::move(state));
        }
        if (group_reader.remaining())
            throw Error("Unused bytes in FSMC group");
        r.take(size - 4);
        result.groups.push_back(std::move(group));
    }
    if (r.remaining())
        throw Error("Unused bytes in FSMC");
    std::ranges::sort(result.event_ids);
    const auto duplicate = std::ranges::unique(result.event_ids);
    result.event_ids.erase(duplicate.begin(), duplicate.end());
    if (result.event_ids.size() > 255)
        throw Error("FSMC has more than 255 unique event ids");
    (void)result.resolve(result.initial);
    for (std::size_t g = 0; g < result.groups.size(); ++g)
        for (auto& state : result.groups[g].states)
            for (auto& event : state.events) {
                const auto target = result.resolve(event.destination, g);
                if (!(state.flags & 0x400000) || (target && !(result.at(*target).flags & 0x400000)))
                    event.network_index = static_cast<std::uint8_t>(
                        std::ranges::lower_bound(result.event_ids, event.id) - result.event_ids.begin());
            }
    return result;
}
std::optional<CreatureFsm::Location> CreatureFsm::resolve(Link link, std::optional<std::size_t> local) const {
    if (!link.state.value)
        return {};
    if (link.group.value) {
        local.reset();
        for (std::size_t g = groups.size(); g-- > 0;)
            if (groups[g].id == link.group) {
                local = g;
                break;
            }
    }
    if (local && *local < groups.size()) {
        const auto& states = groups[*local].states;
        for (std::size_t s = states.size(); s-- > 0;)
            if (states[s].id == link.state)
                return Location{*local, s};
    }
    throw Error("Unresolved FSMC link " + link.group.text() + ":" + link.state.text());
}
const CreatureFsm::State& CreatureFsm::at(Location where) const {
    return groups.at(where.group).states.at(where.state);
}
const CreatureFsm::State* CreatureFsmRunner::state() const {
    return current_ ? &definition_.at(*current_) : nullptr;
}
int CreatureFsmRunner::call(std::string_view name, int ticks) {
    if (name.empty())
        return 0;
    if (!handler)
        throw Error("Unimplemented creature FSM handler: " + std::string(name));
    return handler(*this, name, ticks);
}
int CreatureFsmRunner::start() {
    const auto first = definition_.resolve(definition_.initial);
    if (!first)
        throw Error("Creature FSM has no initial state");
    return enter(*first);
}
int CreatureFsmRunner::enter(CreatureFsm::Location destination) {
    const auto* previous = state();
    const auto& next = definition_.at(destination);
    current_ = destination;
    if (changed)
        changed(previous, next);
    if (next.animation != -1 && animation)
        animation(next.animation, next.flags);
    if (call(next.enter, 0) != 2)
        event(1); // Enter may change current state before the automatic entry event.
    return 2;
}
int CreatureFsmRunner::event(std::int32_t id) {
    const auto* source = state();
    if (!source)
        return 0;
    const auto found = std::ranges::find(source->events, id, &CreatureFsm::Event::id);
    if (found == source->events.end())
        return 0;
    const auto destination = definition_.resolve(found->destination, current_->group);
    if (found->animation != -1 && animation)
        animation(found->animation, 0); // 0x454d90 passes zero flags for an event's animation.
    if (call(found->handler.empty() ? source->exit : found->handler, 0) == 2)
        return 2;
    int result = 1;
    if (destination)
        result = enter(*destination);
    if (notified)
        notified(id);
    return result;
}
void CreatureFsmRunner::run(int ticks) {
    nonnegative_ticks(ticks);
    if (const auto* current = state())
        call(current->run, ticks);
}
} // namespace sacrifice
