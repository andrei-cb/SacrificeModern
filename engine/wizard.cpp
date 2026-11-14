#include "sacrifice/wizard.hpp"
#include <algorithm>
#include <cmath>
#include <format>

namespace sacrifice {
namespace {
int add(int a, int b) {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
std::int16_t short_word(int value) {
    return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(value));
}
std::int16_t truncate_short(double value) {
    // Native _ftol returns the 64-bit integer indefinite for nonfinite/out-of-range input;
    // its low word is zero. Avoid undefined C++ conversions for zero base stats.
    if (!std::isfinite(value) || value < -0x1p63 || value >= 0x1p63)
        return 0;
    return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(static_cast<std::int64_t>(value)));
}
} // namespace
int WizardProgression::adjust(int delta, int minimum, int maximum, const Grant& grant) {
    // Native 0x44fd20 does not order the limits: ja08 authors start level 8 with maximum 7, and a gain past
    // the level threshold then settles at the maximum. Only the threshold table bounds are checked here.
    if (level < 0 || level > 14 || maximum < 0 || maximum > 14)
        throw Error(std::format("Invalid wizard level limits: level {}, range {}..{}", level, minimum, maximum));
    experience = add(experience, delta);
    int change = 0;
    if (delta <= 0) {
        while (experience < 0 && level > minimum) {
            for (const auto index : {0, 1, 5})
                stats[index] = short_word(stats[index] - 50);
            maximum_mana = add(maximum_mana, -100);
            auxiliary = add(auxiliary, -18);
            --change;
            --level;
            experience = add(experience, thresholds[level]);
            stats[18] = gods[level];
        }
        if (experience < 0)
            experience = 0;
        return change;
    }
    if (experience <= thresholds[level])
        return 0; // Equality deliberately does not advance a level.
    while (level < maximum) {
        for (const auto index : {0, 1, 5})
            stats[index] = short_word(stats[index] + 50);
        maximum_mana = add(maximum_mana, 100);
        auxiliary = add(auxiliary, 18);
        experience = add(experience, -thresholds[level]);
        ++level;
        ++change;
        stats[18] = gods[level];
        if (grant)
            grant(*this);
        if (experience <= thresholds[level])
            return change;
    }
    level = maximum;
    experience = thresholds[maximum] - 1;
    return change;
}
void WizardProgression::set(int requested_level, int requested_experience, const Grant& grant) {
    const auto target = std::clamp(requested_level, 0, 14);
    while (level != target) {
        const auto delta =
            level < target ? thresholds.at(level) : std::bit_cast<int>(~static_cast<unsigned>(experience));
        adjust(delta, 0, 14, grant);
    }
    if (experience != requested_experience)
        adjust(add(requested_experience, std::bit_cast<int>(0u - static_cast<unsigned>(experience))), 0, 14,
               grant);
}
void WizardProgression::synchronize(CreatureStats& actor, std::int32_t& mana, std::int32_t& maximum) const {
    for (const auto index : {0, 1, 5}) {
        const double old_base = actor.base[index];
        const float ratio = static_cast<float>(actor.current[index] / old_base);
        const double target_ratio = actor.target[index] / old_base;
        actor.base[index] = stats[index];
        actor.current[index] = truncate_short(stats[index] * static_cast<double>(ratio));
        actor.target[index] = truncate_short(stats[index] * target_ratio);
        actor.changed |= 1u << index;
    }
    maximum = maximum_mana;
    if (mana > maximum)
        mana = maximum;
}
} // namespace sacrifice
