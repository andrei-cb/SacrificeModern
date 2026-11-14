#pragma once
#include "assets.hpp"

namespace sacrifice {
// BLDG header and 48-byte part records, copied by 0x4651f0 and instantiated by 0x457cb0.
struct Building {
    struct Part {
        FourCC name, model;
        std::array<FourCC, 3> models;
        std::uint32_t properties{}; // +16, retained for remaining spell/attachment callers.
        Vec3 offset;
        float angle{};
        FourCC start_spell, spell_40, destroyed_spell; // +36/+40/+44.
        FourCC model_at(unsigned state, std::uint32_t building_flags) const;
    };
    std::uint32_t flags{};
    // +4: 8x8 LAND tile indices (rows of increasing Y), stamped by 0x44cfa0 for sign-bit definitions.
    std::array<std::uint8_t, 64> footprint{};
    std::int32_t maximum_health{};
    float threshold_fraction{}, threshold_complement{}, regeneration_seconds{}, mana_rate{};
    std::array<float, 6> parameters{}; // +88..108, attachment/effect parameters.
    FourCC property, sound;            // +112/+116; +116 is the persistent sound id in 0x465910.
    FourCC primary;
    std::vector<Part> parts;
    unsigned primary_part() const; // First matching part, otherwise part zero (0x457c80).
    static Building decode(ByteView data);
};
} // namespace sacrifice
