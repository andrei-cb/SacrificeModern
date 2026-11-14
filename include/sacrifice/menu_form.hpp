#pragma once
#include "hud.hpp"

namespace sacrifice {
// FORM record consumed by 0x4adca0: 49-byte header, then 88 bytes per widget.
struct MenuForm {
    struct Widget {
        unsigned kind{};
        FourCC id, text, help;
        HudRect rectangle;
        FourCC texture, icon;
        std::array<std::uint32_t, 2> values{};
        std::uint32_t flags{}, parameter{};
        std::array<FourCC, 8> callbacks{};
    };
    FourCC id, title, accept, cancel, alternate;
    HudRect rectangle;
    std::uint32_t value{}, flags{};
    std::uint8_t alpha{};
    std::vector<Widget> widgets;
    static MenuForm decode(ByteView data);
    // 0x4ae060 centers axes selected by bits 0x10/0x20 in physical screen pixels.
    HudRect bounds(unsigned width, unsigned height) const;
    HudRect content(HudRect bounds) const; // 0x4aa6d0: 16px border, optional 32px title.
    HudRect bounds(const Widget& widget, HudRect content) const;
};
} // namespace sacrifice
