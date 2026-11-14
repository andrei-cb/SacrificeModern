#include "sacrifice/creature_actor.hpp"
#include <algorithm>

namespace sacrifice {
std::optional<SaxsAttachment> CreatureActor::closest_bone(Vec3 point) {
    if (!body_ || body_->model.bones.empty())
        return {};
    update_pose();
    const auto& o = motion.orientation;
    const Vec3 delta{point.x - motion.position.x, point.y - motion.position.y, point.z - motion.position.z};
    return saxs_closest_bone(body_->model, *pose_,
                             {delta.x * o[0] + delta.y * o[1] + delta.z * o[2],
                              delta.x * o[3] + delta.y * o[4] + delta.z * o[5],
                              delta.x * o[6] + delta.y * o[7] + delta.z * o[8]});
}
void CreatureActor::request_bone_scale(unsigned bone, Vec3 scale) {
    if (bone >= bone_count())
        return;
    auto& state = bone_scales_.at(bone);
    if (state.requests) {
        state.target.x += scale.x;
        state.target.y += scale.y;
        state.target.z += scale.z;
    } else
        state.target = scale;
    ++state.requests;
    state.active = true;
}
void CreatureActor::copy_bone_scales() {
    if (!pose_)
        return;
    pose_->bone_scales.clear();
    if (std::ranges::none_of(bone_scales_, [](const auto& state) { return state.active; }))
        return;
    pose_->bone_scales.reserve(pose_->bones.size());
    for (unsigned bone = 0; bone < pose_->bones.size(); ++bone)
        pose_->bone_scales.push_back(bone_scales_[bone].current);
}
void CreatureActor::advance_bone_scales(int ticks) {
    bool changed = false;
    for (auto& state : bone_scales_) {
        if (!state.active)
            continue;
        if (state.requests > 1) {
            const double inverse = 1.0 / state.requests;
            state.target = {static_cast<float>(state.target.x * inverse),
                            static_cast<float>(state.target.y * inverse),
                            static_cast<float>(state.target.z * inverse)};
            state.requests = 1;
        }
        const auto old = state.current;
        const float step = static_cast<float>(ticks * (state.requests ? .01 : .05));
        const auto approach = [step](float current, float target) {
            return current < target ? std::min(current + step, target) : std::max(current - step, target);
        };
        state.current = {approach(old.x, state.target.x), approach(old.y, state.target.y),
                         approach(old.z, state.target.z)};
        if (old.x != state.current.x || old.y != state.current.y || old.z != state.current.z)
            changed = true;
        if (state.current.x == state.target.x && state.current.y == state.target.y &&
            state.current.z == state.target.z) {
            if (state.requests) {
                state.target = {1, 1, 1};
                state.requests = 0;
            } else
                state.active = false;
        }
    }
    if (changed) {
        copy_bone_scales();
        pose_changed_ = bounds_dirty_ = true;
        ++pose_revision_; // Frozen/unchanged animation frames still need new mesh and bounds.
    }
}
} // namespace sacrifice
