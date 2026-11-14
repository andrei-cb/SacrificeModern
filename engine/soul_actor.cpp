#include "sacrifice/soul_actor.hpp"
#include "sacrifice/spatial.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
// 0x44de40 with the global table: a missing group falls back to the requested id and draws nothing.
FourCC select_sound(Match& match, FourCC group) {
    if (match.sound_set)
        if (const auto sample = match.sound_set->select(group, match.random()); sample.value)
            return sample;
    return group;
}
} // namespace
SoulActor::~SoulActor() {
    if (spatial_)
        spatial_->unbind(motion);
}
void SoulActor::activated(Match&) {
    if (spatial_)
        spatial_->unbind(motion);
    spatial_ = nullptr;
    motion = {};
    visual = {};
    visual.growth = 0;
    visual.positioned = true;
    soul_flags = 0;
    eligible_sides = ~0u;
    count = 0;
    anchor = collector = nullptr;
    lift = 0;
    destination = collected_from = {};
    frames = 1;
    network_identity = -1;
    accounted = false;
    sync_visual = {};
    sounds.clear();
    object_flags |= 0x10000;
}
void SoulActor::deactivated(Match&) {
    if (accounted && allegiance)
        allegiance->change_souls(std::bit_cast<int>(0u - static_cast<unsigned>(count)));
    accounted = false;
    if (spatial_)
        spatial_->unbind(motion);
    spatial_ = nullptr;
    if (sync_visual)
        sync_visual(*this, false);
}
void SoulActor::initialize(Match& match, Vec3 position, MatchObject* source, int amount,
                           unsigned atlas_frames) {
    motion.position = position;
    anchor = source;
    count = amount;
    frames = atlas_frames;
    if (source && source->allegiance)
        source->allegiance->attach(*this);
    if (allegiance) {
        allegiance->change_souls(amount);
        accounted = true;
    }
    visual.phase_x = static_cast<float>(match.random().next() * 0.00006103515625);
    visual.phase_y = static_cast<float>(match.random().next() * 0.00006103515625);
    if (source && source->allegiance) {
        eligible_sides = source->allegiance->identity;
        if (match.allied_soul_pickup)
            eligible_sides |= source->allegiance->allies;
    }
    visual.alignment = !match.local_side || (eligible_sides & match.local_side->identity) ? 0.0f : 1.0f;
}
void SoulActor::bind_spatial(SpatialWorld& spatial) {
    if (spatial_)
        spatial_->unbind(motion);
    spatial_ = &spatial;
    spatial.bind(motion, MatchObject::group(type), [this] {
        return SpatialWorld::Info{kind_flags, 0, phase, reinterpret_cast<std::uintptr_t>(allegiance)};
    });
}
void SoulActor::reference_event(Match& match, MatchObject& object, int code) {
    if (code < 0) {
        if (anchor == &object)
            anchor = nullptr;
        if (collector == &object)
            collector = nullptr;
        for (auto& sound : sounds)
            if (sound.owner == &object)
                sound.owner = nullptr;
    }
    MatchObject::reference_event(match, object, code);
}
void SoulActor::collect(Match& match, MatchObject* target) {
    if (soul_flags & 0x80000000)
        return;
    if (anchor && (anchor->kind_flags & 8)) {
        anchor->phase = std::bit_cast<int>(static_cast<unsigned>(anchor->phase) + 1u);
        anchor = nullptr;
    }
    collected_from = motion.position;
    if (auto* wizard = dynamic_cast<CreatureActor*>(target); wizard && (wizard->kind_flags & 1)) {
        sounds.push_back({wizard, FourCC::from_text("spir"), select_sound(match, FourCC::from_text("spir")),
                          motion.position});
        wizard->change_souls(count);
    }
    if (accounted && allegiance)
        allegiance->change_souls(std::bit_cast<int>(0u - static_cast<unsigned>(count)));
    accounted = false;
    collector = target;
    destination = motion.position;
    if (target)
        spell_target_point(destination, target);
    soul_flags |= 0x80000000;
}
void SoulActor::find_collector(Match& match) {
    if (!spatial_)
        return;
    spatial_->sync_radius(motion, state_flags, 5);
    Vec3 center = motion.position;
    double radius = 5;
    if (anchor && anchor->source_motion()) {
        const auto& other = *anchor->source_motion();
        center = {static_cast<float>((static_cast<double>(other.position.x) - motion.position.x) * .5 +
                                     motion.position.x),
                  static_cast<float>((static_cast<double>(other.position.y) - motion.position.y) * .5 +
                                     motion.position.y),
                  static_cast<float>((static_cast<double>(other.position.z) - motion.position.z) * .5 +
                                     motion.position.z)};
        const double half = spatial_->collision_radius(other) * .5;
        radius = std::max(5.0, (5.0 - half) * .5 + half);
        eligible_sides = anchor->allegiance ? anchor->allegiance->identity : 0;
        if (match.allied_soul_pickup && anchor->allegiance)
            eligible_sides |= anchor->allegiance->allies;
    } else {
        center.z = static_cast<float>(center.z - lift);
        eligible_sides = ~0u;
    }
    CreatureActor* nearest = nullptr;
    float nearest_squared = 0;
    spatial_->visit_members(motion, 1, [&](const ObjectMotion& candidate) {
        CreatureActor* wizard = nullptr;
        for (const auto& object : match.objects())
            if (object->source_motion() == &candidate) {
                wizard = dynamic_cast<CreatureActor*>(object.get());
                break;
            }
        if (!wizard || !wizard->network_identity || !wizard->alive() || !wizard->allegiance ||
            (!(wizard->common_status & 0x40) && !(eligible_sides & wizard->allegiance->identity)))
            return 0;
        const double dx = static_cast<double>(candidate.position.x) - center.x,
                     dy = static_cast<double>(candidate.position.y) - center.y,
                     dz = static_cast<double>(candidate.position.z) - center.z;
        const double squared = dz * dz + dy * dy + dx * dx;
        if (nearest && squared >= nearest_squared)
            return 0;
        nearest_squared = static_cast<float>(squared);
        nearest = wizard;
        return 0;
    });
    const float squared_radius = static_cast<float>(radius * radius);
    if (nearest && nearest_squared < static_cast<double>(squared_radius))
        collect(match, nearest);
}
bool SoulActor::reconstitute(Match& match, CreatureActor& corpse) {
    // 0x4764d0: returning the soul to its corpse is distinct from wizard collection.
    if (anchor != &corpse || (soul_flags & 0x80000000))
        return false;
    collector = &corpse;
    soul_flags |= 0xc0000000;
    corpse.fsm().event(43);
    corpse.change_souls(count);
    sounds.push_back(
        {&corpse, FourCC::from_text("spin"), select_sound(match, FourCC::from_text("spin")), motion.position});
    if (accounted && allegiance)
        allegiance->change_souls(std::bit_cast<int>(0u - static_cast<unsigned>(count)));
    accounted = false;
    destination = motion.position;
    spell_target_point(destination, collector);
    collected_from = motion.position;
    return true;
}
bool SoulActor::run(Match& match, int ticks) {
    if (!anchor)
        eligible_sides = ~0u;
    const auto flags = soul_flags;
    if (!(flags & 0xc0000000))
        visual.alignment = match.local_side && (eligible_sides & match.local_side->identity) ? 0.0f : 1.0f;
    visual.atlas_offset = visual.alignment > .5 ? frames / 2 : 0;
    if (!(flags & 0x80000000)) {
        visual.growth = static_cast<float>(std::min(1.0, visual.growth + ticks * .005));
        lift = static_cast<float>(visual.growth + visual.growth);
        if (anchor) {
            spell_target_point(motion.position, anchor);
            motion.position.z = static_cast<float>(motion.position.z + lift);
        } else if (match.terrain) {
            TerrainPlane plane;
            const bool valid = terrain_plane(*match.terrain, motion.position.x, motion.position.y, plane);
            double height = valid ? plane.height(motion.position.x, motion.position.y) : 0;
            if (valid && plane.solid && height + lift >= motion.position.z)
                motion.position.z = static_cast<float>(height + lift);
            else if ((!valid || !plane.solid) && motion.position.z <= height - 50) {
                if (network_identity)
                    collect(match, nullptr);
            } else
                motion.position.z = static_cast<float>(motion.position.z - ticks * (1.0 / 60.0));
        }
    } else {
        visual.growth = static_cast<float>(visual.growth - ticks * .05);
        if (visual.growth < 0)
            return true;
        if (collector)
            spell_target_point(destination, collector);
        motion.position = {
            static_cast<float>((static_cast<double>(collected_from.x) - destination.x) * visual.growth +
                               destination.x),
            static_cast<float>((static_cast<double>(collected_from.y) - destination.y) * visual.growth +
                               destination.y),
            static_cast<float>((static_cast<double>(collected_from.z) - destination.z) * visual.growth +
                               destination.z)};
    }
    visual.phase_x = static_cast<float>(visual.phase_x + ticks * .015);
    visual.phase_y = static_cast<float>(visual.phase_y + ticks * .01);
    visual.frame = static_cast<float>(visual.frame + ticks * .25);
    const float half = static_cast<float>(std::max(frames / 2, 1u));
    while (visual.frame >= half)
        visual.frame = static_cast<float>(visual.frame - half);
    if (!(soul_flags & 0xc0000000) && visual.growth >= 1)
        find_collector(match);
    phase = std::bit_cast<int>(static_cast<unsigned>(phase) + 1u);
    if (sync_visual)
        sync_visual(*this, !(state_flags & removed_flag));
    return (state_flags & removed_flag) != 0;
}
SpellActor& SpellActor::launch_corpse_fade(Match& match, MatchObject& actor) {
    auto& spell = static_cast<SpellActor&>(match.create(
        MatchObject::spell, [] { return std::make_unique<SpellActor>(); }, &actor,
        [](MatchObject& object) {
            auto& spell = static_cast<SpellActor&>(object);
            spell.effect = Effect::corpse_fade;
            spell.data.reset();
        }));
    spell.owner = actor.owner;
    spell.effect_target = &actor;
    spell.object_flags |= 0x10000;
    if (actor.source_motion()) {
        spell.motion.position = actor.source_motion()->position;
        spell.motion.orientation = actor.source_motion()->orientation;
    }
    if (actor.allegiance)
        actor.allegiance->attach(spell);
    // 0x44c230 uses mode 2: initialization waits until the child is reached in the object pass.
    return spell;
}
int SpellActor::run_corpse_fade(Match& match, int ticks) {
    if (!stage) {
        visual_requests.push_back({VisualRequest::Kind::corpse_particles});
        // 'pkup' embers rising out of the first collision box (+632/+644) for two seconds (preset 128).
        if (const auto* source = effect_target ? effect_target->source_motion() : nullptr) {
            auto& system = particles(match);
            particle_material(match, system, 0x1a012008, "pkup");
            const Vec3 low = source->collision_boxes.empty() ? source->box_min : source->collision_boxes[0].minimum;
            const Vec3 high = source->collision_boxes.empty() ? source->box_max : source->collision_boxes[0].maximum;
            const double dx = static_cast<double>(high.x) - low.x, dy = static_cast<double>(high.y) - low.y;
            const float preset[3]{static_cast<float>(std::clamp(std::sqrt(dy * dy + dx * dx) * 0.3, 0.3, 2.0)),
                                  high.z - low.z, 2};
            system.set_property(128, preset, preset);
            const float box_low[3]{low.x, low.y, low.z}, box_high[3]{high.x, high.y, high.z};
            system.set_property(2, box_low, box_high);
            const float at[3]{motion.position.x, motion.position.y, motion.position.z};
            system.set_property(0, at, at);
        }
        sound_requests.push_back({FourCC{1718378861u}, {}, 0, 262148u});
        emission = 1;
        remaining = 60;
        stage = 1;
    }
    remaining = std::bit_cast<int>(static_cast<unsigned>(remaining) - static_cast<unsigned>(ticks));
    if (remaining < 30) {
        if (effect_target)
            effect_target->state_flags |= removed_flag;
        if (remaining < 0) {
            emission = 0;
            if (!live_particles) {
                state_flags |= removed_flag;
                return -2;
            }
        }
    }
    age = static_cast<float>(age + static_cast<double>(ticks));
    if (!particle_systems.empty())
        particle_systems.front().advance(age, emission);
    return 1;
}
} // namespace sacrifice
