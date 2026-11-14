#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/spatial.hpp"
#include <cmath>

namespace sacrifice {
namespace {
int integer(double value) {
    if (!std::isfinite(value) || value >= 9223372036854775808.0 || value < -9223372036854775808.0)
        return 0; // Low word of the original _ftol indefinite integer.
    return std::bit_cast<int>(static_cast<unsigned>(static_cast<std::int64_t>(value)));
}
} // namespace
MatchObject* CreatureActor::melee_target(float length, unsigned mask) const {
    if (!spatial_)
        return nullptr;
    const auto* selected = spatial_->melee(motion, length, mask);
    if (!selected)
        return nullptr;
    for (const auto& object : match_->objects())
        if (object->source_motion() == selected)
            return object.get();
    throw Error("Melee spatial object has no match host");
}
int CreatureActor::melee_hit(MatchObject& target, float seconds) {
    if (!(target.kind_flags & 0x15))
        return 0;
    combat_sound(FourCC::from_text(target.kind_flags & 0x10 ? "Xstn" : "Xhit"));
    if (friendly(&target) && &target != effect_target)
        return 0;
    int threshold = 0x8000;
    if (target.kind_flags & 5) {
        const auto* victim = dynamic_cast<const CreatureActor*>(&target);
        if (!victim)
            throw Error("Melee character target has no creature host");
        const auto& other = victim->motion.orientation;
        const auto& own = motion.orientation;
        const double facing =
            (static_cast<double>(own[5]) * other[5] + static_cast<double>(own[4]) * other[4] +
             static_cast<double>(own[3]) * other[3]) *
                0.5 +
            1.5;
        seconds = static_cast<float>(facing * seconds);
        threshold =
            integer((32767.0 * stats.current[0]) / (victim->stats.current[1] / facing + stats.current[0]));
    }
    const int draw = random_->next();
    const int fixed_time = integer(draw < threshold ? seconds * 256.0
                                                    : ((32768 - draw) << 8) * static_cast<double>(seconds) /
                                                          (32768.0 - threshold));
    if (fixed_time <= 0)
        return 0;
    // 0x44f2f0 replaces these WIZD definition fields at load time, alongside its base stats/mana.
    const int base_damage = type == MatchObject::wizard ? 120 : resources_->definition.melee_damage;
    const int amount =
        std::bit_cast<int>(static_cast<unsigned>(fixed_time) * static_cast<unsigned>(base_damage)) >> 8;
    const auto kind = type == MatchObject::wizard ? 0 : resources_->definition.melee_damage_type;
    if (auto* victim = dynamic_cast<CreatureActor*>(&target)) {
        victim->damage(*match_, {kind, amount, reference()});
    } else if (auto* building = dynamic_cast<BuildingActor*>(&target)) {
        building->damage(*match_, {static_cast<unsigned>(kind), amount, reference()});
    } else
        throw Error("Melee target damage host is not implemented");
    notify_event(56, &target); // Attacker's event-spell notifier, not the victim's FSM event.
    return amount;
}
} // namespace sacrifice
