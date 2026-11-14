#pragma once
#include "creature.hpp"

namespace sacrifice {
// Mutable WIZD copy at wizard +4336. 0x44f2f0 discards the serialized progression fields.
struct WizardProgression {
    static constexpr std::array<int, 15> thresholds{8000,   16000,  24000,  32000,  48000,
                                                    64000,  80000,  96000,  128000, 132000,
                                                    164000, 196000, 220000, 236000, 264000};
    static constexpr std::array<std::int16_t, 24> initial_stats{
        1250, 1250, 1000, 1000, 0,    1500, 1000, 0, 0,    1000, 500, 500,
        1000, 1000, 0,    1000, 2000, 2000, 0,    0, 1000, 0,    0,   0};
    std::array<std::int16_t, 24> stats = initial_stats;
    std::array<std::uint8_t, 28> gods{}; // Runtime definition +536; god is stat 18.
    std::int32_t level{}, experience{}, maximum_mana{1000}, auxiliary{120};
    std::int32_t credit_count_80{}, credit_count_20{}; // Runtime definition +556/+560.
    std::vector<FourCC> learned; // Runtime +572 count / +576 IDs, insertion order retained on level loss.
    // Native 0x44fb50 runs once after each upward step, after the new god/stat stores.
    using Grant = std::function<void(WizardProgression&)>;
    int adjust(int delta, int minimum, int maximum, const Grant& grant = {});
    void set(int level, int experience, const Grant& grant = {}); // 0x44fc90, limits 0..14.
    // 0x4833a0 preserves current/target proportions in slots 0, 1 and 5.
    void synchronize(CreatureStats& actor, std::int32_t& mana, std::int32_t& maximum) const;
};
} // namespace sacrifice
