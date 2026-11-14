#include "sacrifice/player_visuals.hpp"
#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/soul_actor.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
std::optional<HudRect> hover_screen_bounds(const std::array<Vec3, 8>& view, unsigned width, unsigned height) {
    Vec3 low = view[0], high = low;
    unsigned clipped = 31;
    for (const auto p : view) {
        unsigned flags = (p.z < .01f ? 1 : 0) | (p.x < -p.z ? 2 : 0) | (p.x > p.z ? 4 : 0) |
                         (p.y > p.z ? 8 : 0) | (p.y < -p.z ? 16 : 0);
        clipped &= flags;
        low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    }
    const float depth = (low.z + high.z) * .5f;
    if (clipped || depth <= .5f)
        return {};
    const float x = width * .5f, y = height * .5f;
    return HudRect{{x + x * low.x / depth, y + y * low.y / depth},
                   {(high.x - low.x) * x / depth, (high.y - low.y) * y / depth}};
}
bool MiniMapMarker::contains(Vec2 point) const {
    // 0x467250 truncates the delta before abs and compares strictly with half-size.
    return std::abs(static_cast<int>(centre.x - point.x)) < half_size &&
           std::abs(static_cast<int>(centre.y - point.y)) < half_size;
}
std::optional<MiniMapMarker> minimap_marker(const Match& match, const MatchSide& own,
                                           const MatchObject& object, const MiniMapTransform& transform,
                                           const MiniMapLayout& layout) {
    const auto* motion = object.source_motion();
    if (!motion || !(object.kind_flags & 0x1f) || (object.common_status & 0x100) ||
        (object.state_flags & MatchObject::removed_flag))
        return {};
    const auto* side = object.allegiance;
    const bool local = side == &own;
    const auto known = std::ranges::find(own.known, &object, &MatchSide::Known::object);
    if (!local && (known == own.known.end() || known->stamp + 12 <= own.stamp ||
                   match.resolve(known->reference) != &object))
        return {};
    if (const auto* actor = dynamic_cast<const CreatureActor*>(&object);
        actor && (!actor->alive() || (object.kind_flags & 8)))
        return {}; // 0x453b00's scripted-reference exception remains pending.
    MiniMapMarker marker;
    marker.colour = side ? unit_side_colour(side->definition.index == 31 ? 8 : side->definition.color) : 0xffffffff;
    bool edge = local;
    if (object.kind_flags & 1) { // 0x466930: every known wizard has an edge arrow.
        marker.textured = true;
        marker.icon = 2;
        marker.half_size = 6;
        edge = true;
    } else if (const auto* building = dynamic_cast<const BuildingActor*>(&object); building && !building->primary) {
        const auto flags = building->building_flags;
        if (flags & 1)
            marker.icon = 0, marker.textured = true;
        else if (flags & 2)
            marker.icon = 4, marker.textured = true;
        else if ((flags & 0x55a0) == 0x55a0)
            marker.icon = 1, marker.textured = true;
        else if ((flags & 0x2810) == 0x2810) {
            if (building->reserved)
                return {};
            marker.icon = 3;
            marker.textured = true;
            marker.colour = 0xff00afff;
        }
        if (marker.textured) {
            marker.half_size = 6;
            edge = local || (match.shared_vision && side &&
                ((side->allies & own.identity) || (own.allies & side->identity) || side->identity == own.identity));
        } else
            edge = false;
    } else if (const auto* soul = dynamic_cast<const SoulActor*>(&object)) {
        if (soul->soul_flags & 0xc0000000)
            return {};
        marker.colour = soul->eligible_sides & own.identity ? 0xff00afff : 0xff0000ff;
        edge = false;
    } else if (object.kind_flags & 0x10)
        edge = false; // Secondary building parts use an ordinary point.
    auto at = transform.normalized(motion->position);
    float x = at.x - .5f, y = at.y - .5f;
    const float distance = std::hypot(x, y);
    if (distance > .4375f) {
        if (!edge)
            return {};
        const float scale = .4375f / distance;
        x *= scale;
        y *= scale;
        at = {.5f + x, .5f + y};
        marker.angle = std::atan2(x, -y);
        marker.textured = true;
        marker.icon = object.kind_flags & 0x10 ? 6 : 5;
        marker.half_size = 6;
    }
    marker.centre = {layout.bounds.origin.x + at.x * layout.bounds.size.x,
                     layout.bounds.origin.y + at.y * layout.bounds.size.y};
    if (object.timer && (match.clock().match & 31) <= 15) {
        marker.half_size = std::max(marker.half_size + 2, marker.half_size * 1.5f);
        marker.colour = 0xff0000ff;
    }
    // 0x466bb0 ranks textured markers, buildings, wizards and owned objects, in that order.
    marker.priority = (marker.textured ? 16 : 0) | (object.kind_flags & 0x10 ? 8 :
                       object.kind_flags & 1 ? 4 : 0) | (local ? 2 : 0);
    return marker;
}
UnitHudLayout UnitHudLayout::make(Vec2 anchor, float depth, unsigned view_width) {
    const float size = view_width * std::clamp(1 / depth, .025f, .25f) * .5f;
    UnitHudLayout result;
    result.arrow = {{anchor.x - size * .5f, anchor.y - size}, {size, size}};
    const float width = size * 1.5f, height = width * .1f;
    for (unsigned i = 0; i < 2; ++i)
        result.bars[i] = {{anchor.x - width * .5f, anchor.y - size - height * (1 + i * 1.2f)},
                          {width, height}};
    return result;
}
std::uint32_t unit_side_colour(unsigned index) {
    // SIDS loader 0x456330 resolves indices through 0x4c7894.
    static constexpr std::array<std::uint32_t, 9> colours{
        0xffffff, 0x0000ff, 0x00ff00, 0xffff00, 0xff00ff, 0xb62f00, 0x2f00b6, 0x00ffff, 0x7f7f7f};
    const auto c = index < colours.size() ? colours[index] : 0xffffff;
    return 0xff000000u | (c & 0xff) << 16 | (c & 0xff00) | (c >> 16);
}
Mesh hover_bracket_mesh(HudRect box, unsigned view_width, std::uint32_t colour) {
    const float scale = view_width / 640.0f, minimum = 32 * scale;
    if (box.size.x < minimum) {
        box.origin.x -= (minimum - box.size.x) * .5f;
        box.size.x = minimum;
    }
    if (box.size.y < minimum) {
        box.origin.y -= (minimum - box.size.y) * .5f;
        box.size.y = minimum;
    }
    const float dx = std::clamp(box.size.x * .1f, 4 * scale, minimum),
                dy = std::clamp(box.size.y * .1f, 4 * scale, minimum),
                x = box.origin.x, y = box.origin.y, r = x + box.size.x, b = y + box.size.y,
                h = box.size.x - dx * 1.5f, v = box.size.y - dx * 1.5f;
    const std::array<Vec2, 20> points{{{x + dx, y}, {x + dx + h, y}, {x + dx, y + dy},
        {x, y}, {x + dx, y - dy}, {r - dx, b}, {r - dx - h, b}, {r - dx, b - dy},
        {r, b}, {r - dx, b + dy}, {x, y + dy}, {x, y + dy + v}, {x - dx, y + dy},
        {x, y}, {x + dx, y + dy}, {r, b - dy}, {r, b - dy - v}, {r + dx, b - dy},
        {r, b}, {r - dx, b - dy}}};
    Mesh mesh;
    for (unsigned i = 0; i < points.size(); ++i)
        mesh.vertices.push_back({{points[i].x, points[i].y, 0}, {}, {},
                                 (colour & 0xffffff) | (i % 5 ? 0u : 0xff000000u)});
    for (unsigned i = 0; i < 20; i += 5)
        for (unsigned j = 1; j <= 4; ++j)
            mesh.indices.insert(mesh.indices.end(), {i, i + j, i + (j == 4 ? 1 : j + 1)});
    return mesh;
}
double building_intercept(const MrmGeometry& model, Vec3 from, Vec3 ray, unsigned vertices) {
    if (model.frames.empty())
        return -1;
    const auto subtract = [](Vec3 a, Vec3 b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; };
    const auto cross = [](Vec3 a, Vec3 b) {
        return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    };
    const auto dot = [](Vec3 a, Vec3 b) {
        return static_cast<double>(a.x) * b.x + static_cast<double>(a.y) * b.y + static_cast<double>(a.z) * b.z;
    };
    const auto vertex = [&](unsigned index) {
        const auto p = model.frames[0][index].position; // Stored in renderer Y-up.
        return Vec3{p.x, p.z, p.y};
    };
    double nearest = -1;
    if (!vertices)
        vertices = static_cast<unsigned>(model.frames[0].size());
    for (const auto index : model.active_faces(vertices)) {
        const auto& face = model.faces[index];
        if (!(dot(ray, face.normal) > 0)) // game3d 0x16002950: back faces and parallel rays do not pick.
            continue;
        const auto a = vertex(face.vertices[0]), b = vertex(face.vertices[1]), c = vertex(face.vertices[2]);
        const auto edge = subtract(b, a), other = subtract(c, a), p = cross(ray, other);
        const double determinant = dot(edge, p);
        if (determinant == 0)
            continue;
        const auto delta = subtract(from, a);
        const double u = dot(delta, p) / determinant;
        if (u < 0 || u > 1)
            continue;
        const auto q = cross(delta, edge);
        const double v = dot(ray, q) / determinant;
        if (v < 0 || u + v > 1)
            continue;
        const double distance = dot(other, q) / determinant;
        if (distance >= 0 && (nearest < 0 || distance < nearest))
            nearest = distance;
    }
    return nearest;
}
Mesh order_marker_mesh(const PlayerControl::Marker& marker, const TextureHeader& texture, Vec3 eye) {
    Mesh mesh;
    const float age = marker.age;
    if (age < 0 || age >= 1)
        return mesh;
    const auto frame = static_cast<unsigned>(age) % std::max(texture.frames, 1u);
    const unsigned columns = std::max(texture.width / std::max(texture.frame_width, 1u), 1u);
    const float su = static_cast<float>(texture.frame_width) / texture.width,
                sv = static_cast<float>(texture.frame_height) / texture.height;
    const float u = (frame % columns) * su, v = (frame / columns) * sv;
    const float yaw = age * 6.2831802f, radius = age * 4 + 1, height = age * 5 + 10;
    const double dx = eye.x - marker.point.x, dy = eye.y - marker.point.y, length = std::hypot(dx, dy);
    const auto c = marker.colour;
    const auto rgb = (c & 0xff) << 16 | (c & 0xff00) | (c >> 16 & 0xff);
    // CYLINDER's 16 samples include the seam (0x160218a0); U spans the frame in 15 steps.
    for (unsigned row = 0; row < 2; ++row)
        for (unsigned i = 0; i < 16; ++i) {
            const double angle = -static_cast<double>(i) * (6.2831802 / 15) + yaw;
            const float x = static_cast<float>(std::sin(angle)), y = static_cast<float>(std::cos(angle));
            const double facing = length ? (x * dx + y * dy) / length : 0;
            // 0x16022290: fourth power of the horizontal view/normal dot, rounded to a byte.
            const auto alpha = static_cast<unsigned>(std::clamp(std::lround(
                facing * facing * facing * facing * static_cast<int>((1 - age) * 255)), 0l, 255l));
            mesh.vertices.push_back({{marker.point.x + (row ? x * radius : 0),
                                       marker.point.z + (row ? height : 0),
                                       marker.point.y + (row ? y * radius : 0)},
                                      {0, 1, 0}, {u + i * su / 15, v + (row ? sv : 0)}, rgb | alpha << 24});
        }
    for (unsigned i = 0; i < 15; ++i)
        mesh.indices.insert(mesh.indices.end(), {i, i + 16, i + 1, i + 1, i + 16, i + 17});
    return mesh;
}
} // namespace sacrifice
