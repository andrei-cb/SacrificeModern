#include "sacrifice/player_input.hpp"
#include <cmath>

namespace sacrifice {
void PointerDevice::press(PointerButton button, Vec2 point, std::uint8_t modifiers, bool twice,
                          std::vector<PointerEvent>& events) {
    auto& state = buttons_[static_cast<unsigned>(button)];
    state = {true, false, twice, point, point};
    events.push_back({PointerEvent::Kind::press, button, point, point, {}, modifiers});
}
void PointerDevice::move(Vec2 point, std::uint8_t modifiers, std::vector<PointerEvent>& events) {
    for (unsigned i = 0; i < buttons_.size(); ++i) {
        auto& state = buttons_[i];
        if (!state.held)
            continue;
        const auto button = static_cast<PointerButton>(i);
        if (!state.dragging && (std::abs(point.x - state.origin.x) >= threshold ||
                               std::abs(point.y - state.origin.y) >= threshold)) {
            state.dragging = true;
            events.push_back({PointerEvent::Kind::drag_start, button, state.origin, state.origin, {}, modifiers});
        }
        if (state.dragging && (point.x != state.previous.x || point.y != state.previous.y)) {
            events.push_back({PointerEvent::Kind::drag, button, point, state.origin,
                              {point.x - state.previous.x, point.y - state.previous.y}, modifiers});
            state.previous = point;
        }
    }
}
void PointerDevice::release(PointerButton button, Vec2 point, std::uint8_t modifiers,
                            std::vector<PointerEvent>& events) {
    auto& state = buttons_[static_cast<unsigned>(button)];
    if (!state.held)
        return;
    // Some window-targeted input sends the final coordinates only on button-up.
    move(point, modifiers, events);
    const auto kind = state.dragging ? PointerEvent::Kind::drag_end
                     : state.twice ? PointerEvent::Kind::double_click : PointerEvent::Kind::click;
    events.push_back({kind, button, point, state.origin, {}, modifiers});
    state = {};
}
bool PointerDevice::held() const {
    return std::ranges::any_of(buttons_, [](const Button& button) { return button.held; });
}
SelectionRectangle SelectionRectangle::make(Vec2 from, Vec2 to, unsigned width, unsigned height) {
    return {{std::clamp(std::min(from.x, to.x), 0.0f, static_cast<float>(width)),
             std::clamp(std::min(from.y, to.y), 0.0f, static_cast<float>(height))},
            {std::clamp(std::max(from.x, to.x), 0.0f, static_cast<float>(width)),
             std::clamp(std::max(from.y, to.y), 0.0f, static_cast<float>(height))}};
}
bool SelectionRectangle::contains(Vec2 point) const {
    return minimum.x <= point.x && maximum.x >= point.x && minimum.y <= point.y && maximum.y >= point.y;
}
} // namespace sacrifice
