#include "sacrifice/title_backdrop.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
constexpr double tau = 6.28318, quarter = 1.5707963267948966;
// Tunnel extent along its axis (+77 / +85): rings are 100 / 32 apart.
constexpr double tunnel_length = 100, ring_gap = tunnel_length / 32;
struct ViewPoint {
    float x, y, z, u, v;
    std::array<float, 4> colour;
};
std::uint32_t pack(const std::array<float, 4>& c) {
    const auto byte = [](float value) { return static_cast<std::uint32_t>(std::clamp(value, 0.f, 255.f)); };
    return byte(c[0]) | byte(c[1]) << 8 | byte(c[2]) << 16 | byte(c[3]) << 24;
}
} // namespace

TitleBackdrop::TitleBackdrop() {
    // 0x4bb710.
    for (int i = 0; i <= sides; ++i)
        ripple_[static_cast<std::size_t>(i)] = (std::sin(static_cast<double>(i) / sides * tau) + 1) * 3.14159;
    for (int i = 0; i < rings; ++i) {
        auto& ring = ring_[static_cast<std::size_t>(i)];
        ring.t = i;
        ring.y = i * ring_gap;
        ring.v = i * .125f;
    }
    begin_segment();
    // 0x40f500 event 1: each of the 17 grid columns starts a random walk down its 11 rows.
    for (std::size_t column = 0; column < 17; ++column) {
        double value = random() / 32768.0 * tau;
        for (std::size_t row = 0; row < 11; ++row) {
            value = random() / 32768.0 + value - .25;
            wave_[row * 17 + column] = value;
        }
    }
}
bool TitleBackdrop::shows_tunnel(FourCC page) {
    for (const auto* quiet : {"logo", "lprg", "opts", "igam", "loby"})
        if (page == FourCC::from_text(quiet))
            return false;
    return true;
}
unsigned TitleBackdrop::random() {
    seed_ = seed_ * 214013 + 2531011;
    return seed_ >> 16 & 0x7fff;
}
void TitleBackdrop::advance(float seconds) {
    carry_ += std::max(seconds, 0.f) * 60;
    const int ticks = static_cast<int>(carry_);
    carry_ -= static_cast<float>(ticks);
    for (int i = 0; i < std::min(ticks, 12); ++i) {
        step_tunnel();
        step_logo();
    }
}
std::array<double, 3> TitleBackdrop::curve(const Segment& segment, double t) const {
    // 0x4bc8e0; the bend heading (+173) is always reset to zero by 0x4bc6d0, so the offset is along Z.
    const double offset =
        (std::sin(3.14159 / segment.count * t + segment.phase) - std::sin(segment.phase)) * segment.amplitude;
    return {segment.x, t / (rings - 1) * tunnel_length, offset + segment.z};
}
void TitleBackdrop::begin_segment() {
    // 0x4bc0e0: a bend of 24..60 rings and amplitude 24..48 from the newest ring, alternating direction.
    Segment next;
    next.count = static_cast<int>(random() / 32768.0 * 36 + 24);
    next.amplitude = (random() / 32768.0 + 1) * 24;
    next.phase = segment_.phase == quarter ? -quarter : quarter;
    next.x = ring_.back().x;
    next.z = ring_.back().z;
    segment_ = next;
}
void TitleBackdrop::place(Ring& ring, const Ring& previous) {
    // 0x4bc250.
    const auto at = curve(segment_, segment_.t);
    ring.x = at[0];
    ring.z = at[2];
    ring.radius = radius_;
    ring.t = segment_.t;
    ring.phase = phase_;
    ring.segment = segment_;
    ring.v = previous.v + .125f;
    if (std::abs(pulse_ - rings + 1) >= 10)
        ring.brightness = std::clamp(
            static_cast<float>((static_cast<int>(random()) - 16483) * 0.0000019073486 + previous.brightness), .3f,
            .8f);
    else if (pulse_ < rings - 1)
        ring.brightness = previous.brightness - (previous.brightness - pulse_brightness_) * .5f;
    else if (pulse_ == rings - 1)
        ring.brightness = 1;
    else
        ring.brightness = (1 - previous.brightness) * .5f + previous.brightness;
    for (std::size_t i = 0; i < 3; ++i)
        ring.colour[i] = std::clamp(
            static_cast<float>((static_cast<int>(random()) - 16483) * 0.00000095367432 + previous.colour[i]), .6f,
            1.4f);
}
void TitleBackdrop::step_tunnel() {
    // 0x4bba80: the view moves 0.12 per tick; each time it passes a ring, the ring data moves one place
    // towards the viewer and a new ring is added at the far end.
    wobble_ -= .03;
    if (wobble_ < 0)
        wobble_ += tau;
    scroll_ -= .12;
    while (ring_[0].y - ring_[1].y >= scroll_) {
        scroll_ += ring_[1].y - ring_[0].y;
        for (std::size_t i = 0; i + 1 < ring_.size(); ++i) {
            const double y = ring_[i].y;
            ring_[i] = ring_[i + 1];
            ring_[i].y = y;
        }
        place(ring_.back(), ring_[ring_.size() - 2]);
        if (segment_.count < ++segment_.t)
            begin_segment();
        // 0x4bc820: ring phase and the bright pulse that travels towards the viewer.
        phase_ += .785398;
        if (phase_ >= tau)
            phase_ -= tau;
        if (pulse_ >= -10)
            --pulse_;
        else if (random() < 4096) {
            pulse_ = rings + 10;
            pulse_brightness_ = ring_.back().brightness;
        }
        // 0x4bc460: the radius drifts between 3 and 6.
        radius_accel_ = std::clamp(radius_accel_ + (static_cast<int>(random()) - 0x4000) / 16384.0 * .0005, -.005,
                                   .005);
        radius_rate_ += radius_accel_;
        if (std::abs(radius_rate_) > .05) {
            radius_rate_ = std::copysign(.05, radius_rate_);
            radius_accel_ = 0;
        }
        radius_ = std::clamp(radius_ + radius_rate_, 3.0, 6.0);
        // 0x4bc8a0.
        if (ring_[0].v >= 1)
            for (auto& ring : ring_)
                ring.v -= 1;
    }
    // 0x4bc5a0: the view rolls about the tunnel axis.
    roll_accel_ = std::clamp(roll_accel_ + (static_cast<int>(random()) - 0x4000) / 16384.0 * 0.00002396842956542969,
                             -0.003067958984375, 0.003067958984375);
    roll_rate_ += roll_accel_;
    if (std::abs(roll_rate_) > 0.0122718359375) {
        roll_rate_ = std::copysign(0.0122718359375, roll_rate_);
        roll_accel_ = 0;
    }
    roll_ += roll_rate_;
    if (roll_ >= tau)
        roll_ -= tau;
    else if (roll_ <= -tau)
        roll_ += tau;
}
void TitleBackdrop::step_logo() {
    // 0x40f500 event 3.
    wave_phase_ += .016;
    if (wave_phase_ >= tau)
        wave_phase_ -= tau;
    for (std::size_t column = 0; column < 17; ++column) {
        const double drift = random() / 32768.0 * .01 - .005;
        for (std::size_t row = 0; row < 11; ++row)
            wave_[row * 17 + column] += drift;
    }
}
Mesh TitleBackdrop::tunnel(float width, float height) {
    // 0x4bbcf0: the eye follows ring 1 and looks at ring 5, both advanced by the scroll fraction, with the
    // world turned about the tunnel axis by the roll. The 45-degree half-angle is RENDERWINDOW's (0x47fb30).
    Mesh mesh;
    if (width <= 0 || height <= 0)
        return mesh;
    const double fraction = scroll_ / -ring_gap;
    const double cos_roll = std::cos(roll_), sin_roll = std::sin(roll_);
    const auto turn = [&](double x, double y, double z) {
        return std::array<double, 3>{cos_roll * x + sin_roll * z, y, -sin_roll * x + cos_roll * z};
    };
    const auto point = [&](const Ring& ring) {
        const auto at = curve(ring.segment, ring.t + fraction);
        return turn(at[0], ring.y - scroll_, at[2]);
    };
    const auto eye = point(ring_[1]), target = point(ring_[5]);
    const auto normalise = [](std::array<double, 3> v) {
        const double length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        return length > 0 ? std::array<double, 3>{v[0] / length, v[1] / length, v[2] / length} : v;
    };
    const auto cross = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
        return std::array<double, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                                     a[0] * b[1] - a[1] * b[0]};
    };
    const auto forward = normalise({target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]});
    const auto right = normalise(cross({0, 0, 1}, forward));
    const auto up = cross(forward, right);
    const double aspect = height / width, half = 45 * std::min(1.0, .75 / aspect) * 3.14159265358979 / 180,
                 tan_width = std::tan(half), tan_height = tan_width * aspect;
    // View-space vertices for every ring, as 0x4bc950 writes them.
    std::vector<ViewPoint> points(static_cast<std::size_t>(rings * (sides + 1)));
    for (int i = 0; i < rings; ++i) {
        const auto& ring = ring_[static_cast<std::size_t>(i)];
        const double level = std::trunc((rings - i) * ring.brightness / static_cast<double>(rings) * 255);
        const int jitter = std::max(0, 10 - std::abs(pulse_ - i));
        const std::array<float, 4> colour{static_cast<float>(std::min(level * ring.colour[0], 255.0)),
                                          static_cast<float>(std::min(level * ring.colour[1], 255.0)),
                                          static_cast<float>(std::min(level * ring.colour[2], 255.0)), 255};
        for (int side = 0; side <= sides; ++side) {
            const double angle = side * (tau / sides);
            double wobble = std::sin(ripple_[static_cast<std::size_t>(side)] + ring.phase + wobble_) * .08 *
                            ring.radius;
            wobble += (static_cast<int>(random()) - 0x4000) * 0.00006103515625 * jitter * .01;
            const double reach = ring.radius + wobble;
            auto world = turn(std::cos(angle) * reach + ring.x, ring.y, std::sin(angle) * reach + ring.z);
            auto& p = points[static_cast<std::size_t>(i * (sides + 1) + side)];
            if (side == sides) {
                const auto& first = points[static_cast<std::size_t>(i * (sides + 1))];
                p = first;
            } else {
                const double dx = world[0] - eye[0], dy = world[1] - eye[1], dz = world[2] - eye[2];
                p.x = static_cast<float>(dx * right[0] + dy * right[1] + dz * right[2]);
                p.y = static_cast<float>(dx * up[0] + dy * up[1] + dz * up[2]);
                p.z = static_cast<float>(dx * forward[0] + dy * forward[1] + dz * forward[2]);
            }
            p.u = static_cast<float>(side) / sides;
            p.v = ring.v;
            p.colour = colour;
        }
    }
    constexpr float near_plane = .05f;
    const auto emit = [&](const ViewPoint& p) {
        Vertex vertex;
        vertex.position = {static_cast<float>(width * .5 + p.x / p.z / tan_width * width * .5),
                           static_cast<float>(height * .5 - p.y / p.z / tan_height * height * .5), 0};
        vertex.uv = {p.u, p.v};
        vertex.color = pack(p.colour);
        mesh.indices.push_back(static_cast<std::uint32_t>(mesh.vertices.size()));
        mesh.vertices.push_back(vertex);
    };
    const auto blend = [](const ViewPoint& a, const ViewPoint& b, float t) {
        ViewPoint p;
        p.x = a.x + (b.x - a.x) * t;
        p.y = a.y + (b.y - a.y) * t;
        p.z = a.z + (b.z - a.z) * t;
        p.u = a.u + (b.u - a.u) * t;
        p.v = a.v + (b.v - a.v) * t;
        for (std::size_t i = 0; i < 4; ++i)
            p.colour[i] = a.colour[i] + (b.colour[i] - a.colour[i]) * t;
        return p;
    };
    const auto triangle = [&](ViewPoint a, ViewPoint b, ViewPoint c) {
        // Clip against the near plane, keeping the polygon's winding.
        std::array<ViewPoint, 4> out;
        std::size_t count = 0;
        const std::array<ViewPoint, 3> in{a, b, c};
        for (std::size_t i = 0; i < 3; ++i) {
            const auto& p = in[i];
            const auto& q = in[(i + 1) % 3];
            const bool p_in = p.z >= near_plane, q_in = q.z >= near_plane;
            if (p_in)
                out[count++] = p;
            if (p_in != q_in)
                out[count++] = blend(p, q, (near_plane - p.z) / (q.z - p.z));
        }
        for (std::size_t i = 2; i < count; ++i) {
            emit(out[0]);
            emit(out[i - 1]);
            emit(out[i]);
        }
    };
    // Far bands first, so nearer walls paint over them. Each band's texture rows are rebased into [0, 1].
    for (int i = rings - 2; i >= 0; --i)
        for (int side = 0; side < sides; ++side) {
            auto a = points[static_cast<std::size_t>(i * (sides + 1) + side + 1)],
                 b = points[static_cast<std::size_t>((i + 1) * (sides + 1) + side + 1)],
                 c = points[static_cast<std::size_t>(i * (sides + 1) + side)],
                 d = points[static_cast<std::size_t>((i + 1) * (sides + 1) + side)];
            const float base = std::floor(std::min(a.v, b.v) + 1e-4f);
            for (auto* p : {&a, &b, &c, &d})
                p->v -= base;
            triangle(a, b, c);
            triangle(b, d, c);
        }
    return mesh;
}
Mesh TitleBackdrop::logo(HudRect box) const {
    // 0x40f170: TXTR:LOGO holds the left half of the logo above the right half; the grid's left eight
    // columns sample the top and the right eight the bottom, each vertex lifted by 40 x its wave.
    Mesh mesh;
    const float cell_width = box.size.x / 16, cell_height = box.size.y / 10;
    const auto wave = [&](std::size_t column, std::size_t row) {
        return static_cast<float>(std::sin(wave_phase_ + wave_[row * 17 + column]) * .08);
    };
    for (std::size_t column = 0; column < 16; ++column)
        for (std::size_t row = 0; row < 10; ++row) {
            const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
            const bool left = column < 8;
            const float u = static_cast<float>(left ? column : column - 8) * .125f,
                        v = static_cast<float>(row) * .05f + (left ? 0 : .5f);
            for (std::size_t corner = 0; corner < 4; ++corner) {
                const std::size_t x = column + (corner & 1), y = row + (corner >> 1);
                Vertex vertex;
                vertex.position = {box.origin.x + static_cast<float>(x) * cell_width,
                                   box.origin.y + static_cast<float>(y) * cell_height + wave(x, y) * 40, 0};
                vertex.uv = {u + (corner & 1 ? .125f : 0), v + (corner >> 1 ? .05f : 0)};
                mesh.vertices.push_back(vertex);
            }
            mesh.indices.insert(mesh.indices.end(), {first, first + 1, first + 2, first + 1, first + 3, first + 2});
        }
    return mesh;
}
} // namespace sacrifice
