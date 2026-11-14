#pragma once
#include "sacrifice/assets.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sacrifice {
struct WidgetModel;
// Remastered Look plant models (drawing only), generated in code. They replace the original widget models
// at the same map anchors, and the grass layer scatters the clutter library over the ground.
// Points are in widget space: X/Y across the ground, Z up from the root at 0. Colours are RGBA, R in the low
// byte, as albedo for the anchor's (or ground's) light.
struct FloraModel {
    struct Point {
        Vec3 position;
        std::uint32_t color{};
    };
    std::vector<Point> points;
    std::vector<std::array<std::uint32_t, 3>> triangles;
    bool rigid{}; // Rocks: no wind or root shade.
};
enum class FloraKind : std::uint8_t { grass, flower, fern, bush, rock, twig, mushroom };
struct FloraStyle {
    FloraKind kind{FloraKind::grass};
    Vec3 leaf{.3f, .45f, .2f};   // Main colour (leaves, stems, stone, wood).
    Vec3 accent{.9f, .85f, .3f}; // Petals or caps.
    float width{1}, height{1};
};
// The role an original widget plays, from its shape and its texture's colours over the area it uses.
FloraStyle classify_widget(const WidgetModel& widget, const Image& texture);
// The style for a widget: a hand-made entry for the game's known widgets (WIDC ids, chosen from their
// textures), else classify_widget.
FloraStyle widget_style(const std::string& id, const WidgetModel& widget, const Image& texture);
// `variants` models of one style, from `seed` (no match random numbers). `far` makes the low-detail level of
// the same models (same seed, same shapes): single-segment blades and stems, no small leaves or leaflets,
// fewer bigger bush cards, coarser stones and caps.
std::vector<FloraModel> make_flora(const FloraStyle& style, std::uint32_t seed, unsigned variants = 4,
                                   bool far = false);
// Ground clutter for the grass layer, each about 1 unit across before scaling.
namespace clutter {
inline constexpr unsigned flowers = 0, flower_count = 8; // White, yellow, violet, red; two of each.
inline constexpr unsigned mushrooms = 8, mushroom_count = 2;
inline constexpr unsigned pebbles = 10, pebble_count = 3;
inline constexpr unsigned twigs = 13, twig_count = 2;
const std::vector<FloraModel>& models();
} // namespace clutter
} // namespace sacrifice
