#include "sacrifice/motion.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
bool inside(const Terrain& t, float x, float y) {
    return x >= 0 && y >= 0 && x < t.width * 10.0 && y < t.height * 10.0;
}
Vec3 difference(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
double wrapped(double a) {
    while (a > 3.14159)
        a -= 6.28318;
    while (a <= -3.14159)
        a += 6.28318;
    return a;
}
} // namespace
double TerrainPlane::height(float x, float y) const {
    return (distance - static_cast<double>(normal.x) * x - static_cast<double>(normal.y) * y) / normal.z;
}
bool terrain_plane(const Terrain& t, float x, float y, TerrainPlane& p) {
    if (!std::isfinite(x) || !std::isfinite(y))
        return false;
    const double cx = std::trunc(x * 0.1), cy = std::trunc(y * 0.1);
    if (cx < 0 || cy < 0 || cx >= t.width || cy >= t.height)
        return false;
    const auto ix = static_cast<unsigned>(cx), iy = static_cast<unsigned>(cy);
    const auto r00 = t.raw(ix, iy), r10 = t.raw(ix + 1, iy), r01 = t.raw(ix, iy + 1),
               r11 = t.raw(ix + 1, iy + 1);
    const float h00 = t.at(ix, iy), h10 = t.at(ix + 1, iy), h01 = t.at(ix, iy + 1),
                h11 = t.at(ix + 1, iy + 1);
    const double fx = x - cx * 10, fy = y - cy * 10;
    double ax = cx, ay = cy, az = h00;
    std::uint16_t corners{};
    if (t.main_diagonal(ix, iy)) {
        if (fy <= fx) {
            p.normal = {h00 - h10, h10 - h11, 10};
            corners = r00 | r10 | r11;
        } else {
            p.normal = {h01 - h11, h00 - h01, 10};
            corners = r00 | r01 | r11;
        }
    } else if (10 - fy >= fx) {
        p.normal = {h00 - h10, h00 - h01, 10};
        corners = r00 | r10 | r01;
    } else {
        p.normal = {h01 - h11, h10 - h11, 10};
        corners = r10 | r01 | r11;
        ax += 1;
        az = h10;
    }
    const double length = std::sqrt(static_cast<double>(p.normal.x) * p.normal.x +
                                    static_cast<double>(p.normal.y) * p.normal.y + 100);
    p.normal = {static_cast<float>(p.normal.x / length), static_cast<float>(p.normal.y / length),
                static_cast<float>(10 / length)};
    p.distance = static_cast<float>((ay * p.normal.y + ax * p.normal.x) * 10 + az * p.normal.z);
    p.solid = !(corners & 0x8000);
    return true;
}
double terrain_surface(const Terrain& t, float x, float y, TerrainPlane* out) {
    if (!inside(t, x, y))
        return 0;
    TerrainPlane local;
    auto& plane = out ? *out : local;
    return terrain_plane(t, x, y, plane) && plane.solid ? plane.height(x, y) : 0;
}
double terrain_true_height(const Terrain& t, float x, float y) {
    TerrainPlane plane;
    return inside(t, x, y) && terrain_plane(t, x, y, plane) ? plane.height(x, y) : 0;
}
bool terrain_edge_slide(const Terrain& t, Vec3 from, Vec3 to, Vec3& result) {
    from.z = static_cast<float>(terrain_surface(t, from.x, from.y));
    if (from.z <= 0)
        return false;
    Vec3 good = from;
    float low = 0, high = 1;
    const auto delta = difference(to, from);
    for (int i = 0; i < 4; ++i) {
        const float mid = (high + low) * 0.5f;
        Vec3 sample{delta.x * mid + from.x, delta.y * mid + from.y, 0};
        const double height = terrain_surface(t, sample.x, sample.y);
        if (height <= 0)
            high = mid;
        else {
            low = mid;
            sample.z = static_cast<float>(height);
            good = sample;
        }
    }
    // cross(to-from, up), scaled by the untravelled fraction.
    const float dx = static_cast<float>(delta.y * (1.0 - low));
    const double dy = -delta.x * (1.0 - low);
    Vec3 a{good.x + dx, static_cast<float>(good.y + dy), 0};
    Vec3 b{good.x - dx, static_cast<float>(good.y - dy), 0};
    a.z = static_cast<float>(terrain_surface(t, a.x, a.y));
    b.z = static_cast<float>(terrain_surface(t, b.x, b.y));
    result = (a.z > 0) == (b.z > 0) ? good : a.z > 0 ? a : b;
    return true;
}
Orientation motion_orientation(float yaw, float pitch, float roll) {
    const double sb = std::sin(static_cast<double>(pitch)), ca = std::cos(static_cast<double>(yaw)),
                 sa = std::sin(static_cast<double>(yaw)), cc = std::cos(static_cast<double>(roll)),
                 sc = std::sin(static_cast<double>(roll));
    const float cb = static_cast<float>(std::cos(static_cast<double>(pitch))),
                sbs = static_cast<float>(sb * sa);
    return {static_cast<float>(sb * sa * sc + cc * ca),
            static_cast<float>(cc * sa - sc * sb * ca),
            static_cast<float>(sc * cb),
            static_cast<float>(-cb * sa),
            static_cast<float>(cb * ca),
            static_cast<float>(sb),
            static_cast<float>(sbs * cc - ca * sc),
            static_cast<float>(-sc * sa - sb * ca * cc),
            static_cast<float>(cc * cb)};
}
Orientation motion_yaw_pitch(float yaw, float pitch) {
    const double sy = std::sin(static_cast<double>(yaw)), cy = std::cos(static_cast<double>(yaw)),
                 sp = std::sin(static_cast<double>(pitch)), cp = std::cos(static_cast<double>(pitch));
    return {static_cast<float>(cy),       static_cast<float>(sy),       0,
            static_cast<float>(-sy * cp), static_cast<float>(cp * cy),  static_cast<float>(sp),
            static_cast<float>(sy * sp),  static_cast<float>(-cy * sp), static_cast<float>(cp)};
}
bool motion_contains_point(const ObjectMotion& object, Vec3 point) {
    const auto& p = object.position;
    for (const auto& box : object.collision_boxes)
        // Literal x87 status tests: C0 accepts unordered lower X/Y and upper Z comparisons;
        // C0|C3 rejects unordered upper X/Y and lower Z comparisons. Hex-Rays loses that distinction.
        if (!(static_cast<double>(box.minimum.x) + p.x >= point.x) &&
            static_cast<double>(p.x) + box.maximum.x > point.x &&
            !(static_cast<double>(box.minimum.y) + p.y >= point.y) &&
            static_cast<double>(p.y) + box.maximum.y > point.y &&
            point.z > static_cast<double>(box.minimum.z) + p.z - 1 &&
            !(point.z >= static_cast<double>(box.maximum.z) + p.z + 2))
            return true;
    return false;
}
double motion_box_separation(Vec3 position, const Box3& first, Vec3 other, const Box3& second) {
    const Box3 expanded{
        {static_cast<float>(static_cast<double>(position.x) + first.minimum.x - second.maximum.x),
         static_cast<float>(static_cast<double>(position.y) + first.minimum.y - second.maximum.y),
         static_cast<float>(static_cast<double>(position.z) + first.minimum.z - second.maximum.z)},
        {static_cast<float>(static_cast<double>(position.x) + first.maximum.x - second.minimum.x),
         static_cast<float>(static_cast<double>(position.y) + first.maximum.y - second.minimum.y),
         static_cast<float>(static_cast<double>(position.z) + first.maximum.z - second.minimum.z)}};
    const float dx = other.x < expanded.minimum.x   ? expanded.minimum.x - other.x
                     : other.x > expanded.maximum.x ? expanded.maximum.x - other.x
                                                    : 0,
                dy = other.y < expanded.minimum.y   ? expanded.minimum.y - other.y
                     : other.y > expanded.maximum.y ? expanded.maximum.y - other.y
                                                    : 0;
    const double dz = other.z < expanded.minimum.z   ? static_cast<double>(expanded.minimum.z) - other.z
                      : other.z > expanded.maximum.z ? static_cast<double>(expanded.maximum.z) - other.z
                                                     : 0;
    return static_cast<double>(dx) * dx + static_cast<double>(dy) * dy + dz * dz;
}
float motion_wrap(double a) {
    return static_cast<float>(wrapped(a));
}
void motion_approach(float& value, float target, float increase, float decrease) {
    if (value < target)
        value = std::min(static_cast<float>(static_cast<double>(value) + increase), target);
    else if (value > target)
        value = std::max(static_cast<float>(static_cast<double>(value) - decrease), target);
}
void motion_approach_angle(float& value, float target, float step) {
    const double delta = wrapped(static_cast<double>(value) - target);
    if (delta > step)
        value = motion_wrap(static_cast<double>(value) - step);
    else if (delta < -step)
        value = motion_wrap(static_cast<double>(value) + step);
    else
        value = target;
}
void motion_damp(Vec3& v, float amount, int ticks) {
    const double length = std::sqrt(static_cast<double>(v.x) * v.x + static_cast<double>(v.y) * v.y +
                                    static_cast<double>(v.z) * v.z);
    if (length < amount) {
        v = {};
        return;
    }
    // The factor is calculated once, not recalculated as the speed falls each tick.
    const double factor = 1 - amount / length;
    double x = v.x, y = v.y, z = v.z;
    for (int i = 0; i < ticks; ++i) {
        x *= factor;
        y *= factor;
        z *= factor;
    }
    if (ticks > 0)
        v = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
}
Box3 motion_box(const Box3& box, const Orientation& m, bool axial) {
    const Vec3 center{static_cast<float>((static_cast<double>(box.minimum.x) + box.maximum.x) * 0.5),
                      static_cast<float>((static_cast<double>(box.minimum.y) + box.maximum.y) * 0.5),
                      static_cast<float>((static_cast<double>(box.minimum.z) + box.maximum.z) * 0.5)};
    Box3 result;
    for (unsigned i = 0; i < (axial ? 6u : 8u); ++i) {
        Vec3 p;
        if (axial) {
            p = center;
            if (i < 2)
                p.x = i & 1 ? box.maximum.x : box.minimum.x;
            else if (i < 4)
                p.y = i & 1 ? box.maximum.y : box.minimum.y;
            else
                p.z = i & 1 ? box.maximum.z : box.minimum.z;
        } else
            p = {i & 1 ? box.maximum.x : box.minimum.x, i & 2 ? box.maximum.y : box.minimum.y,
                 i & 4 ? box.maximum.z : box.minimum.z};
        const Vec3 point{
            static_cast<float>(static_cast<double>(p.x) * m[0] + static_cast<double>(p.y) * m[3] +
                               static_cast<double>(p.z) * m[6]),
            static_cast<float>(static_cast<double>(p.x) * m[1] + static_cast<double>(p.y) * m[4] +
                               static_cast<double>(p.z) * m[7]),
            static_cast<float>(static_cast<double>(p.x) * m[2] + static_cast<double>(p.y) * m[5] +
                               static_cast<double>(p.z) * m[8])};
        if (i == 0)
            result = {point, point};
        else {
            const auto include = [](float& low, float& high, float value) {
                if (value >= low) {
                    if (value > high)
                        high = value;
                } else
                    low = value;
            };
            include(result.minimum.x, result.maximum.x, point.x);
            include(result.minimum.y, result.maximum.y, point.y);
            include(result.minimum.z, result.maximum.z, point.z);
        }
    }
    return result;
}
double motion_segment_contact(Vec3 from, Vec3 delta, const Box3& box, float radius) {
    const std::array<float, 3> start{from.x, from.y, from.z}, change{delta.x, delta.y, delta.z},
        low{box.minimum.x, box.minimum.y, box.minimum.z}, high{box.maximum.x, box.maximum.y, box.maximum.z};
    double result = -1;
    bool outside = false;
    for (unsigned axis = 0; axis < 3; ++axis) {
        double distance;
        if (static_cast<double>(radius) + start[axis] > low[axis]) {
            if (static_cast<double>(start[axis]) - radius < high[axis])
                continue;
            outside = true;
            if (change[axis] >= 0)
                continue;
            distance = static_cast<double>(high[axis]) - start[axis] + radius;
        } else {
            outside = true;
            if (change[axis] <= 0)
                continue;
            distance = static_cast<double>(low[axis]) - start[axis] - radius;
        }
        const double time = distance / change[axis];
        const float t = static_cast<float>(time);
        if (axis && time >= result && result >= 0)
            continue;
        bool within = true;
        for (unsigned c = 0; c < 3; ++c) {
            if (c == axis)
                continue;
            // Native x/y faces keep their first cross coordinate in x87, and store the second.
            double point = static_cast<double>(axis == 0 && c == 1 ? time : t) * change[c] + start[c];
            if (c == 2 || (axis == 2 && c == 1))
                point = static_cast<float>(point);
            if (!(point - radius <= high[c] && point + radius >= low[c]))
                within = false;
        }
        if (within)
            result = t;
    }
    if (!outside)
        result = 0;
    return result < 0 || result > 1 ? -1 : result;
}
double motion_box_contact(Vec3 from, Vec3 delta, const Box3& box, Vec3* hit, Vec3* slide) {
    const std::array<float, 3> start{from.x, from.y, from.z}, change{delta.x, delta.y, delta.z},
        low{box.minimum.x, box.minimum.y, box.minimum.z}, high{box.maximum.x, box.maximum.y, box.maximum.z},
        end{from.x + delta.x, from.y + delta.y, from.z + delta.z};
    double result = 1.1;
    bool outside = false;
    for (unsigned axis = 0; axis < 3; ++axis) {
        float face;
        if (!(start[axis] > low[axis])) {
            outside = true;
            if (!(end[axis] > low[axis]))
                continue;
            face = low[axis];
        } else if (start[axis] >= high[axis]) {
            outside = true;
            if (!(end[axis] < high[axis]))
                continue;
            face = high[axis];
        } else
            continue;
        const double time = (static_cast<double>(face) - start[axis]) / change[axis];
        const float t = static_cast<float>(time);
        if (!(time < result))
            continue;
        std::array<float, 3> point;
        bool within = true;
        for (unsigned c = 0; c < 3; ++c) {
            point[c] = c == axis ? face : static_cast<float>(static_cast<double>(t) * change[c] + start[c]);
            if (c != axis && !(point[c] <= high[c] && point[c] >= low[c]))
                within = false;
        }
        if (!within)
            continue;
        result = t;
        if (hit)
            *hit = {point[0], point[1], point[2]};
        if (slide) {
            auto slid = end;
            slid[axis] = face;
            *slide = {slid[0], slid[1], slid[2]};
        }
    }
    if (!outside) {
        if (hit)
            *hit = from;
        result = 0;
        if (slide) {
            // Existing overlaps choose a horizontal escape face; vertical penetration is ignored here.
            float distance = start[0] - low[0];
            unsigned axis = 0;
            int sign = -1;
            if (high[0] - start[0] < distance) {
                distance = high[0] - start[0];
                sign = 1;
            }
            if (start[1] - low[1] < distance) {
                distance = start[1] - low[1];
                axis = 1;
                sign = -1;
            }
            if (high[1] - start[1] < distance) {
                axis = 1;
                sign = 1;
            }
            auto slid = end;
            if (sign * static_cast<double>(change[axis]) >= 0) {
                *slide = {end[0], end[1], end[2]};
                if (end[0] < low[0] || end[0] > high[0] || end[1] < low[1] || end[1] > high[1] ||
                    end[2] < low[2] || end[2] > high[2])
                    return 2;
            } else
                slid[axis] = start[axis];
            *slide = {slid[0], slid[1], slid[2]};
        }
    }
    return result > 1 ? -1 : result;
}
double ObjectMotion::contact(const ObjectMotion& other, Vec3 delta, Vec3& hit, Vec3* slide) const {
    if (collision_boxes.empty() || other.collision_boxes.empty())
        return -1;
    double result = -1;
    const auto& own = collision_boxes.front(); // Literal original: only the moving object's first box.
    bool use_slide = slide != nullptr;
    for (const auto& b : other.collision_boxes) {
        const Box3 expanded{
            {static_cast<float>(static_cast<double>(b.minimum.x) + other.position.x - own.maximum.x),
             static_cast<float>(static_cast<double>(b.minimum.y) + other.position.y - own.maximum.y),
             static_cast<float>(static_cast<double>(b.minimum.z) + other.position.z - own.maximum.z)},
            {static_cast<float>(static_cast<double>(b.maximum.x) + other.position.x - own.minimum.x),
             static_cast<float>(static_cast<double>(b.maximum.y) + other.position.y - own.minimum.y),
             static_cast<float>(static_cast<double>(b.maximum.z) + other.position.z - own.minimum.z)}};
        const Vec3 end{position.x + delta.x, position.y + delta.y, position.z + delta.z};
        if (!((position.x >= expanded.minimum.x || end.x >= expanded.minimum.x) &&
              (position.x <= expanded.maximum.x || end.x <= expanded.maximum.x) &&
              (position.y >= expanded.minimum.y || end.y >= expanded.minimum.y) &&
              (position.y <= expanded.maximum.y || end.y <= expanded.maximum.y) &&
              (position.z >= expanded.minimum.z || end.z >= expanded.minimum.z) &&
              (position.z <= expanded.maximum.z || end.z <= expanded.maximum.z)))
            continue;
        const double time = motion_box_contact(position, delta, expanded, &hit, slide);
        if (!(time >= 0))
            continue;
        if (result < 0 || time < result)
            result = time;
        delta = difference(use_slide ? *slide : hit, position);
        use_slide = false;
    }
    return result;
}
const ObjectMotion* motion_collide(ObjectMotion& object, Vec3& delta, float* fraction,
                                   const ObjectMotion* ignore,
                                   std::span<const ObjectMotion* const> candidates) {
    if (fraction)
        *fraction = 0;
    const ObjectMotion* collided = nullptr;
    for (const auto* candidate : candidates) {
        if (!candidate || candidate == ignore)
            continue;
        Vec3 hit, slide;
        const double time = object.contact(*candidate, delta, hit, &slide);
        if (!(time >= 0))
            continue;
        delta = difference((object.collision_mask & 0x80000000) ? slide : hit, object.position);
        if (time <= 1) {
            collided = candidate;
            if (fraction)
                *fraction = static_cast<float>(time);
        }
    }
    return collided;
}
void ObjectMotion::push_from(Vec3 point, float amount, unsigned axes) {
    if (flags & 0x10000)
        return;
    Vec3 delta{axes & 1 ? position.x + reference_center.x - point.x : 0,
               axes & 2 ? position.y + reference_center.y - point.y : 0,
               axes & 4 ? position.z + reference_center.z - point.z : 0};
    const double length =
        std::sqrt(static_cast<double>(delta.x) * delta.x + static_cast<double>(delta.y) * delta.y +
                  static_cast<double>(delta.z) * delta.z);
    if (length != 0) {
        const double scale = amount / length;
        delta = {static_cast<float>(delta.x * scale), static_cast<float>(delta.y * scale),
                 static_cast<float>(delta.z * scale)};
    }
    if (axes & 1)
        velocity.x = static_cast<float>((static_cast<double>(delta.x) - velocity.x) * 0.5 + velocity.x);
    if (axes & 2)
        velocity.y = static_cast<float>((static_cast<double>(delta.y) - velocity.y) * 0.5 + velocity.y);
    if (axes & 4)
        velocity.z = static_cast<float>((static_cast<double>(delta.z) - velocity.z) * 0.5 + velocity.z);
}
void ObjectMotion::push_direction(Vec3 direction, float amount, unsigned axes) {
    if (flags & 0x10000)
        return;
    double x = axes & 1 ? direction.x : 0;
    float y = axes & 2 ? direction.y : 0, z = axes & 4 ? direction.z : 0;
    const double length = std::sqrt(x * x + static_cast<double>(y) * y + static_cast<double>(z) * z);
    if (length != 0) {
        const double scale = amount / length;
        x *= scale;
        y = static_cast<float>(y * scale);
        z = static_cast<float>(z * scale);
    }
    if (axes & 1)
        velocity.x = static_cast<float>((x - velocity.x) * 0.5 + velocity.x);
    if (axes & 2)
        velocity.y = static_cast<float>((static_cast<double>(y) - velocity.y) * 0.5 + velocity.y);
    if (axes & 4)
        velocity.z = static_cast<float>((static_cast<double>(z) - velocity.z) * 0.5 + velocity.z);
}
void ObjectMotion::creature_flags(std::uint32_t f, std::uint32_t properties, std::uint32_t& state) {
    collision_mask = 0xc0004015;
    if (f & 0x80)
        flags = 0x10000;
    else {
        flags = f & 2 ? 0x203db : properties & 0xc00 ? 0x209ff : 0x209cf;
        flags = f & 0x10 ? flags | 0x10000 : flags & ~0x10000u;
        if (f & 0x4000000)
            flags &= ~4u;
    }
    if (f & 0x100000) {
        collision_mask = 0xc0004010;
        state |= 0x2000000;
    } else
        state &= ~0x2000000u;
}
void ObjectMotion::advance(const Terrain& terrain, std::uint32_t& state, int ticks, const Hooks& hooks) {
    if (ticks < 0)
        throw Error("Negative motion tick count");
    const auto old = position;
    const bool grounded = (state & 2) != 0;
    float gravity = 0;
    if (grounded) {
        airborne_ticks = 0;
        velocity.z = std::max(velocity.z, 0.0f);
    } else if (flags & 4) {
        gravity = -0.002725f;
        airborne_ticks += ticks;
    }
    collided = nullptr;
    state &= ~7u;
    if (!flags && !collision_mask)
        return;
    Vec3 delta;
    const float offset = collision_mask & 0x80000 ? -box_min.z : 0;
    if (flags & 0x38) {
        if (flags & 8)
            yaw = motion_wrap(static_cast<double>(ticks) * angular_velocity.x + yaw);
        if (flags & 0x10)
            pitch = motion_wrap(static_cast<double>(ticks) * angular_velocity.y + pitch);
        if (flags & 0x20)
            roll = motion_wrap(static_cast<double>(ticks) * angular_velocity.z + roll);
        orientation = (flags & 0x38) == 8 ? motion_orientation(yaw) : motion_orientation(yaw, pitch, roll);
    }
    if (flags & 2) {
        const auto& m = orientation;
        const auto& v = local_velocity;
        // The transform is stored to three floats before multiplication by ticks.
        Vec3 world{static_cast<float>(static_cast<double>(v.x) * m[0] + static_cast<double>(v.y) * m[3] +
                                      static_cast<double>(v.z) * m[6]),
                   static_cast<float>(static_cast<double>(v.x) * m[1] + static_cast<double>(v.y) * m[4] +
                                      static_cast<double>(v.z) * m[7]),
                   static_cast<float>(static_cast<double>(v.x) * m[2] + static_cast<double>(v.y) * m[5] +
                                      static_cast<double>(v.z) * m[8])};
        delta = {ticks * world.x, ticks * world.y, ticks * world.z};
    }
    if (flags & 1) {
        if (flags & 0x40) {
            double x = velocity.x, y = velocity.y, z = velocity.z;
            for (int i = 0; i < ticks; ++i) {
                delta = {static_cast<float>(delta.x + x), static_cast<float>(delta.y + y),
                         static_cast<float>(delta.z + z)};
                x *= 0.99;
                y *= 0.99;
                z = (z + gravity) * 0.998;
            }
            if (ticks > 0)
                velocity = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
        } else {
            const double n = ticks;
            delta = {static_cast<float>(n * velocity.x + delta.x),
                     static_cast<float>(n * velocity.y + delta.y),
                     static_cast<float>(gravity * n * n * 0.5 + n * velocity.z + delta.z)};
            velocity.z = static_cast<float>(n * gravity + velocity.z);
        }
    }
    if ((collision_mask & 0xbfff) && hooks.collide) {
        float fraction = 0;
        collided = hooks.collide(*this, delta, &fraction, nullptr);
        if (collided) {
            state |= 1;
            if ((collision_mask & 0x40000000) && fraction == 0) {
                const auto previous = velocity;
                push_from(collided->position, 0.083333336f, 3);
                const auto change = difference(velocity, previous);
                delta = {delta.x + change.x * ticks, delta.y + change.y * ticks, delta.z + change.z * ticks};
                hooks.collide(*this, delta, nullptr, collided);
            }
        }
    }
    position = {position.x + delta.x, position.y + delta.y, position.z + delta.z};
    const double height = terrain_surface(terrain, position.x, position.y, &plane);
    if (height > 0) {
        ground_height = static_cast<float>(height);
        const float threshold = static_cast<float>(height + offset);
        if (((collision_mask & 0x4000) && position.z < threshold) ||
            ((flags & 0x800) && velocity.z <= 0 && (position.z < threshold || grounded))) {
            position.z = threshold;
            state |= 2;
        }
    } else {
        if ((position.x <= 5 || position.y <= 5 || position.x >= 2555 || position.y >= 2555) && hooks.death)
            hooks.death(1);
        ground_height = static_cast<float>(terrain_true_height(terrain, position.x, position.y));
        state |= 4;
        if (ground_height - position.z > 25 && hooks.death)
            hooks.death(0);
    }
    if ((state & 2) && (flags & 1))
        motion_damp(velocity, 0.02f, ticks);
    state &= ~0x80000u;
    displacement = difference(position, old);
}
} // namespace sacrifice
