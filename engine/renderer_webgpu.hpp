#pragma once
#include "sacrifice/player.hpp"
#include "sacrifice/renderer.hpp"
#include "webgpu_context.hpp"

namespace sacrifice {
class SceneMatch;
struct SkirmishSlot;
struct Character;
// Map-view backend while the full gameplay frontend is being ported.
class WebSceneRenderer {
  public:
    explicit WebSceneRenderer(WebGpuContext& gpu);
    ~WebSceneRenderer();
    void load(const ResourceCatalog& catalog, const Archive& map, std::span<const SkirmishSlot> slots = {},
              bool campaign = false, std::optional<std::uint32_t> seed = {}, const Character* character = nullptr);
    void advance(float seconds, bool single_step = false);
    void options(const GameOptions& options, float fps);
    SceneMatch* simulation();
    void prepare(const Camera& camera, unsigned width, unsigned height);
    void render(WGPUCommandEncoder encoder, WGPUTextureView target);
    const Terrain& terrain() const;
    const TerrainMaterials& materials() const;
    void order_markers(std::span<const PlayerControl::Marker> markers);
    Camera initial_camera() const;
    std::size_t model_count() const;
    std::size_t triangle_count() const;
    std::size_t unresolved_count() const;
    std::size_t particle_count() const;
    std::size_t animated_count() const;
    std::uint32_t ticks() const;
    bool paused{}, effects{true};
    bool models{true}, widgets{true}, environment{true};
    int terrain_detail{2}; // 0 off, 1 detail textures, 2 bump detail maps.

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sacrifice
