#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/navigation.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_registry.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
// x86 integer operations wrap; _ftol returns the low word of a truncated signed 64-bit result.
std::int32_t add(std::int32_t a, std::int32_t b) {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
std::int32_t multiply(std::int32_t a, std::int32_t b) {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
}
std::int32_t integer(double value) {
    if (!std::isfinite(value) || value >= 9223372036854775808.0 || value < -9223372036854775808.0)
        return 0;
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::int64_t>(value)));
}
} // namespace
BuildingActor::~BuildingActor() {
    if (spatial_)
        spatial_->unbind(motion);
}
void BuildingActor::deactivated(Match& match) {
    while (!guards.empty())
        remove_guard(*guards.front());
    if (match.navigation)
        match.navigation->dirty_static(); // 0x464fd0.
    if (spatial_)
        spatial_->unbind(motion);
    spatial_ = nullptr;
    terrain_ = nullptr;
    match_ = nullptr;
}
void BuildingActor::reference_event(Match& match, MatchObject& object, int code) {
    if (code < 0) {
        if (auto* actor = dynamic_cast<CreatureActor*>(&object))
            remove_guard(*actor);
        if (ritual.victim == &object || ritual.wizard == &object) {
            ritual.victim = ritual.wizard = nullptr;
            ritual.action = {};
        }
        if (primary == &object)
            primary = nullptr;
        if (reserved == &object)
            reserved = nullptr;
        if (damage_source_ == object.reference())
            damage_source_ = 0;
    }
    MatchObject::reference_event(match, object, code);
}
void BuildingActor::activated(Match& match) {
    if (match.navigation)
        match.navigation->dirty_static(); // 0x464f00; model-state changes alone do not set this flag.
    match_ = &match;
    if (spatial_)
        spatial_->unbind(motion);
    spatial_ = nullptr;
    terrain_ = nullptr;
    model_ = nullptr;
    model_scale = bounds_scale_ = 1;
    models_ = {};
    definition_ = nullptr;
    definition_id = {};
    part_ = nullptr;
    stage_ = -1;
    model_dirty_ = true;
    motion = {};
    common_status = 0;
    side = 0;
    primary = nullptr;
    controller = reserved = nullptr;
    ritual = {};
    guards.clear();
    reveal = 1;
    building_flags = 0;
    health = maximum_health = threshold = complement_threshold = 0;
    network_identity = -1;
    accumulated_damage = damage_clock = 0;
    damage_source_ = 0;
    regeneration_fraction = regeneration_rate = 0;
    mana_rate = mana_clock = proximity_clock = 0;
    health_dirty = false;
    completed = false;
    sound_handle = 0;
    requests.clear();
    death_requests.clear();
    kind_flags = type;
}
Vec3 BuildingActor::sacrifice_point() const {
    const auto& m = motion.orientation;
    const auto& p = definition_ ? definition_->parameters : std::array<float, 6>{};
    return {static_cast<float>(static_cast<double>(p[3]) * m[0] + static_cast<double>(p[4]) * m[3] +
                               static_cast<double>(p[5]) * m[6] + motion.position.x),
            static_cast<float>(static_cast<double>(p[3]) * m[1] + static_cast<double>(p[4]) * m[4] +
                               static_cast<double>(p[5]) * m[7] + motion.position.y),
            static_cast<float>(static_cast<double>(p[3]) * m[2] + static_cast<double>(p[4]) * m[5] +
                               static_cast<double>(p[5]) * m[8] + motion.position.z)};
}
bool BuildingActor::request_sacrifice(FourCC action, CreatureActor* victim, CreatureActor* doctor,
                                      CreatureActor* wizard) {
    if (!action.value) {
        if (ritual.victim != victim)
            return false;
        ritual.victim = ritual.wizard = nullptr;
        ritual.action = {};
        return true;
    }
    if (ritual.victim || !wizard || !victim || !(building_flags & 2)) {
        if (doctor && network_identity)
            doctor->fsm().event(45);
        return false;
    }
    ritual.victim = victim;
    ritual.wizard = wizard;
    ritual.action = action;
    ritual.remaining = integer(victim->stats.base[5] * .001 * 480.0);
    ritual.initial_health = victim->stats.base[5];
    const int souls = add(victim->soul_count, 1);
    ritual.damage_rate = multiply(100, souls);
    ritual.mana_rate = multiply(800, souls) / 60;
    ritual.fixed_rate = multiply(655360, souls) / 60;
    if (doctor)
        doctor->fsm().event(44);
    if (controller && controller != wizard && controller->allegiance)
        controller->allegiance->event(*controller, wizard, 12);
    return true;
}
void BuildingActor::initialize(const MrmGeometry* model, Vec3 position, float yaw, std::uint32_t map_flags,
                               std::uintptr_t owner_side, const Terrain& terrain, SpatialWorld& spatial) {
    if (spatial_)
        spatial_->unbind(motion);
    motion.position = position;
    motion.yaw = yaw;
    motion.orientation = motion_orientation(yaw);
    motion.flags = 67589;
    common_status = (map_flags & 0x2000000 ? 0x100 : 0) | (map_flags & 0x40000000 ? 2 : 0) |
                    (map_flags & 0x20000000 ? 4 : 0);
    side = owner_side;
    terrain_ = &terrain;
    model_ = model;
    models_.fill(model);
    model_dirty_ = true;
    prepare_bounds();
    if (!model)
        state_flags |= removed_flag;
    spatial_ = &spatial;
    spatial.bind(motion, MatchObject::group(type), [this] {
        return SpatialWorld::Info{
            kind_flags, common_status,
            phase,      allegiance ? reinterpret_cast<std::uintptr_t>(allegiance) : side,
            nullptr,    nullptr,
            true};
    });
    spatial.sync(motion, state_flags);
}
void BuildingActor::configure(Match& match, const Building& definition, unsigned part,
                              std::array<const MrmGeometry*, 3> models, unsigned initial_state) {
    definition_ = &definition;
    part_ = &definition.parts.at(part);
    building_flags = definition.flags;
    maximum_health = health = definition.maximum_health;
    threshold = integer(static_cast<double>(maximum_health) * definition.threshold_fraction);
    complement_threshold = integer((1.0 - definition.threshold_complement) * maximum_health);
    regeneration_fraction = regeneration_rate = 0;
    if (building_flags & 0x100)
        regeneration_rate = static_cast<float>(maximum_health / (definition.regeneration_seconds * 60.0));
    mana_rate = integer(definition.mana_rate * 36408.88888888889);
    models_ = models;
    if (building_flags & 1)
        models_[1] = models_[2] = models_[0];
    requests.push_back({Request::Kind::side_event, 18});
    if (allegiance)
        allegiance->event(*this, nullptr, 18);
    transition(match, initial_state);
}
void BuildingActor::prepare_bounds() {
    if (!model_dirty_ && model_orientation_ == motion.orientation && bounds_scale_ == model_scale)
        return;
    motion.collision_boxes.clear();
    if (model_) {
        const auto scale = [&](Box3 box) {
            return Box3{
                {box.minimum.x * model_scale, box.minimum.y * model_scale, box.minimum.z * model_scale},
                {box.maximum.x * model_scale, box.maximum.y * model_scale, box.maximum.z * model_scale}};
        };
        const auto box = motion_box(scale(model_->bounds), motion.orientation);
        motion.box_min = box.minimum;
        motion.box_max = box.maximum;
        motion.reference_center = {(box.minimum.x + box.maximum.x) * 0.5f,
                                   (box.minimum.y + box.maximum.y) * 0.5f,
                                   (box.minimum.z + box.maximum.z) * 0.5f};
        Vec3 sum;
        for (const auto& collision : model_->collisions) {
            const auto transformed =
                collision.mode == 2
                    ? scale(collision.box)
                    : motion_box(scale(collision.box), motion.orientation, collision.mode == 1);
            motion.collision_boxes.push_back(transformed);
            sum.x += transformed.minimum.x + transformed.maximum.x;
            sum.y += transformed.minimum.y + transformed.maximum.y;
            sum.z += transformed.minimum.z + transformed.maximum.z;
        }
        if (!motion.collision_boxes.empty()) {
            const double k = 0.5 / motion.collision_boxes.size();
            motion.reference_center = {static_cast<float>(sum.x * k), static_cast<float>(sum.y * k),
                                       static_cast<float>(sum.z * k)};
        }
    }
    model_orientation_ = motion.orientation;
    bounds_scale_ = model_scale;
    model_dirty_ = false;
}
void BuildingActor::transition(Match& match, unsigned state) {
    if (state > 2)
        throw Error("Building state outside 0..2");
    if (stage_ >= static_cast<int>(state))
        return;
    const auto previous = stage_;
    const auto cast = [&](FourCC id) {
        requests.push_back({Request::Kind::spell, id.value, this});
        if (match.spell_registry && allegiance)
            if (const auto* definition = match.spell_registry->find(id))
                if (auto* spell = SpellActor::launch_side_cast(match, *allegiance, *definition, this, effect_point())) {
                    spell->owner = this;
                }
    };
    model_ = models_[state];
    model_dirty_ = true;
    if (state == 2) {
        requests.push_back({Request::Kind::notify, 27});
        match.notify_reference(*this, 27);
        if (building_flags & 1) {
            requests.push_back({Request::Kind::ruin_effect, 0x43c8a0});
            // 0x43c8a0: the primary leaves a neutral mana fountain at its position, reserved by the ruin.
            if (!primary) {
                if (match.create_building) {
                    if (auto* font = match.create_building(FourCC::from_text("font"), motion.position, 0, 31,
                                                           0x20000, nullptr, nullptr))
                        font->reserve(this, 1);
                } else
                    requests.push_back({Request::Kind::building_spawn, FourCC::from_text("font").value, this});
            }
            primary = nullptr; // 0x43c8a0 makes each departing part independent.
            SpellActor::launch_altar_ruin(match, *this);
        }
        building_flags = 0;
        completed = false;
        if (previous != -1 && part_ && part_->destroyed_spell.value)
            cast(part_->destroyed_spell);
    } else if (previous == -1) {
        if (!primary) {
            if (building_flags & 0x40)
                requests.push_back({Request::Kind::damage_watcher, 0x43be80, this});
            // 0x44c230(this, 42, handler, primary): a particle host child bound to the building (+476).
            if ((building_flags & 0x55a0) == 0x55a0) {
                requests.push_back({Request::Kind::handler, 0x43b5c0});
                SpellActor::launch_building_host(match, *this, SpellActor::Effect::manalith_host);
            } else if ((building_flags & 0x2810) == 0x2810) {
                requests.push_back({Request::Kind::handler, 0x43b8c0});
                SpellActor::launch_building_host(match, *this, SpellActor::Effect::fountain_host);
            } else if (building_flags & 2) {
                requests.push_back({Request::Kind::handler, 0x43bb10});
                SpellActor::launch_building_host(match, *this, SpellActor::Effect::altar_host);
            }
        }
        if (part_ && part_->start_spell.value)
            cast(part_->start_spell);
    }
    stage_ = static_cast<int>(state);
    if (!model_)
        state_flags |= removed_flag;
}
void BuildingActor::set_stage(Match& match, unsigned state) {
    if (state > 2)
        throw Error("Building state outside 0..2");
    // 0x465300 walks the type list newest first, including parts whose flags were already cleared.
    for (const auto& object : match.objects())
        if (auto* part = dynamic_cast<BuildingActor*>(object.get()); part && (part->kind_flags & building))
            if ((part->primary ? part->primary : part) == this)
                part->transition(match, state);
}
MatchObject* BuildingActor::reserve(MatchObject* object, int mode) {
    if (primary || !(building_flags & 0x10))
        return nullptr;
    // Offline 0x498bd0 permits the edit; an existing reservation wins for modes other than 2.
    if (mode != 2 && reserved)
        return reserved;
    reserved = object;
    return object;
}
void BuildingActor::destroy(Match& match, std::uint32_t source) {
    if (primary)
        return;
    health = 0;
    if (reveal == 1) {
        requests.push_back({Request::Kind::side_event, 16});
        if (allegiance)
            allegiance->event(*this, source ? match.resolve(source) : nullptr, 16);
    }
    set_stage(match, 2);
}
bool BuildingActor::damageable() const {
    return (building_flags & 0x80) && !(common_status & 4) && reveal == 1;
}
bool BuildingActor::damage(Match& match, Damage value) {
    if (reveal != 1)
        return false;
    if (primary)
        return primary->damage(match, value);
    if (!guards.empty()) {
        const auto targets = guards; // A lethal hit may unlink the current guard.
        const auto share = value.amount / static_cast<int>(targets.size());
        for (auto* guard : targets)
            guard->damage(match, {std::bit_cast<int>(value.kind), share, value.source});
        return true;
    }
    if (!damageable())
        return false;
    if (const auto* source = match.resolve(value.source); value.source && source && friendly(source))
        value.amount = multiply(value.amount, match.friendly_damage_scale) >> 7;
    match.attribute_damage(*this, std::bit_cast<std::int32_t>(value.kind), value.amount, value.source);
    std::array<std::uint32_t, 3> words{value.kind, std::bit_cast<std::uint32_t>(value.amount), value.source};
    match.fire(*this, 0, words);
    value.amount = std::bit_cast<std::int32_t>(words[1]);
    if (value.amount > 0 && health > 0 && network_identity != 0) {
        requests.push_back({Request::Kind::side_event, 15});
        if (allegiance)
            allegiance->event(*this, match.resolve(value.source), 15);
        health = add(health, -value.amount);
        health_dirty = true;
        if (health <= 0) {
            if (common_status & 2)
                health = 1;
            else
                destroy(match, value.source);
        }
    }
    return true;
}
void BuildingActor::accumulate_damage(std::int32_t amount, int ticks, std::uint32_t source) {
    if (!primary && reveal == 1 && !guards.empty()) {
        const auto targets = guards;
        const auto share = amount / static_cast<int>(targets.size());
        for (auto* guard : targets)
            guard->accumulate_damage({0, share, source}, ticks);
        return;
    }
    // 0x465ce0 deliberately does not forward secondary-part accumulation to the primary.
    if (!primary && damageable()) {
        amount = multiply(amount, ticks);
        if (const auto* object = match_ ? match_->resolve(source) : nullptr;
            source && object && friendly(object))
            amount = multiply(amount, match_->friendly_damage_scale) >> 7;
        accumulated_damage = add(accumulated_damage, amount);
        damage_source_ = source;
    }
}
void BuildingActor::tick_damage(Match& match, int ticks) {
    if (accumulated_damage < 60)
        return;
    damage_clock = add(damage_clock, ticks);
    if (damage_clock < 16)
        return;
    damage_clock = 0;
    if (!damageable()) {
        accumulated_damage = 0;
        return;
    }
    auto value = accumulated_damage / 60;
    accumulated_damage %= 60;
    match.attribute_damage(*this, 0, value, damage_source_);
    const auto source = std::exchange(damage_source_, 0u);
    if (!primary && value > 0) {
        if (value >= health)
            destroy(match, source); // This path does not test the direct-hit leave-one-health flag.
        else
            health = add(health, -value);
    }
}
void BuildingActor::supply_mana(std::int16_t affinity, std::int32_t& mana, std::int32_t maximum,
                                std::int32_t& fraction, int ticks) const {
    if (reserved || primary || reveal != 1 || !(building_flags & 0xc00) || !mana_rate)
        return;
    auto value = multiply(mana_rate, ticks);
    if (value > 0)
        value = integer(static_cast<double>(affinity) * 0.001 * value);
    fraction = add(fraction, value);
    if (static_cast<std::uint32_t>(fraction) & 0xffff0000u) {
        mana = add(mana, fraction >> 16);
        fraction = static_cast<std::int32_t>(static_cast<std::uint32_t>(fraction) & 0xffff);
        mana = std::min(std::max(mana, 0), maximum);
    }
}
Vec3 BuildingActor::effect_point() const {
    const auto& p = definition_ ? definition_->parameters : std::array<float, 6>{};
    const auto& m = motion.orientation;
    return {p[0] * m[0] + p[1] * m[3] + p[2] * m[6] + motion.position.x,
            p[0] * m[1] + p[1] * m[4] + p[2] * m[7] + motion.position.y,
            p[0] * m[2] + p[1] * m[5] + p[2] * m[8] + motion.position.z};
}
void BuildingActor::scan_mana(Match& match, int ticks) {
    const auto supply = [&](MatchObject& object) {
        auto* actor = dynamic_cast<CreatureActor*>(&object);
        if (!actor || !(actor->kind_flags & 5) || (actor->fsm().state()->flags & 0x4000))
            return 0;
        const auto point = actor->motion.position;
        const double dx = static_cast<double>(motion.position.x) - point.x,
                     dy = static_cast<double>(motion.position.y) - point.y,
                     dz = static_cast<double>(motion.position.z) - point.z;
        if (dz * dz + dy * dy + dx * dx < 2500)
            supply_mana(actor->stats.current[20], actor->mana, actor->maximum_mana,
                        actor->casting.mana_fraction, ticks);
        return 0;
    };
    if ((building_flags & 0x400) && allegiance) {
        if (!match.side_order.empty()) {
            for (auto* supply_side : match.side_order)
                if ((supply_side->identity & allegiance->allies) ||
                    (supply_side->allies & allegiance->identity) || supply_side == allegiance)
                    for (const unsigned group : {0u, 1u})
                        for (auto* actor : supply_side->objects[group])
                            supply(*actor);
        } else {
            // Unbound diagnostic matches do not carry the native SIDE list.
            for (const unsigned group : {0u, 1u})
                match.visit_group(group, 5, [&](MatchObject& actor) {
                    auto* supply_side = actor.allegiance;
                    if (supply_side &&
                        ((supply_side->identity & allegiance->allies) ||
                         (supply_side->allies & allegiance->identity) || supply_side == allegiance))
                        return supply(actor);
                    return 0;
                });
        }
    } else
        for (const unsigned group : {0u, 1u})
            match.visit_group(group, 5, supply);
}
void BuildingActor::scan_proximity(Match& match) {
    match.visit_group(1, 1, [&](MatchObject& object) {
        auto* wizard = dynamic_cast<CreatureActor*>(&object);
        if (!wizard || !wizard->alive() || !enemy(wizard))
            return 0;
        const auto point = wizard->motion.position;
        const double dx = static_cast<double>(motion.position.x) - point.x,
                     dy = static_cast<double>(motion.position.y) - point.y,
                     dz = static_cast<double>(motion.position.z) - point.z;
        if (!(dz * dz + dy * dy + dx * dx < 10000))
            return 0;
        if (controller && controller->allegiance)
            controller->allegiance->event(*controller, wizard, 13);
        return -1;
    });
}
bool BuildingActor::run(Match& match, int ticks) {
    if (primary && primary != this) {
        health = primary->health;
        maximum_health = primary->maximum_health;
        controller = primary->controller;
        reveal = primary->reveal;
    } else {
        if (reveal == 1) {
            tick_damage(match, ticks);
            if ((building_flags & 1) && controller) {
                proximity_clock = add(proximity_clock, -ticks);
                if (proximity_clock <= 0) {
                    proximity_clock = 32;
                    requests.push_back({Request::Kind::proximity_scan, 0x465830});
                    scan_proximity(match);
                }
            }
            if (!primary && (building_flags & 0x100)) {
                const double value = static_cast<double>(ticks) * regeneration_rate + regeneration_fraction;
                regeneration_fraction = static_cast<float>(value);
                if (value >= 1) {
                    const auto whole = integer(value);
                    health = std::min(add(health, whole), maximum_health);
                    regeneration_fraction = static_cast<float>(value - whole);
                }
            }
            mana_clock = add(mana_clock, ticks);
            if (mana_clock >= 8) {
                if (!controller && allegiance)
                    controller = allegiance->wizard();
                if (!reserved && !primary && reveal == 1 && (building_flags & 0xc00) && mana_rate) {
                    requests.push_back({Request::Kind::mana_scan, 0x465880, nullptr, mana_clock});
                    scan_mana(match, mana_clock);
                }
                mana_clock = 0; // Discards excess; the ordered scan reads this before the reset.
            }
        }
        if (definition_ && definition_->sound.value) {
            if (sound_handle && reserved) {
                requests.push_back({Request::Kind::sound_stop, static_cast<std::uint32_t>(sound_handle)});
                sound_handle = 0;
            } else if (!sound_handle && !reserved && reveal == 1 && ticks != 0)
                requests.push_back({Request::Kind::sound_start, definition_->sound.value});
        }
    }
    return MatchObject::run(match, ticks);
}
void BuildingActor::sync_model(Match&) {
    prepare_bounds();
    if (spatial_)
        spatial_->sync(motion, state_flags);
}
void BuildingActor::update(Match&, int ticks) {
    if (terrain_)
        motion.advance(*terrain_, state_flags, ticks,
                       {{}, [this](int reason) { death_requests.push_back(reason); }});
}
} // namespace sacrifice
