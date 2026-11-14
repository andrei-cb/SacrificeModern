#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace sacrifice {
using Bytes = std::vector<std::byte>;
using ByteView = std::span<const std::byte>;
class Error : public std::runtime_error {
    using std::runtime_error::runtime_error;
};
class Reader {
  public:
    explicit Reader(ByteView data, std::string context = "binary")
        : data_(data), context_(std::move(context)) {}
    [[nodiscard]] std::size_t remaining() const {
        return data_.size() - position_;
    }
    [[nodiscard]] std::size_t position() const {
        return position_;
    }
    ByteView take(std::size_t count);
    std::uint8_t u8();
    std::uint16_t u16();
    std::int16_t i16() {
        return std::bit_cast<std::int16_t>(u16());
    }
    std::uint32_t u32();
    float f32() {
        return std::bit_cast<float>(u32());
    }
    std::string fixed_string(std::size_t count);

  private:
    ByteView data_;
    std::size_t position_{};
    std::string context_;
};
struct FourCC {
    std::uint32_t value{};
    static FourCC from_text(std::string_view text);
    [[nodiscard]] std::string text() const;
    auto operator<=>(const FourCC&) const = default;
};
Bytes read_file(const std::filesystem::path& path, std::size_t limit = 512ull * 1024 * 1024);
Bytes inflate_exact(ByteView compressed, std::size_t expected, std::string_view context);
std::string utf8(const std::filesystem::path& path);
#ifdef _WIN32
std::wstring widen(std::string_view text);
#endif
// Asset paths use Windows separators even when loaded from a browser filesystem.
std::filesystem::path game_path(std::string_view text);
std::filesystem::path application_data();
void log(std::string_view message);
// --trace-ai: opt-in log of wizard snapshots and computer-side decisions for playtest reports.
bool trace_ai();
void set_trace_ai(bool enabled);
struct Arguments {
    std::filesystem::path game_dir;
    std::filesystem::path map;
    std::filesystem::path reference;
    std::filesystem::path file;
    std::filesystem::path output;
    std::string command;
    std::string resource;
    std::string clip;
    std::optional<float> model_frame, model_detail;
    std::optional<unsigned> model_vertices, scene_ticks, scene_warmup;
    std::optional<unsigned> scene_weather; // --scene-weather K: smoke test, weather kind K on at full strength.
    std::optional<unsigned> bench_frames;  // --bench-frames N: smoke test, time N more frames and log the averages.
    std::optional<unsigned> graphics_detail; // Native gdet level, 0 highest through 7 lowest.
    bool smoke{};
    bool trace_ai{};
    bool model_preview{};
    bool hide_ui{};
    bool classic_look{}; // --classic-look: ignore the Remastered Look option (reference captures).
    bool hide_environment{};
    std::optional<float> view_yaw;
    // World X, world Y, height, yaw and pitch in degrees (Z-up map coordinates).
    std::optional<std::array<float, 5>> camera;
    // Diagnostic character ordinal, yaw/forward(or pitch)/throttle controls, applied before scene ticks.
    std::optional<std::array<int, 4>> scene_control;
    std::optional<std::array<float, 4>> scene_move;
    std::optional<std::array<int, 2>> scene_attack;
    std::optional<unsigned> scene_select; // Smoke-only selection/command feedback capture.
    std::optional<unsigned> scene_hover; // Smoke-only hovered character; wait past the native card delay.
    std::optional<std::array<float, 2>> scene_pointer; // Smoke-only client pixel hover, using normal picking.
    FourCC scene_target; // Smoke-only target mode, without casting a spell body.
    std::optional<std::tuple<unsigned, FourCC, int>> scene_cast;
    FourCC scene_ritual; // Smoke-only controlled altar ceremony (sacc or sacu).
    FourCC front_end;    // Opening front-end FORM page (the title page by default without --file/--map).
    std::string campaign_actions; // Smoke-only comma-separated win,lose,end,menu,restart,ok commands.
    std::string campaign; // Smoke-only: play this campaign character's current mission (created if new).
    std::optional<std::array<int, 2>> scene_souls;     // Diagnostic character ordinal and soul grant.
    std::optional<std::array<int, 2>> scene_soul_move; // Character and active soul bundle ordinal.
    std::optional<std::array<int, 2>> building_damage; // Diagnostic building ordinal and direct damage.
    std::optional<std::array<int, 2>> creature_damage; // Diagnostic character ordinal and source-free hit.
    std::optional<unsigned> scene_altar_ruin; // Smoke-only denied-respawn wizard / altar destruction.
    std::optional<std::array<int, 2>> scene_burn; // Smoke-only character and native temporary burn duration.
    std::optional<std::pair<unsigned, float>> building_reveal;
    static Arguments parse(const std::vector<std::string>& values);
};
std::filesystem::path discover_game_dir(std::filesystem::path candidate);
// Sacrifice.exe main loop 0x40a8a0: TIMER::GetTimeElapsed (share.dll, resolution 1/60 set at start-up)
// returns the time since the last call in 1/60 s ticks as a float; the loop adds the carried fraction
// (0x4dbfe8), truncates to whole ticks, adds them to the tick count (0x4dbfa8) and hands them to the game
// (0x40ace0) and renderer (0x40adb0). The match tick 0x452700 clamps one call to 12 ticks (the rest is
// dropped, so play slows below 5 frames per second), and 0x4525d0 runs at most 1 tick per call until its
// count (this+636) reaches 10.
// msvcrt rand(): the executable and the game DLLs share one state, never seeded by the game.
struct CrtRandom {
    std::uint32_t state = 1;
    int next() {
        state = state * 214013u + 2531011u;
        return static_cast<int>(state >> 16 & 0x7fff);
    }
};
struct GameClock {
    float fraction{};
    std::uint32_t total{};
    std::uint32_t match{};
    // Whole ticks for `elapsed` 1/60 s ticks of real time.
    int advance(float elapsed) {
        const float sum = fraction + elapsed;
        const auto whole = static_cast<int>(static_cast<std::int64_t>(sum));
        total += static_cast<std::uint32_t>(whole);
        fraction = sum - static_cast<float>(whole);
        return whole;
    }
    // The ticks one match update simulates.
    int match_ticks(int ticks) {
        ticks = std::min(ticks, 12);
        if (match < 10 && ticks >= 1)
            ticks = 1;
        match += static_cast<std::uint32_t>(ticks);
        return ticks;
    }
};
} // namespace sacrifice
