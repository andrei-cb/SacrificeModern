#include "sacrifice/flora.hpp"
#include "sacrifice/scene.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
constexpr float pi = 3.14159265f;
Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 mul(Vec3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
Vec3 mul(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
Vec3 mix(Vec3 a, Vec3 b, float t) { return add(a, mul(sub(b, a), t)); }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(Vec3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
Vec3 normal(Vec3 a) {
    const float l = length(a);
    return l > 1e-6f ? mul(a, 1 / l) : Vec3{0, 0, 1};
}
Vec3 unpack(std::uint32_t c) { return {(c & 255) / 255.0f, (c >> 8 & 255) / 255.0f, (c >> 16 & 255) / 255.0f}; }
std::uint32_t pack(Vec3 c) {
    const auto byte = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255 + .5f); };
    return 0xff000000u | byte(c.x) | byte(c.y) << 8 | byte(c.z) << 16;
}
float luma(Vec3 c) { return c.x * .299f + c.y * .587f + c.z * .114f; }
Vec3 saturate_colour(Vec3 c, float k) {
    const float l = luma(c);
    return {std::max(0.0f, l + (c.x - l) * k), std::max(0.0f, l + (c.y - l) * k), std::max(0.0f, l + (c.z - l) * k)};
}
struct Random {
    std::uint32_t state;
    float operator()() {
        state ^= state >> 15;
        state *= 0x2c1b3c6du;
        state ^= state >> 12;
        state *= 0x297a2d39u;
        state ^= state >> 15;
        return (state & 0xffffff) / 16777215.0f;
    }
    float range(float low, float high) { return low + (high - low) * (*this)(); }
};
struct Builder {
    FloraModel& model;
    bool far{}; // The low-detail level (make_flora's `far`).
    std::uint32_t point(Vec3 p, Vec3 c, Vec3 n = {0, 0, 1}) {
        model.points.push_back({p, normal(n), pack(c)});
        return static_cast<std::uint32_t>(model.points.size() - 1);
    }
    void triangle(std::uint32_t a, std::uint32_t b, std::uint32_t c) { model.triangles.push_back({a, b, c}); }
    void quad(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
        triangle(a, b, c);
        triangle(a, c, d);
    }
    // A curved strip along `spine`, `widths` across `side` (the last width 0 makes a point), coloured c0 to c1,
    // with smooth outward/upward normals tilted gently across the width for a rounded highlight.
    void ribbon(const std::vector<Vec3>& spine, const std::vector<float>& widths, Vec3 side, Vec3 c0, Vec3 c1) {
        std::uint32_t left = 0, right = 0;
        for (std::size_t i = 0; i < spine.size(); ++i) {
            const float t = spine.size() > 1 ? static_cast<float>(i) / (spine.size() - 1) : 0;
            const auto colour = mix(c0, c1, t);
            const Vec3 tangent = i == 0 ? sub(spine[1], spine[0])
                               : i + 1 == spine.size() ? sub(spine[i], spine[i - 1])
                                                       : sub(spine[i + 1], spine[i - 1]);
            Vec3 n = cross(side, tangent);
            if (n.z < 0)
                n = mul(n, -1);
            n = normal(add(normal(n), Vec3{0, 0, .35f}));
            const float w = widths[i] * .5f;
            if (w <= 0) {
                const auto tip = point(spine[i], colour, n);
                if (i)
                    triangle(left, right, tip);
                return;
            }
            const auto nl = normal(sub(n, mul(side, .25f))), nr = normal(add(n, mul(side, .25f)));
            const auto l = point(sub(spine[i], mul(side, w)), colour, nl),
                       r = point(add(spine[i], mul(side, w)), colour, nr);
            if (i)
                quad(left, right, r, l);
            left = l;
            right = r;
        }
    }
    // A flat-shaded triangle with its true outward facet normal (sun lighting is done in the shader).
    void facet(Vec3 a, Vec3 b, Vec3 c, Vec3 colour, Vec3 center, float ao = 1.0f) {
        auto n = normal(cross(sub(b, a), sub(c, a)));
        const auto mid = mul(add(add(a, b), c), 1.0f / 3.0f);
        const auto out = sub(mid, center);
        if (n.x * out.x + n.y * out.y + n.z * out.z < 0)
            n = mul(n, -1);
        const auto shaded = mul(colour, ao * (.88f + .16f * std::max(0.0f, n.z)));
        triangle(point(a, shaded, n), point(b, shaded, n), point(c, shaded, n));
    }
};
// A gracefully arching blade or stem from `base`, rising `height` and leaning `lean` along `direction` (radians).
void blade(Builder& b, Vec3 base, float direction, float height, float lean, float width, Vec3 c0, Vec3 c1,
           unsigned segments = 3, float droop = .22f, float twist = 0.0f) {
    if (b.far) { // One triangle, a little wider so the clump keeps its weight.
        segments = 1;
        width *= 1.35f;
    }
    const Vec3 out{std::cos(direction), std::sin(direction), 0};
    const Vec3 side{std::cos(direction + pi * .5f + twist), std::sin(direction + pi * .5f + twist), 0};
    std::vector<Vec3> spine;
    std::vector<float> widths;
    const float arch_norm = 1.0f / (1.0f - droop);
    for (unsigned i = 0; i <= segments; ++i) {
        const float t = static_cast<float>(i) / segments;
        const float z = height * (t - droop * t * t) * arch_norm;
        spine.push_back(add(base, add(mul(out, lean * t * t), Vec3{0, 0, z})));
        widths.push_back(i == segments ? 0 : width * (1 - .75f * t * t));
    }
    b.ribbon(spine, widths, side, c0, c1);
}
void make_grass(Builder& b, const FloraStyle& s, Random& r) {
    const unsigned count = 9 + static_cast<unsigned>(r() * 5);
    for (unsigned i = 0; i < count; ++i) {
        const float a = r() * 2 * pi, d = r() * s.width * .28f;
        const bool inner = i < count / 3;
        const float h = s.height * (inner ? r.range(.5f, .82f) : r.range(.7f, 1.05f));
        const float lean = h * (inner ? r.range(.12f, .3f) : r.range(.28f, .62f));
        const float droop = inner ? r.range(.1f, .2f) : r.range(.22f, .38f);
        const float twist = r.range(-.45f, .45f);
        const auto colour = mul(s.leaf, r.range(.84f, 1.16f));
        blade(b, {std::cos(a) * d, std::sin(a) * d, -.03f}, a + r.range(-.45f, .45f), h, lean,
              std::max(.055f, s.height * .105f), mul(colour, .46f), add(mul(colour, 1.32f), Vec3{.05f, .05f, 0}), 3,
              droop, twist);
    }
}
void flower_head(Builder& b, Vec3 top, Vec3 tilt, float radius, Vec3 petal, Random& r) {
    const unsigned petals = 5 + static_cast<unsigned>(r() * 3);
    const Vec3 up = normal(add(Vec3{0, 0, 1}, mul(tilt, .35f)));
    const Vec3 bx = normal(cross(up, std::abs(up.x) < .8f ? Vec3{1, 0, 0} : Vec3{0, 1, 0}));
    const Vec3 by = cross(up, bx);
    const auto to_world = [&](float x, float y, float z) {
        return add(top, add(add(mul(bx, x), mul(by, y)), mul(up, z)));
    };
    const Vec3 bud_col = mix(mul(petal, .9f), {.98f, .78f, .18f}, .65f);
    const auto centre = b.point(to_world(0, 0, radius * .24f), add(bud_col, Vec3{.06f, .04f, 0}), up);
    const float turn = r() * 2 * pi;
    // Small 3D stamen dome in the center (near LOD) so blossoms read crisply from above.
    if (!b.far) {
        constexpr unsigned bud_sides = 5;
        std::array<std::uint32_t, bud_sides> ring{};
        for (unsigned i = 0; i < bud_sides; ++i) {
            const float a = turn + i * 2 * pi / bud_sides;
            const Vec3 dir = add(mul(bx, std::cos(a)), mul(by, std::sin(a)));
            ring[i] = b.point(to_world(std::cos(a) * radius * .22f, std::sin(a) * radius * .22f, radius * .06f),
                              mul(bud_col, .75f), normal(add(mul(up, .7f), mul(dir, .7f))));
        }
        for (unsigned i = 0; i < bud_sides; ++i)
            b.triangle(ring[i], ring[(i + 1) % bud_sides], centre);
    }
    const auto throat = b.point(to_world(0, 0, radius * .04f), mul(petal, .68f), up);
    for (unsigned i = 0; i < petals; ++i) {
        const float a = turn + i * 2 * pi / petals, half = pi / petals * .84f;
        const Vec3 dir = add(mul(bx, std::cos(a)), mul(by, std::sin(a)));
        const auto at = [&](float angle, float reach, float rise) {
            return to_world(std::cos(angle) * reach, std::sin(angle) * reach, rise);
        };
        const Vec3 mid_n = normal(add(mul(up, .82f), mul(dir, .35f)));
        const Vec3 tip_n = normal(add(mul(up, .65f), mul(dir, .6f)));
        const Vec3 col_mid = mul(petal, .92f);
        const Vec3 col_tip = add(mul(petal, 1.15f), Vec3{.04f, .04f, .04f});
        if (b.far) {
            const auto left = b.point(at(a - half, radius * .58f, radius * .18f), col_mid, mid_n);
            const auto tip = b.point(at(a, radius, radius * .24f), col_tip, tip_n);
            const auto right = b.point(at(a + half, radius * .58f, radius * .18f), col_mid, mid_n);
            b.quad(throat, left, tip, right);
        } else {
            // Two-segment cupped petal: rises into a gentle cup, then unfurls at the tip.
            const auto l1 = b.point(at(a - half * .72f, radius * .44f, radius * .22f), col_mid, mid_n);
            const auto r1 = b.point(at(a + half * .72f, radius * .44f, radius * .22f), col_mid, mid_n);
            const auto l2 = b.point(at(a - half * .55f, radius * .78f, radius * .28f), petal, mid_n);
            const auto r2 = b.point(at(a + half * .55f, radius * .78f, radius * .28f), petal, mid_n);
            const auto tip = b.point(at(a, radius * 1.04f, radius * .18f), col_tip, tip_n);
            b.triangle(throat, l1, r1);
            b.quad(l1, l2, r2, r1);
            b.triangle(l2, tip, r2);
        }
    }
}
void make_flower(Builder& b, const FloraStyle& s, Random& r) {
    // Wide flower widgets are single big blossoms; narrower ones a small clump.
    const bool blossom = s.width > .9f;
    const unsigned stems = blossom ? 1 + static_cast<unsigned>(r() * 2) : 3 + static_cast<unsigned>(r() * 3);
    const float head = blossom ? s.width * .32f : std::max(.09f, std::max(s.height * .26f, s.width * .2f));
    const auto stem = mul(s.leaf, .8f);
    for (unsigned i = 0; i < stems; ++i) {
        const float a = r() * 2 * pi, d = r() * s.width * (blossom ? .15f : .26f);
        const Vec3 base{std::cos(a) * d, std::sin(a) * d, -.03f};
        const float h = s.height * r.range(.58f, 1.02f), lean = h * r.range(.08f, .28f);
        const Vec3 out{std::cos(a), std::sin(a), 0};
        blade(b, base, a, h, lean, std::max(.032f, s.height * .055f), mul(stem, .55f), stem, 2, .12f);
        const Vec3 top = add(base, add(mul(out, lean), Vec3{0, 0, h}));
        flower_head(b, top, out, head * r.range(.82f, 1.2f), mul(s.accent, r.range(.9f, 1.12f)), r);
        for (int leaf = 0; leaf < 2; ++leaf) {
            const float la = r() * 2 * pi, lh = h * r.range(.28f, .52f), ll = lh * r.range(.65f, 1.1f);
            if (!b.far)
                blade(b, base, la, lh, ll, std::max(.075f, s.height * .145f), mul(s.leaf, .5f), mul(s.leaf, 1.15f), 2,
                      .3f);
        }
    }
}
void make_fern(Builder& b, const FloraStyle& s, Random& r) {
    const unsigned outer = 5 + static_cast<unsigned>(r() * 3);
    const unsigned inner = 2 + static_cast<unsigned>(r() * 2);
    const unsigned fronds = outer + inner;
    for (unsigned i = 0; i < fronds; ++i) {
        const bool young = i >= outer;
        const float a = (young ? (i - outer) * 2 * pi / inner : i * 2 * pi / outer) + r.range(-.32f, .32f);
        const Vec3 out{std::cos(a), std::sin(a), 0}, side{-out.y, out.x, 0};
        const float reach = s.width * (young ? r.range(.24f, .38f) : r.range(.45f, .68f));
        const float rise = s.height * (young ? r.range(.65f, .92f) : r.range(.72f, 1.05f));
        const auto colour = mul(s.leaf, (young ? 1.08f : 1.0f) * r.range(.86f, 1.14f));
        std::vector<Vec3> spine;
        for (int k = 0; k <= 5; ++k) {
            const float t = k / 5.0f;
            const float z = -.03f + rise * std::sin(t * pi * (young ? .62f : .76f));
            spine.push_back(add(mul(out, reach * t), Vec3{0, 0, z}));
        }
        if (b.far) { // One gracefully arched broad frond in place of the rachis and leaflets.
            const float w = s.width * (young ? .2f : .32f);
            b.ribbon({spine[0], spine[2], spine[4], spine[5]}, {w * .45f, w, w * .55f, 0}, side, mul(colour, .5f),
                     add(mul(colour, 1.15f), Vec3{.03f, .04f, 0}));
            continue;
        }
        b.ribbon(spine, {.042f, .035f, .028f, .022f, .016f, 0}, side, mul(colour, .48f), colour);
        for (int k = 1; k < 5; ++k) { // Paired lance-shaped pinnae with slight upward V-dihedral.
            const float t = k / 5.0f;
            const float envelope = std::sin(t * pi * .92f);
            const float size = reach * .42f * envelope;
            const auto seg = sub(spine[k + 1], spine[k]);
            const auto fwd = normal(seg);
            const auto base_n = normal(add(cross(side, fwd), Vec3{0, 0, .5f}));
            for (const float sign : {-1.0f, 1.0f}) {
                const auto root = spine[k];
                const auto leaf_dir = normal(add(mul(side, sign), add(mul(out, .48f), Vec3{0, 0, .14f})));
                const auto tip = add(root, mul(leaf_dir, size));
                const auto front = add(root, mul(seg, .78f));
                const auto mid_back = add(root, add(mul(leaf_dir, size * .48f), mul(fwd, -size * .14f)));
                const auto mid_front = add(root, add(mul(leaf_dir, size * .52f), mul(fwd, size * .22f)));
                const auto c = mix(mul(colour, .58f), mul(colour, 1.18f), t);
                const auto c_tip = add(mul(c, 1.08f), Vec3{.03f, .04f, 0});
                const auto n = normal(add(base_n, mul(side, sign * -.22f)));
                const auto p_root = b.point(root, mul(c, .85f), base_n);
                const auto p_mb = b.point(mid_back, c, n);
                const auto p_tip = b.point(tip, c_tip, n);
                const auto p_mf = b.point(mid_front, c, n);
                const auto p_fr = b.point(front, mul(c, .9f), base_n);
                b.quad(p_root, p_mb, p_tip, p_mf);
                b.triangle(p_root, p_mf, p_fr);
            }
        }
    }
}
void make_bush(Builder& b, const FloraStyle& s, Random& r) {
    // Organic lobed bush: 5 overlapping sub-lobes of multi-leaf sprays with spherical canopy normals so
    // directional sunlight and ambient occlusion give the shrub genuine 3D volume.
    const Vec3 centre{0, 0, s.height * .46f};
    std::array<Vec3, 5> lobes{centre};
    for (int l = 1; l < 5; ++l) {
        const float a = (l - 1) * pi * .5f + r.range(-.35f, .35f);
        lobes[l] = add(centre, Vec3{std::cos(a) * s.width * .24f, std::sin(a) * s.width * .24f,
                                    s.height * r.range(-.12f, .14f)});
    }
    const unsigned sprays = 26;
    for (unsigned i = 0; i < sprays; ++i) {
        const bool keep = !b.far || (i % 5 < 2);
        const auto& lobe = lobes[i % lobes.size()];
        const float a = r() * 2 * pi, z = r.range(-.35f, 1.0f), ring = std::sqrt(std::max(0.0f, 1 - z * z));
        const float depth = r.range(.58f, 1.0f);
        const float jitter_x = r.range(-.45f, .45f), jitter_y = r.range(-.45f, .45f), jitter_z = r.range(-.2f, .45f);
        const float sz_rand = r.range(.15f, .24f), col_rand = r.range(.86f, 1.14f);
        const float spin = r() * 2 * pi;
        if (!keep)
            continue;
        const Vec3 dir{std::cos(a) * ring, std::sin(a) * ring, z};
        const auto at = add(lobe, Vec3{dir.x * s.width * .36f * depth, dir.y * s.width * .36f * depth,
                                       dir.z * s.height * .36f * depth});
        const Vec3 sphere_n = normal(sub(at, Vec3{0, 0, s.height * .38f}));
        const float size = std::max(s.width, s.height) * sz_rand * (b.far ? 1.55f : 1.0f);
        // Canopy AO: deep cool shade inside and below, warm sunlit green on the outer crown.
        const float ao = (.46f + .56f * (sphere_n.z * .5f + .5f)) * (.62f + .38f * depth) * col_rand;
        const Vec3 base_col = mul(s.leaf, ao);
        const Vec3 tip_col = add(mul(base_col, 1.18f), Vec3{.03f, .035f, 0});
        // Each spray is 3 angled, tapered leaf blades fanning out from `at`.
        const unsigned leaves = b.far ? 2 : 3;
        const Vec3 axis = normal(add(sphere_n, Vec3{jitter_x, jitter_y, jitter_z}));
        const Vec3 t1 = normal(cross(axis, std::abs(axis.z) < .85f ? Vec3{0, 0, 1} : Vec3{1, 0, 0}));
        const Vec3 t2 = cross(axis, t1);
        for (unsigned k = 0; k < leaves; ++k) {
            const float la = spin + k * (2 * pi / leaves);
            const Vec3 leaf_out = normal(add(add(mul(t1, std::cos(la)), mul(t2, std::sin(la))), mul(axis, .45f)));
            const Vec3 leaf_side = normal(cross(axis, leaf_out));
            const Vec3 leaf_n = normal(add(mul(sphere_n, .75f), mul(axis, .25f)));
            const float l_len = size * (k == 0 ? 1.15f : .92f);
            const float l_wid = size * .46f;
            const auto p_base = b.point(sub(at, mul(leaf_out, l_len * .22f)), mul(base_col, .78f), leaf_n);
            const auto p_l = b.point(add(add(at, mul(leaf_out, l_len * .35f)), mul(leaf_side, -l_wid)), base_col, leaf_n);
            const auto p_r = b.point(add(add(at, mul(leaf_out, l_len * .35f)), mul(leaf_side, l_wid)), base_col, leaf_n);
            const auto p_tip = b.point(add(at, mul(leaf_out, l_len)), tip_col, leaf_n);
            b.quad(p_base, p_l, p_tip, p_r);
        }
    }
    // Branching woody stems anchoring the bush into the ground.
    for (int i = 0; i < (b.far ? 0 : 5); ++i) {
        const float a = i * 2 * pi / 5 + r.range(-.3f, .3f);
        const float h = s.height * r.range(.45f, .65f), lean = s.width * r.range(.15f, .28f);
        blade(b, {0, 0, -.05f}, a, h, lean, std::max(.065f, s.width * .055f), {.16f, .11f, .07f}, {.24f, .17f, .1f}, 2,
              .15f);
    }
}
void pebble(Builder& b, Vec3 at, float radius, float height, Vec3 colour, Random& r) {
    // Low, embedded, asymmetric fieldstone: widest at the ground so it emerges from soil/snow rather than
    // sitting on top like a ball, with smooth outward/upward normals instead of harsh flat soccer-ball facets.
    constexpr int sides = 8;
    const int step = b.far ? 2 : 1;
    const std::array<float, 3> levels{-.16f, .34f, .78f}, widths{1.15f, .9f, .52f};
    const std::array<float, 3> shades{.64f, .92f, 1.04f};
    std::array<std::array<std::uint32_t, sides>, 3> rings{};
    const float turn = r() * 2 * pi;
    const float squash = r.range(.56f, .84f);
    const Vec3 center = add(at, Vec3{0, 0, height * .18f});
    std::array<float, sides> radial{};
    for (int i = 0; i < sides; ++i)
        radial[i] = r.range(.76f, 1.22f);
    for (int k = 0; k < 3; ++k)
        for (int i = 0; i < sides; ++i) {
            const float a = turn + (i + k * .25f) * 2 * pi / sides;
            const float w = radius * widths[k] * radial[i] * r.range(.92f, 1.08f);
            const float z = height * levels[k] + (k > 0 ? height * r.range(-.06f, .06f) : 0.0f);
            const Vec3 pos = add(at, Vec3{std::cos(a) * w, std::sin(a) * w * squash, z});
            const Vec3 out = sub(pos, center);
            const Vec3 n = normal(Vec3{out.x, out.y, std::max(out.z, 0.0f) * 1.4f + radius * .25f});
            rings[k][i] = b.point(pos, mul(colour, shades[k]), n);
        }
    const Vec3 apex_pos = add(at, Vec3{r.range(-.18f, .18f) * radius, r.range(-.15f, .15f) * radius, height * .94f});
    const auto apex = b.point(apex_pos, mul(colour, 1.08f), {0, 0, 1});
    for (int k = 0; k < 2; ++k)
        for (int i = 0; i < sides; i += step) {
            const int j = (i + step) % sides;
            b.quad(rings[k][i], rings[k][j], rings[k + 1][j], rings[k + 1][i]);
        }
    for (int i = 0; i < sides; i += step)
        b.triangle(rings[2][i], rings[2][(i + step) % sides], apex);
}
void make_rock(Builder& b, const FloraStyle& s, Random& r) {
    const unsigned stones = 1 + static_cast<unsigned>(r() * 3);
    for (unsigned i = 0; i < stones; ++i) {
        const float a = r() * 2 * pi, d = i ? s.width * r.range(.22f, .46f) : 0;
        const float scale = i ? r.range(.32f, .58f) : 1.0f;
        pebble(b, {std::cos(a) * d, std::sin(a) * d, 0}, s.width * .36f * scale,
               std::max(.05f, s.height) * scale * r.range(.8f, 1.15f), mul(s.leaf, r.range(.86f, 1.12f)), r);
    }
}
void make_crystal(Builder& b, const FloraStyle& s, Random& r) {
    // Slender, tilted crystalline ice shards (Stratos SAW1-3) emerging obliquely from the snow.
    const unsigned shards = 1 + static_cast<unsigned>(r() * 3);
    const Vec3 base_col = mix(s.leaf, {.48f, .76f, .94f}, .45f);
    const Vec3 tip_col = add(mix(s.leaf, {.95f, .99f, 1.0f}, .65f), Vec3{.03f, .04f, .05f});
    for (unsigned i = 0; i < shards; ++i) {
        const float a = r() * 2 * pi, d = i ? s.width * r.range(.12f, .28f) : 0;
        const float scale = i ? r.range(.45f, .78f) : 1.0f;
        const float h = std::max(.25f, s.height) * scale * r.range(.85f, 1.25f);
        const float rad = std::max(.05f, h * r.range(.14f, .22f));
        const float tilt_a = i ? a + r.range(-.6f, .6f) : r() * 2 * pi;
        const float tilt = r.range(.28f, .62f);
        const Vec3 axis = normal(Vec3{std::cos(tilt_a) * tilt, std::sin(tilt_a) * tilt, 1.0f});
        const Vec3 bx = normal(cross(axis, std::abs(axis.x) < .8f ? Vec3{1, 0, 0} : Vec3{0, 1, 0}));
        const Vec3 by = cross(axis, bx);
        const Vec3 root{std::cos(a) * d, std::sin(a) * d, -.08f};
        const float spin = r() * 2 * pi;
        constexpr int sides = 5;
        const int step = b.far ? 2 : 1;
        std::array<Vec3, sides> bot{}, mid{};
        for (int k = 0; k < sides; ++k) {
            const float ca = std::cos(spin + k * 2 * pi / sides), sa = std::sin(spin + k * 2 * pi / sides);
            const Vec3 dir = add(mul(bx, ca), mul(by, sa));
            const float w = rad * r.range(.82f, 1.18f);
            bot[k] = add(root, mul(dir, w * 1.15f));
            mid[k] = add(root, add(mul(axis, h * .58f), mul(dir, w * .68f)));
        }
        const Vec3 tip = add(root, mul(axis, h));
        const Vec3 center = add(root, mul(axis, h * .4f));
        const Vec3 col_lo = mul(base_col, r.range(.9f, 1.08f));
        const Vec3 col_hi = mul(tip_col, r.range(.95f, 1.05f));
        for (int k = 0; k < sides; k += step) {
            const int j = std::min(k + step, sides) % sides;
            b.facet(bot[k], bot[j], mid[k], col_lo, center, .92f);
            b.facet(bot[j], mid[j], mid[k], mix(col_lo, col_hi, .35f), center, .98f);
            b.facet(mid[k], mid[j], tip, col_hi, center, 1.05f);
        }
    }
}
void make_twig(Builder& b, const FloraStyle& s, Random& r) {
    const unsigned sticks = 3 + static_cast<unsigned>(r() * 2);
    for (unsigned i = 0; i < sticks; ++i) {
        const float a = r() * 2 * pi, h = s.height * r.range(.6f, 1.0f), lean = s.width * r.range(.18f, .48f);
        const auto colour = mul(s.leaf, r.range(.8f, 1.15f));
        const float width = std::max(.038f, s.height * .048f);
        blade(b, {0, 0, -.03f}, a, h, lean, width, mul(colour, .55f), colour, 3, .18f);
        const float t = r.range(.35f, .62f); // Branch from partway up.
        const Vec3 from{std::cos(a) * lean * t * t, std::sin(a) * lean * t * t, -.03f + h * (t - .18f * t * t) / .82f};
        const float turn = r.range(.6f, 1.25f) * (r() < .5f ? -1 : 1);
        if (!b.far)
            blade(b, from, a + turn, h * .42f, h * .32f, width * .6f, colour, mul(colour, 1.12f), 2, .2f);
    }
}
void make_mushroom(Builder& b, const FloraStyle& s, Random& r) {
    const unsigned caps = 1 + static_cast<unsigned>(r() * 3);
    for (unsigned m = 0; m < caps; ++m) {
        const float a = r() * 2 * pi, d = m ? s.width * r.range(.22f, .44f) : 0;
        const float scale = m ? r.range(.42f, .78f) : 1.0f, h = s.height * scale;
        const Vec3 at{std::cos(a) * d, std::sin(a) * d, 0};
        // Organic curved/leaning stem so mushrooms don't stand rigidly vertical.
        const float lean_a = m ? a + r.range(-.5f, .5f) : r() * 2 * pi;
        const float lean = h * (m ? r.range(.14f, .32f) : r.range(.05f, .16f));
        const Vec3 out{std::cos(lean_a), std::sin(lean_a), 0};
        const Vec3 up = normal(add(Vec3{0, 0, 1}, mul(out, lean / std::max(h, .1f) * 1.1f)));
        const Vec3 bx = normal(cross(up, std::abs(up.x) < .8f ? Vec3{1, 0, 0} : Vec3{0, 1, 0}));
        const Vec3 by = cross(up, bx);
        constexpr int sides = 8;
        const int step = b.far ? 2 : 1; // Far: a coarser cap.
        const float stem = h * .13f, cap = h * .44f * r.range(.88f, 1.15f);
        const Vec3 cap_base = add(at, add(mul(out, lean), Vec3{0, 0, h * .62f}));
        const Vec3 stem_mid = add(at, add(mul(out, lean * .35f), Vec3{0, 0, h * .3f}));
        const Vec3 cap_center = add(cap_base, mul(up, h * .08f));
        std::array<std::uint32_t, sides> low{}, mid{}, high{}, rim{}, crown{};
        const Vec3 white{.86f, .81f, .7f};
        const Vec3 cap_col = mul(s.accent, r.range(.88f, 1.12f));
        const Vec3 spot_col = mix(cap_col, {.95f, .9f, .78f}, .32f);
        for (int i = 0; i < sides; ++i) {
            const float t = i * 2 * pi / sides, c = std::cos(t), sn = std::sin(t);
            const Vec3 rad_xy{c, sn, 0};
            const Vec3 rad = add(mul(bx, c), mul(by, sn));
            const float wave = (i & 1 ? 1.0f : -1.0f) * h * .025f;
            low[i] = b.point(add(at, Vec3{c * stem * 1.25f, sn * stem * 1.25f, -.04f}), mul(white, .52f), rad_xy);
            mid[i] = b.point(add(stem_mid, Vec3{c * stem * .95f, sn * stem * .95f, 0}), mul(white, .82f), rad_xy);
            high[i] = b.point(add(cap_base, mul(rad, stem * .78f)), white, rad);
            const Vec3 rim_p = add(cap_base, add(mul(rad, cap), mul(up, -h * .06f + wave)));
            const Vec3 crown_p = add(cap_base, add(mul(rad, cap * .64f), mul(up, h * .26f)));
            rim[i] = b.point(rim_p, mul(cap_col, .72f), normal(sub(rim_p, cap_center)));
            crown[i] = b.point(crown_p, (i % 3 == 0) ? spot_col : cap_col, normal(sub(crown_p, cap_center)));
        }
        const auto top = b.point(add(cap_base, mul(up, h * .38f)), add(mul(cap_col, 1.12f), Vec3{.05f, .04f, .03f}), up);
        const auto gills = b.point(add(cap_base, mul(up, h * .02f)), mul(white, .62f), mul(up, -1));
        for (int i = 0; i < sides; i += step) {
            const int j = (i + step) % sides;
            if (!b.far) {
                b.quad(low[i], low[j], mid[j], mid[i]);
                b.quad(mid[i], mid[j], high[j], high[i]);
            } else {
                b.quad(low[i], low[j], high[j], high[i]);
            }
            b.quad(rim[i], rim[j], crown[j], crown[i]);
            b.triangle(crown[i], crown[j], top);
            b.triangle(rim[j], rim[i], gills);
        }
    }
}
} // namespace

