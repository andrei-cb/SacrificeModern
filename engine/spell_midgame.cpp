#include "sacrifice/creature_actor.hpp"
#include "sacrifice/navigation.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/soul_actor.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include "sacrifice/spell_data.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
int remove_spell(SpellActor& spell) {
    spell.state_flags |= MatchObject::removed_flag;
    return -2;
}
unsigned mid_cast(SpellActor& spell, unsigned released, unsigned aborted) {
    auto* caster = dynamic_cast<CreatureActor*>(spell.owner);
    // 0x441d20: a wizard reports release 0x20000000 or abort; any other owner, or none (0x453600),
    // aborts. Effects listed by casterless_skips_cast start past the wait in native, so they release.
    if (!spell.owner)
        return SpellActor::casterless_skips_cast(spell.effect) ? released : aborted;
    if (!caster)
        return aborted;
    if (!(caster->kind_flags & 1))
        return aborted;
    const auto [state, alpha] = caster->cast_state();
    spell.cast_alpha = alpha;
    if (!state)
        return 0;
    return state == 0x20000000 ? released : aborted;
}
CreatureActor* living_unit(MatchObject* object) {
    auto* actor = dynamic_cast<CreatureActor*>(object);
    return actor && (actor->kind_flags & 5) && actor->alive() ? actor : nullptr;
}
double distance_squared(Vec3 a, Vec3 b) {
    const double dx = static_cast<double>(a.x) - b.x, dy = static_cast<double>(a.y) - b.y,
                 dz = static_cast<double>(a.z) - b.z;
    return dx * dx + dy * dy + dz * dz;
}
void aim_point(SpellActor& spell) {
    if (spell.effect_target)
        spell_target_point(spell.motion.position, spell.effect_target);
    else
        spell.motion.position = spell.point;
}
FourCC hop_name(const SpellActor& spell);
CreatureActor* nearest_unit(Match& match, Vec3 origin, float radius, MatchObject* source, bool foes,
                            SpellActor* mark) {
    CreatureActor* best = nullptr;
    double best_d = static_cast<double>(radius) * radius;
    const auto visit = [&](MatchObject& object) {
        auto* actor = living_unit(&object);
        if (!actor || actor == source)
            return 0;
        if (foes ? !actor->enemy(source) : !actor->friendly(source))
            return 0;
        if (mark && actor->named(hop_name(*mark)))
            return 0;
        const auto* motion = actor->source_motion();
        if (!motion)
            return 0;
        const double d = distance_squared(origin, motion->position);
        if (d < best_d) {
            best_d = d;
            best = actor;
        }
        return 0;
    };
    match.visit_group(0, 5, visit);
    match.visit_group(1, 5, visit);
    return best;
}
// 0x492760 / 0x492AA0 with mask 5: living wizards and creatures of every side.
std::vector<CreatureActor*> units_near(Match& match, Vec3 origin, float radius) {
    std::vector<CreatureActor*> units;
    const auto visit = [&](MatchObject& object) {
        if (auto* actor = living_unit(&object); actor && actor->source_motion() &&
                                                distance_squared(origin, actor->source_motion()->position) <
                                                    static_cast<double>(radius) * radius)
            units.push_back(actor);
        return 0;
    };
    match.visit_group(0, 5, visit);
    match.visit_group(1, 5, visit);
    return units;
}
CreatureActor* nearest_any_unit(Match& match, Vec3 origin, float radius) {
    CreatureActor* best = nullptr;
    double best_d = static_cast<double>(radius) * radius;
    for (auto* actor : units_near(match, origin, radius))
        if (const double d = distance_squared(origin, actor->source_motion()->position); d < best_d)
            best_d = d, best = actor;
    return best;
}
FourCC hop_name(const SpellActor& spell) {
    if (spell.spell_id.value)
        return spell.spell_id;
    switch (spell.effect) {
    case SpellActor::Effect::rainbow:
        return FourCC::from_text("rbow");
    case SpellActor::Effect::dragonfire:
        return FourCC::from_text("dfir");
    case SpellActor::Effect::erupt:
        return FourCC::from_text("erpt");
    case SpellActor::Effect::chain_lightning:
        return FourCC::from_text("cltn");
    case SpellActor::Effect::animate_dead:
        return FourCC::from_text("aded");
    case SpellActor::Effect::rain_frogs:
        return FourCC::from_text("rfrg");
    case SpellActor::Effect::pyro_explosion:
        return FourCC::from_text("fexp");
    case SpellActor::Effect::halo_of_earth:
        return FourCC::from_text("halo");
    case SpellActor::Effect::spirit_wind:
        return FourCC::from_text("swnd");
    case SpellActor::Effect::demonic_rift:
        return FourCC::from_text("rift");
    default:
        return FourCC::from_text("midg");
    }
}
void mark_hit(CreatureActor& actor, SpellActor& spell) {
    // 0x4692a0 with SPEL+16: hop spells name each victim so 0x492FA0 will not reselect them.
    actor.name_reference(hop_name(spell), &spell);
}
// 0x438FF0: the shared spell-hit burst (0x438e30 / 0x4388b0).
void attach_hit(Match& match, SpellActor& spell, MatchObject* victim, Vec3 point) {
    auto& hit = SpellActor::launch_sacrifice_host(match, spell, SpellActor::Effect::spell_hit);
    hit.effect_target = victim;
    hit.motion.position = point;
    hit.run(match, 0);
}
SpellActor& launch_child(Match& match, SpellActor& source, SpellActor::Effect effect, MatchObject* target,
                         Vec3 position) {
    auto& child = SpellActor::launch_sacrifice_host(match, source, effect);
    child.owner = source.owner;
    child.effect_target = target;
    child.motion.position = position;
    child.point = position;
    child.spell_id = hop_name(source);
    if (source.allegiance)
        source.allegiance->attach(child);
    return child;
}
void strike(Match& match, SpellActor& spell, MatchObject* victim, Vec3 point, bool splash) {
    if (splash)
        spell_damage(match, &spell, victim, spell.data->damage_type, spell.data->amount, spell.data->radius,
                     point);
    else
        spell_damage(match, &spell, victim, spell.data->damage_type, spell.data->amount, spell.data->radius);
    attach_hit(match, spell, victim, point);
}
int run_rain_frog(Match& match, SpellActor& spell, int ticks) {
    // FrogRun 0x42d390: fall / hop, then 0x44d7e0 and the 0x438FF0 burst.
    if (!spell.stage) {
        if (match.terrain && match.spatial && spell.data) {
            if (!spell.had_target) {
                spell.point = spell.motion.position;
                spell.motion.position.z += 40;
            }
            spell.launch_projectile(match);
            spell.generic.initialized = true;
            spell.generic.collision_radius = .3f;
            spell.sprite = {true,
                            FourCC{static_cast<unsigned>(spell.data->integer(
                                FourCC::from_text("ttex"), FourCC::from_text("frgl").value))},
                            0x0a011008, 0xe0ffffff, .75f};
            spell.motion.velocity = {0, 0, -.5f};
            spell.motion.flags |= 5;
            spell.motion.collision_mask = 0x184015;
            spell.friendly_ignore_time = 0;
            spell.remaining = 600;
            spell.stage = 1;
            return 1;
        }
        spell.stage = 16;
    }
    if (spell.stage == 1) {
        // 0x42d390 counts the lifetime down on every call; only the falling stage acts on it.
        spell.remaining -= ticks;
        spell.sprite.frame += ticks * .26666668f;
        if ((spell.state_flags & 1) || spell.remaining <= 0) {
            // Any object contact or the timeout lands the frog (0x441C30 sync bit 1 keeps the object).
            spell.capture_collision(match);
            spell.effect_target = spell.impact_target;
            spell.stage = 16;
        } else if (spell.state_flags & 2) {
            spell.motion.velocity = {};
            spell.motion.flags = 0;
            spell.next_pulse = 20;
            spell.sprite.frame = 0;
            spell.stage = 3;
        } else
            return 1;
    }
    if (spell.stage == 3) {
        spell.remaining -= ticks;
        if ((spell.next_pulse -= ticks) <= 0) {
            // 0x42d160: hop toward the nearest unit of any side within 50, otherwise a random XY point.
            auto* target = nearest_any_unit(match, spell.motion.position, 50);
            spell.effect_target = target;
            Vec3 aim = spell.motion.position;
            if (target)
                spell_target_point(aim, target);
            else {
                aim.x += match.random().next() * 100.f / 32768 - 50;
                aim.y += match.random().next() * 100.f / 32768 - 50;
            }
            const float x = aim.x - spell.motion.position.x, y = aim.y - spell.motion.position.y,
                        distance = std::hypot(x, y);
            spell.motion.velocity = {distance > 0 ? x * .075f / distance : 0,
                                     distance > 0 ? y * .075f / distance : 0, .075f};
            spell.motion.flags = 5;
            spell.state_flags &= ~3u;
            spell.stage = 1;
            if ((match.random().next() & 15) <= 3)
                spell.sound_event(match, 8);
        }
    }
    if (spell.stage == 16) {
        // 0x44D7E0 on whatever was hit (or nothing); only a unit keeps the frog attached.
        spell.sound_event(match, 4);
        spell_damage(match, &spell, spell.effect_target, spell.data->damage_type, spell.data->amount,
                     spell.data->radius);
        auto* target = living_unit(spell.effect_target);
        if (!target)
            spell.stage = 240;
    }
    if (spell.stage == 16) {
        auto* target = living_unit(spell.effect_target);
        if (const auto bone = target->closest_bone(spell.motion.position))
            spell.bone_attachment = {bone->bone, bone->offset, true};
        spell.motion.flags = spell.motion.collision_mask = 0;
        spell.motion.velocity = {};
        spell.sprite.frame = 0;
        spell.remaining = 300;
        spell.stage = 17;
    }
    if (spell.stage == 17) {
        auto* target = living_unit(spell.effect_target);
        if (!target || (spell.remaining -= ticks) <= 0)
            spell.stage = 240;
        else {
            const auto& bone = spell.bone_attachment;
            if (!bone.bound ||
                !target->write_bone_point(spell.motion.position, bone.bone, nullptr, &bone.offset))
                spell_target_point(spell.motion.position, target);
            for (unsigned slot : {3u, 4u}) {
                target->stats.changed |= 1u << slot;
                target->stats.target[slot] = std::max(0, target->stats.target[slot] / 2);
            }
        }
    }
    if (spell.stage >= 240) {
        spell.sound_event(match, 16);
        attach_hit(match, spell, nullptr, spell.motion.position);
        return remove_spell(spell);
    }
    return 1;
}
// 0x44C660 with basis 0x4CAF40: a Catmull-Rom wander through four points relative to the owner, at .0333
// a tick; each whole step drops the oldest point and draws a new one in the box (0x44C330).
void halo_wander(Match& match, SpellActor& spell, int ticks, Vec3 low, Vec3 high) {
    auto& points = spell.surface.tentacles[0].curve;
    float& t = spell.ability.length;
    t += ticks / 30.f;
    while (t >= 1) {
        points = {points[1], points[2], points[3], {}};
        const auto pick = [&](float a, float b) { return match.random().next() * (b - a) / 32768 + a; };
        points[3].x = pick(low.x, high.x);
        points[3].y = pick(low.y, high.y);
        points[3].z = pick(low.z, high.z);
        t -= 1;
    }
    const float t2 = t * t, t3 = t2 * t;
    const float w0 = -.5f * t3 + t2 - .5f * t, w1 = 1.5f * t3 - 2.5f * t2 + 1, w2 = -1.5f * t3 + 2 * t2 + .5f * t,
                w3 = .5f * t3 - .5f * t2;
    const Vec3 base = spell.ability.origin, before = spell.motion.position;
    const auto blend = [&](auto member) {
        return base.*member + w0 * (points[0].*member) + w1 * (points[1].*member) + w2 * (points[2].*member) +
               w3 * (points[3].*member);
    };
    spell.motion.position = {blend(&Vec3::x), blend(&Vec3::y), blend(&Vec3::z)};
    if (ticks)
        spell.motion.velocity = {(spell.motion.position.x - before.x) / ticks,
                                 (spell.motion.position.y - before.y) / ticks,
                                 (spell.motion.position.z - before.z) / ticks};
    // Once released the rock collides (0x445110's 0x114015 mask): this tick's motion update sweeps it
    // along the step at that velocity, so anything in the way is hit.
    if (ticks && (spell.motion.flags & 1))
        spell.motion.position = before;
}
// 0x445320: follow the living owner, wandering 1.5 to 4.5 above its head within +-1.5.
bool halo_orbit(Match& match, SpellActor& spell, int ticks) {
    const auto* owner = spell.owner ? spell.owner->source_motion() : nullptr;
    if (!owner || (spell.owner->state_flags & MatchObject::removed_flag) || !living_unit(spell.owner))
        return false;
    spell.ability.origin = owner->position;
    // 0x474FB0(2.0) every step: the drifting rock keeps ignoring friendly units.
    if (spell.motion.flags & 1)
        spell.friendly_ignore_time = match.projectile_friendly_fire ? 2.f : 0;
    const float head = owner->box_max.z;
    halo_wander(match, spell, ticks, {-1.5f, -1.5f, head + 1.5f}, {1.5f, 1.5f, head + 4.5f});
    return true;
}
int run_halo_rock(Match& match, SpellActor& spell, int ticks) {
    // HaloRockRun 0x445590.
    if (!spell.stage) {
        // 0x445110: a 'roks' rock rises from the ground 2 to 3 out along a path that starts below the
        // surface, shaking the ground and leaving the 0x4380F0 ground effects.
        const auto model = FourCC::from_text("roks");
        spell.set_projectile_model(match, model, 1);
        const auto spin = [&] { return static_cast<float>((match.random().next() - 0x4000) * .000001597086588541667); };
        spell.motion.angular_velocity.x = spin();
        spell.motion.angular_velocity.y = spin();
        spell.motion.angular_velocity.z = spin();
        spell.motion.flags |= 0x38; // Tumbling.
        const float distance = match.random().next() / 32768.f + 2;
        const float heading = match.random().next() * 6.2831802f / 32768;
        Vec3 ground{spell.motion.position.x + std::sin(heading) * distance,
                    spell.motion.position.y + std::cos(heading) * distance, spell.motion.position.z};
        if (match.terrain)
            ground.z = static_cast<float>(terrain_surface(*match.terrain, ground.x, ground.y));
        const Vec3 rel{ground.x - spell.motion.position.x, ground.y - spell.motion.position.y,
                       ground.z - spell.motion.position.z};
        float height = 1;
        if (const auto* geometry = spell.model_geometry())
            height = geometry->bounds.maximum.z * spell.model_scale;
        spell.surface.tentacles[0].curve = {Vec3{rel.x, rel.y, rel.z - height}, rel,
                                            Vec3{rel.x * .75f, rel.y * .75f, rel.z - height},
                                            Vec3{0, 0, height + 3}};
        spell.ability.length = 0;
        spell.ability.origin = spell.motion.position;
        if (!halo_orbit(match, spell, 0))
            halo_wander(match, spell, 0, {}, {});
        match.shake(spell.motion.position, 8, 50);
        SpellActor::launch_earth_burst(match, spell, spell.motion.position, 3, 24);
        match.scar_requests.push_back({FourCC{static_cast<unsigned>(spell.data->integer(
                                           FourCC::from_text("tRE3"), FourCC::from_text("tstr").value))},
                                       spell.motion.position, match.clock().match, ++match.terrain_revision});
        spell.effect_target = nullptr;
        spell.remaining = static_cast<int>(spell.data->duration * 60);
        spell.next_pulse = 45;
        spell.ability.clock = 0;
        spell.sound_event(match, 1);
        // A wizard owner holds the rock until the cast is released; any other owner lets it go at once.
        const auto* caster = dynamic_cast<const CreatureActor*>(spell.owner);
        spell.stage = caster && (caster->kind_flags & 1) ? 1 : 2;
    }
    if (spell.stage == 1) {
        if (const auto next = mid_cast(spell, 2, 240))
            spell.stage = static_cast<int>(next);
        spell.next_pulse -= ticks;
        if (spell.stage == 1) {
            halo_orbit(match, spell, ticks);
            return 1;
        }
    }
    if (spell.stage == 2) {
        if ((spell.next_pulse -= ticks) <= 0)
            spell.stage = 16;
        else {
            halo_orbit(match, spell, ticks);
            return 1;
        }
    }
    if (spell.stage == 16) {
        // Collisions start here (0x445590 stage 16 clears the stale contact bits first).
        const Vec3 velocity = spell.motion.velocity;
        spell.launch_projectile(match);
        spell.motion.velocity = velocity;
        spell.state_flags &= ~3u;
        spell.stage = 17;
    } else if (spell.stage == 17 && (spell.state_flags & 3)) {
        // 0x445520: whatever the drifting rock struck (or the ground) shatters it there.
        spell.capture_collision(match);
        spell.stage = 240;
    }
    if (spell.stage == 17) {
        if (!halo_orbit(match, spell, ticks))
            spell.stage = 240;
        else if ((spell.remaining -= ticks) <= 0)
            spell.stage = 240;
        else if (spell.ability.clock <= static_cast<int>(match.clock().match)) {
            // 0x493150 every 16 ticks: the nearest unnamed unit within the spread.
            spell.ability.clock = static_cast<int>(match.clock().match) + 16;
            if (auto* victim = nearest_unit(match, spell.motion.position, spell.data->spread_limit,
                                            (spell.owner ? spell.owner : &spell), true, &spell)) {
                spell.effect_target = victim;
                mark_hit(*victim, spell);
                spell.stage = 32;
            }
        }
        if (spell.stage == 17)
            return 1;
    }
    if (spell.stage == 32) {
        // 0x445400: home with collisions on and a 'dust' box trail sized from the rock (0x437C10).
        spell.sound_event(match, 2);
        spell.remaining = 600;
        const Vec3 velocity = spell.motion.velocity;
        spell.launch_projectile(match);
        spell.motion.velocity = velocity;
        Vec3 low = spell.motion.box_min, high = spell.motion.box_max;
        if (const auto* geometry = spell.model_geometry()) {
            low = {geometry->bounds.minimum.x, geometry->bounds.minimum.y, geometry->bounds.minimum.z};
            high = {geometry->bounds.maximum.x, geometry->bounds.maximum.y, geometry->bounds.maximum.z};
        }
        const float diagonal = std::sqrt((high.x - low.x) * (high.x - low.x) + (high.y - low.y) * (high.y - low.y) +
                                         (high.z - low.z) * (high.z - low.z));
        SpellActor::launch_box_trail(match, spell, 0x0a011008, FourCC::from_text("dust"), diagonal * .4f,
                                     std::clamp(diagonal * .15f, .25f, 2.f), 1, 1);
        spell.stage = 33;
    }
    if (spell.stage == 33) {
        spell_target_point(spell.point, spell.effect_target);
        spell_approach_velocity(spell.motion, spell.point, ticks * spell.data->acceleration,
                                spell.data->speed, 7, 10);
        if (spell.state_flags & 3) {
            spell.capture_collision(match);
            spell.stage = 240;
        } else if ((spell.remaining -= ticks) <= 0)
            spell.stage = 240;
        if (spell.stage == 33)
            return 1;
    }
    // Stage 240: 0x439270 shatter and 0x44D7E0 damage.
    strike(match, spell, spell.impact_target, spell.motion.position, false);
    spell.sound_event(match, 4);
    SpellActor::launch_rock_shatter(match, spell);
    return remove_spell(spell);
}
int run_cast_spiral(Match& match, SpellActor& spell, int ticks) {
    // 0x43A840: a streamer point circling the caster's box at (alpha + 1) x pi/30 rad a tick, climbing from its
    // bottom to its top with the cast alpha squared; 30 ticks after the alpha settles, or once the cast
    // resolves, the tail drains away.
    auto* caster = dynamic_cast<CreatureActor*>(spell.effect_target);
    auto& streamer = spell.altar_streamer;
    if (!caster || !caster->alive()) {
        if (!spell.stage)
            return remove_spell(spell);
        spell.effect_target = caster = nullptr;
        spell.stage = 240;
    }
    if (!spell.stage) {
        streamer.enabled = true;
        streamer.primitive.configure(spell.remaining, .5f);
        spell.remaining = 30;
        spell.stage = 1;
    }
    if (spell.stage != 240 && caster) {
        const auto [state, alpha] = caster->cast_state();
        spell.cast_alpha = alpha;
        if (state)
            spell.stage = 240;
    }
    spell.age += (spell.cast_alpha + 1) * .20943934f * ticks * .5f;
    if (caster) {
        const auto& m = caster->motion;
        const Vec3 centre{(m.box_min.x + m.box_max.x) * .5f, (m.box_min.y + m.box_max.y) * .5f,
                          (m.box_min.z + m.box_max.z) * .5f},
            half{(m.box_max.x - m.box_min.x) * .5f, (m.box_max.y - m.box_min.y) * .5f,
                 (m.box_max.z - m.box_min.z) * .5f};
        spell.motion.position = {m.position.x + centre.x + std::sin(spell.age) * half.x,
                                 m.position.y + centre.y + std::cos(spell.age) * half.y,
                                 m.position.z + centre.z + 2 * half.z * spell.cast_alpha * spell.cast_alpha - half.z};
    }
    if ((spell.cast_alpha >= 1 || spell.cast_alpha <= 0) && spell.remaining-- <= 0)
        spell.stage = 240;
    streamer.ticks += ticks;
    const auto frames = match.texture_frames ? match.texture_frames(streamer.texture) : 1u;
    return streamer.primitive.add_point(spell.stage == 240 ? nullptr : &spell.motion.position, streamer.ticks,
                                        frames)
               ? 1
               : remove_spell(spell);
}
int run_rift_demon(Match& match, SpellActor& spell, int ticks) {
    // DemonicRiftDemonRun 0x430e60: home, 0x44d7e0, then the streamer tail.
    if (!spell.stage) {
        spell.ability.origin = spell.motion.position;
        spell.launch_projectile(match);
        spell.motion.collision_mask = 0;
        // 0x430B40: a quarter of the aim sideways, full speed up, renormalized to 'speed'.
        auto& v = spell.motion.velocity;
        v.x *= .25f;
        v.y *= .25f;
        v.z = spell.data->speed;
        if (const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); length != 0)
            v = {v.x * spell.data->speed / length, v.y * spell.data->speed / length,
                 v.z * spell.data->speed / length};
        spell.remaining = 30;
        spell.altar_streamer.enabled = true;
        spell.altar_streamer.texture = FourCC{static_cast<unsigned>(
            spell.data->integer(FourCC::from_text("tdtx"), FourCC::from_text("rift").value))};
        spell.altar_streamer.material = 0x1a010808;
        spell.altar_streamer.primitive.configure(spell.data->integer(FourCC::from_text("idln"), 45), .5f);
        spell.sound_event(match, 2);
        spell.stage = 1;
    }
    if (spell.stage == 1 && (spell.remaining -= ticks) <= 0) {
        spell_lead_target(spell.point, spell.effect_target, spell.data->speed, spell.motion, *match.terrain);
        const float distance = spell_approach_velocity(
            spell.motion, spell.point, ticks * spell.data->acceleration, spell.data->speed, 7, 1);
        if (distance <= 1)
            spell.stage = 16;
    } else if (spell.stage == 16) {
        spell.sound_event(match, 4);
        if (auto* victim = living_unit(spell.effect_target)) {
            strike(match, spell, victim, spell.motion.position, false);
            spell.ability.length = std::max(1.f, victim->motion.box_max.z * 2);
        }
        spell.stage = 17;
        spell.age = 0;
    } else if (spell.stage == 17) {
        spell.age += ticks * .10472f;
        Vec3 centre = spell.point;
        spell_target_point(centre, spell.effect_target);
        const float progress = std::min(1.f, spell.age * .03978877f), radius = 2 * (1 - progress * progress);
        spell.point = {centre.x + std::sin(spell.age) * radius, centre.y + std::cos(spell.age) * radius,
                       centre.z + spell.ability.length * progress};
        spell_approach_velocity(spell.motion, spell.point, ticks * spell.data->acceleration * 4,
                                spell.data->speed * .75f);
        if (progress >= 1 || !spell.effect_target) {
            spell.sound_event(match, 4); // Stage 32 flies off to a random point.
            spell.effect_target = nullptr;
            spell.point = {spell.motion.position.x + match.random().next() * (40.f / 32768) - 20,
                           spell.motion.position.y + match.random().next() * (40.f / 32768) - 20,
                           spell.motion.position.z + match.random().next() * (15.f / 32768) + 10};
            spell.stage = 33;
        }
    } else if (spell.stage == 33 || spell.stage == 49) {
        // 0x430D40 thresholds: 2 out to the random point, 10 back home to the rift (stage 48 sound 4).
        const float reach = spell.stage == 33 ? 2.f : 10.f;
        const float distance = spell_approach_velocity(
            spell.motion, spell.point, ticks * spell.data->acceleration, spell.data->speed, 7, reach);
        if (distance <= reach) {
            if (spell.stage == 33) {
                spell.sound_event(match, 4);
                spell.point = spell.ability.origin;
                spell.stage = 49;
            } else {
                spell.sound_event(match, 4);
                spell.motion.flags = 0;
                spell.stage = 240;
            }
        }
    }
    auto& tail = spell.altar_streamer;
    tail.ticks += ticks;
    const unsigned frames = match.texture_frames ? match.texture_frames(tail.texture) : 1u;
    return tail.primitive.add_point(spell.stage == 240 ? nullptr : &spell.motion.position, tail.ticks, frames)
               ? 1
               : remove_spell(spell);
}
int run_animate_streamer(Match& match, SpellActor& spell, int ticks) {
    // AnimateDeadStreamerRun 0x42f040: cubic from the caster (+460) to the soul (+476).
    auto& streamer = spell.altar_streamer;
    if (!spell.stage) {
        const Vec3 start = spell.owner && spell.owner->source_motion()
                               ? spell.owner->source_motion()->position
                               : spell.motion.position;
        Vec3 end = start;
        if (spell.effect_target)
            spell_target_point(end, spell.effect_target);
        streamer.enabled = true;
        streamer.texture = FourCC{static_cast<unsigned>(
            spell.data->integer(FourCC::from_text("tstx"), FourCC::from_text("and2").value))};
        streamer.material = 0x1a010808;
        streamer.primitive.configure(spell.data->integer(FourCC::from_text("inum"), 20), .5f);
        const Vec3 delta{end.x - start.x, end.y - start.y, end.z - start.z};
        // A bow of +-5 along the caster's first orientation row, 1.2x at the near control point.
        const float bow = match.random().next() * 10.f * .000030517578125f - 5;
        const auto* caster = spell.owner ? spell.owner->source_motion() : nullptr;
        const float bx = caster ? bow * caster->orientation[0] : 0, by = caster ? bow * caster->orientation[1] : 0;
        streamer.controls[0] = start;
        streamer.controls[1] = {start.x + delta.x * .1f + bx * 1.2f, start.y + delta.y * .1f + by * 1.2f,
                                start.z + delta.z * .1f + 10.f};
        streamer.controls[2] = {start.x + delta.x * .8f + bx, start.y + delta.y * .8f + by,
                                start.z + delta.z * .8f + 5.f};
        streamer.controls[3] = end;
        spell.motion.position = start;
        spell.stage = 1;
        spell.cast_alpha = 0;
    }
    if (spell.effect_target && spell.cast_alpha < 1)
        spell_target_point(streamer.controls[3], spell.effect_target);
    spell.cast_alpha = std::min(1.f, spell.cast_alpha + ticks * .0083333338f);
    const auto blend = [&](auto member) {
        const auto a = streamer.controls[0].*member, b = streamer.controls[1].*member,
                   c = streamer.controls[2].*member, d = streamer.controls[3].*member;
        const double t = spell.cast_alpha, u = 1 - t;
        return static_cast<float>(u * u * u * a + 3 * u * u * t * b + 3 * u * t * t * c + t * t * t * d);
    };
    spell.motion.position = {blend(&Vec3::x), blend(&Vec3::y), blend(&Vec3::z)};
    streamer.ticks += ticks;
    const auto frames = match.texture_frames ? match.texture_frames(streamer.texture) : 1u;
    if (spell.cast_alpha >= 1.f && spell.stage == 1) {
        spell.stage = 240;
    }
    if (!streamer.primitive.add_point(spell.stage == 240 ? nullptr : &spell.motion.position, streamer.ticks,
                                      frames))
        return remove_spell(spell);
    return 1;
}
int wait_cast(Match& match, SpellActor& spell, unsigned released = 16, unsigned aborted = 240) {
    if (!spell.stage) {
        SpellActor::launch_cast_hands(match, spell);
        spell.stage = 1;
    }
    if (spell.stage == 1) {
        if (const auto next = mid_cast(spell, released, aborted))
            spell.stage = static_cast<int>(next);
        if (spell.stage == 1)
            return 1;
    }
    return 0;
}
void rainbow_curve(SpellActor& spell, Vec3 end, float start, float finish) {
    // RAINBOW::SetParams / MakeNewCurve 0x1601a360 / 0x1601a5d0.
    const Vec3 from = spell.ability.origin;
    const float lift = static_cast<float>(std::sqrt(distance_squared(from, end))) * .5f;
    const int count = std::max(2, static_cast<int>((finish - start) * 32));
    spell.crossline.points.clear();
    for (int n = 0; n < count; ++n) {
        const float t = start + (finish - start) * n / (count - 1), u = 1 - t;
        const float blend = t * t * (3 - 2 * t);
        const Vec3 at{from.x + (end.x - from.x) * blend, from.y + (end.y - from.y) * blend,
                      from.z + (end.z - from.z) * blend + 3 * u * t * lift};
        spell.crossline.points.push_back({at, .5f, n + 1 < count, static_cast<std::uint8_t>(n ? 255 : 0)});
    }
}
CreatureActor* rainbow_target(Match& match, SpellActor& spell, Vec3 origin, float radius) {
    CreatureActor* best = nullptr;
    float missing = -1;
    const auto visit = [&](MatchObject& object) {
        auto* actor = living_unit(&object);
        if (!actor || !actor->friendly(spell.owner ? spell.owner : &spell) || actor->named(hop_name(spell)) ||
            distance_squared(origin, actor->motion.position) > static_cast<double>(radius) * radius)
            return 0;
        const auto maximum = actor->stats.base[CreatureStats::health];
        if (maximum <= 0)
            return 0;
        const float fraction = 1 - actor->stats.current[CreatureStats::health] / static_cast<float>(maximum);
        if (fraction > missing) {
            best = actor;
            missing = fraction;
        }
        return 0;
    };
    match.visit_group(0, 5, visit);
    match.visit_group(1, 5, visit);
    return best;
}
int run_rainbow(Match& match, SpellActor& spell, int ticks) {
    if (spell.stage == 64 || spell.stage == 80) {
        if (spell.stage == 64 && (spell.remaining -= ticks) <= 0)
            spell.stage = 80;
        else if (spell.stage == 80)
            spell.ability.draw_back += ticks / 60.f;
        if (spell.ability.draw_back >= 1)
            return remove_spell(spell);
        rainbow_curve(spell, spell.ability.destination, spell.ability.draw_back, 1);
        return 1;
    }
    if (const auto waiting = wait_cast(match, spell))
        return waiting;
    if (spell.stage == 16) {
        // 0x42C990: the first arc ends on the cast target, or on the cast point without one.
        spell.effect_target = living_unit(spell.effect_target);
        spell.ability.origin = spell.motion.position;
        if (!spell.rate)
            if (auto* caster = dynamic_cast<CreatureActor*>(spell.owner))
                caster->write_reference_point(spell.ability.origin, 0, false);
        if (!spell.rate)
            SpellActor::launch_generic_trail(match, spell, 0x1a012008, FourCC::from_text("glo2"), .5f, .5f, 2,
                                             1.5f, 0);
        spell.cast_alpha = 0;
        spell.crossline.enabled = spell.crossline.continuous = true;
        spell.crossline.texture =
            FourCC{static_cast<unsigned>(spell.data->integer(FourCC::from_text("ttex"), 1919836791u))};
        spell.stage = 17;
        spell.stop_sound_event(2);
        spell.sound_event(match, 2);
    }
    if (spell.stage == 17) {
        Vec3 end = spell.point;
        spell_target_point(end, spell.effect_target);
        spell.cast_alpha = std::min(1.f, spell.cast_alpha + ticks / 60.f); // 0x42C990, 1/60 per tick.
        rainbow_curve(spell, end, 0, spell.cast_alpha);
        if (spell.cast_alpha >= 1) {
            if (auto* target = living_unit(spell.effect_target)) {
                SpellActor::heal(match, *target, target->stats.base[CreatureStats::health], 256, 0);
                attach_hit(match, spell, target, target->motion.position);
                mark_hit(*target, spell);
            }
            auto& arc =
                launch_child(match, spell, SpellActor::Effect::rainbow, nullptr, spell.motion.position);
            arc.crossline = spell.crossline;
            arc.ability.origin = spell.ability.origin;
            arc.ability.destination = end;
            arc.ability.draw_back = 0;
            arc.stage = 64;
            arc.remaining = static_cast<int>(spell.data->real(FourCC::from_text("flif"), 1) * 60);
            spell.motion.position = end;
            const float reach = (6 - ++spell.rate) * (1.f / 6) * spell.data->spread_limit;
            if (spell.rate >= 6)
                return remove_spell(spell);
            spell.effect_target = rainbow_target(match, spell, end, reach);
            if (!spell.effect_target) {
                // 0x42cc70: retain the remaining arcs even with no eligible
                // creature; wander to a random ground point within the hop range.
                spell.point = {end.x + (match.random().next() / 16384.f - 1) * reach,
                               end.y + (match.random().next() / 16384.f - 1) * reach, end.z};
                if (match.terrain) {
                    spell.point.z =
                        static_cast<float>(terrain_surface(*match.terrain, spell.point.x, spell.point.y));
                    if (spell.point.z <= 0)
                        return remove_spell(spell);
                }
            }
            spell.stage = 16;
        }
    }
    return spell.stage >= 240 ? remove_spell(spell) : 1;
}
int run_dragonfire(Match& match, SpellActor& spell, int ticks) {
    // 0x426430. rate counts failed searches, ability.draw_back the damage dealt, model_scale eases to the
    // stage's goal (0x426340): the cast alpha at 1/120, then 1 at .03, then 0 at .01 per tick.
    if (!spell.stage) {
        spell.set_projectile_model(match,
                                   FourCC{static_cast<unsigned>(spell.data->integer(
                                       FourCC::from_text("tmdl"), FourCC::from_text("dfir").value))},
                                   1);
        spell.model_scale = 0;
        // 0x438050(emission 1.25, life 1.75, box .1, size .3) and 0x4367C0(2, 0, .5).
        SpellActor::launch_model_trail(match, spell, 0x1a012008, FourCC::from_text("firy"), 1.25f, 1.75f, .1f,
                                       .3f, true);
        SpellActor::launch_glow(match, spell, 0x1a012008, "firy", 2, 0, .5f, 1, &spell, 1);
        spell.light = {true, spell.motion.position, {1, .5f, 0}, 0, 1, 75}; // 0x426340, dword_4CB8E0.
        spell.rate = 0;
        spell.ability.draw_back = 0;
        spell.sound_event(match, 1);
    }
    const auto ease_scale = [&](float goal, float step) {
        spell.model_scale = goal > spell.model_scale ? std::min(goal, spell.model_scale + step * ticks)
                                                     : std::max(goal, spell.model_scale - step * ticks);
        spell.light.position = spell.motion.position;
        spell.light.intensity = spell.model_scale;
        spell.model_frame += ticks * .41666666f;
    };
    if (const auto waiting = wait_cast(match, spell)) {
        // 0x425C30: circle the caster, widening and rising with the scale.
        if (spell.owner && spell.owner->source_motion()) {
            const float scale = spell.model_scale;
            spell.age += ticks * (.010471967f + .041887868f * scale);
            const float a = scale * scale, radius = 2 + 3 * a;
            spell.motion.position = spell.owner->source_motion()->position;
            spell.motion.position.x += std::sin(spell.age) * radius;
            spell.motion.position.y += std::cos(spell.age) * radius;
            spell.motion.position.z += 4 * a;
            spell.motion.orientation = spell_orientation({std::cos(spell.age), -std::sin(spell.age), 0});
        }
        ease_scale(spell.cast_alpha, 1.f / 120);
        return waiting;
    }
    if (spell.stage == 16) {
        // 0x4262A0: sound 2, fly off along the facing at the data speed.
        spell.launch_projectile(match);
        const auto& o = spell.motion.orientation;
        spell.motion.velocity = {o[3] * spell.data->speed, o[4] * spell.data->speed, o[5] * spell.data->speed};
        spell.motion.collision_mask = 0;
        spell.stage = 17;
        spell.sound_event(match, 2);
        spell.sound_event(match, 8);
    }
    const auto retarget = [&](CreatureActor* from) { // 0x425EA0.
        spell.effect_target = nullptr;
        if (from) {
            // Overshoot: 10 ahead of the victim and 25 above it.
            const auto& o = from->motion.orientation;
            spell.point = {from->motion.position.x + o[3] * 10, from->motion.position.y + o[4] * 10,
                           from->motion.position.z + 25};
        } else {
            // 0x492840: enemies 25..spread away, not yet named by this spell; a random one.
            std::vector<CreatureActor*> found;
            const float low = 25 * 25, high = spell.data->spread_limit * spell.data->spread_limit;
            MatchObject* side = spell.owner ? spell.owner : &spell;
            for (auto* actor : units_near(match, spell.motion.position, spell.data->spread_limit + 1)) {
                const double d = distance_squared(spell.motion.position, actor->source_motion()->position);
                if (d >= low && d <= high && actor->enemy(side) && !actor->named(hop_name(spell)) &&
                    found.size() < 64)
                    found.push_back(actor);
            }
            if (!found.empty()) {
                auto* pick = found[match.random().next() % found.size()];
                spell.effect_target = pick;
                spell_target_point(spell.point, pick);
            } else {
                ++spell.rate;
                spell.point.x = spell.motion.position.x + (match.random().next() - 0x4000) * .00152587890625f;
                spell.point.y = spell.motion.position.y + (match.random().next() - 0x4000) * .00152587890625f;
                const float ground =
                    match.terrain ? static_cast<float>(terrain_surface(*match.terrain, spell.point.x, spell.point.y))
                                  : spell.motion.position.z;
                spell.point.z = ground <= 0 ? spell.motion.position.z
                                            : ground + 2 + 50.f * match.random().next() / 32768;
            }
        }
        spell.sound_event(match, 8);
    };
    if (spell.stage == 17 || spell.stage >= 240) {
        // 0x426090: steer for the target's body or the wander point.
        if (spell.stage == 17) {
            if (auto* victim = living_unit(spell.effect_target)) {
                spell_target_point(spell.point, victim);
                if (spell_box_distance_squared(spell.motion.position, victim->motion, 16) <= 1) {
                    spell.sound_event(match, 4);
                    retarget(victim);
                }
            } else if (spell.effect_target)
                spell.effect_target = nullptr;
            else if (distance_squared(spell.point, spell.motion.position) < 25)
                retarget(nullptr);
        }
        const float d2 = static_cast<float>(distance_squared(spell.motion.position, spell.point));
        const float acceleration = spell.data->acceleration * .5f * (d2 < 400 ? 3 - d2 * .005f : 1);
        spell_approach_velocity(spell.motion, spell.point, ticks * acceleration, spell.data->speed, 7, 2);
        spell.motion.orientation = spell_orientation(spell.motion.velocity);
        if (match.terrain) {
            const float floor = static_cast<float>(terrain_surface(*match.terrain, spell.motion.position.x,
                                                                   spell.motion.position.y)) +
                                1;
            if (spell.motion.position.z < floor) {
                spell.motion.position.z = floor;
                spell.motion.velocity.z = std::max(0.f, spell.motion.velocity.z);
            }
        }
    }
    if (spell.stage == 17) {
        // 0x425D70: anything but the caster within 4 of the dragon burns and takes half damage per
        // second; one it passes straight through (within 1 across) takes the full damage once.
        const auto brush = [&](MatchObject& object) {
            auto* actor = living_unit(&object);
            if (!actor || actor == spell.owner ||
                spell_box_distance_squared(spell.motion.position, actor->motion, 16) > 16)
                return 0;
            const float dx = spell.motion.position.x - actor->motion.position.x,
                        dy = spell.motion.position.y - actor->motion.position.y;
            if (dx * dx + dy * dy < 1 && !actor->named(hop_name(spell))) {
                spell_damage(match, &spell, actor, spell.data->damage_type, spell.data->amount, 0,
                             spell.motion.position);
                mark_hit(*actor, spell);
                attach_hit(match, spell, actor, spell.motion.position);
                spell.ability.draw_back += spell.data->amount;
            }
            actor->burn_near(spell.motion.position, 10, 180);
            actor->accumulate_damage({spell.data->damage_type, spell.data->amount / 2, spell.reference()},
                                     ticks);
            spell.ability.draw_back += spell.data->amount * ticks / 60.f;
            return 0;
        };
        match.visit_group(0, 5, brush);
        match.visit_group(1, 5, brush);
        if (6.f * spell.data->amount < spell.ability.draw_back || spell.rate > 5) {
            spell.stage = 240;
            spell.effect_target = nullptr;
            spell.motion.flags = 0;
            spell.stop_sound_event(2);
        }
    }
    if (spell.stage >= 240) {
        ease_scale(0, .01f);
        if (spell.model_scale <= 0)
            return remove_spell(spell);
        return 1;
    }
    ease_scale(1, .03f);
    return 1;
}
void erupt_wave(Match& match, SpellActor& spell, float progress) {
    // 0x441010 / 0x440f60 rebuild an expanding annulus over the peak and remove its previous samples.
    auto& patch = spell.land_wave;
    match.displace(patch, 0);
    if (progress >= 1)
        return;
    const float root = std::sqrt(progress), radius = 80.f * root, half_width = 20.f * std::sqrt(root);
    const float inner = std::max(1.f, radius - half_width), outer = radius + half_width;
    const float peak2 = radius * radius, inner2 = inner * inner, outer2 = outer * outer;
    for (int y = 0; y < patch.size; ++y)
        for (int x = 0; x < patch.size; ++x) {
            const int dx = x - patch.size / 2, dy = y - patch.size / 2;
            const float squared = 100.f * (dx * dx + dy * dy);
            float weight = 0;
            if (squared >= inner2 && squared < outer2)
                weight = squared < peak2 ? (squared - inner2) / (peak2 - inner2)
                                         : (outer2 - squared) / (outer2 - peak2);
            patch.samples[y * patch.size + x] = static_cast<std::int16_t>(128 * weight);
        }
    const float envelope = progress < .2f ? 5.f * progress : std::min(1.f, 4.f * (1.f - progress));
    match.displace(patch, .234375f * envelope);
    if (progress >= .5f && envelope < .9f)
        return;
    for (const unsigned group : {0u, 1u})
        match.visit_group(group, 5, [&](MatchObject& object) {
            auto* actor = living_unit(&object);
            if (actor && (actor->state_flags & 2) &&
                std::abs(radius - std::hypot(actor->motion.position.x - spell.motion.position.x,
                                            actor->motion.position.y - spell.motion.position.y)) < half_width &&
                match.random().next() < 8192)
                actor->knock_down(0, 10.f);
            return 0;
        });
}
void launch_mrm_debris(Match& match, SpellActor& source, int count, float speed, FourCC model, float scale,
                       std::uint32_t material, FourCC trail, Vec3 at, float life);
