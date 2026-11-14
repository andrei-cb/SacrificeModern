#include "sacrifice/menu_form.hpp"

namespace sacrifice {
MenuForm MenuForm::decode(ByteView data) {
    Reader r(data, "FORM");
    const auto signed_word = [&] { return std::bit_cast<std::int32_t>(r.u32()); };
    MenuForm form;
    form.id = FourCC{r.u32()};
    form.title = FourCC{r.u32()};
    const auto y = signed_word(), x = signed_word(), width = signed_word(), height = signed_word();
    if (width <= 0 || height <= 0)
        throw Error("Invalid FORM dimensions");
    form.rectangle = {{static_cast<float>(x), static_cast<float>(y)},
                      {static_cast<float>(width), static_cast<float>(height)}};
    form.value = r.u32();
    form.accept = FourCC{r.u32()};
    form.cancel = FourCC{r.u32()};
    form.alternate = FourCC{r.u32()};
    form.alpha = r.u8();
    form.flags = r.u32();
    const auto count = r.u32();
    if (count > (data.size() - 49) / 88)
        throw Error("Truncated FORM widgets");
    for (unsigned i = 0; i < count; ++i) {
        Widget w;
        w.kind = r.u32() & 255;
        w.id = FourCC{r.u32()};
        w.text = FourCC{r.u32()};
        w.help = FourCC{r.u32()};
        const auto top = signed_word(), left = signed_word(), wide = signed_word(), high = signed_word();
        w.rectangle = {{static_cast<float>(left), static_cast<float>(top)},
                       {static_cast<float>(wide), static_cast<float>(high)}};
        w.texture = FourCC{r.u32()};
        w.icon = FourCC{r.u32()};
        for (auto& v : w.values)
            v = r.u32();
        w.flags = r.u32();
        w.parameter = r.u32();
        for (auto& callback : w.callbacks)
            callback = FourCC{r.u32()};
        form.widgets.push_back(w);
    }
    return form;
}
HudRect MenuForm::bounds(unsigned width, unsigned height) const {
    auto b = rectangle;
    if (flags & 0x10)
        b.origin.x = static_cast<float>(static_cast<int>((width - b.size.x) / 2));
    if (flags & 0x20)
        b.origin.y = static_cast<float>(static_cast<int>((height - b.size.y) / 2));
    return b;
}
HudRect MenuForm::content(HudRect b) const {
    if (flags & 2) {
        b.origin.x += 16;
        b.origin.y += flags & 1 ? 32 : 16;
        b.size.x -= 32;
        b.size.y -= flags & 1 ? 48 : 32;
    }
    return b;
}
HudRect MenuForm::bounds(const Widget& widget, HudRect parent) const {
    auto b = widget.rectangle;
    if (widget.kind == 1)
        b.size.y = widget.flags & 0x80000 ? 24.0f : 16.0f;
    else if (widget.kind == 10)
        b.size.y = std::max(b.size.y, 16.0f);
    if (widget.flags & 2)
        b.origin.x = static_cast<float>(static_cast<int>((parent.size.x - b.size.x) / 2));
    if (widget.flags & 4)
        b.origin.y = static_cast<float>(static_cast<int>((parent.size.y - b.size.y) / 2));
    b.origin.x += parent.origin.x;
    b.origin.y += parent.origin.y;
    return b;
}
} // namespace sacrifice
