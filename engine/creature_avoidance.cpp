#include "sacrifice/creature_actor.hpp"
#include "sacrifice/spatial.hpp"
#include <cmath>

namespace sacrifice {
namespace {
Vec3 transform(const Orientation& o, Vec3 v) {
    return {static_cast<float>(static_cast<double>(o[0]) * v.x + static_cast<double>(o[3]) * v.y +
                               static_cast<double>(o[6]) * v.z),
            static_cast<float>(static_cast<double>(o[1]) * v.x + static_cast<double>(o[4]) * v.y +
                               static_cast<double>(o[7]) * v.z),
            static_cast<float>(static_cast<double>(o[2]) * v.x + static_cast<double>(o[5]) * v.y +
                               static_cast<double>(o[8]) * v.z)};
}
} // namespace
bool CreatureActor::probe_movement(Match& match, float yaw, float pitch, float length, float* height,
                                   MatchObject** blocker) {
    const auto* land = terrain ? terrain : match.terrain;
    if (!land)
        throw Error("Native movement probe requires terrain");
    Vec3 delta = transform(motion.orientation, motion.local_velocity);
    if (yaw != 0 || pitch != 0)
        delta = transform(motion_yaw_pitch(yaw, pitch), delta);
    delta = {static_cast<float>(static_cast<double>(length) * delta.x),
             static_cast<float>(static_cast<double>(length) * delta.y),
             static_cast<float>(static_cast<double>(length) * delta.z)};
    const ObjectMotion* hit = nullptr;
    if (motion_hooks.collide)
        hit = motion_hooks.collide(motion, delta, nullptr, nullptr);
    else if (match.spatial)
        hit = match.spatial->collide(motion, delta, nullptr);
    MatchObject* object = nullptr;
    if (hit) {
        for (const auto& candidate : match.objects())
            if (candidate->source_motion() == hit) {
                object = candidate.get();
                break;
            }
        if (!object)
            throw Error("Native movement probe collided with an unbound match object");
    }
    // The ordered target is ignored only after collision shortened delta; do not undo that rewrite.
    if (object == orders.target())
        object = nullptr;
    if (blocker)
        *blocker = object;
    const Vec3 point{motion.position.x + delta.x, motion.position.y + delta.y, motion.position.z + delta.z};
    const double ground = terrain_surface(*land, point.x, point.y);
    if (height)
        *height = static_cast<float>(ground);
    const auto flags = fsm_->state() ? fsm_->state()->flags : 0;
    return (ground <= 0 && !(flags & 2)) || object;
}
int CreatureActor::avoid_obstacles(Match& match, unsigned flags, std::optional<Vec3> goal, float length,
                                   MatchObject** blocker) {
    MatchObject* hit = nullptr;
    if (blocker)
        *blocker = nullptr;
    if (!probe_movement(match, 0, 0, length, nullptr, &hit)) {
        if (blocker)
            *blocker = hit;
        const double next = static_cast<double>(path.avoidance) - 0.1;
        path.avoidance = next < 0 ? 0 : static_cast<float>(next);
        return 0;
    }
    if (blocker)
        *blocker = hit;
    if (hit) {
        const auto velocity = transform(motion.orientation, motion.local_velocity);
        const auto& moved = hit->source_motion()->displacement;
        const double speed_squared = static_cast<double>(velocity.z) * velocity.z +
                                     static_cast<double>(velocity.y) * velocity.y +
                                     static_cast<double>(velocity.x) * velocity.x;
        const double dot = static_cast<double>(velocity.z) * moved.z +
                           static_cast<double>(velocity.y) * moved.y +
                           static_cast<double>(velocity.x) * moved.x;
        if (!(speed_squared * 0.9 >= dot))
            return 0;
        if (goal) {
            bool occupied = motion_contains_point(*hit->source_motion(), *goal);
            if (!occupied && (flags & 1) && (hit->kind_flags & 5) && !enemy(hit)) {
                const double dx = static_cast<double>(motion.position.x) - goal->x,
                             dy = static_cast<double>(motion.position.y) - goal->y;
                const auto* target = orders.target();
                if (dx * dx + dy * dy < 225 && match.spatial)
                    occupied = match.spatial->occupied(*goal, 5, &motion,
                                                       target ? target->source_motion() : nullptr);
            }
            if (occupied) {
                if (!match.navigation || match.navigation->near_goal(path)) {
                    if (flags & 1)
                        return 3;
                } else
                    match.navigation->discard_waypoint(path);
            }
        }
    }
    controls[0] = 0;
    const auto success = [&] {
        path.avoidance = static_cast<float>(static_cast<double>(path.avoidance) + 1);
        return 1;
    };
    if ((flags & 2) && !probe_movement(match, 0, 0.75f, length)) {
        controls[1] = 127;
        return success();
    }
    for (float angle = 0.5f; angle < 2.75f; angle = static_cast<float>(angle + 0.5)) {
        if (!probe_movement(match, angle, 0, length)) {
            controls[0] = -127;
            return success();
        }
        if (!probe_movement(match, -angle, 0, length)) {
            controls[0] = 127;
            return success();
        }
        if (flags & 2) {
            if (!probe_movement(match, angle, 0.75f, length)) {
                controls[0] = -127;
                controls[1] = 127;
                return success();
            }
            if (!probe_movement(match, -angle, 0.75f, length)) {
                controls[0] = 127;
                controls[1] = 127;
                return success();
            }
        }
    }
    return 2;
}
} // namespace sacrifice
