#pragma once
#include "assets.hpp"
#include "core.hpp"
#include <array>
#include <cstdint>

namespace sacrifice {
class CreatureActor;
class Match;
void configure_debug_wizard(Match& match, CreatureActor& actor, int god, int level);
struct Terrain;

// The DirectInput keyboard device (0x4051b0, vtable 0x4bf678) with the default 'joys' bindings of the key
// table 0x4c78b8: W/Up 0x10000, S/Down 0x20000, A/Left 0x40000, D/Right 0x80000, =/keypad+ 0x100000 and
// -/keypad- 0x200000. Keys are DirectInput scan codes (extended keys | 0x80).
class KeyboardDevice {
  public:
    static constexpr std::uint32_t forward_bit = 0x10000, back_bit = 0x20000, left_bit = 0x40000,
                                   right_bit = 0x80000, throttle_up_bit = 0x100000, throttle_down_bit = 0x200000;
    KeyboardDevice();
    void bind(std::uint8_t key, std::uint32_t mask); // 0x405330 mode 1.
    // 0x405580(0) / 0x405530 / 0x405700: copy the previous state, read the new one, derive edges and masks.
    void poll(const std::array<bool, 256>& keys);
    std::uint32_t held() const { // +4
        return held_;
    }
    std::uint32_t pressed() const { // +8: keys that went down at this poll.
        return pressed_;
    }
    std::uint32_t released() const { // +12
        return released_;
    }
    // 0x405820: yaw, forward (negative ahead) and the accumulated throttle (+828, -127..127).
    std::array<int, 3> axes();
    // 0x451ea0: any steering or held key hands the wizard to the keyboard (+1701).
    bool engaged() const;

  private:
    std::array<std::uint32_t, 256> masks_{};
    std::array<bool, 256> state_{}, previous_{};
    std::uint32_t held_{}, pressed_{}, released_{};
    int throttle_{-127};
};

// Camera +232, 16.16 pixels. 0x453260 keeps the largest request scaled by (radius - distance) / radius from
// the view position (+92), only for the LAND game view (+256 == 1). 0x47f390 decays it by 8738 per tick, and
// 0x47fbb0 moves the view centre (+32/+36) by +-amplitude for one drawn frame (natively with the shared
// rand(); the host passes its own stream so frame rate cannot move game randomness).
struct CameraShake {
    int amount{};
    void request(Vec3 eye, Vec3 source, int amplitude, float radius);
    void update(int ticks);
    Vec2 offset(CrtRandom& random) const;
};

// Camera mode 0 of the follow camera (records at 0x4d22a0, flags 0x183): zoom easing (0x47f390 flag 2),
// zoom and drag input (0x4814a0, 0x4810c0), yaw recentring (0x480b30) and the Bezier follow placement
// behind the target's view angles (0x47fca0), using the SAXS instance's frozen camera dimensions.
class FollowCamera {
  public:
    // +220 / +224: 0x47ecb0 copies dword_4daa38, initialized indirectly by 0x451470 to 0.25.
    float zoom{.25f}, zoom_target{.25f};
    float height_scale{1}; // Normal follow-view preference; scripted camera placement stays native.
    unsigned flags{}; // Mode word: 1 recentre, 2 drag ended, 4 dragging, 0x80 turning, 0x100 standing.
    Vec3 position;
    float yaw{}, pitch{}; // Game convention: forward (-sin yaw cos pitch, cos yaw cos pitch, sin pitch).
    bool invert_x{}, invert_y{}; // Options 'wdrx' / 'wdry': byte_4dc020 bits 8 / 0x10.

    // 'drag' (0x4810c0): pixel deltas over the view; the half-angles are the RENDERWINDOW degrees.
    void drag(CreatureActor& target, float dx, float dy, unsigned width, unsigned height, float half_width,
              float half_height, bool end);
    void wheel(int delta); // dword_4dbb60 x -0.0005.
    // 0x4814a0's window scrolling (option ctrl bits 1 / 2), outside a drag: a cursor within two pixels of
    // the left/right edge turns the view 0.01 per tick plus 0.003 per pixel of outward mouse motion
    // (flag 16); the top/bottom edge zooms in/out 0.01 per tick plus 0.001 per pixel. When the edge
    // turn ends, flag 16 becomes 8 so the yaw recentres like a finished drag. The native runs fullscreen
    // with the cursor confined; in a window, a pointer outside it (or lost with the focus) must not keep
    // scrolling, so `inside` false disables both edges.
    void edge_scroll(CreatureActor& target, Vec2 cursor, unsigned width, unsigned height, Vec2 motion, int ticks,
                     unsigned options, bool inside);
    // One frame of `ticks`; `local` is the keyboard-driven wizard of the local player.
    void update(CreatureActor& target, const Terrain& terrain, int ticks, std::uint32_t held, bool local);
    // 0x47fca0 / 0x47fc30 also place a cinematic eye behind an object, facing the scripted aim.
    void place(const CreatureActor& target, const Terrain& terrain, Vec2 angles);
    // The same placement behind a point with explicit (distance, height) dimensions: the SAXS camera offset,
    // or for other models a radius of |bounds min XY| + 1 and 0.9 of the bounds' height.
    void place(Vec3 target, Vec2 offset, const Terrain& terrain, Vec2 angles);
};
// 0x47fca0's cubic Bezier at a zoom: distance and height factors, then the pitch offset.
std::array<float, 3> follow_curve(float zoom);
} // namespace sacrifice
