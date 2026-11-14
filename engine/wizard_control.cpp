#include "sacrifice/wizard_control.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/scene.hpp"
#include "sacrifice/spell_registry.hpp"
#include "sacrifice/world_save.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
void configure_debug_wizard(Match& match, CreatureActor& actor, int god, int level) {
    WorldJournal::Scope recording(match, WorldJournal::Command::debug_wizard,
                                  {actor.reference(), god, level});
    match.assign(actor, 0);
    actor.abort_cast();
    actor.casting = {};
    actor.orders.clear();
    WizardProgression setup;
    setup.stats[18] = static_cast<std::int16_t>(god);
    setup.gods.fill(static_cast<std::uint8_t>(god));
    const auto grant = [&](WizardProgression& state) {
        if (match.spell_registry)
            match.spell_registry->grant(state);
    };
    grant(setup);
    setup.set(level, 0, grant);
    actor.adopt_progression(match, setup);
}

namespace {
float wrap(float angle) {
    while (angle > 3.14159f)
        angle -= 6.28318f;
    while (angle < -3.14159f)
        angle += 6.28318f;
    return angle;
}
// 0x411720: step an angle toward a target by at most `step`; returns the remaining difference.
float approach(float& angle, float target, float step) {
    const float difference = wrap(angle - target);
    if (step < difference) {
        angle = wrap(angle - step);
        return difference - step;
    }
    if (-step <= difference) {
        angle = target;
        return 0;
    }
    angle = wrap(angle + step);
    return -(step + difference);
}
} // namespace
// The cubic Bezier of 0x47fca0 over the control points at 0x4d23d0 with the basis at 0x4caf80.
std::array<float, 3> follow_curve(float t) {
    static constexpr std::array<std::array<float, 3>, 4> points{
        {{1, .9f, 0}, {2, 1.15f, -.1f}, {4, 2, -.2f}, {8, 3, -.3f}}};
    const float u = 1 - t;
    const std::array<float, 4> weights{u * u * u, 3 * u * u * t, 3 * u * t * t, t * t * t};
    std::array<float, 3> result{};
    for (std::size_t i = 0; i < 4; ++i)
        for (std::size_t c = 0; c < 3; ++c)
            result[c] += points[i][c] * weights[i];
    return result;
}
namespace {
// 0x44ecd0: LAND::GetTrueHeight, zero outside the map but the plane height over holes, so the camera does not
// drop under the land beside a void edge.
float ground(const Terrain& terrain, float x, float y) {
    return terrain_height(terrain, x, y, true);
}
} // namespace

KeyboardDevice::KeyboardDevice() {
    // The 'joys' rows of 0x4c78b8; 0x4051b0 itself binds the arrows and space (257) first.
    for (const auto& [key, mask] : std::array<std::pair<std::uint8_t, std::uint32_t>, 12>{{{17, forward_bit},
                                                                                          {200, forward_bit},
                                                                                          {31, back_bit},
                                                                                          {208, back_bit},
                                                                                          {30, left_bit},
                                                                                          {203, left_bit},
                                                                                          {32, right_bit},
                                                                                          {205, right_bit},
                                                                                          {13, throttle_up_bit},
                                                                                          {12, throttle_down_bit},
                                                                                          {78, throttle_up_bit},
                                                                                          {74, throttle_down_bit}}})
        bind(key, mask);
    bind(57, 257);
}
void KeyboardDevice::bind(std::uint8_t key, std::uint32_t mask) {
    masks_[key] = mask;
}
void KeyboardDevice::poll(const std::array<bool, 256>& keys) {
    previous_ = state_;
    state_ = keys;
    held_ = pressed_ = released_ = 0;
    for (std::size_t key = 0; key < 256; ++key) {
        if (state_[key])
            held_ |= masks_[key];
        if (state_[key] && !previous_[key])
            pressed_ |= masks_[key];
        if (!state_[key] && previous_[key])
            released_ |= masks_[key];
    }
}
std::array<int, 3> KeyboardDevice::axes() {
    const auto edge = pressed_ | released_;
    const auto axis = [&](std::uint32_t negative, std::uint32_t positive) {
        if (edge & negative)
            return -63;
        if (held_ & negative)
            return -127;
        if (edge & positive)
            return 63;
        return held_ & positive ? 127 : 0;
    };
    const int yaw = axis(left_bit, right_bit), forward = axis(forward_bit, back_bit);
    if (edge & throttle_up_bit)
        throttle_ += 2;
    else if (held_ & throttle_up_bit)
        throttle_ += 4;
    else if (edge & throttle_down_bit)
        throttle_ -= 2;
    else if (held_ & throttle_down_bit)
        throttle_ -= 4;
    throttle_ = std::clamp(throttle_, -127, 127);
    return {yaw, forward, throttle_};
}
bool KeyboardDevice::engaged() const {
    return (held_ | pressed_ | released_) != 0;
}

