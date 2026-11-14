#include "sacrifice/creature_actor.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
int integer(double value) {
    if (!std::isfinite(value) || value >= 9223372036854775808.0 || value < -9223372036854775808.0)
        return 0;
    return std::bit_cast<int>(static_cast<unsigned>(static_cast<std::int64_t>(value)));
}
} // namespace
int CreatureActor::approach_order(Match& match, Vec3 goal, float radius, float yaw, float face_yaw,
                                  int budget) {
    const auto flags = fsm_->state() ? fsm_->state()->flags : 0;
    if (!(flags & 1) || !network_identity || (state_flags & 0x80000))
        return 0;
    if (approach_result) {
        const double dx = static_cast<double>(goal.x) - path.target.x,
                     dy = static_cast<double>(goal.y) - path.target.y;
        const double moved = dx * dx + dy * dy;
        if (approach_result < 0) {
            if (moved < 4 && approach_blocker) {
                const auto& other = *approach_blocker->source_motion();
                if (motion_box_separation(motion.position, {motion.box_min, motion.box_max}, other.position,
                                          {other.box_min, other.box_max}) < 1)
                    return approach_result;
            }
        } else if (moved == 0) {
            const double x = static_cast<double>(goal.x) - motion.position.x,
                         y = static_cast<double>(goal.y) - motion.position.y;
            if (static_cast<double>(radius) * radius > x * x + y * y) {
                if (face_yaw > 0)
                    face_order_yaw(yaw);
                return approach_result;
            }
        }
    }
    approach_blocker = nullptr;
    approach_result = steer_order(match, goal, radius, budget);
    if (approach_result > 0 && face_yaw > 0)
        face_order_yaw(yaw);
    return approach_result;
}
int CreatureActor::steer_order(Match& match, Vec3 goal, float radius, int budget) {
    const auto flags = fsm_->state() ? fsm_->state()->flags : 0;
    OrderAim aim;
    if (flags & 2) {
        const auto* land = terrain ? terrain : match.terrain;
        if (!land)
            throw Error("Native flying approach requires terrain");
        const double dx = static_cast<double>(goal.x) - path.target.x,
                     dy = static_cast<double>(goal.y) - path.target.y;
        if (dx * dx + dy * dy > 4) {
            path.target.x = goal.x;
            path.target.y = goal.y;
        }
        float angle = face_order_point(goal, &aim);
        if (static_cast<double>(radius) * radius >
            static_cast<double>(aim.delta.z) * aim.delta.z + aim.horizontal_squared) {
            controls = {};
            return 1;
        }
        const float ahead_x = static_cast<float>(static_cast<double>(motion.orientation[3]) *
                                                     motion.local_velocity.y * 40 +
                                                 motion.position.x),
                    ahead_y = static_cast<float>(static_cast<double>(motion.orientation[4]) *
                                                     motion.local_velocity.y * 40 +
                                                 motion.position.y);
        const float horizontal = static_cast<float>(std::sqrt(aim.horizontal_squared));
        const float target_ground = static_cast<float>(terrain_true_height(*land, goal.x, goal.y));
        const double elevation = static_cast<double>(goal.z) - target_ground;
        const float stored_elevation = static_cast<float>(elevation);
        const double lower_delta = elevation - 5, lower_distance = std::abs(lower_delta) * 3;
        const float upper_delta = static_cast<float>(static_cast<double>(stored_elevation) - 200),
                    upper_distance = static_cast<float>(std::abs(static_cast<double>(upper_delta)) * 3);
        const float low = horizontal >= lower_distance
                              ? 5
                              : static_cast<float>(
                                    (lower_distance - horizontal) * lower_delta / lower_distance + 5),
                    high = horizontal >= upper_distance
                               ? 200
                               : static_cast<float>((static_cast<double>(upper_distance) - horizontal) *
                                                        upper_delta / upper_distance +
                                                    200);
        const float ahead_ground = static_cast<float>(terrain_true_height(*land, ahead_x, ahead_y));
        const double gap = static_cast<double>(motion.position.z) - ahead_ground;
        const float stored_gap = static_cast<float>(gap);
        double pitch = 0;
        if (gap < low)
            pitch = std::min((low - stored_gap) * 0.1, 0.5);
        else if (stored_gap > high)
            pitch = std::max((static_cast<double>(high) - stored_gap) * 0.1, -0.5);
        if (pitch == motion.pitch)
            controls[1] = 0;
        else {
            const double value = (pitch - motion.pitch) * 256;
            const float stored = static_cast<float>(value);
            controls[1] = value < -127 ? -127 : stored > 127 ? 127 : integer(stored);
        }
        const int avoid = avoid_obstacles(match, 3, goal, 30, &approach_blocker);
        if (avoid == 3)
            return -1;
        if (avoid > 0 && avoid <= 2)
            angle = 0;
        const int forward = horizontal < 10 ? integer(horizontal * 12.8) : 128;
        if (angle > 1.57)
            controls[2] = forward / 2;
        else if (angle >= 0.78)
            controls[2] = integer(((1.57 - angle) * 0.6369426751592356 + 0.5) * forward);
        else
            controls[2] = forward;
        return 0;
    }
    float angle;
    std::optional<Vec3> waypoint;
    if (flags & 0x100)
        angle = face_order_point(goal, &aim);
    else {
        if (!match.navigation)
            throw Error("Native ground approach requires navigation");
        Vec3 next;
        const int result = match.navigation->step(path, motion.position, goal, next, budget);
        if (result == -1) {
            controls[0] = controls[1] = 0;
            return -1;
        }
        if (result) {
            next.z = motion.position.z;
            waypoint = next;
            angle = face_order_point(next, &aim);
            const double dx = static_cast<double>(motion.position.x) - goal.x,
                         dy = static_cast<double>(motion.position.y) - goal.y;
            aim.horizontal_squared = static_cast<float>(dx * dx + dy * dy);
        } else
            angle = face_order_point(goal, &aim);
    }
    // The original direct-ground branches leave this scratch point uninitialized. Initialize it to
    // the destination so the x64 port can steer that branch without reading indeterminate memory.
    const int avoid =
        avoid_obstacles(match, 1, waypoint ? waypoint : std::optional<Vec3>(goal), 15, &approach_blocker);
    if (avoid == 3)
        return -1;
    if (avoid > 0 && avoid <= 2)
        angle = 0;
    // Reconstruction deviation: with every probe blocked (2) by a non-enemy creature, nudge that
    // creature aside. Natively an escort ordered onto its leader (0x48ce30 order 7) can pin it for good.
    if (avoid == 2 && approach_blocker && (approach_blocker->kind_flags & 5) && !enemy(approach_blocker))
        if (auto* other = const_cast<ObjectMotion*>(approach_blocker->source_motion())) {
            const Vec3 away{other->position.x - motion.position.x, other->position.y - motion.position.y, 0};
            other->push_direction(away.x || away.y ? away : Vec3{1, 0, 0}, .1f, 3);
        }
    if (static_cast<double>(radius) * radius >= aim.horizontal_squared)
        return 1;
    if (aim.horizontal_squared > 1600 && (status_flags & 0x200)) {
        fsm_->event(16);
        return 0;
    }
    const double fraction = std::clamp((angle - 0.1745327777777778) * 0.4583666232703823, 0.0, 1.0);
    const int forward = integer((1 - fraction) * (1 - fraction) * -127);
    controls[1] = forward;
    const double turn = static_cast<double>(angle) * angle * 20 * 1.513622603430878, squared = turn * turn;
    if (aim.horizontal_squared < squared)
        controls[1] = integer(aim.horizontal_squared / squared * forward);
    return 0;
}
} // namespace sacrifice
