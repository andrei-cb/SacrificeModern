#include "sacrifice/creature_actor.hpp"
#include "sacrifice/navigation.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include "sacrifice/spell_data.hpp"
#include "sacrifice/spell_registry.hpp"
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
FourCC hop_name(const SpellActor& spell) {
    if (spell.spell_id.value)
        return spell.spell_id;
    switch (spell.effect) {
    case SpellActor::Effect::healing_aura:
        return FourCC::from_text("haur");
    case SpellActor::Effect::fire_wall:
        return FourCC::from_text("frwl");
    case SpellActor::Effect::spike_wall:
        return FourCC::from_text("swal");
    case SpellActor::Effect::frozen_ground:
        return FourCC::from_text("froz");
    case SpellActor::Effect::wailing_wall:
        return FourCC::from_text("wwal");
    case SpellActor::Effect::vine_wall:
        return FourCC::from_text("vwal");
    case SpellActor::Effect::rain_of_fire:
        return FourCC::from_text("rfir");
    case SpellActor::Effect::fence:
        return FourCC::from_text("fenc");
    case SpellActor::Effect::plague:
        return FourCC::from_text("plag");
    case SpellActor::Effect::meteor_shower:
        return FourCC::from_text("metr");
    default:
        return FourCC::from_text("sieg");
    }
}
void mark_hit(CreatureActor& actor, SpellActor& spell) {
    actor.name_reference(hop_name(spell), &spell);
}
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
    child.data = source.data;
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
Vec3 follow_owner(SpellActor& spell) {
    if (spell.effect_target && (spell.effect_target->kind_flags & 5) && spell.effect_target->source_motion())
        spell.motion.position = spell.effect_target->source_motion()->position;
    else if (spell.owner && spell.owner->source_motion())
        spell.motion.position = spell.owner->source_motion()->position;
    return spell.motion.position;
}
Vec3 wall_axis(const SpellActor& spell) {
    if (spell.surface.kind == SpellActor::CurvedSurface::vines ||
        spell.surface.kind == SpellActor::CurvedSurface::spikes ||
        spell.surface.kind == SpellActor::CurvedSurface::fire_wall)
        return spell.ability.destination;
    Vec3 from = spell.owner && spell.owner->source_motion() ? spell.owner->source_motion()->position
                                                            : spell.motion.position;
    const Vec3 to = spell.motion.position;
    Vec3 delta{to.x - from.x, to.y - from.y, 0};
    const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    if (length < .01f)
        return {1.f, 0.f, 0.f};
    return {delta.y / length, -delta.x / length, 0.f};
}
float segment_distance(Vec3 point, Vec3 a, Vec3 b) {
    const Vec3 ab{b.x - a.x, b.y - a.y, b.z - a.z};
    const Vec3 ap{point.x - a.x, point.y - a.y, point.z - a.z};
    const float ab2 = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
    const float t = ab2 > 0.f ? std::clamp((ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / ab2, 0.f, 1.f) : 0.f;
    const Vec3 closest{a.x + ab.x * t, a.y + ab.y * t, a.z + ab.z * t};
    return static_cast<float>(std::sqrt(distance_squared(point, closest)));
}
template <typename Fn> void visit_units(Match& match, MatchObject* source, bool foes, Fn&& fn) {
    const auto visit = [&](MatchObject& object) {
        auto* actor = living_unit(&object);
        if (!actor || actor == source)
            return 0;
        if (foes ? !actor->enemy(source) : !actor->friendly(source))
            return 0;
        fn(*actor);
        return 0;
    };
    match.visit_group(0, 5, visit);
    match.visit_group(1, 5, visit);
}
int run_wall_child(Match& match, SpellActor& spell, int ticks) {
    const bool demon = spell.effect == SpellActor::Effect::wailing_demon;
    if (demon) {
        // WailingWallDemon 0x433760: a streamer on a Catmull-Rom path (0x44C820 / 0x44C660, basis 0x4CAF40)
        // through random points of a box, dropping the oldest point as each segment ends. Around the wall
        // (0x4336D0: its length +-10, width +-(fwid + 5), 0..fhgt + 5 over the ground) a spirit drifts a
        // segment every two seconds; on a unit it circles inside the unit's box a segment every half second
        // for eight seconds (sound 4 as it starts), then lets the name go. Neither deals damage.
        auto* wall = dynamic_cast<SpellActor*>(spell.effect_target);
        const bool ambient = wall && wall->effect == SpellActor::Effect::wailing_wall;
        auto* victim = ambient ? nullptr : living_unit(spell.effect_target);
        auto& streamer = spell.altar_streamer;
        auto& controls = streamer.controls;
        const auto random_point = [&] {
            const auto pick = [&](float low, float high) {
                return low + match.visual_random.next() * (high - low) / 32768;
            };
            if (ambient) {
                const float width = wall->data->real(FourCC::from_text("fwid"), 10),
                            height = wall->data->real(FourCC::from_text("fhgt"), 7.5f);
                const float along = pick(wall->ability.draw_back - 10, wall->ability.length + 10),
                            across = pick(-(width + 5), width + 5), up = pick(0, height + 5);
                const auto& axis = wall->ability.destination;
                return Vec3{axis.x * along - axis.y * across, axis.y * along + axis.x * across, up};
            }
            const auto& m = victim->motion;
            return Vec3{pick(m.box_min.x, m.box_max.x), pick(m.box_min.y, m.box_max.y),
                        pick(m.box_min.z, m.box_max.z)};
        };
        if ((ambient && (wall->state_flags & MatchObject::removed_flag)) || (!ambient && !victim))
            spell.stage = 240;
        if (!spell.stage) {
            streamer.enabled = true;
            streamer.texture = FourCC{static_cast<unsigned>(
                spell.data->integer(FourCC::from_text("tdmn"), FourCC::from_text("rift").value))};
            streamer.material = 0x1a010808;
            streamer.primitive.configure(ambient ? 45 : 60, ambient ? .5f : .3f);
            for (auto& control : controls)
                control = random_point();
            if (!ambient) {
                mark_hit(*victim, spell); // 0x4692A0 with SPEL+16, held for 480 ticks.
                spell.remaining = 480;
            }
            spell.age = 0;
            spell.stage = ambient ? 1 : 16;
        }
        if (spell.stage == 16) {
            spell.sound_event(match, 4);
            spell.stage = 17;
        }
        if (spell.stage == 1 || spell.stage == 17) {
            spell.age += ticks * (ambient ? .0083333338f : .033333335f);
            for (; spell.age >= 1; spell.age -= 1) {
                controls[0] = controls[1];
                controls[1] = controls[2];
                controls[2] = controls[3];
                controls[3] = random_point();
            }
            const float t = spell.age, t2 = t * t, t3 = t2 * t;
            const auto spline = [&](auto member) {
                const float p0 = controls[0].*member, p1 = controls[1].*member, p2 = controls[2].*member,
                            p3 = controls[3].*member;
                return t3 * (-.5f * p0 + 1.5f * p1 - 1.5f * p2 + .5f * p3) +
                       t2 * (p0 - 2.5f * p1 + 2 * p2 - .5f * p3) + t * (-.5f * p0 + .5f * p2) + p1;
            };
            const Vec3 offset{spline(&Vec3::x), spline(&Vec3::y), spline(&Vec3::z)};
            const Vec3 anchor = ambient ? wall->motion.position : victim->motion.position;
            spell.motion.position = {anchor.x + offset.x, anchor.y + offset.y, anchor.z + offset.z};
            if (ambient && match.terrain)
                spell.motion.position.z =
                    static_cast<float>(terrain_surface(*match.terrain, spell.motion.position.x,
                                                       spell.motion.position.y)) +
                    offset.z;
            if (!ambient && (spell.remaining -= ticks) < 0) {
                victim->unname(hop_name(spell), &spell);
                spell.effect_target = nullptr;
            }
        }
        if (spell.stage == 240) {
            if (auto* held = dynamic_cast<CreatureActor*>(spell.effect_target); held && !ambient)
                held->unname(hop_name(spell), &spell);
        } else if (match.visual_random.next() < 2 * 546.1333333f) // 0x435BC0(2): sound 8.
            spell.sound_event(match, 8);
        streamer.ticks += ticks;
        const unsigned frames = match.texture_frames ? match.texture_frames(streamer.texture) : 1;
        return streamer.primitive.add_point(spell.stage == 240 ? nullptr : &spell.motion.position, streamer.ticks,
                                            frames)
                   ? 1
                   : remove_spell(spell);
    }
    auto& p = spell.particles(match);
    // 0x427D80 bits wander along the fire wall; 0x427FB0 is the fire wall's earth effect, which runs the
    // vine wall's per-end emitter 0x42E990 with 'firy' preset 128 {1.25, 3, 1}.
    const bool fire = spell.effect == SpellActor::Effect::fire_wall_bit;
    const bool fire_earth = spell.effect == SpellActor::Effect::fire_wall_earth;
    if (!spell.stage) {
        if (fire) {
            spell.particle_material(match, p, 0x1a012008,
                                    FourCC{static_cast<unsigned>(spell.data->integer(
                                        FourCC::from_text("tptx"), FourCC::from_text("firy").value))});
            const float parameters[]{1, 5, .8f};
            p.set_property(128, parameters, parameters);
            const float low[]{-3, -3, -1}, high[]{3, 3, 1};
            p.set_property(2, low, high);
        } else if (fire_earth) {
            spell.particle_material(match, p, 0x1a012008, FourCC::from_text("firy"));
            const float parameters[]{1.25f, 3, 1};
            p.set_property(128, parameters, parameters);
        } else {
            // 0x42E870: 'tdrt' (default 'dirt'), preset 129 {1.25, 3}.
            spell.particle_material(match, p, 0x0a011008,
                                    FourCC{static_cast<unsigned>(spell.data->integer(
                                        FourCC::from_text("tdrt"), FourCC::from_text("dirt").value))});
            const float size = 1.25f, life = 3;
            p.set_property(129, &size, &life);
        }
        spell.stage = 1;
    }
    if (!fire) {
        // 0x42E990: one particle per call at each growing end, +-.25 along and +-.75 fwid across, over land;
        // the effect retires once nothing is left alive.
        if (auto* wall = dynamic_cast<SpellActor*>(spell.effect_target);
            wall && !(wall->state_flags & MatchObject::removed_flag)) {
            const Vec3 axis = wall_axis(*wall);
            const float across_limit =
                wall->data->real(FourCC::from_text("fwid"), wall->effect == SpellActor::Effect::fire_wall ? 10.f : 5.f) *
                .75f;
            for (unsigned bit : {1u, 2u}) {
                if (!(wall->ability.clock & bit))
                    continue;
                const float end = bit == 1 ? wall->ability.draw_back : wall->ability.length;
                const float along = end + match.visual_random.next() * .5f / 32768 - .25f,
                            across = match.visual_random.next() * 2 * across_limit / 32768 - across_limit;
                Vec3 at{wall->motion.position.x + axis.x * along - axis.y * across,
                        wall->motion.position.y + axis.y * along + axis.x * across, 0};
                at.z = match.terrain ? static_cast<float>(terrain_surface(*match.terrain, at.x, at.y)) : 1;
                if (at.z > 0)
                    p.set_position(at), p.emit(0, 1);
            }
        }
        spell.age += ticks;
        p.advance(spell.age, 0);
        return p.alive() ? 1 : remove_spell(spell);
    }
    if (!spell.effect_target || (spell.effect_target->state_flags & MatchObject::removed_flag))
        spell.stage = 240;
    float emission = 0;
    if (auto* parent = dynamic_cast<SpellActor*>(spell.effect_target); parent && spell.stage != 240) {
        // A random point between the ends, .75 of the wall's height over land; the rate follows the
        // wall's share of its spread, at most .5.
        const Vec3 axis = wall_axis(*parent);
        const float low = parent->ability.draw_back, high = parent->ability.length;
        const float at = low + match.visual_random.next() * (high - low) / 32768;
        spell.motion.position = {parent->motion.position.x + axis.x * at,
                                 parent->motion.position.y + axis.y * at, parent->motion.position.z};
        const float ground = match.terrain ? static_cast<float>(terrain_surface(
                                                 *match.terrain, spell.motion.position.x, spell.motion.position.y))
                                           : parent->motion.position.z;
        spell.motion.position.z = ground;
        if (!match.terrain || ground > 0) {
            spell.motion.position.z += parent->surface.parameters[4] * .75f;
            emission = std::min(1.f, (high - low) * parent->surface.opacity /
                                         std::max(parent->data->spread_limit, .001f)) *
                       .5f;
        }
    }
    spell.age += ticks;
    p.set_position(spell.motion.position);
    p.advance(spell.age, emission);
    return spell.stage == 240 && !p.alive() ? remove_spell(spell) : 1;
}
bool fly_drop(Match& match, SpellActor& spell, int ticks) {
    // 0x428e40 / 0x428d80: fall from above until collision, z, or 600 ticks. Tests without a
    // spatial host skip the flight and resolve on the first run.
    if (!spell.stage) {
        if (!match.terrain || !match.spatial || !spell.data) {
            if (spell.effect == SpellActor::Effect::rain_fire_drop)
                spell.motion.position = spell.point;
            spell.stage = 16;
            return false;
        }
        const bool fire = spell.effect == SpellActor::Effect::rain_fire_drop;
        if (fire) {
            // 0x428e40 falls from the cloud's scattered origin toward its independent ground point.
            // It does not retarget a creature or acquire GenericShoot's friendly-ignore timer.
            spell.launch_projectile(match);
            spell.friendly_ignore_time = 0;
            spell.motion.collision_mask &= ~0x20000u;
            const Vec3 delta{spell.point.x - spell.motion.position.x, spell.point.y - spell.motion.position.y,
                             spell.point.z - spell.motion.position.z};
            const auto length = std::sqrt(distance_squared(spell.point, spell.motion.position));
            const auto scale = length > 0 ? 1 / length : 0; // 0x428e40: a unit direction, one per tick.
            spell.motion.velocity = {static_cast<float>(delta.x * scale), static_cast<float>(delta.y * scale),
                                     static_cast<float>(delta.z * scale)};
            // 0x437C10(1, 2, 2, .75): preset 129 size 2, life .75, a +-1 box, emission 2.
            SpellActor::launch_box_trail(match, spell, 0x1a012008, FourCC::from_text("firy"), 1, 2, .75f, 2);
        } else {
            spell.point = spell.motion.position;
            spell.motion.position.z += 40;
            spell.launch_projectile(match);
            spell.motion.velocity = {0, 0, -std::max(spell.data->speed, .1f)};
        }
        spell.remaining = 600;
        spell.generic.initialized = true;
        spell.generic.collision_radius = .25f;
        spell.motion.flags = 1;
        // 0x428e40: the fire drop is an 'rnfr' SPRITE 5 wide (SetParams spans +-size / 2); our quad size is
        // the half-extent.
        spell.sprite = {true,
                        FourCC::from_text(spell.effect == SpellActor::Effect::plague_bomb ? "ppus" : "rnfr"),
                        0x1a012008, 0xffffffff, fire ? 2.5f : 1.f};
        spell.stage = 1;
        return true;
    }
    if (spell.stage != 1)
        return false;
    spell.remaining -= ticks;
    spell.sprite.frame += ticks * .5f;
    if (spell.remaining <= 0) {
        spell.state_flags |= MatchObject::removed_flag;
        return true;
    }
    if (spell.state_flags & 3) {
        spell.capture_collision(match);
        spell.stage = 16;
        return false;
    }
    return true;
}
int run_drop(Match& match, SpellActor& spell, int ticks, bool splash) {
    if (fly_drop(match, spell, ticks))
        return 1;
    strike(match, spell, spell.impact_target, spell.motion.position, splash);
    if (spell.effect == SpellActor::Effect::rain_fire_drop) {
        // 0x426E50: fireball blast, 0x439F10 shockwave of max(radius, 7.5), then the area damage.
        spell.sound_event(match, 4);
        SpellActor::launch_fireball_visual(match, spell, SpellActor::Effect::fireball_blast);
        SpellActor::launch_shockwave(match, spell, std::max(spell.data->radius, 7.5f));
    }
    return remove_spell(spell);
}
int run_plague_bomb(Match& match, SpellActor& spell, int ticks) {
    // PlagueBombRun 0x432c10: fall, then 0x432BB0 plus a radius visit.
    auto& p = spell.particles(match);
    if (!spell.stage) {
        spell.launch_projectile(match);
        spell.motion.flags = 1;
        spell.motion.velocity = {0, 0, -spell.data->speed};
        spell.remaining = 600;
        spell.particle_material(match, p, 0x0a011008, "blud"); // 0x432A70: dword_4C7890.
        const float size = 1, life = 1.5f;
        p.set_property(129, &size, &life);
        const float low[]{-.01f, -.01f, -spell.data->speed * .5f - .01f},
            high[]{.01f, .01f, -spell.data->speed * .5f + .01f};
        p.set_property(3, low, high);
        spell.stage = 1;
    }
    if (spell.stage == 1) {
        if (spell.state_flags & 3) {
            // Stage 16: sound 4, a 'pois' splash (0x439830 +-(5, 5, 2.5), 3 s, half of it draining), and
            // 0x432BB0 on the struck unit and on every unit, of either side, within the radius: poison for
            // 'flif' (10) seconds at three quarters of the amount.
            spell.capture_collision(match);
            spell.sound_event(match, 4);
            SpellActor::launch_dust_splatter(match, spell, 0x0a011008, FourCC::from_text("pois"),
                                             spell.motion.position, {5, 5, 2.5f}, 3, .5f);
            const auto& d = *spell.data;
            const auto infect = [&](CreatureActor& actor) {
                SpellActor::poison(match, actor, spell.owner, d.real(FourCC::from_text("flif"), 10),
                                   static_cast<int>(static_cast<std::int64_t>(d.amount * .75)), d.damage_type,
                                   d.integer(FourCC::from_text("ires"), 0), d.real(FourCC::from_text("freg"), 1),
                                   d.integer(FourCC::from_text("iacc"), 0), d.spread_limit);
            };
            if (auto* victim = living_unit(spell.impact_target))
                infect(*victim);
            for (unsigned group : {0u, 1u})
                match.visit_group(group, 5, [&](MatchObject& object) {
                    if (auto* actor = living_unit(&object);
                        actor && distance_squared(spell.motion.position, actor->motion.position) <=
                                     static_cast<double>(d.radius) * d.radius)
                        infect(*actor);
                    return 0;
                });
            spell.stage = 240;
        } else if (spell.motion.position.z <= 0 || (spell.remaining -= ticks) <= 0)
            spell.stage = 240;
    }
    if (spell.stage >= 240) {
        spell.motion.flags = 0;
        spell.motion.velocity = {};
    }
    p.set_position(spell.motion.position);
    spell.host.time += ticks;
    p.advance(spell.host.time, spell.stage == 1 ? 1.f : 0.f);
    return spell.stage >= 240 && !p.alive() ? remove_spell(spell) : 1;
}
int run_meteor(Match& match, SpellActor& spell, int ticks) {
    // MeteorRun 0x445d80: fall, then 0x44D0B0 / 0x453260, 0x439270, 0x44D7E0.
    if (!spell.stage) {
        if (match.terrain)
            spell.motion.position.z = static_cast<float>(terrain_surface(
                                          *match.terrain, spell.motion.position.x, spell.motion.position.y)) -
                                      3;
        // 0x445B40: a 'rock' of scale .8..1.2 thrown from 3 below the ground at .8..1.2 x the speed,
        // tumbling at random, with the 0x4380F0 ground effects, the 0x4380A0 'tRT1' trail and sound 2.
        spell.set_projectile_model(match, FourCC::from_text("rock"),
                                   .8f + match.random().next() * (.4f / 32768));
        spell.launch_projectile(match);
        spell.motion.velocity =
            spell_ballistic_velocity(spell.motion.position, spell.point,
                                     spell.data->speed * (.8f + match.random().next() * (.4f / 32768)), 6);
        const auto spin = [&] { return static_cast<float>((match.random().next() - 0x4000) * .000001597086588541667); };
        spell.motion.angular_velocity.x = spin();
        spell.motion.angular_velocity.y = spin();
        spell.motion.angular_velocity.z = spin();
        spell.motion.flags = 61;
        spell.motion.collision_mask = 0;
        spell.remaining = 1200;
        spell.next_pulse = 60;
        spell.stage = 1;
        Vec3 ground = spell.motion.position;
        ground.z += 3;
        match.shake(ground, 16, 50);
        SpellActor::launch_earth_burst(match, spell, ground, 3, 24);
        match.scar_requests.push_back({FourCC{static_cast<unsigned>(spell.data->integer(
                                           FourCC::from_text("tRE3"), FourCC::from_text("tstr").value))},
                                       ground, match.clock().match, ++match.terrain_revision});
        SpellActor::launch_model_trail(match, spell, 0x0a011008,
                                       FourCC{static_cast<unsigned>(spell.data->integer(
                                           FourCC::from_text("tRT1"), FourCC::from_text("dust").value))},
                                       1, 1);
        spell.sound_event(match, 2);
    }
    if (spell.stage == 1) {
        if ((spell.next_pulse -= ticks) <= 0) {
            spell.motion.collision_mask = 0x114015;
            spell.stage = 16;
        }
        return 1;
    }
    // Stage 16: a meteor that sinks to z <= 0 or outlives 1200 ticks just disappears.
    if (!(spell.state_flags & 3))
        return spell.motion.position.z <= 0 || (spell.remaining -= ticks) <= 0 ? remove_spell(spell) : 1;
    spell.capture_collision(match);
    if (!spell.impact_target) {
        // Ground hits only: 0x44D0B0(15, 1) dent, a 0x453260(8, 30) shake and the 0x44EAF0 ring that knocks
        // down units 0..20 out with a 1 in 4 chance.
        match.dent(spell.motion.position, 15.f, 1.f);
        match.shake(spell.motion.position, 8, 30);
        for (const unsigned group : {0u, 1u})
            match.visit_group(group, 5, [&](MatchObject& object) {
                auto* actor = living_unit(&object);
                if (actor && (actor->state_flags & 2) &&
                    std::abs(10 - std::hypot(actor->motion.position.x - spell.motion.position.x,
                                             actor->motion.position.y - spell.motion.position.y)) < 10 &&
                    match.random().next() < 8192)
                    actor->knock_down(0, 10.f);
                return 0;
            });
    }
    SpellActor::launch_rock_shatter(match, spell);
    strike(match, spell, spell.impact_target, spell.motion.position, true);
    spell.sound_event(match, 4);
    return remove_spell(spell);
}
// VINES::RunVines 0x16015f80 (sub_16015C70) and SPIKES::RunSpikes 0x16016ef0 re-read
// LAND::GetTrueHeight at each root every update and lift the root and tip by the change, so the wall
// rides later terrain deformation (Volcano, Earthquake scars).
void follow_terrain(Match& match, SpellActor& spell) {
    if (!match.terrain)
        return;
    for (auto& vine : spell.surface.vines_data) {
        const float delta =
            static_cast<float>(terrain_surface(*match.terrain, vine.curve[0].x, vine.curve[0].y)) - vine.curve[0].z;
        for (auto& point : vine.curve)
            point.z += delta;
    }
}
int run_spike_hit(Match& match, SpellActor& spell, int ticks) {
    // SpikeWallHitNTT 0x4477a0. Stage 0 (0x447600): at the victim's ground point a 'tspi' (default 'spik')
    // spike of length 5 pops up at pi / 60 a tick, with a 0x436DF0 'dirt' spray (box 3, size .5, 1 s) and a
    // 'tscr' (default 'quk1') scar; the victim is named, knocked down (0x4591A0(1, 10)) and damaged
    // (0x44D7E0). For 300 ticks after, a living victim's speed target is halved every tick.
    auto* target = dynamic_cast<CreatureActor*>(spell.effect_target);
    if (!spell.stage) {
        if (!target)
            return remove_spell(spell);
        spell.remaining = 300;
        spell.stage = 1;
        spell.surface.kind = SpellActor::CurvedSurface::spikes;
        spell.surface.material = 0x00010001;
        spell.surface.textures[0] = FourCC{static_cast<unsigned>(
            spell.data->integer(FourCC::from_text("ttex"), FourCC::from_text("spik").value))};
        spell.surface.opacity = 1;
        spell.sound_event(match, 4);
        spell.motion.position = target->motion.position;
        if (match.terrain)
            spell.motion.position.z = static_cast<float>(
                terrain_surface(*match.terrain, spell.motion.position.x, spell.motion.position.y));
        if (spell.motion.position.z > 0) {
            SpellActor::launch_dirt_spray(match, spell, &spell, FourCC::from_text("dirt"), true, 3, .5f, 1);
            const auto& root = spell.motion.position;
            spell.add_vine(root, {root.x, root.y, root.z + 5}, .5f);
            spell.surface.vines_data.back().speed = 1.f / 30;
            match.scar_requests.push_back({FourCC{static_cast<unsigned>(spell.data->integer(
                                               FourCC::from_text("tscr"), FourCC::from_text("quk1").value))},
                                           root, match.clock().match, ++match.terrain_revision});
        }
        mark_hit(*target, spell);
        if (target->alive())
            target->knock_down(1, 10);
        strike(match, spell, target, target->motion.position, false);
    }
    if (target && target->alive() && !(target->state_flags & MatchObject::removed_flag)) {
        target->stats.changed |= 8;
        target->stats.target[3] = static_cast<std::int16_t>(std::max(0.f, target->stats.target[3] * .5f));
    }
    spell.remaining -= ticks;
    follow_terrain(match, spell);
    spell.advance_vines(ticks, spell.remaining < 30 ? 0.f : 1.f);
    if (spell.remaining < 0)
        return remove_spell(spell);
    return 1;
}
// 0x427BE0: an end stops over non-positive ground or within half the width of a building box.
bool wall_end_blocked(Match& match, const SpellActor& spell, float at, float width) {
    const auto& axis = spell.ability.destination;
    const Vec3 point{spell.motion.position.x + axis.x * at, spell.motion.position.y + axis.y * at, 0};
    if (match.terrain && terrain_surface(*match.terrain, point.x, point.y) <= 0)
        return true;
    bool hit = false;
    const float half_width = width * .5f;
    match.visit_group(2, MatchObject::building, [&](MatchObject& object) {
        if (const auto* motion = object.source_motion();
            !hit && motion &&
            spell_box_distance_squared({point.x, point.y, motion->position.z}, *motion, 6553600) <=
                static_cast<double>(half_width) * half_width)
            hit = true;
        return 0;
    });
    return hit;
}
// 0x427CD0: each growing end (ability.clock bits 1 and 2: draw_back below 0, length above) moves `step`
// out to the spread. Clamping and a block each toggle the end's bit, so an end clamped onto a blocked point
// keeps "growing" as native does.
void grow_wall_ends(Match& match, SpellActor& spell, float step, float width) {
    auto& mask = spell.ability.clock;
    const float spread = spell.data->spread_limit;
    if (mask & 1) {
        spell.ability.draw_back -= step;
        if (spell.ability.draw_back < -spread)
            mask ^= 1, spell.ability.draw_back = -spread;
        if (wall_end_blocked(match, spell, spell.ability.draw_back, width))
            mask ^= 1;
    }
    if (mask & 2) {
        spell.ability.length += step;
        if (spell.ability.length > spread)
            mask ^= 2, spell.ability.length = spread;
        if (wall_end_blocked(match, spell, spell.ability.length, width))
            mask ^= 2;
    }
}
int run_fence_post(Match& match, SpellActor& spell, int ticks) {
    // 0x420d60 / 0x420ff0: two growing, bobbing SPRITE layers, with occasional discharges.
    if (!spell.stage) {
        if (spell.remaining <= 0)
            spell.remaining = spell.data ? static_cast<int>(spell.data->duration * 60.f) : 1800;
        spell.sprite = {true,
                        FourCC{static_cast<unsigned>(
                            spell.data->integer(FourCC::from_text("tisp"), FourCC::from_text("fbal").value))},
                        0x0a011008};
        spell.sprite_overlay = {true,
                                FourCC{static_cast<unsigned>(spell.data->integer(
                                    FourCC::from_text("tosp"), FourCC::from_text("cst0").value))},
                                0x1a012008};
        spell.sprite.size = spell.sprite_overlay.size = 0;
        spell.name_reference(FourCC::from_text("fenc"), &spell);
        if (match.terrain)
            spell.motion.position.z = static_cast<float>(terrain_surface(
                                          *match.terrain, spell.motion.position.x, spell.motion.position.y)) +
                                      3;
        // Four short birth arcs from random points 10 away (below the post flattened to .3), the first
        // with 'Xltn', then sound 8.
        for (int i = 0; i < 4; ++i) {
            const float yaw = match.random().next() * 6.2831802f / 32768,
                        pitch = match.random().next() * 6.2831802f / 32768;
            const auto o = motion_orientation(yaw, pitch);
            float up = o[5];
            if (up < 0)
                up *= .3f;
            const Vec3 from{o[3] * 10 + spell.motion.position.x, o[4] * 10 + spell.motion.position.y,
                            up * 10 + spell.motion.position.z};
            auto& arc = SpellActor::launch_inert_lightning(match, spell, from, spell.motion.position, nullptr,
                                                           1.5f, .25f, 0, 2, .5f, 1);
            if (!i)
                arc.sound_requests.push_back({FourCC::from_text("Xltn"), FourCC::from_text("Xltn"),
                                              arc.reference(), 0x40004});
        }
        spell.sound_event(match, 8);
        spell.stage = 1;
        if (match.navigation)
            match.navigation->dirty_static();
    }
    spell.remaining -= ticks;
    if (spell.remaining > 0 && match.navigation) {
        // 0x4702F0 shape 3: an AI cost disc of 64 to 10, fading out to 25.
        constexpr std::array footprint{10.f, 25.f};
        match.navigation->stamp(3, spell.motion.position, footprint, 64);
    }
    spell.cast_alpha +=
        std::clamp((spell.remaining > 0 ? 1.f : 0.f) - spell.cast_alpha, -ticks / 60.f, ticks / 60.f);
    spell.age += ticks * .026179917f;
    if (match.terrain)
        spell.motion.position.z = static_cast<float>(terrain_surface(*match.terrain, spell.motion.position.x,
                                                                     spell.motion.position.y)) +
                                  3 + std::sin(spell.age) * spell.cast_alpha * 1.5f;
    // SPRITE parameters are full dimensions; ParticleQuad uses half extents.
    spell.sprite.size = spell.cast_alpha * .75f;
    spell.sprite_overlay.size = spell.cast_alpha * 1.5f;
    spell.sprite.frame += ticks * .25f;
    spell.sprite_overlay.frame += ticks * .25f;
    if (spell.remaining > 0 && spell.cast_alpha == 1 && match.random().next() % 90 <= ticks) {
        std::vector<CreatureActor*> candidates;
        for (unsigned group : {0u, 1u})
            match.visit_group(group, 5, [&](MatchObject& object) {
                if (auto* actor = living_unit(&object);
                    actor && candidates.size() < 64 &&
                    spell_box_distance_squared(spell.motion.position, actor->motion, 100) <= 100)
                    candidates.push_back(actor);
                return 0;
            });
        if (!candidates.empty()) {
            auto* victim = candidates[match.random().next() % candidates.size()];
            auto& bolt =
                launch_child(match, spell, SpellActor::Effect::lightning_bolt, victim, spell.motion.position);
            spell_target_point(bolt.point, victim);
            bolt.run(match, 0);
        } else {
            SpellActor* neighbour = nullptr;
            for (const auto& object : match.objects())
                if (auto* post = dynamic_cast<SpellActor*>(object.get());
                    post && post != &spell && post->effect == SpellActor::Effect::fence_post &&
                    !(post->state_flags & MatchObject::removed_flag) &&
                    distance_squared(post->motion.position, spell.motion.position) <= 225 &&
                    (match.random().next() & 7) == 0)
                    neighbour = post;
            if (neighbour) {
                // 0x437370: a quarter-second lit arc to the neighbouring post.
                spell.sound_event(match, 2);
                SpellActor::launch_inert_lightning(match, spell, spell.motion.position,
                                                   neighbour->motion.position, nullptr, 1.5f, .25f, 0, 2, .5f,
                                                   1);
            }
        }
    }
    if (spell.remaining <= 0 && spell.cast_alpha == 0) {
        if (match.navigation)
            match.navigation->dirty_static();
        return remove_spell(spell);
    }
    return 1;
}
void frozen_danger(Match& match, const SpellActor& spell, Vec3 at) {
    // 0x4702F0 shape 3: the AI cost disc (32 inside .75 x radius, fading out to 1.5 x radius), every tick.
    if (match.navigation) {
        const std::array footprint{spell.data->radius * .75f, spell.data->radius * 1.5f};
        match.navigation->stamp(3, at, footprint, 32);
    }
}
int run_frozen_field(Match& match, SpellActor& spell, int ticks) {
    // FrozenGroundRun 0x4225F0. The GROUNDHUGGER (0x2011008 'ttex', inner p[1], outer p[0] = sqrt(growth) x
    // spread) opens at .05 per tick and freezes grounded units it reaches; after duration / 2 seconds it
    // shatters from the centre out, 2.5 every 5 ticks.
    auto& p = spell.surface.parameters;
    if (!spell.stage) {
        spell.remaining = static_cast<int>(spell.data->duration * 30);
        spell.cast_alpha = 0;
        spell.surface.kind = SpellActor::CurvedSurface::ground;
        spell.surface.textures[0] = FourCC{static_cast<unsigned>(
            spell.data->integer(FourCC::from_text("ttex"), FourCC::from_text("icec").value))};
        spell.surface.opacity = 128.f / 255;
        p[0] = p[1] = 0;
        p[2] = spell.data->spread_limit;
        spell.sound_event(match, 4);
        spell.stage = 16;
    }
    const auto grounded = [&](const CreatureActor& actor) {
        if (!match.terrain)
            return false;
        const float height = static_cast<float>(
            terrain_surface(*match.terrain, actor.motion.position.x, actor.motion.position.y));
        return std::abs(actor.motion.position.z - height) < 5;
    };
    const auto reach = [&](const CreatureActor& actor) {
        const float x = actor.motion.position.x - spell.motion.position.x,
                    y = actor.motion.position.y - spell.motion.position.y;
        return std::sqrt(x * x + y * y);
    };
    const auto everyone = [&](auto&& fn) {
        for (unsigned group = 0; group < 2; ++group)
            match.visit_group(group, 5, [&](MatchObject& object) {
                if (auto* actor = living_unit(&object))
                    fn(*actor);
                return 0;
            });
    };
    if (spell.stage == 16) {
        if (spell.cast_alpha == 1)
            spell.stage = 32;
        // 0x4223F0: everyone but the caster inside the ice and near the ground is named and frozen ('frez').
        const float radius = p[0];
        everyone([&](CreatureActor& actor) {
            if (&actor == spell.owner || actor.named(hop_name(spell)) || reach(actor) >= radius || !grounded(actor))
                return;
            mark_hit(actor, spell);
            if (const auto* freeze =
                    match.spell_registry ? match.spell_registry->find(FourCC::from_text("frez")) : nullptr;
                freeze && actor.allegiance)
                SpellActor::launch_side_cast(match, *actor.allegiance, *freeze, &actor, actor.motion.position);
        });
    } else if (spell.stage == 32) {
        if ((spell.remaining -= ticks) <= 0)
            spell.stage = 48;
    } else if (spell.stage == 48 || spell.stage == 49) {
        if (spell.stage == 48) {
            // 0x422550: the next 2.5 of ice breaks with sound 16 and a 'shrd' ring, damaging grounded units
            // in that band once (0x4224B0).
            const float inner = p[1], outer = inner + 2.5f;
            p[1] = outer;
            spell.sound_event(match, 16);
            SpellActor::launch_ground_ring(match, spell, 0x0a011008, FourCC::from_text("shrd"), inner + 1.25f,
                                           2.5f, .25f, 10, 20, 1, 64, 256);
            everyone([&](CreatureActor& actor) {
                const float distance = reach(actor);
                if (distance >= inner && distance <= outer && grounded(actor))
                    spell_damage(match, &spell, &actor, spell.data->damage_type, spell.data->amount, 0);
            });
            if (p[1] >= p[0])
                return remove_spell(spell);
            spell.stage = 49;
            spell.next_pulse = 5;
        }
        if ((spell.next_pulse -= ticks) <= 0)
            spell.stage = 48;
    } else if (spell.stage == 240 && spell.cast_alpha <= 0)
        return remove_spell(spell);
    frozen_danger(match, spell, spell.motion.position);
    const float goal = spell.stage == 240 ? 0.f : 1.f;
    spell.cast_alpha = spell.cast_alpha < goal ? std::min(goal, spell.cast_alpha + ticks * .05f)
                                               : std::max(goal, spell.cast_alpha - ticks * .05f);
    p[0] = std::sqrt(spell.cast_alpha) * spell.data->spread_limit;
    return 1;
}
void pulse_wall(Match& match, SpellActor& spell, int ticks, bool spike, bool bits, bool wailing) {
    const Vec3 axis = wall_axis(spell);
    const bool vines = spell.effect == SpellActor::Effect::vine_wall;
    const float length = (vines || spike || bits || wailing) ? spell.ability.length * 2
                                                             : std::max(spell.data->spread_limit, 10.f);
    // The vine wall keeps its two ends apart (ability.draw_back <= 0 <= ability.length, 0x427CD0).
    const bool ends = vines || spike || bits || wailing;
    const float low = ends ? spell.ability.draw_back : -length * .5f, high = ends ? spell.ability.length : length * .5f;
    const Vec3 start{spell.motion.position.x + axis.x * low, spell.motion.position.y + axis.y * low,
                     spell.motion.position.z};
    const Vec3 end{spell.motion.position.x + axis.x * high, spell.motion.position.y + axis.y * high,
                   spell.motion.position.z};
    const float half = (vines || spike || bits || wailing)
                           ? spell.data->real(FourCC::from_text("fwid"), wailing ? 10.f : 5.f) * .5f
                           : std::max(8.f, spell.data->radius);
    visit_units(match, (spell.owner ? spell.owner : &spell), true, [&](CreatureActor& actor) {
        if (!actor.source_motion())
            return;
        auto position = actor.source_motion()->position;
        if (vines || spike || bits || wailing)
            position.z = start.z; // Wall width is horizontal; VinesHold handles the height limit.
        if (segment_distance(position, start, end) > half)
            return;
        if (bits) {
            const float ground =
                match.terrain ? static_cast<float>(terrain_surface(*match.terrain, actor.motion.position.x,
                                                                   actor.motion.position.y))
                              : spell.motion.position.z;
            if (actor.motion.position.z + actor.motion.box_min.z > ground + spell.surface.parameters[4] ||
                actor.motion.position.z + actor.motion.box_max.z < ground)
                return;
            actor.accumulate_damage({spell.data->damage_type, spell.data->amount, spell.reference()}, ticks);
            actor.burn_near(actor.motion.position, 1, 10); // 0x428030: 0x45CB90(1, 10).
            return;
        }
        if (wailing) {
            // WailingWallRun 0x427B20 / 0x433A10: a unit within the wall's height loses a second's worth of
            // its whole mana a second and is slowed, and gets a spirit of its own unless one holds it.
            const float ground =
                match.terrain ? static_cast<float>(terrain_surface(*match.terrain, actor.motion.position.x,
                                                                   actor.motion.position.y))
                              : spell.motion.position.z;
            if (actor.motion.position.z + actor.motion.box_min.z >
                    ground + spell.data->real(FourCC::from_text("fhgt"), 7.5f) ||
                actor.motion.position.z + actor.motion.box_max.z < ground)
                return;
            actor.change_mana_fixed(static_cast<int>(-actor.maximum_mana * ticks * (65536.0 / 60)));
            actor.motion.velocity.x *= .8f;
            actor.motion.velocity.y *= .8f;
        }
        if (actor.named(hop_name(spell)))
            return;
        if (spike) {
            // 0x427B20 / 0x447340: a unit within the wall's height that is not yet named gets the
            // SpikeWallHitNTT, which names, knocks down and damages it.
            const float ground =
                match.terrain ? static_cast<float>(terrain_surface(*match.terrain, actor.motion.position.x,
                                                                   actor.motion.position.y))
                              : spell.motion.position.z;
            if (actor.motion.position.z + actor.motion.box_min.z > ground + spell.data->real(FourCC::from_text("fhgt"), 7.5f) ||
                actor.motion.position.z + actor.motion.box_max.z < ground)
                return;
            auto& ntt = launch_child(match, spell, SpellActor::Effect::spike_wall_hit, &actor, actor.motion.position);
            ntt.run(match, 0);
            return;
        }
        if (vines) {
            // 0x4279F0: the unit must overlap the wall's height above the ground under it.
            const float ground =
                match.terrain ? static_cast<float>(terrain_surface(*match.terrain, actor.motion.position.x,
                                                                   actor.motion.position.y))
                              : spell.motion.position.z;
            if (actor.motion.position.z + actor.motion.box_min.z > ground + spell.data->real(FourCC::from_text("fhgt"), 7.5f) ||
                actor.motion.position.z + actor.motion.box_max.z < ground)
                return;
            // 0x42ed10: crossing the wall launches an eight-vine hold. The
            // wall's stock damage is zero; a generic hit cannot entangle anyone.
            if (actor.named(FourCC::from_text("PARA")))
                return;
            auto& hold =
                launch_child(match, spell, SpellActor::Effect::grasping_vines, &actor, actor.motion.position);
            auto definition = std::make_shared<SpellData>(*spell.data);
            definition->duration = std::max(10.f, spell.remaining / 60.f);
            hold.data = std::move(definition);
            hold.stage = 16;
            hold.run(match, 0);
            return;
        }
        if (!wailing) {
            mark_hit(actor, spell);
            strike(match, spell, &actor, actor.motion.position, false);
        } else {
            auto& demon =
                launch_child(match, spell, SpellActor::Effect::wailing_demon, &actor, actor.motion.position);
            demon.run(match, 0);
        }
    });
    const Vec3 mid{(start.x + end.x) * .5f, (start.y + end.y) * .5f, start.z};
    if (bits && !spell.host.initialized) {
        auto& bit = launch_child(match, spell, SpellActor::Effect::fire_wall_bit, &spell, mid);
        bit.remaining = spell.remaining;
        bit.run(match, 0);
        spell.host.initialized = true;
    }
}
int run_wall(Match& match, SpellActor& spell, int ticks, SpellActor::Effect earth, bool spike, bool bits,
             bool wailing) {
    // VineWallRun 0x42eda0 sets up at stage 0 (0x42EA60) and grows while the wizard is still casting.
    const bool vine_wall = spell.effect == SpellActor::Effect::vine_wall && !spike && !bits && !wailing;
    // FireWallRun 0x4282b0 also sets up at once (0x428090) and grows its two ends during the cast.
    const bool fire_wall = bits && !wailing;
    // SpikeWallStart 0x447390 (setup 0x446FB0) does too, through 0x447240.
    const bool spike_wall = spike && !bits && !wailing;
    // WailingWallRun 0x433D60 (setup 0x433B90) as well, with a 'tcst' caster column.
    const bool two_ends = vine_wall || fire_wall || spike_wall || wailing;
    if (two_ends) {
        if (!spell.stage) {
            SpellActor::launch_cast_hands(match, spell);
            if (fire_wall && spell.owner)
                SpellActor::launch_caster_column(match, spell, 0x1a012008, FourCC::from_text("firy"), 2, 0, 2, 2,
                                                 3);
            if (wailing && spell.owner)
                SpellActor::launch_caster_column(
                    match, spell, 0x1a010808,
                    FourCC{static_cast<unsigned>(
                        spell.data->integer(FourCC::from_text("tcst"), FourCC::from_text("cst1").value))},
                    2, 0, 2, 2, 3);
            spell.stage = 16; // Runs the shared setup below, then waits on the cast at stage 1.
        } else if (spell.stage == 1) {
            // The caster's vfunc 280 state: 16 aborts, 0x20000000 releases; no caster aborts.
            if (!spell.owner)
                spell.stage = 240;
            else if (auto* caster = dynamic_cast<CreatureActor*>(spell.owner)) {
                const auto [state, alpha] = caster->cast_state();
                spell.cast_alpha = alpha;
                if (state == 16)
                    spell.stage = 240;
                else if (state == 0x20000000)
                    spell.stage = 17;
            }
        }
    } else if (const auto waiting = wait_cast(match, spell))
        return waiting;
    if (spell.stage == 16) {
        aim_point(spell);
        if (spell.effect == SpellActor::Effect::vine_wall || spike || bits || wailing) {
            if (match.terrain)
                spell.motion.position.z = static_cast<float>(
                    terrain_surface(*match.terrain, spell.motion.position.x, spell.motion.position.y));
            spell.ability.destination = wall_axis(spell);
            spell.surface.kind = (bits || wailing) ? SpellActor::CurvedSurface::fire_wall
                                 : spike           ? SpellActor::CurvedSurface::spikes
                                                   : SpellActor::CurvedSurface::vines;
            spell.surface.opacity = (bits || wailing) ? 0.f : 1.f;
            spell.surface.material = wailing ? 0x02011008 : bits ? 0x12012008 : 0x00010001;
            // Native sets 0x12010808 (blend mode 0x800, multiply in 3d_d3d), which our renderers do not
            // support (D3D11 draws it opaque, black behind the flames); the additive mode is kept.
            spell.surface.textures[0] = FourCC{static_cast<unsigned>(spell.data->integer(
                FourCC::from_text("ttex"),
                FourCC::from_text(fire_wall ? "fxpl" : spike ? "mud1" : wailing ? "wwal" : "vine").value))};
            if (fire_wall || wailing) {
                // 0x428090: random phases and phase drifts, a (1,.5,0) light of radius 100.
                auto& q = spell.surface.parameters;
                q[5] = match.visual_random.next() * 6.2831802f / 32768;
                q[6] = match.visual_random.next() * 6.2831802f / 32768;
                spell.ability.origin = {match.visual_random.next() * .20943934f / 32768 - .10471967f,
                                        match.visual_random.next() * .20943934f / 32768 - .10471967f, 0};
                if (fire_wall)
                    spell.light = {true, spell.motion.position, {1, .5f, 0}, 0, 0, 100};
            }
            spell.emission = 0; // Wailing wall: the next spirit mark either side.
            spell.ability.length = 0;
            spell.ability.draw_back = 0;
            spell.ability.clock = 3; // 0x42EA60: both ends grow.
            spell.age = 0;
            spell.sound_event(match, 2);
        }
        spell.remaining = static_cast<int>(spell.data->duration * 60.f);
        spell.next_pulse = 16;
        if (earth != SpellActor::Effect::healing) {
            auto& child = launch_child(match, spell, earth, &spell, spell.motion.position);
            child.remaining = spell.remaining;
            child.run(match, 0);
        }
        if (fire_wall) {
            auto& bit = launch_child(match, spell, SpellActor::Effect::fire_wall_bit, &spell, spell.motion.position);
            bit.run(match, 0);
            spell.host.initialized = true;
        }
        spell.stage = two_ends ? 1 : 17;
        if (match.navigation)
            match.navigation->dirty_static();
    }
    const float wall_width =
        wailing ? spell.data->real(FourCC::from_text("fwid"), 10.f) * .5f * spell.surface.opacity
                : spell.data->real(FourCC::from_text("fwid"), fire_wall ? 10.f : 5.f) *
                      (fire_wall ? spell.surface.opacity : 1.f);
    const auto grow_ends = [&](float step) { grow_wall_ends(match, spell, step, wall_width); };
    if (spell.stage == 17 || (two_ends && spell.stage == 1)) {
        if (bits || wailing) {
            const auto old_length = static_cast<int>(spell.ability.length - spell.ability.draw_back);
            for (int n = 0; n < ticks; ++n)
                grow_ends(fire_wall ? .5f : .25f);
            if (wailing) {
                // Each end reaching the next 15-unit mark (from 0) frees an ambient spirit.
                const int count = (spell.ability.draw_back <= -spell.emission) + (spell.ability.length >= spell.emission);
                if (count) {
                    spell.emission += 15;
                    for (int n = 0; n < count; ++n) {
                        auto& spirit =
                            launch_child(match, spell, SpellActor::Effect::wailing_demon, &spell, spell.motion.position);
                        spirit.run(match, 0);
                    }
                }
            }
            if (match.navigation && static_cast<int>(spell.ability.length - spell.ability.draw_back) != old_length)
                match.navigation->dirty_static();
            spell.surface.opacity = std::min(1.f, spell.surface.opacity + ticks / 60.f);
            spell.surface.frame += ticks * (wailing ? .25f : .26666667f);
            auto& p = spell.surface.parameters;
            p[0] = spell.ability.length;
            p[1] = spell.ability.destination.x;
            p[2] = spell.ability.destination.y;
            p[3] = spell.data->real(FourCC::from_text("fwid"), 10.f) * .5f * spell.surface.opacity;
            p[4] = spell.data->real(FourCC::from_text("fhgt"), 7.5f) * spell.surface.opacity;
            p[7] = spell.ability.draw_back;
            p[8] = spell.ability.length;
            // Phase drifts wander by up to .00087 a tick.
            p[5] += ticks * spell.ability.origin.x;
            p[6] += ticks * spell.ability.origin.y;
            spell.ability.origin.x += match.visual_random.next() * .0017453277f / 32768 - .00087266386f;
            spell.ability.origin.y += match.visual_random.next() * .0017453277f / 32768 - .00087266386f;
            if (fire_wall) {
                // The light flickers at .3-1 of the alpha.
                spell.light.position = spell.motion.position;
                spell.light.intensity =
                    (match.visual_random.next() * .7f / 32768 + .3f) * spell.surface.opacity;
                // 0x435BC0(4): a 'tscr' (default 'quk1') scar at a random point on the wall, 1 in 15 ticks.
                if (match.visual_random.next() < 2184.5333f) {
                    const float along = spell.ability.draw_back + match.visual_random.next() *
                                                                      (spell.ability.length - spell.ability.draw_back) /
                                                                      32768,
                                across = (match.visual_random.next() / 16384.f - 1) * p[3];
                    const Vec3 at{spell.motion.position.x + spell.ability.destination.x * along - spell.ability.destination.y * across,
                                  spell.motion.position.y + spell.ability.destination.y * along + spell.ability.destination.x * across,
                                  spell.motion.position.z};
                    match.scar_requests.push_back(
                        {FourCC{static_cast<unsigned>(spell.data->integer(FourCC::from_text("tscr"),
                                                                          FourCC::from_text("quk1").value))},
                         at, match.clock().match, ++match.terrain_revision});
                }
            }
        }
        if (spell.surface.kind == SpellActor::CurvedSurface::vines || spike) {
            const auto& axis = spell.ability.destination;
            const auto old_length = static_cast<int>(spell.ability.length);
            const auto add_vine_at = [&](float along) { // 0x42EB30.
                const float across = match.visual_random.next() * 10.f / 32768 - 5;
                Vec3 root{spell.motion.position.x + axis.x * along - axis.y * across,
                          spell.motion.position.y + axis.y * along + axis.x * across, spell.motion.position.z};
                if (match.terrain)
                    root.z = static_cast<float>(terrain_surface(*match.terrain, root.x, root.y));
                if (root.z <= 0)
                    return;
                if (spike) {
                    // 0x4470B0: a spike along (+-5, +-5, 5..10) of length 2..5 and radius .5, rising at
                    // pi/2..pi / 60 a tick; one in four plays sound 8 there; each leaves a 'tscr' scar.
                    const auto pick = [&](float a, float b) { return a + match.visual_random.next() * (b - a) / 32768; };
                    Vec3 lean{pick(-5, 5), pick(-5, 5), pick(5, 10)};
                    const float rate = pick(3.1415901f, 1.5707951f) / 60, length = pick(5, 2);
                    const float norm = std::sqrt(lean.x * lean.x + lean.y * lean.y + lean.z * lean.z);
                    spell.add_vine(root, {root.x + lean.x / norm * length, root.y + lean.y / norm * length,
                                          root.z + lean.z / norm * length}, .5f);
                    spell.surface.vines_data.back().speed = rate / 1.5707951f;
                    if ((match.random().next() & 3) == 0) {
                        const Vec3 saved = spell.motion.position;
                        spell.motion.position = root;
                        spell.sound_event(match, 8);
                        spell.motion.position = saved;
                    }
                    match.scar_requests.push_back({FourCC{static_cast<unsigned>(spell.data->integer(
                                                       FourCC::from_text("tscr"), FourCC::from_text("quk1").value))},
                                                   root, match.clock().match, ++match.terrain_revision});
                    return;
                }
                const Vec3 tip{root.x + match.visual_random.next() * 4.f / 32768 - 2,
                               root.y + match.visual_random.next() * 4.f / 32768 - 2,
                               root.z + 5 + match.visual_random.next() * 5.f / 32768};
                spell.add_vine(root, tip, .5f);
            };
            for (int n = 0; (vine_wall || spike_wall) && n < ticks; ++n) {
                grow_ends(.25f);
                const auto mask = spell.ability.clock;
                if ((match.visual_random.next() & 3) == 0 && spell.surface.vines_data.size() < 126) {
                    if (mask & 1)
                        add_vine_at(match.visual_random.next() / 32768.f - .5f + spell.ability.draw_back);
                    if (mask & 2)
                        add_vine_at(match.visual_random.next() / 32768.f - .5f + spell.ability.length);
                }
            }
            follow_terrain(match, spell);
            spell.advance_vines(ticks, 1);
            if (match.navigation && static_cast<int>(spell.ability.length) != old_length)
                match.navigation->dirty_static();
        }
        if (spell.stage == 1)
            return 1; // Growing during the cast; entangling and the countdown start on release.
        spell.next_pulse += ticks;
        if (bits || vine_wall || spike_wall || wailing || spell.next_pulse >= 16) {
            const int pulse_ticks = bits || vine_wall || spike_wall || wailing ? ticks : spell.next_pulse;
            spell.next_pulse = 0;
            pulse_wall(match, spell, pulse_ticks, spike, bits, wailing);
        }
        spell.remaining -= ticks;
        if (spell.remaining <= 0)
            spell.stage = 240;
    }
    if (spell.stage >= 240) {
        if (bits || wailing) {
            spell.stop_sound_event(2);
            spell.surface.opacity = std::max(0.f, spell.surface.opacity - ticks * .01f);
            if (fire_wall)
                spell.light.intensity = spell.surface.opacity;
            spell.surface.parameters[4] =
                spell.data->real(FourCC::from_text("fhgt"), 7.5f) * spell.surface.opacity;
            if (spell.surface.opacity > 0)
                return 1;
        }
        if (spell.surface.kind == SpellActor::CurvedSurface::vines || spike)
            follow_terrain(match, spell);
        if ((spell.surface.kind == SpellActor::CurvedSurface::vines || spike) &&
            spell.advance_vines(ticks, 0))
            return 1;
        if (match.navigation)
            match.navigation->dirty_static();
        return remove_spell(spell);
    }
    return 1;
}
int run_fence(Match& match, SpellActor& spell, int ticks) {
    // FenceRun 0x4212F0 (setup 0x421280: both ends growing at .3 per tick, posts from 5 either side, 10
    // apart).
    if (!spell.stage) {
        spell.ability.clock = 3;
        spell.ability.draw_back = spell.ability.length = 0;
        spell.emission = -5;
        spell.age = 5;
    }
    const auto waiting = wait_cast(match, spell);
    if (spell.stage == 1)
        SpellActor::caster_sparks(match, spell, 8); // A moving caster throws sparks.
    if (waiting)
        return waiting;
    if (spell.stage == 16) {
        aim_point(spell);
        spell.ability.origin = spell.motion.position;
        spell.ability.destination = wall_axis(spell);
        spell.stage = 17;
    }
    if (spell.stage == 17) {
        grow_wall_ends(match, spell, ticks * .3f, spell.data->real(FourCC::from_text("fwid"), 10));
        const auto post_at = [&](float s) {
            const auto& origin = spell.ability.origin;
            const auto& axis = spell.ability.destination;
            const Vec3 post{origin.x + axis.x * s, origin.y + axis.y * s, origin.z};
            auto& child = launch_child(match, spell, SpellActor::Effect::fence_post, nullptr, post);
            child.remaining = static_cast<int>(spell.data->duration * 60.f);
            child.run(match, 0);
        };
        while (spell.emission >= spell.ability.draw_back) {
            post_at(spell.emission);
            spell.emission -= 10;
        }
        while (spell.age <= spell.ability.length) {
            post_at(spell.age);
            spell.age += 10;
        }
        if (!spell.ability.clock)
            return remove_spell(spell);
        return 1;
    }
    return remove_spell(spell);
}
int run_frozen_ground(Match& match, SpellActor& spell, int ticks) {
    // FrozenGroundStart 0x422050: a spinning MRM grows in the hand during the cast (scale toward the cast
    // alpha at 1/180 per tick, lifted from the ground by that scale), arcs to the ground, shatters into
    // 'shrd' bits and opens the field.
    if (!spell.stage) {
        // 0x421C20.
        SpellActor::launch_cast_hands(match, spell);
        spell.set_projectile_model(match,
                                   FourCC{static_cast<unsigned>(spell.data->integer(
                                       FourCC::from_text("tmod"), FourCC::from_text("bold").value))},
                                   0);
        const auto spin = [&] { return (match.random().next() / 16384.f - 1) * .052359833f; };
        spell.motion.angular_velocity = {spin(), spin(), spin()};
        spell.start_return_projectile(match, spell.motion.position, 0);
        spell.motion.flags = 0x38;
        spell.model_scale = 0;
        spell.next_pulse = 0;
        spell.sound_event(match, 1);
        spell.stage = 1;
    }
    float goal = spell.stage >= 240 ? 0.f : spell.stage == 1 ? spell.cast_alpha : 1.f;
    if (spell.stage == 1) {
        if (const auto next = mid_cast(spell, 16, 240))
            spell.stage = static_cast<int>(next);
        goal = spell.cast_alpha;
        if (const auto* caster = spell.owner ? spell.owner->source_motion() : nullptr) {
            // unk_4CB134 hand offset, then the height above the ground scales with the model.
            const auto& o = caster->orientation;
            spell.motion.position = {caster->position.x + o[3] + o[6] * 2, caster->position.y + o[4] + o[7] * 2,
                                     caster->position.z + o[5] + o[8] * 2};
            if (match.terrain) {
                const float ground = static_cast<float>(
                    terrain_surface(*match.terrain, spell.motion.position.x, spell.motion.position.y));
                spell.motion.position.z = (spell.motion.position.z - ground) * spell.model_scale + ground;
            }
        }
    }
    if (spell.stage == 16) {
        // 0x437B70: a 'cst0' box trail sized from the model's half diagonal.
        const auto& m = spell.motion;
        const float dx = m.box_max.x - m.box_min.x, dy = m.box_max.y - m.box_min.y, dz = m.box_max.z - m.box_min.z;
        const float half = std::sqrt(dx * dx + dy * dy + dz * dz) * .5f;
        SpellActor::launch_box_trail(match, spell, 0x1a012008, FourCC::from_text("cst0"), half * .7071f,
                                     std::clamp(half * .5f, .25f, 2.f), 2, .5f);
        spell.ability.origin = spell.motion.position;
        spell.age = 0;
        spell.stage = 17;
        spell.sound_event(match, 2);
    }
    if (spell.stage == 17) {
        frozen_danger(match, spell, spell.point);
        if (match.terrain)
            spell.point.z = static_cast<float>(terrain_surface(*match.terrain, spell.point.x, spell.point.y));
        spell.age = std::min(1.f, spell.age + ticks / 120.f);
        const float t = spell.age, u = 1 - t, blend = 3 * u * t * t + t * t * t;
        spell.motion.position = {spell.ability.origin.x + (spell.point.x - spell.ability.origin.x) * blend,
                                 spell.ability.origin.y + (spell.point.y - spell.ability.origin.y) * blend,
                                 spell.ability.origin.z + (spell.point.z - spell.ability.origin.z) * blend +
                                     45 * u * t};
        if (spell.age >= 1)
            spell.stage = 32;
    } else if (spell.stage == 32) {
        SpellActor::launch_fragment_burst(match, spell, FourCC::from_text("shrd"), 64, 10); // 0x439400.
        auto& field = launch_child(match, spell, SpellActor::Effect::frozen_ground_field, nullptr,
                                   spell.motion.position);
        field.run(match, 1);
        spell.stage = 240;
    } else if (spell.stage >= 240 && spell.model_scale == 0)
        return remove_spell(spell);
    // Scale toward the goal: 1/180 per tick while held, .05 once shattered.
    const float rate = (spell.stage >= 240 ? .05f : 1 / 180.f) * ticks;
    spell.model_scale = spell.model_scale < goal ? std::min(goal, spell.model_scale + rate)
                                                 : std::max(goal, spell.model_scale - rate);
    return 1;
}
int run_healing_aura(Match& match, SpellActor& spell, int ticks) {
    auto& cloud = spell.altar_cloud;
    if (!spell.stage) {
        cloud.enabled = true;
        cloud.material = 0x1a012008;
        cloud.texture = FourCC{static_cast<unsigned>(
            spell.data->integer(FourCC::from_text("tcld"), FourCC::from_text("fbal").value))};
        if (!match.cloud_noise)
            match.cloud_noise = std::make_shared<CloudNoise>(match.visual_random);
        cloud.noise = match.cloud_noise;
        // 0x42c460: a yellow light (unk_4CB8EC) radius 50 follows the aura's fade.
        spell.light = {true, spell.motion.position, {1, 1, 0}, 0, 1, 50};
        if (spell.owner) {
            SpellActor::launch_cast_hands(match, spell);
            SpellActor::launch_caster_column(match, spell, 0x1a012008, FourCC::from_text("glo2"), 2, 0, 2,
                                             1.5f, 3);
            spell.stage = 1;
        } else
            spell.stage = 16; // 0x44C0D0: no caster skips the cast.
    }
    const auto waiting = wait_cast(match, spell);
    if (spell.stage == 16) {
        spell.remaining = static_cast<int>(spell.data->duration * 60.f);
        spell.next_pulse = 16;
        spell.stage = 17;
        spell.sound_event(match, 2);
    }
    if (spell.stage == 17) {
        follow_owner(spell);
        spell.next_pulse += ticks;
        if (spell.next_pulse >= 16) {
            spell.next_pulse = 0;
            // 0x492300(5, spell+440, spread, 0x42C570) walks the spell's side, caster included.
            const auto heal = [&](CreatureActor& actor) {
                if (!actor.source_motion())
                    return;
                if (distance_squared(spell.motion.position, actor.source_motion()->position) >
                    static_cast<double>(spell.data->spread_limit) * spell.data->spread_limit)
                    return;
                SpellActor::heal(match, actor, 256, 64, 256);
            };
            visit_units(match, (spell.owner ? spell.owner : &spell), false, heal);
            if (auto* caster = living_unit(spell.owner))
                heal(*caster);
        }
        spell.remaining -= ticks;
        if (spell.remaining <= 0)
            spell.stage = 240;
    }
    if (spell.stage < 240 && !living_unit(spell.effect_target))
        spell.stage = 240;
    const float goal = spell.stage >= 240 ? 0.f : spell.stage == 17 ? 1.f : spell.cast_alpha;
    spell.age = spell.age < goal ? std::min(goal, spell.age + ticks / 60.f)
                                 : std::max(goal, spell.age - ticks / 60.f);
    if (const auto* target = spell.effect_target ? spell.effect_target->source_motion() : nullptr) {
        spell_target_point(spell.motion.position, spell.effect_target);
        cloud.radii = {(target->box_max.x - target->box_min.x) * .55f * spell.age,
                       (target->box_max.y - target->box_min.y) * .55f * spell.age,
                       (target->box_max.z - target->box_min.z) * .605f * spell.age};
    }
    spell.light.position = spell.motion.position;
    spell.light.intensity = spell.age;
    cloud.opacity = spell.age * 128.f / 255;
    cloud.frame += ticks * .25f;
    cloud.phase.x += ticks * 8.f;
    cloud.phase.y += ticks * 8.f;
    cloud.phase.z += ticks * 8.f;
    if (spell.stage >= 240 && spell.age == 0)
        return remove_spell(spell);
    if (waiting)
        return waiting;
    return 1;
}
int run_meteor_shower(Match& match, SpellActor& spell, int ticks) {
    if (!spell.stage) {
        // MeteorShowerRun 0x445FD0: a caster gets the 0x436AC0 'triz' (default 'rock') column (rate 2,
        // life 2, spread 2, height 8) and a 0x436DF0 'dirt' spray (box 3, size .5).
        spell.effect_target = spell.owner;
        if (spell.owner) {
            SpellActor::launch_caster_column(match, spell, 0x0a011008,
                                             FourCC{static_cast<unsigned>(spell.data->integer(
                                                 FourCC::from_text("triz"), FourCC::from_text("rock").value))},
                                             2, 0, 2, 2, 8);
            SpellActor::launch_dirt_spray(match, spell, &spell, FourCC::from_text("dirt"), true, 3, .5f, 0);
        }
    }
    if (const auto waiting = wait_cast(match, spell))
        return waiting;
    if (spell.stage == 16) {
        spell.remaining = static_cast<int>(spell.data->duration * 60);
        spell.stage = 17;
    }
    if (spell.stage == 17) {
        if (!living_unit(spell.owner))
            return remove_spell(spell);
        spell.motion.position = spell.owner->source_motion()->position;
        if (match.random().next() % 60 <= ticks) {
            std::vector<CreatureActor*> candidates;
            for (unsigned group : {0u, 1u})
                match.visit_group(group, 5, [&](MatchObject& object) {
                    if (auto* actor = living_unit(&object);
                        actor && actor != spell.owner &&
                        distance_squared(spell.motion.position, actor->motion.position) <=
                            spell.data->spread_limit * spell.data->spread_limit)
                        candidates.push_back(actor);
                    return 0;
                });
            Vec3 at = spell.motion.position;
            const auto selected = match.random().next() % (candidates.size() + 4);
            if (selected < candidates.size())
                spell_target_point(at, candidates[selected]);
            else {
                const float angle = match.random().next() * 6.28318f / 32768,
                            radius = match.random().next() * spell.data->spread_limit / 32768;
                at.x += std::sin(angle) * radius;
                at.y += std::cos(angle) * radius;
            }
            if (match.terrain)
                at.z = static_cast<float>(terrain_surface(*match.terrain, at.x, at.y));
            Vec3 origin = spell.motion.position;
            origin.x += match.random().next() * 30.f / 32768 - 15;
            origin.y += match.random().next() * 30.f / 32768 - 15;
            auto& meteor = launch_child(match, spell, SpellActor::Effect::meteor, nullptr, origin);
            meteor.point = at;
            meteor.run(match, 0);
        }
        if ((spell.remaining -= ticks) <= 0)
            return remove_spell(spell);
    }
    return spell.stage >= 240 ? remove_spell(spell) : 1;
}
int run_fire_rain(Match& match, SpellActor& spell, int ticks) {
    const bool plague = spell.effect == SpellActor::Effect::plague;
    // RainOfFireRun 0x429210 / 0x428fa0: a fixed cloud 100 units above the cast point.
    // The target's subsequent movement/death never moves that cloud.
    if (!spell.stage) {
        SpellActor::launch_cast_hands(match, spell);
        if (!plague && spell.owner) // 0x436AC0: 'firy' column (rate 2, size 0, life 2, spread 2, height 4).
            SpellActor::launch_caster_column(match, spell, 0x1a012008, FourCC::from_text("firy"), 2, 0, 2, 2, 4);
        if (plague && spell.owner) // PlagueRun 0x432EB0: the same column in 'pois', blended.
            SpellActor::launch_caster_column(match, spell, 0x0a011008, FourCC::from_text("pois"), 2, 0, 2, 2, 4);
        spell.motion.position = spell.point;
        spell.motion.position.z += plague ? 75.f : 100.f;
        spell.remaining = static_cast<int>(spell.data->duration * 60.f);
        spell.stage = 1;
        spell.sound_event(match, 1);
        auto& cloud = spell.altar_cloud;
        cloud.enabled = true;
        cloud.material = 0x02001000;
        if (!match.cloud_noise)
            match.cloud_noise = std::make_shared<CloudNoise>(match.visual_random);
        cloud.noise = match.cloud_noise;
    }
    if (spell.stage == 1) {
        if (const auto next = mid_cast(spell, 16, 240))
            spell.stage = static_cast<int>(next);
    }
    if (spell.stage == 16 && plague)
        spell.stage = 17; // PlagueRun has no release sounds or weather.
    if (spell.stage == 16) {
        spell.stop_sound_event(1);
        spell.sound_event(match, 2);
        // 0x4523E0: LAND::SetEffect(4, 1, 5, duration * 1.2, 0) brings weather 4 over the land for a while.
        if (!plague)
            match.weather.set(4, 1, 5, spell.data->duration * 1.2f, false);
        spell.stage = 17;
    }
    if (spell.stage == 17) {
        // 0x429060 -> 0x44d430: choose among nearby living units plus four random-ground slots.
        // Both sides are eligible; this is not a nearest-enemy attack.
        if (match.random().next() % 60 <= ticks) {
            std::vector<CreatureActor*> candidates;
            for (unsigned group : {0u, 1u})
                match.visit_group(group, 5, [&](MatchObject& object) {
                    if (auto* actor = living_unit(&object);
                        actor && candidates.size() < 256 &&
                        distance_squared(spell.point, actor->motion.position) <=
                            static_cast<double>(spell.data->spread_limit) * spell.data->spread_limit)
                        candidates.push_back(actor);
                    return 0;
                });
            Vec3 at = spell.point;
            const auto selected = match.random().next() % (candidates.size() + 4);
            if (selected < candidates.size())
                spell_target_point(at, candidates[selected]);
            else {
                const auto radius = match.random().next() * spell.data->spread_limit / 32768.0;
                const auto angle = match.random().next() * 6.2831802 / 32768.0;
                at.x += static_cast<float>(std::sin(angle) * radius);
                at.y += static_cast<float>(std::cos(angle) * radius);
            }
            if (match.terrain)
                at.z = static_cast<float>(terrain_true_height(*match.terrain, at.x, at.y));
            const auto radius =
                (match.random().next() * .69999999 / 32768.0 + .1000000014901161) * spell.data->spread_limit;
            const auto angle = match.random().next() * 6.2831802 / 32768.0;
            // 0x429060(…, 0) drops a plague bomb straight down over its mark; fire rain scatters across the
            // cloud (…, 1).
            const Vec3 start = plague ? Vec3{at.x, at.y, spell.motion.position.z}
                                      : Vec3{spell.motion.position.x + static_cast<float>(std::sin(angle) * radius),
                                             spell.motion.position.y + static_cast<float>(std::cos(angle) * radius),
                                             spell.motion.position.z};
            auto& child = launch_child(
                match, spell, plague ? SpellActor::Effect::plague_bomb : SpellActor::Effect::rain_fire_drop,
                nullptr, start);
            child.point = at;
            child.run(match, 0);
        }
        spell.remaining -= ticks;
        if (spell.remaining <= 0)
            spell.stage = 240;
    }
    if (spell.stage >= 240)
        spell.stop_sound_event(2);
    const float goal = spell.stage >= 240 ? 0.f : spell.stage == 1 ? spell.cast_alpha : 1.f;
    spell.age += std::clamp(goal - spell.age, -ticks / 120.f, ticks / 120.f);
    auto& cloud = spell.altar_cloud;
    const float size = .5f + spell.age * .5f;
    cloud.radii = {spell.data->spread_limit * size, spell.data->spread_limit * size, 15 * size};
    cloud.opacity = spell.age;
    cloud.phase.x += ticks;
    cloud.phase.y += ticks;
    cloud.phase.z += ticks;
    return spell.stage >= 240 && spell.age == 0 ? remove_spell(spell) : 1;
}
} // namespace
int SpellActor::run_siege(Match& match, int ticks) {
    if (!data)
        throw Error("Siege god spell requires its SPEL definition");
    switch (effect) {
    case Effect::healing_aura:
        return run_healing_aura(match, *this, ticks);
    case Effect::fire_wall:
        return run_wall(match, *this, ticks, Effect::fire_wall_earth, false, true, false);
    case Effect::spike_wall:
        return run_wall(match, *this, ticks, Effect::vine_wall_earth, true, false, false); // 0x42E990 too.
    case Effect::frozen_ground:
        return run_frozen_ground(match, *this, ticks);
    case Effect::wailing_wall:
        return run_wall(match, *this, ticks, Effect::healing, false, false, true);
    case Effect::vine_wall:
        return run_wall(match, *this, ticks, Effect::vine_wall_earth, false, false, false);
    case Effect::rain_of_fire:
        return run_fire_rain(match, *this, ticks);
    case Effect::fence:
        return run_fence(match, *this, ticks);
    case Effect::plague:
        return run_fire_rain(match, *this, ticks);
    case Effect::meteor_shower:
        return run_meteor_shower(match, *this, ticks);
    case Effect::fire_wall_bit:
    case Effect::fire_wall_earth:
    case Effect::wailing_demon:
    case Effect::vine_wall_earth:
        return run_wall_child(match, *this, ticks);
    case Effect::spike_wall_hit:
        return run_spike_hit(match, *this, ticks);
    case Effect::frozen_ground_field:
        return run_frozen_field(match, *this, ticks);
    case Effect::plague_bomb:
        return run_plague_bomb(match, *this, ticks);
    case Effect::meteor:
        return run_meteor(match, *this, ticks);
    case Effect::rain_fire_drop:
        return run_drop(match, *this, ticks, true);
    case Effect::fence_post:
        return run_fence_post(match, *this, ticks);
    default:
        throw Error("Invalid siege god spell");
    }
}
bool SpellActor::stamp_navigation(Navigation& navigation) const {
    const bool wall = effect == Effect::fire_wall || effect == Effect::spike_wall ||
                      effect == Effect::vine_wall || effect == Effect::wailing_wall;
    const bool post = effect == Effect::fence_post;
    if (!wall && !post)
        return false;
    if (post) {
        const float box[]{8.f, 8.f};
        navigation.stamp(1, motion.position, box, 6, true);
        return true;
    }
    const float want =
        effect == Effect::vine_wall || effect == Effect::spike_wall || effect == Effect::fire_wall
            ? ability.length * 2
        : data ? std::max(data->spread_limit, 10.f)
               : 10.f;
    const Vec3 axis = wall_axis(*this);
    const Vec3 delta{axis.x * want, axis.y * want, 0};
    const Vec3 from{motion.position.x - delta.x * .5f, motion.position.y - delta.y * .5f, motion.position.z};
    const float stroke[]{delta.x, delta.y, 4.f};
    navigation.stamp(4, from, stroke, 6, true);
    return true;
}
} // namespace sacrifice
