#pragma once
#include "spell_data.hpp"
#include "wad.hpp"
#include "wizard.hpp"
#include <memory>

namespace sacrifice {
// Common definition prefix copied eight bytes into the original runtime allocation.
// SPEL consumes 310 bytes (stock resources contain 314), STRC 64, CRE8 528, WIZD 840.
struct SpellDefinition {
    FourCC name, id, icon;
    std::uint32_t kind{}; // 2 creature, 4 spell, 8 wizard, 16 structure.
    std::uint16_t priority{}, fast_cast_ticks{}, slow_cast_ticks{};
    std::shared_ptr<const SpellData> spell; // Only kind 4; function/parameter fields from native prefix.
    std::int32_t cooldown{}, mana_cost{}, required_souls{};
    float range{};           // Serialized +28, shared by every definition kind.
    std::uint32_t targets{}; // Serialized +32, used by creature attack-capability classification.
    // STRC +44..+60: BLDG ids for gods 1..5, chosen by 0x450890 (<= 1 first, >= 5 last).
    std::array<FourCC, 5> structures{};
    static SpellDefinition decode(FourCC tag, ByteView bytes);
};
// 0x408d50 / 0x44ee70 / 0x44f650. Categories append; definitions insert before equal priorities.
class SpellRegistry {
  public:
    struct Group {
        FourCC id;
        std::vector<SpellDefinition> definitions;
    };
    void add_group(FourCC id);
    void add(FourCC group, SpellDefinition definition);
    const Group* group(FourCC id) const;
    const SpellDefinition* find(FourCC id) const; // 0x44f740, first definition in registry traversal.
    FourCC group_of(FourCC id) const;             // 0x44ee30, first containing root category.
    const std::vector<Group>& groups() const {
        return groups_;
    }
    bool learn(WizardProgression& state, FourCC id) const; // 0x44fab0, unique IDs, no @ categories.
    unsigned grant(WizardProgression& state) const;        // 0x44fb50, current god/level.
    bool add_to_books(std::array<CreatureSpellBook, 3>& books, FourCC id, bool enabled = false) const;
    void synchronize(const WizardProgression& state, std::array<CreatureSpellBook, 3>& books) const;
    static SpellRegistry load(const ResourceCatalog& catalog);

  private:
    std::vector<Group> groups_;
};
} // namespace sacrifice