void erupt_debris(Match& match, SpellActor& spell) {
    // 0x444950: one call in 15 (0x435BC0(4)) throws a 'roks' chunk from the ground 5 to 30 out, leaning
    // outward at .25, with a 'dirt' trail, the 0x4380F0 ground effects and sound 8; it lasts 3 s (0x438590).
    const auto model = FourCC::from_text("roks");
    if (!(4 * 546.1333333333333 > match.random().next()) || !match.spell_models.contains(model))
        return;
    const float distance = match.random().next() * 25.f / 32768 + 5;
    const float heading = match.random().next() * 6.2831802f / 32768;
    const float s = std::sin(heading), c = std::cos(heading);
    Vec3 at{spell.motion.position.x + distance * s, spell.motion.position.y + distance * c, spell.motion.position.z};
    if (match.terrain)
        at.z = static_cast<float>(terrain_surface(*match.terrain, at.x, at.y));
    auto& chunk = launch_child(match, spell, SpellActor::Effect::mrm_debris, nullptr, at);
    chunk.set_projectile_model(match, model, 1);
    chunk.ability.length = 1;
    chunk.launch_projectile(match);
    chunk.motion.position = at;
    const float norm = .25f / std::sqrt(s * s + c * c + 1);
    chunk.motion.velocity = {s * norm, c * norm, norm};
    chunk.motion.flags = 61;
    chunk.motion.collision_mask = 0x84000;
    const auto spin = [&] { return match.random().next() * .10471967f / 32768 - .052359834f; };
    chunk.motion.angular_velocity.x = spin();
    chunk.motion.angular_velocity.y = spin();
    chunk.motion.angular_velocity.z = spin();
    chunk.remaining = 180;
    chunk.stage = 1;
    SpellActor::launch_model_trail(match, chunk, 0x0a011008, FourCC::from_text("dirt"), 1, 1, .3f, .5f, true);
    match.shake(at, 16, 50);
    SpellActor::launch_earth_burst(match, chunk, at, 3, 24);
    match.scar_requests.push_back({FourCC{static_cast<unsigned>(spell.data->integer(
                                       FourCC::from_text("tRE3"), FourCC::from_text("tstr").value))},
                                   at, match.clock().match, ++match.terrain_revision});
    chunk.sound_event(match, 8);
}
int run_erupt(Match& match, SpellActor& spell, int ticks) {
    if (!spell.stage) {
        // 0x4447D0: the mound sits on the 10-unit grid point nearest the target, with a 0x436DF0 'dust'
        // spray (box 15, size 2, 1 s) and, for a caster, the 0x436AC0 'dirt' column.
        spell.motion.position = spell.point;
        if (match.terrain)
            spell.motion.position.z =
                static_cast<float>(terrain_surface(*match.terrain, spell.point.x, spell.point.y));
        spell.start_mound(match, FourCC::from_text("erpt"), 9, 15.f, 44.f);
        SpellActor::launch_dirt_spray(match, spell, nullptr, FourCC::from_text("dust"), true, 15, 2, 1);
        if (spell.owner)
            SpellActor::launch_caster_column(match, spell, 0x0a011008, FourCC::from_text("dirt"), 1, 0, 1, 1.5f, 1);
        spell.sound_event(match, 1);
        if (!spell.owner) // 0x44C140: without a caster the mound rises over two seconds instead.
            spell.stage = 2;
    }
    if (spell.stage == 2) {
        erupt_debris(match, spell);
        spell.age = std::min(1.f, spell.age + ticks / 120.f);
        if (spell.age >= 1)
            spell.stage = 16;
    }
    const bool waiting = wait_cast(match, spell) != 0;
    const bool moving_land = !spell.land_mound.samples.empty();
    if (waiting) {
        erupt_debris(match, spell);
        const float step = ticks / 240.f;
        spell.age = std::clamp(spell.cast_alpha, spell.age - step, spell.age + step);
    } else if (spell.stage == 16) {
        spell.age = moving_land ? std::min(1.f, spell.age + ticks / 240.f) : 1.f;
        if (spell.age == 1.f)
            spell.stage = 32;
    }
    if (spell.stage == 32) {
        if (moving_land)
            match.displace(spell.land_mound, .75f);
        else
            aim_point(spell);
        if (match.terrain)
            spell.motion.position.z = static_cast<float>(
                terrain_surface(*match.terrain, spell.motion.position.x, spell.motion.position.y));
        spell.stop_sound_event(1);
        spell.sound_event(match, 2);
        // 0x439830 'tcld' (default 'dust') splatter, 0x439320's 64 'rock' bits from a +-3 box at 10, and
        // 0x438760's eight 'roks' chunks with 'dirt' trails.
        SpellActor::launch_dust_splatter(match, spell, 0x0a011008,
                                         FourCC{static_cast<unsigned>(spell.data->integer(
                                             FourCC::from_text("tcld"), FourCC::from_text("dust").value))},
                                         spell.motion.position, {7.5f, 7.5f, 5}, 8, .3f);
        {
            const Vec3 low = spell.motion.box_min, high = spell.motion.box_max;
            spell.motion.box_min = {-3, -3, -3};
            spell.motion.box_max = {3, 3, 3};
            SpellActor::launch_fragment_burst(match, spell, FourCC::from_text("rock"), 64, 10);
            spell.motion.box_min = low;
            spell.motion.box_max = high;
        }
        if (match.spell_models.contains(FourCC::from_text("roks")))
            launch_mrm_debris(match, spell, 8, .25f, FourCC::from_text("roks"), 1, 0x0a011008,
                              FourCC::from_text("dirt"), spell.motion.position, 5);
        match.scar_requests.push_back({FourCC{static_cast<unsigned>(spell.data->integer(
                                           FourCC::from_text("tscr"), FourCC::from_text("escr").value))},
                                       spell.motion.position, match.clock().match, ++match.terrain_revision});
        spell_damage(match, &spell, nullptr, spell.data->damage_type, spell.data->amount, spell.data->radius,
                     spell.motion.position);
        spell_exclusion_field(match, {spell.motion.position.x, spell.motion.position.y, spell.motion.position.z - 20},
                              20.f, 40.f, 1.f / 3.f, 7, 5); // 0x44D9A0, 20 below the peak.
        match.shake(spell.motion.position, 2, 10.f);
        SpellActor::launch_shockwave(match, spell, std::max(spell.data->radius, 7.5f));
        attach_hit(match, spell, nullptr, spell.motion.position);
        if (!moving_land)
            return remove_spell(spell);
        spell.land_wave = Terrain::Displacement(spell.motion.position, 21);
        spell.remaining = 32;
        spell.stage = 40;
    } else if (spell.stage == 40) {
        spell.remaining = std::max(0, spell.remaining - ticks);
        spell.rate -= ticks;
        if (spell.rate <= 0 || !spell.remaining) {
            erupt_wave(match, spell, 1.f - spell.remaining / 32.f);
            spell.rate = 6;
        }
        if (!spell.remaining) {
            spell.age = .675f;
            spell.stage = 48;
        }
    } else if (spell.stage == 48) {
        spell.age = std::max(-.03333334f, spell.age - ticks * .01740741f);
    } else if (spell.stage == 240) {
        spell.age = std::max(0.f, spell.age - ticks / 60.f);
    }
    const bool finished = (spell.stage == 48 && spell.age <= -.03333334f) ||
                          (spell.stage == 240 && spell.age == 0);
    spell.next_pulse -= ticks;
    if (spell.next_pulse <= 0 || finished) {
        match.displace(spell.land_mound, spell.age * .75f);
        spell.next_pulse = 8;
    }
    if (finished) {
        spell.land_mound.samples.clear(); // 0x444b70 leaves the final shallow depression in LAND.
        return remove_spell(spell);
    }
    if (moving_land && spell.age > 0) {
        match.shake(spell.motion.position, static_cast<int>(std::sqrt(spell.age) * 16), 100.f);
        if (match.navigation) {
            const float inner = spell.age * 5.f;
            const std::array footprint{inner, std::max(inner, spell.data->radius * spell.age * 1.5f)};
            match.navigation->stamp(3, spell.motion.position, footprint, 32);
        }
    }
    return 1;
}
int run_chain_lightning(Match& match, SpellActor& spell, int ticks) {
    if (spell.stage == 64)
        return spell.advance_beam(match, ticks) ? 1 : remove_spell(spell);
    // ChainLightningStart 0x420320: Lightning's 'cst0' hand sparkle (size .6, life 1, +-.5, emission rising
    // from .1 over a second; kept in age) and 0x435B50(8) sparks from a moving caster during the cast.
    const bool casting = spell.stage <= 1 && spell.owner;
    if (!spell.stage && spell.owner) {
        auto& p = spell.particles(match);
        spell.particle_material(match, p, 0x1a012008, FourCC::from_text("cst0"));
        const float size = .6f, life = 1, low[3]{-.5f, -.5f, -.5f}, high[3]{.5f, .5f, .5f};
        p.set_property(132, &size, &life);
        p.set_property(2, low, high);
        spell.age = .1f;
    }
    if (!spell.particle_systems.empty()) {
        if (casting) {
            spell.age = std::min(1.f, spell.age + ticks / 60.f);
            if (const auto* caster = spell.owner->source_motion()) {
                const auto& o = caster->orientation; // unk_4CB134 hand offset.
                spell.motion.position = {caster->position.x + o[3] + o[6] * 2, caster->position.y + o[4] + o[7] * 2,
                                         caster->position.z + o[5] + o[8] * 2};
            }
        } else
            spell.age = 0;
        spell.host.time += ticks;
        spell.particle_systems.front().advance(spell.host.time, spell.age);
    }
    if (const auto waiting = wait_cast(match, spell)) {
        if (spell.stage == 1)
            SpellActor::caster_sparks(match, spell, 8);
        return waiting;
    }
    if (spell.stage == 16) {
        Vec3 target = spell.point;
        spell_target_point(target, spell.effect_target);
        Vec3 from = spell.motion.position;
        if (!spell.rate)
            if (auto* caster = dynamic_cast<CreatureActor*>(spell.owner))
                caster->write_reference_point(from, 0, true);
        // ChainLightningRun 0x420220: 0x4355A0(spell, range, deviation 1.5, 2 strands, flags 1).
        spell.start_beam(match, from, target, 1.5f, 2, 1);
        spell.stage = 17;
    }
    if (spell.stage == 17 && spell.advance_beam(match, ticks) == 2) {
        auto* target = living_unit(spell.effect_target);
        if (target) {
            spell_damage(match, &spell, target, spell.data->damage_type, spell.data->amount,
                         spell.data->radius);
            attach_hit(match, spell, target, target->motion.position);
            mark_hit(*target, spell);
        }
        auto& tail =
            launch_child(match, spell, SpellActor::Effect::chain_lightning, target, spell.motion.position);
        tail.beam = spell.beam;
        tail.ability = spell.ability;
        tail.generic = spell.generic;
        tail.host.rate = spell.host.rate;
        tail.stage = 64;
        spell.motion.position = spell.ability.destination;
        const float reach = (6 - ++spell.rate) * (1.f / 6) * spell.data->spread_limit;
        spell.effect_target = nearest_unit(match, spell.motion.position, reach,
                                           (spell.owner ? spell.owner : &spell), true, &spell);
        if (spell.rate >= 6 || !spell.effect_target)
            return remove_spell(spell);
        spell.stage = 16;
    }
    return spell.stage >= 240 ? remove_spell(spell) : 1;
}
int run_animate_dead(Match& match, SpellActor& spell, int ticks) {
    // AnimateDeadRun 0x42f4f0 drives the shared revival body (0x41B7C0 / 0x41B290 / 0x41B200 / 0x41B300).
    // Only its first call consults the cast (0x441D20): the soul's corpse rises while the caster still casts.
    auto& p = spell.particles(match);
    auto* corpse = dynamic_cast<CreatureActor*>(spell.impact_target);
    if (!spell.stage) {
        auto* soul = dynamic_cast<SoulActor*>(spell.effect_target);
        corpse = soul ? dynamic_cast<CreatureActor*>(soul->anchor) : nullptr;
        spell.impact_target = corpse;
        // 0x41B7C0: 'glo2' sparks sized by the corpse box (0x44CA80 .3, 2, .25; .5 without one) falling
        // under gravity, emitted at full rate; the revival lasts 'duration'.
        spell.particle_material(match, p, 0x1a010001, FourCC::from_text("glo2"));
        float size = .5f;
        if (corpse) {
            const auto& low = corpse->motion.box_min;
            const auto& high = corpse->motion.box_max;
            const float x = high.x - low.x, y = high.y - low.y, z = high.z - low.z;
            size = std::clamp(std::sqrt(x * x + y * y + z * z) * .3f * .5f, .25f, 2.f);
        }
        const float life = 1, gravity[3]{0, 0, -.002725f};
        p.set_property(129, &size, &life);
        p.set_property(4, gravity, gravity);
        p.flags |= 0x10000;
        spell.cast_alpha = 1;
        spell.remaining = static_cast<int>(spell.data->duration * 60);
        spell.stage = 16;
        SpellActor::launch_cast_hands(match, spell);
        spell.next_pulse = 4; // The first streamer leaves four ticks in.
        if (mid_cast(spell, 16, 240) == 240)
            spell.stage = 240;
    } else if (spell.stage == 16) {
        // 0x41B290: 0x4764D0 reconstitutes the soul's corpse; a refusal ends the spell.
        auto* soul = dynamic_cast<SoulActor*>(spell.effect_target);
        spell.stage = soul && corpse && soul->reconstitute(match, *corpse) ? 32 : 240;
    } else if (spell.stage == 32) {
        auto& hit = SpellActor::launch_sacrifice_host(match, spell, SpellActor::Effect::spell_hit);
        hit.effect_target = corpse;
        hit.motion.position = spell.motion.position;
        hit.run(match, 0);
        spell.stop_sound_event(1);
        spell.sound_event(match, 2 | 4);
        if (corpse)
            corpse->fsm().event(49);
        spell.age = 0;
        spell.stage = 33;
    }
    if (spell.stage == 33 && corpse) {
        // 0x41B200: the corpse spins up over two seconds and bobs once.
        spell.age = std::min(1.f, spell.age + ticks / 120.f);
        corpse->motion.yaw += spell.age * ticks * .052359833f;
        corpse->motion.velocity.z = std::sin(spell.age * 3.14159f) * .03f;
        if ((spell.remaining -= ticks) <= 0)
            spell.stage = 128;
    } else if (spell.stage == 128) {
        spell.stop_sound_event(2);
        if (corpse) {
            spell.sound_event(match, 16);
            corpse->fsm().event(50);
        }
        spell.next_pulse = 0;
        spell.stage = 240;
    }
    if (spell.stage == 240) {
        spell.cast_alpha = 0;
        if (!p.alive())
            return remove_spell(spell);
    }
    // Thirty ticks before the end the 0x41B650 rays home on the corpse.
    if (spell.remaining <= 30 && corpse && !spell.had_target) {
        auto& rays = launch_child(match, spell, SpellActor::Effect::gd_resurrect, corpse, spell.motion.position);
        rays.revival_role = SpellActor::RevivalRole::streamer;
        rays.run(match, 0);
        spell.had_target = true;
    }
    // A streamer from the caster to the corpse every 16 ticks, with sound 8, until the revival ends.
    if (spell.next_pulse && (spell.next_pulse -= ticks) <= 0)
        do {
            auto& streamer = launch_child(match, spell, SpellActor::Effect::animate_dead_streamer, corpse,
                                          spell.motion.position);
            streamer.run(match, 0);
            spell.sound_event(match, 8);
        } while ((spell.next_pulse += 16) <= 0);
    // 0x41B300: two chances a tick (at the full rate) of a spark from a random point on the corpse.
    if (spell.stage != 240) {
        if (corpse && corpse->bone_count()) {
            for (int n = 0; n < 2 * ticks; ++n) {
                if (match.random().next() >= static_cast<int>(spell.cast_alpha * 32768))
                    continue;
                Vec3 at;
                if (corpse->write_bone_point(at, match.random().next() % corpse->bone_count(), &match.random())) {
                    p.set_position(at, &at);
                    p.emit(spell.host.time, 1);
                }
            }
        } else if (!corpse)
            spell.stage = 240;
    }
    if (corpse)
        spell_target_point(spell.motion.position, corpse);
    spell.host.time += ticks;
    p.advance(spell.host.time, 0);
    return 1;
}
int run_rain_frogs(Match& match, SpellActor& spell, int ticks) {
    auto& cloud = spell.altar_cloud;
    // 0x42d650: losing the caster fades the cloud out from any later stage.
    if (spell.stage && spell.stage < 240 && !spell.owner)
        spell.stage = 240;
    if (!spell.stage) {
        if (spell.owner) {
            // 'tcst' (default 'frgl') is never zero, so a caster always gets the 0x436AC0 column.
            SpellActor::launch_cast_hands(match, spell);
            SpellActor::launch_caster_column(match, spell, 0x1a012008,
                                             FourCC{static_cast<unsigned>(spell.data->integer(
                                                 FourCC::from_text("tcst"), FourCC::from_text("frgl").value))},
                                             2, 0, 2, 2, 4);
        }
        aim_point(spell);
        spell.ability.origin = spell.motion.position;
        spell.motion.position.z += 75;
        cloud.enabled = true;
        cloud.material = 0x02001000; // 0x428fa0: untextured CLOUDPRIM.
        if (!match.cloud_noise)
            match.cloud_noise = std::make_shared<CloudNoise>(match.visual_random);
        cloud.noise = match.cloud_noise;
        cloud.phase = spell.motion.position;
        spell.remaining = static_cast<int>(spell.data->duration * 60.f);
        spell.next_pulse = 0;
        spell.stage = 1;
    }
    if (spell.stage == 1) {
        if (const auto next = mid_cast(spell, 16, 240))
            spell.stage = static_cast<int>(next);
    }
    if (spell.stage == 16) {
        if (static_cast<int>(match.random().next() % 40) <= ticks) {
            // 0x429060 / 0x44D430: rand() % (units + 4) picks a unit of any side within the spread,
            // otherwise a random spot on the spread disc around the rain point.
            const auto& origin = spell.ability.origin;
            const auto units = units_near(match, origin, spell.data->spread_limit);
            const auto pick = match.random().next() % (units.size() + 4);
            CreatureActor* victim = pick < units.size() ? units[pick] : nullptr;
            Vec3 drop = origin;
            if (victim)
                spell_target_point(drop, victim);
            else {
                const float radius = match.random().next() * spell.data->spread_limit * .000030517578125f,
                            angle = match.random().next() * 6.2831802f * .000030517578125f;
                drop.x += std::sin(angle) * radius;
                drop.y += std::cos(angle) * radius;
                if (match.terrain)
                    drop.z = static_cast<float>(terrain_surface(*match.terrain, drop.x, drop.y));
            }
            auto& frog = launch_child(match, spell, SpellActor::Effect::rain_frog, victim, drop);
            frog.point = drop;
            frog.motion.position.z = spell.motion.position.z;
            frog.had_target = true;
            frog.run(match, 1);
        }
        spell.remaining -= ticks;
        if (spell.remaining <= 0)
            spell.stage = 240;
    }
    const float goal = spell.stage >= 240 ? 0.f : spell.stage == 16 ? 1.f : spell.cast_alpha;
    spell.age = spell.age < goal ? std::min(goal, spell.age + ticks / 120.f)
                                 : std::max(goal, spell.age - ticks / 120.f);
    const float size = (spell.age + 1) * .5f;
    cloud.radii = {spell.data->spread_limit * size, spell.data->spread_limit * size, 15 * size};
    cloud.opacity = spell.age;
    cloud.phase.x += ticks;
    cloud.phase.y += ticks;
    cloud.phase.z += ticks;
    if (spell.stage >= 240 && spell.age == 0)
        return remove_spell(spell);
    return 1;
}
// 0x438760: `count` tumbling `model` chunks thrown up and out at `speed`, each with a 0x438050 trail
// (emission 1, life 1, box .3, size .5) and a `life` second timer (0x438600 / 0x438590).
void launch_mrm_debris(Match& match, SpellActor& source, int count, float speed, FourCC model, float scale,
                       std::uint32_t material, FourCC trail, Vec3 at, float life) {
    for (int n = 0; n < count; ++n) {
        const float heading = match.random().next() * 6.2831802f / 32768 - 3.1415901f;
        const float tilt = std::sin(match.random().next() * 1.5707951f / 32768);
        auto& chunk = launch_child(match, source, SpellActor::Effect::mrm_debris, nullptr, at);
        chunk.set_projectile_model(match, model, scale);
        chunk.ability.length = scale;
        chunk.launch_projectile(match);
        chunk.motion.position = at;
        chunk.motion.velocity = {std::sin(heading) * tilt * speed, std::cos(heading) * tilt * speed, speed * tilt};
        chunk.motion.flags = 61;
        chunk.motion.collision_mask = 0x84000;
        chunk.motion.angular_velocity = {match.random().next() * .10471967f / 32768 - .052359834f,
                                         match.random().next() * .10471967f / 32768 - .052359834f,
                                         match.random().next() * .10471967f / 32768 - .052359834f};
        chunk.remaining = static_cast<int>(life * 60);
        chunk.stage = 1;
        SpellActor::launch_model_trail(match, chunk, material, trail, 1, 1, .3f, .5f, true);
    }
}
int run_mrm_debris(Match&, SpellActor& spell, int ticks) {
    // 0x438590: each landing halves the time left; the last second shrinks the model away.
    if (spell.state_flags & 2) {
        spell.state_flags &= ~2u;
        spell.remaining >>= 1;
    }
    if ((spell.remaining -= ticks) <= 0)
        return remove_spell(spell);
    if (spell.remaining < 60)
        spell.model_scale = spell.remaining * spell.ability.length / 60.f;
    return 1;
}
int run_explosion(Match& match, SpellActor& spell, int ticks) {
    // PyroExplosionRun 0x426910: count - 1 more explosions at random points within the spread, each
    // a child of this one with the same caster (0x44E740), and all wait on the same cast.
    auto& cloud = spell.altar_cloud;
    if (!spell.stage && !spell.rate) {
        spell.rate = 1;
        for (int n = 1; n < spell.data->count; ++n) {
            Vec3 at{spell.point.x + (match.random().next() / 16384.f - 1) * spell.data->spread_limit,
                    spell.point.y + (match.random().next() / 16384.f - 1) * spell.data->spread_limit, spell.point.z};
            if (match.terrain)
                at.z = static_cast<float>(terrain_true_height(*match.terrain, at.x, at.y));
            auto& extra = launch_child(match, spell, SpellActor::Effect::pyro_explosion, nullptr, at);
            extra.point = at;
            extra.rate = 1;
        }
    }
    if (!spell.stage) {
        aim_point(spell);
        cloud.enabled = true;
        // 0x426760: 'ttex' (default 'fbal'), a (1,1,0) light of radius 25. Native material 0x12010808 uses blend
        // mode 0x800, which our renderers do not support; the additive material is kept.
        cloud.texture = FourCC{static_cast<unsigned>(
            spell.data->integer(FourCC::from_text("ttex"), FourCC::from_text("fbal").value))};
        cloud.material = 0x1a012008;
        spell.light = {true, spell.motion.position, {1, 1, 0}, 0, 0, 25};
        if (!match.cloud_noise)
            match.cloud_noise = std::make_shared<CloudNoise>(match.visual_random);
        cloud.noise = match.cloud_noise;
        spell.sound_event(match, 1);
    }
    wait_cast(match, spell);
    // The scale eases to the cast alpha, 1 once released (1/210 per tick), or 0 when aborted (.05).
    const float goal = spell.stage >= 240 ? 0.f : spell.stage == 16 ? 1.f : spell.cast_alpha;
    const float step = ticks * (spell.stage >= 240 ? .05f : .0047619049f);
    spell.age = goal > spell.age ? std::min(goal, spell.age + step) : std::max(goal, spell.age - step);
    spell.light.position = spell.motion.position;
    spell.light.intensity = std::sqrt(spell.age);
    if (spell.stage >= 240 && spell.age <= 0)
        return remove_spell(spell);
    const float radius = .1f + 2 * spell.age;
    cloud.radii = {radius, radius, radius};
    cloud.opacity = std::min(1.f, std::sqrt(spell.age) * 2);
    cloud.phase.x += ticks * 8;
    cloud.phase.y += ticks * 8;
    cloud.phase.z += ticks * 8;
    if (spell.stage == 16 && spell.age >= 1) {
        spell.stop_sound_event(1);
        spell_damage(match, &spell, nullptr, spell.data->damage_type, spell.data->amount, spell.data->radius,
                     spell.motion.position);
        SpellActor::launch_shockwave(match, spell, std::max(spell.data->spread_limit * 1.5f, 7.5f));
        SpellActor::launch_fireball_visual(match, spell, SpellActor::Effect::fireball_blast);
        // 0x438760: eight flaming 'bold' chunks; then the push 2.5 below the centre at 1/3 (0x44D9A0).
        launch_mrm_debris(match, spell, 8, .25f, FourCC::from_text("bold"), .2f, 0x1a012008,
                          FourCC::from_text("firy"), spell.motion.position, 5);
        spell_exclusion_field(match, {spell.motion.position.x, spell.motion.position.y, spell.motion.position.z - 2.5f},
                              10.f, 20.f, 1.f / 3.f, 7, 5);
        return remove_spell(spell);
    }
    return 1;
}
int run_halo(Match& match, SpellActor& spell) {
    // 0x4458d0 stage 0: a wizard caster gets the 0x435EC0 hands and a 0x436DF0 'dirt' spray on the host
    // (box 2, size .4, 1 s); any other caster (the Rhinok's abRH) takes stage 2 and releases all six rocks.
    if (!spell.stage) {
        if (auto* caster = dynamic_cast<CreatureActor*>(spell.owner); caster && !(caster->kind_flags & 1))
            spell.stage = 16;
        else if (caster)
            SpellActor::launch_dirt_spray(match, spell, &spell, FourCC::from_text("dirt"), true, 2, .4f, 1);
    }
    const auto waiting = wait_cast(match, spell, 16, 240);
    if (spell.stage == 1 || spell.stage == 16) {
        if (spell.owner && spell.owner->source_motion())
            spell.motion.position = spell.owner->source_motion()->position;
        const int desired =
            spell.stage == 16 ? 6 : std::clamp(static_cast<int>((spell.cast_alpha + .1f) * 6), 0, 6);
        while (spell.rate < desired) {
            ++spell.rate;
            auto& rock =
                launch_child(match, spell, SpellActor::Effect::halo_rock, nullptr, spell.motion.position);
            if (!rock.owner)
                rock.owner = spell.effect_target;
            rock.run(match, 1); // HaloRockRun 0x445590 via 0x44E850.
        }
    }
    if (waiting)
        return waiting;
    return remove_spell(spell);
}
int run_rift(Match& match, SpellActor& spell, int ticks) {
    // DemonicRiftRun 0x430840. 0x430550 opens the mouth at the caster's feet; demons hunt around the target
    // point.
    if (!spell.stage) {
        aim_point(spell);
        spell.point = spell.motion.position;
        if (const auto* caster = spell.owner ? spell.owner->source_motion() : nullptr)
            spell.motion.position = caster->position;
        if (match.terrain)
            spell.motion.position.z = static_cast<float>(
                terrain_surface(*match.terrain, spell.motion.position.x, spell.motion.position.y));
        spell.ability.origin = spell.motion.position;
        spell.surface.kind = SpellActor::CurvedSurface::ring;
        spell.surface.material = 0x1a010001;
        spell.surface.textures[0] = FourCC{static_cast<unsigned>(
            spell.data->integer(FourCC::from_text("tftx"), FourCC::from_text("fwal").value))};
        const float radius = spell.data->real(FourCC::from_text("frad"), 2.5f),
                    height = spell.data->real(FourCC::from_text("ffht"), 1.5f);
        // A rising 'rif3' haze over the mouth (0x43A5E0 re-run as GenericTrailRun) for the duration.
        const Vec3 top{spell.motion.position.x, spell.motion.position.y, spell.motion.position.z + 1};
        if (auto* haze = SpellActor::launch_generic_moving(match, spell, 0x1a010808, FourCC::from_text("rif3"),
                                                           spell.motion.position, top, 0, height, radius * 1.2f,
                                                           0, 1, .1f, spell.data->duration)) {
            haze->effect = SpellActor::Effect::generic_trail;
            match.assign(*haze, SpellActor::generic_trail_function);
        }
        SpellActor::launch_cast_hands(match, spell);
        // 0x43A720: two 'tdtx' streamers spiralling up the caster while it casts.
        if (auto* caster = dynamic_cast<CreatureActor*>(spell.owner))
            for (int n = 0; n < 2; ++n) {
                auto& spiral = launch_child(match, spell, SpellActor::Effect::cast_spiral, caster,
                                            caster->motion.position);
                spiral.altar_streamer.texture = FourCC{static_cast<unsigned>(
                    spell.data->integer(FourCC::from_text("tdtx"), FourCC::from_text("rift").value))};
                spiral.altar_streamer.material = 0x1a010808;
                spiral.remaining = 30;
                spiral.age = n * 6.28318f / 2;
                spiral.run(match, 0);
            }
        spell.sound_event(match, 1);
        spell.remaining = static_cast<int>(spell.data->duration * 60.f);
        spell.next_pulse = 0;
        spell.stage = 1;
    }
    if (spell.stage == 1) {
        if (const auto next = mid_cast(spell, 16, 240))
            spell.stage = static_cast<int>(next);
    }
    if (spell.stage == 16) {
        if ((spell.next_pulse -= ticks) <= 0) {
            spell.next_pulse = 30;
            // 0x4306D0: a demon for a unit within 1.2 x spread of the target point (named for the spell), or a
            // one-in-four demon to a random spot .5..1.2 x spread out, 2..10 above the ground.
            const float spread = spell.data->spread_limit;
            if (auto* victim = nearest_unit(match, spell.point, spread * 1.2f,
                                            (spell.owner ? spell.owner : &spell), true, &spell)) {
                mark_hit(*victim, spell);
                auto& demon =
                    launch_child(match, spell, SpellActor::Effect::rift_demon, victim, spell.motion.position);
                demon.run(match, 1);
            } else if (!(match.random().next() & 3)) {
                const float angle = 2 * match.random().next() * .0000958737f,
                            distance = match.random().next() * (spread * .5f - spread * 1.2f) * .000030517578125f +
                                       spread * 1.2f;
                Vec3 spot{spell.point.x + std::sin(angle) * distance, spell.point.y + std::cos(angle) * distance,
                          spell.point.z};
                if (match.terrain)
                    spot.z = static_cast<float>(terrain_surface(*match.terrain, spot.x, spot.y));
                spot.z += match.random().next() * 8.f * .000030517578125f + 2;
                auto& demon =
                    launch_child(match, spell, SpellActor::Effect::rift_demon, nullptr, spell.motion.position);
                demon.point = spot;
                demon.run(match, 1);
            }
        }
        spell.remaining -= ticks;
        if (spell.remaining <= 0) {
            spell.stage = 33;
            spell.remaining = 480; // Keep the mouth open while the last demons return.
        }
    } else if (spell.stage == 33 && (spell.remaining -= ticks) <= 0)
        spell.stage = 240;
    if ((match.random().next() & 63) <= ticks)
        spell.sound_event(match, 8);
    spell.motion.position = spell.ability.origin;
    if (match.terrain)
        spell.motion.position.z = static_cast<float>(
            terrain_surface(*match.terrain, spell.motion.position.x, spell.motion.position.y));
    const float goal = spell.stage >= 240 ? 0.f : spell.stage == 1 ? spell.cast_alpha : 1.f;
    spell.age += std::clamp(goal - spell.age, -ticks / 60.f, ticks / 60.f);
    auto& surface = spell.surface;
    const float radius = spell.data->real(FourCC::from_text("frad"), 2.5f) * spell.age,
                height = spell.data->real(FourCC::from_text("ffht"), 1.5f);
    surface.parameters = {0, .02f, 255, radius * .5f, height, 255, radius, height, 255, radius, .02f, 0};
    surface.opacity = spell.age;
    surface.frame += ticks * .25f;
    return spell.stage >= 240 && spell.age == 0 ? remove_spell(spell) : 1;
}
} // namespace
// Shared with Bovine Intervention (0x4437A0 stage 33 runs the same 0x440ED0(80, 20, 3) wave and 0x438760 chunks).
void spell_erupt_wave(Match& match, SpellActor& spell, float progress) {
    erupt_wave(match, spell, progress);
}
void spell_mrm_debris(Match& match, SpellActor& source, int count, float speed, FourCC model, float scale,
                      std::uint32_t material, FourCC trail, Vec3 at, float life) {
    launch_mrm_debris(match, source, count, speed, model, scale, material, trail, at, life);
}
int SpellActor::run_midgame(Match& match, int ticks) {
    if (!data)
        throw Error("Midgame god spell requires its SPEL definition");
    switch (effect) {
    case Effect::rainbow:
        return run_rainbow(match, *this, ticks);
    case Effect::dragonfire:
        return run_dragonfire(match, *this, ticks);
    case Effect::erupt:
        return run_erupt(match, *this, ticks);
    case Effect::chain_lightning:
        return run_chain_lightning(match, *this, ticks);
    case Effect::animate_dead:
        return run_animate_dead(match, *this, ticks);
    case Effect::rain_frogs:
        return run_rain_frogs(match, *this, ticks);
    case Effect::pyro_explosion:
        return run_explosion(match, *this, ticks);
    case Effect::halo_of_earth:
        return run_halo(match, *this);
    case Effect::spirit_wind:
        return run_soul_path(match, ticks);
    case Effect::demonic_rift:
        return run_rift(match, *this, ticks);
    case Effect::rain_frog:
        return run_rain_frog(match, *this, ticks);
    case Effect::halo_rock:
        return run_halo_rock(match, *this, ticks);
    case Effect::mrm_debris:
        return run_mrm_debris(match, *this, ticks);
    case Effect::rift_demon:
        return run_rift_demon(match, *this, ticks);
    case Effect::cast_spiral:
        return run_cast_spiral(match, *this, ticks);
    case Effect::animate_dead_streamer:
        return run_animate_streamer(match, *this, ticks);
    default:
        throw Error("Invalid midgame god spell");
    }
}
} // namespace sacrifice
