#pragma once
#include "sacrifice/renderer.hpp"
#include "sacrifice/scene.hpp"
#include "sacrifice/trigger.hpp"
#include "sacrifice/wizard_control.hpp"
#include <algorithm>
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
    // One view zoom (+220 / +224) runs through the scene: play resumes at the zoom the scene left.
    follow.zoom = triggers.camera_zoom;
    follow.zoom_target = triggers.camera_zoom_target;
}
// Outside a cinematic the script's zoom is the follow camera's, so an object eye placed at the scene's
// start (0x47fca0 with +220) matches the view the player had.
inline void seed_script_zoom(Triggers& triggers, const FollowCamera& follow) {
    triggers.seed_zoom(follow.zoom, follow.zoom_target);
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
// Leaving a scene whose eye is not on the wizard (0x453450 -> 0x47f2c0, camera mode 1, flags 0x55) blends
// the view back over clamp(0.002 x distance to the wizard, 0.2, 1) seconds, eased by 0x43a240 and kept 0.5
// over the land (0x44df50). When the eye ended on the wizard, 0x480190 turns its view angles instead
// (Triggers) and there is no blend.
// Port repair: natively mode 1 aims at the wizard itself (0x47f110 with it), so 0x47f9e0's yaw from the aim
// to the wizard is 0 and the eye heads for a point south of it, then mode 0 cuts to the follow view. Here the
// blend runs from the scene's view to the follow view, so it ends where play continues.
struct SceneExitBlend {
    std::optional<Camera> scene; // The last scene view, while its eye was away from the wizard.
    std::uint32_t scene_tick{};
    Camera start;
    float progress{1}, seconds{1};
    void watch(const Triggers& triggers, const Camera& camera, std::uint32_t wizard, std::uint32_t tick) {
        if (triggers.camera_eye.object != wizard) {
            scene = camera;
            scene_tick = tick;
        } else
            scene.reset();
    }
    // After follow_view: blends `camera` (the follow view) from the scene's last view.
    void apply(Camera& camera, const CreatureActor& wizard, const Terrain& terrain, std::uint32_t tick, int ticks) {
        if (scene) {
            if (tick >= scene_tick && tick - scene_tick <= 12) {
                start = *scene;
                const auto& p = wizard.motion.position;
                const float dx = start.position.x - p.x, dy = start.position.z - p.y, dz = start.position.y - p.z;
                seconds = std::clamp(std::sqrt(dx * dx + dy * dy + dz * dz) * .002f, .2f, 1.0f);
                progress = 0;
            }
            scene.reset();
        }
        if (progress >= 1)
            return;
        progress = std::min(1.0f, progress + static_cast<float>(ticks) / (seconds * 60));
        if (progress >= 1)
            return;
        const float t = ((-1.4f * progress + 2.1f) * progress + .3f) * progress;
        const auto mix = [t](float a, float b) { return a + (b - a) * t; };
        Camera blended = camera;
        blended.position = {mix(start.position.x, camera.position.x), mix(start.position.y, camera.position.y),
                            mix(start.position.z, camera.position.z)};
        const float floor = terrain_height(terrain, blended.position.x, blended.position.z, true) + .5f;
        blended.position.y = std::max(blended.position.y, floor);
        blended.yaw = start.yaw + std::remainder(camera.yaw - start.yaw, 2 * std::numbers::pi_v<float>) * t;
        blended.pitch = mix(start.pitch, camera.pitch);
        camera = blended;
    }
};
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