void FollowCamera::drag(CreatureActor& target, float dx, float dy, unsigned width, unsigned height,
                        float half_width, float half_height, bool end) {
    // 0x4810c0: drags set flag 4 and the drag end clears it into flag 2; byte_4dc020 bits 8/0x10 invert.
    flags = end ? (flags & ~4u) | 2 : flags | 4;
    if (invert_x)
        dx = -dx;
    if (invert_y)
        dy = -dy;
    target.view_yaw = wrap(target.view_yaw + 2 * dx * half_width * 3.1415f / (static_cast<float>(width) * 90));
    target.view_pitch = std::clamp(
        target.view_pitch + 2 * dy * half_height * 3.1415f / (static_cast<float>(height) * 90), -1.5f, 1.5f);
}
void FollowCamera::wheel(int delta) {
    zoom_target = std::clamp(zoom_target - static_cast<float>(delta) * .0005f, 0.0f, 1.0f);
}
void FollowCamera::edge_scroll(CreatureActor& target, Vec2 cursor, unsigned width, unsigned height, Vec2 motion,
                               int ticks, unsigned options, bool inside) {
    if (!inside)
        options = 0;
    const auto step = static_cast<float>(ticks) * .01f;
    bool turning = false;
    if (!(flags & 4)) {
        if (options & 1) {
            if (cursor.x <= 2) {
                turning = true;
                target.view_yaw = wrap(target.view_yaw + step - std::min(motion.x * .003f, 0.0f));
            } else if (cursor.x > static_cast<float>(width) - 2) {
                turning = true;
                target.view_yaw = wrap(target.view_yaw - (step + std::max(motion.x * .003f, 0.0f)));
            }
        }
        if (options & 2) {
            if (cursor.y <= 2)
                zoom_target -= step - std::min(motion.y * .001f, 0.0f);
            else if (cursor.y > static_cast<float>(height) - 2)
                zoom_target += step + std::max(motion.y * .001f, 0.0f);
        }
    }
    zoom_target = std::clamp(zoom_target, 0.0f, 1.0f);
    if (turning)
        flags = (flags | 16) & ~0xau;
    else if (flags & 16)
        flags = (flags & ~0x18u) | 8;
}
void CameraShake::request(Vec3 eye, Vec3 source, int amplitude, float radius) {
    const double x = source.x - eye.x, y = source.y - eye.y, z = source.z - eye.z;
    const double distance = std::sqrt(x * x + y * y + z * z);
    if (distance < radius)
        amount = std::max(amount, static_cast<int>(static_cast<std::int64_t>(
                                      (radius - distance) * amplitude / radius * 65536.0)));
}
void CameraShake::update(int ticks) {
    if (amount > 0)
        amount = std::max(amount - 8738 * ticks, 0);
}
Vec2 CameraShake::offset(CrtRandom& random) const {
    if (!amount)
        return {};
    const int pixels = (amount + 0x8000) >> 16;
    const int x = (pixels * (random.next() - 0x4000)) >> 14;
    const int y = (pixels * (random.next() - 0x4000)) >> 14;
    return {static_cast<float>(x), static_cast<float>(y)};
}

void FollowCamera::update(CreatureActor& target, const Terrain& terrain, int ticks, std::uint32_t held,
                          bool local) {
    // 0x4814a0: the throttle keys also zoom (window scrolling is `edge_scroll`).
    if (held & KeyboardDevice::throttle_up_bit)
        zoom_target -= static_cast<float>(ticks) * .01f;
    else if (held & KeyboardDevice::throttle_down_bit)
        zoom_target += static_cast<float>(ticks) * .01f;
    zoom_target = std::clamp(zoom_target, 0.0f, 1.0f);
    // 0x47f390 mode flag 2: ease the zoom by 0.03 per tick.
    const float step = static_cast<float>(ticks) * .03f;
    zoom = zoom < zoom_target ? std::min(zoom + step, zoom_target) : std::max(zoom - step, zoom_target);
    // 0x480b30: turning or standing recentres the view yaw on the body unless a drag holds it.
    const unsigned before = flags & 0x180;
    flags &= ~0x180u;
    if (local) {
        if (target.controls[0])
            flags |= 0x80;
        if (!target.controls[1])
            flags |= 0x100;
    }
    const unsigned now = flags & 0x180;
    if (flags & 0x14)
        flags &= ~1u;
    else if (flags & 0xa) {
        if (!local || ((before ^ now) & now))
            flags &= ~0xau;
        flags &= ~1u;
    } else {
        if (now)
            flags |= 1;
        if ((flags & 1) && approach(target.view_yaw, target.motion.yaw, static_cast<float>(ticks) * .05f) == 0)
            flags &= ~1u;
    }
    place(target, terrain, {target.view_yaw, target.view_pitch});
}
void FollowCamera::place(const CreatureActor& target, const Terrain& terrain, Vec2 angles) {
    // Native SAXS branch: GetCameraOffset, not animated world bounds.
    place(target.motion.position, target.camera_offset(), terrain, angles);
}
void FollowCamera::place(Vec3 target, Vec2 offset, const Terrain& terrain, Vec2 angles) {
    // 0x47fca0: behind the target along the given yaw, scaled by the body extent.
    const auto curve = follow_curve(zoom);
    const float distance = curve[0] * offset.x, height = curve[1] * offset.y * height_scale;
    yaw = angles.x;
    pitch = curve[2] + angles.y;
    position = {target.x + std::sin(yaw) * distance, target.y - std::cos(yaw) * distance, target.z + height};
    // Keep the camera's height over the land within 0.75..1.25 of the nominal offset.
    const float lift = target.z - ground(terrain, target.x, target.y);
    const float above = position.z - ground(terrain, position.x, position.y) - lift;
    const float low = height * .75f, high = height * 1.25f;
    if (above < low || above > high) {
        const float correction = (above < low ? low : high) - above;
        position.z += correction;
        if (distance > 0)
            pitch = curve[2] - std::atan2(correction, distance) + angles.y;
    }
}
} // namespace sacrifice
