#pragma once
#include "assets.hpp"

namespace sacrifice {
enum class PointerButton : unsigned { left, right, middle };
struct PointerEvent {
    enum class Kind { press, click, double_click, drag_start, drag, drag_end } kind{};
    PointerButton button{};
    Vec2 point, origin, delta;
    std::uint8_t modifiers{}; // Ctrl=1, Shift=2, Alt=4 at this event.
};
// 0x406800: a held button becomes a drag at four pixels on either axis. A drag release must
// not also produce a click. The Win32 host supplies ordered client positions and button edges.
class PointerDevice {
  public:
    void press(PointerButton button, Vec2 point, std::uint8_t modifiers, bool twice,
               std::vector<PointerEvent>& events);
    void move(Vec2 point, std::uint8_t modifiers, std::vector<PointerEvent>& events);
    void release(PointerButton button, Vec2 point, std::uint8_t modifiers,
                 std::vector<PointerEvent>& events);
    bool held() const;
    // The native four pixels at 96 DPI. A high-DPI host scales it, or ordinary click jitter turns into tiny
    // box drags that empty the selection.
    float threshold{4};

  private:
    struct Button {
        bool held{}, dragging{}, twice{};
        Vec2 origin, previous;
    };
    std::array<Button, 3> buttons_;
};
// 0x453120 normalizes and clips the box to its view; 0x47f750 includes all four edges.
struct SelectionRectangle {
    Vec2 minimum, maximum;
    static SelectionRectangle make(Vec2 from, Vec2 to, unsigned width, unsigned height);
    bool contains(Vec2 point) const;
};
} // namespace sacrifice
