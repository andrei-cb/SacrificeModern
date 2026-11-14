#pragma once
#include "assets.hpp"
#include "platform.hpp"
#include "scene.hpp"
#include "lighting.hpp"
#include "options.hpp"
#include <memory>
struct ImDrawList;

namespace sacrifice {
struct Camera {
    Vec3 position{1280, 2600, -1200};
    float yaw{}, pitch{-0.45f}, speed{500};
    Vec2 centre_offset; // Pixels the view centre moves for this frame (camera shake, 0x47fbb0).
    void update(const InputState& input, float dt);
};
// One PARTICLESYSTEM quad in simulation (Z-up) coordinates: half extent `size`,
// colour bytes R, G, B, A from the low byte up, and the texture frame's UV rectangle.
struct ParticleQuad {
    Vec3 center;
    float size{};
    std::uint32_t color{};
    Vec2 uv0, uv1;
    // DrawGenPntAligned (particle flag 0x40000): generator X/Z axes in game space.
    // Other particles continue to use the camera's right/up vectors.
    bool generator_aligned{};
    Vec3 right{}, up{};
};
struct ParticleBatch {
    std::uint32_t flags{}; // PRIMITIVE material word (blend, depth and texture bits as for models).
    void* texture{};       // A view from upload_ui_image, kept alive by the caller.
    std::vector<ParticleQuad> quads;
    Mesh mesh; // World-space effect primitive, already transformed into renderer coordinates.
};
// Remastered Look weather state, for drawing only: how wet the ground is, how much snow has settled, how
// much ash is falling, the lightning flash and how stormy it is (0..1 each). update_weather_look (scene_effects) keeps it.
struct WeatherLook {
    float wet{}, snow{}, ash{}, flash{}, storm{};
    std::uint64_t tick{UINT64_MAX}; // Match tick of the last update.
};
// Frame timing (port diagnostics, not native): per-frame averages over the frames since the last reset.
// CPU figures are milliseconds of the calling thread; GPU figures come from timestamp queries a few frames
// late (desktop only; `gpu` is false where they are unavailable).
struct RenderTimings {
    unsigned frames{};
    float cpu_draw{};    // Renderer::draw: building and submitting the scene.
    float cpu_widgets{}; // Within it: building and uploading the widget fields (the map's plants).
    float cpu_grass{};   // Within it: building and uploading the grass layer and clutter.
    float cpu_end{};     // Renderer::end: post-processing, UI and present (may wait for the GPU).
    bool gpu{};
    float gpu_shadow{}, gpu_land{}, gpu_models{}, gpu_foliage{}, gpu_effects{}, gpu_post{}, gpu_other{}, gpu_total{};
    float widget_vertices{}, widget_triangles{}, grass_vertices{}, grass_triangles{}, casters{};
};
class Renderer {
  public:
    explicit Renderer(NativeWindow window);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    void set_mesh(const Mesh& mesh);
    void set_model(const ModelAsset& model);
    void update_model_mesh(const Mesh& mesh);
    void set_image(const Image& image);
    // Independent UI images. The returned owner keeps the shader view alive; get() is its ImGui handle.
    // Upload once and retain it across frames, without replacing the asset inspector's image.
    std::shared_ptr<void> upload_ui_image(const Image& image);
    // Begin the original HUD's source-alpha/additive mode; finish with ImDrawCallback_ResetRenderState.
    void ui_additive(ImDrawList& list);
    void clear_image();
    void set_terrain_materials(const TerrainMaterials& materials);
    // After scars or SetTile changed the land's tiles: uploads only the scar layers that changed and redraws
    // the cells whose layer changed.
    void update_terrain_tiles(const TerrainMaterials& materials, const Terrain& terrain);
    // Land view LOD for the tiled mesh: call after set_mesh and set_terrain_materials.
    void set_terrain_lod(const Terrain& terrain);
    // After the match edited the land (Match::terrain_revision): refreshes the land view in place, rebuilding
    // only the changed area's vertices and grass; without a land view, rebuilds the tiled mesh instead.
    void update_terrain(const Terrain& terrain);
    // LAND::SetEdgeTexture: the land view skirts' texture; null hides them.
    void set_terrain_edge(const ModelAsset::Texture* texture);
    // Uploads each scene model once and draws its placements after the main mesh.
    void set_scene(const MapScene& scene);
    RenderTimings timings(bool reset = true); // Averages since the last reset.
    void append_scene(const MapScene& scene); // Upload models appended by dynamic match factories.
    unsigned scene_detail_vertices(std::size_t model, std::size_t placement) const;
    void clear_scene();
    // Replaces the vertices of a dynamic scene model (a free soul) after set_scene, in the same grouping.
    void update_scene_model(std::size_t model, const std::vector<Vertex>& vertices);
    // Animated SAXS poses may omit triangles with invalid bones; update their grouped indices too.
    void update_scene_model(std::size_t model, const Mesh& mesh);
    // A character has one placement; copy its full Z-up simulation transform into renderer space.
    void update_scene_transform(std::size_t model, const ObjectMotion& motion);
    void update_scene_transform(std::size_t model, std::size_t placement, const ObjectMotion& motion,
                                float scale = 1);
    void update_scene_visibility(std::size_t model, std::size_t placement, bool visible, float reveal = 1,
                                 float opacity = 1);
    void update_scene_texture(std::size_t model, void* texture, bool grayscale = false);
    // Map ENVI lighting/fog and the LAND sky mesh, applied when draw() is asked for the environment.
    void set_environment(const Environment& environment, const ModelAsset& sky);
    void clear_environment();
    void update_environment(const Environment& environment);
    // Replaces the sky vertices with a later SkyState of the same environment.
    void update_sky(const std::vector<Vertex>& vertices);
    // Replaces the texture of ENVI sky slot `slot` (a Smacker frame of the slot's size).
    void update_sky_texture(unsigned slot, const Image& image);
    // Particle quads and effect meshes drawn after the scene models, replaced every frame.
    void set_particles(std::vector<ParticleBatch> batches);
    // Replaced each frame from live effect hosts; terrain receives LAND's adjusted copies.
    void set_point_lights(std::span<const PointLight> lights, const Terrain& terrain);
    void begin(unsigned width, unsigned height);
    // `terrain_detail`: 0 no land detail pass, 1 detail textures (DT intensity), 2 bump detail maps.
    void set_detail(RenderDetail detail, bool widgets = true, int texture_filter = 1, int terrain_detail = 2);
    [[nodiscard]] const RenderDetail& detail() const;
    // The graphics page's Remastered Look (post_process.hpp); off draws as the original did.
    void set_remastered(bool remastered);
    void set_weather(const WeatherLook& weather);
    void draw(const Camera& camera, bool wireframe = false, bool model_materials = true, bool scene = false,
              bool environment = false);
    void end(bool vsync = true, const std::filesystem::path& capture_path = {});
    void capture(const std::filesystem::path& path);
    // The back buffer as drawn so far (RGBA), e.g. the scene before the interface pass.
    Image frame_image();
    void* image_handle() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sacrifice
