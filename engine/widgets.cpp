#include "sacrifice/widgets.hpp"
#include "sacrifice/scene.hpp"
#include "remaster_tuning.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace sacrifice {
WidgetField::WidgetField(std::shared_ptr<const WidgetModel> model, std::vector<Placement> placements, Vec3 ambient)
    : model_(std::move(model)), placements_(std::move(placements)), ambient_(ambient) {
    if (!model_ || model_->points.empty())
        throw Error("Widget field has no model points");
    auto low = model_->points.front().position, high = low;
    for (const auto& point : model_->points) {
        const auto p = point.position;
        low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    }
    const double x = low.x - high.x, y = low.y - high.y, z = low.z - high.z;
    far_range_ = static_cast<float>(std::clamp(std::sqrt((x * x + y * y + z * z) * .33333334) * 50, 30.0, 200.0));
    low_ = low.z;
    height_ = high.z - low.z;
    near_range_ = std::max(far_range_ * .1f, 5.0f);
    // AddWidget inserts at the head. RecalculateBSP consumes that reversed placement order.
    std::vector<unsigned> entries(placements_.size());
    std::iota(entries.rbegin(), entries.rend(), 0u);
    nodes_.reserve(entries.size());
    root_ = build(std::move(entries), nullptr);
}
int WidgetField::build(std::vector<unsigned> entries, const Vec2* parent) {
    if (entries.empty())
        return -1;
    const auto first = placements_[entries.front()].position;
    float best = 0;
    std::optional<unsigned> other;
    for (std::size_t i = 1; i < entries.size(); ++i) {
        const auto p = placements_[entries[i]].position;
        const double x = first.x - p.x, y = first.y - p.y;
        const double squared = x * x + y * y;
        const double score = parent ? (1 - std::abs(x * parent->x + y * parent->y)) * std::sqrt(squared) : squared;
        if (score > best) {
            best = static_cast<float>(score);
            other = entries[i];
        }
    }
    Node node;
    node.normal = {0, 1};
    node.distance = first.y;
    if (other) {
        const auto p = placements_[*other].position;
        const double x = first.y - p.y, y = p.x - first.x;
        const double inverse = 1 / std::sqrt(x * x + y * y);
        node.normal = {static_cast<float>(x * inverse), static_cast<float>(y * inverse)};
        node.distance = static_cast<float>(node.normal.x * static_cast<double>(first.x) + node.normal.y * static_cast<double>(first.y));
    }
    std::vector<unsigned> positive, negative;
    for (const auto index : entries) {
        const auto p = placements_[index].position;
        const double side = p.x * static_cast<double>(node.normal.x) + p.y * static_cast<double>(node.normal.y) - node.distance;
        if (side < -.01)
            negative.push_back(index);
        else if (side > .01)
            positive.push_back(index);
        else
            node.members.push_back(index);
    }
    // All three native partition lists use head insertion.
    std::ranges::reverse(positive);
    std::ranges::reverse(negative);
    std::ranges::reverse(node.members);
    const auto index = static_cast<int>(nodes_.size());
    nodes_.push_back(std::move(node));
    const auto normal = nodes_[index].normal;
    nodes_[index].positive = build(std::move(positive), &normal);
    nodes_[index].negative = build(std::move(negative), &normal);
    return index;
}
void WidgetField::set_flora(std::vector<FloraModel> models, std::vector<FloraModel> far) {
    flora_ = std::move(models);
    flora_far_ = far.size() == flora_.size() ? std::move(far) : std::vector<FloraModel>{};
    flora_points_ = flora_triangles_ = 0;
    for (const auto& model : flora_) {
        flora_points_ = std::max(flora_points_, model.points.size());
        flora_triangles_ = std::max(flora_triangles_, model.triangles.size());
    }
}
std::size_t WidgetField::vertex_capacity(unsigned copies, bool flora) const {
    return placements_.size() * (1 + copies) * std::max(model_->points.size(), flora ? flora_points_ : 0);
}
std::size_t WidgetField::index_capacity(unsigned copies, bool flora) const {
    return placements_.size() * (1 + copies) * std::max(model_->triangles.size(), flora ? flora_triangles_ : 0) * 3;
}
FloraMesh WidgetField::flora_mesh() const {
    FloraMesh mesh;
    auto all = flora_;
    all.insert(all.end(), flora_far_.begin(), flora_far_.end());
    for (const auto& model : all) {
        mesh.models.push_back({static_cast<unsigned>(mesh.indices.size()), static_cast<unsigned>(model.triangles.size() * 3),
                               static_cast<int>(mesh.vertices.size())});
        for (const auto& point : model.points) {
            Vertex vertex{point.position, point.normal, {}, point.color};
            vertex.texture_layer = model.rigid ? 1 : 0;
            mesh.vertices.push_back(vertex);
        }
        for (const auto& triangle : model.triangles)
            mesh.indices.insert(mesh.indices.end(), {triangle[0], triangle[1], triangle[2]});
    }
    return mesh;
}
float WidgetField::range(float quality) const {
    return static_cast<float>(std::sqrt(std::max(quality, 0.0f)) * (far_range_ - near_range_) + near_range_);
}
std::uint32_t widget_colour(Vec3 point, Vec3 eye, Vec3 ambient, std::span<const PointLight> lights) {
    Vec3 colour{std::clamp(ambient.x, 0.0f, 1.0f), std::clamp(ambient.y, 0.0f, 1.0f),
                std::clamp(ambient.z, 0.0f, 1.0f)};
    for (const auto& light : lights) {
        if (!light.enabled || light.outer <= 0 || light.intensity <= 0)
            continue;
        const double ex = eye.x - light.position.x, ey = eye.y - light.position.y, ez = eye.z - light.position.z;
        const double outer2 = static_cast<double>(light.outer) * light.outer;
        // PointToPoint selects/fades lights against a sphere of radius 5 * outer around the eye.
        const float selected = static_cast<float>(std::min(1.0, 2 - (ex * ex + ey * ey + ez * ez) / (outer2 * 25)));
        if (selected < 0)
            continue;
        const double x = point.x - light.position.x, y = point.y - light.position.y, z = point.z - light.position.z;
        const float squared = static_cast<float>(x * x + y * y + z * z), inner2 = light.inner * light.inner;
        if (squared > outer2)
            continue;
        // Preserve LightPoint's native arithmetic: full inside the inner radius; outside, its
        // factor is (d^2-inner^2)/(outer^2-inner^2). This differs from the D3D7 normal-lit path.
        const double weight = selected * light.intensity * (squared < inner2 ? 1.0 :
            outer2 > inner2 ? (squared - inner2) / (outer2 - inner2) : 1.0);
        colour.x = static_cast<float>(colour.x + light.color.x * weight);
        colour.y = static_cast<float>(colour.y + light.color.y * weight);
        colour.z = static_cast<float>(colour.z + light.color.z * weight);
    }
    const auto channel = [](float value) {
        return static_cast<unsigned>(std::nearbyint(std::clamp(value, 0.0f, 1.0f) * 255));
    };
    return 0xff000000u | channel(colour.x) | channel(colour.y) << 8 | channel(colour.z) << 16;
}
WidgetField::Frame WidgetField::draw(const Terrain& terrain, Vec3 eye, Vec3 forward, float quality,
                                    std::span<const PointLight> lights, const Extras* extras) const {
    Frame frame;
    const bool steady = extras && extras->replace && !flora_.empty();
    const float distance = (steady ? std::max(far_range_, extras->min_range) : range(quality)) *
                           (extras ? extras->range_scale : 1.0f),
                outer2 = distance * distance,
                inner2 = static_cast<float>(outer2 * .64);
    const float fade = 256 / (outer2 - inner2);
    const bool foliage = extras && extras->foliage;
    const bool flora = extras && extras->replace && !flora_.empty();
    frame.flora = flora;
    // Remastered: no plants around fountains and altars, or on scar decals (burns, paths), though the map
    // placed some there.
    const auto on_scar = [&](float x, float y) {
        if (!extras || !extras->replace)
            return false;
        for (const auto& clearing : clearings_)
            if ((x - clearing.x) * (x - clearing.x) + (y - clearing.y) * (y - clearing.y) < clearing.z * clearing.z)
                return true;
        if (terrain.scar_layers.empty() || x < 0 || y < 0)
            return false;
        const auto cx = static_cast<std::size_t>(x / 10), cy = static_cast<std::size_t>(y / 10);
        return cx < terrain.width && cy < terrain.height && terrain.scar_layers[cy * terrain.width + cx] != 0;
    };
    const unsigned copies = extras && height_ <= extras->copy_max_height ? extras->copies : 0;
    std::vector<std::uint32_t> fading;
    const bool instanced = flora && extras->instanced;
    std::vector<std::vector<FloraInstance>> near_instances, fading_instances;
    if (instanced) {
        near_instances.resize(flora_.size() + flora_far_.size());
        fading_instances.resize(flora_.size() + flora_far_.size());
    }
    // Distance thinning for the new plants: past flora_thin_start of the range each plant shrinks away as its
    // rank passes the thinning line (no popping). Returns the size left (0: not drawn) and the line.
    struct Thin { float size, keep; };
    const auto thinning = [&](float squared, std::uint32_t seed) {
        const float d = std::sqrt(squared) / distance, start = tuning::flora_thin_start;
        const float rank = ((seed * 0x85ebca6bu) >> 8 & 0xffff) / 65535.0f;
        const float keep = d < start ? 1.1f : 1.1f - (d - start) / (1 - start) * (1.1f - tuning::flora_far_keep);
        return Thin{std::clamp((keep - rank) * 8, 0.0f, 1.0f), keep};
    };
    // One plant at `p` (Z sampled from LAND by the caller); `seed` picks its model, size and thinning rank,
    // and its top byte (`hash`) varies the foliage shading and wind.
    const auto plant = [&](Vec3 p, float angle, float scale, float squared, std::uint32_t seed) {
        const unsigned hash = seed >> 24;
        auto colour = widget_colour(p, eye, ambient_, lights);
        if (squared > inner2)
            colour = (colour & 0xffffff) | static_cast<unsigned>(std::clamp((outer2 - squared) * fade, 0.0f, 255.0f)) << 24;
        const float c = std::cos(angle), s = std::sin(angle);
        const auto base = static_cast<std::uint32_t>(frame.vertices.size());
        if (flora) {
            // A generated model, its albedo lit by the anchor colour (alpha keeps the fade).
            const auto index = hash % flora_.size();
            const auto& model = flora_[index];
            float size = scale * (.85f + .3f * ((hash >> 3) & 31) / 31.0f);
            // Fewer plants far away, and the ones left grow a little to keep the ground covered.
            const auto [thin, keep] = thinning(squared, seed);
            if (thin <= 0)
                return;
            size *= thin * (1 + tuning::foliage_fill * (1 - std::min(keep, 1.0f)));
            const float d = std::sqrt(squared) / distance;
            if (instanced) {
                auto& groups = squared > inner2 ? fading_instances : near_instances;
                // Low detail past flora_lod_start, dissolving across the band: both models, each with its
                // share in alpha (only outside the fade band, whose alpha is the range fade).
                const float u = flora_far_.empty() ? 0.0f
                    : std::clamp((d - tuning::flora_lod_start) / tuning::flora_lod_band, 0.0f, 1.0f);
                const auto with_alpha = [&](float a) {
                    return squared > inner2 ? colour
                                            : (colour & 0xffffff) | static_cast<std::uint32_t>(a * 255 + .5f) << 24;
                };
                if (u < 1) {
                    groups[index].push_back({{p.x, p.z, p.y}, size, c, s, with_alpha(1 - u), hash});
                    frame.triangles += model.triangles.size();
                }
                if (u > 0) {
                    groups[flora_.size() + index].push_back({{p.x, p.z, p.y}, size, c, s, with_alpha(u), hash});
                    frame.triangles += flora_far_[index].triangles.size();
                }
                return;
            }
            frame.triangles += model.triangles.size();
            for (const auto& point : model.points) {
                const auto v = point.position, n = point.normal;
                const auto channel = [&](int shift) {
                    return ((colour >> shift & 255) * (point.color >> shift & 255) + 127) / 255 << shift;
                };
                Vertex vertex{{(v.x * c + v.y * s) * size + p.x, v.z * size + p.z, p.y + (v.y * c - v.x * s) * size},
                              {n.x * c + n.y * s, n.z, n.y * c - n.x * s}, {},
                              (colour & 0xff000000u) | channel(0) | channel(8) | channel(16)};
                vertex.texture_layer = model.rigid ? foliage_layer(0, hash, true) : foliage_layer(v.z * size, hash, false);
                frame.vertices.push_back(vertex);
            }
            auto& indices = squared > inner2 ? fading : frame.indices;
            for (const auto& triangle : model.triangles)
                indices.insert(indices.end(), {base + triangle[0], base + triangle[1], base + triangle[2]});
            return;
        }
        for (const auto& point : model_->points) {
            const auto v = point.position;
            // The native arithmetic order, so unscaled plants match the original exactly.
            Vertex vertex{{v.x * c * scale + v.y * s * scale + p.x, v.z * scale + p.z,
                           p.y - v.x * s * scale + v.y * c * scale},
                          {0, 1, 0}, {point.uv.x, 1 - point.uv.y}, colour};
            if (foliage)
                vertex.texture_layer = foliage_layer((v.z - low_) * scale, hash, false);
            frame.vertices.push_back(vertex);
        }
        frame.triangles += model_->triangles.size();
        auto& indices = squared > inner2 ? fading : frame.indices;
        for (const auto& triangle : model_->triangles)
            indices.insert(indices.end(), {base + triangle[0], base + triangle[1], base + triangle[2]});
    };
    // Inside the view's side planes, give or take `margin`.
    const bool cull = extras && extras->cull;
    const auto in_view = [&](Vec3 p, float margin) {
        for (const auto& plane : extras->frustum)
            if (plane[0] * p.x + plane[1] * p.y + plane[2] * p.z + plane[3] < -margin)
                return false;
        return true;
    };
    // A plant's reach from its anchor: its height, grown by the copies' scale and the thinning fill.
    const float margin = 1 + height_ * 2;
    const auto emit = [&](const Node& node) {
        for (const auto index : node.members) {
            const auto& placement = placements_[index];
            Vec3 p = placement.position;
            const double x = p.x - eye.x, y = p.y - eye.y;
            const float squared = static_cast<float>(x * x + y * y);
            if (squared > outer2)
                continue;
            // Thinned-out plants are skipped before their ground height and light are looked up.
            const std::uint32_t seed = index * 2654435761u;
            const bool drawn = !flora || thinning(squared, seed).size > 0;
            if (!drawn && !copies)
                continue;
            // DrawWidgetList samples LAND each frame, including holes and edited heights.
            p.z = terrain_height(terrain, p.x, p.y);
            if (cull ? !in_view(p, margin + (copies ? extras->copy_spread : 0))
                     : x * forward.x + y * forward.y + (p.z - eye.z) * forward.z <= 0)
                continue;
            if (drawn && (!cull || in_view(p, margin)) && !on_scar(p.x, p.y))
                plant(p, placement.angle, 1, squared, seed);
            // Remastered drawing-only copies of small plants.
            for (unsigned copy = 0; copy < copies; ++copy) {
                auto hash = index * 0x9e3779b9u ^ (copy + 1) * 0x85ebca6bu;
                const auto next = [&] {
                    hash ^= hash >> 15;
                    hash *= 0x2c1b3c6du;
                    hash ^= hash >> 12;
                    return (hash & 0xffff) / 65535.0f;
                };
                // Not thinned by distance: a copy that comes and goes as the camera moves reads as pop-in.
                // The range's fade band hides it at the edge like any plant.
                const float a = next() * 6.2831853f, r = (.35f + .65f * next()) * extras->copy_spread;
                Vec3 q{p.x + std::cos(a) * r, p.y + std::sin(a) * r, 0};
                const double qx = q.x - eye.x, qy = q.y - eye.y;
                const auto q2 = static_cast<float>(qx * qx + qy * qy);
                const float turn = next() * 6.2831853f, scale = .75f + .5f * next();
                if (q2 > outer2 || (flora && thinning(q2, hash).size <= 0) || on_scar(q.x, q.y))
                    continue;
                q.z = terrain_height(terrain, q.x, q.y);
                if (cull && !in_view(q, margin))
                    continue;
                plant(q, turn, scale, q2, hash);
            }
        }
    };
    // The native BSP visits the far child, its coplanar list, then the near child. It skips
    // distant half-spaces using the same (possibly > base range) distance as anchor culling.
    struct Work { int node; bool list; };
    std::vector<Work> work;
    if (root_ >= 0)
        work.push_back({root_, false});
    while (!work.empty()) {
        const auto item = work.back();
        work.pop_back();
        const auto& node = nodes_[item.node];
        if (item.list) {
            emit(node);
            continue;
        }
        const double side = eye.x * static_cast<double>(node.normal.x) + eye.y * static_cast<double>(node.normal.y) - node.distance;
        const bool positive = side >= 0;
        const int near_child = positive ? node.positive : node.negative, far_child = positive ? node.negative : node.positive;
        if (near_child >= 0)
            work.push_back({near_child, false});
        if (positive ? side < distance : side > -distance) {
            work.push_back({item.node, true});
            if (far_child >= 0)
                work.push_back({far_child, false});
        }
    }
    frame.near_indices = static_cast<unsigned>(frame.indices.size());
    frame.indices.insert(frame.indices.end(), fading.begin(), fading.end());
    for (std::size_t m = 0; m < near_instances.size(); ++m) {
        frame.models.push_back({static_cast<unsigned>(frame.instances.size()),
                                static_cast<unsigned>(near_instances[m].size()),
                                static_cast<unsigned>(fading_instances[m].size())});
        frame.instances.insert(frame.instances.end(), near_instances[m].begin(), near_instances[m].end());
        frame.instances.insert(frame.instances.end(), fading_instances[m].begin(), fading_instances[m].end());
    }
    return frame;
}
} // namespace sacrifice
