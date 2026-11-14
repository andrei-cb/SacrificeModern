#include "sacrifice/effect_primitives.hpp"
#include "sacrifice/match.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 mul(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 normalize(Vec3 v) {
    const auto length = std::sqrt(static_cast<double>(v.x) * v.x + static_cast<double>(v.y) * v.y +
                                  static_cast<double>(v.z) * v.z);
    return length ? mul(v, static_cast<float>(1 / length)) : v;
}
}
// 0x16019830: all four curves exist even when only one strand is selected.
void LightningPrimitive::new_curve(CrtRandom& random) {
    for (auto& curve : curves_) {
        curve[0] = {};
        float along = .14285715f;
        for (unsigned i = 1; i < 7; ++i) {
            curve[i].x = static_cast<float>((random.next() - 0x4000) / 16384.0);
            curve[i].z = static_cast<float>((random.next() - 0x4000) / 16384.0);
            curve[i].y = static_cast<float>((random.next() - 0x4000) * .14285715 / 16384 + along);
            along += .14285715f;
        }
        curve[7] = {0, 1, 0};
    }
}
void LightningPrimitive::set_params(const Parameters& p, CrtRandom& random) {
    if (!initialized_) {
        new_curve(random); // LIGHTNING::InitModel.
        initialized_ = true;
    }
    phase_ += p.step;
    if (phase_ < 0 || phase_ > 1) {
        phase_ = 0;
        new_curve(random);
    }
    const auto forward = sub(p.target, p.source);
    // LIGHTNINGPRIM::MakeMatrix uses the unnormalized Z delta in this blend.
    const float blend = forward.z * forward.z;
    const Vec3 preferred{0, blend, 1 - blend};
    const auto side = mul(normalize(cross(forward, preferred)), p.deviation),
               up = mul(normalize(cross(cross(forward, preferred), forward)), p.deviation);
    const int maximum = std::max(p.segments, 1), strands = std::clamp(p.strands, 1, 4);
    const int segments = static_cast<int>(std::min(static_cast<float>(maximum),
                                                  std::max(2.0f, maximum * (p.end - p.start))));
    const float increment = (p.end - p.start) * 7 / segments;
    points_.clear();
    points_.reserve(static_cast<std::size_t>(strands) * (segments + 1));
    Vec3 noise;
    for (int branch = 0; branch < strands; ++branch) {
        float sample = p.start * 7;
        for (int i = 0; i <= segments; ++i, sample += increment) {
            // Valid native intervals lie within the eight curve points. The last point is evaluated
            // as segment six at fraction one, avoiding the original zero-weight out-of-bounds read.
            const auto at = std::clamp(sample, 0.0f, 7.0f);
            const auto index = std::min(static_cast<unsigned>(at), 6u);
            const float fraction = at - index;
            const auto& curve = curves_[branch];
            auto point = add(curve[index], mul(sub(curve[index + 1], curve[index]), fraction));
            const float taper = std::min(fraction, 1 - fraction);
            noise.x = static_cast<float>(random.next() * .30000001 / 32768 - .1500000059604645 + noise.x * .75);
            noise.y = static_cast<float>(random.next() * .30000001 / 32768 - .1500000059604645 + noise.y * .75);
            noise.z = static_cast<float>(random.next() * .30000001 / 32768 - .1500000059604645 + noise.z * .75);
            point.x += taper * noise.x;
            point.y += taper * noise.y * .125f;
            point.z += taper * noise.z;
            const auto world = add(p.source, add(mul(side, point.x), add(mul(forward, point.y), mul(up, point.z))));
            points_.push_back({world, p.width * (i == 0 || i == segments ? .25f : 1), i != segments, 255});
        }
    }
}
// CROSSLINE::Draw 0x160257f0: a centre and three radial edges per point, joined by six triangles.
Mesh LightningPrimitive::mesh(const TextureHeader& texture, unsigned frame) const {
    return crossline_mesh(points_, texture, frame, false);
}
Mesh crossline_mesh(std::span<const LightningPrimitive::Point> points_, const TextureHeader& texture,
                    unsigned frame, bool continuous) {
    Mesh mesh;
    if (points_.size() < 2)
        return mesh;
    const auto columns = std::max(texture.width / std::max(texture.frame_width, 1u), 1u);
    frame %= std::max(texture.frames, 1u);
    const float su = static_cast<float>(texture.frame_width) / std::max(texture.width, 1u),
                sv = static_cast<float>(texture.frame_height) / std::max(texture.height, 1u);
    const Vec2 uv0{(frame % columns) * su, (frame / columns) * sv}, uv1{uv0.x + su, uv0.y + sv};
    std::vector<Vec3> tangents(points_.size());
    for (std::size_t i = 1; i < points_.size(); ++i)
        if (points_[i - 1].connected) {
            const auto delta = sub(points_[i].position, points_[i - 1].position);
            tangents[i] = delta;
            tangents[i - 1] = add(tangents[i - 1], delta);
        }
    Vec3 up{0, 0, 1}, side;
    for (std::size_t i = 0; i < points_.size(); ++i) {
        const auto& p = points_[i];
        const auto tangent = normalize(tangents[i]);
        if (tangent.x || tangent.y || tangent.z) {
            auto perpendicular = cross(tangent, up);
            if (perpendicular.x * perpendicular.x + perpendicular.y * perpendicular.y +
                    perpendicular.z * perpendicular.z < 1e-8f)
                perpendicular = cross(tangent, std::abs(tangent.z) > .9f ? Vec3{0, 1, 0} : Vec3{0, 0, 1});
            side = normalize(perpendicular);
            up = cross(side, tangent);
        }
        const Vec3 offsets[4]{{}, mul(up, p.width),
                               mul(add(mul(side, .866f), mul(up, .5f)), -p.width),
                               mul(sub(mul(up, .5f), mul(side, .866f)), -p.width)};
        for (unsigned edge = 0; edge < 4; ++edge) {
            const auto at = add(p.position, offsets[edge]);
            mesh.vertices.push_back({{at.x, at.z, at.y}, {},
                                     {continuous ? uv0.x + i * su / (points_.size() + 1)
                                                 : (i & 1 ? uv0.x : uv1.x), edge ? uv0.y : uv1.y},
                                     0xffffffu | static_cast<std::uint32_t>(p.alpha) << 24});
        }
        if (p.connected && i + 1 < points_.size()) {
            const auto base = static_cast<std::uint32_t>(i * 4);
            for (unsigned edge = 1; edge < 4; ++edge)
                mesh.indices.insert(mesh.indices.end(), {base, base + edge, base + 4,
                                                        base + edge, base + edge + 4, base + 4});
        }
    }
    return mesh;
}
// MUZZLE (NewModel type 60): SetParams halves fMFx for the local X and Z extents and keeps fMFz only for
// the forward bounds; 0x410dd0 aims local Y from the host at its target with world up as the reference.
// Draw indexes only the nine y = 0 vertices of its 19-entry table (0x1605578c): a 3x3 grid whose centre
// takes the frame's first UV corner and whose edges mirror it out to the opposite one, in eight triangles.
Mesh muzzle_mesh(Vec3 at, Vec3 target, float width, const TextureHeader& texture, unsigned frame) {
    Mesh mesh;
    auto forward = normalize(sub(target, at));
    if (!forward.x && !forward.y && !forward.z)
        forward = {1, 0, 0};
    auto side = cross(forward, Vec3{0, 0, 1});
    if (side.x * side.x + side.y * side.y + side.z * side.z < 1e-8f)
        side = cross(forward, Vec3{0, 1, 0});
    side = normalize(side);
    const auto vertical = normalize(cross(side, forward));
    const auto columns = std::max(texture.width / std::max(texture.frame_width, 1u), 1u);
    frame %= std::max(texture.frames, 1u);
    const float su = static_cast<float>(texture.frame_width) / std::max(texture.width, 1u),
                sv = static_cast<float>(texture.frame_height) / std::max(texture.height, 1u);
    const Vec2 uv0{(frame % columns) * su, (frame / columns) * sv}, uv1{uv0.x + su, uv0.y + sv};
    // Native order (x, z): the ring from (-1, 1) clockwise, then the centre.
    static constexpr int grid[9][2]{{-1, 1}, {0, 1}, {1, 1}, {1, 0}, {1, -1}, {0, -1}, {-1, -1}, {-1, 0}, {0, 0}};
    const float half = width * .5f;
    for (const auto& [x, z] : grid) {
        const auto p = add(at, add(mul(vertical, x * half), mul(side, z * half)));
        mesh.vertices.push_back({{p.x, p.z, p.y}, {}, {x ? uv1.x : uv0.x, z ? uv1.y : uv0.y}, 0xffffffffu});
    }
    mesh.indices = {0, 1, 7, 1, 8, 7, 1, 2, 8, 2, 3, 8, 8, 3, 5, 3, 4, 5, 7, 8, 6, 8, 5, 6};
    return mesh;
}
void StreamerPrimitive::configure(int lifetime, float width) {
    lifetime_ = std::max(lifetime, 1);
    width_ = width;
    points_.clear();
    stamps_.clear();
    last_tick_ = 0;
    previous_active_ = false;
    frame_ = 0;
    frame_fraction_ = 0;
}
bool StreamerPrimitive::add_point(const Vec3* position, int tick, unsigned texture_frames) {
    tick = std::max(tick, last_tick_);
    const auto elapsed = last_tick_ && previous_active_ ? tick - last_tick_ : 0;
    last_tick_ = tick;
    const auto delta = position ? sub(*position, previous_) : Vec3{};
    if (position) {
        if (points_.empty() || delta.x * delta.x + delta.y * delta.y + delta.z * delta.z > .01) {
            const auto count = std::min(std::max(elapsed, 1), lifetime_);
            const auto retained = std::min(points_.size(), static_cast<std::size_t>(lifetime_ - count));
            points_.resize(retained);
            stamps_.resize(retained);
            points_.insert(points_.begin(), count, {});
            stamps_.insert(stamps_.begin(), count, 0);
            for (int i = 0; i < count; ++i) {
                // Native interpolation divides by the original elapsed count even when capacity clips it.
                const auto at = add(*position, mul(sub(previous_, *position), i / static_cast<float>(std::max(elapsed, 1))));
                points_[i] = {at, width_, i != 0 || elapsed != 0, 255};
                stamps_[i] = tick - i;
            }
            previous_ = *position;
        }
        previous_active_ = true;
    } else
        previous_active_ = false;
    while (!stamps_.empty() && stamps_.back() <= tick - lifetime_) {
        stamps_.pop_back();
        points_.pop_back();
    }
    if (!points_.empty())
        points_.back().connected = false;
    const auto advance = elapsed * 15.0 / 60 + frame_fraction_;
    frame_ += static_cast<unsigned>(advance);
    frame_fraction_ = static_cast<float>(advance - static_cast<unsigned>(advance));
    if (frame_ >= std::max(texture_frames, 1u)) {
        frame_ = 0;
        frame_fraction_ = 0;
    }
    return !points_.empty();
}
Mesh StreamerPrimitive::mesh(const TextureHeader& texture) const {
    return crossline_mesh(points_, texture, frame_, true);
}
CloudNoise::CloudNoise(CrtRandom& random) {
    for (auto& value : values_)
        value = static_cast<std::uint16_t>(random.next());
}
double CloudNoise::sample(Vec3 p) const {
    const double coordinates[3]{p.x * .003125, p.y * .003125, p.z * .003125};
    int cell[3];
    double fraction[3];
    for (unsigned i = 0; i < 3; ++i) {
        cell[i] = static_cast<int>(coordinates[i]);
        // The native negative-coordinate adjustment also applies at exact negative integers.
        if (coordinates[i] < 0)
            --cell[i];
        fraction[i] = coordinates[i] - cell[i];
    }
    const auto value = [&](int x, int y, int z) {
        return static_cast<double>(values_[((x & 31) * 32 + (y & 31)) * 32 + (z & 31)]);
    };
    const auto plane = [&](int z) {
        const auto a = value(cell[0], cell[1], z), b = value(cell[0] + 1, cell[1], z);
        const auto c = value(cell[0], cell[1] + 1, z), d = value(cell[0] + 1, cell[1] + 1, z);
        const auto low = a + (c - a) * fraction[1], high = b + (d - b) * fraction[1];
        return low + (high - low) * fraction[0];
    };
    const auto low = plane(cell[2]);
    return (low + (plane(cell[2] + 1) - low) * fraction[2]) / 32768;
}
Mesh cloud_mesh(const CloudNoise& noise, Vec3 radii, Vec3 phase, float opacity, Vec3 origin, Vec3 eye,
                const TextureHeader& texture, unsigned frame, float detail) {
    Mesh mesh;
    if (radii.x == 0 || radii.y == 0 || radii.z == 0)
        return mesh;
    detail = std::clamp(detail, 0.0f, 1.0f);
    const auto columns = static_cast<unsigned>(detail * 24 + 16);
    auto rings = static_cast<unsigned>((std::sqrt(detail) + 1) * 8);
    if (!(rings & 1))
        ++rings;
    frame %= std::max(texture.frames, 1u);
    const auto across = std::max(texture.width / std::max(texture.frame_width, 1u), 1u);
    const float su = static_cast<float>(texture.frame_width) / std::max(texture.width, 1u),
                sv = static_cast<float>(texture.frame_height) / std::max(texture.height, 1u);
    const Vec2 center{(frame % across + .5f) * su, (frame / across + .5f) * sv};
    const auto eye_local = sub(eye, origin);
    const auto length = std::sqrt(radii.x * radii.x + radii.y * radii.y + radii.z * radii.z);
    const auto vertex = [&](Vec3 p, Vec2 uv) {
        const Vec3 sample{p.x * 1920 / radii.x + phase.x, p.y * 1920 / radii.y + phase.y,
                          p.z * 1920 / radii.z + phase.z};
        p = mul(p, static_cast<float>(noise.sample(sample) * .3 + .85));
        const Vec3 normal{p.x * length / radii.x, p.y * length / radii.y, p.z * length / radii.z};
        const auto view = sub(p, eye_local);
        const double dot = normal.x * view.x + normal.y * view.y + normal.z * view.z;
        const double denominator = (normal.x * normal.x + normal.y * normal.y + normal.z * normal.z) *
                                   (view.x * view.x + view.y * view.y + view.z * view.z);
        const auto alpha = static_cast<std::uint32_t>(std::clamp(dot < 0 && denominator > 0
            ? std::min(1.0, 2 * dot * dot / denominator) * opacity * 255 : 0, 0.0, 255.0));
        const auto world = add(p, origin);
        mesh.vertices.push_back({{world.x, world.z, world.y}, {}, uv, alpha << 24});
    };
    vertex({0, 0, radii.z}, center);
    for (unsigned ring = 1; ring <= rings; ++ring) {
        const double latitude = 3.14159 * ring / (rings + 1);
        const float s = static_cast<float>(std::sin(latitude)), z = static_cast<float>(std::cos(latitude) * radii.z);
        const auto uv_radius = std::min(static_cast<float>(ring) / (rings + 1), 1 - static_cast<float>(ring) / (rings + 1));
        for (unsigned j = 0; j < columns; ++j) {
            const double angle = -6.28318 * j / (columns - 1);
            const float x = static_cast<float>(std::sin(angle)), y = static_cast<float>(std::cos(angle));
            vertex({s * radii.x * x, s * radii.y * y, z}, {center.x + uv_radius * su * x, center.y + uv_radius * sv * y});
        }
    }
    const auto bottom = static_cast<std::uint32_t>(mesh.vertices.size());
    vertex({0, 0, -radii.z}, center);
    for (unsigned j = 0; j + 1 < columns; ++j) {
        mesh.indices.insert(mesh.indices.end(), {0, j + 2, j + 1});
        for (unsigned ring = 0; ring + 1 < rings; ++ring) {
            const auto a = 1 + ring * columns + j, b = a + columns;
            mesh.indices.insert(mesh.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
        const auto a = 1 + (rings - 1) * columns + j;
        mesh.indices.insert(mesh.indices.end(), {a, a + 1, bottom});
    }
    return mesh;
}
} // namespace sacrifice
