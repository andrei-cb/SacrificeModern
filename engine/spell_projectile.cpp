#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/sound.hpp"
#include "sacrifice/spatial.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
double length(Vec3 v) {
    return std::sqrt(static_cast<double>(v.x) * v.x + static_cast<double>(v.y) * v.y +
                     static_cast<double>(v.z) * v.z);
}
Box3 scaled(Box3 box, float scale) {
    return {{box.minimum.x * scale, box.minimum.y * scale, box.minimum.z * scale},
            {box.maximum.x * scale, box.maximum.y * scale, box.maximum.z * scale}};
}
int subtract(int value, int ticks) {
    return std::bit_cast<int>(static_cast<unsigned>(value) - static_cast<unsigned>(ticks));
}
} // namespace
SpellActor::~SpellActor() {
    if (spatial_)
        spatial_->unbind(motion);
}
void SpellActor::deactivated(Match& match) {
    match.displace(land_wave, 0);
    match.displace(land_mound, 0);
    if (effect == Effect::earth_stomper && match.terrain && !terrain_cells.empty()) {
        auto& land = *const_cast<Terrain*>(match.terrain);
        for (const auto cell : terrain_cells) {
            auto& raw =
                land.heights[static_cast<unsigned>(cell.y) * land.width + static_cast<unsigned>(cell.x)];
            raw = static_cast<std::uint16_t>((raw & 0x8000) |
                                             std::clamp((raw & 0x7fff) - static_cast<int>(cell.z), 0, 32767));
        }
        terrain_cells.clear();
        ++match.terrain_revision;
    }
    if (match.flush_spell_sounds)
        match.flush_spell_sounds(*this);
    if (effect == Effect::skin_of_stone || effect == Effect::turn_to_stone) {
        const auto name = FourCC::from_text(effect == Effect::turn_to_stone ? "PARA" : "PROT");
        if (auto* target = dynamic_cast<CreatureActor*>(effect_target);
            target && (!target->named(name) || target->named(name) == this))
            target->grayscale = false;
    }
    if (clear_preview) {
        clear_preview();
        clear_preview = {};
    }
    if (spatial_)
        spatial_->unbind(motion);
    spatial_ = nullptr;
}
bool SpellActor::run(Match& match, int ticks) {
    if (friendly_ignore_time > 0) {
        const double time = static_cast<double>(friendly_ignore_time) - ticks * (1.0 / 60.0);
        friendly_ignore_time = static_cast<float>(time);
        if (time > 0)
            motion.collision_mask |= 0x20000;
        else
            motion.collision_mask &= ~0x20000u;
    }
    const bool result = MatchObject::run(match, ticks);
    // IsStillAlive: effects holding particle systems wait on their real particle count.
    if (!particle_systems.empty()) {
        std::size_t count = 0;
        for (const auto& system : particle_systems)
            count += system.count();
        live_particles = static_cast<unsigned>(count);
    }
    return result;
}
void SpellActor::reference_event(Match& match, MatchObject& object, int code) {
    if (code < 0) {
        if (sacrifice.altar == &object)
            sacrifice.altar = nullptr;
        for (auto& doctor : sacrifice.doctors)
            if (doctor == &object)
                doctor = nullptr;
        if (impact_target == &object)
            impact_target = nullptr;
        if (structure.building == &object)
            structure.building = nullptr;
        if (motion.collided == object.source_motion())
            motion.collided = nullptr;
    }
    MatchObject::reference_event(match, object, code);
}
void SpellActor::bind_world(Match& match) {
    if (!match.terrain || !match.spatial)
        throw Error("Moving spells require terrain and spatial services");
    if (spatial_)
        spatial_->unbind(motion);
    spatial_ = match.spatial;
    spatial_->bind(motion, MatchObject::group(type), [this] {
        return SpatialWorld::Info{kind_flags,
                                  0,
                                  phase,
                                  reinterpret_cast<std::uintptr_t>(allegiance),
                                  owner ? owner->source_motion() : nullptr,
                                  effect_target ? effect_target->source_motion() : nullptr,
                                  true};
    });
}
void SpellActor::start_beam(Match& match, Vec3 from, Vec3 to, float deviation, int strands, unsigned flags) {
    // 0x4355A0 → 0x4355E0(spell, range, 'ltng', deviation, 0.25 s, hold 0.5 s, strands, width 0.5, flags).
    motion.position = ability.origin = from;
    point = to;
    if (flags & 2)
        effect_target = spell_trace_ray(match, *this, from, point, 0x4015);
    ability.destination = point;
    ability.draw_back = ability.length = 0;
    ability.clock = 30;
    host.rate = 1.f / 15; // Quarter-second extension and retraction, half-second hold.
    beam.enabled = true;
    beam.texture = FourCC{1819569767u};
    beam.material = flags & 8 ? 0x0a011008u : flags & 4 ? 0x1a010401u : 0x1a012008u;
    beam.deviation = deviation;
    beam.strands = strands;
    generic.collision_radius = .5f;
    if (flags & 1) // 0x44CA20(spell, position, dword_4CB8F8, 1, 1, 100).
        light = {true, from, {1, 1, 1}, 1, 1, 100};
    sound_event(match, 2);
}
int SpellActor::advance_beam(Match& match, int ticks) {
    // 0x435840: the primitive keeps its source and target and draws the [+60, +64] interval of its curve.
    if (effect_target)
        spell_target_point(ability.destination, effect_target);
    int result = 1;
    if (ability.draw_back < 1) {
        ability.draw_back = std::min(1.f, ability.draw_back + ticks * host.rate);
        if (ability.draw_back >= 1) {
            result = 2;
            sound_event(match, 4);
        }
    } else if ((ability.clock -= ticks) < 0) {
        ability.length = std::min(1.f, ability.length + ticks * host.rate);
        if (ability.length >= 1)
            return 0;
        result = 3;
        if (light.enabled) // Retraction dims the light by 0.1 per call.
            light.intensity = std::max(0.f, light.intensity - .1f);
    }
    if (light.enabled) {
        const float t = ability.draw_back;
        light.position = {ability.origin.x + (ability.destination.x - ability.origin.x) * t,
                          ability.origin.y + (ability.destination.y - ability.origin.y) * t,
                          ability.origin.z + (ability.destination.z - ability.origin.z) * t};
    }
    beam.primitive.set_params({ability.origin, ability.destination, .2f, beam.deviation, ability.length,
                               ability.draw_back, generic.collision_radius, 16, beam.strands},
                              match.visual_random);
    beam.frame += ticks * .5f;
    return result;
}
void SpellActor::set_projectile_model(Match& match, FourCC model, float scale) {
    if (model_id == model && model_) {
        model_scale = scale;
        return;
    }
    const auto found = match.spell_models.find(model);
    if (found == match.spell_models.end() || !found->second)
        throw Error("Spell model is not bound: " + model.text());
    model_id = model;
    model_ = found->second;
    model_scale = scale;
    generic.initialized = false;
    if (match.create_spell_model)
        match.create_spell_model(*this);
}
void SpellActor::start_creature_projectile(Match& match, int lifetime, FourCC model, float scale,
                                           bool ballistic, bool muzzle) {
    if (!data || data->speed <= 0 || !match.terrain || !match.spatial)
        throw Error("Creature projectile requires positive speed, terrain and spatial services");
    if (muzzle)
        if (auto* caster = dynamic_cast<CreatureActor*>(owner))
            caster->write_reference_point(motion.position, 0, true);
    spell_lead_target(point, effect_target, data->speed, motion, *match.terrain);
    if (ballistic)
        motion.velocity = spell_ballistic_velocity(motion.position, point, data->speed);
    else {
        motion.velocity = {point.x - motion.position.x, point.y - motion.position.y,
                           point.z - motion.position.z};
        const double distance = length(motion.velocity);
        if (distance > 0) {
            const double factor = data->speed / distance;
            motion.velocity = {static_cast<float>(motion.velocity.x * factor),
                               static_cast<float>(motion.velocity.y * factor),
                               static_cast<float>(motion.velocity.z * factor)};
        }
    }
    if (auto* caster = dynamic_cast<CreatureActor*>(owner))
        spell_scatter(motion.velocity, caster->stats.current[8], match.random());
    if (length(motion.velocity) > 0)
        motion.orientation = spell_orientation(motion.velocity);
    if (model.value) {
        set_projectile_model(match, model, scale);
    } else {
        generic.initialized = true;
        generic.collision_radius = .2f;
    }
    motion.flags = (motion.flags & ~5u) | (ballistic ? 5u : 1u);
    motion.collision_mask |= 0x134015;
    friendly_ignore_time = match.projectile_friendly_fire ? 2.f : 0;
    bind_world(match);
    remaining = lifetime;
    stage = 17;
    sound_event(match, 2);
}
void SpellActor::start_return_projectile(Match& match, Vec3 destination, float speed) {
    const Vec3 delta{destination.x - motion.position.x, destination.y - motion.position.y,
                     destination.z - motion.position.z};
    const double distance = length(delta);
    if (distance > 0)
        motion.velocity = {static_cast<float>(delta.x * speed / distance),
                           static_cast<float>(delta.y * speed / distance),
                           static_cast<float>(delta.z * speed / distance)};
    motion.flags = 1;
    motion.collision_mask = 0;
    bind_world(match);
}
int SpellActor::fly_creature_projectile(Match& match, int ticks) {
    age += static_cast<float>(ticks);
    for (auto& system : particle_systems) {
        if (effect == Effect::fire_clawpult && !(system.flags & 1))
            system.set_frame({});
        else
            system.set_frame({motion.orientation, motion.position});
        system.advance(age, stage == 240 ? 0 : emission);
    }
    if (stage == 17) {
        if (state_flags & 3) {
            capture_collision(match);
            motion.velocity = {};
            motion.flags = motion.collision_mask = 0;
            stage = 32; // Damage runs on the next invocation, after the physics contact.
        } else if ((remaining -= ticks) <= 0) {
            motion.velocity = {};
            motion.flags = motion.collision_mask = 0;
            emission = 0;
            sprite.enabled = crossline.enabled = light.enabled = false;
            model_scale = 0;
            stage = 240;
        }
        return 1;
    }
    if (stage == 240) {
        emission = 0;
        if (std::ranges::any_of(particle_systems, [](const auto& p) { return p.alive(); }))
            return 1;
        state_flags |= removed_flag;
        return -2;
    }
    if (stage == 32)
        sound_event(match, 4);
    return 0;
}
void SpellActor::sync_model(Match&) {
    if (crossline.enabled && !crossline.points.empty()) {
        Vec3 minimum{1e30f, 1e30f, 1e30f}, maximum{-1e30f, -1e30f, -1e30f};
        for (const auto& p : crossline.points) {
            minimum.x = std::min(minimum.x, p.position.x - motion.position.x - p.width);
            minimum.y = std::min(minimum.y, p.position.y - motion.position.y - p.width);
            minimum.z = std::min(minimum.z, p.position.z - motion.position.z - p.width);
            maximum.x = std::max(maximum.x, p.position.x - motion.position.x + p.width);
            maximum.y = std::max(maximum.y, p.position.y - motion.position.y + p.width);
            maximum.z = std::max(maximum.z, p.position.z - motion.position.z + p.width);
        }
        motion.box_min = minimum;
        motion.box_max = maximum;
        motion.collision_boxes = {{minimum, maximum}};
    } else if (generic.initialized) {
        const float radius = generic.collision_radius;
        motion.box_min = {-radius, -radius, -radius};
        motion.box_max = {radius, radius, radius};
        motion.collision_boxes = {{motion.box_min, motion.box_max}}; // Particle MODEL mode 2.
        motion.reference_center = {};
    } else if (model_) {
        const auto visual = motion_box(scaled(model_->bounds, model_scale), motion.orientation);
        motion.box_min = visual.minimum;
        motion.box_max = visual.maximum;
        motion.collision_boxes.clear();
        for (const auto& record : model_->collisions) {
            auto box = scaled(record.box, model_scale);
            if (record.mode != 2)
                box = motion_box(box, motion.orientation, record.mode == 1);
            motion.collision_boxes.push_back(box);
        }
        Vec3 sum;
        for (const auto& box : motion.collision_boxes) {
            sum.x += box.minimum.x + box.maximum.x;
            sum.y += box.minimum.y + box.maximum.y;
            sum.z += box.minimum.z + box.maximum.z;
        }
        if (!motion.collision_boxes.empty()) {
            const double factor = 0.5 / motion.collision_boxes.size();
            motion.reference_center = {static_cast<float>(sum.x * factor), static_cast<float>(sum.y * factor),
                                       static_cast<float>(sum.z * factor)};
        } else
            motion.reference_center = {(visual.minimum.x + visual.maximum.x) * 0.5f,
                                       (visual.minimum.y + visual.maximum.y) * 0.5f,
                                       (visual.minimum.z + visual.maximum.z) * 0.5f};
    }
    if (spatial_)
        spatial_->sync(motion, state_flags);
}
void SpellActor::update(Match& match, int ticks) {
    if (!spatial_)
        return;
    ObjectMotion::Hooks hooks;
    hooks.collide = [this](ObjectMotion& object, Vec3& delta, float* fraction, const ObjectMotion* ignore) {
        return spatial_->collide(object, delta, fraction, ignore);
    };
    hooks.death = [this](int reason) {
        if (network_identity && reason)
            state_flags |= removed_flag;
    };
    motion.advance(*match.terrain, state_flags, ticks, hooks);
}
void SpellActor::sound_event(Match& match, unsigned mask) {
    for (const auto& sound : data->sounds) {
        if (!(sound.events & mask) || !sound.group.value)
            continue;
        const auto* emitter = sound.flags & 0x20   ? this
                              : sound.flags & 0x40 ? owner
                              : sound.flags & 0x80 ? effect_target
                                                   : this;
        if (!emitter)
            continue;
        FourCC sample = sound.group; // Native 0x44de40 falls back to the requested id if lookup misses.
        if (match.sound_set)
            if (const auto selected = match.sound_set->select(sound.group, match.random()); selected.value)
                sample = selected;
        unsigned flags = sound.flags & 1 ? 0x40005 : 0x40004;
        if (sound.flags & 2)
            flags |= 0x40;
        sound_requests.push_back({sound.group, sample, emitter->reference(), flags});
    }
}
void SpellActor::stop_sound_event(unsigned mask) {
    for (const auto& sound : data->sounds) {
        if (!(sound.events & mask) || !sound.group.value || !(sound.flags & 1))
            continue;
        const auto* emitter = sound.flags & 0x20   ? this
                              : sound.flags & 0x40 ? owner
                              : sound.flags & 0x80 ? effect_target
                                                   : this;
        if (!emitter)
            emitter = this;
        sound_requests.push_back({sound.group, {}, emitter->reference(), 0, true});
    }
}
SpellActor& SpellActor::launch_fireball(Match& match, CreatureActor& actor) {
    auto& object = match.create(
        MatchObject::spell, [] { return std::make_unique<SpellActor>(); }, &actor,
        [&](MatchObject& object) {
            auto* spell = dynamic_cast<SpellActor*>(&object);
            if (!spell)
                throw Error("Spell pool object has an incompatible host type");
            spell->effect = Effect::fireball;
            spell->data = actor.casting.data;
        });
    auto& spell = static_cast<SpellActor&>(object);
    spell.owner = actor.owner;
    spell.effect_target = actor.effect_target;
    spell.point = actor.casting.point;
    spell.motion.position = actor.motion.position;
    spell.motion.orientation = actor.motion.orientation;
    spell.motion.yaw = actor.motion.yaw;
    spell.motion.pitch = actor.motion.pitch;
    spell.motion.roll = actor.motion.roll;
    spell.object_flags |= 0x10000;
    spell.motion.flags |= 0x10000;
    if (actor.allegiance)
        actor.allegiance->attach(spell);
    spell.run(match, 0);
    return spell;
}
int SpellActor::run_fireball(Match& match, int ticks) {
    if (!data)
        throw Error("FireballRun requires a spell definition");
    const auto remove = [&] {
        state_flags |= removed_flag;
        return -2;
    };
    if (!stage) {
        model_id = {
            static_cast<unsigned>(data->integer(FourCC::from_text("tmod"), FourCC::from_text("bold").value))};
        const auto found = match.spell_models.find(model_id);
        if (found == match.spell_models.end() || !found->second)
            throw Error("Fireball model is not bound: " + model_id.text());
        model_ = found->second;
        if (match.create_spell_model)
            match.create_spell_model(*this);
        bind_world(match);
        visual_requests.push_back({VisualRequest::Kind::cast_generic});
        launch_cast_hands(match, *this);
        visual_requests.push_back({VisualRequest::Kind::fireball_model});
        visual_requests.push_back({VisualRequest::Kind::fireball_light});
        light = {true, motion.position, {1, 0.5f, 0}, 0, 1, 25}; // 0x44ca20 with dword_4CB8E0.
        flight_remaining = 800;
        cast_alpha = 0;
        scale_target = 1;
        scale_rate = 0.2f;
        const auto angular = [&] {
            const auto draw = match.random().next();
            return static_cast<float>(((draw + static_cast<double>(draw)) / 32768.0 - 1.0) *
                                      0.05235983333333333);
        };
        motion.angular_velocity = {angular(), angular(), angular()};
        motion.flags = 0x38;
        model_scale = 0.01f;
        stage = 1;
        had_target = effect_target != nullptr;
    }
    if (stage == 1) {
        if (owner && owner->source_motion()) {
            const auto& source = *owner->source_motion();
            const auto& o = source.orientation;
            motion.position = {
                static_cast<float>(source.position.x + static_cast<double>(o[3]) + 2.0 * o[6]),
                static_cast<float>(source.position.y + static_cast<double>(o[4]) + 2.0 * o[7]),
                static_cast<float>(source.position.z + static_cast<double>(o[5]) + 2.0 * o[8])};
        }
        const auto* caster = dynamic_cast<const CreatureActor*>(owner);
        if (!caster || !(owner->kind_flags & 1)) // 0x441D20: no wizard caster, including none, aborts.
            stage = 255;
        else {
            const auto state = caster->cast_state().first;
            if (state)
                stage = state == 0x20000000 ? 16 : 255;
        }
    } else if (stage == 16 || stage == 17) {
        if (stage == 16) {
            sound_event(match, 2);
            model_scale = 0.25f;
            spell_target_point(point, effect_target);
            motion.velocity = {point.x - motion.position.x, point.y - motion.position.y,
                               point.z - motion.position.z};
            const double distance = length(motion.velocity);
            if (distance != 0) {
                const double scale = data->speed / distance;
                motion.velocity = {static_cast<float>(motion.velocity.x * scale),
                                   static_cast<float>(motion.velocity.y * scale),
                                   static_cast<float>(motion.velocity.z * scale)};
            }
            motion.flags |= 1;
            motion.collision_mask |= 0x114015;
            if (match.projectile_friendly_fire) {
                friendly_ignore_time = 2;
                motion.collision_mask |= 0x20000;
            } else {
                friendly_ignore_time = 0;
                motion.collision_mask |= 0x20000;
            }
            visual_requests.push_back({VisualRequest::Kind::fireball_trail});
            launch_fireball_visual(match, *this, Effect::fireball_trail);
            stage = 17;
        }
        if (had_target && !effect_target) {
            had_target = false;
            TerrainPlane plane;
            const bool solid = point.x >= 0 && point.y >= 0 && point.x < match.terrain->width * 10.0 &&
                               point.y < match.terrain->height * 10.0 &&
                               terrain_plane(*match.terrain, point.x, point.y, plane) && plane.solid;
            point.z = static_cast<float>(terrain_true_height(*match.terrain, point.x, point.y));
            if (!solid)
                state_flags |= 2;
        }
        spell_lead_target(point, effect_target, data->speed, motion, *match.terrain);
        const float acceleration = static_cast<float>(static_cast<double>(ticks) * data->acceleration);
        spell_approach_velocity(motion, point, acceleration, data->speed);
        const float height =
            static_cast<float>(terrain_surface(*match.terrain, motion.position.x, motion.position.y));
        if (height > 0) {
            const double dx = static_cast<double>(motion.position.x) - point.x,
                         dy = static_cast<double>(motion.position.y) - point.y,
                         dz = static_cast<double>(motion.position.z) - point.z;
            const float distance = static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz));
            const double clearance = distance < 3 ? 0 : distance >= 10 ? 1 : (distance - 3.0) / 7.0;
            if (static_cast<double>(motion.position.z) - height < clearance)
                motion.position.z = static_cast<float>(clearance + height);
        }
        if (state_flags & 3) {
            stage = 32;
            impact_target = nullptr;
            if (motion.collided)
                for (const auto& object : match.objects())
                    if (object->source_motion() == motion.collided) {
                        impact_target = object.get();
                        break;
                    }
            state_flags &= ~3u;
        } else {
            flight_remaining = subtract(flight_remaining, ticks);
            if (flight_remaining <= 0)
                stage = 255;
        }
    } else if (stage == 32) {
        sound_event(match, 4);
        visual_requests.push_back({VisualRequest::Kind::fireball_blast});
        launch_fireball_visual(match, *this, Effect::fireball_blast);
        launch_shockwave(match, *this, std::max(data->radius, 7.5f));
        visual_requests.push_back({VisualRequest::Kind::fireball_decal});
        visual_requests.push_back({VisualRequest::Kind::remove_widgets});
        spell_damage(match, this, impact_target, data->damage_type, data->amount, data->radius);
        visual_requests.push_back({VisualRequest::Kind::light_remove});
        return remove();
    } else if (stage == 255) {
        scale_target = 0;
        scale_rate = 0.05f;
        if (cast_alpha == 0)
            return remove();
    }
    const double change = static_cast<double>(ticks) * scale_rate;
    if (cast_alpha < scale_target)
        cast_alpha = static_cast<float>(
            std::min(static_cast<double>(cast_alpha) + change, static_cast<double>(scale_target)));
    else if (cast_alpha > scale_target)
        cast_alpha = static_cast<float>(
            std::max(static_cast<double>(cast_alpha) - change, static_cast<double>(scale_target)));
    model_scale = static_cast<float>(cast_alpha * 0.25);
    light.intensity = cast_alpha;
    light.position = motion.position;
    return 1;
}
} // namespace sacrifice
