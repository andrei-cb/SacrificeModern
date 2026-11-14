#include "sacrifice/wizard_preview.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/scene.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>

namespace sacrifice {
struct WizardPreview::Impl {
    const ResourceCatalog& catalog;
    struct Model {
        SceneModel source;
        std::shared_ptr<const CreatureClip> clip;
        Vec3 centre;
        float radius{}, half_height{};
    };
    std::map<FourCC, Model> models;
    FourCC selected;
    Image image;
    std::vector<float> depth;
    float elapsed{}, since_frame{}, frame{};
    unsigned triangle_count{};
    std::string failure;
    explicit Impl(const ResourceCatalog& resources) : catalog(resources) {}
    Model& load(FourCC wizard) {
        if (auto it = models.find(wizard); it != models.end())
            return it->second;
        Model model;
        model.source = SceneModel::load_character(catalog, wizard, FourCC::from_text("WIZD"));
        const auto& rules = *model.source.character->rules;
        const auto initial = rules.fsm.resolve(rules.fsm.initial);
        const int slot = initial ? rules.fsm.at(*initial).animation : -1;
        if (slot >= 0)
            model.clip = rules.clips.at(slot);
        // The live creature's stance chooser leaves its initial movement clip via stan/hovr.
        // Resolve that animation without creating a match, casting spells or playing voice cues.
        CreatureFsmRunner stance(rules.fsm);
        stance.animation = [&](int animation, std::uint32_t) {
            const auto& clip = rules.clips.at(animation);
            if (clip && clip->animation.bones == rules.bones)
                model.clip = clip;
        };
        stance.handler = [](CreatureFsmRunner& machine, std::string_view name, int) {
            if (name == "CreatureChooseStance")
                return machine.event(0x7374616e); // stan
            if (name == "CreatureChooseOFlyAnim")
                return machine.event(0x686f7672); // hovr
            return 0;
        };
        stance.start();
        stance.run(0);
        // Some wizards (Abraxus, Buta, Yogo, Seerix) enter their stance through a one- or two-frame pose;
        // the live creature leaves it on clip end (event 2, as advance_animation fires), so do the same.
        for (int step = 0; step < 4 && model.clip && model.clip->animation.frames.size() <= 2; ++step) {
            stance.event(2);
            stance.run(0);
        }
        if (!model.clip || model.clip->animation.bones != rules.bones)
            for (const auto& clip : rules.clips)
                if (clip && clip->animation.bones == rules.bones) {
                    model.clip = clip;
                    break;
                }
        Vec3 low{1e9f, 1e9f, 1e9f}, high{-1e9f, -1e9f, -1e9f};
        for (const auto& v : model.source.asset.mesh.vertices) {
            low = {std::min(low.x, v.position.x), std::min(low.y, v.position.y),
                   std::min(low.z, v.position.z)};
            high = {std::max(high.x, v.position.x), std::max(high.y, v.position.y),
                    std::max(high.z, v.position.z)};
        }
        model.centre = {(low.x + high.x) / 2, (low.y + high.y) / 2, (low.z + high.z) / 2};
        model.half_height = std::max((high.y - low.y) / 2, .1f);
        model.radius = std::max(std::hypot(high.x - low.x, high.z - low.z) / 2, .1f);
        return models.emplace(wizard, std::move(model)).first->second;
    }
    struct Point {
        float x, y, z, u, v, light;
    };
    static float edge(const Point& a, const Point& b, float x, float y) {
        return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
    }
    void render(Model& model) {
        const auto& body = *model.source.character->body;
        const auto pose =
            model.clip ? saxs_pose(body.model, model.clip->animation, frame) : saxs_bind_pose(body.model);
        const auto mesh = saxs_mesh(body.model, body.descriptor, *model.source.character->topology, pose);
        triangle_count = static_cast<unsigned>(mesh.indices.size() / 3);
        const float angle = 3.1415901f + elapsed * .35f, cs = std::cos(angle), sn = std::sin(angle);
        const float focal = image.height * 1.15f;
        const float distance = std::max(model.half_height * focal / (image.height * .43f),
                                        model.radius * focal / (image.width * .43f)) +
                               model.radius;
        std::vector<Point> points;
        points.reserve(mesh.vertices.size());
        for (const auto& v : mesh.vertices) {
            const float x = v.position.x - model.centre.x, z = v.position.z - model.centre.z;
            const float inverse = 1 / (distance - sn * x + cs * z);
            const float nx = cs * v.normal.x + sn * v.normal.z, nz = -sn * v.normal.x + cs * v.normal.z;
            const float light = .6f + .4f * std::max(0.f, -.3f * nx + .6f * v.normal.y - .74f * nz);
            points.push_back({image.width * .5f + (cs * x + sn * z) * focal * inverse,
                              image.height * .5f - (v.position.y - model.centre.y) * focal * inverse, inverse,
                              v.uv.x * inverse, v.uv.y * inverse, light * inverse});
        }
        depth.assign(image.rgba.size(), 0);
        // A gently moving plasma field behind the model, as on the original character page.
        for (unsigned y = 0; y < image.height; ++y)
            for (unsigned x = 0; x < image.width; ++x) {
                const float u = float(x) / image.width, v = float(y) / image.height;
                const float wave = (std::sin(u * 12 + elapsed) + std::sin(v * 15 - elapsed * .7f) +
                                    std::sin((u + v) * 10 + elapsed * .4f) + 3) /
                                   6;
                const unsigned shade = static_cast<unsigned>(12 + wave * 25);
                image.rgba[y * image.width + x] = 0xff000000 | (shade + 12) << 16 | shade << 8 | shade / 2;
            }
        std::vector<unsigned> triangles(triangle_count);
        std::iota(triangles.begin(), triangles.end(), 0);
        // Back-to-front alpha layers; opaque pixels still resolve by depth, independent of mesh order.
        std::ranges::sort(triangles, [&](unsigned a, unsigned b) {
            const auto z = [&](unsigned t) {
                return points[mesh.indices[t * 3]].z + points[mesh.indices[t * 3 + 1]].z +
                       points[mesh.indices[t * 3 + 2]].z;
            };
            return z(a) < z(b);
        });
        for (const unsigned triangle : triangles) {
            const auto &a = points[mesh.indices[triangle * 3]], &b = points[mesh.indices[triangle * 3 + 1]],
                       &c = points[mesh.indices[triangle * 3 + 2]];
            const float area = edge(a, b, c.x, c.y);
            if (!std::isfinite(area) || std::abs(area) < .001f)
                continue;
            const auto part = mesh.triangle_materials[triangle];
            const auto& texture = body.textures.at(part).image;
            if (texture.rgba.empty())
                continue;
            const int left = std::max(0, int(std::floor(std::min({a.x, b.x, c.x})))),
                      right = std::min(int(image.width) - 1, int(std::ceil(std::max({a.x, b.x, c.x})))),
                      top = std::max(0, int(std::floor(std::min({a.y, b.y, c.y})))),
                      bottom = std::min(int(image.height) - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
            for (int y = top; y <= bottom; ++y)
                for (int x = left; x <= right; ++x) {
                    const float wa = edge(b, c, x + .5f, y + .5f) / area,
                                wb = edge(c, a, x + .5f, y + .5f) / area, wc = 1 - wa - wb;
                    if (wa < 0 || wb < 0 || wc < 0)
                        continue;
                    const auto at = y * image.width + x;
                    const float z = wa * a.z + wb * b.z + wc * c.z;
                    if (z < depth[at])
                        continue;
                    const float u = (wa * a.u + wb * b.u + wc * c.u) / z,
                                v = (wa * a.v + wb * b.v + wc * c.v) / z;
                    const unsigned tx = std::min(unsigned((u - std::floor(u)) * texture.width),
                                                 texture.width - 1),
                                   ty = std::min(unsigned((v - std::floor(v)) * texture.height),
                                                 texture.height - 1);
                    const auto texel = texture.rgba[ty * texture.width + tx];
                    const unsigned alpha = texel >> 24;
                    if (alpha < 8)
                        continue;
                    const float light = (wa * a.light + wb * b.light + wc * c.light) / z;
                    auto pixel = 0xff000000u;
                    for (unsigned shift = 0; shift < 24; shift += 8) {
                        const auto colour = std::clamp(int((texel >> shift & 255) * light), 0, 255);
                        pixel |= ((colour * alpha + (image.rgba[at] >> shift & 255) * (255 - alpha)) / 255)
                                 << shift;
                    }
                    image.rgba[at] = pixel;
                    if (alpha >= 128)
                        depth[at] = z;
                }
        }
    }
};
WizardPreview::WizardPreview(const ResourceCatalog& catalog) : impl_(std::make_unique<Impl>(catalog)) {}
WizardPreview::~WizardPreview() = default;
bool WizardPreview::update(FourCC wizard, float seconds, unsigned width, unsigned height) {
    auto& p = *impl_;
    const float size_scale = std::min({1.f, 384.f / std::max(width, 1u), 512.f / std::max(height, 1u)});
    width = std::max(64u, static_cast<unsigned>(width * size_scale));
    height = std::max(64u, static_cast<unsigned>(height * size_scale));
    const bool changed = wizard != p.selected || width != p.image.width || height != p.image.height;
    if (wizard != p.selected) {
        p.selected = wizard;
        p.elapsed = p.frame = p.since_frame = 0;
        p.failure.clear();
    }
    if (!wizard.value || !p.failure.empty())
        return false;
    p.elapsed += seconds;
    p.since_frame += seconds;
    if (!changed && p.since_frame < 1.f / 30)
        return false;
    try {
        auto& model = p.load(wizard);
        if (model.clip)
            p.frame =
                model.clip->sequence.advance(p.frame, p.since_frame * model.clip->rate * 60,
                                             static_cast<unsigned>(model.clip->animation.frames.size()));
        p.since_frame = 0;
        p.image = {std::clamp(width, 64u, 384u), std::clamp(height, 64u, 512u), {}};
        p.image.rgba.resize(std::size_t(p.image.width) * p.image.height);
        p.render(model);
        return true;
    } catch (const std::exception& error) {
        p.failure = error.what();
        log("Wizard preview: " + p.failure);
        p.image = {};
        return true;
    }
}
const Image& WizardPreview::image() const {
    return impl_->image;
}
std::string_view WizardPreview::error() const {
    return impl_->failure;
}
unsigned WizardPreview::triangles() const {
    return impl_->triangle_count;
}
} // namespace sacrifice
