#include "sacrifice/creature_actor.hpp"
#include <cmath>

namespace sacrifice {
namespace {
int integer(double value) {
    if (!std::isfinite(value) || value >= 9223372036854775808.0 || value < -9223372036854775808.0)
        return 0;
    return std::bit_cast<int>(static_cast<unsigned>(static_cast<std::int64_t>(value)));
}
} // namespace
float CreatureActor::face_order_point(Vec3 point, OrderAim* result) {
    OrderAim aim;
    aim.delta = {point.x - motion.position.x, point.y - motion.position.y, point.z - motion.position.z};
    const auto& o = motion.orientation;
    const auto& d = aim.delta;
    aim.horizontal_squared =
        static_cast<float>(static_cast<double>(d.x) * d.x + static_cast<double>(d.y) * d.y);
    const float x = static_cast<float>(static_cast<double>(d.x) * o[0] + static_cast<double>(d.y) * o[1] +
                                       static_cast<double>(d.z) * o[2]);
    const float y = static_cast<float>(static_cast<double>(d.x) * o[3] + static_cast<double>(d.y) * o[4] +
                                       static_cast<double>(d.z) * o[5]);
    aim.angle = static_cast<float>(std::atan2(-static_cast<double>(x), y));
    const float absolute = std::abs(aim.angle);
    if (network_identity) {
        if (absolute >= 0.5235983333333333)
            controls[0] = aim.angle >= 0 ? -127 : 127;
        else
            controls[0] = integer(aim.angle * 1.909860930293259 * -127.0);
    }
    if (result)
        *result = aim;
    return absolute;
}
float CreatureActor::face_order_yaw(float yaw) {
    double delta = static_cast<double>(yaw) - motion.yaw;
    while (delta > 3.14159)
        delta -= 6.28318;
    while (delta <= -3.14159)
        delta += 6.28318;
    const float absolute = static_cast<float>(std::abs(delta));
    if (network_identity) {
        if (absolute >= 0.5235983333333333)
            controls[0] = delta >= 0 ? -127 : 127;
        else
            controls[0] = integer(delta * 1.909860930293259 * -127.0);
    }
    return absolute;
}
} // namespace sacrifice
