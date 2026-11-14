#pragma once
#include "assets.hpp"
#include "flora.hpp"
#include "lighting.hpp"
#include <array>
#include <memory>

namespace sacrifice {
struct WidgetModel;
// Remastered foliage vertices (drawing only): texture_layer 0x40000000 marks a plant vertex, with its height
// above the plant's root in 1/16 units (bits 8..19), a per-plant hash (bits 0..7), and 0x20000000 when the
// colour already holds the ground's baked light (the grass layer) rather than the widget's anchor light.
constexpr std::uint32_t foliage_layer(float height, unsigned hash, bool baked) {
    const auto h = static_cast<std::uint32_t>(height <= 0 ? 0 : height >= 255.9f ? 4095 : height * 16);
    return 0x40000000u | (baked ? 0x20000000u : 0u) | h << 8 | (hash & 0xffu);
}
// Remastered instanced flora (drawing only): one plant, placed in renderer space (X, height, map Y), with
// its turn, size, anchor light (alpha is the range fade) and hash. The flora vertex shader expands it.
struct FloraInstance {
    Vec3 position;
    float size{};
    float cos{1}, sin{};
    std::uint32_t colour{}, hash{};
};
static_assert(sizeof(FloraInstance) == 32);
// A widget field's flora models in one static mesh, uploaded once. Vertices are model space with the
// point's albedo; texture_layer is 1 for rigid models (clutter) and 0 for swaying ones.
struct FloraMesh {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    struct Range { unsigned first_index{}, index_count{}; int base_vertex{}; };
    std::vector<Range> models;
};
// WIDGET's anchor list/BSP, independent of the terrain and renderer lifetimes.
class WidgetField {
  public:
    struct Placement { Vec3 position; float angle{}; };
    struct Frame {
        std::vector<Vertex> vertices;
        std::vector<std::uint32_t> indices;
        unsigned near_indices{};
        bool flora{}; // Untextured flora models: draw without the widget texture.
        // Instanced flora (Extras::instanced): instances grouped by model, each group's near plants first,
        // then those in the fade band. `models[m]` is the group of FloraMesh::models[m] (the near models,
        // then their low-detail levels). In the detail switch band a plant is drawn with both models, each
        // with a dissolve amount in its colour's alpha (the near pass dithers it out).
        struct Group { unsigned first{}, near_count{}, fading_count{}; };
        std::vector<FloraInstance> instances;
        std::vector<Group> models;
        std::size_t triangles{}; // Triangles drawn, either way.
    };
    // Remastered Look extras (drawing only): a longer range, drawing-only copies of small plants at hashed
    // offsets around each anchor, and foliage vertex data for the plant shader.
    struct Extras {
        float range_scale{1};
        unsigned copies{};
        float copy_max_height{}, copy_spread{};
        bool foliage{};
        bool replace{}; // Draw the generated flora models (set_flora) instead of the original model.
        // With `replace`: draw out to at least this (times range_scale), whatever the view detail; the
        // original ranges came from each widget's own size and shrink to ~5 units at low detail.
        float min_range{};
        // With `replace`: emit FloraInstance records (Frame::instances) instead of expanded vertices.
        bool instanced{};
        // Frustum culling: with `cull`, the view's side planes in map space (X, Y, Z up) as a, b, c, d with
        // a unit normal pointing inwards. Plants wholly outside one (with a margin for their size) are
        // skipped; without it only plants behind the camera are.
        bool cull{};
        std::array<std::array<float, 4>, 4> frustum{};
    };
    WidgetField(std::shared_ptr<const WidgetModel> model, std::vector<Placement> placements, Vec3 ambient);
    float range(float view_quality) const; // WIDGET::IsInView copies GFXDETAILINFO.minimum without clamping.
    Frame draw(const Terrain& terrain, Vec3 eye, Vec3 forward, float view_quality,
               std::span<const PointLight> lights = {}, const Extras* extras = nullptr) const;
    float height() const { return height_; } // Model height (its Z extent).
    // Remastered Look replacement models (FloraModel, flora.hpp), one picked per anchor by its hash.
    // `far`: the same models' low-detail level (make_flora's `far`), drawn past flora_lod_start; may be empty.
    void set_flora(std::vector<FloraModel> models, std::vector<FloraModel> far = {});
    bool has_flora() const { return !flora_.empty(); }
    FloraMesh flora_mesh() const; // The flora models for instanced drawing.
    // Remastered: map X, Y and radius where replaced plants are not drawn (MapScene::clearings).
    void set_clearings(std::vector<Vec3> clearings) { clearings_ = std::move(clearings); }
    // Most vertices and indices one frame can emit, with `copies` drawing-only copies per small plant.
    // `flora` false when the flora models are drawn instanced (no expanded flora vertices).
    std::size_t vertex_capacity(unsigned copies, bool flora = true) const;
    std::size_t index_capacity(unsigned copies, bool flora = true) const;
    std::size_t instance_capacity(unsigned copies) const { return placements_.size() * (1 + copies) * 2; }
    std::size_t size() const { return placements_.size(); }
    const WidgetModel& model() const { return *model_; }

  private:
    struct Node {
        Vec2 normal;
        float distance{};
        int positive{-1}, negative{-1};
        std::vector<unsigned> members;
    };
    std::shared_ptr<const WidgetModel> model_;
    std::vector<Placement> placements_;
    std::vector<Node> nodes_;
    Vec3 ambient_;
    float near_range_{}, far_range_{};
    float low_{}, height_{};
    std::vector<FloraModel> flora_, flora_far_;
    std::vector<Vec3> clearings_;
    std::size_t flora_points_{}, flora_triangles_{};
    int root_{-1};
    int build(std::vector<unsigned> entries, const Vec2* parent);
};
// _3dngn::SelectWindowLights(0x10001) / LightPoint: one packed colour per anchor, without normals.
std::uint32_t widget_colour(Vec3 point, Vec3 eye, Vec3 ambient, std::span<const PointLight> lights);
} // namespace sacrifice
