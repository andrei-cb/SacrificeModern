#include "sacrifice/spell_registry.hpp"
#include <set>

namespace sacrifice {
namespace {
FourCC cc(std::string_view text) {
    return FourCC::from_text(text);
}
bool definition_tag(FourCC tag) {
    return tag == cc("SPEL") || tag == cc("CRE8") || tag == cc("WIZD") || tag == cc("STRC");
}
bool skip_children(const ResourceRecord& record) {
    // WAD descriptor flag 4 and native resource-type flag 0x40 (0x4c7ca8 table).
    return (record.flags & 4) || record.tag == cc("LAND") || record.tag == cc("SAXC") ||
           record.tag == cc("3DSC") || record.tag == cc("MRMC");
}
} // namespace
SpellDefinition SpellDefinition::decode(FourCC tag, ByteView bytes) {
    const auto kind = tag == cc("CRE8")   ? 2u
                      : tag == cc("SPEL") ? 4u
                      : tag == cc("WIZD") ? 8u
                      : tag == cc("STRC") ? 16u
                                          : 0u;
    const auto size = kind == 2 ? 528u : kind == 4 ? 310u : kind == 8 ? 840u : 64u;
    if (!kind || bytes.size() < size)
        throw Error("Truncated or unsupported spell definition");
    Reader reader(bytes.first(size), "spell definition prefix");
    SpellDefinition result;
    result.name = {reader.u32()};
    result.kind = reader.u32();
    if (result.kind != kind)
        throw Error("Spell definition tag/kind mismatch");
    result.id = {reader.u32()};
    result.icon = {reader.u32()};
    reader.take(4);
    result.cooldown = std::bit_cast<int>(reader.u32());
    result.mana_cost = std::bit_cast<int>(reader.u32());
    result.range = reader.f32();
    result.targets = reader.u32();
    reader.u16(); // Serialized fast cast time is replaced by 0x44ef50.
    result.slow_cast_ticks = static_cast<std::uint16_t>(3 * reader.u16() / 4);
    result.fast_cast_ticks = result.slow_cast_ticks >> 1;
    result.priority = reader.u16();
    if (kind == 4)
        result.spell = std::make_shared<SpellData>(SpellData::decode(bytes));
    if (kind == 16) {
        Reader buildings(bytes.subspan(44, 20), "STRC buildings");
        for (auto& id : result.structures)
            id = {buildings.u32()};
    }
    if (kind == 2) {
        Reader souls(bytes.subspan(82, 2), "CRE8 soul cost");
        result.required_souls = souls.i16();
    }
    return result;
}
void SpellRegistry::add_group(FourCC id) {
    groups_.push_back({id, {}});
}
const SpellRegistry::Group* SpellRegistry::group(FourCC id) const {
    const auto found = std::ranges::find(groups_, id, &Group::id);
    return found == groups_.end() ? nullptr : &*found;
}
void SpellRegistry::add(FourCC id, SpellDefinition definition) {
    auto found = std::ranges::find(groups_, id, &Group::id);
    if (found == groups_.end())
        throw Error("Missing spell category: " + id.text());
    const auto position = std::ranges::find_if(
        found->definitions, [&](const auto& item) { return item.priority >= definition.priority; });
    found->definitions.insert(position, std::move(definition));
}
const SpellDefinition* SpellRegistry::find(FourCC id) const {
    for (const auto& category : groups_) {
        const auto found = std::ranges::find(category.definitions, id, &SpellDefinition::id);
        if (found != category.definitions.end())
            return &*found;
    }
    return nullptr;
}
FourCC SpellRegistry::group_of(FourCC id) const {
    for (const auto& category : groups_)
        if (std::ranges::find(category.definitions, id, &SpellDefinition::id) != category.definitions.end())
            return category.id;
    return {};
}
bool SpellRegistry::learn(WizardProgression& state, FourCC id) const {
    if (!find(id) || (group_of(id).value >> 24) == '@' ||
        std::ranges::find(state.learned, id) != state.learned.end())
        return false;
    state.learned.push_back(id);
    return true;
}
unsigned SpellRegistry::grant(WizardProgression& state) const {
    // 0x4ce7e0, indexed by god (0 means no grant).
    static const std::array<std::array<FourCC, 3>, 5> categories{{{cc("Slif"), cc("Clif"), cc("free")},
                                                                  {cc("Sfir"), cc("Cfir"), cc("free")},
                                                                  {cc("Sear"), cc("Cear"), cc("free")},
                                                                  {cc("Sair"), cc("Cair"), cc("free")},
                                                                  {cc("Sdea"), cc("Cdea"), cc("free")}}};
    const auto god = state.stats[18];
    if (!god)
        return 0;
    if (god < 1 || god > 5)
        throw Error("Invalid wizard god for spell grant");
    unsigned count = 0;
    for (const auto id : categories[god - 1])
        if (const auto* category = group(id))
            for (const auto& definition : category->definitions)
                if (definition.priority / 100 == state.level)
                    count += learn(state, definition.id);
    return count;
}
bool SpellRegistry::add_to_books(std::array<CreatureSpellBook, 3>& books, FourCC id, bool enabled) const {
    const auto* definition = find(id);
    if (!definition)
        return false;
    const unsigned book = definition->kind == 2 ? 0
                          : definition->kind == 16 || id == cc("sacc") || id == cc("sacu") ||
                                  id == cc("grdn") || id == cc("tele")
                              ? 2
                              : 1;
    return books[book].add(id, definition->priority, enabled);
}
void SpellRegistry::synchronize(const WizardProgression& state,
                                std::array<CreatureSpellBook, 3>& books) const {
    // 0x4833a0 appends missing learned definitions; it never clears old book entries/cooldowns.
    for (const auto id : state.learned)
        add_to_books(books, id);
}
SpellRegistry SpellRegistry::load(const ResourceCatalog& catalog) {
    SpellRegistry registry;
    for (const auto& archive : catalog.archives()) {
        const auto& records = archive.records();
        std::set<std::size_t> loaded;
        for (std::size_t index = 0; index < records.size();) {
            const auto& record = records[index];
            if (record.tag == cc("SCAT")) {
                registry.add_group(record.name);
                // Native ProcessChunk starts with the category's children, so a flag-4 category
                // still preloads them. Nested categories are visited later by the outer scan;
                // their already-loaded definitions stay in this first category.
                for (auto child = index + 1; child < record.subtree_end;) {
                    const auto& item = records[child];
                    if (!item.folder() && definition_tag(item.tag) && loaded.insert(child).second)
                        registry.add(record.name, SpellDefinition::decode(item.tag, archive.read(child)));
                    child = skip_children(item) ? item.subtree_end : child + 1;
                }
            }
            index = skip_children(record) ? record.subtree_end : index + 1;
        }
    }
    return registry;
}
} // namespace sacrifice
