#include "sacrifice/spell_combat.hpp"
#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/spatial.hpp"
#include <cmath>

namespace sacrifice {
namespace {
double length(Vec3 v) {
    return std::sqrt(static_cast<double>(v.x) * v.x + static_cast<double>(v.y) * v.y +
                     static_cast<double>(v.z) * v.z);
}
Vec3 scaled(Vec3 v, double scale) {
    return {static_cast<float>(v.x * scale), static_cast<float>(v.y * scale),
            static_cast<float>(v.z * scale)};
}
int integer(double value) {
    if (!std::isfinite(value) || value >= 9223372036854775808.0 || value < -9223372036854775808.0)
        return 0;
    return std::bit_cast<int>(static_cast<unsigned>(static_cast<std::int64_t>(value)));
}
void damage(Match& match, MatchObject& target, MatchObject& source, int type, int amount) {
    if (auto* actor = dynamic_cast<CreatureActor*>(&target))
        actor->damage(match, {type, amount, source.reference()});
    else if (auto* building = dynamic_cast<BuildingActor*>(&target))
        building->damage(match, {static_cast<unsigned>(type), amount, source.reference()});
    else
        throw Error("Spell damage target requires an unimplemented host");
}
} // namespace
void spell_target_point(Vec3& point, const MatchObject* target) {
    if (!target)
        return;
    const auto* motion = target->source_motion();
    if (!motion)
        throw Error("Spell target has no motion host");
    if (motion->collision_boxes.empty()) {
        point = {static_cast<float>((static_cast<double>(motion->box_min.x) + motion->box_max.x) * 0.5 +
                                    motion->position.x),
                 static_cast<float>((static_cast<double>(motion->box_min.y) + motion->box_max.y) * 0.5 +
                                    motion->position.y),
                 static_cast<float>((static_cast<double>(motion->box_min.z) + motion->box_max.z) * 0.5 +
                                    motion->position.z)};
        return;
    }
    Vec3 sum;
    for (const auto& box : motion->collision_boxes) {
        sum.x = static_cast<float>(static_cast<double>(box.maximum.x) + box.minimum.x + sum.x);
        sum.y = static_cast<float>(static_cast<double>(box.maximum.y) + box.minimum.y + sum.y);
        sum.z = static_cast<float>(static_cast<double>(box.maximum.z) + box.minimum.z + sum.z);
    }
    const double scale = 0.5 / motion->collision_boxes.size();
    point = {static_cast<float>(static_cast<float>(sum.x * scale) + motion->position.x),
             static_cast<float>(static_cast<float>(sum.y * scale) + motion->position.y),
             static_cast<float>(sum.z * scale + motion->position.z)};
}
void spell_lead_target(Vec3& point, const MatchObject* target, float speed, const ObjectMotion& source,
                       const Terrain& terrain) {
    if (!target)
        return;
    spell_target_point(point, target);
    const auto& motion = *target->source_motion();
    const double dx = static_cast<double>(source.position.x) - point.x,
                 dy = static_cast<double>(source.position.y) - point.y,
                 dz = static_cast<double>(source.position.z) - point.z;
    const float travel = static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz) / speed);
    Vec3 velocity;
    if (motion.flags & 2) {
        const auto& o = motion.orientation;
        const auto& v = motion.local_velocity;
        velocity = {static_cast<float>(static_cast<double>(v.x) * o[0] + static_cast<double>(v.y) * o[3] +
                                       static_cast<double>(v.z) * o[6]),
                    static_cast<float>(static_cast<double>(v.x) * o[1] + static_cast<double>(v.y) * o[4] +
                                       static_cast<double>(v.z) * o[7]),
                    static_cast<float>(static_cast<double>(v.x) * o[2] + static_cast<double>(v.y) * o[5] +
                                       static_cast<double>(v.z) * o[8])};
    }
    if (motion.flags & 1) {
        velocity.x += motion.velocity.x;
        velocity.y += motion.velocity.y;
        velocity.z += motion.velocity.z;
    }
    const float local_z = point.z - motion.position.z;
    point = {static_cast<float>(static_cast<double>(velocity.x) * travel + point.x),
             static_cast<float>(static_cast<double>(velocity.y) * travel + point.y),
             static_cast<float>(static_cast<double>(velocity.z) * travel + point.z)};
    const float height = static_cast<float>(terrain_surface(terrain, point.x, point.y));
    if (target->state_flags & 2) {
        if (height > 0)
            point.z = height + local_z;
        else
            point = {static_cast<float>(point.x - static_cast<double>(velocity.x) * travel),
                     static_cast<float>(point.y - static_cast<double>(velocity.y) * travel),
                     static_cast<float>(point.z - static_cast<double>(velocity.z) * travel)};
    }
}
float spell_approach_velocity(ObjectMotion& motion, Vec3 point, float acceleration, float speed,
                              unsigned axes, float slowdown_radius) {
    Vec3 difference{axes & 1 ? point.x - motion.position.x : 0, axes & 2 ? point.y - motion.position.y : 0,
                    axes & 4 ? point.z - motion.position.z : 0};
    const double distance = length(difference);
    const float stored_distance = static_cast<float>(distance);
    if (distance < slowdown_radius)
        speed = static_cast<float>(static_cast<double>(stored_distance) * speed / slowdown_radius);
    double desired_z = difference.z;
    if (stored_distance > speed) {
        const double factor = static_cast<double>(speed) / stored_distance;
        difference.x = static_cast<float>(difference.x * factor);
        difference.y = static_cast<float>(difference.y * factor);
        desired_z *= factor; // The native Z component stays in x87 until velocity subtraction.
    }
    difference = {difference.x - motion.velocity.x, difference.y - motion.velocity.y,
                  static_cast<float>(desired_z - motion.velocity.z)};
    const double change = length(difference);
    if (change > acceleration)
        difference = scaled(difference, acceleration / change);
    motion.velocity = {difference.x + motion.velocity.x, difference.y + motion.velocity.y,
                       difference.z + motion.velocity.z};
    const double magnitude = length(motion.velocity);
    if (magnitude > speed)
        motion.velocity = scaled(motion.velocity, speed / magnitude);
    return stored_distance;
}
double spell_box_distance_squared(Vec3 point, const ObjectMotion& target, float range_squared) {
    const double dx0 = static_cast<double>(point.x) - target.position.x,
                 dy0 = static_cast<double>(point.y) - target.position.y,
                 dz0 = static_cast<double>(point.z) - target.position.z;
    double x = 0, y = 0, z = 0;
    for (const auto& box : target.collision_boxes) {
        x = std::max(x, std::max(static_cast<double>(box.minimum.x) * box.minimum.x,
                                 static_cast<double>(box.maximum.x) * box.maximum.x));
        y = std::max(y, std::max(static_cast<double>(box.minimum.y) * box.minimum.y,
                                 static_cast<double>(box.maximum.y) * box.maximum.y));
        z = std::max(z, std::max(static_cast<double>(box.minimum.z) * box.minimum.z,
                                 static_cast<double>(box.maximum.z) * box.maximum.z));
    }
    const float radius = static_cast<float>(std::sqrt(x + y + z));
    const double first_distance = std::sqrt(dx0 * dx0 + dy0 * dy0 + dz0 * dz0) - radius;
    const double clearance =
        first_distance <= 0
            ? 0
            : std::sqrt(dx0 * dx0 + static_cast<double>(static_cast<float>(dy0)) * static_cast<float>(dy0) +
                        static_cast<double>(static_cast<float>(dz0)) * static_cast<float>(dz0)) -
                  radius;
    double result = clearance * clearance;
    if (result > range_squared)
        return result;
    result = static_cast<double>(range_squared) + 1;
    for (const auto& box : target.collision_boxes) {
        const Box3 world{{box.minimum.x + target.position.x, box.minimum.y + target.position.y,
                          box.minimum.z + target.position.z},
                         {box.maximum.x + target.position.x, box.maximum.y + target.position.y,
                          box.maximum.z + target.position.z}};
        const float dx = point.x < world.minimum.x   ? world.minimum.x - point.x
                         : point.x > world.maximum.x ? world.maximum.x - point.x
                                                     : 0,
                    dy = point.y < world.minimum.y   ? world.minimum.y - point.y
                         : point.y > world.maximum.y ? world.maximum.y - point.y
                                                     : 0;
        const double dz = point.z < world.minimum.z   ? static_cast<double>(world.minimum.z) - point.z
                          : point.z > world.maximum.z ? static_cast<double>(world.maximum.z) - point.z
                                                      : 0;
        const float squared =
            static_cast<float>(static_cast<double>(dx) * dx + static_cast<double>(dy) * dy + dz * dz);
        if (result >= squared)
            result = squared;
    }
    return result;
}
MatchObject* spell_damage(Match& match, MatchObject* source, MatchObject* direct, int type, int amount,
                          float radius, std::optional<Vec3> origin) {
    if (!source)
        return direct;
    if (!origin) {
        const auto* motion = source->source_motion();
        if (!motion)
            throw Error("Spell damage source has no position");
        origin = motion->position;
    }
    MatchObject* inside = nullptr;
    if ((type & 7) == 2 || (type & 7) == 4) {
        const float range_squared = static_cast<float>(static_cast<double>(radius) * radius);
        for (unsigned group = 0; group < 3; ++group) {
            for (auto it = match.objects().begin(); it != match.objects().end();) {
                auto* target = (it++)->get();
                if (MatchObject::group(target->type) != group || target == source || target == direct ||
                    !(target->kind_flags & 0x15))
                    continue;
                const auto* motion = target->source_motion();
                if (!motion)
                    throw Error("Area damage target has no motion host");
                const float squared =
                    static_cast<float>(spell_box_distance_squared(*origin, *motion, range_squared));
                if (squared > range_squared)
                    continue;
                if (squared == 0)
                    inside = target;
                const int scaled_amount =
                    radius == 0 ? amount
                                : integer((1.0 - std::sqrt(static_cast<double>(squared)) / radius) * amount);
                damage(match, *target, *source, type, scaled_amount);
            }
        }
    }
    if (direct) {
        damage(match, *direct, *source, type, amount);
        return direct;
    }
    return inside;
}
int evaluate_spell_damage(const SpellData& spell, const MatchObject& caster, const MatchObject& target) {
    if (!(target.kind_flags & 0x15))
        return 0;
    int health;
    if (const auto* actor = dynamic_cast<const CreatureActor*>(&target))
        health = actor->stats.current[CreatureStats::health];
    else if (const auto* building = dynamic_cast<const BuildingActor*>(&target))
        health = building->health;
    else
        throw Error("Damage evaluator target requires an unimplemented host");
    const auto value = std::min(spell.amount, health);
    return caster.enemy(&target) ? value : std::bit_cast<int>(0u - static_cast<unsigned>(value));
}
void spell_scatter(Vec3& velocity, int accuracy, CrtRandom& random) {
    const float limit =
        static_cast<float>(std::atan2(accuracy * 0.001 * 0.14 + static_cast<double>(0.03f), 1.0));
    const float horizontal = static_cast<float>((random.next() - 16384) / 16384.0);
    const int vertical = random.next() - 16384;
    const double v = vertical / 16384.0;
    const float pitch = static_cast<float>(limit * 0.1 * v * v * v);
    const float yaw = static_cast<float>(static_cast<double>(horizontal) * horizontal * horizontal * limit);
    const auto rotation = motion_orientation(yaw, pitch);
    const auto original = velocity;
    velocity = {static_cast<float>(static_cast<double>(rotation[0]) * original.x +
                                   static_cast<double>(rotation[3]) * original.y +
                                   static_cast<double>(rotation[6]) * original.z),
                static_cast<float>(static_cast<double>(rotation[1]) * original.x +
                                   static_cast<double>(rotation[4]) * original.y +
                                   static_cast<double>(rotation[7]) * original.z),
                static_cast<float>(static_cast<double>(rotation[2]) * original.x +
                                   static_cast<double>(rotation[5]) * original.y +
                                   static_cast<double>(rotation[8]) * original.z)};
}
Orientation spell_orientation(Vec3 velocity) {
    const auto normalize = [](Vec3 v) {
        const double magnitude = std::sqrt(static_cast<double>(v.x) * v.x + static_cast<double>(v.y) * v.y +
                                           static_cast<double>(v.z) * v.z);
        return scaled(v, 1.0 / magnitude); // Retains the scalar routine's NaNs for a zero direction.
    };
    const auto cross = [](Vec3 a, Vec3 b) {
        return Vec3{static_cast<float>(static_cast<double>(a.y) * b.z - static_cast<double>(a.z) * b.y),
                    static_cast<float>(static_cast<double>(a.z) * b.x - static_cast<double>(a.x) * b.z),
                    static_cast<float>(static_cast<double>(a.x) * b.y - static_cast<double>(a.y) * b.x)};
    };
    const auto forward = normalize(velocity);
    const double dot = forward.z;
    const double factor = dot * dot <= 0.75 ? 0 : (dot * dot - 0.75) * 4;
    const double weight = factor * factor;
    const Vec3 up{0, static_cast<float>(weight), static_cast<float>(1 - weight)};
    const auto right = normalize(cross(forward, up));
    const auto corrected_up = cross(right, forward);
    return {right.x,   right.y,        right.z,        forward.x,     forward.y,
            forward.z, corrected_up.x, corrected_up.y, corrected_up.z};
}
Vec3 spell_ballistic_velocity(Vec3 from, Vec3 to, float speed, int mode) {
    const double dx = static_cast<double>(to.x) - from.x, dy = static_cast<double>(to.y) - from.y;
    const float horizontal = static_cast<float>(std::sqrt(dx * dx + dy * dy));
    const double squared = static_cast<double>(horizontal) * horizontal;
    const auto firing_angle = [&](float velocity, float& angle) {
        const double fall = squared * .002725 / (2 * static_cast<double>(velocity) * velocity);
        const float rise = static_cast<float>(static_cast<double>(to.z) - from.z + fall);
        const double discriminant = squared - rise * fall * 4;
        if (rise == 0 || discriminant < 0)
            return false;
        angle = static_cast<float>(
            std::atan2(((mode ? -1 : 1) * std::sqrt(discriminant) + horizontal) / (rise + rise), 1.0));
        if (angle < 0)
            angle = static_cast<float>(angle + 3.1415);
        return true;
    };
    float angle = std::bit_cast<float>(1061752784u); // Native unreachable-shot fallback, pi/4.
    if (mode > 1) {
        float lower = 0, upper = speed + speed;
        const int trials = mode - 1;
        mode = 1;
        for (int i = 0; i < trials; ++i) {
            float bound;
            if (firing_angle(speed, angle)) {
                bound = lower;
                upper = speed;
            } else {
                bound = upper;
                lower = speed;
            }
            // This is an addition, not a midpoint, in 0x44d196..0x44d1a5.
            speed = static_cast<float>(static_cast<double>(bound) * .5 + speed);
        }
    }
    if (!firing_angle(speed, angle))
        angle = std::bit_cast<float>(1061752784u);
    if (horizontal == 0)
        return {0, 0, to.z >= from.z ? speed : -speed};
    const float factor = static_cast<float>(speed / std::sqrt(dx * dx + dy * dy) * std::sin(angle));
    return {static_cast<float>(factor * dx), static_cast<float>(factor * dy),
            static_cast<float>(std::cos(angle) * speed)};
}
MatchObject* spell_trace_ray(Match& match, const MatchObject& source, Vec3 from, Vec3& endpoint,
                             unsigned mask) {
    const Vec3 ray{endpoint.x - from.x, endpoint.y - from.y, endpoint.z - from.z};
    double nearest = 1.01;
    MatchObject* hit = nullptr;
    if (match.spatial)
        match.spatial->visit_ray(from, endpoint, mask, [&](const ObjectMotion& other) {
            if (source.source_motion() == &other || (source.owner && source.owner->source_motion() == &other))
                return 0;
            const Vec3 relative{from.x - other.position.x, from.y - other.position.y,
                                from.z - other.position.z};
            for (const auto& box : other.collision_boxes) {
                const double fraction = motion_segment_contact(relative, ray, box, 0);
                if (fraction >= 0 && fraction < nearest) {
                    nearest = fraction;
                    for (const auto& object : match.objects())
                        if (object->source_motion() == &other)
                            hit = object.get();
                }
            }
            return 0;
        });
    Vec3 ground;
    const float land = match.terrain ? terrain_intercept(*match.terrain, from, ray, ground) : -1;
    if (land > 0 && land < nearest) {
        endpoint = ground;
        return nullptr;
    }
    if (nearest <= 1)
        endpoint = {from.x + static_cast<float>(ray.x * nearest),
                    from.y + static_cast<float>(ray.y * nearest),
                    from.z + static_cast<float>(ray.z * nearest)};
    return hit;
}
double spell_box_separation(const ObjectMotion& a, const ObjectMotion& b) {
    if (a.collision_boxes.empty() || b.collision_boxes.empty())
        throw Error("Native first-box separation requires collision records");
    return motion_box_separation(a.position, a.collision_boxes.front(), b.position,
                                 b.collision_boxes.front());
}
void spell_exclusion_field(Match& match, Vec3 center, float inner, float outer, float speed, unsigned axes,
                           unsigned mask, const MatchObject* filter, unsigned mode) {
    const float inner_squared = inner * inner, outer_squared = outer * outer;
    static constexpr std::array<unsigned, 5> group_masks{4, 1, 16, 130, 96}; // 0x4c0468.
    for (unsigned group = 0; group < group_masks.size(); ++group) {
        if (!(group_masks[group] & mask))
            continue;
        match.visit_group(group, mask, [&](MatchObject& object) {
            auto* actor = dynamic_cast<CreatureActor*>(&object);
            if (!actor || (object.object_flags & 0x10000))
                return 0;
            if (filter && mode && !((mode & 1) ? filter->enemy(&object) : filter->friendly(&object)))
                return 0;
            Vec3 d;
            spell_target_point(d, actor);
            d = {d.x - center.x, d.y - center.y, d.z - center.z};
            double squared = 0;
            if (axes & 1)
                squared = static_cast<double>(d.x) * d.x;
            else
                d.x = 0;
            if (axes & 2)
                squared += static_cast<double>(d.y) * d.y;
            else
                d.y = 0;
            if (axes & 4)
                squared += static_cast<double>(d.z) * d.z;
            else
                d.z = 0;
            double weight = 1;
            if (squared > inner_squared) {
                if (!(squared < outer_squared))
                    return 0;
                const float inverse = static_cast<float>(1.0 / (outer_squared - inner_squared));
                weight = (outer_squared - squared) * inverse;
            }
            const double length = std::sqrt(static_cast<double>(d.z) * d.z + static_cast<double>(d.y) * d.y +
                                            static_cast<double>(d.x) * d.x);
            if (length != 0) {
                const double factor = speed / length;
                d = {static_cast<float>(factor * d.x), static_cast<float>(factor * d.y),
                     static_cast<float>(factor * d.z)};
            }
            const double fourth = weight * (weight * weight * weight);
            auto& v = actor->motion.velocity;
            if (axes & 1)
                v.x = static_cast<float>((d.x - v.x) * fourth + v.x);
            if (axes & 2)
                v.y = static_cast<float>((d.y - v.y) * fourth + v.y);
            if (axes & 4)
                v.z = static_cast<float>((d.z - v.z) * fourth + v.z);
            return 0;
        });
    }
}
} // namespace sacrifice
