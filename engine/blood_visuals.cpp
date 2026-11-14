#include "sacrifice/spell_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include <cmath>

namespace sacrifice {
// 0x45ce10: append to the creature's +2240 host; the preset preserves its new frame and velocity box.
SpellActor* SpellActor::emit_blood(Match& match, CreatureActor& victim, MatchObject* source, int count, float speed) {
    auto* host = victim.blood_host;
    if (!host) {
        host = &static_cast<SpellActor&>(match.create(MatchObject::spell,
            [] { return std::make_unique<SpellActor>(); }, &victim,
            [](MatchObject& object) { static_cast<SpellActor&>(object).effect = Effect::blood; }));
        host->owner = host->effect_target = &victim;
        host->object_flags |= 0x10000;
        victim.blood_host = host;
        host->run_blood(match, 1); // Native immediate initialization, without advancing the emitter clock.
    }
    Vec3 direction;
    if (const auto* from = source ? source->source_motion() : nullptr) {
        direction = {victim.motion.position.x - from->position.x, victim.motion.position.y - from->position.y,
                     victim.motion.position.z - from->position.z};
        const auto length = std::sqrt(static_cast<double>(direction.x) * direction.x +
                                      static_cast<double>(direction.y) * direction.y +
                                      static_cast<double>(direction.z) * direction.z);
        if (length)
            direction = {static_cast<float>(direction.x / length), static_cast<float>(direction.y / length),
                         static_cast<float>(direction.z / length)};
    }
    const auto scale = speed / 60.0;
    const float low[3]{static_cast<float>((direction.x - .5) * scale),
                       static_cast<float>((direction.y - .5) * scale), static_cast<float>(direction.z * scale)},
                high[3]{static_cast<float>((direction.x + .5) * scale),
                        static_cast<float>((direction.y + .5) * scale), static_cast<float>((direction.z + 1) * scale)};
    auto& p = host->particles(match);
    p.set_property(3, low, high);
    const ParticleSystem::Frame frame{victim.motion.orientation, victim.motion.position};
    p.set_frame(frame, &frame);
    Vec3 center;
    spell_target_point(center, &victim);
    const float at[3]{center.x, center.y, center.z};
    p.set_property(2, at, at);
    host->particle_material(match, p, 0x0a011008, "blud");
    const float preset[3]{.25f, std::bit_cast<float>(count), std::bit_cast<float>(match.blood_solid ? 1u : 0u)};
    p.set_property(133, preset, preset); // Native overwrites the temporary spawn box, but retains the frame.
    return host;
}
int SpellActor::run_blood(Match& match, int ticks) {
    auto& p = particles(match);
    if (!stage) {
        particle_material(match, p, 0x10000, "blud");
        if (const auto* target = effect_target ? effect_target->source_motion() : nullptr) {
            const ParticleSystem::Frame frame{target->orientation, target->position};
            p.set_frame(frame, &frame);
        }
        host.time = 0;
        stage = 1;
        return 1;
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
