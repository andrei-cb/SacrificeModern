#include "sacrifice/spell_data.hpp"

namespace sacrifice {
namespace {
constexpr SpellFunction functions[] = {
#include "spell_functions.inc"
};
std::uint32_t resolve(std::string_view name, bool evaluator) {
    const auto* function = spell_function(name, evaluator);
    return function ? spell_function_id(function->name) : 0;
}
} // namespace
std::uint32_t spell_function_id(std::string_view name) {
    if (name.find('\0') != std::string_view::npos)
        name = name.substr(0, name.find('\0'));
    const auto size = name.size() + 1;
    std::uint32_t result = 0;
    std::size_t offset = 0;
    const auto byte = [&](std::size_t i) {
        return i < name.size() ? static_cast<unsigned char>(name[i]) : 0u;
    };
    for (; offset + 4 <= size; offset += 4)
        result +=
            byte(offset) | (byte(offset + 1) << 8) | (byte(offset + 2) << 16) | (byte(offset + 3) << 24);
    if (offset < size) {
        unsigned shift = static_cast<unsigned>(8 * (3 - (size - offset)));
        for (; offset < size; ++offset, shift += 8)
            result += byte(offset) << shift;
    }
    return result;
}
std::span<const SpellFunction> spell_functions() {
    return functions;
}
const SpellFunction* spell_function(std::string_view name, bool evaluator) {
    name = name.substr(0, name.find('\0'));
    if (name.empty())
        return nullptr;
    for (const auto& function : functions)
        if (function.evaluator == evaluator && function.name.substr(0, 27) == name.substr(0, 27))
            return &function;
    return nullptr;
}
SpellData SpellData::decode(ByteView bytes) {
    if (bytes.size() < 310)
        throw Error("Truncated SPEL prefix");
    SpellData result;
    Reader fields(bytes.first(310), "SPEL fields");
    fields.take(20);
    result.cooldown = std::bit_cast<int>(fields.u32());
    result.mana_cost = std::bit_cast<int>(fields.u32());
    result.range = fields.f32();
    result.targets = fields.u32();
    fields.take(8);
    for (auto& name : result.blocked_effects)
        name = {fields.u32()};
    result.radius = fields.f32();
    result.damage_type = std::bit_cast<int>(fields.u32());
    result.amount = std::bit_cast<int>(fields.u32());
    result.speed = fields.f32();
    result.acceleration = fields.f32();
    result.duration = fields.f32();
    result.spread_limit = fields.f32();
    result.flags = fields.u32();
    result.property_flags = fields.u32();
    result.ai_power = std::bit_cast<int>(fields.u32());
    result.count = std::bit_cast<int>(fields.u32());
    fields.take(8);
    for (auto& sound : result.sounds) {
        sound.events = fields.u8();
        sound.flags = fields.u8();
        sound.group = {fields.u32()};
    }
    for (auto& parameter : result.parameters) {
        parameter.name = {fields.u32()};
        parameter.word = fields.u32();
    }
    result.evaluator = fields.fixed_string(28);
    fields.take(8);
    result.run = fields.fixed_string(28);
    fields.take(8);
    result.end = fields.fixed_string(28);
    result.evaluator_id = resolve(result.evaluator, true);
    if (!result.evaluator_id) {
        std::string_view fallback;
        if (result.flags & 0x40000000)
            fallback = result.flags & 0x400000 ? "EvalSpell_DamageArea" : "EvalSpell_Damage";
        else if (result.flags & 0x80000000)
            fallback = "EvalSpell_Heal";
        else if (result.flags & 0x8000000)
            fallback = "EvalSpell_Paralyze";
        else if (result.flags & 0x4000000)
            fallback = "EvalSpell_Protect";
        result.evaluator_id = resolve(fallback, true);
    }
    result.run_id = resolve(result.run, false);
    if (!result.run_id)
        result.run_id = resolve("NullSpellRun", false);
    result.end_id = resolve(result.end, false);
    return result;
}
std::int32_t SpellData::integer(FourCC name, std::int32_t fallback) const {
    for (const auto& parameter : parameters)
        if (parameter.name == name)
            return std::bit_cast<int>(parameter.word);
    return fallback;
}
float SpellData::real(FourCC name, float fallback) const {
    for (const auto& parameter : parameters)
        if (parameter.name == name)
            return std::bit_cast<float>(parameter.word);
    return fallback;
}
} // namespace sacrifice
