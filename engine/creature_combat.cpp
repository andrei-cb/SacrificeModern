#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/sound.hpp"
#include "sacrifice/spell_actor.hpp"
#include <cmath>

namespace sacrifice {
namespace {
std::int32_t add(std::int32_t a, std::int32_t b) {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
std::int32_t multiply(std::int32_t a, std::int32_t b) {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
}
std::int16_t short_word(std::int32_t value) {
    return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(value));
}
} // namespace
bool CreatureActor::friendly(const MatchObject* source) const {
    return MatchObject::friendly(source) &&
           (!(resources_->definition.flags & 0x4000) || !source || !source->allegiance ||
            !(source->allegiance->identity & friendly_exception_mask));
}
bool CreatureActor::enemy(const MatchObject* source) const {
    return MatchObject::enemy(source) ||
           ((resources_->definition.flags & 0x4000) && source && source->allegiance &&
            (source->allegiance->identity & friendly_exception_mask));
}
void CreatureActor::reference_event(Match& match, MatchObject& object, int code) {
    if (code & 0xA)
        orders.clear_target(object);
    if (code < 0)
        for (auto& requirement : casting.requirements)
            if (requirement == &object)
                requirement = nullptr;
    if (code < 0) {
        if (blood_host == &object)
            blood_host = nullptr;
        if (flame_host == &object)
            flame_host = nullptr;
        if (approach_blocker == &object)
            approach_blocker = nullptr;
        if (accumulated_source_ == object.reference())
            accumulated_source_ = 0;
        if (ritual_building == &object)
            ritual_building = nullptr;
        if (home_altar == &object)
            home_altar = nullptr;
        if (doctor_altar == &object)
            doctor_altar = nullptr;
        if (ritual_abort_target == &object)
            ritual_abort_target = nullptr;
    }
    if (code & 0x10) {
        if (ritual_abort_owner == &object)
            ritual_abort_owner = nullptr;
    }
    MatchObject::reference_event(match, object, code);
}
void CreatureActor::earn_credit(int amount, std::uint32_t flags) {
    // Offline +252. Creature credit forwards to its wizard controller before counters change.
    if (type == MatchObject::wizard) {
        credit_requests.push_back({amount, flags});
        if (flags & 0x80) {
            credit_count_80 = add(credit_count_80, 1);
            progression.credit_count_80 = add(progression.credit_count_80, 1);
        }
        if (flags & 0x20) {
            credit_count_20 = add(credit_count_20, 1);
            progression.credit_count_20 = add(progression.credit_count_20, 1);
        }
        const auto delta = multiply(amount, match_->experience_scale) / 32;
        const auto minimum = network_identity ? match_->minimum_wizard_level : progression.level;
        const auto maximum = network_identity ? match_->maximum_wizard_level : progression.level;
        const auto change =
            progression.adjust(delta, minimum, maximum, [this](auto& state) { grant_level(state); });
        if (change)
            level_changed(change);
        else
            replication_flags |= 0x2000;
        return;
    } else if (controller && (controller->kind_flags & MatchObject::wizard))
        if (auto* wizard = dynamic_cast<CreatureActor*>(controller))
            wizard->earn_credit(amount, flags | 0x40);
    if (flags & 0x80)
        credit_count_80 = add(credit_count_80, 1);
    if (flags & 0x20)
        credit_count_20 = add(credit_count_20, 1);
}
int CreatureActor::kill_value() const {
    if (type == MatchObject::wizard) // 0x482db0
        return add(multiply(2000, progression.level), 5000);
    return multiply(5, definition().value); // 0x482d90
}
void CreatureActor::credit_kill(const CreatureActor& victim, std::uint32_t flags) {
    // The network id (+772) is always present offline. Victims of type wizard/creature also play the
    // 0x454ba0 kill cue and the 0x454ac0 message; that presentation is not connected.
    if (enemy(&victim))
        earn_credit(victim.kill_value(), flags);
}
int CreatureActor::damage_dealt(Match& match, MatchObject& target, int damage_type, int amount) {
    std::array<std::uint32_t, 3> words{static_cast<std::uint32_t>(damage_type),
                                       static_cast<std::uint32_t>(amount), target.reference()};
    match.fire(*this, 1,
               words); // Only amount is read back by 0x482f50; target remains its original argument.
    amount = std::bit_cast<std::int32_t>(words[1]);
    if (ritual_building)
        amount =
            std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::int64_t>(amount * 1.5)));
    if (enemy(&target))
        earn_credit(amount, 0);
    if (stats.current[7] && (target.kind_flags & 5)) {
        const auto gain = multiply(amount, stats.current[7]) / 1000;
        auto& health = stats.current[CreatureStats::health];
        health = short_word(add(health, gain)); // Store wraps before the signed maximum comparison.
        if (health > stats.base[CreatureStats::health])
            health = stats.base[CreatureStats::health];
    }
    return amount;
}
bool CreatureActor::damageable() const {
    return !(common_status & 4) && fsm_->state() && !(fsm_->state()->flags & 0x1000);
}
void CreatureActor::mitigate(Damage& value) const {
    // 0x454920, then creature vtable +244 (0x459890). Multiplication wraps before division.
    if (value.source) {
        const auto* source = match_ ? match_->resolve(value.source) : nullptr;
        if (friendly_source ? friendly_source(value.source) : source && friendly(source))
            value.amount = multiply(value.amount, allegiance && match_ ? match_->friendly_damage_scale
                                                                       : friendly_damage_scale) >>
                           7;
    }
    if (value.type) {
        const auto index = static_cast<std::uint32_t>(value.type) & 7;
        if (index > 4)
            throw Error("Illegal creature damage modifier");
        const float factor = index ? 1.0f : 0.4f;
        value.amount = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::int64_t>(
            static_cast<double>(multiply(value.amount, stats.current[9 + index])) * factor * 0.001)));
    }
    value.amount =
        multiply(value.amount, allegiance ? allegiance->damage_multiplier : damage_multiplier) / 1000;
}
void CreatureActor::attribute(Damage& value) {
    last_damage_source = value.source;
    credited_attacker = 0;
    if (value.source && attribute_damage)
        attribute_damage(value);
    else if (match_)
        match_->attribute_damage(*this, value.type, value.amount, value.source);
}
void CreatureActor::combat_sound(FourCC group) {
    FourCC sample;
    for (const auto& set : resources_->sound_sets)
        if (set && (sample = set->select(group, *random_)).value)
            break;
    combat_requests.push_back({CombatRequest::Kind::sound, 0, 0, group, sample});
}
FourCC CreatureActor::voice_sample(FourCC group, int index) const {
    if (resources_ && random_)
        for (const auto& set : resources_->sound_sets)
            if (set)
                if (const auto sample = set->select(group, *random_, index); sample.value)
                    return sample;
    return {};
}
void CreatureActor::side_event(int code) {
    combat_requests.push_back({CombatRequest::Kind::side_event, code, credited_attacker});
    if (allegiance && match_ && (!(fsm_->state()->flags & 0x8000000) || allegiance != match_->local_side))
        allegiance->event(*this, match_->resolve(credited_attacker), code);
}
bool CreatureActor::lose_health(int amount) {
    // 0x459a60 deliberately leaves health unchanged on a lethal result. The death path owns it.
    auto& health = stats.current[CreatureStats::health];
    const auto maximum = stats.base[CreatureStats::health];
    if (health == maximum)
        side_event(3);
    if (amount >= health && !(common_status & 2)) {
        side_event(5);
        return true;
    }
    const auto remaining = add(health, std::bit_cast<std::int32_t>(0u - static_cast<std::uint32_t>(amount)));
    health = short_word(remaining <= 1 ? 1 : remaining);
    const auto* attacker = match_ ? match_->resolve(credited_attacker) : nullptr;
    if (credited_attacker &&
        !(friendly_source ? friendly_source(credited_attacker) : attacker && friendly(attacker)))
        side_event(health <= (maximum >> 2) ? 4 : 10);
    return false;
}
bool CreatureActor::damage(Match& match, Damage value) {
    if (!damageable())
        return false;
    const auto blood_source = value.source; // 0x459950 retains its original argument across watchers.
    mitigate(value);
    attribute(value);
    std::array<std::uint32_t, 3> words{static_cast<std::uint32_t>(value.type),
                                       static_cast<std::uint32_t>(value.amount), value.source};
    match.fire(*this, 0, words); // Watchers see mitigated damage and may rewrite all three words.
    value = {std::bit_cast<std::int32_t>(words[0]), std::bit_cast<std::int32_t>(words[1]), words[2]};
    state_flags &= ~0x1000u;
    replication_flags |= 0x20;
    if (value.amount < 0)
        return true; // Accepted negative damage still attributes/fires watchers; it does not heal.
    if (lose_health(value.amount)) {
        lethal(match, value.amount);
        return true;
    }
    const auto maximum = stats.base[CreatureStats::health];
    const auto event = (value.type & 7) || value.amount <= (maximum >> 2) ? 6 : 7;
    if (fsm_->event(event))
        combat_sound(FourCC::from_text("Xdam"));
    if (!maximum)
        throw Error("Creature blood count requires nonzero base health");
    const auto numerator = add(multiply(32, value.amount), random_->next() & 31);
    const auto count = add(std::bit_cast<std::int32_t>(
                               static_cast<std::uint32_t>(static_cast<std::int64_t>(numerator) / maximum)),
                           4);
    combat_requests.push_back({CombatRequest::Kind::blood, count, blood_source, {}, {}, 5});
    SpellActor::emit_blood(match, *this, match.resolve(blood_source), count, 5);
    return true;
}
void CreatureActor::accumulate_damage(Damage value, int ticks) {
    if (ticks < 0)
        throw Error("Negative creature damage ticks");
    if (!damageable())
        return;
    value.amount = multiply(value.amount, ticks);
    mitigate(value);
    accumulated_damage = add(accumulated_damage, value.amount);
    accumulated_source_ = value.source;
}
void CreatureActor::tick_damage(Match& match, int ticks) {
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
    Damage value{0, accumulated_damage / 60, accumulated_source_};
    accumulated_damage %= 60;
    attribute(value);
    accumulated_source_ = 0;
    if (value.amount > 0) {
        replication_flags |= 0x20;
        if (lose_health(value.amount) && network_identity)
            lethal(match, 0); // Native accumulated path passes zero to the gib probability test.
    }
}
void CreatureActor::kill(Match& match) {
    lethal(match, 0);
}
void CreatureActor::lethal(Match& match, int amount) {
    // 0x453820 reads blood level/settings, not an object's damage resistance. Float stores matter.
    bool gibbed = false;
    if (!(common_status & 1)) {
        if (force_gibs)
            gibbed = true;
        else {
            const auto maximum = stats.base[CreatureStats::health];
            const double reciprocal = 1.0 / maximum;
            const float scale = static_cast<float>(gore_level * gore_level * 32.0);
            const float missing =
                static_cast<float>(std::max(1.0 - stats.current[CreatureStats::health] * reciprocal, 0.0));
            const float fraction = static_cast<float>(std::min(amount * reciprocal, 1.0));
            const float excess = static_cast<float>(std::min(
                (static_cast<double>(amount) - stats.current[CreatureStats::health]) * reciprocal, 1.0));
            gibbed = static_cast<double>(excess) * fraction * missing * scale * 0.02 > random_->next();
        }
    }
    if (gibbed && type != MatchObject::wizard) {
        gib(false);
        if (credited_attacker) {
            combat_requests.push_back({CombatRequest::Kind::kill_credit, 160, credited_attacker});
            if (auto* killer = dynamic_cast<CreatureActor*>(match.resolve(credited_attacker)))
                killer->credit_kill(*this, 0x20);
        }
        return;
    }
    stats.current[CreatureStats::health] = 0;
    if (fsm_->event(5)) {
        combat_sound(FourCC::from_text("Xdie"));
        const auto count = (random_->next() >> 10) + 16;
        combat_requests.push_back({CombatRequest::Kind::blood, count, 0, {}, {}, 10});
        SpellActor::emit_blood(match, *this, nullptr, count, 10);
        if (credited_attacker) {
            combat_requests.push_back({CombatRequest::Kind::kill_credit, 156, credited_attacker});
            if (auto* killer = dynamic_cast<CreatureActor*>(match.resolve(credited_attacker)))
                killer->credit_kill(*this, 0x80);
        }
    }
}
void CreatureActor::freeze_animation() {
    rate_ = 0;
    if (clip_)
        frame_ = static_cast<float>(clip_->animation.frames.size() - 1);
    ++pose_revision_;
}
void CreatureActor::make_corpse(Match& match) {
    if (type == MatchObject::wizard) {
        match.fire(*this, 2);
        fsm_->event(30);
        freeze_animation();
        return;
    }
    complete_corpse(match);
}
void CreatureActor::complete_corpse(Match& match) {
    match.fire(*this, 2);
    if (soul_count && !(kind_flags & MatchObject::wizard)) {
        const auto released = soul_count;
        if (network_identity) {
            if (match.create_souls)
                match.create_souls(motion.position, this, released);
            else
                combat_requests.push_back({CombatRequest::Kind::release_souls, released});
        }
        change_souls(std::bit_cast<int>(0u - static_cast<unsigned>(released)));
    }
    combat_requests.push_back({CombatRequest::Kind::death_notification, 15});
    set_guard(match, nullptr);
    match.notify_reference(*this, 15);
    freeze_animation();
    phase = 0;
    kind_flags = (kind_flags & 0xfffffff2u) | periodic_flag;
}
void CreatureActor::gib(bool suppress_souls) {
    combat_requests.push_back({CombatRequest::Kind::gibs, 8});
    if (soul_count) {
        const auto released = soul_count;
        if (network_identity && !suppress_souls) {
            if (match_ && match_->create_souls)
                match_->create_souls(motion.position, this, released);
            else
                combat_requests.push_back({CombatRequest::Kind::release_souls, released});
        }
        change_souls(std::bit_cast<int>(0u - static_cast<unsigned>(released)));
    }
    state_flags |= removed_flag;
}
void CreatureActor::die(Match& match, int) {
    if (!network_identity)
        return;
    if (type != MatchObject::wizard) {
        gib(true); // 0x4425e0 ignores the reason and requests full disintegration, without souls.
        return;
    }
    // 0x4424d0: boundary/deep-hole recovery chooses a free point around the home altar.
    if (home_altar && !(home_altar->common_status & 0x100) && match.terrain && match.spatial) {
        const auto center = home_altar->motion.position;
        const double dx = static_cast<double>(motion.position.x) - center.x,
                     dy = static_cast<double>(motion.position.y) - center.y,
                     dz = static_cast<double>(motion.position.z) - center.z,
                     length = std::sqrt(dz * dz + dx * dx + dy * dy);
        const double factor = length != 0 ? 20 / length : 1;
        Vec3 point{static_cast<float>(dx * factor + center.x), static_cast<float>(dy * factor + center.y),
                   static_cast<float>(dz * factor + center.z)};
        const auto free = [&] {
            point.z = static_cast<float>(terrain_surface(*match.terrain, point.x, point.y));
            return point.z > 0 && !match.spatial->occupied(point, 21);
        };
        int remaining = 32;
        while (!free()) {
            if (remaining-- == 0)
                break;
            const float angle =
                static_cast<float>(random_->next() * 6.2831802 * .000030517578125 - 3.141590118408203);
            const double radius = random_->next() * 50.0 * .000030517578125;
            point.x = static_cast<float>(std::sin(angle) * radius + center.x);
            point.y = static_cast<float>(std::cos(angle) * radius + center.y);
        }
        if (remaining > 0) {
            relocate(match, point);
            return;
        }
    }
    combat_requests.push_back({CombatRequest::Kind::gibs, 8});
    eliminate_wizard(match);
}
} // namespace sacrifice
