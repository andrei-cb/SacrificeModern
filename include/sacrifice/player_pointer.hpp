#pragma once
#include "player.hpp"
#include "wad.hpp"

namespace sacrifice {
// IFAC cursor rows (0x4604d0) and the identity-based, >30 tick hover delay (0x406430/0x406c40).
class PlayerPointer {
  public:
    void load(const ResourceCatalog& catalog);
    FourCC icon(FourCC mode, FourCC context) const;
    static FourCC context(const PlayerControl& player, const MatchObject* target, bool ground);
    // 0x461470 / 0x452470: 1.5-second creature/soul target grace, depleted by pointer travel.
    void advance(float seconds);
    MatchObject* retain(Match& match, MatchObject* physical, Vec2 pixel, unsigned width,
                        unsigned mask, bool valid);
    MatchObject* retained(const Match& match) const;
    void clear_target();
    // `ticks` may be fractional: high frame rates accumulate the 60 Hz interface ticks rather than
    // counting each frame as one, which made cards appear early.
    void hover(std::uint32_t identity, FourCC context, float ticks);
    bool ready() const { return identity_ && elapsed_ > 30; }
    void reset() { identity_ = elapsed_ = 0; carry_ = 0; context_ = {}; clear_target(); }
  private:
    struct Mode { FourCC id; std::vector<PlayerControl::SoundRule> rows; };
    std::vector<Mode> modes_;
    std::uint32_t identity_{};
    FourCC context_;
    int elapsed_{};
    float carry_{};
    std::uint32_t retained_{};
    float grace_{};
    Vec2 previous_;
};
} // namespace sacrifice
