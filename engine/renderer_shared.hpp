#pragma once
#include "remaster_tuning.hpp"
#include "sacrifice/renderer.hpp"
#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <utility>
namespace sacrifice::renderer_shared {
struct ParticleRange {
    const ParticleBatch* batch;
    unsigned first, count;
    float depth{};
    bool sorted{};
};
struct ParticleGeometry {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<ParticleRange> ranges;
};
ParticleGeometry expand_particles(std::span<const ParticleBatch> batches, const Camera& camera, Vec3 right,
                                  Vec3 up, Vec3 forward, float near_plane);
constexpr float detail_range = 88.0f;
// Remastered sun shadows. Casters are opaque or alpha-blended (alpha-tested in the shadow pass) materials
// that write depth; additive glows, overlays and depth-ignoring primitives cast none.
inline bool casts_shadow(std::uint32_t flags) {
    const auto mode = flags & 0x7f00;
    return (mode == 0 || mode == 0x1000) && !(flags & 0x06000000);
}
constexpr float shadow_depth = 1500; // Light-space depth reach either side of the focus.
// Remastered bump detail light: the sun's compass direction at the original's fixed 45 degree rise
// (EnableBumpMapping's 0.7071 horizontal, blue 218), so flat ground darkens as much as in the classic look
// and only the relief's lit side follows the sun. `sun` points towards the sun (renderer space). Lit from
// the side rather than from behind the camera, the relief reads far stronger, so bump_relief softens it.
inline DirectX::XMFLOAT4 sun_bump(DirectX::XMFLOAT4 sun, float enabled) {
    float x = sun.x, y = sun.z;
    if (const float length = std::sqrt(x * x + y * y); length > 1e-4f) {
        x *= 0.70709997f * tuning::bump_relief / length;
        y *= 0.70709997f * tuning::bump_relief / length;
    } else
        x = y = 0;
    const auto byte = [](float v) { return static_cast<float>(static_cast<int>(v * 127.5f + 128.0f)) / 255; };
    return {byte(x), byte(y), 218.0f / 255, enabled};
}
// The sun's view: an orthographic square of 2 x shadow_range around a ground point ahead of the camera,
// snapped to whole shadow-map texels so the shadow edges do not crawl as the camera moves. `sun` points
// towards the sun (renderer space); `ground` is the terrain height under the camera.
inline DirectX::XMMATRIX sun_shadow_matrix(DirectX::XMFLOAT4 sun, const Camera& camera, float ground) {
    using namespace DirectX;
    const auto towards =
        XMVector3Normalize(XMVectorSet(sun.x, std::max(sun.y, tuning::shadow_min_elevation), sun.z, 0));
    const auto up = std::abs(XMVectorGetY(towards)) > .99f ? XMVectorSet(0, 0, 1, 0) : XMVectorSet(0, 1, 0, 0);
    const auto view = XMMatrixLookToLH(XMVectorZero(), XMVectorNegate(towards), up);
    const float range = tuning::shadow_range, ahead = range * .5f;
    const auto focus = XMVectorSet(camera.position.x + std::sin(camera.yaw) * ahead, ground,
                                   camera.position.z + std::cos(camera.yaw) * ahead, 1);
    XMFLOAT3 f{};
    XMStoreFloat3(&f, XMVector3TransformCoord(focus, view));
    const float texel = 2 * range / tuning::shadow_map_size;
    f.x = std::floor(f.x / texel) * texel;
    f.y = std::floor(f.y / texel) * texel;
    return view * XMMatrixOrthographicOffCenterLH(f.x - range, f.x + range, f.y - range, f.y + range,
                                                  f.z - shadow_depth, f.z + shadow_depth);
}
// The shaders' shadow vector: enabled, sampling spread (texture units), depth bias, model strength.
inline DirectX::XMFLOAT4 shadow_constants() {
    return {1, tuning::shadow_softness / tuning::shadow_map_size, tuning::shadow_bias / (2 * shadow_depth),
            tuning::shadow_strength};
}
// Remastered plant wind: time in seconds (match ticks, so it stops with the game), strength (calm wind plus
// the storm) and a fixed direction in renderer X/Z.
inline DirectX::XMFLOAT4 wind_constants(const WeatherLook& weather) {
    const float seconds = weather.tick == UINT64_MAX ? 0.0f : static_cast<float>(weather.tick % 216000) / 60.0f;
    return {seconds, tuning::wind_strength + tuning::wind_storm * weather.storm, .8f, .6f};
}
// Remastered soft particles: blended camera-facing sprites fade where they meet the surface behind them.
// Opaque, depth-ignoring, ground-aligned (generator-aligned) and mesh primitives (rain, splashes, rings)
// keep their hard edges.
inline bool soft_particles(const ParticleBatch& batch, std::uint32_t flags) {
    if ((flags & 0x7f00) == 0 || (flags & 0x04000000) || !batch.mesh.vertices.empty())
        return false;
    for (const auto& quad : batch.quads)
        if (quad.generator_aligned)
            return false;
    return true;
}
// Remastered Look fog constants: enabled, sky layer (0), and the mist floor and falloff as fractions of the
// map's height range (remaster_tuning.hpp).
struct RemasterFog {
    float enabled, sky, floor, falloff;
};
inline RemasterFog remaster_constants(const Terrain* terrain) {
    if (!terrain || !(terrain->maximum > terrain->minimum))
        return {1, 0, 0, 40};
    const float span = terrain->maximum - terrain->minimum;
    const float falloff = span * tuning::mist_falloff;
    return {1, 0, terrain->minimum + span * tuning::mist_floor, falloff > 10 ? falloff : 10.0f};
}
// Effect materials keep the native word. Blend mode 0x800 (the 0x1a010808 / 0x12010808 streamers, glows and
// haze) has no equivalent here: D3D11 would draw it opaque, black around the texture, and WebGPU rejects it.
// Effects draw it additively instead. The native decoder (3d_d3d:0x1800a500) makes 0x800 a multiply (ZERO,
// SRCCOLOR) with texture-only colour, but its textures (rift, rif3) are opaque and mostly black, so a literal
// multiply draws dark squares; additive was kept deliberately (2026-11-11).
constexpr std::uint32_t effect_flags(std::uint32_t flags) {
    return (flags & 0x7f00) == 0x800 ? (flags & ~0x7f00u) | 0x2000 : flags;
}
struct MaterialRange {
    std::uint32_t flags{};
    unsigned first{}, count{};
};
std::vector<MaterialRange> group_model(const ModelAsset& model, Mesh& grouped);
std::vector<std::vector<std::uint32_t>> detail_mips(const TerrainMaterials& materials);
// Backends upload changed window_pixels and lod_faces after update().
struct TerrainView {
    // Land view LOD (TerrainLod): per-layer tile mips 32..1 texels for composing the texture windows, the
    // per-cell tile layer, the six composites, and the per-cell draw layer.
    std::vector<std::array<std::vector<std::uint32_t>, 6>> tile_mips;
    std::optional<TerrainLod> lod;
    std::vector<std::uint16_t> cell_tiles;
    std::array<TerrainLod::Window, 6> composed{};
    bool composed_valid{};
    // Remastered Look: full-detail range (detail_range in the original), held steady while the camera moves.
    float range{detail_range};
    // Land view mesh: per grid point attributes (size + 1 across), base tiles for the detail pass, and the
    // per-frame face vertices.
    std::vector<Vertex> lod_grid;
    std::vector<std::uint8_t> cell_bases;
    std::vector<Vertex> lod_faces;
    // ILANDVIEW rebuild state (constructor 0x16043250): frame counter (view+88), last build (+28, -20 so the
    // first build gets the full range), the decaying range measure (+32), the ramped range (+36), and the
    // camera saved at the last build (+40).
    struct LandState {
        int counter{}, last{-20};
        float motion{1e6f}, range{64};
        bool built{};
        float eye[3]{}, forward[3]{}, up[3]{};
    } land;
    std::array<std::vector<std::uint32_t>, 6> window_pixels;
    std::array<bool, 6> window_dirty{};
    void reset(const Terrain& terrain);
    // Copies the tile layers' mips. `full` rebuilds every layer and drops all cached grass and windows (a new
    // tile set); otherwise only the scar layers (256 and up) whose mips changed are copied, and only the
    // cells whose layer changed, or whose scar layer was redrawn, lose their grass and window texels.
    // Returns the layers it rewrote, for the backend's texture array.
    std::vector<std::size_t> update_tiles(const TerrainMaterials& materials, const Terrain& terrain,
                                          bool full = true);
    // After the match edits the land (heights, holes, tiles; Match::terrain_revision): re-reads it and redoes
    // only what changed points touch (the LOD, those grid vertices, nearby grass cells, the land faces).
    // False when there is no LOD or the map size changed; reset() is needed then.
    bool refresh(const Terrain& terrain);
    // Static grass chunk keys (cy * 65536 + cx) whose cells changed; backends caching chunks drop these.
    std::vector<std::int64_t> take_dirty_grass_chunks() { return std::exchange(dirty_grass_chunks_, {}); }
    // Remastered grass layer (drawing only): blades on grassy ground within tuning::grass_range of the camera,
    // as triangles (foliage vertices with the ground's baked light). Empty without a land view.
    void grass(const Terrain& terrain, const Camera& camera, std::vector<Vertex>& vertices,
               std::vector<std::uint32_t>& indices);
    // Static grass (desktop): one chunk of grass_chunk_cells x grass_chunk_cells map cells, every tuft and
    // clutter item at full size. Each vertex carries its root in `normal` and its thinning rank (0..1) in
    // uv.x, and the vertex shader thins and shrinks it with distance as grass() does on the CPU.
    // Tufts are sorted by rank, and `rank_indices[k]` counts the indices of those ranked below k / 16, so a
    // far chunk draws only the prefix that can still show (grass_keep).
    static constexpr int grass_chunk_cells = 4;
    void grass_chunk(const Terrain& terrain, int chunk_x, int chunk_y, std::vector<Vertex>& vertices,
                     std::vector<std::uint32_t>& indices, std::array<unsigned, 17>& rank_indices);
    // The thinning line at a distance (a fraction of the grass reach): tufts ranked below it show. The same
    // curve as the plants' (flora_thin_start, flora_far_keep); grass_vertex matches it.
    static float grass_keep(float t) {
        const float start = tuning::flora_thin_start;
        return t < start ? 1.1f : 1.1f - (t - start) / (1 - start) * (1.1f - tuning::flora_far_keep);
    }
    bool grass_ready() const { return lod && !lod_grid.empty() && !cell_tiles.empty() && tuning::grass_density > 0; }
    int grass_size() const { return lod ? static_cast<int>(lod->size) : 0; } // Map cells across.
    // Bumped whenever cached grass is invalid (terrain, tiles or clearings changed).
    unsigned grass_generation() const { return grass_generation_; }
    // Map X, Y and radius kept bare of grass and clutter (MapScene::clearings); kept across terrain resets.
    // The scene is re-sent every frame, so only a real change drops the cached grass.
    void set_clearings(std::vector<Vec3> clearings) {
        if (std::ranges::equal(clearings, clearings_, [](Vec3 a, Vec3 b) {
                return a.x == b.x && a.y == b.y && a.z == b.z;
            }))
            return;
        clearings_ = std::move(clearings);
        grass_cells_.clear();
        ++grass_generation_;
    }
    bool update(const Camera& camera, float half_width, float half_height, bool edges);

