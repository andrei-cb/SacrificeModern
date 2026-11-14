#include "sacrifice/platform.hpp"
#include <SDL.h>
#include <emscripten/html5.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <cmath>

namespace sacrifice {
namespace {
unsigned scan_code(SDL_Scancode value) {
    // Physical SDL keys -> DirectInput scan codes used by the game's bindings.
#define KEY(key, code) {SDL_SCANCODE_##key, code}
    static const std::pair<SDL_Scancode, unsigned> codes[]{
        KEY(ESCAPE,1), KEY(1,2), KEY(2,3), KEY(3,4), KEY(4,5), KEY(5,6), KEY(6,7), KEY(7,8),
        KEY(8,9), KEY(9,10), KEY(0,11), KEY(MINUS,12), KEY(EQUALS,13), KEY(BACKSPACE,14), KEY(TAB,15),
        KEY(Q,16), KEY(W,17), KEY(E,18), KEY(R,19), KEY(T,20), KEY(Y,21), KEY(U,22), KEY(I,23),
        KEY(O,24), KEY(P,25), KEY(LEFTBRACKET,26), KEY(RIGHTBRACKET,27), KEY(RETURN,28), KEY(LCTRL,29),
        KEY(A,30), KEY(S,31), KEY(D,32), KEY(F,33), KEY(G,34), KEY(H,35), KEY(J,36), KEY(K,37),
        KEY(L,38), KEY(SEMICOLON,39), KEY(APOSTROPHE,40), KEY(GRAVE,41), KEY(LSHIFT,42), KEY(BACKSLASH,43),
        KEY(Z,44), KEY(X,45), KEY(C,46), KEY(V,47), KEY(B,48), KEY(N,49), KEY(M,50), KEY(COMMA,51),
        KEY(PERIOD,52), KEY(SLASH,53), KEY(RSHIFT,54), KEY(KP_MULTIPLY,55), KEY(LALT,56), KEY(SPACE,57),
        KEY(CAPSLOCK,58), KEY(F1,59), KEY(F2,60), KEY(F3,61), KEY(F4,62), KEY(F5,63), KEY(F6,64),
        KEY(F7,65), KEY(F8,66), KEY(F9,67), KEY(F10,68), KEY(NUMLOCKCLEAR,69), KEY(SCROLLLOCK,70),
        KEY(KP_7,71), KEY(KP_8,72), KEY(KP_9,73), KEY(KP_MINUS,74), KEY(KP_4,75), KEY(KP_5,76),
        KEY(KP_6,77), KEY(KP_PLUS,78), KEY(KP_1,79), KEY(KP_2,80), KEY(KP_3,81), KEY(KP_0,82),
        KEY(KP_PERIOD,83), KEY(NONUSBACKSLASH,86), KEY(F11,87), KEY(F12,88), KEY(KP_ENTER,156),
        KEY(RCTRL,157), KEY(KP_DIVIDE,181), KEY(PRINTSCREEN,183), KEY(RALT,184), KEY(HOME,199),
        KEY(UP,200), KEY(PAGEUP,201), KEY(LEFT,203), KEY(RIGHT,205), KEY(END,207), KEY(DOWN,208),
        KEY(PAGEDOWN,209), KEY(INSERT,210), KEY(DELETE,211), KEY(LGUI,219), KEY(RGUI,220), KEY(APPLICATION,221)};
#undef KEY
    for (const auto& [key, code] : codes)
        if (key == value) return code;
    return 0;
}
unsigned virtual_key(SDL_Keycode key) {
    if (key >= SDLK_a && key <= SDLK_z) return static_cast<unsigned>(key - SDLK_a + 'A');
    if (key >= SDLK_0 && key <= SDLK_9) return static_cast<unsigned>(key);
    if (key >= SDLK_F1 && key <= SDLK_F12) return static_cast<unsigned>(key - SDLK_F1 + 0x70);
    switch (key) {
    case SDLK_BACKSPACE: return 8; case SDLK_TAB: return 9; case SDLK_RETURN: return 13;
    case SDLK_ESCAPE: return 27; case SDLK_SPACE: return 32; case SDLK_PAGEUP: return 33;
    case SDLK_PAGEDOWN: return 34; case SDLK_END: return 35; case SDLK_HOME: return 36;
    case SDLK_LEFT: return 37; case SDLK_UP: return 38; case SDLK_RIGHT: return 39; case SDLK_DOWN: return 40;
    case SDLK_INSERT: return 45; case SDLK_DELETE: return 46;
    default: return 0;
    }
}
std::uint8_t modifiers(const InputState& input) {
    const auto& s = input.scancodes;
    return static_cast<std::uint8_t>(((s[29] || s[157]) ? 1 : 0) |
        ((s[42] || s[54]) ? 2 : 0) | ((s[56] || s[184]) ? 4 : 0));
}
}
Window::Window(bool hidden) {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
        throw Error(SDL_GetError());
    handle_ = SDL_CreateWindow("SacrificeModern", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width_, height_, SDL_WINDOW_RESIZABLE | (hidden ? SDL_WINDOW_HIDDEN : 0));
    if (!handle_) throw Error(SDL_GetError());
}
void Window::borderless(bool fullscreen) {
    if (fullscreen == borderless_)
        return;
    borderless_ = fullscreen;
    SDL_SetWindowFullscreen(handle_, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}
Window::~Window() {
    SDL_DestroyWindow(handle_);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
}
void Window::native_cursor(bool enabled) {
    native_cursor_ = enabled;
    SDL_ShowCursor(enabled ? SDL_DISABLE : SDL_ENABLE);
}
bool Window::pump() {
    input_.mouse_x = input_.mouse_y = 0;
    input_.wheel = 0;
    input_.pressed_codes = {};
    input_.pointer_events.clear();
    input_.pointer_cancelled = false;
    double css_width{}, css_height{};
    emscripten_get_element_css_size("#canvas", &css_width, &css_height);
    const double dpi = emscripten_get_device_pixel_ratio();
    const auto width = static_cast<unsigned>(std::max(1.0, std::round(css_width * dpi)));
    const auto height = static_cast<unsigned>(std::max(1.0, std::round(css_height * dpi)));
    // The port uses physical pixels for rendering and pointer coordinates. Apply browser DPI here
    // only; SDL's high-DPI mode would multiply this canvas size a second time.
    width_ = width; height_ = height;
    int sdl_width{}, sdl_height{};
    SDL_GetWindowSize(handle_, &sdl_width, &sdl_height);
    if (static_cast<unsigned>(sdl_width) != width || static_cast<unsigned>(sdl_height) != height) {
        SDL_SetWindowSize(handle_, static_cast<int>(width), static_cast<int>(height));
    }
    const auto cancel = [&] {
        input_ = {};
        input_.pointer_cancelled = true;
        pointer_ = {};
        SDL_SetRelativeMouseMode(SDL_FALSE);
    };
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (ImGui::GetCurrentContext()) ImGui_ImplSDL2_ProcessEvent(&e);
        switch (e.type) {
        case SDL_QUIT: return false;
        case SDL_WINDOWEVENT:
            // A lost focus also loses the pointer: the follow camera must not keep edge-scrolling.
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) { cancel(); input_.pointer_inside = false; }
            if (e.window.event == SDL_WINDOWEVENT_ENTER) input_.pointer_inside = true;
            if (e.window.event == SDL_WINDOWEVENT_LEAVE) input_.pointer_inside = false;
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            const bool down = e.type == SDL_KEYDOWN;
            const auto scan = scan_code(e.key.keysym.scancode), vk = virtual_key(e.key.keysym.sym);
            if (scan) {
                if (down && !input_.scancodes[scan]) {
                    input_.pressed_codes[scan] = true;
                    input_.press_modifiers[scan] = modifiers(input_);
                }
                input_.scancodes[scan] = down;
            }
            if (vk) input_.keys[vk] = down;
            input_.keys[16] = input_.scancodes[42] || input_.scancodes[54];
            input_.keys[17] = input_.scancodes[29] || input_.scancodes[157];
            input_.keys[18] = input_.scancodes[56] || input_.scancodes[184];
            if (down && e.key.keysym.sym == SDLK_ESCAPE) {
                input_.looking = false;
                SDL_SetRelativeMouseMode(SDL_FALSE);
            }
            break;
        }
        case SDL_MOUSEWHEEL:
            input_.wheel += e.wheel.y * (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -120 : 120);
            break;
        case SDL_MOUSEMOTION:
            input_.cursor_x = static_cast<float>(e.motion.x);
            input_.cursor_y = static_cast<float>(e.motion.y);
            input_.pointer_inside = true;
            input_.mouse_x += e.motion.xrel;
            input_.mouse_y += e.motion.yrel;
            pointer_.move({input_.cursor_x,input_.cursor_y}, modifiers(input_), input_.pointer_events);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            const auto raw = e.button.button;
            if (raw != SDL_BUTTON_LEFT && raw != SDL_BUTTON_RIGHT && raw != SDL_BUTTON_MIDDLE) break;
            const auto button = raw == SDL_BUTTON_MIDDLE ? PointerButton::middle :
                ((raw == SDL_BUTTON_LEFT) != swap_ ? PointerButton::left : PointerButton::right);
            input_.cursor_x = static_cast<float>(e.button.x);
            input_.cursor_y = static_cast<float>(e.button.y);
            const Vec2 at{input_.cursor_x,input_.cursor_y};
            const bool down = e.type == SDL_MOUSEBUTTONDOWN;
            if (down) pointer_.press(button, at, modifiers(input_), e.button.clicks >= 2, input_.pointer_events);
            else pointer_.release(button, at, modifiers(input_), input_.pointer_events);
            if (button == PointerButton::right) {
                input_.looking = down && free_camera;
                const bool captured = ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse;
                SDL_SetRelativeMouseMode(down && free_camera && !captured ? SDL_TRUE : SDL_FALSE);
            }
            break;
        }
        }
    }
    return true;
}
} // namespace sacrifice
