#include "sacrifice/platform.hpp"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <algorithm>
#include <cstdlib>
#include <windowsx.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace sacrifice {
namespace {
unsigned scan_code(WPARAM key, LPARAM data) {
    const auto scan = static_cast<unsigned>(((data >> 16) & 0x7f) | ((data >> 17) & 0x80));
    if (scan || key >= 256)
        return scan;
    // Window-targeted accessibility messages can supply only a virtual key. Physical key events
    // retain their original scan code, including extended keys and the user's keyboard layout.
    const auto mapped = MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC_EX);
    return (mapped & 0x7f) | ((mapped & 0xff00) ? 0x80 : 0);
}
std::uint8_t pointer_modifiers(const InputState& input) {
    const auto& scan = input.scancodes;
    return static_cast<std::uint8_t>(((scan[29] || scan[157]) ? 1 : 0) |
                                     ((scan[42] || scan[54]) ? 2 : 0) |
                                     ((scan[56] || scan[184]) ? 4 : 0));
}
} // namespace
Window::Window(bool hidden) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = procedure;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SacrificeModernWindow";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw Error("Window class registration failed");
    RECT bounds{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, false, 0);
    handle_ = CreateWindowExW(0, wc.lpszClassName, L"SacrificeModern — engine reconstruction",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left,
                              bounds.bottom - bounds.top, nullptr, nullptr, wc.hInstance, this);
    if (!handle_)
        throw Error("Window creation failed");
    RAWINPUTDEVICE devices[] = {{1, 2, 0, handle_}, {1, 6, 0, handle_}};
    if (!RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE)))
        throw Error("Raw Input registration failed");
    pointer_.threshold = std::max(4.0f, 4.0f * static_cast<float>(GetDpiForWindow(handle_)) / 96);
    ShowWindow(handle_, hidden ? SW_HIDE : SW_SHOWDEFAULT);
}
Window::~Window() {
    if (IsWindow(handle_))
        DestroyWindow(handle_);
}
void Window::borderless(bool fullscreen) {
    if (fullscreen == borderless_)
        return;
    borderless_ = fullscreen;
    if (fullscreen) {
        GetWindowPlacement(handle_, &placement_);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(handle_, MONITOR_DEFAULTTONEAREST), &monitor);
        SetWindowLongPtrW(handle_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        const auto& r = monitor.rcMonitor;
        SetWindowPos(handle_, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(handle_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(handle_, &placement_);
        SetWindowPos(handle_, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
}
void Window::native_cursor(bool enabled) {
    native_cursor_ = enabled;
    if (input_.pointer_inside)
        SetCursor(enabled ? nullptr : LoadCursorW(nullptr, IDC_ARROW));
}
bool Window::pump() {
    input_.mouse_x = input_.mouse_y = 0;
    input_.wheel = 0;
    input_.pressed_codes = {};
    input_.pointer_events.clear();
    input_.pointer_cancelled = false;
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT)
            return false;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return true;
}
LRESULT CALLBACK Window::procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self)
        switch (message) {
        case WM_SETCURSOR:
            if (self->native_cursor_ && LOWORD(l) == HTCLIENT) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;
        case WM_MOUSELEAVE:
            self->input_.pointer_inside = false;
            break;
        case WM_SIZE:
            self->width_ = LOWORD(l);
            self->height_ = HIWORD(l);
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (w < 256)
                self->input_.keys[w] = true;
            {
                const auto code = scan_code(w, l);
                if (!self->input_.scancodes[code]) {
                    self->input_.pressed_codes[code] = true;
                    const auto& scan = self->input_.scancodes;
                    self->input_.press_modifiers[code] = static_cast<std::uint8_t>(
                        ((scan[29] || scan[157]) ? 1 : 0) | ((scan[42] || scan[54]) ? 2 : 0) |
                        ((scan[56] || scan[184]) ? 4 : 0));
                }
                self->input_.scancodes[code] = true;
            }
            if (w == VK_F10)
                return 0; // Viewer command: do not enter Win32's implicit menu loop.
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (w < 256)
                self->input_.keys[w] = false;
            self->input_.scancodes[scan_code(w, l)] = false;
            if (w == VK_F10)
                return 0;
            break;
        case WM_MOUSEWHEEL:
            self->input_.wheel += GET_WHEEL_DELTA_WPARAM(w);
            break;
        case WM_KILLFOCUS:
            self->input_ = {};
            self->input_.pointer_cancelled = true;
            self->pointer_ = {};
            ReleaseCapture();
            break;
        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(l) != hwnd && self->pointer_.held()) {
                self->pointer_ = {};
                self->input_.pointer_events.clear();
                self->input_.pointer_cancelled = true;
                self->input_.looking = false;
            }
            break;
        case WM_MOUSEMOVE:
            if (!self->input_.pointer_inside) {
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&track);
                self->input_.pointer_inside = true;
            }
            self->input_.cursor_x = static_cast<float>(GET_X_LPARAM(l));
            self->input_.cursor_y = static_cast<float>(GET_Y_LPARAM(l));
            self->pointer_.move({self->input_.cursor_x, self->input_.cursor_y},
                                pointer_modifiers(self->input_), self->input_.pointer_events);
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONDBLCLK: {
            const bool left = message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK;
            const bool right = message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK;
            const auto button = left != self->swap_ && (left || right) ? PointerButton::left
                                : left || right                      ? PointerButton::right
                                                                     : PointerButton::middle;
            const bool twice = message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDBLCLK ||
                               message == WM_MBUTTONDBLCLK;
            self->input_.cursor_x = static_cast<float>(GET_X_LPARAM(l));
            self->input_.cursor_y = static_cast<float>(GET_Y_LPARAM(l));
            self->pointer_.press(button, {self->input_.cursor_x, self->input_.cursor_y},
                                 pointer_modifiers(self->input_), twice, self->input_.pointer_events);
            if (button == PointerButton::right) {
                self->input_.looking = true;
            }
            SetCapture(hwnd);
            break;
        }
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP: {
            const bool left = message == WM_LBUTTONUP, right = message == WM_RBUTTONUP;
            const auto button = left != self->swap_ && (left || right) ? PointerButton::left
                                : left || right                      ? PointerButton::right
                                                                     : PointerButton::middle;
            self->input_.cursor_x = static_cast<float>(GET_X_LPARAM(l));
            self->input_.cursor_y = static_cast<float>(GET_Y_LPARAM(l));
            self->pointer_.release(button, {self->input_.cursor_x, self->input_.cursor_y},
                                   pointer_modifiers(self->input_), self->input_.pointer_events);
            if (button == PointerButton::right)
                self->input_.looking = false;
            if (!self->pointer_.held())
                ReleaseCapture();
            break;
        }
        case WM_INPUT: {
            RAWINPUT raw{};
            UINT size = sizeof(raw);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, &raw, &size,
                                sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1) &&
                raw.header.dwType == RIM_TYPEMOUSE && !(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                self->input_.mouse_x += static_cast<float>(raw.data.mouse.lLastX);
                self->input_.mouse_y += static_cast<float>(raw.data.mouse.lLastY);
            }
            break;
        }
        case WM_DPICHANGED: {
            auto* r = reinterpret_cast<RECT*>(l);
            self->pointer_.threshold = std::max(4.0f, 4.0f * static_cast<float>(HIWORD(w)) / 96);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
    // Process our release before the backend releases Win32 capture. Otherwise its synchronous
    // WM_CAPTURECHANGED would cancel a still-held game gesture on every ordinary button-up.
    if (ImGui::GetCurrentContext())
        ImGui_ImplWin32_WndProcHandler(hwnd, message, w, l);
    return DefWindowProcW(hwnd, message, w, l);
}
} // namespace sacrifice
