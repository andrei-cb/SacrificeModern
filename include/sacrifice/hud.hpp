#pragma once
#include "assets.hpp"

namespace sacrifice {
// GBAR's 16 cell flags, two moving-cap selectors, two cap sizes, and 5x5 RGBA colors.
// Native vertical painter 0x45f120 and color-grid submission 0x45f250.
struct GradientBar {
    std::array<std::uint8_t, 16> cells{};
    std::uint8_t top_tracks_fill{}, bottom_tracks_fill{}, top_cap{}, bottom_cap{};
    std::array<std::uint32_t, 25> colors{}; // R is the low byte, like Image/Vertex.
    static GradientBar decode(ByteView data);
    [[nodiscard]] Mesh vertical(Vec2 origin, Vec2 size, int fraction) const; // 16.16 fill, clamped 0..1.
    [[nodiscard]] Mesh horizontal(Vec2 origin, Vec2 size, int fraction) const; // 0x45f3c0, transposed grid.
};
// 0x4516b0: screen scale is min(max(width / 800, 1), max(height / 600, 1)).
float hud_scale(unsigned width, unsigned height);
struct HudRect {
    Vec2 origin, size;
    [[nodiscard]] bool contains(Vec2 point) const;
};
// View modes 2 and 4 (0x47ee70): twelve selected creatures and one ability; circular minimap.
struct SelectionPanelLayout {
    HudRect bounds, ability;
    std::array<HudRect, 12> slots;
    float divider{};
    static SelectionPanelLayout make(unsigned width, unsigned height);
};
struct MiniMapLayout {
    HudRect bounds;
    Vec2 centre;
    float radius{};
    static MiniMapLayout make(unsigned width, unsigned height);
    bool contains(Vec2 p) const;
    Vec2 project(Vec3 point, unsigned map_width, unsigned map_height) const;
    Vec3 unproject(Vec2 point, unsigned map_width, unsigned map_height) const;
};
// 0x466d30 / 0x467420 / 0x467470: world centre, visible world spans, view rotation.
struct MiniMapTransform {
    Vec2 centre, span;
    float angle{};
    Vec2 normalized(Vec3 point) const;
    Vec3 world(Vec2 point) const;
};
// Camera-following minimap (0x467320), zoom drag (0x4670f0) and terrain bounds (0x45d8b0).
struct MiniMapControl {
    float zoom{.2f};
    Vec2 minimum, maximum;
    void reset(const Terrain& terrain);
    void drag(float dy, unsigned screen_height);
    MiniMapTransform transform(const Terrain& terrain, Vec3 camera, float angle) const;
};
bool minimap_box_contains(const MiniMapTransform& transform, const MiniMapLayout& layout,
                          Vec2 start, Vec2 end, Vec3 point); // 0x481310 / 0x47eb60.
// LAND::CreateMiniMap 0x16042990: tile's last mip colour, low fog nibble, native RGB565 LUT.
Image terrain_minimap(const Terrain& terrain, const TerrainMaterials& materials,
                      std::span<const std::uint8_t> fog);
// Main view mode 3 (0x47ee70), slot pools 0x470940/0x470990 and hit test 0x470f10.
struct SpellBarLayout {
    float scale{}, top{}, slot_top{}, tab_size{}, slot_size{};
    std::array<HudRect, 3> tabs;
    std::array<HudRect, 16> slots;
    struct Hit {
        bool tab{};
        unsigned index{};
    };
    static SpellBarLayout make(unsigned width, unsigned height);
    [[nodiscard]] std::optional<Hit> hit(Vec2 point, unsigned visible) const;
};
struct SpellIconVisual {
    HudRect rectangle;
    std::uint8_t alpha{255};
    bool additive{};
    static SpellIconVisual make(HudRect slot, int mana, int cost); // 0x475650.
};
Mesh spell_cooldown(HudRect slot, int remaining, int duration); // 0x4758c0, eight-segment sweep.
} // namespace sacrifice
