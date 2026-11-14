#include "renderer_shared.hpp"
#include "sacrifice/scene.hpp"
#include "sacrifice/flora.hpp"
#include "sacrifice/widgets.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <ranges>
namespace sacrifice {
void Camera::update(const InputState& input, float dt) {
    if (input.looking) {
        yaw += input.mouse_x * 0.003f;
        pitch = std::clamp(pitch - input.mouse_y * 0.003f, -1.5f, 1.5f);
    }
    float step = speed * dt * (input.keys[16] ? 3.0f : 1.0f);
    float forward = (input.keys['W'] ? 1.0f : 0) - (input.keys['S'] ? 1.0f : 0),
          right = (input.keys['D'] ? 1.0f : 0) - (input.keys['A'] ? 1.0f : 0);
    position.x += (std::sin(yaw) * forward + std::cos(yaw) * right) * step;
    position.z += (std::cos(yaw) * forward - std::sin(yaw) * right) * step;
    position.y += ((input.keys['E'] ? 1.0f : 0) - (input.keys['Q'] ? 1.0f : 0)) * step;
}
namespace renderer_shared {
ParticleGeometry expand_particles(std::span<const ParticleBatch> batches, const Camera& camera, Vec3 r,
                                  Vec3 u, Vec3 f, float near_plane) {
    std::size_t vertices{}, indices{};
    for (const auto& batch : batches) {
        vertices += batch.quads.size() * 4 + batch.mesh.vertices.size();
        indices += batch.quads.size() * 6 + batch.mesh.indices.size();
    }
    ParticleGeometry geometry;
    if (!vertices)
        return geometry;
    geometry.vertices.resize(vertices);
    auto* out = geometry.vertices.data();
    std::vector<ParticleRange> ranges;
    unsigned written = 0;
    std::vector<std::uint32_t> effect_indices;
    effect_indices.reserve(indices);
    std::vector<float> effect_depths;
    effect_depths.reserve(vertices);
    const auto view_depth = [&](const Vec3& at) {
        return (at.x - camera.position.x) * f.x + (at.y - camera.position.y) * f.y +
               (at.z - camera.position.z) * f.z;
    };
    for (const auto& batch : batches) {
        const auto first = static_cast<unsigned>(effect_indices.size());
        for (const auto& q : batch.quads) {
            const Vec3 at{q.center.x, q.center.z, q.center.y};
            const float depth = (at.x - camera.position.x) * f.x + (at.y - camera.position.y) * f.y +
                                (at.z - camera.position.z) * f.z;
            if (depth <= near_plane || depth * 0.5f < q.size)
                continue;
            const Vec3 quad_right =
                           q.generator_aligned ? Vec3{q.right.x, q.right.z, q.right.y} : Vec3{r.x, r.y, r.z},
                       quad_up = q.generator_aligned ? Vec3{q.up.x, q.up.z, q.up.y} : Vec3{u.x, u.y, u.z};
            const Vec3 rx{quad_right.x * q.size, quad_right.y * q.size, quad_right.z * q.size},
                uy{quad_up.x * q.size, quad_up.y * q.size, quad_up.z * q.size};
            const Vec3 corners[4]{{at.x - rx.x + uy.x, at.y - rx.y + uy.y, at.z - rx.z + uy.z},
                                  {at.x + rx.x + uy.x, at.y + rx.y + uy.y, at.z + rx.z + uy.z},
                                  {at.x - rx.x - uy.x, at.y - rx.y - uy.y, at.z - rx.z - uy.z},
                                  {at.x + rx.x - uy.x, at.y + rx.y - uy.y, at.z + rx.z - uy.z}};
            for (unsigned i = 0; i < 4; ++i) {
                out[written + i] = {corners[i],
                                    {0, 1, 0},
                                    {i & 1 ? q.uv1.x : q.uv0.x, i & 2 ? q.uv1.y : q.uv0.y},
                                    q.color,
                                    0xffffffffu};
                effect_depths.push_back(view_depth(corners[i]));
            }
            effect_indices.insert(effect_indices.end(),
                                  {written, written + 1, written + 2, written + 1, written + 3, written + 2});
            written += 4;
        }
        std::ranges::copy(batch.mesh.vertices, out + written);
        for (const auto& vertex : batch.mesh.vertices)
            effect_depths.push_back(view_depth(vertex.position));
        for (const auto index : batch.mesh.indices)
            effect_indices.push_back(index + written);
        written += static_cast<unsigned>(batch.mesh.vertices.size());
        if (effect_indices.size() > first) {
            // World transparency does not write depth. Sort its triangles across
            // material/host boundaries, so a returning funnel covers dust behind
            // it instead of the trail's later submission painting over the funnel.
            const bool sorted =
                (batch.flags & 0x7f00) && (batch.flags & 0x02000000) && !(batch.flags & 0x04000000);
            if (sorted) {
                for (unsigned i = first; i < effect_indices.size(); i += 3) {
                    float depth = 0;
                    for (unsigned corner = 0; corner < 3; ++corner)
                        depth += effect_depths[effect_indices[i + corner]];
                    ranges.push_back({&batch, i, 3, depth / 3, true});
                }
            } else
                ranges.push_back({&batch, first, static_cast<unsigned>(effect_indices.size() - first)});
        }
    }
    // Opaque, depth-writing effects (spike and vine walls, land blocks) draw before every blended one, so a
    // translucent effect drawn earlier (a tornado) cannot be painted over by spikes standing behind it.
    std::ranges::stable_partition(ranges, [](const ParticleRange& range) {
        const auto flags = range.batch->flags;
        return !(flags & 0x7f00) && !(flags & 0x06000000);
    });
    // Other depth-writing effects and screen overlays keep their submission order and
    // form boundaries between sortable world-effect runs.
    for (std::size_t first = 0; first < ranges.size();) {
        if (!ranges[first].sorted) {
            ++first;
            continue;
        }
        auto end = first + 1;
        while (end < ranges.size() && ranges[end].sorted)
            ++end;
        std::stable_sort(ranges.begin() + first, ranges.begin() + end,
                         [](const ParticleRange& a, const ParticleRange& b) { return a.depth > b.depth; });
        first = end;
    }
    std::vector<std::uint32_t> ordered_indices;
    ordered_indices.reserve(effect_indices.size());
    std::vector<ParticleRange> draws;
    for (const auto& range : ranges) {
        const auto first = static_cast<unsigned>(ordered_indices.size());
        ordered_indices.insert(ordered_indices.end(), effect_indices.begin() + range.first,
                               effect_indices.begin() + range.first + range.count);
        if (!draws.empty() && draws.back().batch == range.batch)
            draws.back().count += range.count;
        else
            draws.push_back({range.batch, first, range.count});
    }
    geometry.vertices.resize(written);
    geometry.indices = std::move(ordered_indices);
    geometry.ranges = std::move(draws);
    return geometry;
}
void TerrainView::compose_window(unsigned slot, const TerrainLod::Window& window) {
    std::vector<std::uint32_t> pixels(256 * 256);
    const unsigned size = lod->size, texels = window.texels, mip = 6 - std::countr_zero(texels);
    for (unsigned cy = 0; cy < window.cells; ++cy)
        for (unsigned cx = 0; cx < window.cells; ++cx) {
            const auto tile = cell_tiles[static_cast<std::size_t>(window.y + cy) * size + window.x + cx];
            if (tile >= tile_mips.size())
                continue;
            const auto& source = tile_mips[tile][mip - 1];
            for (unsigned ty = 0; ty < texels; ++ty)
                std::copy_n(source.begin() + ty * texels, texels,
                            pixels.begin() + (cy * texels + ty) * 256 + cx * texels);
        }
    window_pixels[slot] = std::move(pixels);
    window_dirty[slot] = true;
}
void TerrainView::reset(const Terrain& terrain) {
    lod = TerrainLod::build(terrain);
    if (!lod->size) {
        lod.reset();
        return;
    }
    cell_tiles.resize(static_cast<std::size_t>(terrain.width) * terrain.height);
    for (std::size_t cell = 0; cell < cell_tiles.size(); ++cell)
        cell_tiles[cell] = static_cast<std::uint16_t>(
            !terrain.scar_layers.empty() && terrain.scar_layers[cell] ? terrain.scar_layers[cell]
                                                                      : terrain.tiles[cell]);
    cell_bases = terrain.tiles;
    land_heights_ = terrain.heights;
    land_light_ = terrain.lightmap;
    const auto grid = terrain.mesh(false);
    const unsigned size = lod->size, pitch = size + 1;
    lod_grid.resize(static_cast<std::size_t>(pitch) * pitch);
    for (unsigned y = 0; y <= size; ++y)
        for (unsigned x = 0; x <= size; ++x) {
            auto v =
                grid.vertices[static_cast<std::size_t>(std::min(y, size - 1)) * size + std::min(x, size - 1)];
            v.position = {x * 10.0f, terrain.at(x, y), y * 10.0f};
            lod_grid[static_cast<std::size_t>(y) * pitch + x] = v;
        }
    composed_valid = false;
    land = {};
    grass_cells_.clear();
    tile_grass_.clear();
    ++grass_generation_;
}
std::vector<std::size_t> TerrainView::update_tiles(const TerrainMaterials& materials, const Terrain& terrain,
                                                   bool full) {
    const auto layers = 256 + materials.scars.size();
    // Scar layers are only ever added within one tile set; fewer layers means a new one.
    full = full || tile_mips.size() < 256 || layers < tile_mips.size();
    const auto source_of = [&](std::size_t tile) -> const TerrainTile* {
        return tile < 256 ? (materials.tiles[tile] ? &*materials.tiles[tile] : nullptr) : &materials.scars[tile - 256];
    };
    std::vector<std::size_t> changed;
    std::vector<bool> redrawn(layers);
    if (full) {
        tile_mips.assign(layers, {});
        scar_top_.clear();
    } else
        tile_mips.resize(layers);
    scar_top_.resize(layers - 256);
    for (std::size_t tile = full ? 0 : 256; tile < layers; ++tile) {
        const auto* source = source_of(tile);
        auto& mips = tile_mips[tile];
        if (!full && source && !mips[0].empty() && scar_top_[tile - 256] == source->mipmaps[0].rgba &&
            std::ranges::all_of(std::views::iota(1u, 7u),
                                [&](unsigned level) { return mips[level - 1] == source->mipmaps[level].rgba; }))
            continue;
        for (unsigned level = 1; level < 7; ++level)
            mips[level - 1] = source ? source->mipmaps[level].rgba
                                     : std::vector<std::uint32_t>((64u >> level) * (64u >> level), 0xff000000u);
        if (tile >= 256)
            scar_top_[tile - 256] = source->mipmaps[0].rgba;
        changed.push_back(tile);
        redrawn[tile] = true;
    }
    if (full) {
        composed_valid = false;
        land = {};
        grass_cells_.clear();
        tile_grass_.clear();
        dirty_grass_chunks_.clear();
        ++grass_generation_;
        if (lod && terrain.tiles.size() == cell_tiles.size()) {
            for (std::size_t cell = 0; cell < cell_tiles.size(); ++cell)
                cell_tiles[cell] = static_cast<std::uint16_t>(
                    !terrain.scar_layers.empty() && terrain.scar_layers[cell] ? terrain.scar_layers[cell]
                                                                              : terrain.tiles[cell]);
            cell_bases = terrain.tiles;
        }
        return changed;
    }
    if (!changed.empty())
        tile_grass_.clear(); // Reclassified on the next grass build (prepare_grass).
    if (lod && terrain.tiles.size() == cell_tiles.size()) {
        const auto [x0, y0, x1, y1] = refresh_cells(terrain, redrawn);
        if (x1 >= x0)
            invalidate_cells(x0 - 1, y0 - 1, x1 + 1, y1 + 1, true); // Grass blends the neighbouring cells' tiles.
    }
    return changed;
}
std::array<int, 4> TerrainView::refresh_cells(const Terrain& terrain, const std::vector<bool>& redrawn) {
    const int size = static_cast<int>(lod->size);
    std::array<int, 4> bounds{size, size, -1, -1};
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const auto cell = static_cast<std::size_t>(y) * size + x;
            const auto layer = static_cast<std::uint16_t>(
                !terrain.scar_layers.empty() && terrain.scar_layers[cell] ? terrain.scar_layers[cell]
                                                                          : terrain.tiles[cell]);
            if (layer == cell_tiles[cell] && terrain.tiles[cell] == cell_bases[cell] &&
                !(layer < redrawn.size() && redrawn[layer]))
                continue;
            cell_tiles[cell] = layer;
            cell_bases[cell] = terrain.tiles[cell];
            bounds = {std::min(bounds[0], x), std::min(bounds[1], y), std::max(bounds[2], x), std::max(bounds[3], y)};
        }
    return bounds;
}
void TerrainView::invalidate_cells(int x0, int y0, int x1, int y1, bool layers) {
    const int size = static_cast<int>(lod->size);
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, size - 1);
    y1 = std::min(y1, size - 1);
    if (x1 < x0 || y1 < y0)
        return;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            grass_cells_.erase(static_cast<std::int64_t>(y) * 65536 + x);
    constexpr int cells = grass_chunk_cells;
    for (int y = y0 / cells; y <= y1 / cells; ++y)
        for (int x = x0 / cells; x <= x1 / cells; ++x)
            dirty_grass_chunks_.push_back(static_cast<std::int64_t>(y) * 65536 + x);
    if (dirty_grass_chunks_.size() > 4096) { // Nobody drained the list (the WebGPU backend caches no chunks).
        dirty_grass_chunks_.clear();
        ++grass_generation_;
    }
    for (unsigned i = 0; layers && i < 6; ++i) { // Window texels only show the cells' layers.
        const auto& w = composed[i];
        const int wx1 = w.x + static_cast<int>(w.cells) - 1, wy1 = w.y + static_cast<int>(w.cells) - 1;
        recompose_[i] = recompose_[i] || (x0 <= wx1 && w.x <= x1 && y0 <= wy1 && w.y <= y1);
    }
    land = {}; // Rebuild the faces (they carry the cells' layers and the grid's heights), as reset() does.
}
bool TerrainView::refresh(const Terrain& terrain) {
    if (!lod || terrain.width != lod->size || terrain.height != lod->size || lod_grid.empty() ||
        terrain.tiles.size() != cell_tiles.size() || terrain.lightmap.size() != cell_tiles.size())
        return false;
    const int size = static_cast<int>(lod->size), pitch = size + 1;
    const auto grid_point = [&](int x, int y) { // As reset() fills lod_grid.
        auto v = terrain.grid_vertex(static_cast<unsigned>(std::min(x, size - 1)),
                                     static_cast<unsigned>(std::min(y, size - 1)));
        v.position = {x * 10.0f, terrain.at(static_cast<unsigned>(x), static_cast<unsigned>(y)), y * 10.0f};
        return v;
    };
    if (land_heights_.size() != terrain.heights.size() || land_light_.size() != terrain.lightmap.size())
        return false;
    // Changed grid points: a changed height sample (with its hole bit) or baked light.
    int x0 = pitch, y0 = pitch, x1 = -1, y1 = -1;
    const auto touch = [&](int x, int y) {
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    };
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const auto sample = static_cast<std::size_t>(y) * size + x;
            if (terrain.heights[sample] != land_heights_[sample]) {
                touch(x, y);
                // Terrain::raw: the extra column reads the next row's first sample, the extra row row 0.
                if (!x)
                    touch(size, y ? y - 1 : size);
                if (!y)
                    touch(x, size);
                if (!x && !y)
                    touch(size, size - 1);
            }
            if (terrain.lightmap[sample] != land_light_[sample]) {
                touch(x, y);
                touch(x == size - 1 ? size : x, y == size - 1 ? size : y); // Copied into the extra row/column.
            }
        }
    land_heights_ = terrain.heights;
    land_light_ = terrain.lightmap;
    if (x1 >= x0) {
        lod->update(terrain, x0, y0, x1, y1);
        // Normals read the neighbouring samples; the last row and column repeat the ones before them.
        const int gx1 = std::min(x1 + 1, size), gy1 = std::min(y1 + 1, size);
        for (int y = std::max(y0 - 1, 0); y <= gy1; ++y)
            for (int x = std::max(x0 - 1, 0); x <= gx1; ++x)
                lod_grid[static_cast<std::size_t>(y) * pitch + x] = grid_point(x, y);
        if (gx1 == size - 1)
            for (int y = std::max(y0 - 1, 0); y <= gy1; ++y)
                lod_grid[static_cast<std::size_t>(y) * pitch + size] = grid_point(size, y);
        if (gy1 == size - 1)
            for (int x = std::max(x0 - 1, 0); x <= gx1; ++x)
                lod_grid[static_cast<std::size_t>(size) * pitch + x] = grid_point(x, size);
        // A point reaches the cells on both sides of it, and grass blends one cell further.
        invalidate_cells(x0 - 2, y0 - 2, x1 + 1, y1 + 1, false);
    }
    const auto [cx0, cy0, cx1, cy1] = refresh_cells(terrain, {});
    if (cx1 >= cx0)
        invalidate_cells(cx0 - 1, cy0 - 1, cx1 + 1, cy1 + 1, true);
    return true;
}
namespace {
Vec3 unpack_rgb(std::uint32_t c) {
    return {(c & 255) / 255.0f, (c >> 8 & 255) / 255.0f, (c >> 16 & 255) / 255.0f};
}
std::uint32_t pack_rgb(Vec3 c) {
    const auto byte = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255 + .5f); };
    return 0xff000000u | byte(c.x) | byte(c.y) << 8 | byte(c.z) << 16;
}
float smooth(float low, float high, float v) {
    const float t = std::clamp((v - low) / (high - low), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}
} // namespace
float TerrainView::clearing(float x, float y) const {
    float open = 1;
    for (const auto& c : clearings_) {
        const float d = std::sqrt((x - c.x) * (x - c.x) + (y - c.y) * (y - c.y));
        open = std::min(open, std::clamp((d - c.z) / 10, 0.0f, 1.0f));
    }
    return open;
}
// One map cell's blades, generated from hashes of the cell (never the match's random stream). Density and
// colour blend between cell centres, so grass thins out across tile borders rather than in squares.
const std::vector<TerrainView::Blade>& TerrainView::grass_cell(const Terrain& terrain, int cx, int cy) {
    const auto key = static_cast<std::int64_t>(cy) * 65536 + cx;
    if (const auto found = grass_cells_.find(key); found != grass_cells_.end())
        return found->second;
    auto& blades = grass_cells_[key];
    const int size = static_cast<int>(lod->size);
    const unsigned pitch = lod->size + 1;
    const auto tile_at = [&](int x, int y) {
        x = std::clamp(x, 0, size - 1);
        y = std::clamp(y, 0, size - 1);
        return cell_tiles[static_cast<std::size_t>(y) * size + x];
    };
    const auto light_at = [&](float x, float y) { // Bilinear baked ground light at map point (x, y).
        const float gx = std::clamp(x / 10, 0.0f, static_cast<float>(size)), gy = std::clamp(y / 10, 0.0f, static_cast<float>(size));
        const auto x0 = std::min(static_cast<unsigned>(gx), lod->size - 1), y0 = std::min(static_cast<unsigned>(gy), lod->size - 1);
        const float fx = gx - x0, fy = gy - y0;
        const auto at = [&](unsigned px, unsigned py) { return unpack_rgb(lod_grid[py * pitch + px].color); };
        const auto a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
        const auto mix = [&](float p, float q, float r, float t) { return (p * (1 - fx) + q * fx) * (1 - fy) + (r * (1 - fx) + t * fx) * fy; };
        return Vec3{mix(a.x, b.x, c.x, d.x), mix(a.y, b.y, c.y, d.y), mix(a.z, b.z, c.z, d.z)};
    };
    const unsigned candidates = static_cast<unsigned>(tuning::grass_density * 100);
    for (unsigned i = 0; i < candidates; ++i) {
        auto hash = static_cast<unsigned>(cx) * 0x9e3779b9u ^ static_cast<unsigned>(cy) * 0x85ebca77u ^ (i + 1) * 0xc2b2ae3du;
        const auto next = [&] {
            hash ^= hash >> 15;
            hash *= 0x2c1b3c6du;
            hash ^= hash >> 12;
            hash *= 0x297a2d39u;
            hash ^= hash >> 15;
            return (hash & 0xffff) / 65535.0f;
        };
        const float x = (cx + next()) * 10, y = (cy + next()) * 10;
        // Bilinear grassiness and colour from the four nearest cell centres.
        const float fx = x / 10 - .5f, fy = y / 10 - .5f;
        const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
        const float tx = fx - x0, ty = fy - y0;
        float density = 0;
        Vec3 colour{};
        for (int k = 0; k < 4; ++k) {
            const auto tile = tile_at(x0 + (k & 1), y0 + (k >> 1));
            const float weight = ((k & 1) ? tx : 1 - tx) * ((k >> 1) ? ty : 1 - ty);
            density += tile_grass_[tile] * weight;
            colour = {colour.x + tile_colour_[tile].x * weight, colour.y + tile_colour_[tile].y * weight,
                      colour.z + tile_colour_[tile].z * weight};
        }
        if (next() >= density * clearing(x, y))
            continue;
        const float ground = terrain_height(terrain, x, y);
        if (ground == 0 && terrain_height(terrain, x, y, true) != 0)
            continue; // A hole.
        const auto light = light_at(x, y);
        const float shade = .85f + .3f * next();
        // The tile's average is greyer than its texels, so the blade takes back saturation to match the
        // ground around it; tips are lighter and a little yellower.
        const float k = tuning::ground_brightness * shade;
        Vec3 lit{colour.x * light.x * k, colour.y * light.y * k, colour.z * light.z * k};
        const float luma = lit.x * .299f + lit.y * .587f + lit.z * .114f;
        const float boost = colour.y > colour.x ? 1.7f : 1.15f; // Less for dry (olive, straw) grass.
        lit = {luma + (lit.x - luma) * boost, luma + (lit.y - luma) * boost, luma + (lit.z - luma) * boost};
        const Vec3 root{lit.x * .75f, lit.y * .75f, lit.z * .75f};
        const Vec3 tip{lit.x * 1.25f + .04f, lit.y * 1.3f + .04f, lit.z * 1.05f};
        Blade blade{};
        blade.x = x;
        blade.y = ground;
        blade.z = y;
        blade.height = tuning::grass_height * (.55f + .9f * next()) * (.55f + .45f * density);
        blade.width = tuning::grass_width * (.7f + .6f * next());
        blade.angle = next() * 3.1415927f;
        const float lean = next() * 6.2831853f, amount = blade.height * .3f * next();
        blade.lean_x = std::cos(lean) * amount;
        blade.lean_z = std::sin(lean) * amount;
        blade.root = pack_rgb(root);
        blade.tip = pack_rgb(tip);
        blade.hash = static_cast<std::uint8_t>(hash >> 24);
        blade.rank = static_cast<std::uint8_t>(next() * 255);
        blades.push_back(blade);
    }
    // Ground clutter (Remastered option 4): flowers and mushrooms in lush grass, twigs and pebbles on dry
    // or bare ground, by the local grassiness. Each takes the ground's light, mostly as brightness.
    const unsigned scattered = static_cast<unsigned>(tuning::clutter_density * 100 + .5f);
    for (unsigned i = 0; i < scattered; ++i) {
        auto hash = static_cast<unsigned>(cx) * 0x7feb352du ^ static_cast<unsigned>(cy) * 0x846ca68bu ^ (i + 1) * 0x9e3779b9u;
        const auto next = [&] {
            hash ^= hash >> 16;
            hash *= 0x7feb352du;
            hash ^= hash >> 15;
            hash *= 0x846ca68bu;
            hash ^= hash >> 16;
            return (hash & 0xffff) / 65535.0f;
        };
        const float x = (cx + next()) * 10, y = (cy + next()) * 10;
        const auto tile = tile_at(static_cast<int>(x / 10), static_cast<int>(y / 10));
        if (tile >= 256 || clearing(x, y) < 1)
            continue; // Nothing on scar decals or around fountains and altars.
        const float lush = tile_grass_[tile];
        const auto c = tile_colour_[tile];
        const float high = std::max({c.x, c.y, c.z}), low = std::min({c.x, c.y, c.z});
        // Snow (bright and grey) or ice (bright and blue).
        const bool snow = (high > .55f && high - low < .15f * high) || (high > .45f && c.z > c.x + .08f && c.z >= c.y);
        const float pick = next();
        unsigned model;
        if (lush > .4f)
            model = pick < .62f ? clutter::flowers + static_cast<unsigned>(next() * clutter::flower_count) % clutter::flower_count
                  : pick < .74f ? clutter::mushrooms + static_cast<unsigned>(next() * clutter::mushroom_count) % clutter::mushroom_count
                  : pick < .9f  ? clutter::pebbles + static_cast<unsigned>(next() * clutter::pebble_count) % clutter::pebble_count
                                : clutter::twigs + static_cast<unsigned>(next() * clutter::twig_count) % clutter::twig_count;
        else if (lush > .05f)
            model = pick < .2f ? clutter::flowers + 2 + static_cast<unsigned>(next() * 2) % 2 // Yellow.
                  : pick < .6f ? clutter::pebbles + static_cast<unsigned>(next() * clutter::pebble_count) % clutter::pebble_count
                               : clutter::twigs + static_cast<unsigned>(next() * clutter::twig_count) % clutter::twig_count;
        else if (snow ? pick > .1f : pick > .5f)
            continue; // Bare ground stays mostly bare; snow and ice almost entirely.
        else
            model = pick < (snow ? 1 : .35f)
                        ? clutter::pebbles + static_cast<unsigned>(next() * clutter::pebble_count) % clutter::pebble_count
                        : clutter::twigs + static_cast<unsigned>(next() * clutter::twig_count) % clutter::twig_count;
        const float ground = terrain_height(terrain, x, y);
        if (ground == 0 && terrain_height(terrain, x, y, true) != 0)
            continue;
        auto light = light_at(x, y);
        const float l = light.x * .299f + light.y * .587f + light.z * .114f, k = tuning::ground_brightness;
        light = {(l + (light.x - l) * .35f) * k, (l + (light.y - l) * .35f) * k, (l + (light.z - l) * .35f) * k};
        if (model >= clutter::pebbles && model < clutter::pebbles + clutter::pebble_count) // Stones take the ground's hue.
            light = {light.x * (.6f + .8f * c.x), light.y * (.6f + .8f * c.y), light.z * (.6f + .8f * c.z)};
        Blade item{};
        item.x = x;
        item.y = ground;
        item.z = y;
        const bool stone = model >= clutter::pebbles && model < clutter::pebbles + clutter::pebble_count;
        item.height = tuning::clutter_size * (stone ? .55f : 1.0f) * (.7f + .6f * next());
        item.angle = next() * 6.2831853f;
        item.root = pack_rgb(light);
        item.hash = static_cast<std::uint8_t>(hash >> 24);
        item.rank = static_cast<std::uint8_t>(next() * 255);
        item.clutter = static_cast<std::uint8_t>(model + 1);
        blades.push_back(item);
    }
    return blades;
}
void TerrainView::prepare_grass() {
    if (tile_grass_.size() != tile_mips.size()) {
        // Grassy tiles by their average colour (the 1x1 mip): green ones are lush, olive or straw ones get
        // sparse dry grass, and grey, white, dark or other hues (snow, stone, ash, flesh) stay bare.
        tile_grass_.assign(tile_mips.size(), 0);
        tile_colour_.assign(tile_mips.size(), {});
        for (std::size_t tile = 0; tile < tile_mips.size(); ++tile) {
            if (tile_mips[tile][5].empty())
                continue;
            const auto c = unpack_rgb(tile_mips[tile][5][0]);
            const float high = std::max({c.x, c.y, c.z}), low = std::min({c.x, c.y, c.z});
            const float saturation = (high - low) / std::max(high, 1e-3f);
            const float green = smooth(.02f, .1f, c.y - std::max(c.x, c.z)) * smooth(.04f, .12f, c.y - c.z) * smooth(.12f, .3f, saturation);
            const float dry = (c.x > c.z + .08f && c.y > c.z + .06f && c.x - c.y < .2f)
                                  ? .45f * smooth(.15f, .35f, saturation) : 0.0f;
            // Scar decals (layers from 256: mana fountain pools, burns, paths) stay bare.
            tile_grass_[tile] = tile >= 256 ? 0.0f : std::max(green, dry) * smooth(.08f, .2f, high);
            tile_colour_[tile] = c;
        }
    }
}
void TerrainView::emit_blade(const Blade& blade, float grow, bool encode, std::vector<Vertex>& vertices,
                             std::vector<std::uint32_t>& indices) {
    const auto layer = [&](float h) { return foliage_layer(h, blade.hash, true); };
    const Vec3 root = encode ? Vec3{blade.x, blade.y, blade.z} : Vec3{0, 1, 0};
    const Vec2 rank{encode ? blade.rank / 255.0f : 0, 0};
    if (blade.clutter) {
        const auto& model = clutter::models()[blade.clutter - 1];
        const float scale = blade.height * grow, c = std::cos(blade.angle), s = std::sin(blade.angle);
        const auto base = static_cast<std::uint32_t>(vertices.size());
        for (const auto& point : model.points) {
            const auto v = point.position;
            const auto channel = [&](int shift) {
                return std::min(255u, ((blade.root >> shift & 255) * (point.color >> shift & 255) + 127) / 255) << shift;
            };
            vertices.push_back({{blade.x + (v.x * c + v.y * s) * scale, blade.y + v.z * scale,
                                 blade.z + (v.y * c - v.x * s) * scale},
                                root, rank, 0xff000000u | channel(0) | channel(8) | channel(16),
                                layer(model.rigid ? 0 : v.z * scale)});
        }
        for (const auto& triangle : model.triangles)
            indices.insert(indices.end(), {base + triangle[0], base + triangle[1], base + triangle[2]});
        return;
    }
    // A tuft of three blades fanning out from one root, the outer ones shorter and leaning away.
    for (int k = 0; k < 3; ++k) {
        const float angle = blade.angle + k * 2.0943951f, height = blade.height * grow * (k ? .7f : 1.0f);
        const float c = std::cos(angle) * blade.width * .5f, s = std::sin(angle) * blade.width * .5f;
        const float lean_x = k ? blade.lean_x * .5f + s * 2.5f * (k == 1 ? 1 : -1) : blade.lean_x,
                    lean_z = k ? blade.lean_z * .5f - c * 2.5f * (k == 1 ? 1 : -1) : blade.lean_z;
        const auto base = static_cast<std::uint32_t>(vertices.size());
        vertices.push_back({{blade.x - c, blade.y, blade.z - s}, root, rank, blade.root, layer(0)});
        vertices.push_back({{blade.x + c, blade.y, blade.z + s}, root, rank, blade.root, layer(0)});
        vertices.push_back({{blade.x + lean_x * grow, blade.y + height, blade.z + lean_z * grow},
                            root, rank, blade.tip, layer(height)});
        indices.insert(indices.end(), {base, base + 1, base + 2});
    }
}
void TerrainView::grass_chunk(const Terrain& terrain, int chunk_x, int chunk_y, std::vector<Vertex>& vertices,
                              std::vector<std::uint32_t>& indices, std::array<unsigned, 17>& rank_indices) {
    vertices.clear();
    indices.clear();
    rank_indices.fill(0);
    if (!grass_ready())
        return;
    prepare_grass();
    const int size = static_cast<int>(lod->size);
    std::vector<Blade> blades;
    for (int cy = chunk_y * grass_chunk_cells; cy < std::min(size, (chunk_y + 1) * grass_chunk_cells); ++cy)
        for (int cx = chunk_x * grass_chunk_cells; cx < std::min(size, (chunk_x + 1) * grass_chunk_cells); ++cx) {
            const auto& cell = grass_cell(terrain, cx, cy);
            blades.insert(blades.end(), cell.begin(), cell.end());
            grass_cells_.erase(static_cast<std::int64_t>(cy) * 65536 + cx); // Kept on the GPU instead.
        }
    std::ranges::stable_sort(blades, {}, &Blade::rank);
    unsigned bucket = 1;
    for (const auto& blade : blades) {
        for (; bucket <= 16 && blade.rank / 255.0f >= bucket / 16.0f; ++bucket)
            rank_indices[bucket] = static_cast<unsigned>(indices.size());
        emit_blade(blade, 1, true, vertices, indices);
    }
    for (; bucket <= 16; ++bucket)
        rank_indices[bucket] = static_cast<unsigned>(indices.size());
}
void TerrainView::grass(const Terrain& terrain, const Camera& camera, std::vector<Vertex>& vertices,
                        std::vector<std::uint32_t>& indices) {
    vertices.clear();
    indices.clear();
    if (!grass_ready())
        return;
    prepare_grass();
    const float reach = tuning::grass_range;
    const float ex = camera.position.x, ez = camera.position.z;
    const float fx = std::sin(camera.yaw), fz = std::cos(camera.yaw);
    // Forget cells well out of reach, so the cache stays the size of the view.
    for (auto it = grass_cells_.begin(); it != grass_cells_.end();) {
        const float x = (static_cast<float>(it->first % 65536) + .5f) * 10 - ex,
                    z = (static_cast<float>(it->first / 65536) + .5f) * 10 - ez;
        it = x * x + z * z > reach * reach * 4 ? grass_cells_.erase(it) : std::next(it);
    }
    const int size = static_cast<int>(lod->size);
    const int x0 = std::max(0, static_cast<int>(std::floor((ex - reach) / 10))),
              x1 = std::min(size - 1, static_cast<int>(std::floor((ex + reach) / 10))),
              y0 = std::max(0, static_cast<int>(std::floor((ez - reach) / 10))),
              y1 = std::min(size - 1, static_cast<int>(std::floor((ez + reach) / 10)));
    for (int cy = y0; cy <= y1; ++cy)
        for (int cx = x0; cx <= x1; ++cx) {
            const float dx = (cx + .5f) * 10 - ex, dz = (cy + .5f) * 10 - ez;
            if (dx * dx + dz * dz > (reach + 8) * (reach + 8))
                continue;
            if (dx * fx + dz * fz < -10 && dx * dx + dz * dz > 400)
                continue; // Behind the camera.
            for (const auto& blade : grass_cell(terrain, cx, cy)) {
                const float bx = blade.x - ex, bz = blade.z - ez;
                const float t = std::sqrt(bx * bx + bz * bz) / reach;
                // Thinned past flora_thin_start of the reach, and shrinking away at the edge: each tuft shrinks to nothing as
                // its rank passes the thinning line, rather than popping.
                if (t >= 1)
                    continue;
                const float keep = grass_keep(t);
                const float thin = std::clamp((keep - blade.rank / 255.0f) * 8, 0.0f, 1.0f);
                if (thin <= 0)
                    continue;
                const float grow = std::min(1.0f, (1 - t) * 5) * thin;
                emit_blade(blade, grow, false, vertices, indices);
            }
        }
}
std::vector<std::vector<std::uint32_t>> detail_mips(const TerrainMaterials& materials) {
    std::vector<std::vector<std::uint32_t>> levels;
    for (const auto& detail : materials.details) {
        std::vector<std::uint32_t> level(256 * 256);
        if (!detail.empty()) {
            // 3d_d3d.dll 0x1800c8d0 (X1R5G5B5 bump maps): per texel, cross((sx, 0, dx / 255), (0, sy, dy /
            // 255)) with sx = sy = 1 / (255 x scale) over the next texel's index differences (zero past the
            // last column and row), normalized and packed at n x 15.5 + 16 per channel.
            const float step = 1.0f / (255.0f * materials.detail_bump_scale);
            for (unsigned y = 0; y < 256; ++y)
                for (unsigned x = 0; x < 256; ++x) {
                    const auto at = [&](unsigned u, unsigned v) {
                        return static_cast<int>(detail[v * 256 + u]);
                    };
                    const float dx = x < 255 ? (at(x + 1, y) - at(x, y)) * 0.0039215689f : 0,
                                dy = y < 255 ? (at(x, y + 1) - at(x, y)) * 0.0039215689f : 0;
                    float n[3]{-dx * step, -step * dy, step * step};
                    const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    std::uint32_t texel = static_cast<std::uint32_t>(at(x, y)) << 24;
                    for (unsigned c = 0; c < 3; ++c) {
                        const auto five = static_cast<unsigned>(n[c] / length * 15.5f + 16.0f);
                        texel |= (five * 255 + 15) / 31 << (8 * c);
                    }
                    level[y * 256 + x] = texel;
                }
        }
        for (unsigned size = 256;; size /= 2) {
            levels.push_back(level);
            if (size == 1)
                break;
            std::vector<std::uint32_t> next(size / 2 * (size / 2));
            for (unsigned y = 0; y < size / 2; ++y)
                for (unsigned x = 0; x < size / 2; ++x) {
                    const auto* row = &level[2 * y * size + 2 * x];
                    std::uint32_t texel = 0;
                    for (unsigned c = 0; c < 32; c += 8)
                        texel |= ((row[0] >> c & 255) + (row[1] >> c & 255) + (row[size] >> c & 255) +
                                  (row[size + 1] >> c & 255) + 2) /
                                     4
                                 << c;
                    next[y * (size / 2) + x] = texel;
                }
            level = std::move(next);
        }
    }
    return levels;
}
bool TerrainView::update(const Camera& camera, float half_width, float half_height, bool edges) {
    window_dirty.fill(false);
    if (!lod)
        return false;
    // sub_1603B500 per frame: the window update (sub_160465C0) advances the frame counter; when
    // ILANDVIEW::NewMeshNeeded 0x1603b680 asks, the windows are repositioned (update with -1, counting
    // again) and sub_16038BD0 rebuilds the mesh. Map coordinates: X/Y horizontal, Z u
    auto& s = land;
    const float sin_yaw = std::sin(camera.yaw), cos_yaw = std::cos(camera.yaw),
                sin_pitch = std::sin(camera.pitch), cos_pitch = std::cos(camera.pitch);
    const float at[3]{camera.position.x, camera.position.z, camera.position.y},
        forward[3]{sin_yaw * cos_pitch, cos_yaw * cos_pitch, sin_pitch},
        up[3]{-sin_yaw * sin_pitch, -cos_yaw * sin_pitch, cos_pitch};
    const auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    ++s.counter;
    bool rebuild = !s.built || !composed_valid;
    if (!rebuild && s.counter - s.last > 20) {
        // The ramped range is grown back once the decaying measure passes 0.5.
        const float r = s.range < range ? s.range / range : range / s.range;
        s.motion = (1 - r) * (s.motion - (1 - r) + 1);
        rebuild = s.motion > 0.5f;
    }
    if (!rebuild) {
        const float moved[3]{at[0] - s.eye[0], at[1] - s.eye[1], at[2] - s.eye[2]};
        rebuild = dot(moved, moved) >= 100 || dot(up, s.up) * dot(forward, s.forward) < 0.984f;
    }
    if (rebuild) {
        ++s.counter;
        // sub_16038BD0: a rebuild within 20 frames of the last one shrinks the range.
        const int frames = s.counter - s.last;
        s.range = range;
        if (frames < 20 && range <= detail_range) // The Remastered Look keeps its range while moving.
            s.range = static_cast<float>((std::sqrt(frames * 0.050000001) * 0.75 + 0.25) * s.range);
        s.last = s.counter;
        s.motion = 0;
        s.built = true;
        std::copy_n(at, 3, s.eye);
        std::copy_n(forward, 3, s.forward);
        std::copy_n(up, 3, s.up);
        const auto land = lod->classify({{cos_yaw, -sin_yaw, 0},
                                         {forward[0], forward[1], forward[2]},
                                         {up[0], up[1], up[2]},
                                         {at[0], at[1], at[2]},
                                         half_width,
                                         half_height},
                                        s.range);
        for (unsigned i = 0; i < 6; ++i) {
            const auto& w = land.windows[i];
            if (!composed_valid || recompose_[i] || w.x != composed[i].x || w.y != composed[i].y)
                compose_window(i, w);
            composed[i] = w;
        }
        recompose_.fill(false);
        composed_valid = true;
        const unsigned pitch = lod->size + 1;
        lod_faces.clear();
        for (const auto& t : land.triangles) {
            // Our mesh winds faces the other way round from the land view's.
            const std::array<std::uint32_t, 3> order{t.points[0], t.points[2], t.points[1]};
            unsigned cx = pitch, cy = pitch;
            for (const auto point : order) {
                cx = std::min(cx, point % pitch);
                cy = std::min(cy, point / pitch);
            }
            const auto cell = static_cast<std::size_t>(cy) * lod->size + cx;
            for (const auto point : order) {
                auto v = lod_grid[point];
                if (t.layer == TerrainLod::near_tiles) {
                    // 0x1603a070: texels 0.5..63.5 of the cell's tile.
                    constexpr float lo = 0.0078125f, hi = 1 - lo;
                    v.uv = {point % pitch > cx ? hi : lo, point / pitch > cy ? hi : lo};
                    v.texture_layer = cell_tiles[cell] | static_cast<std::uint32_t>(cell_bases[cell]) << 16;
                } else
                    v.texture_layer = 0x80000000u | t.layer;
                lod_faces.push_back(v);
            }
        }
        if (edges)
            for (const auto& [first, second] : land.edges) {
                // sub_1603A3B0: a 50-unit skirt below each unshared edge whose outward horizontal normal
                // (cross(p0 - p1, up)) faces away from the eye, textured with EDGE at texels
                // 1/256..255/256.
                const auto& a = lod_grid[first].position;
                const auto& b = lod_grid[second].position;
                const float nx = a.z - b.z, nz = -(a.x - b.x);
                if (nx * (a.x - camera.position.x) + nz * (a.z - camera.position.z) <= 0)
                    continue;
                const float length = std::sqrt(nx * nx + nz * nz);
                constexpr float lo = 1.0f / 256, hi = 255.0f / 256;
                const auto corner = [&](const Vec3& q, float drop, Vec2 uv, std::uint32_t light) {
                    Vertex v;
                    v.position = {q.x, q.y - drop, q.z};
                    v.normal = {nx / length, 0, nz / length};
                    // 0x1603ac40's first lightmap pass includes batch 6 (EDGE).
                    // World XY is constant down the skirt, so both rows share the endpoint light.
                    v.color = light;
                    v.uv = uv;
                    v.texture_layer = 0x80000006u;
                    return v;
                };
                const auto ca = lod_grid[first].color, edge_b = lod_grid[second].color;
                const auto v0 = corner(a, 0, {lo, lo}, ca), v1 = corner(b, 0, {hi, lo}, edge_b),
                           v2 = corner(a, 50, {lo, hi}, ca), v3 = corner(b, 50, {hi, hi}, edge_b);
                lod_faces.insert(lod_faces.end(), {v0, v1, v2, v1, v3, v2});
            }
    }
    return rebuild;
}
std::vector<MaterialRange> group_model(const ModelAsset& model, Mesh& grouped) {
    if (model.mesh.indices.size() % 3 ||
        model.mesh.triangle_materials.size() != model.mesh.indices.size() / 3 ||
        model.textures.size() != model.mesh.materials.size())
        throw Error("Inconsistent model material arrays");
    grouped = model.mesh;
    grouped.indices.clear();
    std::vector<MaterialRange> result;
    for (std::size_t i = 0; i < model.mesh.materials.size(); ++i) {
        MaterialRange range{model.mesh.materials[i].flags, static_cast<unsigned>(grouped.indices.size()), 0};
        for (std::size_t triangle = 0; triangle < model.mesh.triangle_materials.size(); ++triangle)
            if (model.mesh.triangle_materials[triangle] == i) {
                const auto start = model.mesh.indices.begin() + triangle * 3;
                grouped.indices.insert(grouped.indices.end(), start, start + 3);
            }
        range.count = static_cast<unsigned>(grouped.indices.size()) - range.first;
        result.push_back(range);
    }
    if (grouped.indices.size() != model.mesh.indices.size())
        throw Error("Invalid triangle material index");
    return result;
}
} // namespace renderer_shared
} // namespace sacrifice
