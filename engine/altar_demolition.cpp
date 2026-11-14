#include "sacrifice/building_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
SpellActor& SpellActor::launch_altar_ruin(Match& match, BuildingActor& part) {
    auto& spell = static_cast<SpellActor&>(match.create(
        MatchObject::spell, [] { return std::make_unique<SpellActor>(); }, &part,
        [](MatchObject& object) {
            auto& spell = static_cast<SpellActor&>(object);
            spell.effect = Effect::altar_ruin;
            spell.data.reset();
        }));
    spell.owner = &part;
    spell.effect_target = &part;
    spell.motion.position = part.motion.position;
    spell.motion.orientation = part.motion.orientation;
    spell.object_flags |= 0x10000;
    spell.ruin.tower = part.part_name() == FourCC{1414484001u}; // TOW!.
    if (part.allegiance)
        part.allegiance->attach(spell);
    spell.run(match, 0); // 0x441bf0 mode 1.
    return spell;
}
int SpellActor::run_altar_ruin(Match& match, int ticks) {
    auto* part = dynamic_cast<BuildingActor*>(effect_target);
    const auto remove = [&] {
        state_flags |= removed_flag;
        return -2;
    };
    if (!part && (ruin.tower || !stage))
        return remove();
    if (!stage) {
        visual_requests.push_back({VisualRequest::Kind::altar_demolition});
        stage = 1;
        if (ruin.tower)
            part->motion.flags = part->object_flags = 56;
        else {
            auto& p = particles(match);
            const auto& low = part->motion.box_min;
            const auto& high = part->motion.box_max;
            const double x = low.x - high.x, y = low.y - high.y;
            const auto radius = std::sqrt(x * x + y * y) * .5;
            const auto size = static_cast<float>(std::clamp(radius * .2, .5, 2.0));
            const float preset[3]{size, static_cast<float>(size + radius), 2};
            particle_material(match, p, 0x1a012008, "glo2");
            p.set_property(137, preset, preset);
            p.set_flags(0x2000401, 0);
            host.rate = 1;
        }
    }
    if (ruin.tower) {
        if (stage == 1) {
            ruin.progress = static_cast<float>(ruin.progress + ticks * (1.0 / 240.0));
            if (ruin.progress >= 1) {
                ruin.progress = ruin.scale = 1;
                stage = 2;
                part->motion.flags |= 1;
                part->object_flags |= 1;
                part->motion.velocity = {};
            }
            match.shake(motion.position, static_cast<int>(static_cast<std::int64_t>(ruin.progress * 16.0)), 150);
        } else if (stage == 2) {
            part->motion.velocity.z = 1;
            part->model_scale = static_cast<float>(std::sqrt(ruin.scale));
            ruin.scale = static_cast<float>(ruin.scale - ticks * (1.0 / 240.0));
            if (ruin.scale <= 0) {
                auto& ring = launch_sacrifice_host(match, *this, Effect::ruin_ring);
                ring.effect_target = nullptr;
                launch_fireball_visual(match, *this, Effect::fireball_blast);
                part->state_flags |= removed_flag;
                return remove();
            }
        }
        spell_target_point(motion.position, part);
        part->motion.angular_velocity.x = static_cast<float>(ruin.progress * .05235983333333333);
        part->motion.pitch = static_cast<float>((match.random().next() - 0x4000) *
                                                (ruin.progress * .1745327777777778) * .00006103515625);
        part->motion.roll = static_cast<float>((match.random().next() - 0x4000) *
                                               (ruin.progress * .1745327777777778) * .00006103515625);
    } else {
        if (stage == 240) {
            if (part)
                part->state_flags |= removed_flag;
            effect_target = nullptr;
            part = nullptr;
            stage = 241;
        }
        if (stage == 241) {
            host.rate = 0;
            if (!particles(match).alive())
                return remove();
        }
        if (part) {
            ruin.scale = static_cast<float>(ruin.scale - ticks * .003333333333333334);
            if (ruin.scale <= 0) {
                ruin.scale = 0;
                stage = 240;
            }
            part->reveal = ruin.scale;
            const auto& low = part->motion.box_min;
            const auto& high = part->motion.box_max;
            motion.position = {part->motion.position.x + low.x + (high.x - low.x) * .5f,
                               part->motion.position.y + low.y + (high.y - low.y) * .5f,
                               part->motion.position.z + low.z + (high.z - low.z) * ruin.scale};
            particles(match).set_position(motion.position);
        } else if (stage < 240)
            stage = 240;
        host.time += ticks;
        particles(match).advance(host.time, host.rate);
    }
    return 1;
}
int SpellActor::run_ruin_ring(Match& match, int ticks) {
    if (!host.initialized) { // 0x437850: forb, three seconds, radii 5 -> 100, no second material.
        host.initialized = true;
        altar_ring.enabled = true;
        altar_ring.texture = FourCC::from_text("forb");
        altar_ring.radius = 5;
        altar_ring.height = .5f;
        host.rate = (100.0f - 5) / 180;
        remaining = 180;
        linger = 180;
    }
    const auto frames = std::max(match.texture_frames ? match.texture_frames(altar_ring.texture) : 1u, 1u);
    altar_ring.frame += ticks * static_cast<float>(frames) / std::max(linger, 1);
    motion.position.z += ticks * .05f;
    // 0x437730 SetParams(radius, 0.5, radius x 0.9, radius x 0.25): the height stays 0.5; the last is
    // material 1's inner radius.
    altar_ring.radius += ticks * host.rate;
    altar_ring.end_radius = altar_ring.radius * .9f;
    altar_ring.inner = altar_ring.radius * .25f;
    remaining -= ticks;
    if (remaining <= 0) {
        state_flags |= removed_flag;
        return -2;
    }
    return 1;
}
} // namespace sacrifice
