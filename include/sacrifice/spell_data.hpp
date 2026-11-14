#pragma once
#include "core.hpp"

namespace sacrifice {
// 0x450640 hashes the registered ASCII name including its NUL terminator, with an unusual tail shift.
std::uint32_t spell_function_id(std::string_view name);
struct SpellFunction {
    std::string_view name;
    bool evaluator{};
};
std::span<const SpellFunction> spell_functions(); // 272 run and 7 evaluator names; no executable addresses.
const SpellFunction* spell_function(std::string_view name, bool evaluator);
// SPEL's 310-byte native prefix. Remaining fields retain their original positions in the archive;
// these fields support function dispatch, parameters, healing and native projectile combat.
struct SpellData {
    std::int32_t cooldown{}, mana_cost{}, amount{};
    float range{};
    float radius{}, speed{}, acceleration{}, duration{}, spread_limit{}; // +84: PoisonRun squared-box cutoff.
    std::int32_t damage_type{};
    std::uint32_t targets{}, flags{};
    std::uint32_t property_flags{}; // Serialized +92, runtime +100.
    std::int32_t ai_power{}; // Serialized +96, runtime +104: estimated spell power for army comparisons.
    std::int32_t count{1};   // Serialized +100, runtime +108: halo rocks, divine pillars, Death victims.
    struct Sound {
        std::uint8_t events{}, flags{};
        FourCC group;
    };
    std::array<Sound, 5> sounds;           // Serialized +112: five packed six-byte sound-event records.
    std::array<FourCC, 4> blocked_effects; // +44: target may not carry these named references.
    struct Parameter {
        FourCC name;
        std::uint32_t word{};
    };
    std::array<Parameter, 8> parameters; // Serialized +142, eight unaligned key/value pairs.
    std::string evaluator, run, end;
    std::uint32_t evaluator_id{}, run_id{}, end_id{};
    std::int32_t integer(FourCC name, std::int32_t fallback) const;
    float real(FourCC name, float fallback) const;
    static SpellData decode(ByteView bytes);
};
} // namespace sacrifice
