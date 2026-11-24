#include "sacrifice/creature_actor.hpp"
#include "sacrifice/soul_actor.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include "sacrifice/spell_data.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
FourCC texture(const SpellData& data, const char* key, const char* fallback) {
    return FourCC{
        static_cast<unsigned>(data.integer(FourCC::from_text(key), FourCC::from_text(fallback).value))};
}
float cubic(float a, float b, float c, float d, float t) {
    const float u = 1 - t;
    return u * u * u * a + 3 * u * u * t * b + 3 * u * t * t * c + t * t * t * d;
}
Vec3 curve(const std::array<Vec3, 4>& p, float t) {
    return {cubic(p[0].x, p[1].x, p[2].x, p[3].x, t), cubic(p[0].y, p[1].y, p[2].y, p[3].y, t),
            cubic(p[0].z, p[1].z, p[2].z, p[3].z, t)};
}
void ground(Match& match, Vec3& point) {
    if (match.terrain)
        point.z = static_cast<float>(terrain_surface(*match.terrain, point.x, point.y));
}
} // namespace
void SpellActor::add_vine(Vec3 root, Vec3 tip, float radius) {
    surface.opacity = 1;
    CurvedSurface::Vine vine;
    for (unsigned n = 0; n < 4; ++n) {
        const float t = n / 3.f;
        vine.curve[n] = {root.x + (tip.x - root.x) * t, root.y + (tip.y - root.y) * t,
                         root.z + (tip.z - root.z) * t};
    }
    vine.radius = radius;
    surface.vines_data.push_back(vine);
}
bool SpellActor::advance_vines(int ticks, float goal) {
    bool visible = false;
    for (auto& vine : surface.vines_data) {
        const float step = ticks * vine.speed;
        vine.progress = vine.progress < goal ? std::min(goal, vine.progress + step)
                                             : std::max(goal, vine.progress - step);
        visible |= vine.progress > 0;
    }
    return visible;
}
namespace {
void build_tornado_profile(const std::array<float, 92>& params, std::array<Vec3, 7>& centres,
                           std::array<float, 7>& radii) {
    const Vec3 low{params[0], params[1], params[2]}, high{params[4], params[5], params[6]};
    constexpr float fractions[]{0, 1.f / 6, .25f, .5f, .75f, 5.f / 6, 1};
    const float base = params[3] / 3, top = params[7];
    constexpr float radius_t[]{0, .6f, .6f, .675f, .75f, .95f, 1};
    for (unsigned n = 0; n < 7; ++n) {
        const float t = fractions[n];
        centres[n] = {low.x + (high.x - low.x) * t, low.y + (high.y - low.y) * t,
                      low.z + (high.z - low.z) * t};
        radii[n] = n ? base + (top - base) * radius_t[n] : params[3];
    }
    const float wobble = (high.z - low.z) * .3f;
    centres[2].x += std::sin(params[9]) * wobble;
    centres[2].y += std::sin(params[10]) * wobble;
    centres[4].x += std::sin(params[11]) * wobble;
    centres[4].y += std::sin(params[12]) * wobble;
    centres[3] = {(centres[2].x + centres[4].x) * .5f, (centres[2].y + centres[4].y) * .5f,
                  (centres[2].z + centres[4].z) * .5f};
}
} // namespace
// LATHES5D::Draw 0x1600bdd0: two cubic five-component profiles (XYZ, radius, alpha),
// revolved around Z. TORNADO::SetParams 0x1600ce10 supplies two joined cubic profiles.
Mesh SpellActor::curved_surface_mesh(unsigned layer, const TextureHeader& texture, float detail,
                                     const Terrain* terrain, std::optional<Vec3> eye,
                                     std::optional<Vec3> camera_right) const {
    Mesh mesh;
    const auto& p = surface.parameters;
    const unsigned columns = static_cast<unsigned>(std::clamp(static_cast<int>(detail * 9 + 7), 7, 16));
    const unsigned rows =
        surface.kind == CurvedSurface::tornado
            ? static_cast<unsigned>((std::sqrt(std::clamp(detail, 0.f, 1.f)) * 4 + 3) * 4) * 2 - 1
            : static_cast<unsigned>(std::clamp(static_cast<int>(std::sqrt(detail) * 9 + 7), 7, 16));
    const unsigned across = std::max(texture.width / std::max(texture.frame_width, 1u), 1u);
    const float su = static_cast<float>(texture.frame_width) / std::max(texture.width, 1u),
                sv = static_cast<float>(texture.frame_height) / std::max(texture.height, 1u);
    const auto frame = static_cast<unsigned>(std::max(0.f, surface.frame)) % std::max(texture.frames, 1u);
    const float u0 = (frame % across) * su, v0 = (frame / across) * sv;
    if (surface.kind == CurvedSurface::fire_wall) {
        // FIREWALL: a terrain-following ribbon, with a wavering ridge between its two ground edges, from
        // p[7] to p[8] along the axis (the fire wall's two ends grow apart separately, 0x427CD0).
        const unsigned segments = std::max(1u, static_cast<unsigned>(std::ceil(p[8] - p[7])));
        const Vec3 axis{p[1], p[2], 0};
        for (unsigned segment = 0; segment < segments; ++segment) {
            const auto first = static_cast<unsigned>(mesh.vertices.size());
            for (unsigned edge = 0; edge < 2; ++edge) {
                const unsigned column = segment + edge;
                const float t = column / static_cast<float>(segments), along = p[7] + t * (p[8] - p[7]);
                const float wave = std::sin(along * .2f + p[5]), bend = std::sin(along * .15f + p[6]);
                for (unsigned row = 0; row < 3; ++row) {
                    const float across = row == 1 ? bend * p[3] * .3f : (row == 0 ? -p[3] : p[3]);
                    const float x = motion.position.x + axis.x * along - axis.y * across,
                                y = motion.position.y + axis.y * along + axis.x * across;
                    const float ground =
                        terrain ? static_cast<float>(terrain_surface(*terrain, x, y)) : motion.position.z;
                    const float height = row == 1 ? p[4] * (1 + wave * .25f) : 0.f;
                    const float alpha = std::min(1.f, std::min(t, 1 - t) * 20) * surface.opacity;
                    const auto color = 0xffffffu | static_cast<unsigned>(std::clamp(alpha * 255, 0.f, 255.f))
                                                       << 24;
                    mesh.vertices.push_back(
                        {{x, ground + height, y},
                         {},
                         {u0 + (segment % 10 + edge) * .1f * su, v0 + (row == 1 ? 0.f : 1.f) * sv},
                         color});
                }
            }
            for (unsigned row = 0; row < 2; ++row) {
                const auto a = first + row, b = a + 3;
                mesh.indices.insert(mesh.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
            }
        }
        return mesh;
    }
    if (surface.kind == CurvedSurface::cylinder || surface.kind == CurvedSurface::ring) {
        // BEZIERCYLINDER: cubic XYZ, radius and alpha. BEZIERDONUT uses radius/height/alpha,
        // with each ring draped over LAND (the volcano's animated ground spatter).
        const bool ring = surface.kind == CurvedSurface::ring;
        for (unsigned row = 0; row <= rows; ++row) {
            const float t = row / static_cast<float>(rows);
            Vec3 centre{};
            float radius, alpha;
            if (ring) {
                radius = cubic(p[0], p[3], p[6], p[9], t);
                centre.z = cubic(p[1], p[4], p[7], p[10], t);
                alpha = cubic(p[2], p[5], p[8], p[11], t);
            } else {
                centre = {cubic(p[0], p[3], p[6], p[9], t), cubic(p[1], p[4], p[7], p[10], t),
                          cubic(p[2], p[5], p[8], p[11], t)};
                radius = cubic(p[12], p[13], p[14], p[15], t);
                alpha = cubic(p[16], p[17], p[18], p[19], t);
            }
            const auto color =
                0xffffffu | static_cast<unsigned>(std::clamp(alpha * surface.opacity, 0.f, 255.f)) << 24;
            for (unsigned column = 0; column <= columns; ++column) {
                const float u = column / static_cast<float>(columns), angle = u * 6.28318f;
                const float x = motion.position.x + centre.x + std::sin(angle) * radius,
                            y = motion.position.y + centre.y + std::cos(angle) * radius;
                const float z =
                    ring && terrain ? static_cast<float>(terrain_surface(*terrain, x, y)) : motion.position.z;
                mesh.vertices.push_back({{x, z + centre.z, y}, {}, {u0 + u * su, v0 + t * sv}, color});
            }
        }
        for (unsigned row = 0; row < rows; ++row)
            for (unsigned column = 0; column < columns; ++column) {
                const auto a = row * (columns + 1) + column, b = a + columns + 1;
                mesh.indices.insert(mesh.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
            }
        return mesh;
    }
    if (surface.kind == CurvedSurface::tentacle) {
        // TENTACLE::Draw 0x1600c540: two cubic tubes, each with its own material
        // and cubic radius profile (native template at 0x4cb570).
        const auto& tube = surface.tentacles.at(layer);
        Vec3 previous_right{1, 0, 0};
        for (unsigned row = 0; row <= rows; ++row) {
            const float t = row / static_cast<float>(rows), u = 1 - t;
            const auto at = curve(tube.curve, t);
            const auto& c = tube.curve;
            Vec3 tangent{
                u * u * (c[1].x - c[0].x) + 2 * u * t * (c[2].x - c[1].x) + t * t * (c[3].x - c[2].x),
                u * u * (c[1].y - c[0].y) + 2 * u * t * (c[2].y - c[1].y) + t * t * (c[3].y - c[2].y),
                u * u * (c[1].z - c[0].z) + 2 * u * t * (c[2].z - c[1].z) + t * t * (c[3].z - c[2].z)};
            const float length =
                std::sqrt(tangent.x * tangent.x + tangent.y * tangent.y + tangent.z * tangent.z);
            if (length > .0001f)
                tangent = {tangent.x / length, tangent.y / length, tangent.z / length};
            else
                tangent = {0, 0, 1};
            const float dot =
                previous_right.x * tangent.x + previous_right.y * tangent.y + previous_right.z * tangent.z;
            Vec3 right{previous_right.x - dot * tangent.x, previous_right.y - dot * tangent.y,
                       previous_right.z - dot * tangent.z};
            const float width = std::sqrt(right.x * right.x + right.y * right.y + right.z * right.z);
            if (width > .0001f)
                right = {right.x / width, right.y / width, right.z / width};
            else
                right = {0, 1, 0};
            previous_right = right;
            const Vec3 up{tangent.y * right.z - tangent.z * right.y,
                          tangent.z * right.x - tangent.x * right.z,
                          tangent.x * right.y - tangent.y * right.x};
            const auto& r = tube.radii;
            const float radius = cubic(r[0], r[1], r[2], r[3], t);
            for (unsigned column = 0; column <= columns; ++column) {
                const float v = column / static_cast<float>(columns), angle = -6.28318f * v;
                const float x = std::sin(angle) * radius, y = std::cos(angle) * radius;
                mesh.vertices.push_back({{at.x + right.x * x + up.x * y, at.z + right.z * x + up.z * y,
                                          at.y + right.y * x + up.y * y},
                                         {},
                                         {u0 + v * su, v0 + t * sv},
                                         0xffffffff});
            }
        }
        for (unsigned row = 0; row < rows; ++row)
            for (unsigned column = 0; column < columns; ++column) {
                const auto a = row * (columns + 1) + column, b = a + columns + 1;
                mesh.indices.insert(mesh.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
            }
        return mesh;
    }
    if (surface.kind == CurvedSurface::spikes) {
        // SPIKES::Draw: pointed tip, a shoulder at 25% of the length, and a broad base.
        for (const auto& spike : surface.vines_data) {
            if (spike.progress <= 0)
                continue;
            const auto first = static_cast<unsigned>(mesh.vertices.size());
            const auto& root = spike.curve[0];
            const auto& end = spike.curve[3];
            const Vec3 tip{root.x + (end.x - root.x) * spike.progress,
                           root.y + (end.y - root.y) * spike.progress,
                           root.z + (end.z - root.z) * spike.progress};
            mesh.vertices.push_back({{tip.x, tip.z, tip.y}, {}, {u0 + .5f * su, v0 + .5f * sv}, 0xffffffff});
            for (unsigned ring = 0; ring < 2; ++ring) {
                const float t = ring ? 0.f : .75f, radius = spike.radius * spike.progress * (ring ? 1 : .75f);
                for (unsigned side = 0; side <= columns; ++side) {
                    const float angle = side * 6.28318f / columns, x = std::sin(angle), y = std::cos(angle);
                    mesh.vertices.push_back(
                        {{root.x + (tip.x - root.x) * t + x * radius, root.z + (tip.z - root.z) * t,
                          root.y + (tip.y - root.y) * t + y * radius},
                         {},
                         {u0 + (.5f + x * (ring ? .45f : .1125f)) * su,
                          v0 + (.5f + y * (ring ? .45f : .1125f)) * sv},
                         0xffffffff});
                }
            }
            for (unsigned side = 0; side < columns; ++side) {
                const auto a = first + 1 + side, b = a + columns + 1;
                mesh.indices.insert(mesh.indices.end(), {first, a, a + 1, a, b, a + 1, a + 1, b, b + 1});
            }
        }
        return mesh;
    }
    if (surface.kind == CurvedSurface::vines) {
        // VINES::Draw 0x160167f0: cubic centreline, horizontal rings tapering
        // to a single tip. Growth shortens both the curve and its base radius.
        const unsigned sides = 8, sections = 11;
        for (const auto& vine : surface.vines_data) {
            if (vine.progress <= 0)
                continue;
            const auto first = static_cast<unsigned>(mesh.vertices.size());
            for (unsigned row = 0; row < sections; ++row) {
                const float t = row / static_cast<float>(sections), progress = t * vine.progress;
                const auto at = curve(vine.curve, progress);
                const float radius = vine.radius * vine.progress * (1 - t);
                for (unsigned column = 0; column <= sides; ++column) {
                    const float u = column / static_cast<float>(sides), angle = -6.28318f * u;
                    mesh.vertices.push_back(
                        {{at.x + std::sin(angle) * radius, at.z, at.y + std::cos(angle) * radius},
                         {},
                         {u0 + u * su, v0 + t * sv},
                         0xffffffff});
                }
            }
            const auto at = curve(vine.curve, vine.progress);
            const auto tip = static_cast<unsigned>(mesh.vertices.size());
            mesh.vertices.push_back({{at.x, at.z, at.y}, {}, {u0 + .5f * su, v0 + sv}, 0xffffffff});
            for (unsigned row = 0; row < sections; ++row)
                for (unsigned column = 0; column < sides; ++column) {
                    const auto a = first + row * (sides + 1) + column, b = a + sides + 1;
                    if (row + 1 == sections)
                        mesh.indices.insert(mesh.indices.end(), {a, tip, a + 1});
                    else
                        mesh.indices.insert(mesh.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
                }
        }
        return mesh;
    }
    if (surface.kind == CurvedSurface::rays) {
        // SPIKERAYS::Draw 0x16026330: three fins, each with two length sections.
        const float radius = p[0], near = p[1], far = p[2];
        const auto color = (surface.color & 0xffffffu) |
                           static_cast<unsigned>(std::clamp(surface.opacity * 255, 0.f, 255.f)) << 24;
        for (const auto& ray : surface.rays_data) {
            const auto first = static_cast<unsigned>(mesh.vertices.size());
            const float tip = near + (std::sin(ray.phase) + 1) * .5f * (far - near);
            for (unsigned row = 0; row < 3; ++row)
                for (unsigned corner = 0; corner < 4; ++corner) {
                    const float length = row == 0 ? 0.f : row == 1 ? radius : tip;
                    const auto& b = ray.base[corner];
                    const float scale = surface.opacity * (effect == Effect::air_clawpult ? p[3]
                                                           : p[3] > 0                     ? p[3]
                                                                                          : 1.f);
                    const Vec3 local{(b.x + ray.axis.x * length) * scale, (b.y + ray.axis.y * length) * scale,
                                     (b.z + ray.axis.z * length) * scale};
                    const auto& o = motion.orientation;
                    mesh.vertices.push_back(
                        {{motion.position.x + local.x * o[0] + local.y * o[3] + local.z * o[6],
                          motion.position.z + local.x * o[2] + local.y * o[5] + local.z * o[8],
                          motion.position.y + local.x * o[1] + local.y * o[4] + local.z * o[7]},
                         {},
                         {u0 + (corner ? su : 0), v0 + row * .5f * sv},
                         color});
                }
            for (unsigned row = 0; row < 2; ++row)
                for (unsigned fin = 1; fin < 4; ++fin) {
                    const auto a = first + row * 4, b = a + 4;
                    mesh.indices.insert(mesh.indices.end(), {a, a + fin, b, a + fin, b + fin, b});
                }
        }
        return mesh;
    }
    if (surface.kind == CurvedSurface::cuboid) {
        // CUBOID::SetParams 0x160237f0 / Draw 0x160238b0: six textured faces, centred on the victim.
        constexpr unsigned faces[6][4]{{0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 6, 2},
                                       {1, 5, 3, 7}, {2, 3, 6, 7}, {4, 5, 0, 1}};
        const auto color = 0xffffffu | static_cast<unsigned>(std::clamp(surface.opacity * 255, 0.f, 255.f))
                                           << 24;
        for (const auto& face : faces) {
            const auto first = static_cast<unsigned>(mesh.vertices.size());
            for (unsigned n = 0; n < 4; ++n) {
                const auto corner = face[n];
                const Vec3 local{((corner & 1) ? 1.f : -1.f) * p[0] * .5f,
                                 ((corner & 2) ? 1.f : -1.f) * p[1] * .5f,
                                 ((corner & 4) ? 1.f : -1.f) * p[2] * .5f};
                mesh.vertices.push_back(
                    {{motion.position.x + local.x, motion.position.z + local.z, motion.position.y + local.y},
                     {},
                     {u0 + (n & 1) * su, v0 + (n >> 1) * sv},
                     color});
            }
            mesh.indices.insert(mesh.indices.end(),
                                {first, first + 1, first + 2, first + 1, first + 3, first + 2});
        }
        return mesh;
    }
    if (surface.kind == CurvedSurface::ground) {
        // GROUNDHUGGER 0x16014ba0: terrain-sampled rings 2.5 apart from the inner radius (p[1]) to the outer
        // one (p[0]), with segments for the largest radius (p[2]) and alternating texture strips.
        const float radius = p[0], inner = std::clamp(p[1], 0.f, radius);
        if (radius <= 0 || !terrain)
            return mesh;
        const unsigned segments =
            std::max(5u, static_cast<unsigned>(std::max(radius, p[2]) * 6.28318f / 5 + 2) | 1u);
        const unsigned rings = static_cast<unsigned>(std::max(0.f, std::ceil((radius - inner) / 2.5f)));
        for (unsigned row = 0; row <= rings + 1; ++row) {
            const float r = std::min(radius, inner + row * 2.5f), alpha = surface.opacity;
            const auto color = 0xffffffu | static_cast<unsigned>(std::clamp(alpha * 255, 0.f, 255.f)) << 24;
            for (unsigned column = 0; column <= segments; ++column) {
                const float angle = column * 6.28318f / segments, x = std::sin(angle) * r,
                            y = std::cos(angle) * r;
                const float wx = motion.position.x + x, wy = motion.position.y + y;
                const float z = static_cast<float>(terrain_surface(*terrain, wx, wy));
                mesh.vertices.push_back(
                    {{wx, z + (row <= rings ? .5f : 0.f), wy},
                     {},
                     {u0 + (.5f + ((column & 1) ? .5f : -.5f) * r / radius) * su, v0 + (row & 1) * sv},
                     color});
            }
        }
        for (unsigned row = 0; row <= rings; ++row)
            for (unsigned column = 0; column < segments; ++column) {
                const auto a = row * (segments + 1) + column, b = a + segments + 1;
                mesh.indices.insert(mesh.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
            }
        return mesh;
    }
    std::array<Vec3, 7> centres{};
    std::array<float, 7> radii{};
    if (surface.kind == CurvedSurface::tornado)
        build_tornado_profile(p, centres, radii);
    const bool tornado = surface.kind == CurvedSurface::tornado;
    const unsigned first_row = tornado && layer ? (rows - 1) / 2 : 0;
    const unsigned last_row = tornado && layer == 0 ? (rows - 1) / 2 : rows - 1;
    for (unsigned row = first_row; row <= last_row; ++row) {
        const float t = static_cast<float>(row) / (rows - 1);
        Vec3 centre;
        float radius, alpha;
        if (surface.kind == CurvedSurface::lathe) {
            const auto base = 4 + layer * 22;
            centre = {cubic(p[base + 2], p[base + 7], p[base + 12], p[base + 17], t),
                      cubic(p[base + 3], p[base + 8], p[base + 13], p[base + 18], t),
                      cubic(p[base + 4], p[base + 9], p[base + 14], p[base + 19], t)};
            radius = cubic(p[base + 5], p[base + 10], p[base + 15], p[base + 20], t);
            alpha = cubic(p[base + 6], p[base + 11], p[base + 16], p[base + 21], t);
        } else {
            const unsigned part = t < .5f ? 0 : 3;
            const float u = t < .5f ? t * 2 : (t - .5f) * 2;
            centre = curve({centres[part], centres[part + 1], centres[part + 2], centres[part + 3]}, u);
            radius = cubic(radii[part], radii[part + 1], radii[part + 2], radii[part + 3], u);
            // TORNADO's two cubic alpha profiles: fade the sky end into the clouds,
            // with full density at the join and ground (0x1600ce10 / 0x1600d270).
            alpha = (part == 0 ? cubic(0, .75f, 1, 1, u) : 1.f) * 255.f * std::sqrt(surface.opacity);
        }
        const auto color = 0xffffffu | static_cast<unsigned>(std::clamp(alpha, 0.f, 255.f)) << 24;
        if (tornado && layer == 2) {
            // TORNADO::Draw's lower skirt: two camera-facing strips from .9 to 2 radii.
            const Vec3 at{motion.position.x + centre.x, motion.position.y + centre.y,
                          motion.position.z + centre.z};
            const Vec3 viewer = eye.value_or(Vec3{at.x, at.y - 10, at.z});
            const float dx = viewer.x - at.x, dy = viewer.y - at.y;
            const float length = std::sqrt(dx * dx + dy * dy);
            const Vec3 facing = length > .001f ? Vec3{-dy / length, dx / length, 0} : Vec3{1, 0, 0};
            const Vec3 right = camera_right.value_or(facing);
            const Vec3 side{(facing.x + right.x) * .5f, (facing.y + right.y) * .5f, 0};
            constexpr float widths[]{.9f, 2.f, -2.f, -.9f};
            const auto skirt_color = row == first_row ? 0xffffffu : color;
            for (unsigned n = 0; n < 4; ++n)
                mesh.vertices.push_back(
                    {{at.x + side.x * radius * widths[n], at.z, at.y + side.y * radius * widths[n]},
                     {},
                     {u0 + (n == 1 || n == 2 ? su : 0), v0 + (t * 2 - 1) * sv},
                     skirt_color});
            if (row > first_row) {
                const auto a = static_cast<unsigned>(mesh.vertices.size()) - 8;
                for (unsigned side_index : {0u, 2u}) {
                    const auto b = a + side_index;
                    mesh.indices.insert(mesh.indices.end(), {b, b + 1, b + 4, b + 1, b + 5, b + 4});
                }
            }
            continue;
        }
        for (unsigned column = 0; column <= columns; ++column) {
            const float u = static_cast<float>(column) / columns;
            const float angle = u * 6.28318f + (surface.kind == CurvedSurface::lathe ? p[0] : p[8]);
            const Vec3 local{centre.x + std::sin(angle) * radius, centre.y + std::cos(angle) * radius,
                             centre.z};
            static constexpr Orientation world{1, 0, 0, 0, 1, 0, 0, 0, 1};
            const auto& o = tornado ? world : motion.orientation;
            auto vertex_color = color;
            if (!tornado) {
                // LATHES5D's ring emitter 0x16022150 attenuates by the squared
                // horizontal eye/radial dot product. Silhouettes must fade away.
                const Vec3 viewer =
                    eye.value_or(Vec3{motion.position.x, motion.position.y - 10, motion.position.z});
                const Vec3 delta{viewer.x - motion.position.x, viewer.y - motion.position.y,
                                 viewer.z - motion.position.z};
                const float dx = delta.x * o[0] + delta.y * o[1] + delta.z * o[2] - centre.x,
                            dy = delta.x * o[3] + delta.y * o[4] + delta.z * o[5] - centre.y;
                const float length2 = dx * dx + dy * dy;
                const float dot = dx * std::sin(angle) + dy * std::cos(angle);
                const float facing = length2 > 1e-8f ? std::clamp(dot * dot / length2, 0.f, 1.f) : 0;
                vertex_color = 0xffffffu | static_cast<unsigned>(std::clamp(alpha * facing, 0.f, 255.f))
                                               << 24;
            }
            mesh.vertices.push_back({{motion.position.x + local.x * o[0] + local.y * o[3] + local.z * o[6],
                                      motion.position.z + local.x * o[2] + local.y * o[5] + local.z * o[8],
                                      motion.position.y + local.x * o[1] + local.y * o[4] + local.z * o[7]},
                                     {},
                                     {u0 + u * su, v0 + (tornado ? t * 2 - (layer == 1 ? 1 : 0) : t) * sv},
                                     vertex_color});
        }
    }
    if (tornado && layer == 2)
        return mesh;
    for (unsigned row = 0; row < last_row - first_row; ++row)
        for (unsigned column = 0; column < columns; ++column) {
            const auto a = row * (columns + 1) + column, b = a + columns + 1;
            mesh.indices.insert(mesh.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
            mesh.triangle_materials.insert(mesh.triangle_materials.end(), 2, 0);
        }
    return mesh;
}
void SpellActor::initialize_spike_surface(Match& match, Vec3 half_extents) {
    surface.kind = CurvedSurface::rays;
    const float radius = std::sqrt(half_extents.x * half_extents.x + half_extents.y * half_extents.y +
                                   half_extents.z * half_extents.z);
    surface.parameters[0] = radius;
    surface.parameters[1] = radius + std::clamp(radius * .5f, .5f, 5.f);
    surface.parameters[2] = radius + std::clamp(radius * 2, 1.f, 10.f);
    constexpr Vec3 model[]{{0, 0, 0}, {0, 0, 1}, {.866f, 0, -.5f}, {-.866f, 0, -.5f}};
    for (unsigned n = 0; n < 24; ++n) {
        CurvedSurface::Ray ray;
        ray.phase = match.visual_random.next() * 6.28318f / 32768;
        ray.speed = match.visual_random.next() * .078539751f / 32768 + .026179917f;
        const float a = match.visual_random.next() * 6.28318f / 32768,
                    b = match.visual_random.next() * 6.28318f / 32768,
                    c = match.visual_random.next() * 6.28318f / 32768;
        const auto rotate = [&](Vec3 p) {
            const Vec3 x{p.x, p.y * std::cos(a) - p.z * std::sin(a), p.y * std::sin(a) + p.z * std::cos(a)};
            const Vec3 y{x.x * std::cos(b) + x.z * std::sin(b), x.y, -x.x * std::sin(b) + x.z * std::cos(b)};
            return Vec3{y.x * std::cos(c) - y.y * std::sin(c), y.x * std::sin(c) + y.y * std::cos(c), y.z};
        };
        ray.axis = rotate({0, 1, 0});
        for (unsigned i = 0; i < 4; ++i)
            ray.base[i] = rotate({model[i].x * radius * .5f, 0, model[i].z * radius * .5f});
        surface.rays_data.push_back(ray);
    }
}
void SpellActor::update_tornado_surface(int ticks) {
    surface.kind = CurvedSurface::tornado;
    surface.textures = {texture(*data, "ttx1", "torn"), texture(*data, "ttx2", "torn"),
                        texture(*data, "ttx3", "wwwl")};
    surface.frame += ticks * .25f;
    surface.opacity = stage >= 240 ? std::max(0.f, surface.opacity - ticks * .01f)
                                   : std::min(1.f, surface.opacity + ticks * .01f);
    auto& p = surface.parameters;
    p[0] = ability.origin.x - motion.position.x;
    p[1] = ability.origin.y - motion.position.y;
    p[2] = ability.origin.z + 500 - motion.position.z;
    p[3] = 300;
    p[4] = p[0] * (1 - surface.opacity);
    p[5] = p[1] * (1 - surface.opacity);
    p[6] = (ability.origin.z - motion.position.z) * (1 - surface.opacity);
    p[7] = 1.5f;
    p[8] += ticks * .1047196667f;
    constexpr float speeds[]{.01f, -.008f, -.013f, .006f};
    for (unsigned n = 9; n < 13; ++n)
        p[n] += ticks * speeds[n - 9];
}
void SpellActor::tornado_centre(float world_z, Vec2& centre, float& radius) const {
    std::array<Vec3, 7> centres{};
    std::array<float, 7> radii{};
    build_tornado_profile(surface.parameters, centres, radii);
    const float low_z = motion.position.z + centres.front().z;
    const float high_z = motion.position.z + centres.back().z;
    const float span = high_z - low_z;
    const float t = span != 0.f ? std::clamp((world_z - low_z) / span, 0.f, 1.f) : 1.f;
    const unsigned part = t <= .5f ? 0 : 3;
    const float u = t <= .5f ? t * 2 : (t - .5f) * 2;
    const auto local = curve({centres[part], centres[part + 1], centres[part + 2], centres[part + 3]}, u);
    radius = cubic(radii[part], radii[part + 1], radii[part + 2], radii[part + 3], u);
    centre = {motion.position.x + local.x, motion.position.y + local.y};
}
int SpellActor::run_soul_path(Match& match, int ticks) {
    auto* soul = dynamic_cast<SoulActor*>(effect_target);
    auto* caster = dynamic_cast<CreatureActor*>(owner);
    if (!stage) {
        if (!soul || !caster) {
            state_flags |= removed_flag;
            return -2;
        }
        surface.kind = CurvedSurface::lathe;
        surface.textures = {texture(*data, "ttx1", "swnd"), texture(*data, "ttx2", "swnd")};
        ability.origin = caster->motion.position;
        ability.origin.x += caster->motion.orientation[3] * 2;
        ability.origin.y += caster->motion.orientation[4] * 2;
        ability.origin.z += caster->motion.orientation[5] * 2 + 1.5f;
        motion.position = ability.origin;
        spell_target_point(point, soul);
        const float length = std::sqrt(static_cast<float>(std::pow(point.x - motion.position.x, 2) +
                                                          std::pow(point.y - motion.position.y, 2) +
                                                          std::pow(point.z - motion.position.z, 2))) *
                             .25f;
        ability.destination = {caster->motion.orientation[0] * length, caster->motion.orientation[1] * length,
                               caster->motion.orientation[2] * length};
        for (unsigned n = 0; n < 8; ++n)
            surface.phases[n] =
                static_cast<float>((match.random().next() * 6.2831802 / 32768 - (n >= 4 ? 3.1415901184 : 0)) *
                                   (n >= 4 ? 1.0 / 60 : 1));
        launch_ground_trail(match, *this, 0x0a011008, texture(*data, "ttrl", "dirt"), {-1.2f, -1.2f, -.1f},
                            {1.2f, 1.2f, .25f}, .75f, .75f);
        sound_event(match, 2);
        stage = 1;
    }
    if (!caster || (stage > 1 && !soul))
        stage = 240;
    if (stage == 1) {
        const auto [state, alpha] = caster->cast_state();
        cast_alpha = alpha;
        if (state)
            stage = state == 0x20000000 ? 16 : 240;
    }
    if (stage == 16) {
        cast_alpha = 1;
        spell_target_point(point, soul);
        if (age >= 1)
            stage = 32;
    } else if (stage == 32) {
        if (soul->soul_flags & 0xc0000000)
            stage = 240;
        else {
            if (auto* corpse = dynamic_cast<CreatureActor*>(soul->anchor)) {
                launch_corpse_fade(match, *corpse);
                corpse->state_flags |= removed_flag;
            }
            soul->anchor = nullptr;
            sound_event(match, 4);
            stage = 33;
        }
    } else if (stage == 33)
        stage = 48;
    if (stage < 240) {
        age = std::min(stage == 48 ? 2.f : 1.f, age + ticks * cast_alpha / 240.f);
        const bool returning = age > 1;
        const auto to = caster->motion.position;
        Vec3 a, b;
        if (!returning) {
            a = {ability.origin.x + (point.x - ability.origin.x) * .4f,
                 ability.origin.y + (point.y - ability.origin.y) * .4f,
                 ability.origin.z + (point.z - ability.origin.z) * .4f};
            b = {point.x + ability.destination.x, point.y + ability.destination.y,
                 point.z + ability.destination.z};
            ground(match, a);
            ground(match, b);
            motion.position = curve({ability.origin, a, b, point}, age);
        } else {
            a = {point.x - ability.destination.x, point.y - ability.destination.y,
                 point.z - ability.destination.z};
            b = {to.x + (point.x - to.x) * .4f, to.y + (point.y - to.y) * .4f, to.z + (point.z - to.z) * .4f};
            ground(match, a);
            ground(match, b);
            motion.position = curve({point, a, b, to}, age - 1);
        }
        ground(match, motion.position);
        if (stage == 48) {
            soul->motion.position = motion.position;
            soul->motion.position.z += 1;
            if (age >= 2)
                stage = 240;
        }
        if (stage >= 16 && age < 1.95f && (next_pulse -= ticks) <= 0) {
            next_pulse = 32;
            for (unsigned group = 0; group < 2; ++group)
                match.visit_group(group, 5, [&](MatchObject& object) {
                    auto* victim = dynamic_cast<CreatureActor*>(&object);
                    if (victim && victim != caster && victim->alive() && victim->enemy(this) &&
                        victim->named(spell_id) != this &&
                        spell_box_distance_squared(motion.position, victim->motion,
                                                   data->spread_limit * data->spread_limit) <=
                            data->spread_limit * data->spread_limit) {
                        victim->name_reference(spell_id, this);
                        auto& arc = launch_inert_lightning(match, *this, motion.position,
                                                           victim->motion.position, victim, 1.5f, .01f, .5f,
                                                           2, .5f, 1);
                        arc.sound_requests.push_back(
                            {FourCC::from_text("Xltn"), FourCC::from_text("Xltn"), arc.reference(), 0x40004});
                        victim->knock_down(0, 10);
                        spell_damage(match, this, victim, data->damage_type, data->amount, data->radius);
                    }
                    return 0;
                });
        }
    }
    surface.opacity = stage >= 240 ? std::max(0.f, surface.opacity - ticks * .05f)
                                   : std::min(1.f, surface.opacity + ticks * .05f);
    auto& p = surface.parameters;
    p[0] += ticks * .314159f;
    p[1] = 1;
    p[2] = 0;
    p[4] = 0;
    p[5] = .5f;
    p[26] = .5f;
    p[27] = 1;
    constexpr float z[]{0, .5f, 1, 1.5f, 1.5f, 2, 2.5f, 3}, radius[]{.1f, .2f, .7f, 1, 1, 1.3f, 1.6f, 2};
    for (unsigned n = 0; n < 8; ++n) {
        const auto at = 6 + (n / 4) * 22 + (n % 4) * 5;
        p[at + 2] = z[n];
        p[at + 3] = radius[n];
        p[at + 4] = (n == 7 ? 0.f : 255.f) * surface.opacity;
    }
    p[16] = std::sin(surface.phases[0]) * .5f;
    p[17] = std::sin(surface.phases[1]) * .5f;
    p[33] = std::sin(surface.phases[2]);
    p[34] = std::sin(surface.phases[3]);
    p[43] = std::sin(surface.phases[1] + surface.phases[2]) * .5f;
    p[44] = std::sin(surface.phases[3] + surface.phases[0]) * .5f;
    for (unsigned c = 0; c < 3; ++c) {
        p[21 + c] = (p[33 + c] - p[16 + c]) * .5f + p[16 + c];
        p[28 + c] = p[21 + c];
        p[11 + c] = (p[16 + c] - p[6 + c]) * .5f + p[6 + c];
        p[38 + c] = (p[43 + c] - p[33 + c]) * .5f + p[33 + c];
    }
    for (unsigned n = 0; n < 4; ++n)
        surface.phases[n] += ticks * surface.phases[n + 4];
    if (stage >= 240 && surface.opacity == 0) {
        state_flags |= removed_flag;
        return -2;
    }
    return 1;
}
} // namespace sacrifice