  private:
    void compose_window(unsigned slot, const TerrainLod::Window& window);
    struct Blade {
        float x, y, z, height, width, angle, lean_x, lean_z;
        std::uint32_t root, tip;
        std::uint8_t hash, rank;
        std::uint8_t clutter; // 0: a grass tuft; else 1 + the clutter model (flora.hpp), `height` its scale.
    };
    std::unordered_map<std::int64_t, std::vector<Blade>> grass_cells_; // Per map cell, built on demand.
    std::vector<float> tile_grass_;                                    // Per tile layer: 0 bare .. 1 lush.
    std::vector<Vec3> tile_colour_;                                    // Per tile layer: average colour.
    std::vector<Vec3> clearings_;
    float clearing(float x, float y) const; // 0 inside a clearing, rising to 1 just outside.
    unsigned grass_generation_{};
    std::vector<std::int64_t> dirty_grass_chunks_;
    std::array<bool, 6> recompose_{};                  // Windows to compose again at the next rebuild.
    std::vector<std::vector<std::uint32_t>> scar_top_; // Scar layers' full-size mip, to spot redrawn scars.
    std::vector<std::uint16_t> land_heights_;          // Terrain heights and light the view was built from.
    std::vector<std::uint32_t> land_light_;
    // Drops the grass of map cells [x0, x1] x [y0, y1] and rebuilds the faces over them; `layers` (their
    // tiles changed) also composes the texture windows over them again.
    void invalidate_cells(int x0, int y0, int x1, int y1, bool layers);
    // Re-reads cell layers from `terrain`; `redrawn` marks scar layers whose texels changed. Returns the
    // changed cells' bounds (x0, y0, x1, y1), x1 < x0 when none changed.
    std::array<int, 4> refresh_cells(const Terrain& terrain, const std::vector<bool>& redrawn);
    const std::vector<Blade>& grass_cell(const Terrain& terrain, int cx, int cy);
    void prepare_grass(); // Classifies the tile layers (tile_grass_, tile_colour_) once per tile set.
    // One tuft or clutter item scaled by `grow`; `encode` stores the root and rank for the shader.
    static void emit_blade(const Blade& blade, float grow, bool encode, std::vector<Vertex>& vertices,
                           std::vector<std::uint32_t>& indices);
};
} // namespace sacrifice::renderer_shared
