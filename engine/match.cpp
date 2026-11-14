#include "sacrifice/match.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/motion.hpp"
#include "sacrifice/navigation.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/side_ai.hpp"
#include "sacrifice/sound.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/trigger.hpp"
#include "sacrifice/world_save.hpp"
#include <algorithm>
#include <array>

namespace sacrifice {
void Match::set_cinematic(bool enabled) {
    if (cinematic == enabled)
        return;
    cinematic = enabled;
    // 0x453400 visits activation mask 13: wizards, creatures and their corpses. Script orders
    // replace only the segment before this marker; ending the scene restores the saved queue.
    for (const auto& object : active_)
        if (auto* actor = dynamic_cast<CreatureActor*>(object.get()); actor && (actor->kind_flags & 13)) {
            if (enabled)
                actor->issue_order({CreatureOrders::cinematic_marker, actor});
            else
                actor->orders.through_marker(true);
        }
}
bool MatchObject::friendly(const MatchObject* source) const {
    if (!source)
        return true;
    if (!allegiance || !source->allegiance)
        return false;
    const auto& side = *allegiance;
    const auto& other = *source->allegiance;
    return ((side.allies & other.identity) || (other.allies & side.identity) ||
            side.identity == other.identity) &&
           !(source->state_flags & 0x2000);
}
bool MatchObject::enemy(const MatchObject* source) const {
    if (!source)
        return true;
    if (source->state_flags & 0x2000)
        return true;
    if (!allegiance || !source->allegiance)
        return false;
    const auto& side = *allegiance;
    const auto& other = *source->allegiance;
    return ((side.enemies & other.identity) || (other.enemies & side.identity)) &&
           side.identity != other.identity;
}
void MatchObject::reference_event(Match& match, MatchObject& object, int code) {
    if ((code & 0x10) && effect_target == &object)
        effect_target = nullptr;
    if (code >= 0)
        return;
    std::erase_if(named_references, [&](const auto& entry) { return entry.object == &object; });
    if (last_damage_source == object.reference())
        last_damage_source = 0;
    if (credited_attacker == object.reference())
        credited_attacker = 0;
    match.unwatch(*this, reinterpret_cast<std::uintptr_t>(&object));
    if (controller == &object)
        controller = nullptr;
    if (owner == &object)
        owner = nullptr;
}
bool MatchObject::name_reference(FourCC name, MatchObject* object) {
    if (!name.value || !object || named_references.size() >= 8 ||
        std::ranges::any_of(named_references,
                            [&](const auto& entry) { return entry.name == name && entry.object == object; }))
        return false;
    named_references.push_back({name, object});
    return true;
}
MatchObject* MatchObject::named(FourCC name) const {
    const auto found = std::ranges::find(named_references, name, &NamedReference::name);
    return found == named_references.end() ? nullptr : found->object;
}
void MatchObject::unname(FourCC name, const MatchObject* object) {
    std::erase_if(named_references,
                  [&](const auto& entry) { return entry.name == name && entry.object == object; });
}
unsigned MatchObject::group(std::uint32_t type) {
    static constexpr std::array<std::uint32_t, 5> masks{4, 1, 16, 130, 96};
    for (unsigned i = 0; i < masks.size(); ++i)
        if (type & masks[i])
            return i;
    throw Error("Unknown match object type");
}
int MatchObject::run_behaviours(int ticks) {
    while (behaviour) {
        const auto result = behaviour(*this, ticks);
        if (result == 0)
            continue;
        if (result == -1) {
            behaviour = nullptr;
            return 0;
        }
        return result == -2 ? 1 : 0;
    }
    return 0;
}
void MatchObject::periodic(Match& match) {
    SpellActor::launch_corpse_fade(match, *this);
}
bool MatchObject::run(Match& match, int ticks) {
    countdown -= ticks;
    if (!(kind_flags & periodic_flag)) {
        phase += ticks;
    } else if (phase) {
        phase += ticks;
        if (phase >= 60) {
            periodic(match);
            phase = 0;
        }
    }
    if (state_flags & removed_flag)
        return true;
    if (run_behaviours(ticks))
        return true;
    sync_model(match);
    if (timer)
        timer = std::max(timer - ticks, 0);
    update(match, ticks);
    return false;
}
MatchObject& Match::activate(std::unique_ptr<MatchObject> object, const MatchObject* after) {
    object->allegiance = nullptr;
    object->kind_flags = object->type;
    object->controller = object->owner = nullptr;
    object->effect_target = nullptr;
    object->named_references.clear();
    object->last_damage_source = object->credited_attacker = 0;
    object->state_flags = 0;
    object->object_flags = 0;
    object->common_status = 0;
    object->timer = 0;
    object->behaviour = nullptr;
    object->watch_mask = ~0u;
    object->stack.clear();
    object->phase = random_.next() & 0x3f;
    object->countdown = random_.next() & 0xf;
    object->seen_by = object->hidden_from = 0;
    object->scan_side = nullptr;
    auto position = active_.begin();
    if (after) {
        position = std::ranges::find_if(active_, [&](const auto& o) { return o.get() == after; });
        if (position == active_.end()) {
            unlink_group(*object);
            references_.erase(object->reference_);
            throw Error("Activation after an inactive object");
        }
        ++position;
    }
    auto& placed = **active_.insert(position, std::move(object));
    if (!placed.reference_) {
        if (!next_reference_)
            throw Error("Match reference space exhausted");
        placed.reference_ = next_reference_++;
        references_.emplace(placed.reference_, &placed);
    }
    link_group(placed, MatchObject::group(placed.type));
    placed.activated(*this);
    return placed;
}
MatchObject& Match::create(std::uint32_t type, const std::function<std::unique_ptr<MatchObject>()>& make,
                           const MatchObject* after, const std::function<void(MatchObject&)>& configure) {
    auto& pool = inactive_[MatchObject::group(type)];
    const auto found = std::ranges::find_if(pool, [&](const auto& o) { return o->type == type; });
    std::unique_ptr<MatchObject> object;
    if (found != pool.end()) {
        object = std::move(*found);
        pool.erase(found);
    } else {
        object = make();
        object->type = type;
    }
    try {
        if (configure)
            configure(*object);
    } catch (...) {
        unlink_group(*object);
        references_.erase(object->reference_);
        throw;
    }
    return activate(std::move(object), after);
}
void Match::release_watchers(MatchObject& object) {
    for (auto& slot : object.watchers) {
        free_watchers_ += static_cast<unsigned>(slot.size());
        slot.clear();
    }
}
void Match::deactivate(const MatchObject& object) {
    const auto found = std::ranges::find_if(active_, [&](const auto& o) { return o.get() == &object; });
    if (found == active_.end())
        throw Error("Deactivating an inactive object");
    notify_reference(**found, -1);
    auto owned = std::move(*found);
    active_.erase(found);
    owned->deactivated(*this);
    if (stop_looping_sounds)
        stop_looping_sounds(owned->reference());
    if (owned->allegiance)
        owned->allegiance->detach(*owned);
    release_watchers(*owned);
    link_group(*owned, 5 + MatchObject::group(owned->type));
    inactive_[MatchObject::group(owned->type)].push_front(std::move(owned));
}
void Match::unlink_group(MatchObject& object) {
    if (object.group_previous_)
        *object.group_previous_ = object.group_next_;
    if (object.group_next_)
        object.group_next_->group_previous_ = object.group_previous_;
    object.group_next_ = nullptr;
    object.group_previous_ = nullptr;
}
void Match::link_group(MatchObject& object, unsigned group) {
    unlink_group(object);
    auto& head = group_heads_.at(group);
    object.group_next_ = head;
    object.group_previous_ = &head;
    if (head)
        head->group_previous_ = &object.group_next_;
    head = &object;
}
int Match::visit_group(unsigned group, unsigned mask, const std::function<int(MatchObject&)>& callback) {
    auto* at = group_heads_.at(group);
    while (at) {
        auto* next = at->group_next_;
        if ((at->kind_flags & mask) && callback(*at) == -1)
            return -1;
        at = next;
    }
    return 0;
}
MatchObject* Match::resolve(std::uint32_t reference) const {
    const auto found = references_.find(reference);
    return found == references_.end() ? nullptr : found->second;
}
void Match::notify_reference(MatchObject& object, int code) {
    if (code < 0 && direct_control_target == &object)
        direct_control_target = nullptr;
    if ((code & 4) && forget_interface)
        forget_interface(object);
    if (code < 0)
        for (auto* side : side_order) {
            // Full removal also releases SIDE sightings and controller agents. A pooled object
            // keeps its reference identity, so resolve() alone cannot detect its deactivation.
            std::erase_if(side->known, [&](const MatchSide::Known& entry) { return entry.object == &object; });
            if (side->controller)
                side->controller->drop(object);
        }
    for (auto it = active_.begin(); it != active_.end();) {
        auto& target = **it++;
        target.reference_event(*this, object, code);
    }
}
void Match::attribute_damage(MatchObject& target, int type, int& amount, std::uint32_t source) {
    target.last_damage_source = source;
    target.credited_attacker = 0;
    auto* attacker = resolve(source);
    if (attacker && !(attacker->kind_flags & 5))
        attacker = attacker->owner;
    if (attacker && (attacker->kind_flags & 5)) {
        target.credited_attacker = attacker->reference();
        amount = attacker->damage_dealt(*this, target, type, amount);
        // Victim-side awareness (0x472820) awaits its visibility/controller host.
    }
}
bool Match::watch(MatchObject& object, unsigned slot, const MatchObject::Watcher& watcher) {
    if (slot >= 4 || !free_watchers_)
        return false;
    --free_watchers_;
    object.watchers[slot].push_front(watcher);
    return true;
}
void Match::unwatch(MatchObject& object, unsigned slot, std::uintptr_t key, const MatchObject* watcher) {
    if (slot >= 4)
        return;
    auto& list = object.watchers[slot];
    const auto found = std::ranges::find_if(
        list, [&](const auto& w) { return w.key == key && (!watcher || w.watcher == watcher); });
    if (found != list.end()) {
        list.erase(found);
        ++free_watchers_;
    }
}
void Match::unwatch(MatchObject& object, std::uintptr_t key) {
    for (auto& list : object.watchers) {
        const auto found = std::ranges::find_if(list, [&](const auto& w) { return w.key == key; });
        if (found != list.end()) {
            list.erase(found);
            ++free_watchers_;
            return;
        }
    }
}
void Match::assign(MatchObject& object, std::uintptr_t id) {
    if (!id) {
        object.behaviour = nullptr;
        return;
    }
    const auto found = functions.find(id);
    if (found == functions.end())
        throw Error("Cannot assign an unknown behaviour handler");
    object.behaviour = found->second;
}
void Match::fire(MatchObject& object, unsigned slot, std::span<std::uint32_t> words) {
    if (slot >= 4 || !(object.watch_mask & (1u << slot)))
        return;
    auto& list = object.watchers[slot];
    for (auto it = list.begin(); it != list.end();) {
        const auto next = std::next(it);
        const auto watcher = *it;
        auto& context = *watcher.watcher;
        const auto base = context.stack.size();
        context.stack.insert(context.stack.end(), words.begin(), words.end());
        context.stack.push_back(watcher.argument);
        auto saved = std::move(context.behaviour);
        assign(context, watcher.handler);
        while (context.behaviour && context.behaviour(context, 0) == 0) {
        }
        context.behaviour = std::move(saved);
        std::copy_n(context.stack.begin() + static_cast<std::ptrdiff_t>(base), words.size(), words.begin());
        context.stack.resize(base);
        it = next;
    }
}
float Match::presentation_blend() const {
    if (!interpolate || presented_ticks_ <= 0)
        return 1;
    // Draw one tick behind the simulation: after n ticks, `fraction` of the next has elapsed.
    return std::clamp((static_cast<float>(presented_ticks_) - 1 + clock_.fraction) / presented_ticks_, 0.f, 1.f);
}
ObjectMotion Match::presented(const MatchObject& object) const {
    ObjectMotion result;
    const auto* motion = object.source_motion();
    if (!motion)
        return result;
    result.position = motion->position;
    result.orientation = motion->orientation;
    const auto found = previous_poses_.find(&object);
    if (found == previous_poses_.end())
        return result;
    const auto& before = found->second;
    const float dx = motion->position.x - before.position.x, dy = motion->position.y - before.position.y,
                dz = motion->position.z - before.position.z;
    // A teleport, respawn or reused pooled object jumps; 10 units per tick is beyond any normal move.
    const float limit = 10.f * static_cast<float>(presented_ticks_);
    if (dx * dx + dy * dy + dz * dz > limit * limit)
        return result;
    const float t = presentation_blend();
    result.position = {before.position.x + dx * t, before.position.y + dy * t, before.position.z + dz * t};
    for (std::size_t i = 0; i < 9; ++i)
        result.orientation[i] = before.orientation[i] + (motion->orientation[i] - before.orientation[i]) * t;
    return result;
}
int Match::advance(float elapsed) {
    return tick(clock_.advance(elapsed));
}
int Match::tick(int ticks) {
    if (world_journal)
        world_journal->begin_tick(ticks);
    ticks = std::min(ticks, 12);
    const bool live = running && !paused;
    if (ticks > 0) {
        // Presentation snapshot: a paused or stepped update draws the current pose (no blend).
        previous_poses_.clear();
        presented_ticks_ = 0;
    }
    if (live && ticks > 0) {
        previous_poses_.reserve(active_.size());
        for (const auto& object : active_)
            if (const auto* motion = object->source_motion())
                previous_poses_.emplace(object.get(), PresentedPose{motion->position, motion->orientation});
    }
    if (live) {
        ticks = clock_.match_ticks(ticks);
        if (ticks > 0)
            presented_ticks_ = ticks;
        if (navigation)
            navigation->advance(ticks);
    }
    // 0x453940: outside a running, unpaused match only objects flagged 0x80 run.
    for (auto it = active_.begin(); it != active_.end();) {
        auto& object = **it;
        if (live || (object.state_flags & MatchObject::paused_run_flag)) {
            if ((object.state_flags & MatchObject::removed_flag) || object.run(*this, ticks)) {
                // 0x4529e0 with -1: vtable +184, then deactivation (vtable +16) into the inactive pool.
                const auto next = std::next(it);
                deactivate(object);
                it = next;
                continue;
            }
            object.state_flags |= MatchObject::ran_flag;
        }
        ++it;
    }
    if (live && triggers && (!local_side || (local_side->outcome != 1 && local_side->outcome != -1)))
        triggers->tick(*this);
    if (live) {
        // The 0x471fb0 side pass (list +608): known-object expiry, controller tick (vtable +20) for
        // computer sides, then the side work queue (0x473130).
        for (auto* side : side_order)
            side->tick(*this, ticks);
        ambient(ticks);
        weather.advance(random_, ticks, [&](int action, FourCC group, float volume) {
            FourCC sample;
            if ((action == 1 || action == 2) && sound_set) sample = sound_set->select(group, random_);
            weather_sounds.push_back({action, sample, group, volume});
        });
        apply_terrain_dents();
    }
    if (world_journal)
        world_journal->end_tick();
    return ticks;
}
void Match::dent(Vec3 position, float radius, float depth) {
    // 0x44d0b0 with flag 1: LAND::DentMap. Flag 2's event 30 is still open.
    terrain_dents.push_back({position, radius, depth});
}
void Match::displace(Terrain::Displacement& patch, float scale) {
    if (terrain && const_cast<Terrain&>(*terrain).apply_displacement(patch, scale)) {
        ++terrain_revision;
        land_heights_changed_ = true;
    }
}
int Match::set_edge(int x, int y, int flags, int size) {
    if (!terrain)
        return 0;
    const int written = const_cast<Terrain&>(*terrain).set_edge(x, y, flags, size);
    if (written) {
        ++terrain_revision;
        land_changed_ = true;
    }
    return written;
}
void Match::apply_terrain_dents() {
    if (terrain && !terrain_dents.empty()) {
        auto& land = const_cast<Terrain&>(*terrain);
        for (const auto& request : terrain_dents)
            land.dent_map(request.position, request.radius, request.depth);
        ++terrain_revision;
        land_changed_ = true;
    }
    terrain_dents.clear();
    if ((land_changed_ || land_heights_changed_) && navigation && terrain)
        navigation->refresh_land(*terrain, land_changed_);
    land_changed_ = false;
    land_heights_changed_ = false;
}
void Match::ambient(int ticks) {
    // 0x452090; the weights at 0x4ceb44 choose sample 0 (18/32), 1 (9/32), 2 (4/32) or none (1/32).
    static constexpr std::array<std::uint8_t, 32> weights{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                                          0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3};
    ambient_timer_ += ticks;
    if (ambient_timer_ < 60)
        return;
    ambient_timer_ = random_.next() & 0x1f;
    struct World {
        const char* tileset;
        const char* samples; // three FourCC prefixes: <prefix>C, <prefix>N, <prefix>U
        int kind;
        float chance, a0, a1, b0, b1, c0, c1;
    };
    static constexpr std::array<World, 5> worlds{{
        {"char", "Ach", 6, 1, 0, 0, 1, 1, 0, 0},
        {"prsc", "Ape", 1, 0.0016666667f, 5, 20, 0.5f, 1, 30, 180},
        {"ST_A", "Ast", 2, 0.0016666667f, 5, 10, 0.2f, 1, 30, 180},
        {"JA_A", "Aja", 0, 0, 0, 0, 0, 0, 0, 0},
        {"PYRO", nullptr, 5, 0.0016666667f, 5, 10, 0.2f, 0.5f, 10, 45},
    }};
    const World* world = nullptr;
    for (const auto& w : worlds)
        if (FourCC::from_text(w.tileset) == tileset_)
            world = &w;
    AmbientCue cue;
    if ((random_.next() & 0xf) < 3) {
        const auto index = weights[random_.next() & 0x1f];
        if (index < 3 && world && world->samples) {
            cue.group = FourCC::from_text(std::string(world->samples) + "CNU"[index]);
            if (sound_set) {
                const auto sample = sound_set->select(*cue.group, random_);
                if (sample.value)
                    cue.sample = sample;
            }
        }
    }
    if (world && world->kind && world->chance * 32768.0 > random_.next()) {
        const auto ranged = [&](float low, float high) {
            return static_cast<float>(random_.next() * (high - low) * 0.000030517578125 + low);
        };
        // Drawn in the original's order: a, b, c; passed to 0x4523e0 as (b, a, c).
        const float a = ranged(world->a0, world->a1);
        const float b = ranged(world->b0, world->b1);
        const float c = ranged(world->c0, world->c1);
        cue.effect_kind = world->kind;
        cue.effect_values[0] = b;
        cue.effect_values[1] = a;
        cue.effect_values[2] = c;
    }
    if (cue.effect_kind)
        weather.set(cue.effect_kind, cue.effect_values[0], cue.effect_values[1], cue.effect_values[2], false);
    if (cue.group || cue.effect_kind)
        ambient_cues.push_back(cue);
}
} // namespace sacrifice