FloraStyle classify_widget(const WidgetModel& widget, const Image& texture) {
    FloraStyle style;
    Vec3 low = widget.points.front().position, high = low;
    for (const auto& point : widget.points) {
        const auto p = point.position;
        low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    }
    style.width = std::max({high.x - low.x, high.y - low.y, .1f});
    style.height = std::max(high.z - low.z, .05f);
    // The texture's opaque texels over the triangles the widget uses (draws flip V, as WIDGETMODEL::Load).
    std::vector<Vec3> texels;
    if (texture.width && texture.height && texture.rgba.size() >= std::size_t{texture.width} * texture.height)
        for (const auto& t : widget.triangles) {
            if (t[0] >= widget.points.size() || t[1] >= widget.points.size() || t[2] >= widget.points.size())
                continue;
            const auto a = widget.points[t[0]].uv, b = widget.points[t[1]].uv, c = widget.points[t[2]].uv;
            constexpr int steps = 8;
            for (int i = 0; i <= steps; ++i)
                for (int j = 0; i + j <= steps; ++j) {
                    const float u = static_cast<float>(i) / steps, v = static_cast<float>(j) / steps, w = 1 - u - v;
                    const float x = a.x * w + b.x * u + c.x * v, y = 1 - (a.y * w + b.y * u + c.y * v);
                    const auto tx = static_cast<unsigned>(std::clamp(x - std::floor(x), 0.0f, .9999f) * texture.width),
                               ty = static_cast<unsigned>(std::clamp(y - std::floor(y), 0.0f, .9999f) * texture.height);
                    const auto texel = texture.rgba[std::size_t{ty} * texture.width + tx];
                    if ((texel >> 24) >= 128)
                        texels.push_back(unpack(texel));
                }
        }
    Vec3 average{.35f, .4f, .25f};
    if (!texels.empty()) {
        Vec3 sum{};
        for (const auto& c : texels)
            sum = add(sum, c);
        average = mul(sum, 1.0f / texels.size());
    }
    // Petals: texels far from the average that are saturated, not green and of another hue, or white on a
    // dark plant.
    const auto hue = [](Vec3 c) { return std::atan2(1.7320508f * (c.y - c.z), 2 * c.x - c.y - c.z); };
    const float average_top = std::max({average.x, average.y, average.z}),
                average_chroma = average_top - std::min({average.x, average.y, average.z});
    const bool tinted = average_chroma > .2f * average_top;
    Vec3 petals{}, rest{};
    float accents = 0;
    for (const auto& c : texels) {
        const float top = std::max({c.x, c.y, c.z}), bottom = std::min({c.x, c.y, c.z});
        const bool greenish = c.y >= c.x - .02f && c.y >= c.z;
        float turn = std::abs(hue(c) - hue(average));
        turn = std::min(turn, 2 * pi - turn);
        const bool vivid = top - bottom > .35f * top && top > .35f && !greenish && (!tinted || turn > .7f);
        const bool white = luma(c) > .75f && luma(average) < .45f;
        if (length(sub(c, average)) > .28f && (vivid || white)) {
            petals = add(petals, c);
            ++accents;
        } else
            rest = add(rest, c);
    }
    const float share = texels.empty() ? 0 : accents / texels.size();
    // Widget textures are dark (the original lights them by anchor); bring the colours up to ground level.
    const auto bright = [](Vec3 c, float level) {
        c = {std::max(c.x, 0.0f), std::max(c.y, 0.0f), std::max(c.z, 0.0f)};
        const float top = std::max({c.x, c.y, c.z, 1e-3f});
        return top < level ? mul(c, std::min(level / top, 3.5f)) : c;
    };
    const float top = std::max({average.x, average.y, average.z}), bottom = std::min({average.x, average.y, average.z});
    const bool green = average.y > average.x + .01f && average.y > average.z;
    const bool warm = !green && top - bottom > .3f * top;
    style.leaf = bright(saturate_colour(average, 1.15f), .5f);
    if (style.height < .4f * style.width)
        style.kind = green ? FloraKind::fern : FloraKind::rock;
    else if (style.width >= 1.4f)
        style.kind = FloraKind::bush;
    else if (share > .06f && share < .45f && style.height > .2f) {
        style.kind = FloraKind::flower;
        style.accent = bright(saturate_colour(mul(petals, 1 / accents), 1.2f), .75f);
        style.leaf = bright(saturate_colour(mul(rest, 1 / (texels.size() - accents)), 1.15f), .45f);
    } else if (style.height > 1.2f * style.width)
        style.kind = FloraKind::grass;
    else if (green)
        style.kind = FloraKind::fern;
    else if (warm && style.height <= .9f) { // Small warm-coloured growths: toadstools in the plant's colour.
        style.kind = FloraKind::mushroom;
        style.accent = bright(saturate_colour(average, 1.3f), .6f);
    } else
        style.kind = FloraKind::twig;
    if (style.kind == FloraKind::fern && style.height < .4f * style.width)
        style.height = std::max(style.height, style.width * .35f); // Flat leafy widgets: low ferns.
    return style;
}
FloraStyle widget_style(const std::string& id, const WidgetModel& widget, const Image& texture) {
    auto style = classify_widget(widget, texture);
    struct Known {
        const char* id;
        FloraKind kind;
        Vec3 leaf, accent;
    };
    using enum FloraKind;
    // Persephone (P*), Charnel (C*), James (J*), Pyro (PY*) and Stratos (S*) widgets, by what their textures
    // show: flower heads, toadstool caps, clover and broad leaves, grass strips, bush sprites, leaf litter,
    // ash and ice.
    static constexpr Known known[]{
        {"PCC1", fern, {.22f, .42f, .14f}, {}},           {"PCC2", fern, {.22f, .42f, .14f}, {}},
        {"PCF1", fern, {.3f, .5f, .18f}, {}},             {"PCF2", flower, {.25f, .45f, .16f}, {.62f, .6f, .95f}},
        {"PCF3", flower, {.25f, .45f, .16f}, {.98f, .78f, .15f}},
        {"PCF4", mushroom, {}, {.85f, .3f, .25f}},       {"PCG1", grass, {.3f, .5f, .15f}, {}},
        {"PCG2", grass, {.3f, .5f, .15f}, {}},            {"PCW5", rock, {.45f, .36f, .25f}, {}},
        {"PCW6", rock, {.38f, .4f, .42f}, {}},            {"PCW7", fern, {.3f, .52f, .18f}, {}},
        {"PCW8", fern, {.25f, .4f, .18f}, {}},            {"PGL1", grass, {.3f, .48f, .15f}, {}},
        {"PGL2", grass, {.32f, .46f, .16f}, {}},           {"CAC1", mushroom, {}, {.78f, .45f, .15f}},
        {"CAC2", mushroom, {}, {.62f, .4f, .4f}},         {"CAC3", mushroom, {}, {.62f, .4f, .4f}},
        {"CAC4", mushroom, {}, {.62f, .4f, .4f}},         {"CAF1", flower, {.3f, .18f, .12f}, {.85f, .15f, .12f}},
        {"CAF2", flower, {.3f, .18f, .12f}, {.85f, .15f, .12f}},
        {"CAF3", flower, {.3f, .18f, .12f}, {.9f, .2f, .1f}},
        {"CAF4", flower, {.3f, .18f, .12f}, {.8f, .12f, .3f}},
        {"CAH1", grass, {.3f, .17f, .13f}, {}},          {"CAH2", grass, {.3f, .17f, .13f}, {}},
        {"CAH3", grass, {.3f, .17f, .13f}, {}},            {"CAP1", rock, {.55f, .3f, .28f}, {}},
        {"CPL1", mushroom, {}, {.85f, .4f, .15f}},        {"CPL2", mushroom, {}, {.7f, .35f, .45f}},
        {"CPL3", mushroom, {}, {.85f, .4f, .15f}},        {"JAW1", bush, {.3f, .42f, .14f}, {}},
        {"JAW2", bush, {.3f, .42f, .14f}, {}},            {"JAW3", bush, {.3f, .42f, .14f}, {}},
        {"JAW4", bush, {.3f, .42f, .14f}, {}},            {"JAW5", bush, {.3f, .42f, .14f}, {}},
        {"PYA1", rock, {.2f, .19f, .19f}, {}},            {"PYA2", rock, {.2f, .19f, .19f}, {}},
        {"PYA3", rock, {.2f, .19f, .19f}, {}},            {"PYA4", rock, {.2f, .19f, .19f}, {}},
        {"SAW1", crystal, {.76f, .9f, .98f}, {}},         {"SAW2", crystal, {.76f, .9f, .98f}, {}},
        {"SAW3", crystal, {.76f, .9f, .98f}, {}},         {"SAW6", flower, {.4f, .62f, .72f}, {.45f, .85f, 1.0f}},
        {"SAW7", flower, {.4f, .62f, .72f}, {.45f, .85f, 1.0f}},
        {"SAW8", flower, {.4f, .62f, .72f}, {.55f, .5f, 1.0f}},
        {"SAW9", flower, {.4f, .62f, .72f}, {.55f, .5f, 1.0f}},
    };
    for (const auto& entry : known)
        if (id == entry.id) {
            style.kind = entry.kind;
            style.leaf = entry.leaf;
            style.accent = entry.accent;
            break;
        }
    // The widgets lie low (flat sprites); give the new models some body.
    if (style.kind == FloraKind::flower)
        style.height = std::max(style.height, .4f);
    else if (style.kind == FloraKind::mushroom) {
        style.height = std::clamp(style.height, .3f, style.width * .9f);
        style.width = std::max(style.width, .3f);
    } else if (style.kind == FloraKind::rock)
        style.height = std::clamp(style.height, .08f, style.width * .3f);
    else if (style.kind == FloraKind::crystal)
        style.height = std::clamp(style.height, .3f, std::max(.6f, style.width * .9f));
    else if (style.kind == FloraKind::fern)
        style.height = std::max(style.height, style.width * .35f);
    return style;
}
std::vector<FloraModel> make_flora(const FloraStyle& style, std::uint32_t seed, unsigned variants, bool far) {
    std::vector<FloraModel> models(std::max(variants, 1u));
    for (unsigned i = 0; i < models.size(); ++i) {
        Random random{seed * 0x9e3779b9u ^ (i + 1) * 0x85ebca6bu ^ 0x5bd1e995u};
        random();
        Builder builder{models[i], far};
        switch (style.kind) {
        case FloraKind::grass: make_grass(builder, style, random); break;
        case FloraKind::flower: make_flower(builder, style, random); break;
        case FloraKind::fern: make_fern(builder, style, random); break;
        case FloraKind::bush: make_bush(builder, style, random); break;
        case FloraKind::rock: make_rock(builder, style, random); break;
        case FloraKind::crystal: make_crystal(builder, style, random); break;
        case FloraKind::twig: make_twig(builder, style, random); break;
        case FloraKind::mushroom: make_mushroom(builder, style, random); break;
        }
        models[i].rigid = style.kind == FloraKind::rock || style.kind == FloraKind::crystal;
    }
    return models;
}
const std::vector<FloraModel>& clutter::models() {
    static const auto library = [] {
        std::vector<FloraModel> all;
        const auto append = [&](FloraStyle style, std::uint32_t seed, unsigned count) {
            auto models = make_flora(style, seed, count);
            all.insert(all.end(), models.begin(), models.end());
        };
        const std::array<Vec3, 4> petals{Vec3{.92f, .92f, .88f}, Vec3{.98f, .82f, .18f}, Vec3{.62f, .38f, .9f},
                                         Vec3{.9f, .22f, .2f}};
        for (unsigned i = 0; i < petals.size(); ++i)
            append({FloraKind::flower, {.25f, .5f, .18f}, petals[i], .45f, .42f}, 100 + i, 2);
        append({FloraKind::mushroom, {}, {.75f, .16f, .12f}, .4f, .32f}, 200, 1);
        append({FloraKind::mushroom, {}, {.55f, .38f, .22f}, .4f, .3f}, 201, 1);
        append({FloraKind::rock, {.55f, .53f, .5f}, {}, .38f, .12f}, 300, 3);
        append({FloraKind::twig, {.36f, .26f, .16f}, {}, .5f, .35f}, 400, 2);
        return all;
    }();
    return library;
}
} // namespace sacrifice
