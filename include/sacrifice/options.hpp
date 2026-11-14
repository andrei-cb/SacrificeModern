#pragma once
#include "core.hpp"
#include <array>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace sacrifice {
// Main view's GFXDETAILINFO, updated by 0x452c60 rather than MODEL's fallback record.
struct RenderDetail {
    float decay{.5f}, reach{10}, minimum{1}, maximum{1};
    float quality(float depth, float radius, float model_detail = 1) const;
};
struct RenderDetailControl {
    float adaptive{1};
    // `fps` is one frame's rate. Like _3dngn::UpdateStats (0x18006f60) behind GetFramerate, the control
    // reacts to the average over the last 32 frames, so a single slow frame does not drop model detail.
    RenderDetail update(float fps, int target_fps, int level, bool starting = false);

  private:
    std::array<float, 32> frame_times_{};
    float total_{};
    unsigned next_{}, frames_{};
};
// One integer entry of the native settings table at 0x4c8db8 (32-byte records: id, name, type 4,
// persisted flag, value pointer, default, minimum, maximum).
struct OptionSetting {
    FourCC id;
    int initial{}, minimum{}, maximum{};
};
// The values behind FORM:opts and its pages (handlers 0x40d080 opts, 0x40e820 sopt, 0x40eb10 copt).
class GameOptions {
  public:
    static const std::array<OptionSetting, 32> settings;
    GameOptions() { reset(); }
    void reset(); // 0x40c960(0): every entry back to its default ('dflt').
    [[nodiscard]] static const OptionSetting* find(FourCC id);
    [[nodiscard]] int value(FourCC id) const;
    void set(FourCC id, int value); // Clamped to the entry's range.
    // 0x40cd40's numb text: %d by default, gama x0.001 "%1.1f", mspd x0.03125 "%1.2f", ctr! 1 << v,
    // and the volume sliders as percentages.
    [[nodiscard]] std::string display(FourCC id) const;
    // 0x40cc60: copt's wscx/wscy/mwzm/wdrx/wdry are bits 1/2/4/8/16 of ctrl (byte_4dc020); the other
    // checkboxes are booleans of their own id.
    [[nodiscard]] std::optional<bool> checked(FourCC id) const;
    void toggle(FourCC id);
    // 'rate' is the port's frame cap (30..144; natively the 10..30 Target Framerate). The detail controller
    // keeps aiming at the native range, so it gets the cap limited to 30.
    [[nodiscard]] int detail_target() const;
    [[nodiscard]] nlohmann::json save() const;
    void load(const nlohmann::json& saved);

  private:
    std::array<int, settings.size()> values_{};
};
} // namespace sacrifice
