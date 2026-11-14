#pragma once
#include "hud.hpp"

namespace sacrifice {
// XMNU callbacks 0x409d30/0x409d90; pooled cross-menu graph 0x482770..0x482d60.
class CommandMenu {
  public:
    struct Entry {
        FourCC id, text, icon, command, argument;
        std::array<int, 4> links{-1, -1, -1, -1}; // up, down, left, right.
    };
    void load(const ResourceCatalog& catalog);
    bool open(FourCC root, Vec2 point, int delay = -1); // 0x461840; compiled default 0x4dc018.
    void close();
    bool active() const { return current_ >= 0; }
    bool visible() const { return active() && remaining_ == 0; }
    Vec2 centre() const { return {static_cast<float>(x_), static_cast<float>(y_)}; }
    void tick(int ticks); // 0x452560, independent of the world's paused state.
    // Returns 'invu' or 'visu' when a new node requests the throttled native sound.
    FourCC move(Vec2 point); // 0x482b80 / 0x482c50.
    std::optional<Entry> select(Vec2 point); // 'xmsl': closes even when outside the cross.
    std::array<const Entry*, 5> displayed() const; // center, up, down, left, right (0x482990).
    const Entry* find(FourCC id) const;

  private:
    std::vector<Entry> entries_;
    int current_{-1}, x_{}, y_{}, delay_{}, remaining_{}, sound_clock_{};
    int index(FourCC id) const;
    int hit(Vec2 point, int& column, int& row) const;
};
} // namespace sacrifice
