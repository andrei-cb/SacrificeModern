#pragma once
#include "core.hpp"
#include "player_input.hpp"
#ifdef _WIN32
#include <Windows.h>
#else
struct SDL_Window;
#endif
#include <array>

namespace sacrifice {
#ifdef _WIN32
using NativeWindow = HWND;
#else
using NativeWindow = SDL_Window*;
#endif
struct InputState {
    std::array<bool, 256> keys{};
    float mouse_x{}, mouse_y{};
    bool looking{};
    bool pointer_inside{};
    // Ordered client-area pointer gestures survive a down/move/up sequence in one message pump.
    std::vector<PointerEvent> pointer_events;
    bool pointer_cancelled{};
    float cursor_x{}, cursor_y{};
    // DirectInput scan codes (extended keys | 0x80) for the native key bindings, and this frame's wheel.
    std::array<bool, 256> scancodes{};
    std::array<bool, 256> pressed_codes{}; // Retain a short press even if key-up arrives in the same pump.
    std::array<std::uint8_t, 256> press_modifiers{}; // Ctrl=1, Shift=2, Alt=4 at that key-down.
    int wheel{};
};
// Whether this frame has a click (for example a cinematic line's advance).
inline bool clicked(const InputState& input) {
    for (const auto& event : input.pointer_events)
        if (event.kind == PointerEvent::Kind::click)
            return true;
    return false;
}
class Window {
  public:
    explicit Window(bool hidden = false);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    [[nodiscard]] NativeWindow handle() const {
        return handle_;
    }
    [[nodiscard]] bool pump();
    void native_cursor(bool enabled);
    // Port window mode: borderless fullscreen on the window's monitor, or back to the normal window. The
    // browser asks for fullscreen, which it grants on the next user gesture.
    void borderless(bool fullscreen);
#ifdef __EMSCRIPTEN__
    // Right-button pointer lock belongs to the free camera, not the command cross.
    bool free_camera{true};
#endif
    void swap_buttons(bool swap) { // Option 'swap' (0x406400 sets the input device's +132).
        swap_ = swap;
    }
    [[nodiscard]] const InputState& input() const {
        return input_;
    }
    [[nodiscard]] unsigned width() const {
        return width_;
    }
    [[nodiscard]] unsigned height() const {
        return height_;
    }

  private:
#ifdef _WIN32
    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l);
#endif
    NativeWindow handle_{};
    unsigned width_{1280}, height_{800};
    InputState input_;
    PointerDevice pointer_;
    bool swap_{};
    bool native_cursor_{};
    bool borderless_{};
#ifdef _WIN32
    WINDOWPLACEMENT placement_{sizeof(WINDOWPLACEMENT)};
#endif
};
} // namespace sacrifice
