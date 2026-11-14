#include "sacrifice/player_pointer.hpp"
#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/soul_actor.hpp"
#include "sacrifice/side.hpp"
#include <cmath>

namespace sacrifice {
void PlayerPointer::clear_target() {
    retained_ = 0;
    grace_ = 0;
}
MatchObject* PlayerPointer::retained(const Match& match) const {
    auto* object = match.resolve(retained_);
    return object && !(object->state_flags & MatchObject::removed_flag) ? object : nullptr;
}
void PlayerPointer::advance(float seconds) {
    grace_ -= std::max(seconds, 0.0f);
    if (grace_ <= 0)
        clear_target();
}
MatchObject* PlayerPointer::retain(Match& match, MatchObject* physical, Vec2 pixel, unsigned width,
                                  unsigned mask, bool valid) {
    if (physical && (physical->kind_flags & mask) && valid) {
        retained_ = physical->reference();
        previous_ = pixel;
        grace_ = 1.5f;
        return physical;
    }
    auto* object = retained(match);
    if (!object || !(object->kind_flags & 7) || !(object->kind_flags & mask)) {
        clear_target();
        return physical;
    }
    // Distance is normalized to 640 pixels; the native grace is measured in seconds.
    grace_ -= std::hypot(pixel.x - previous_.x, pixel.y - previous_.y) * 32.0f / std::max(width, 1u);
    previous_ = pixel;
    if (grace_ <= 0) {
        clear_target();
        return physical;
    }
    return object;
}
void PlayerPointer::load(const ResourceCatalog& catalog) {
    modes_.clear();
    for (const auto* name : {"norm", "targ", "xmnu", "drag", "mdrg", "idrg", "NULL"}) {
        const auto mode = FourCC::from_text(name);
        if (const auto location = catalog.find_chunk(FourCC::from_text("IFAC"), mode))
            modes_.push_back({mode, PlayerControl::decode_sound_rules(catalog.read(*location))});
    }
    reset();
}
FourCC PlayerPointer::icon(FourCC mode, FourCC context) const {
    for (const auto& entry : modes_)
        if (entry.id == mode)
            for (const auto& row : entry.rows)
                if ((row.command == context || row.command.value == 0xffffffffu) &&
                    (!row.result.value || row.result.value == 0xffffffffu))
                    return row.sound; // The third IFAC word is the cursor ICON for result zero.
    return FourCC::from_text("Cnor");
}
FourCC PlayerPointer::context(const PlayerControl& player, const MatchObject* target, bool ground) {
    const auto id = FourCC::from_text;
    if (!target)
        return ground ? id("land") : id("sds!");
    const auto* side = target->allegiance;
    const auto& own = player.side();
    const bool hostile = side && side->identity != own.identity &&
                         ((side->enemies & own.identity) || (own.enemies & side->identity));
    if (const auto* soul = dynamic_cast<const SoulActor*>(target))
        return (soul->soul_flags & 0xc0000000) ? id("sds!")
            : (soul->eligible_sides & player.side().identity) ? id("SPIR") : id("spir");
    if (const auto* building = dynamic_cast<const BuildingActor*>(target)) {
        if (!(building->building_flags & 0x55b7))
            return id("sds!");
        if (building->allegiance == &player.side())
            return id("fbdg");
        if (hostile)
            return id("ebdg");
        return building->primary || (building->building_flags & 0x2810) != 0x2810 || building->reserved
            ? id("nbdg") : id("font");
    }
    if (target->kind_flags & 5) {
        if (hostile)
            return id("eunt");
        if (target->state_flags & 0x100000)
            return id("tunt");
        if (target->allegiance == &player.side())
            return id("funt");
        return target->common_status & 8 ? id("runt") : id("nunt");
    }
    return target->kind_flags & 8 ? id("corp") : id("sds!");
}
void PlayerPointer::hover(std::uint32_t identity, FourCC context, float ticks) {
    if (identity != identity_) {
        identity_ = identity;
        context_ = context;
        elapsed_ = 0;
        carry_ = 0;
    } else if (identity_) {
        carry_ += std::max(ticks, 0.0f);
        const auto whole = static_cast<int>(carry_);
        carry_ -= static_cast<float>(whole);
        elapsed_ = std::min(elapsed_ + whole, 3600);
    }
    context_ = context;
}
} // namespace sacrifice
