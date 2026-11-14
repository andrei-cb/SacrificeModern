#pragma once
#include "hud.hpp"
#include "player.hpp"

namespace sacrifice {
// MODEL::ShowHUD (game3d 0x1601b8c0), at model reference 0 in screen space.
struct UnitHudLayout {
    HudRect arrow;
    std::array<HudRect, 2> bars;
    static UnitHudLayout make(Vec2 anchor, float depth, unsigned view_width);
};
// RunConfirm -> CYLINDER: a 16-vertex ring tapering to a point at the order position.
// Returns renderer-space geometry; texture is TXTR:aura, material 0x1a012009.
Mesh order_marker_mesh(const PlayerControl::Marker& marker, const TextureHeader& texture, Vec3 eye);
std::uint32_t unit_side_colour(unsigned index); // Native BGR palette -> vertex RGBA.
Mesh hover_bracket_mesh(HudRect bounds, unsigned view_width, std::uint32_t colour); // 0x460710.
// 0x460710 projects the view-space box at its shared mean depth. X/Y are divided by view tangents.
std::optional<HudRect> hover_screen_bounds(const std::array<Vec3, 8>& view, unsigned width, unsigned height);
// 0x466870..0x466a90: shared by minimap drawing and the 0x467250 square hit test.
struct MiniMapMarker {
    Vec2 centre;
    float half_size{1}, angle{};
    std::uint32_t colour{0xffffffff};
    unsigned icon{}, priority{};
    bool textured{};
    bool contains(Vec2 point) const;
};
std::optional<MiniMapMarker> minimap_marker(const Match& match, const MatchSide& side,
                                           const MatchObject& object, const MiniMapTransform& transform,
                                           const MiniMapLayout& layout);
// MRMINSTANCE::CheckIntercept mode 2, static frame-zero model; local Z-up ray.
double building_intercept(const MrmGeometry& model, Vec3 from, Vec3 ray, unsigned vertices = 0);
} // namespace sacrifice
