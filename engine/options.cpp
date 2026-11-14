#include "sacrifice/options.hpp"
#include <algorithm>
#include <format>
#include <cmath>

namespace sacrifice {
float RenderDetail::quality(float depth, float radius, float model_detail) const {
    const float distance = radius * reach;
    const float amount = (maximum - minimum) * model_detail + minimum;
    return std::clamp(depth <= distance ? amount :
        std::pow(decay, (depth - distance) / std::max(distance, .0001f)) * amount, 0.0f, 1.0f);
}
RenderDetail RenderDetailControl::update(float fps, int target_fps, int level, bool starting) {
    // 0x4cebd4: multiplier and ceiling, ordered highest (0) to lowest (7).
    constexpr std::array<std::array<float, 2>, 8> levels{{
        {1000, 25}, {2, 16}, {1.5f, 10}, {1, 4}, {.75f, 3}, {.5f, 1}, {.25f, .25f}, {0, .01f}}};
    if (std::isfinite(fps) && fps > 0) {
        const float seconds = 1 / fps;
        total_ += seconds - frame_times_[next_];
        frame_times_[next_] = seconds;
        next_ = (next_ + 1) % frame_times_.size();
        frames_ = std::min<unsigned>(frames_ + 1, static_cast<unsigned>(frame_times_.size()));
        fps = total_ > 0 ? static_cast<float>(frames_) / total_ : 0;
    }
    if (starting)
        adaptive = 1;
    else if (std::isfinite(fps) && fps > 0) {
        const float ratio = fps / std::max(target_fps, 1);
        if (ratio < 1)
            adaptive *= .85f + .15f * ratio * ratio;
        else if (ratio > 1)
            adaptive *= 1.176470588235294f + (1 - 1.176470588235294f) / std::sqrt(ratio);
        adaptive = std::clamp(adaptive, .01f, 25.0f);
    }
    const auto& limits = levels[std::clamp(level, 0, 7)];
    const float quality = std::min(std::max(adaptive * limits[0], .01f), limits[1]);
    const float root = std::sqrt(quality);
    return {root * .050000012f + .44999999f, root * 3 + 7, root, std::max(root, limits[0])};
}
namespace {
constexpr FourCC id(const char (&text)[5]) {
    return {static_cast<std::uint32_t>(static_cast<unsigned char>(text[0])) << 24 |
            static_cast<std::uint32_t>(static_cast<unsigned char>(text[1])) << 16 |
            static_cast<std::uint32_t>(static_cast<unsigned char>(text[2])) << 8 |
            static_cast<std::uint32_t>(static_cast<unsigned char>(text[3]))};
}
// copt checkbox bits of ctrl (0x40cc60).
constexpr std::array<std::pair<FourCC, int>, 5> control_bits{
    {{id("wscx"), 1}, {id("wscy"), 2}, {id("mwzm"), 4}, {id("wdrx"), 8}, {id("wdry"), 16}}};
} // namespace

// Integer entries of 0x4c8db8 that FORM:opts edits, in table order. ctrl has no range of its own; the
// port bounds it to its five checkbox bits.
const std::array<OptionSetting, 32> GameOptions::settings{{
    {id("mspd"), 32, 16, 128}, {id("ctr!"), 7, 5, 9},     {id("texr"), 0, 0, 3},     {id("gdet"), 3, 0, 7},
    {id("mscr"), 256, 32, 256}, {id("rate"), 144, 30, 144}, {id("ffir"), 1, 0, 1},     {id("fdam"), 64, 0, 128},
    {id("tipo"), 1, 0, 1},     {id("rdem"), 1, 0, 1},     {id("swap"), 0, 0, 1},     {id("dblc"), 20, 10, 60},
    {id("xmnu"), 15, 0, 60},   {id("ctrl"), 7, 0, 31},    {id("gama"), 1300, 1000, 2000},
    {id("tbuf"), 1, 0, 1},     {id("tfil"), 1, 0, 2},     {id("widj"), 1, 0, 1},     {id("smpv"), 100, 0, 100},
    {id("strv"), 100, 0, 100}, {id("dmus"), 0, 0, 1},     {id("spek"), 0, 0, 3},     {id("nadv"), 0, 0, 1},
    {id("krst"), 50, 0, 100},  {id("krdl"), 10, 5, 20},
    // Port settings (not in 0x4c8db8): the land detail mode bits (dword_4DC040 0x20 detail textures, 0x40
    // bump maps; 0x40d380 keeps at most one on), the window mode, the framerate readout, drawing between
    // ticks, the view shake (0x47fbb0) and the remastered look (a modern addition; off is the original's).
    {id("dmap"), 0, 0, 1},     {id("bmap"), 1, 0, 1},     {id("wmod"), 0, 0, 1},     {id("sfps"), 0, 0, 1},
    {id("smth"), 1, 0, 1},     {id("shak"), 1, 0, 1},     {id("rmst"), 1, 0, 1},
}};

void GameOptions::reset() {
    for (std::size_t i = 0; i < settings.size(); ++i)
        values_[i] = settings[i].initial;
}
const OptionSetting* GameOptions::find(FourCC key) {
    const auto it = std::ranges::find(settings, key, &OptionSetting::id);
    return it == settings.end() ? nullptr : &*it;
}
int GameOptions::value(FourCC key) const {
    const auto* entry = find(key);
    return entry ? values_[static_cast<std::size_t>(entry - settings.data())] : 0;
}
void GameOptions::set(FourCC key, int value) {
    if (const auto* entry = find(key))
        values_[static_cast<std::size_t>(entry - settings.data())] = std::clamp(value, entry->minimum, entry->maximum);
}
std::string GameOptions::display(FourCC key) const {
    const auto v = value(key);
    if (key == id("gama"))
        return std::format("{:1.1f}", v * 0.001);
    if (key == id("mspd"))
        return std::format("{:1.2f}", v * 0.03125);
    if (key == id("ctr!"))
        return std::to_string(1 << v);
    if (key == id("smpv") || key == id("strv"))
        return std::format("{}%", v);
    return std::to_string(v);
}
std::optional<bool> GameOptions::checked(FourCC key) const {
    if (key == id("lmap") || key == id("mtex"))
        return true; // Light maps and multitexturing are always used (shown checked, not editable).
    for (const auto& [box, bit] : control_bits)
        if (box == key)
            return (value(id("ctrl")) & bit) != 0;
    if (const auto* entry = find(key); entry && entry->minimum == 0 && entry->maximum == 1)
        return value(key) != 0;
    return std::nullopt;
}
void GameOptions::toggle(FourCC key) {
    for (const auto& [box, bit] : control_bits)
        if (box == key)
            return set(id("ctrl"), value(id("ctrl")) ^ bit);
    if (const auto state = checked(key)) {
        set(key, !*state);
        if (!*state && (key == id("dmap") || key == id("bmap"))) // 0x40d380: one detail mode at a time.
            set(key == id("dmap") ? id("bmap") : id("dmap"), 0);
    }
}
int GameOptions::detail_target() const {
    return std::min(value(id("rate")), 30);
}
nlohmann::json GameOptions::save() const {
    auto saved = nlohmann::json::object();
    for (std::size_t i = 0; i < settings.size(); ++i)
        saved[settings[i].id.text()] = values_[i];
    return saved;
}
void GameOptions::load(const nlohmann::json& saved) {
    if (!saved.is_object())
        return;
    for (const auto& entry : settings)
        if (const auto it = saved.find(entry.id.text()); it != saved.end() && it->is_number_integer())
            set(entry.id, it->get<int>());
}
} // namespace sacrifice
