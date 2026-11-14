#include "sacrifice/creature_actor.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/sound.hpp"
#include <algorithm>

namespace sacrifice {
unsigned CreatureActor::bone_count() const {
    return body_ ? static_cast<unsigned>(body_->model.bones.size()) : 0;
}
unsigned CreatureActor::bone_parent(unsigned bone) const {
    return body_->model.bones.at(bone).parent;
}
bool CreatureActor::write_bone_point(Vec3& point, unsigned bone, CrtRandom* random, const Vec3* offset) {
    if (!body_)
        return false;
    update_pose();
    const auto local = offset ? saxs_reference_point(*pose_, {static_cast<std::int16_t>(bone), 0, *offset})
                              : saxs_bone_point(body_->model, *pose_, bone, random);
    const auto& o = motion.orientation;
    point = {static_cast<float>(static_cast<double>(local.x) * o[0] + static_cast<double>(local.y) * o[3] +
                                static_cast<double>(local.z) * o[6] + motion.position.x),
             static_cast<float>(static_cast<double>(local.x) * o[1] + static_cast<double>(local.y) * o[4] +
                                static_cast<double>(local.z) * o[7] + motion.position.y),
             static_cast<float>(static_cast<double>(local.x) * o[2] + static_cast<double>(local.y) * o[5] +
                                static_cast<double>(local.z) * o[8] + motion.position.z)};
    return true;
}
void CreatureActor::burn_bone(unsigned bone, int ticks) {
    if (ticks <= 0)
        permanent_burns |= 1u << (bone & 31);
    else {
        burn_ticks = std::max(burn_ticks, ticks);
        temporary_burns |= 1u << (bone & 31);
    }
    if (!flame_host)
        flame_host = &SpellActor::launch_burning_limbs(*match_, *this);
}
void CreatureActor::burn_near(Vec3 position, float radius, int ticks) {
    if (!alive())
        return;
    burn_requests.push_back({position, radius, ticks});
    burn_ticks = std::max(burn_ticks, ticks);
    if (!body_ || !bone_count())
        return; // Synthetic actors have no SAXS model to ignite.
    update_pose();
    const Vec3 delta{position.x - motion.position.x, position.y - motion.position.y, position.z - motion.position.z};
    const auto& o = motion.orientation;
    const Vec3 local{delta.x * o[0] + delta.y * o[1] + delta.z * o[2],
                     delta.x * o[3] + delta.y * o[4] + delta.z * o[5],
                     delta.x * o[6] + delta.y * o[7] + delta.z * o[8]};
    const float squared_radius = radius * radius;
    float nearest = 100000000;
    unsigned closest = 0;
    for (unsigned bone = 0; bone < bone_count(); ++bone) {
        const auto p = saxs_bone_point(body_->model, *pose_, bone);
        const float x = local.x - p.x, y = local.y - p.y, z = local.z - p.z;
        const double squared = static_cast<double>(x) * x + static_cast<double>(y) * y + static_cast<double>(z) * z;
        if (squared < nearest || bone == 0) {
            nearest = static_cast<float>(squared);
            closest = bone;
        }
        if (squared < squared_radius)
            burn_bone(bone, ticks);
    }
    if (!temporary_burns)
        burn_bone(closest, ticks);
}
SpellActor& SpellActor::launch_burning_limbs(Match& match, CreatureActor& victim) {
    const auto group = FourCC::from_text("flam");
    const auto sample = match.sound_set ? match.sound_set->select(group, match.random()) : FourCC{};
    auto& fire = static_cast<SpellActor&>(match.create(MatchObject::spell,
        [] { return std::make_unique<SpellActor>(); }, &victim,
        [](MatchObject& object) { static_cast<SpellActor&>(object).effect = Effect::burning_limbs; }));
    fire.owner = fire.effect_target = &victim;
    fire.object_flags |= 0x10000;
    if (sample.value)
        fire.sound_requests.push_back({group, sample, victim.reference(), 0x2000006});
    return fire;
}
int SpellActor::run_burning_limbs(Match& match, int ticks) {
    auto& p = particles(match);
    if (!stage) { // 0x45c830, capacity 256, additive firy, preset 128 {0.3, 1, 0.5}.
        particle_material(match, p, 0x1a012008, "firy");
        const float preset[3]{.30000001f, 1, .5f};
        p.set_property(128, preset, preset);
        p.clear(256);
        stage = 1;
    }
    if (auto* victim = dynamic_cast<CreatureActor*>(effect_target)) {
        victim->state_flags &= ~0x200u;
        if (!(victim->permanent_burns | victim->temporary_burns)) {
            if (victim->fsm().state()->flags & 8)
                victim->fsm().event(23);
            victim->burn_ticks = 0;
        } else {
            victim->state_flags |= 0x200;
            victim->burn_ticks -= ticks;
            if (victim->burn_ticks <= 0) {
                victim->burn_ticks = 0;
                victim->temporary_burns = 0;
            }
            const auto bones = victim->bone_count();
            for (unsigned i = 0; bones && i < bones / 3 + 1; ++i) {
                const auto bone = match.random().next() % bones;
                const auto bit = 1u << (bone & 31);
                if ((victim->permanent_burns | victim->temporary_burns) & bit) {
                    victim->write_bone_point(motion.position, bone, &match.visual_random);
                    p.set_position(motion.position, &motion.position);
                    p.emit(0, 1);
                    if ((victim->temporary_burns & bit) && !(match.random().next() & 31)) {
                        if (!victim->burn_ticks || !victim->alive())
                            victim->temporary_burns &= ~bit;
                        else
                            victim->temporary_burns |= 1u << (victim->bone_parent(bone) & 31);
                    }
                }
            }
        }
    }
    host.time += ticks;
    p.advance(host.time, 0);
    if (!p.alive()) {
        state_flags |= removed_flag;
        return -2;
    }
    return 1;
}
} // namespace sacrifice
