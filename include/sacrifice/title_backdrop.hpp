#pragma once
#include "assets.hpp"
#include "hud.hpp"
#include <array>

namespace sacrifice {
// The front end's moving background and title logo (Sacrifice.exe). 0x40fb60 switches most front-end screens to
// the TXTR:tunl flight (object 0x4f3f70: init 0x4bb710, step 0x4bba80, draw 0x4bbcf0); the title FORM's `logo`
// widget is painted by 0x40f500/0x40f170 as a waving 16x10 grid of TXTR:LOGO. Both step once per 60 Hz tick.
class TitleBackdrop {
  public:
    static FourCC tunnel_texture() {
        return FourCC::from_text("tunl");
    }
    static FourCC logo_texture() {
        return FourCC::from_text("LOGO");
    }
    TitleBackdrop();
    // 0x40fb60: the splash, saving, options, in-game and lobby screens have no tunnel.
    static bool shows_tunnel(FourCC page);
    void advance(float seconds);
    // Screen-space triangles (ImGui vertex colours) over a width x height view, far rings first.
    Mesh tunnel(float width, float height);
    Mesh logo(HudRect box) const;

  private:
    struct Segment {
        int count{33};
        double t{}, phase{}, amplitude{}, x{}, z{};
    };
    struct Ring {
        Segment segment;
        double radius{4}, x{}, z{}, y{}, t{}, phase{};
        float v{}, brightness{.5f};
        std::array<float, 3> colour{1, 1, 1};
    };
    static constexpr int rings = 33, sides = 60;
    unsigned random();
    void step_tunnel();
    void step_logo();
    void begin_segment();
    void place(Ring& ring, const Ring& previous);
    std::array<double, 3> curve(const Segment& segment, double t) const;
    std::uint32_t seed_{1};
    float carry_{};
    std::array<Ring, rings> ring_;
    Segment segment_;
    std::array<double, sides + 1> ripple_{};
    double scroll_{}, radius_{4}, radius_rate_{}, radius_accel_{}, roll_{}, roll_rate_{}, roll_accel_{};
    double phase_{}, wobble_{};
    int pulse_{-1};
    float pulse_brightness_{};
    std::array<double, 17 * 11> wave_{};
    double wave_phase_{};
};
} // namespace sacrifice
