#include "sacrifice/hud.hpp"
#include <cmath>

namespace sacrifice {
GradientBar GradientBar::decode(ByteView data) {
    Reader reader(data, "GBAR");
    GradientBar result;
    for (auto& cell : result.cells)
        cell = reader.u8();
    result.top_tracks_fill = reader.u8();
    result.bottom_tracks_fill = reader.u8();
    result.top_cap = reader.u8();
    result.bottom_cap = reader.u8();
    for (auto& color : result.colors)
        color = reader.u32(); // 0x45f250 swaps R/B for legacy D3D; our vertices already use RGBA.
    return result;
}
Mesh GradientBar::vertical(Vec2 origin, Vec2 size, int fraction) const {
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(size.x) ||
        !std::isfinite(size.y) || size.x <= 0 || size.y <= 0)
        throw Error("Invalid gradient bar rectangle");
    // Cap heights depend on the width, not the height (0x45f120).
    const double top = top_cap * static_cast<double>(size.x) / 64,
                 bottom = bottom_cap * static_cast<double>(size.x) / 64,
                 fill = std::clamp(fraction, 0, 65536) / 65536.0,
                 end = origin.y + static_cast<double>(size.y),
                 middle = end - ((size.y - top - bottom) * fill + bottom);
    const std::array<double, 5> rows{origin.y, top_tracks_fill == 1 ? middle - top : origin.y + top,
                                    middle, bottom_tracks_fill == 1 ? middle + bottom : end - bottom, end};
    Mesh mesh;
    mesh.vertices.reserve(25);
    mesh.indices.reserve(96);
    for (unsigned y = 0; y < 5; ++y)
        for (unsigned x = 0; x < 5; ++x) {
            Vertex vertex;
            vertex.position = {origin.x + size.x * (x * 0.25f), static_cast<float>(rows[y]), 0};
            vertex.color = colors[y * 5 + x];
            mesh.vertices.push_back(vertex);
        }
    for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 4; ++x) {
            const auto a = y * 5 + x, b = a + 1, c = a + 5, d = c + 1;
            // Preserve the native quad argument order selected by the cell flag.
            if (cells[y * 4 + x])
                mesh.indices.insert(mesh.indices.end(), {d, c, b, c, a, b});
            else
                mesh.indices.insert(mesh.indices.end(), {a, b, c, b, d, c});
        }
    return mesh;
}
Mesh GradientBar::horizontal(Vec2 origin, Vec2 size, int fraction) const {
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(size.x) ||
        !std::isfinite(size.y) || size.x <= 0 || size.y <= 0)
        throw Error("Invalid gradient bar rectangle");
    const double a = top_cap * size.y / 64.0, b = bottom_cap * size.y / 64.0,
                 fill = std::clamp(fraction, 0, 65536) / 65536.0,
                 middle = origin.x + b + (size.x - a - b) * fill, end = origin.x + size.x;
    const std::array<double, 5> columns{end, top_tracks_fill == 1 ? middle + a : end - a,
                                      middle, bottom_tracks_fill == 1 ? middle - b : origin.x + b, origin.x};
    Mesh mesh;
    for (unsigned y = 0; y < 5; ++y)
        for (unsigned x = 0; x < 5; ++x)
            mesh.vertices.push_back({{static_cast<float>(columns[x]), origin.y + size.y * (1 - y * .25f), 0},
                                      {}, {}, colors[x * 5 + y]});
    for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 4; ++x) {
            const unsigned a0 = y * 5 + x, b0 = a0 + 1, c0 = a0 + 5, d0 = c0 + 1;
            if (cells[x * 4 + y])
                mesh.indices.insert(mesh.indices.end(), {d0, c0, b0, c0, a0, b0});
            else
                mesh.indices.insert(mesh.indices.end(), {a0, b0, c0, b0, d0, c0});
        }
    return mesh;
}
float hud_scale(unsigned width, unsigned height) {
    return std::min(std::max(width / 800.0f, 1.0f), std::max(height / 600.0f, 1.0f));
}
SelectionPanelLayout SelectionPanelLayout::make(unsigned width, unsigned height) {
    const auto scale = hud_scale(width, height);
    const float portrait = std::floor(32 * scale), slot = std::floor(36 * scale), h = 8 * portrait;
    SelectionPanelLayout result;
    result.bounds = {{0, std::floor((height - h) * .5f)}, {portrait + 2 * slot, h}};
    result.divider = slot;
    for (unsigned i = 0; i < result.slots.size(); ++i)
        result.slots[i] = {{(i % 2) * slot, result.bounds.origin.y + portrait + (i / 2) * portrait},
                           {slot, portrait}};
    result.ability = {{2 * slot, result.bounds.origin.y + 4 * portrait - std::floor(portrait * .5f)},
                      {portrait, portrait}};
    return result;
}
MiniMapLayout MiniMapLayout::make(unsigned width, unsigned height) {
    const float size = static_cast<float>(height / 3);
    return {{{width - size, height - size}, {size, size}},
            {width - size * .5f, height - size * .5f}, size * .46875f};
}
bool MiniMapLayout::contains(Vec2 p) const {
    const float x = p.x - centre.x, y = p.y - centre.y;
    return x * x + y * y < bounds.size.x * bounds.size.x * .25f;
}
Vec2 MiniMapLayout::project(Vec3 point, unsigned map_width, unsigned map_height) const {
    return {centre.x + (point.x / (map_width * 10.0f) - .5f) * bounds.size.x,
            centre.y - (point.y / (map_height * 10.0f) - .5f) * bounds.size.y};
}
Vec3 MiniMapLayout::unproject(Vec2 point, unsigned map_width, unsigned map_height) const {
    return {(point.x - bounds.origin.x) / bounds.size.x * map_width * 10,
            (1 - (point.y - bounds.origin.y) / bounds.size.y) * map_height * 10, 0};
}
Vec2 MiniMapTransform::normalized(Vec3 point) const {
    const float x = (point.x - centre.x) / span.x, y = (point.y - centre.y) / span.y,
                c = std::cos(angle), s = std::sin(angle);
    return {.5f + x * c - y * s, .5f - x * s - y * c};
}
Vec3 MiniMapTransform::world(Vec2 point) const {
    const float x = point.x - .5f, y = point.y - .5f, c = std::cos(angle), s = std::sin(angle);
    return {centre.x + (x * c - y * s) * span.x, centre.y + (-x * s - y * c) * span.y, 0};
}
void MiniMapControl::reset(const Terrain& terrain) {
    zoom = .2f;
    int left = 124, top = 124, right = 132, bottom = 132;
    bool found = false;
    for (unsigned y = 0; y < terrain.height; ++y)
        for (unsigned x = 0; x < terrain.width; ++x)
            if (!(terrain.raw(x, y) & 0x8000)) {
                if (!found) {
                    left = right = static_cast<int>(x) + 1;
                    top = bottom = static_cast<int>(y);
                    found = true;
                } else {
                    left = std::min(left, static_cast<int>(x) + 1);
                    right = std::max(right, static_cast<int>(x) + 1);
                    top = std::min(top, static_cast<int>(y));
                    bottom = std::max(bottom, static_cast<int>(y));
                }
            }
    minimum = {left * 10.0f, top * 10.0f};
    maximum = {right * 10.0f, bottom * 10.0f};
}
void MiniMapControl::drag(float dy, unsigned screen_height) {
    const float step = 1 + 14.4f / std::max(screen_height, 1u);
    zoom = std::clamp(static_cast<float>(zoom * std::pow(step, dy)), .05f, 1.0f);
}
MiniMapTransform MiniMapControl::transform(const Terrain& terrain, Vec3 camera, float angle) const {
    return {{std::clamp(camera.x, minimum.x, maximum.x), std::clamp(camera.y, minimum.y, maximum.y)},
            {terrain.width * 10 * zoom, terrain.height * 10 * zoom}, angle};
}
bool minimap_box_contains(const MiniMapTransform& transform, const MiniMapLayout& layout,
                          Vec2 start, Vec2 end, Vec3 point) {
    std::array<Vec3, 4> corners;
    for (unsigned i = 0; i < 4; ++i)
        corners[i] = transform.world({((i & 1 ? end.x : start.x) - layout.bounds.origin.x) / layout.bounds.size.x,
                                      ((i & 2 ? end.y : start.y) - layout.bounds.origin.y) / layout.bounds.size.y});
    const auto cross = [&](unsigned a, unsigned b) {
        const auto& x = corners[a];
        const auto& y = corners[b];
        return (x.y - point.y) * (y.x - x.x) - (x.x - point.x) * (y.y - x.y);
    };
    const float first = cross(0, 1);
    return cross(1, 3) * first > .81399995f && cross(3, 2) * first > .81399995f &&
           cross(2, 0) * first > .81399995f;
}
Image terrain_minimap(const Terrain& terrain, const TerrainMaterials& materials,
                      std::span<const std::uint8_t> fog) {
    if (fog.size() != static_cast<std::size_t>(terrain.width) * terrain.height)
        throw Error("Minimap fog dimensions differ from terrain");
    Image result{terrain.width, terrain.height, {}};
    result.rgba.reserve(fog.size());
    for (unsigned y = 0; y < terrain.height; ++y)
        for (unsigned x = 0; x < terrain.width; ++x) {
            const auto index = static_cast<std::size_t>(y) * terrain.width + x;
            const auto tile = terrain.tiles[index];
            const auto& asset = materials.tiles[tile];
            const auto color = terrain.raw(x, y) & 0x8000 ? 0xff808000u :
                asset && !asset->mipmaps[6].rgba.empty() ? asset->mipmaps[6].rgba[0] : 0xff000000u;
            const unsigned level = fog[index] & 15, light = level ?
                128 - (14 - std::min(level, 14u)) * 80 / 14 : 0;
            const unsigned r = ((color & 255) >> 1) * light >> 10,
                           g = (((color >> 8) & 255) >> 1) * light >> 9,
                           b = (((color >> 16) & 255) >> 1) * light >> 10;
            result.rgba.push_back(0xff000000u | (r * 255 / 31) | (g * 255 / 63) << 8 | (b * 255 / 31) << 16);
        }
    return result;
}
bool HudRect::contains(Vec2 point) const {
    return point.x >= origin.x && point.y >= origin.y && point.x < origin.x + size.x &&
           point.y < origin.y + size.y;
}
SpellBarLayout SpellBarLayout::make(unsigned width, unsigned height) {
    SpellBarLayout result;
    result.scale = hud_scale(width, height);
    // The view truncates these two dimensions independently, before placing the slot pools.
    result.tab_size = static_cast<float>(static_cast<int>(48 * result.scale));
    result.slot_size = static_cast<float>(static_cast<int>(32 * result.scale));
    result.top = height - result.tab_size - result.slot_size;
    result.slot_top = result.top + result.tab_size;
    for (unsigned i = 0; i < result.tabs.size(); ++i)
        result.tabs[i] = {{i * result.tab_size, result.top}, {result.tab_size, result.tab_size}};
    for (unsigned i = 0; i < result.slots.size(); ++i)
        result.slots[i] = {{i * result.slot_size, result.slot_top}, {result.slot_size, result.slot_size}};
    return result;
}
std::optional<SpellBarLayout::Hit> SpellBarLayout::hit(Vec2 point, unsigned visible) const {
    for (unsigned i = 0; i < tabs.size(); ++i)
        if (tabs[i].contains(point))
            return Hit{true, i};
    for (unsigned i = 0; i < std::min<unsigned>(visible, static_cast<unsigned>(slots.size())); ++i)
        if (slots[i].contains(point))
            return Hit{false, i};
    return {};
}
SpellIconVisual SpellIconVisual::make(HudRect slot, int mana, int cost) {
    SpellIconVisual result{slot};
    mana = std::max(mana, 0);
    if (cost > mana) {
        const double ratio = static_cast<double>(mana) / cost;
        result.rectangle.size = {static_cast<float>(slot.size.x * ratio),
                                 static_cast<float>(slot.size.y * ratio)};
        result.alpha = static_cast<std::uint8_t>(160ll * mana / cost + 15);
        result.additive = true;
    }
    result.rectangle.origin = {slot.origin.x + (slot.size.x - result.rectangle.size.x - 1) * .5f,
                               slot.origin.y + (slot.size.x - result.rectangle.size.y - 1) * .5f};
    return result;
}
Mesh spell_cooldown(HudRect slot, int remaining, int duration) {
    Mesh result;
    if (remaining <= 0 || duration <= 0)
        return result;
    constexpr std::array<Vec2, 9> points{{{0, -1}, {.8f, -.8f}, {1, 0}, {.8f, .8f}, {0, 1},
                                         {-.8f, .8f}, {-1, 0}, {-.8f, -.8f}, {0, -1}}};
    const double complete = std::clamp(static_cast<double>(duration - remaining) / duration, 0.0, 1.0);
    const auto radius = static_cast<float>(static_cast<int>(slot.size.x * .625f));
    const Vec3 centre{slot.origin.x + slot.size.x * .5f, slot.origin.y + slot.size.y * .5f, 0};
    for (unsigned i = 0; i < 8; ++i) {
        const double start = i * .125, end = start + .125;
        if (complete >= end)
            continue;
        const auto fraction = static_cast<float>(std::max(complete - start, 0.0) * 8);
        const auto a = points[i], b = points[i + 1];
        Vertex first, last, middle;
        first.position = {centre.x + radius * (a.x + (b.x - a.x) * fraction),
                          centre.y + radius * (a.y + (b.y - a.y) * fraction), 0};
        last.position = {centre.x + radius * b.x, centre.y + radius * b.y, 0};
        middle.position = centre;
        first.color = last.color = 0x007f6000;
        middle.color = 0xffffc000;
        const auto base = static_cast<std::uint32_t>(result.vertices.size());
        result.vertices.insert(result.vertices.end(), {first, last, middle});
        result.indices.insert(result.indices.end(), {base, base + 1, base + 2});
    }
    return result;
}
} // namespace sacrifice
