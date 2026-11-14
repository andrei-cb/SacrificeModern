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
    std::uint32_t point(Vec3 p, Vec3 c) {
        model.points.push_back({p, pack(c)});
        return static_cast<std::uint32_t>(model.points.size() - 1);
    }
    void triangle(std::uint32_t a, std::uint32_t b, std::uint32_t c) { model.triangles.push_back({a, b, c}); }
    void quad(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
        triangle(a, b, c);
        triangle(a, c, d);
    }
    // A flat strip along `spine`, `widths` across `side` (the last width 0 makes a point), coloured c0 to c1.
    void ribbon(const std::vector<Vec3>& spine, const std::vector<float>& widths, Vec3 side, Vec3 c0, Vec3 c1) {
        std::uint32_t left = 0, right = 0;
        for (std::size_t i = 0; i < spine.size(); ++i) {
            const float t = spine.size() > 1 ? static_cast<float>(i) / (spine.size() - 1) : 0;
            const auto colour = mix(c0, c1, t);
            const float w = widths[i] * .5f;
            if (w <= 0) {
                const auto tip = point(spine[i], colour);
                if (i)
                    triangle(left, right, tip);
                return;
            }
            const auto l = point(sub(spine[i], mul(side, w)), colour), r = point(add(spine[i], mul(side, w)), colour);
            if (i)
                quad(left, right, r, l);
            left = l;
            right = r;
        }
    }
    // A flat-shaded triangle, lit from above (faceted stone).
    void facet(Vec3 a, Vec3 b, Vec3 c, Vec3 colour) {
        auto n = normal(cross(sub(b, a), sub(c, a)));
        if (n.z < 0)
            n = mul(n, -1);
        const auto lit = mul(colour, .5f + .45f * n.z + .12f * n.x);
        triangle(point(a, lit), point(b, lit), point(c, lit));
    }
};
// A curved blade or stem from `base`, rising `height` and leaning `lean` along `direction` (radians).
void blade(Builder& b, Vec3 base, float direction, float height, float lean, float width, Vec3 c0, Vec3 c1,
           unsigned segments = 3) {
    if (b.far) { // One triangle, a little wider so the clump keeps its weight.
        segments = 1;
        width *= 1.3f;
    }
    const Vec3 out{std::cos(direction), std::sin(direction), 0};
    const Vec3 side{-out.y, out.x, 0};
    std::vector<Vec3> spine;
    std::vector<float> widths;
    for (unsigned i = 0; i <= segments; ++i) {
        const float t = static_cast<float>(i) / segments;
        spine.push_back(add(base, add(mul(out, lean * t * t), Vec3{0, 0, height * t})));
        widths.push_back(i == segments ? 0 : width * (1 - .8f * t));
    }
    b.ribbon(spine, widths, side, c0, c1);
}
void make_grass(Builder& b, const FloraStyle& s, Random& r) {
    const unsigned count = 7 + static_cast<unsigned>(r() * 5);
    for (unsigned i = 0; i < count; ++i) {
        const float a = r() * 2 * pi, d = r() * s.width * .2f;
        const float h = s.height * r.range(.55f, 1.0f);
        const auto colour = mul(s.leaf, r.range(.85f, 1.15f));
        blade(b, {std::cos(a) * d, std::sin(a) * d, 0}, a + r.range(-.5f, .5f), h, h * r.range(.15f, .45f),
              std::max(.05f, s.height * .09f), mul(colour, .5f), add(mul(colour, 1.25f), Vec3{.04f, .04f, 0}));
    }
}
void flower_head(Builder& b, Vec3 top, float radius, Vec3 petal, Random& r) {
    const unsigned petals = 5 + static_cast<unsigned>(r() * 2);
    const auto centre = b.point(add(top, Vec3{0, 0, radius * .15f}), mix(mul(petal, 1.2f), {.95f, .8f, .3f}, .45f));
    const float turn = r() * 2 * pi;
    for (unsigned i = 0; i < petals; ++i) {
        const float a = turn + i * 2 * pi / petals, half = pi / petals * .8f;
        const auto at = [&](float angle, float reach, float rise) {
            return add(top, Vec3{std::cos(angle) * reach, std::sin(angle) * reach, rise});
        };
        const auto left = b.point(at(a - half, radius * .55f, radius * .1f), mul(petal, .8f));
        const auto tip = b.point(at(a, radius, radius * .35f), add(mul(petal, 1.1f), Vec3{.05f, .05f, .05f}));
        const auto right = b.point(at(a + half, radius * .55f, radius * .1f), mul(petal, .8f));
        b.quad(centre, left, tip, right);
    }
}
void make_flower(Builder& b, const FloraStyle& s, Random& r) {
    // Wide flower widgets are single big blossoms; narrower ones a small clump.
    const bool blossom = s.width > .9f;
    const unsigned stems = blossom ? 1 + static_cast<unsigned>(r() * 2) : 3 + static_cast<unsigned>(r() * 3);
    const float head = blossom ? s.width * .3f : std::max(.08f, std::max(s.height * .24f, s.width * .18f));
    const auto stem = mul(s.leaf, .8f);
    for (unsigned i = 0; i < stems; ++i) {
        const float a = r() * 2 * pi, d = r() * s.width * (blossom ? .15f : .25f);
        const Vec3 base{std::cos(a) * d, std::sin(a) * d, 0};
        const float h = s.height * r.range(.6f, 1.0f), lean = h * r.range(.05f, .25f);
        blade(b, base, a, h, lean, std::max(.03f, s.height * .05f), mul(stem, .6f), stem, 2);
        const Vec3 top = add(base, Vec3{std::cos(a) * lean, std::sin(a) * lean, h});
        flower_head(b, top, head * r.range(.8f, 1.2f), mul(s.accent, r.range(.9f, 1.1f)), r);
        for (int leaf = 0; leaf < 2; ++leaf) {
            const float la = r() * 2 * pi, lh = h * r.range(.25f, .45f), ll = lh * r.range(.6f, 1.0f);
            if (!b.far)
                blade(b, base, la, lh, ll, std::max(.06f, s.height * .12f), mul(s.leaf, .55f), mul(s.leaf, 1.1f), 2);
        }
    }
}
void make_fern(Builder& b, const FloraStyle& s, Random& r) {
    const unsigned fronds = 6 + static_cast<unsigned>(r() * 3);
    for (unsigned i = 0; i < fronds; ++i) {
        const float a = i * 2 * pi / fronds + r.range(-.3f, .3f);
        const Vec3 out{std::cos(a), std::sin(a), 0}, side{-out.y, out.x, 0};
        const float reach = s.width * r.range(.4f, .6f), rise = s.height * r.range(.7f, 1.0f);
        const auto colour = mul(s.leaf, r.range(.85f, 1.15f));
        std::vector<Vec3> spine;
        for (int k = 0; k <= 5; ++k) {
            const float t = k / 5.0f;
            spine.push_back(add(mul(out, reach * t), Vec3{0, 0, rise * std::sin(t * pi * .75f) * (1 - .35f * t)}));
        }
        if (b.far) { // One broad leaf in place of the frond and its leaflets.
            const float w = s.width * .3f;
            b.ribbon({spine[0], spine[2], spine[4], spine[5]}, {w * .5f, w, w * .6f, 0}, side, mul(colour, .55f),
                     mix(mul(colour, .6f), mul(colour, 1.15f), .7f));
            continue;
        }
        b.ribbon(spine, {.04f, .035f, .03f, .025f, .02f, 0}, side, mul(colour, .5f), colour);
        for (int k = 1; k < 5; ++k) { // Leaflets in pairs.
            const float t = k / 5.0f, size = s.width * .22f * (1 - t * .6f);
            for (const float sign : {-1.0f, 1.0f}) {
                const auto root = spine[k];
                const auto tip = add(root, add(mul(side, sign * size), add(mul(out, size * .5f), Vec3{0, 0, -size * .2f})));
                const auto front = add(root, mul(sub(spine[k + 1], root), .7f));
                const auto c = mix(mul(colour, .6f), mul(colour, 1.15f), t);
                b.triangle(b.point(root, c), b.point(tip, add(c, Vec3{.03f, .04f, 0})), b.point(front, c));
            }
        }
    }
}
void make_bush(Builder& b, const FloraStyle& s, Random& r) {
    const unsigned cards = 46;
    const Vec3 centre{0, 0, s.height * .45f};
    for (unsigned i = 0; i < cards; ++i) {
        if (b.far && i % 5 >= 2) { // Two cards in five, bigger; the same random draws keep the shape.
            for (int k = 0; k < 8; ++k)
                r();
            continue;
        }
        const float a = r() * 2 * pi, z = r.range(-.35f, 1.0f), ring = std::sqrt(std::max(0.0f, 1 - z * z));
        const float depth = r.range(.55f, 1.0f);
        const Vec3 dir{std::cos(a) * ring, std::sin(a) * ring, z};
        const auto at = add(centre, Vec3{dir.x * s.width * .5f * depth, dir.y * s.width * .5f * depth,
                                         dir.z * s.height * .5f * depth});
        auto n = normal(add(dir, Vec3{r.range(-.6f, .6f), r.range(-.6f, .6f), r.range(-.3f, .6f)}));
        const auto u = normal(cross(n, std::abs(n.z) < .9f ? Vec3{0, 0, 1} : Vec3{1, 0, 0}));
        const auto v = cross(n, u);
        const float size = std::max(s.width, s.height) * r.range(.13f, .22f) * (b.far ? 1.55f : 1.0f);
        const auto colour = mul(s.leaf, (.45f + .6f * (z * .5f + .5f)) * (.55f + .45f * depth) * r.range(.85f, 1.15f));
        const auto p0 = b.point(add(at, mul(u, size)), colour), p1 = b.point(add(at, mul(v, size * .6f)), mul(colour, 1.1f)),
                   p2 = b.point(sub(at, mul(u, size)), colour), p3 = b.point(sub(at, mul(v, size * .6f)), mul(colour, .85f));
        b.quad(p0, p1, p2, p3);
    }
    // A few dark stems so the base doesn't float.
    for (int i = 0; i < (b.far ? 0 : 3); ++i) {
        const float a = r() * 2 * pi;
        blade(b, {0, 0, 0}, a, s.height * .5f, s.width * .15f, std::max(.06f, s.width * .05f), {.18f, .13f, .08f},
              {.25f, .18f, .1f}, 2);
    }
}
void pebble(Builder& b, Vec3 at, float radius, float height, Vec3 colour, Random& r) {
    constexpr int sides = 6;
    const int step = b.far ? 2 : 1; // Far: every other side.
    const std::array<float, 3> levels{-.05f, .45f, .85f}, widths{.85f, 1.0f, .55f};
    std::array<std::array<Vec3, sides>, 3> rings;
    const float turn = r() * 2 * pi;
    for (int k = 0; k < 3; ++k)
        for (int i = 0; i < sides; ++i) {
            const float a = turn + (i + k * .5f) * 2 * pi / sides, w = radius * widths[k] * r.range(.8f, 1.15f);
            rings[k][i] = add(at, Vec3{std::cos(a) * w, std::sin(a) * w * .8f, height * levels[k]});
        }
    const auto apex = add(at, Vec3{r.range(-.1f, .1f) * radius, 0, height});
    for (int k = 0; k < 2; ++k)
        for (int i = 0; i < sides; i += step) {
            const int j = (i + step) % sides;
            b.facet(rings[k][i], rings[k][j], rings[k + 1][i], colour);
            b.facet(rings[k][j], rings[k + 1][j], rings[k + 1][i], colour);
        }
    for (int i = 0; i < sides; i += step)
        b.facet(rings[2][i], rings[2][(i + step) % sides], apex, colour);
}
void make_rock(Builder& b, const FloraStyle& s, Random& r) {
    const unsigned stones = 1 + static_cast<unsigned>(r() * 3);
    for (unsigned i = 0; i < stones; ++i) {
        const float a = r() * 2 * pi, d = i ? s.width * r.range(.2f, .4f) : 0;
        const float scale = i ? r.range(.35f, .6f) : 1.0f;
        pebble(b, {std::cos(a) * d, std::sin(a) * d, 0}, s.width * .38f * scale,
               std::max(.05f, s.height) * scale * r.range(.8f, 1.2f), mul(s.leaf, r.range(.85f, 1.15f)), r);
    }
}
void make_twig(Builder& b, const FloraStyle& s, Random& r) {
    const unsigned sticks = 3 + static_cast<unsigned>(r() * 2);
    for (unsigned i = 0; i < sticks; ++i) {
        const float a = r() * 2 * pi, h = s.height * r.range(.6f, 1.0f), lean = s.width * r.range(.15f, .45f);
        const auto colour = mul(s.leaf, r.range(.8f, 1.15f));
        const float width = std::max(.035f, s.height * .045f);
        blade(b, {0, 0, 0}, a, h, lean, width, mul(colour, .6f), colour, 3);
        const float t = r.range(.35f, .6f); // One branch from partway up.
        const Vec3 from{std::cos(a) * lean * t * t, std::sin(a) * lean * t * t, h * t};
        const float turn = r.range(.6f, 1.2f) * (r() < .5f ? -1 : 1);
        if (!b.far)
            blade(b, from, a + turn, h * .4f, h * .3f, width * .6f, colour, mul(colour, 1.1f), 2);
    }
}
void make_mushroom(Builder& b, const FloraStyle& s, Random& r) {
    const unsigned caps = 1 + static_cast<unsigned>(r() * 3);
    for (unsigned m = 0; m < caps; ++m) {
        const float a = r() * 2 * pi, d = m ? s.width * r.range(.2f, .4f) : 0;
        const float scale = m ? r.range(.45f, .75f) : 1.0f, h = s.height * scale;
        const Vec3 at{std::cos(a) * d, std::sin(a) * d, 0};
        constexpr int sides = 7;
        const int step = b.far ? 2 : 1; // Far: a coarser cap.
        const float stem = h * .12f, cap = h * .42f;
        std::array<std::uint32_t, sides> low{}, high{}, rim{}, crown{};
        const Vec3 white{.86f, .82f, .72f};
        for (int i = 0; i < sides; ++i) {
            const float t = i * 2 * pi / sides, c = std::cos(t), sn = std::sin(t);
            low[i] = b.point(add(at, Vec3{c * stem, sn * stem, -.02f}), mul(white, .6f));
            high[i] = b.point(add(at, Vec3{c * stem * .8f, sn * stem * .8f, h * .62f}), white);
            rim[i] = b.point(add(at, Vec3{c * cap, sn * cap, h * .55f}), mul(s.accent, .7f));
            crown[i] = b.point(add(at, Vec3{c * cap * .65f, sn * cap * .65f, h * .88f}), s.accent);
        }
        const auto top = b.point(add(at, Vec3{0, 0, h}), add(mul(s.accent, 1.1f), Vec3{.05f, .05f, .05f}));
        const auto gills = b.point(add(at, Vec3{0, 0, h * .62f}), mul(white, .75f));
        for (int i = 0; i < sides; i += step) {
            const int j = std::min(i + step, sides) % sides;
            b.quad(low[i], low[j], high[j], high[i]);
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
        {"SAW1", rock, {.78f, .85f, .95f}, {}},           {"SAW2", rock, {.78f, .85f, .95f}, {}},
        {"SAW3", rock, {.78f, .85f, .95f}, {}},           {"SAW6", flower, {.4f, .62f, .72f}, {.45f, .85f, 1.0f}},
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
        style.height = std::clamp(style.height, .12f, style.width * .45f);
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
        case FloraKind::twig: make_twig(builder, style, random); break;
        case FloraKind::mushroom: make_mushroom(builder, style, random); break;
        }
        models[i].rigid = style.kind == FloraKind::rock;
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
        append({FloraKind::rock, {.55f, .53f, .5f}, {}, .6f, .22f}, 300, 3);
        append({FloraKind::twig, {.36f, .26f, .16f}, {}, .5f, .35f}, 400, 2);
        return all;
    }();
    return library;
}
} // namespace sacrifice
