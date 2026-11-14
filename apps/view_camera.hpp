#pragma once
#include "sacrifice/renderer.hpp"
#include "sacrifice/trigger.hpp"
#include "sacrifice/wizard_control.hpp"
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>

// The playing view shared by the desktop and browser builds: the scripted camera tracks follow the view
// outside cinematics, a cinematic shows the script's eye and aim, and play shows the follow camera.
namespace sacrifice {
// Outside a cinematic the script's eye and aim follow the view, so a camera move (0x49b0b0 actions) starts
// from what the player sees. The aim is 50 units along the view direction.
inline void seed_script_camera(Triggers& triggers, const Camera& camera) {
    constexpr float distance = 50;
    triggers.seed_camera({camera.position.x, camera.position.z, camera.position.y},
                         {camera.position.x + std::sin(camera.yaw) * std::cos(camera.pitch) * distance,
                          camera.position.z + std::cos(camera.yaw) * std::cos(camera.pitch) * distance,
                          camera.position.y + std::sin(camera.pitch) * distance});
}
// The playing follow camera: 0.85 of the native height factor is a viewer preference, not a recovered value.
inline FollowCamera playing_follow() {
    FollowCamera follow;
    follow.height_scale = .85f;
    return follow;
}
// Restarts the follow camera at the wizard (leaving a cinematic), keeping the player's view options.
inline void reset_follow(FollowCamera& follow) {
    const auto height = follow.height_scale;
    const bool invert_x = follow.invert_x, invert_y = follow.invert_y;
    follow = {};
    follow.height_scale = height;
    follow.invert_x = invert_x;
    follow.invert_y = invert_y;
}
// During a cinematic the view is the script's eye looking at its aim.
inline void cinematic_view(const Triggers& triggers, Camera& camera, FollowCamera& follow) {
    const auto eye = triggers.camera_eye.point, aim = triggers.camera_aim.point;
    camera.position = {eye.x, eye.z, eye.y};
    const auto dx = aim.x - eye.x, dy = aim.y - eye.y, dz = aim.z - eye.z;
    if (dx * dx + dy * dy + dz * dz > .0001f) {
        camera.yaw = std::atan2(dx, dy);
        camera.pitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy));
    }
    reset_follow(follow);
}
// The follow camera behind the local wizard for `ticks`, taking a scripted zoom (action 121) when the
// script changes it.
inline void follow_view(FollowCamera& follow, CreatureActor& wizard, const Terrain& terrain, const Triggers& triggers,
                        unsigned& zoom_revision, int ticks, std::uint32_t held, Camera& camera) {
    if (zoom_revision != triggers.camera_zoom_revision) {
        zoom_revision = triggers.camera_zoom_revision;
        follow.zoom_target = triggers.camera_zoom_target;
    }
    follow.update(wizard, terrain, ticks, held, true);
    camera.position = {follow.position.x, follow.position.z, follow.position.y};
    camera.yaw = -follow.yaw;
    camera.pitch = follow.pitch;
}
// Presentation only (not native): the tick-driven view (follow camera or cinematic) is drawn between its
// pose before the last match tick and its current pose, by Match::presentation_blend, so it moves smoothly
// on displays faster than the 60 Hz tick. Free and inspector cameras pass through.
struct CameraBlend {
    std::optional<Camera> previous, current;
    std::uint32_t tick{};
    Camera shown(const Camera& live, std::uint32_t match_tick, float blend, bool tick_driven) {
        if (!tick_driven) {
            previous.reset();
            current.reset();
            return live;
        }
        if (!current || match_tick != tick) {
            previous = current ? current : live;
            tick = match_tick;
        }
        current = live;
        const auto dx = live.position.x - previous->position.x, dy = live.position.y - previous->position.y,
                   dz = live.position.z - previous->position.z;
        if (dx * dx + dy * dy + dz * dz > 50.f * 50.f)
            return live; // A cut, not a move.
        auto result = live;
        const auto mix = [blend](float a, float b) { return a + (b - a) * blend; };
        result.position = {mix(previous->position.x, live.position.x), mix(previous->position.y, live.position.y),
                           mix(previous->position.z, live.position.z)};
        auto turn = std::remainder(live.yaw - previous->yaw, 2 * std::numbers::pi_v<float>);
        result.yaw = previous->yaw + turn * blend;
        result.pitch = mix(previous->pitch, live.pitch);
        return result;
    }
};
} // namespace sacrifice
