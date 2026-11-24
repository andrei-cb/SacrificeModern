#include "sacrifice/creature_actor.hpp"
#include "sacrifice/creature_orders.hpp"
#include "sacrifice/navigation.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include "sacrifice/spell_data.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
void spell_erupt_wave(Match& match, SpellActor& spell, float progress);
void spell_mrm_debris(Match& match, SpellActor& source, int count, float speed, FourCC model, float scale,
                      std::uint32_t material, FourCC trail, Vec3 at, float life);
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
                            SpellActor* mark, bool any = false) {
    CreatureActor* best = nullptr;
    double best_d = static_cast<double>(radius) * radius;
    const auto visit = [&](MatchObject& object) {
        auto* actor = living_unit(&object);
        if (!actor || actor == source)
            return 0;
        if (!any && (foes ? !actor->enemy(source) : !actor->friendly(source)))
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
    case SpellActor::Effect::charm:
        return FourCC::from_text("chrm");
    case SpellActor::Effect::blind_rage:
        return FourCC::from_text("rage");
    case SpellActor::Effect::bovine:
        return FourCC::from_text("bovi");
    case SpellActor::Effect::cloud_kill:
        return FourCC::from_text("ckil");
    case SpellActor::Effect::intestinal_vap:
        return FourCC::from_text("ivap");
    case SpellActor::Effect::divine_retribution:
        return FourCC::from_text("retr");
    case SpellActor::Effect::volcano:
        return FourCC::from_text("volc");
    case SpellActor::Effect::bore:
        return FourCC::from_text("bore");
    case SpellActor::Effect::tornado:
        return FourCC::from_text("torn");
    case SpellActor::Effect::death:
        return FourCC::from_text("deat");
    default:
        return FourCC::from_text("ultm");
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
int run_lava(Match& match, SpellActor& spell, int ticks) {
    if (!spell.stage) {
        spell.start_creature_projectile(match, 1200,
                                        FourCC{static_cast<unsigned>(spell.data->integer(
                                            FourCC::from_text("tmod"), FourCC::from_text("bold").value))},
                                        1, true, false);
        // The shared projectile helper starts event 2; Volcano owns that eruption loop.
        // Native LavaBall setup starts only its flight event 8.
        spell.sound_requests.clear();
        spell.sound_event(match, 8);
        const float scale = .9f + match.random().next() * (.2f / 32768);
        spell.model_scale = scale * scale;
        spell.light = {true, spell.motion.position, {1, .5f, 0}, 1, 1, 25};
        spell_scatter(spell.motion.velocity, 750, match.random());
        const auto spin = [&] { return match.random().next() * (.10471967f / 32768) - .052359834f; };
        spell.motion.angular_velocity = {spin(), spin(), spin()};
        spell.motion.flags |= 0x38;
        // 0x429880: 0x438050(emission 2, life .75, box .3, size .5), the body-sized trail.
        SpellActor::launch_model_trail(match, spell, 0x1a012008, FourCC::from_text("firy"), 2, .75f, .3f, .5f);
    }
    if (const int flying = spell.fly_creature_projectile(match, ticks)) {
        spell.light.position = spell.motion.position;
        return flying;
    }
    spell_damage(match, &spell, spell.impact_target, spell.data->damage_type, spell.data->amount,
                 spell.data->radius, spell.motion.position);
    SpellActor::launch_fireball_visual(match, spell, SpellActor::Effect::fireball_blast);
    spell.sound_event(match, 4);
    return remove_spell(spell);
}
int run_cloud_bolt(Match& match, SpellActor& spell, int ticks) {
    if (!spell.stage) {
        // CloudKillBoltStart 0x420670: 0x4355A0(spell, spread, deviation 3, 2 strands, flags 3).
        spell.start_beam(match, spell.motion.position, spell.point, 3, 2, 3);
        spell.stage = 1;
    }
    const int state = spell.advance_beam(match, ticks);
    if (state == 2) {
        // 0x420600 damages at the beam endpoint, including untargeted ground strikes.
        spell_damage(match, &spell, spell.effect_target, spell.data->damage_type, spell.data->amount,
                     spell.data->radius, spell.ability.destination);
        attach_hit(match, spell, spell.effect_target, spell.ability.destination);
    }
    return state ? 1 : remove_spell(spell);
}
int run_tornado_particles(Match& match, SpellActor& spell, int ticks) {
    const bool ground = spell.effect == SpellActor::Effect::tornado_ground;
    auto* parent = dynamic_cast<SpellActor*>(spell.effect_target);
    auto* caster = dynamic_cast<CreatureActor*>(spell.owner);
    auto& p = spell.particles(match);
    if (!spell.stage) {
        spell.particle_material(match, p, ground ? 0x0a011008 : 0x1a012008,
                                FourCC{static_cast<unsigned>(
                                    spell.data->integer(FourCC::from_text(ground ? "ttrl" : "tcst"),
                                                        FourCC::from_text(ground ? "dirt" : "cst0").value))});
        const float size = ground ? 2.f : .5f, life = 1.5f;
        p.set_property(ground ? 129 : 132, &size, &life);
        p.set_flags(ground ? 0 : 0x80000, 0x400000);
        if (ground) {
            const float low[]{-15, -15, 0}, high[]{15, 15, 5};
            p.set_property(2, low, high);
        }
        spell.stage = 1;
    }
    float emission = 0;
    if (ground && parent && !(parent->state_flags & MatchObject::removed_flag) && parent->stage < 240 &&
        match.terrain) {
        spell.motion.position = parent->motion.position;
        const float height = static_cast<float>(
            terrain_surface(*match.terrain, spell.motion.position.x, spell.motion.position.y));
        if (height > 0)
            emission = std::clamp((5 - (spell.motion.position.z - height)) * .4f, 0.f, 2.f);
        spell.motion.position.z = height;
    } else if (!ground && caster) {
        const auto [state, alpha] = caster->cast_state();
        if (!state && spell.stage < 240) {
            emission = 1;
            spell.motion.position = caster->motion.position;
            const float radius = 2.5f + alpha * 2.5f;
            const float low[]{-radius, -radius, 0}, high[]{radius, radius, 0};
            p.set_property(2, low, high);
            const float acceleration = (alpha * 3 + 2) * 2 / (90 * 90);
            const float a[]{0, 0, acceleration * .95f}, b[]{0, 0, acceleration};
            p.set_property(4, a, b);
            spell.age += ticks * alpha * .052359834f;
            SpellActor::caster_sparks(match, spell, 8); // TornadoCastRun 0x423B50: 0x435B50(8).
        } else
            spell.stage = 240;
    } else
        spell.stage = 240;
    spell.host.time += ticks;
    const auto orientation = spell_orientation({std::sin(spell.age), std::cos(spell.age), 0});
    if (ground)
        p.set_frame({orientation, spell.motion.position});
    else {
        spell.motion.orientation = orientation;
        p.set_frame({}); // Casting particles rotate with their host, once.
    }
    p.advance(spell.host.time, emission);
    return emission == 0 && !p.alive() ? remove_spell(spell) : 1;
}
int run_ivap_guts(Match& match, SpellActor& spell, int ticks) {
    // IVapSpriteRun 0x4319F0: a 'tgut' sprite (1.75) on the 0x44C660 Catmull-Rom path, a segment every half
    // second, through random points of the victim's box (starting from min + max / 2, as native writes it);
    // when the victim goes it shrinks away over half a second.
    auto* victim = living_unit(spell.effect_target);
    auto& controls = spell.altar_streamer.controls;
    const auto random_point = [&] {
        const auto& m = victim->motion;
        const auto pick = [&](float low, float high) {
            return low + match.visual_random.next() * (high - low) / 32768;
        };
        return Vec3{pick(m.box_min.x, m.box_max.x), pick(m.box_min.y, m.box_max.y), pick(m.box_min.z, m.box_max.z)};
    };
    if (!spell.stage) {
        if (!victim)
            return remove_spell(spell);
        spell.sprite = {true,
                        FourCC{static_cast<unsigned>(
                            spell.data->integer(FourCC::from_text("tgut"), FourCC::from_text("gut2").value))},
                        0x0a011008u, 0xffffffff, 1.75f};
        const auto& m = victim->motion;
        controls[0] = controls[1] = {m.box_min.x + m.box_max.x * .5f, m.box_min.y + m.box_max.y * .5f,
                                     m.box_min.z + m.box_max.z * .5f};
        controls[2] = random_point();
        controls[3] = random_point();
        spell.age = 0;
        spell.stage = 1;
    }
    if (!victim)
        spell.stage = 240;
    if (spell.stage == 240) {
        spell.sprite.size -= ticks / 30.f;
        if (spell.sprite.size <= 0)
            return remove_spell(spell);
    } else {
        spell.age += ticks * .033333335f;
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
            return t3 * (-.5f * p0 + 1.5f * p1 - 1.5f * p2 + .5f * p3) + t2 * (p0 - 2.5f * p1 + 2 * p2 - .5f * p3) +
                   t * (-.5f * p0 + .5f * p2) + p1;
        };
        const auto& at = victim->motion.position;
        spell.motion.position = {at.x + spline(&Vec3::x), at.y + spline(&Vec3::y), at.z + spline(&Vec3::z)};
    }
    spell.sprite.frame += ticks * .25f;
    return 1;
}
int run_child_visual(Match& match, SpellActor& spell, int ticks) {
    const bool heart = spell.effect == SpellActor::Effect::charm_heart,
               guts = spell.effect == SpellActor::Effect::ivap_sprite,
               smoke = spell.effect == SpellActor::Effect::volcano_smoke,
               spatter = spell.effect == SpellActor::Effect::volcano_spatter;
    if (!spell.stage) {
        if (heart || guts || smoke) {
            const FourCC key = FourCC::from_text(heart ? "tluv" : guts ? "tgut" : "tsmk");
            const FourCC texture = FourCC::from_text(heart ? "hart" : guts ? "gut2" : "vsmk");
            spell.sprite = {true, FourCC{static_cast<unsigned>(spell.data->integer(key, texture.value))},
                            heart ? 0x1a012008u : 0x0a011008u, 0xffffffff,
                            heart  ? .25f
                            : guts ? 1.75f
                                   : 10};
        } else {
            auto& p = spell.particles(match);
            spell.particle_material(match, p, 0x1a012008, spatter ? "firy" : "cst0");
            const float size = spatter ? .5f : .5f, life = spatter ? 2.f : 1.5f;
            p.set_property(spatter ? 131 : 132, &size, &life);
            if (spatter) {
                const float low[]{-.1f, -.1f, .15f}, high[]{.1f, .1f, .4f}, gravity[]{0, 0, -.002725f};
                p.set_property(3, low, high);
                p.set_property(4, gravity, gravity);
            } else
                p.set_flags(0x80000, 0x400000);
        }
        spell.remaining = heart ? 600 : static_cast<int>(spell.data->duration * 60);
        spell.cast_alpha = 0;
        spell.age = match.visual_random.next() * (6.28318f / 32768);
        if (heart) {
            spell.sprite.phase_x = match.visual_random.next() * (6.2831802f / 32768);
            spell.sprite.phase_y = match.visual_random.next() * (6.2831802f / 32768);
        }
        spell.stage = 1;
    }
    const bool target_alive =
        spell.effect_target && !(spell.effect_target->state_flags & MatchObject::removed_flag);
    if (auto* creature = dynamic_cast<CreatureActor*>(spell.effect_target); creature && !creature->alive())
        spell.stage = 240;
    if (!target_alive || (!guts && (spell.remaining -= ticks) <= 0))
        spell.stage = 240;
    if (target_alive && spell.effect_target->source_motion()) {
        spell.motion.position = spell.effect_target->source_motion()->position;
        if (heart || guts) {
            const auto& box = *spell.effect_target->source_motion();
            if (heart) {
                // 0x42b260: height grows .25 -> min+max z while rising from mid-body to 1.5 x top.
                const float alpha = spell.cast_alpha, middle = (box.box_min.z + box.box_max.z) * .5f,
                            height = .25f + (box.box_min.z + box.box_max.z - .25f) * alpha;
                spell.sprite.size = height;
                spell.sprite.flag = true;
                spell.sprite.phase_x += ticks * .015f; // 0x42b260: wave phases, random starts.
                spell.sprite.phase_y += ticks * .01f;
                spell.motion.position.z += middle + (box.box_max.z * 1.5f - middle) * alpha + height * .5f;
            } else
                spell.motion.position.z += box.reference_center.z;
            if (guts) {
                const float radius = .5f * (box.box_max.x - box.box_min.x);
                spell.motion.position.x += std::sin(spell.age) * radius;
                spell.motion.position.y += std::cos(spell.age) * radius;
            }
        } else if (smoke)
            spell.motion.position.z += 15;
    }
    spell.age += ticks * .015f;
    spell.cast_alpha = spell.stage == 240 ? std::max(0.f, spell.cast_alpha - ticks / 30.f)
                                          : std::min(1.f, spell.cast_alpha + ticks / 60.f);
    spell.sprite.frame += ticks * (smoke ? .13333334f : .25f);
    spell.sprite.color = (static_cast<unsigned>(spell.cast_alpha * 255) << 24) | 0xffffff;
    if (!spell.particle_systems.empty()) {
        auto& p = spell.particle_systems.front();
        if (!spatter) {
            const float radius = 2.5f + spell.cast_alpha * 2.5f;
            const float low[]{-radius, -radius, 0}, high[]{radius, radius, 0};
            p.set_property(2, low, high);
        }
        p.set_frame(
            {spell_orientation({std::sin(spell.age), std::cos(spell.age), 0}), spell.motion.position});
        spell.host.time += ticks;
        p.advance(static_cast<float>(spell.host.time), spell.stage == 240 ? 0.f : 1.f);
        if (spell.stage == 240 && p.alive())
            return 1;
    }
    return spell.stage == 240 && spell.cast_alpha <= 0 ? remove_spell(spell) : 1;
}
void convert_side(Match& match, CreatureActor& victim, SpellActor& spell) {
    // vtable +104 with the caster's SIDE, then 0x45D300 follow and local selection.
    if (!spell.allegiance)
        return;
    spell.allegiance->attach(victim);
    victim.follow_and_select(match);
}
int streamer_tick(Match& match, SpellActor& spell, int ticks, bool draining = false) {
    auto& tail = spell.altar_streamer;
    tail.ticks += ticks;
    const unsigned frames = match.texture_frames ? match.texture_frames(tail.texture) : 1u;
    return tail.primitive.add_point(draining ? nullptr : &spell.motion.position, tail.ticks, frames)
               ? 1
               : remove_spell(spell);
}
// CharmRun 0x42ae00 (setup 0x42aaf0): the streamers wander a Catmull-Rom path (0x44c4a0) inside the
// caster's body box while casting, steer to the target (0x44d620), then wander inside the target's box.
int run_charm(Match& match, SpellActor& spell, int ticks) {
    auto& tail = spell.altar_streamer;
    auto* target = living_unit(spell.effect_target); // Native: kind & 5 and alive.
    const auto random_in = [&](Vec3 extent) { // 0x44C280: rand() over [-e, e] per axis.
        const auto axis = [&](float e) {
            return static_cast<float>(match.random().next() * (-e - e) * .000030517578125 + e);
        };
        const float x = axis(extent.x), y = axis(extent.y), z = axis(extent.z);
        return Vec3{x, y, z};
    };
    // 0x44E3F0: smoothed box centre (10% per call) and a radial x/y, half-height z extent.
    Vec3 extent{1, 1, 1};
    const auto anchor = [&](MatchObject* object) {
        const auto& m = *object->source_motion();
        Vec3 centre{(m.box_min.x + m.box_max.x) * .5f, (m.box_min.y + m.box_max.y) * .5f,
                    (m.box_min.z + m.box_max.z) * .5f};
        auto& previous = spell.ability.origin;
        if (distance_squared(previous, centre) < 10000) {
            centre = {previous.x + (centre.x - previous.x) * .1f, previous.y + (centre.y - previous.y) * .1f,
                      previous.z + (centre.z - previous.z) * .1f};
            previous = centre;
        }
        const float hx = (m.box_max.x - m.box_min.x) * .5f, hy = (m.box_max.y - m.box_min.y) * .5f,
                    radial = std::sqrt((hx * hx + hy * hy) * .5f);
        extent = {radial, radial, (m.box_max.z - m.box_min.z) * .5f};
        spell.point = {m.position.x + centre.x, m.position.y + centre.y, m.position.z + centre.z};
    };
    const auto wander = [&] {
        const auto previous = spell.motion.position;
        tail.phase += ticks * .033333335f;
        while (tail.phase >= 1) {
            for (unsigned i = 0; i < 3; ++i)
                tail.controls[i] = tail.controls[i + 1];
            tail.controls[3] = random_in(extent);
            tail.phase -= 1;
        }
        const auto coordinate = [&](auto member) {
            const auto a = tail.controls[0].*member, b = tail.controls[1].*member, c = tail.controls[2].*member,
                       d = tail.controls[3].*member;
            const double t = tail.phase;
            return static_cast<float>(
                (((-.5 * a + 1.5 * b - 1.5 * c + .5 * d) * t + a - 2.5 * b + 2 * c - .5 * d) * t + (-.5 * a + .5 * c)) *
                    t +
                b);
        };
        spell.motion.position = {spell.point.x + coordinate(&Vec3::x), spell.point.y + coordinate(&Vec3::y),
                                 spell.point.z + coordinate(&Vec3::z)};
        if (ticks)
            spell.motion.velocity = {(spell.motion.position.x - previous.x) / ticks,
                                     (spell.motion.position.y - previous.y) / ticks,
                                     (spell.motion.position.z - previous.z) / ticks};
    };
    // Before 16 the path follows the caster; from 16 on, a lost target ends the charm.
    if (spell.stage >= 16 && !target)
        spell.stage = 240;
    else if (auto* host = spell.stage >= 16 ? static_cast<MatchObject*>(target) : spell.owner;
             host && host->source_motion())
        anchor(host);
    if (!spell.stage) {
        tail.enabled = true;
        tail.material = 0x1a012008;
        tail.texture = FourCC{static_cast<unsigned>(
            spell.data->integer(FourCC::from_text("ttex"), FourCC::from_text("rist").value))};
        tail.primitive.configure(spell.data->integer(FourCC::from_text("ilen"), 60),
                                 spell.data->real(FourCC::from_text("fwid"), .25f));
        spell.remaining = static_cast<int>(spell.data->duration * 60);
        if (!spell.owner && spell.effect_target && spell.effect_target->source_motion())
            anchor(spell.effect_target); // 0x44E020 on the target before the caster check.
        // 0x42AAF0: 0x437C10(box .5, size .15, rate 1, life 1.5), preset 129.
        SpellActor::launch_box_trail(match, spell, 0x1a012008,
                                     FourCC{static_cast<unsigned>(spell.data->integer(
                                         FourCC::from_text("thrt"), FourCC::from_text("hrot").value))},
                                     .5f, .15f, 1.5f, 1);
        if (auto* caster = dynamic_cast<CreatureActor*>(spell.owner)) {
            caster->write_reference_point(spell.motion.position, match.random().next() & 1, true);
            Vec3 centre;
            spell_target_point(centre, caster);
            const Vec3 hand{spell.motion.position.x - centre.x, spell.motion.position.y - centre.y,
                            spell.motion.position.z - centre.z};
            tail.controls[0] = tail.controls[1] = hand;
            tail.controls[2] = random_in({1, 1, 1});
            tail.controls[3] = random_in({1, 1, 1});
        } else
            spell.motion.position = spell.point;
        if (!spell.ability.clock) {
            // 0x42AAF0: the primary creates two companions; only the primary converts allegiance.
            for (int i = 0; i < 2; ++i) {
                auto& echo = launch_child(match, spell, SpellActor::Effect::charm, spell.effect_target,
                                          spell.motion.position);
                echo.ability.clock = i + 1;
                echo.run(match, 0);
            }
        }
        // 0x44C0D0(49, 1, 32): a casterless charm starts at the target.
        if (target)
            mark_hit(*target, spell); // 0x4692A0 with SPEL+16, by the primary and both companions.
        spell.stage = spell.owner ? 1 : 32;
        return 1;
    }
    switch (spell.stage) {
    case 1:
        if (const auto next = mid_cast(spell, 16, 240))
            spell.stage = static_cast<int>(next); // 0x441C30: the new stage runs on the next call.
        wander();
        break;
    case 16: {
        // 0x42ADE0: the mover takes over with the current wander velocity, toward the body anchor.
        const auto velocity = spell.motion.velocity;
        const auto anchor_point = spell.point;
        spell.launch_projectile(match);
        spell.motion.velocity = velocity;
        spell.point = anchor_point;
        spell.motion.collision_mask = 0;
        spell.stage = 17;
    }
        [[fallthrough]];
    case 17:
        if (spell_approach_velocity(spell.motion, spell.point, ticks * spell.data->acceleration, spell.data->speed,
                                    7, 5) < 2)
            spell.stage = 32;
        break;
    case 32: {
        // 0x42AD30: continue the path from the current offset and approach velocity.
        const Vec3 offset{spell.motion.position.x - spell.point.x, spell.motion.position.y - spell.point.y,
                          spell.motion.position.z - spell.point.z};
        tail.controls[1] = offset;
        tail.controls[0] = {offset.x - spell.motion.velocity.x * 8, offset.y - spell.motion.velocity.y * 8,
                            offset.z - spell.motion.velocity.z * 8};
        tail.controls[2] = random_in(extent);
        tail.controls[3] = random_in(extent);
        spell.motion.velocity = {};
        spell.motion.flags &= ~1u;
        spell.stage = 33;
    }
        [[fallthrough]];
    case 33:
        wander();
        if ((spell.remaining -= ticks) <= 0)
            spell.stage = 48;
        break;
    case 48:
        spell.remaining = 180;
        spell.stage = 64;
        if (!spell.ability.clock && target) {
            convert_side(match, *target, spell);
            attach_hit(match, spell, target, target->motion.position);
            auto& heart =
                launch_child(match, spell, SpellActor::Effect::charm_heart, target, target->motion.position);
            heart.remaining = 600;
            heart.run(match, 0); // CharmHeartRun 0x42b260 via 0x44E850.
        }
        break;
    case 64:
        if ((spell.remaining -= ticks) <= 0)
            spell.stage = 240;
        wander();
        break;
    default:
        break;
    }
    return streamer_tick(match, spell, ticks, spell.stage == 240);
}
int run_rage_creature(Match& match, SpellActor& spell, int ticks) {
    // BlindRageCreatureRun 0x424b70: remaining countdown, 0x46DD00 attack 6/7.
    auto* host = living_unit(spell.effect_target);
    if (!spell.stage) {
        spell.remaining = spell.data ? static_cast<int>(spell.data->duration * 60.f) : 1800;
        if (host)
            host->name_reference(hop_name(spell), &spell);
        if (host) {
            spell.rate = host->stats.base[CreatureStats::health];
            constexpr std::uintptr_t rage_damage = 0x424900;
            match.functions.try_emplace(rage_damage, [](MatchObject& object, int) {
                auto& rage = static_cast<SpellActor&>(object);
                if (rage.stack.size() >= 2) {
                    const int amount = static_cast<int>(rage.stack[1]);
                    if (amount > 0 && (rage.rate -= amount) <= 0)
                        rage.remaining = 0;
                }
                return 1;
            });
            match.watch(*host, 1, {&spell, spell.reference(), rage_damage});
            host->orders.clear();
        }
        spell.next_pulse = 64;
        // 0x424990: preset 132 {.3, 2} over the host's box, eight at once, then .1667 emission.
        auto& p = spell.particles(match);
        spell.particle_material(match, p, 0x1a012008,
                                FourCC{static_cast<unsigned>(spell.data->integer(
                                    FourCC::from_text("ttex"), FourCC::from_text("swrl").value))});
        const float size = .3f, life = 2.f;
        p.set_property(132, &size, &life);
        p.set_flags(0, 0x600000);
        if (host) {
            const float low[]{host->motion.box_min.x, host->motion.box_min.y, host->motion.box_min.z};
            const float high[]{host->motion.box_max.x, host->motion.box_max.y, host->motion.box_max.z};
            p.set_property(2, low, high);
            p.emit(0, 8);
            spell.sound_event(match, 4);
        }
        spell.stage = 1;
    }
    if (!host || !host->alive())
        return remove_spell(spell);
    spell.motion.position = host->motion.position;
    auto& p = spell.particles(match);
    const float low[]{host->motion.box_min.x, host->motion.box_min.y, host->motion.box_min.z};
    const float high[]{host->motion.box_max.x, host->motion.box_max.y, host->motion.box_max.z};
    p.set_property(2, low, high);
    p.set_position({}); // Preset 132 follows the raging creature in local space.
    spell.host.time += ticks;
    p.advance(spell.host.time, .16666667f);
    if (spell.next_pulse >= 64 || (host->orders.kind() != 6 && host->orders.kind() != 7)) {
        spell.next_pulse = 0;
        // 0x492AA0: the nearest unit of any side within the data radius, else order 7 on the spot.
        const float reach = spell.data ? spell.data->radius : 30.f;
        if (auto* victim = nearest_unit(match, host->motion.position, reach, host, true, nullptr, true))
            host->issue_order({.kind = 6, .target = victim});
        else
            host->issue_order({.kind = 7, .point = host->motion.position});
    }
    spell.next_pulse += ticks;
    spell.remaining -= ticks;
    if (spell.remaining <= 0) {
        host->orders.clear();
        match.unwatch(*host, 1, spell.reference(), &spell);
        host->unname(hop_name(spell), &spell);
        return remove_spell(spell);
    }
    return 1;
}
int run_blind_rage(Match& match, SpellActor& spell, int ticks) {
    // BlindRageRun 0x424e60. age is the alpha (model scale 2 sqrt(age)), ability.length the ring's growth.
    // Each tick the goal is the target's body centre dropped to the ground (0x44E020 / 0x44ECB0).
    if (spell.effect_target && living_unit(spell.effect_target))
        spell_target_point(spell.point, spell.effect_target);
    if (match.terrain)
        spell.point.z = static_cast<float>(terrain_true_height(*match.terrain, spell.point.x, spell.point.y));
    if (!spell.stage) {
        spell.sound_event(match, 1);
        spell.set_projectile_model(match, FourCC::from_text("rage"), 1);
        if (spell.owner && spell.owner->source_motion()) {
            // Owner frame * (0, 2, 1): two ahead and one up.
            const auto& m = *spell.owner->source_motion();
            spell.motion.orientation = m.orientation;
            spell.motion.position = {m.position.x + m.orientation[3] * 2 + m.orientation[6],
                                     m.position.y + m.orientation[4] * 2 + m.orientation[7],
                                     m.position.z + m.orientation[5] * 2 + m.orientation[8]};
        }
        spell.motion.velocity = {};
        spell.ability.length = 0;
        spell.age = 0;
        if (spell.owner)
            SpellActor::launch_caster_column(
                match, spell, 0x1a012008,
                FourCC{static_cast<unsigned>(spell.data->integer(FourCC::from_text("tcst"), FourCC::from_text("swrl").value))},
                1, 0, 2, 2, 2);
    }
    const auto steer = [&](float accel, float slowdown, bool lift) {
        Vec3 goal = spell.point;
        if (lift)
            goal.z += 5;
        const float distance =
            spell_approach_velocity(spell.motion, goal, ticks * accel, spell.data->speed, 7, slowdown);
        spell.motion.position.x += spell.motion.velocity.x * ticks;
        spell.motion.position.y += spell.motion.velocity.y * ticks;
        spell.motion.position.z += spell.motion.velocity.z * ticks;
        if (spell.motion.velocity.x || spell.motion.velocity.y || spell.motion.velocity.z)
            spell.motion.orientation = spell_orientation(spell.motion.velocity);
        return distance;
    };
    const auto finish = [&] {
        spell.model_scale = 2 * std::sqrt(std::max(0.f, spell.age));
        spell.model_frame += ticks * .5f;
        return 1;
    };
    if (const auto waiting = wait_cast(match, spell)) {
        spell.age = spell.cast_alpha;
        finish();
        return waiting;
    }
    if (spell.stage == 16) {
        // 0x438050: 'rtrl' trail (emission 1, life 1, box .3, size .5); full alpha.
        SpellActor::launch_model_trail(match, spell, 0x1a012008, FourCC::from_text("rtrl"), 1, 1, .3f, .5f, true);
        spell.age = 1;
        spell.stage = 17;
    }
    if (spell.stage == 17) {
        if (steer(spell.data->acceleration, 5, true) <= 5)
            spell.stage = 32;
    } else if (spell.stage == 32 || spell.stage == 33) {
        if (spell.stage == 32) {
            spell.state_flags &= ~2u;
            spell.remaining = 90;
            spell.stage = 33;
        }
        steer(spell.data->acceleration, 5, true);
        if ((spell.remaining -= ticks) <= 0)
            spell.stage = 48;
    } else if (spell.stage == 48) {
        // Dive at triple acceleration; native waits for the land contact (+564 & 2).
        const bool landed =
            match.terrain && spell.motion.position.z <= terrain_surface(*match.terrain, spell.motion.position.x,
                                                                        spell.motion.position.y);
        if (steer(spell.data->acceleration * 3, 0, false) <= 1 || landed || (spell.state_flags & 2)) {
            spell.motion.velocity = {};
            spell.sound_event(match, 2);
            // 0x437850: 'trng' (default 'forb') ring over two seconds, radius 1 -> spread, with the
            // 'trg2' (default 'peri') band as its second material.
            auto& ring = SpellActor::launch_sacrifice_host(match, spell, SpellActor::Effect::ruin_ring);
            ring.motion.position = spell.motion.position;
            ring.host.initialized = true;
            ring.altar_ring.enabled = true;
            ring.altar_ring.texture = FourCC{static_cast<unsigned>(
                spell.data->integer(FourCC::from_text("trng"), FourCC::from_text("forb").value))};
            ring.altar_ring.second = FourCC{static_cast<unsigned>(
                spell.data->integer(FourCC::from_text("trg2"), FourCC::from_text("peri").value))};
            ring.altar_ring.radius = 1;
            ring.altar_ring.end_radius = .9f;
            ring.altar_ring.inner = .25f;
            ring.altar_ring.height = .5f;
            ring.remaining = ring.linger = 120;
            ring.host.rate = (spell.data->spread_limit - 1) / 120;
            spell.next_pulse = 0;
            spell.stage = 65;
        }
    }
    if (spell.stage == 65) {
        spell.age = std::max(0.f, spell.age - ticks / 30.f);
        spell.ability.length = std::min(1.f, spell.ability.length + ticks / 120.f);
        const float reach = spell.ability.length * spell.data->spread_limit;
        if ((spell.next_pulse -= ticks) <= 0) {
            spell.next_pulse = 4;
            // 0x4923E0 group 0 creatures within the growing ring, unless already raging (0x424940).
            match.visit_group(0, 4, [&](MatchObject& object) {
                auto* creature = dynamic_cast<CreatureActor*>(&object);
                if (!creature || !(creature->kind_flags & 4) || !creature->alive() ||
                    creature->named(hop_name(spell)) || !creature->source_motion() ||
                    distance_squared(spell.motion.position, creature->source_motion()->position) >
                        static_cast<double>(reach) * reach)
                    return 0;
                auto& rage = launch_child(match, spell, SpellActor::Effect::rage_creature, creature,
                                          creature->motion.position);
                rage.run(match, 1);
                return 0;
            });
            if (spell.ability.length >= 1)
                spell.stage = 240;
        }
    } else if (spell.stage >= 240) {
        spell.age -= ticks / 30.f;
        if (spell.age <= 0)
            return remove_spell(spell);
    }
    return finish();
}
int run_bovine(Match& match, SpellActor& spell, int ticks) {
    // BovineInterventionStart 0x4437A0.
    auto* target = living_unit(spell.effect_target);
    if (!spell.stage) {
        // 0x443590 changes this spell's speed to one; keep the registry's immutable definition intact.
        auto data = std::make_shared<SpellData>(*spell.data);
        data->speed = 1;
        spell.data = data;
        spell.set_projectile_model(match, FourCC::from_text("bovi"), 5);
        // A caster raises the cow 15 ahead of itself; without one it rises within 30 of the target point.
        // 0x453340 then looks within that distance for open land (up to 32 tries).
        Vec3 at = spell.point;
        float reach = 30;
        if (spell.owner && spell.owner->source_motion()) {
            const auto& m = *spell.owner->source_motion();
            at = {m.position.x + m.orientation[3] * 15, m.position.y + m.orientation[4] * 15, m.position.z};
            reach = 15;
        } else {
            at.x += match.random().next() * 60.f / 32768 - 30;
            at.y += match.random().next() * 60.f / 32768 - 30;
        }
        const auto open = [&](Vec3 point) {
            return !match.terrain || terrain_surface(*match.terrain, point.x, point.y) > 0;
        };
        const Vec3 centre = at;
        for (int tries = 32; !open(at) && tries-- > 0;) {
            const float heading = match.random().next() * 6.2831802f / 32768 - 3.1415901f;
            const float distance = match.random().next() * reach / 32768;
            at.x = centre.x + std::sin(heading) * distance;
            at.y = centre.y + std::cos(heading) * distance;
        }
        if (match.terrain)
            at.z = static_cast<float>(terrain_surface(*match.terrain, at.x, at.y));
        spell.motion.position = spell.ability.origin = at;
        spell.motion.position.z -= 5;
        if (spell.owner)
            SpellActor::launch_dirt_spray(match, spell, &spell, FourCC::from_text("dirt"), true, 5, 1, 0);
    }
    if (const auto waiting = wait_cast(match, spell, 16, 248)) {
        if (spell.stage == 1)
            match.shake(spell.motion.position, 16, spell.cast_alpha * 100);
        return waiting;
    }
    if (spell.stage == 16) {
        spell.motion.position = spell.ability.origin;
        spell.launch_projectile(match);
        spell.motion.velocity = {0, 0, spell.data->speed};
        spell.motion.flags = 5;
        spell.motion.collision_mask = 0;
        spell.remaining = 1200;
        spell.stage = 17;
        match.shake(spell.motion.position, 16, 100);
        // 0x4380F0's ground effects where the cow breaks out.
        SpellActor::launch_earth_burst(match, spell, spell.motion.position, 3, 24);
        match.scar_requests.push_back({FourCC{static_cast<unsigned>(spell.data->integer(
                                           FourCC::from_text("tRE3"), FourCC::from_text("tstr").value))},
                                       spell.motion.position, match.clock().match, ++match.terrain_revision});
        spell.sound_event(match, 2);
        return 1;
    }
    if (spell.stage == 17) {
        spell_target_point(spell.point, spell.effect_target);
        // Rising, the cow eases across along a (0, .1, .9, 1) Bezier of its spent climb speed; falling, it
        // is over the target.
        float progress = 1;
        if (spell.motion.velocity.z > 0) {
            const float t = std::clamp(1 - spell.motion.velocity.z / spell.data->speed, 0.f, 1.f), u = 1 - t;
            progress = 3 * u * u * t * .1f + 3 * u * t * t * .9f + t * t * t;
        }
        spell.motion.position.x =
            spell.ability.origin.x + (spell.point.x - spell.ability.origin.x) * progress;
        spell.motion.position.y =
            spell.ability.origin.y + (spell.point.y - spell.ability.origin.y) * progress;
        spell.model_frame = std::min(64.f, spell.model_frame + ticks * .25f);
        const float ground = match.terrain ? static_cast<float>(terrain_surface(
                                                 *match.terrain, spell.motion.position.x, spell.motion.position.y))
                                           : spell.motion.position.z;
        if (spell.motion.velocity.z <= 0 &&
            ((spell.state_flags & 3) || spell.motion.position.z <= spell.point.z))
            spell.stage = 32;
        else if (spell.motion.position.z + 25 < ground || (spell.remaining -= ticks) <= 0) {
            spell.sound_event(match, 16);
            return remove_spell(spell);
        } else
            return 1;
    }
    if (spell.stage == 32) {
        // Impact: sound 4, a 'bovs' scar, the 'tcld' splatter, 64 'rock' bits, eight 'roks' chunks and
        // sound 16; the cow model goes, then the 0x440ED0(80, 20, 3) land wave runs.
        spell.sound_event(match, 4);
        match.scar_requests.push_back({FourCC::from_text("bovs"), spell.motion.position, match.clock().match,
                                       ++match.terrain_revision});
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
            spell_mrm_debris(match, spell, 8, .25f, FourCC::from_text("roks"), 1, 0x0a011008,
                             FourCC::from_text("dirt"), spell.motion.position, 5);
        spell.sound_event(match, 16);
        if (target) {
            // vtable +256 morphs the victim into CRE8 "cow ". Lethal 0x45a7c0 still retires the original.
            if (match.create_creature) {
                try {
                    if (match.prepare_creature)
                        match.prepare_creature(FourCC::from_text("cow "));
                    match.create_creature(*target, FourCC::from_text("cow "), target->motion.position,
                                          target);
                } catch (const Error&) {
                }
            }
            target->kill(match);
            attach_hit(match, spell, target, target->motion.position);
        }
        spell.model_scale = 0;
        spell.motion.velocity = {};
        spell.motion.flags = 0;
        spell.motion.collision_mask = 0;
        spell.effect_target = nullptr;
        if (!match.terrain)
            return remove_spell(spell);
        spell.land_wave = Terrain::Displacement(spell.motion.position, 21);
        spell.remaining = 32;
        spell.rate = 0;
        spell.stage = 33;
        return 1;
    }
    if (spell.stage == 33) {
        spell.remaining = std::max(0, spell.remaining - ticks);
        spell.rate -= ticks;
        if (spell.rate <= 0 || !spell.remaining) {
            spell_erupt_wave(match, spell, 1.f - spell.remaining / 32.f);
            spell.rate = 6;
        }
        return spell.remaining ? 1 : remove_spell(spell);
    }
    if (spell.stage == 240)
        spell.sound_event(match, 16);
    return spell.stage >= 240 ? remove_spell(spell) : 1;
}
int run_cloud_kill(Match& match, SpellActor& spell, int ticks) {
    auto& cloud = spell.altar_cloud;
    if (!spell.stage) {
        SpellActor::launch_cast_hands(match, spell);
        spell.remaining = static_cast<int>(spell.data->duration * 60.f);
        spell.next_pulse = 0;
        spell.stage = 1;
        cloud.enabled = true;
        cloud.texture = FourCC{static_cast<unsigned>(
            spell.data->integer(FourCC::from_text("tcld"), FourCC::from_text("dirt").value))};
        cloud.noise = std::make_shared<CloudNoise>(match.visual_random);
        cloud.phase = spell.motion.position;
        aim_point(spell);
        spell.motion.position.z += 22;
        // 0x420850: the cloud's noise drifts along a normalized random direction at speed four.
        Vec3 drift{match.random().next() / 16384.f - 1, match.random().next() / 16384.f - 1,
                   match.random().next() / 16384.f - 1};
        const float length = std::sqrt(drift.x * drift.x + drift.y * drift.y + drift.z * drift.z);
        if (length > 0)
            spell.ability.destination = {drift.x * 4 / length, drift.y * 4 / length, drift.z * 4 / length};
    }
    if (spell.stage == 1) {
        if (const auto next = mid_cast(spell, 16, 240))
            spell.stage = static_cast<int>(next);
        else
            SpellActor::caster_sparks(match, spell, 8); // CloudKillRun 0x420A30 stage 1: 0x435B50(8).
    }
    if (spell.stage == 16) {
        Vec3 destination = spell.point;
        spell_target_point(destination, spell.effect_target);
        destination.z += 22;
        spell_approach_velocity(spell.motion, destination, ticks * spell.data->acceleration,
                                spell.data->speed);
        if (match.terrain) {
            const float floor = static_cast<float>(
                terrain_surface(*match.terrain, spell.motion.position.x, spell.motion.position.y));
            if (spell.motion.position.z - floor < 20 && spell.motion.velocity.z < 1.f / 30)
                spell.motion.velocity.z = 1.f / 30;
        }
        spell.motion.position.x += ticks * spell.motion.velocity.x;
        spell.motion.position.y += ticks * spell.motion.velocity.y;
        spell.motion.position.z += ticks * spell.motion.velocity.z;
        if (match.random().next() % 60 <= ticks) {
            // 0x4207c0 -> 0x44d430: nearby units on either side plus four random-ground slots.
            std::vector<CreatureActor*> candidates;
            const float range_squared = spell.data->spread_limit * spell.data->spread_limit;
            for (unsigned group : {0u, 1u})
                match.visit_group(group, 5, [&](MatchObject& object) {
                    if (auto* actor = living_unit(&object);
                        actor && candidates.size() < 256 &&
                        spell_box_distance_squared(spell.motion.position, actor->motion, range_squared) <=
                            range_squared)
                        candidates.push_back(actor);
                    return 0;
                });
            CreatureActor* victim = nullptr;
            Vec3 at = spell.motion.position;
            const auto selected = match.random().next() % (candidates.size() + 4);
            if (selected < candidates.size()) {
                victim = candidates[selected];
                spell_target_point(at, victim);
            } else {
                const float radius = match.random().next() * spell.data->spread_limit / 32768.f;
                const float angle = match.random().next() * 6.2831802f / 32768.f;
                at.x += std::sin(angle) * radius;
                at.y += std::cos(angle) * radius;
                if (match.terrain)
                    at.z = static_cast<float>(terrain_true_height(*match.terrain, at.x, at.y));
            }
            Vec3 from = spell.motion.position;
            auto& bolt = launch_child(match, spell, SpellActor::Effect::cloud_kill_bolt, victim, from);
            bolt.point = at;
            bolt.run(match, 1); // CloudKillBoltRun 0x420600 via 0x4207C0.
        }
        spell.remaining -= ticks;
        if (spell.remaining <= 0)
            spell.stage = 240;
    }
    const float opacity_target = spell.stage >= 240 ? 0.f : spell.stage == 1 ? spell.cast_alpha : 1.f;
    cloud.opacity += std::clamp(opacity_target - cloud.opacity, -ticks * .01f, ticks * .01f);
    cloud.radii = {cloud.opacity * 5, cloud.opacity * 5, cloud.opacity * 2.5f};
    cloud.phase.x += ticks * spell.ability.destination.x;
    cloud.phase.y += ticks * spell.ability.destination.y;
    cloud.phase.z += ticks * spell.ability.destination.z;
    cloud.frame += ticks * .25f;
    if (spell.stage >= 240 && cloud.opacity == 0)
        return remove_spell(spell);
    return 1;
}
int run_ivap(Match& match, SpellActor& spell, int ticks) {
    // IVapRun 0x431CE0.
    if (!spell.stage) {
        spell.sprite = {true,
                        FourCC{static_cast<unsigned>(
                            spell.data->integer(FourCC::from_text("ttex"), FourCC::from_text("gut1").value))},
                        0x0a011008, 0xffffffff, 1.75f};
        spell.sprite_overlay = {true,
                                FourCC{static_cast<unsigned>(spell.data->integer(
                                    FourCC::from_text("ttx2"), FourCC::from_text("iva1").value))},
                                0x1a012008, 0xffffffff, 1.75f};
        if (auto* caster = dynamic_cast<CreatureActor*>(spell.owner))
            caster->write_reference_point(spell.motion.position, 0, false);
        spell.sound_event(match, 1);
    }
    auto* target = living_unit(spell.effect_target);
    // From the strike on, losing the victim ends the spell at once.
    if (!target && spell.stage >= 32 && spell.stage < 240)
        return remove_spell(spell);
    const auto waiting = wait_cast(match, spell);
    spell.sprite.frame += ticks * .25f;
    spell.sprite_overlay.frame += ticks * .25f;
    if (waiting)
        return waiting;
    if (spell.stage == 16) {
        spell.stop_sound_event(1);
        spell.sound_event(match, 2);
        spell.launch_projectile(match);
        spell.motion.collision_mask = 0;
        spell.stage = 17;
    }
    if (spell.stage == 17) {
        if (!target)
            return remove_spell(spell);
        spell_lead_target(spell.point, target, spell.data->speed, spell.motion, *match.terrain);
        const float distance = spell_approach_velocity(
            spell.motion, spell.point, ticks * spell.data->acceleration, spell.data->speed, 7, 2);
        if (distance <= .5f) {
            // Stage 32: sound 4, the sprite goes, the victim is named and four guts circle inside it.
            spell.motion.flags = 0;
            spell.motion.velocity = {};
            spell.sprite.enabled = false;
            spell.sprite_overlay.enabled = false;
            spell.sound_event(match, 4);
            mark_hit(*target, spell);
            for (int n = 0; n < 4; ++n) {
                auto& sprite = launch_child(match, spell, SpellActor::Effect::ivap_sprite, target,
                                            target->motion.position);
                sprite.run(match, 0); // IVapSpriteRun 0x4319f0 via 0x44E850.
            }
            spell.remaining = static_cast<int>(spell.data->duration * 60);
            spell.stage = 33;
        }
    } else if (spell.stage == 33) {
        // 0x435BC0(12): one tick in five a random non-root bone swells (2, 1, 2); one swell in eight makes
        // the victim flinch (FSM event 7).
        if (match.random().next() < 12 * 546.1333333f && target->bone_count() > 1) {
            target->request_bone_scale(1 + match.random().next() % (target->bone_count() - 1), {2, 1, 2});
            if (!(match.random().next() & 7))
                target->fsm().event(7);
        }
        if ((spell.remaining -= ticks) <= 0) {
            spell.cast_alpha = 1;
            spell.stage = 49;
        }
    } else if (spell.stage == 49) {
        for (unsigned bone = 0; bone < target->bone_count(); ++bone)
            target->request_bone_scale(bone, {spell.cast_alpha, spell.cast_alpha, spell.cast_alpha});
        spell.cast_alpha += ticks * .01f;
        if (spell.cast_alpha >= 3) {
            // Stage 64: sound 16 and the victim's vfunc 256(1, 0, 0, 1) death.
            spell.sound_event(match, 16);
            target->kill(match);
            return remove_spell(spell);
        }
    } else if (spell.stage >= 240) {
        // An aborted cast stops sound 4 and fades the sprite over a second.
        spell.stop_sound_event(4);
        const float alpha = std::max(0.f, (spell.sprite.color >> 24) / 255.f - ticks / 60.f);
        spell.sprite.color = (static_cast<unsigned>(alpha * 255) << 24) | 0xffffff;
        spell.sprite_overlay.color = spell.sprite.color;
        if (alpha <= 0)
            return remove_spell(spell);
    }
    return 1;
}
int run_divine_pillar(Match& match, SpellActor& spell, int ticks) {
    // DivineRetributionRun 0x42e000. cast_alpha is the growth (+48) mirrored from `age`, easing toward `emission` (+68) at 1/180;
    // ability.destination is the tip (+88) driven by 0x42DCC0 toward `point` (+116) with motion.velocity;
    // ability.length is the tip's distance to its goal (+64), ability.draw_back / origin.x the spring
    // speed and acceleration (+72 / +76).
    auto& tip = spell.ability.destination;
    auto& tip_velocity = spell.motion.velocity;
    auto& goal = spell.point;
    auto& distance = spell.ability.length;
    float& speed = spell.ability.draw_back;
    float& acceleration = spell.ability.origin.x;
    auto& base = spell.motion.position;
    const auto ground_at = [&](Vec3 at) {
        return match.terrain ? static_cast<float>(terrain_surface(*match.terrain, at.x, at.y)) : at.z;
    };
    const auto above = [&] { return Vec3{base.x, base.y, base.z + 60}; };
    const auto set_spring = [&](float s, float a) { speed = s, acceleration = a; };
    // 0x42DD20: a unit not flagged 0x10000, alive, not yet named, 10..45 from the base (z halved).
    const auto reachable = [&](CreatureActor* actor) {
        if (!actor || (actor->motion.flags & 0x10000) || !actor->alive() || actor->named(hop_name(spell)))
            return false;
        const double dx = base.x - actor->motion.position.x, dy = base.y - actor->motion.position.y,
                     dz = (base.z - actor->motion.position.z) * .5;
        const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
        return d <= 45 && d >= 10;
    };
    auto* victim = living_unit(spell.effect_target);
    bool carry = false;
    CreatureActor* tracked = nullptr;
    if (!spell.stage) {
        // Setup: sunk 60 below the ground, textures, ECRK scar, sound 2 and a 'dirt' spray.
        base.z = ground_at(base) - 60;
        spell.cast_alpha = 0;
        spell.emission = 0;
        tip_velocity = {};
        set_spring(.1f, .01f);
        spell.effect_target = nullptr;
        spell.surface.kind = SpellActor::CurvedSurface::tentacle;
        spell.surface.opacity = 1;
        spell.surface.material = 0x00010001;
        spell.surface.textures[0] = FourCC{static_cast<unsigned>(
            spell.data->integer(FourCC::from_text("ttex"), FourCC::from_text("vine").value))};
        spell.surface.textures[1] = FourCC{static_cast<unsigned>(
            spell.data->integer(FourCC::from_text("ttx2"), FourCC::from_text("vine").value))};
        match.scar_requests.push_back({FourCC{static_cast<unsigned>(spell.data->integer(
                                           FourCC::from_text("tscr"), FourCC::from_text("ecrk").value))},
                                       base, match.clock().match, ++match.terrain_revision});
        spell.remaining = static_cast<int>(spell.data->duration * 60.f);
        spell.stage = 16;
        spell.sound_event(match, 2);
        SpellActor::launch_dirt_spray(match, spell, &spell, FourCC::from_text("dirt"), true, 5, .75f, 2);
    }
    switch (spell.stage) {
    case 16:
    case 24:
        if (spell.stage == 16) {
            // 0x441D20(24, 240): the stalk follows the cast; the new stage runs on the next call.
            const auto next = mid_cast(spell, 24, 240);
            spell.emission = spell.cast_alpha;
            if (next)
                spell.stage = static_cast<int>(next);
        } else {
            spell.emission = 1;
            if (spell.age >= 1)
                spell.stage = 32;
        }
        goal = above();
        tip = goal;
        break;
    case 32:
        spell.effect_target = nullptr;
        spell.age = 1;
        distance = 0;
        set_spring(.1f, .01f);
        spell.stage = 33;
        [[fallthrough]];
    case 33: {
        // 0x492230(5, base, 60, 0x42DF20): any unit of either side within reach becomes the target.
        CreatureActor* best = nullptr;
        const auto visit = [&](MatchObject& object) {
            if (auto* actor = living_unit(&object); !best && reachable(actor) &&
                                                    distance_squared(base, actor->motion.position) < 60.0 * 60)
                best = actor;
            return 0;
        };
        match.visit_group(0, 5, visit);
        match.visit_group(1, 5, visit);
        spell.effect_target = best;
        if (best)
            spell.stage = 48;
        else if (distance < 3) {
            // 0x42DE40: a new idle goal in a 45-degree cone, 30 out and 60 up at most.
            const float tilt = match.random().next() * 1.5707951f * .000030517578125f - .78539753f,
                        turn = match.random().next() * 6.2831802f * .000030517578125f, reach = std::sin(tilt) * 30;
            goal = {base.x + std::sin(turn) * reach, base.y + std::cos(turn) * reach, base.z + std::cos(tilt) * 60};
        }
        break;
    }
    case 48:
        spell.sound_event(match, 8);
        set_spring(.5f, .05f);
        distance = 50;
        spell.stage = 49;
        [[fallthrough]];
    case 49:
        tracked = victim;
        if (victim)
            spell_target_point(goal, victim);
        if (!reachable(victim))
            spell.stage = 32;
        else if (distance < .1f)
            spell.stage = 64;
        break;
    case 64:
        spell.sound_event(match, 4);
        spell_damage(match, &spell, spell.effect_target, spell.data->damage_type, spell.data->amount,
                     spell.data->radius);
        set_spring(.7f, .07f);
        if (victim)
            mark_hit(*victim, spell);
        goal = above();
        spell.stage = 80;
        distance = 50;
        [[fallthrough]];
    case 80:
        if (victim && victim->alive()) {
            carry = true;
            if (distance < 7)
                spell.stage = 96;
        } else
            spell.stage = 32;
        break;
    case 96:
        if (victim)
            victim->motion.velocity = tip_velocity; // Fling with the tip's velocity.
        spell.stage = 32;
        break;
    case 240:
        spell.sound_event(match, 16);
        spell.stage = 241;
        spell.emission = 0;
        spell.effect_target = nullptr;
        [[fallthrough]];
    case 241:
        if (spell.age <= 0)
            return remove_spell(spell);
        break;
    default:
        break;
    }
    // Common tail: snap the base to the ground, grow or count down, then move the tip.
    const float old_z = base.z;
    base.z = ground_at(base);
    if (spell.age == spell.emission && spell.age == 1) {
        if ((spell.remaining -= ticks) <= 0 && spell.stage < 240)
            spell.stage = 240;
    } else {
        const float step = ticks / 180.f;
        spell.age = spell.age < spell.emission ? std::min(spell.emission, spell.age + step)
                                           : std::max(spell.emission, spell.age - step);
        spell.effect_target = nullptr;
        base.z -= (1 - spell.age) * 60;
        goal = above();
    }
    tip.z += base.z - old_z;
    // 0x42DCC0: 0x44D620 steering (slowdown radius 3), then the tip integrates its velocity.
    ObjectMotion spring;
    spring.position = tip;
    spring.velocity = tip_velocity;
    distance = spell_approach_velocity(spring, goal, ticks * acceleration, speed, 7, 3);
    tip_velocity = spring.velocity;
    tip = {tip.x + ticks * tip_velocity.x, tip.y + ticks * tip_velocity.y, tip.z + ticks * tip_velocity.z};
    if (tracked && tracked->source_motion()) {
        // 0x401A70: while reaching, measure to the target's box rather than its centre.
        distance = static_cast<float>(std::sqrt(spell_box_distance_squared(tip, tracked->motion, 100)));
    }
    if (carry && victim) {
        // The held unit's body centre rides the tip.
        Vec3 centre;
        spell_target_point(centre, victim);
        victim->motion.position = {victim->motion.position.x - centre.x + tip.x,
                                   victim->motion.position.y - centre.y + tip.y,
                                   victim->motion.position.z - centre.z + tip.z};
        victim->motion.velocity = tip_velocity;
    }
    if (spell.stage > 32 && spell.stage < 240 && (spell.remaining -= ticks) <= 0)
        spell.stage = 240;
    spell.cast_alpha = spell.age;
    // 0x42d9e0 / TENTACLE: two joined cubic sections and the native
    // radius controls (2,2,2,1.8), (1.8,1.6,1,0), rather than a lightning line.
    const Vec3 root = base;
    const Vec3 mid{(root.x + tip.x) * .5f, (root.y + tip.y) * .5f, (root.z + tip.z) * .5f};
    const float dx = tip.x - root.x, dy = tip.y - root.y, length = std::hypot(dx, dy);
    const Vec3 right = length > .001f ? Vec3{-dy / length, dx / length, 0} : Vec3{1, 0, 0};
    auto& lower = spell.surface.tentacles[0];
    auto& upper = spell.surface.tentacles[1];
    lower.curve = {
        root, {root.x, root.y, root.z + 10}, {mid.x - right.x * 10, mid.y - right.y * 10, mid.z}, mid};
    lower.radii = {2, 2, 2, 1.8f};
    upper.curve = {mid,
                   {mid.x + right.x * 10, mid.y + right.y * 10, mid.z},
                   {tip.x + (mid.x + right.x * 10 - tip.x) * .3f,
                    tip.y + (mid.y + right.y * 10 - tip.y) * .3f, tip.z + (mid.z - tip.z) * .7f},
                   tip};
    upper.radii = {1.8f, 1.6f, 1, 0};
    return 1;
}
int run_divine_retribution(Match& match, SpellActor& spell) {
    // DivineRetributionStart 0x42d840: 0x44C0D0(16, 32) spawns at once; the stalks follow the cast.
    if (!spell.stage) {
        spell.stage = spell.owner ? 16 : 32;
        return 1;
    }
    if (spell.stage == 16)
        SpellActor::launch_cast_hands(match, spell);
    aim_point(spell);
    const int pillars = std::clamp(spell.data->count, 0, 64);
    for (int n = 0; n < pillars; ++n) {
        const double angle =
            (n + .5) * 6.28318 / pillars + match.random().next() * (.78539753 / 32768) - .39269876;
        const double radius = 21.6 - match.random().next() * (12.6 / 32768);
        Vec3 at{spell.motion.position.x + static_cast<float>(std::sin(angle) * radius),
                spell.motion.position.y + static_cast<float>(std::cos(angle) * radius), spell.motion.position.z};
        if (match.terrain && match.spatial && !open_point(match, at, at, 18, 21))
            continue;
        auto& pillar = launch_child(match, spell, SpellActor::Effect::divine_pillar, nullptr, at);
        pillar.run(match, 1); // DivineRetributionRun 0x42e000 via 0x44E850.
    }
    return remove_spell(spell);
}
int run_volcano_visual(Match& match, SpellActor& spell, int ticks) {
    const bool smoke = spell.effect == SpellActor::Effect::volcano_smoke;
    auto& surface = spell.surface;
    if (!spell.stage) {
        surface.kind = smoke ? SpellActor::CurvedSurface::cylinder : SpellActor::CurvedSurface::ring;
        surface.material = smoke ? 0x0a011008 : 0x1a012008;
        surface.textures[0] = FourCC{static_cast<unsigned>(spell.data->integer(
            FourCC::from_text(smoke ? "tsmk" : "tspl"), FourCC::from_text(smoke ? "vsmk" : "sptr").value))};
        if (smoke) {
            // VolcanoSmokeRun / BEZIERCYLINDER, native profile at 0x4cb3f0.
            const float profile[]{0, 0,   318, 0,   0,   293, 0,   0,   325, 0,
                                  0, 268, 246, 190, 137, 52,  255, 255, 255, 255};
            std::copy(std::begin(profile), std::end(profile), surface.parameters.begin());
        } else {
            auto& p = spell.particles(match);
            spell.particle_material(match, p, 0x1a012008,
                                    FourCC{static_cast<unsigned>(spell.data->integer(
                                        FourCC::from_text("tspt"), FourCC::from_text("sprt").value))});
            const float size = .5f, life = 2;
            p.set_property(131, &size, &life);
            const float low[]{-.1f, -.1f, .15f}, high[]{.1f, .1f, .4f}, gravity[]{0, 0, -.002725f};
            p.set_property(3, low, high);
            p.set_property(4, gravity, gravity);
            const float bounds_low[]{-12, -12, 0}, bounds_high[]{12, 12, 2};
            p.set_property(2, bounds_low, bounds_high);
        }
        spell.stage = 1;
    }
    auto* parent = dynamic_cast<SpellActor*>(spell.effect_target);
    if (!parent || parent->stage >= 240 || (parent->state_flags & MatchObject::removed_flag))
        spell.stage = 240;
    if (parent)
        spell.motion.position = parent->motion.position;
    spell.age = spell.stage >= 240 ? std::max(0.f, spell.age - ticks / 120.f)
                                   : std::min(1.f, spell.age + ticks / 120.f);
    surface.opacity = spell.age;
    surface.frame += ticks * (smoke ? .13333334f : .1f);
    if (!smoke) {
        // VolcanoSpatterRun: animate the four cubic radius/height/alpha profiles at 0x4cb480.
        static constexpr float profiles[4][12]{{9, 0, 255, 9, 0, 255, 9, 0, 255, 9, 0, 0},
                                               {10, 4, 255, 13, 0, 255, 17, 0, 255, 14, 0, 0},
                                               {12, 4, 255, 14, 10, 255, 18, 6, 255, 18, 0, 0},
                                               {13, 0, 255, 17, 0, 255, 20, 0, 255, 23, 0, 0}};
        const float t = std::fmod(surface.frame / 8, 1.f), u = 1 - t;
        for (unsigned i = 0; i < 12; ++i)
            surface.parameters[i] = u * u * u * profiles[0][i] + 3 * u * u * t * profiles[1][i] +
                                    3 * u * t * t * profiles[2][i] + t * t * t * profiles[3][i];
        auto& p = spell.particles(match);
        p.set_position(spell.motion.position);
        spell.host.time += ticks;
        p.advance(spell.host.time, spell.stage >= 240 ? 0.f : .5f);
        if (spell.stage >= 240 && p.alive())
            return 1;
    }
    return spell.stage >= 240 && spell.age == 0 ? remove_spell(spell) : 1;
}
int run_volcano_erupt(Match& match, SpellActor& spell, int ticks) {
    // VolcanoEruptRun 0x42a260: remaining = duration * 60, 0x429C30 lava, 0x44C230 smoke.
    if (match.terrain)
        spell.motion.position.z = static_cast<float>(
            terrain_surface(*match.terrain, spell.motion.position.x, spell.motion.position.y));
    if (!spell.stage) {
        spell.remaining = spell.data ? static_cast<int>(spell.data->duration * 60.f) : 3600;
        spell.next_pulse = 60;
        auto& smoke =
            launch_child(match, spell, SpellActor::Effect::volcano_smoke, &spell, spell.motion.position);
        smoke.remaining = spell.remaining;
        smoke.run(match, 0);
        // VolcanoEruptRun / BEZIERCYLINDER, native profile at 0x4cb430.
        spell.surface.kind = SpellActor::CurvedSurface::cylinder;
        spell.surface.material = 0x10010000;
        spell.surface.textures[0] = FourCC{static_cast<unsigned>(
            spell.data->integer(FourCC::from_text("tspt"), FourCC::from_text("sprt").value))};
        const float profile[]{0, 0, 310, 0, 0, 262, 0, 0, 265, 0, 0, -5, 140, 2, 20, 10, 255, 255, 255, 255};
        std::copy(std::begin(profile), std::end(profile), spell.surface.parameters.begin());
        spell.light = {true, spell.motion.position, {1, .5f, 0}, 0, 20, 200}; // dword_4CB8E0.
        spell.sound_event(match, 2);
        spell.stage = 1;
    }
    if (spell.stage == 1) {
        // 0x42A140 over mask 21 within 50: full damage to 20, fading to none at 50; living units are
        // also pushed out at falloff / 12 and set burning (0x45CB90(falloff, 10)).
        const auto burn = [&](MatchObject& object) {
            const auto* motion = object.source_motion();
            if (!motion || (object.state_flags & MatchObject::removed_flag))
                return 0;
            const float dx = motion->position.x - spell.motion.position.x,
                        dy = motion->position.y - spell.motion.position.y;
            const float distance = std::sqrt(dx * dx + dy * dy);
            if (distance >= 50)
                return 0;
            const float falloff = distance > 20 ? 1 - (distance - 20) / 30 : 1;
            if (auto* actor = living_unit(&object)) {
                actor->accumulate_damage({spell.data->damage_type, static_cast<int>(spell.data->amount * falloff),
                                          spell.reference()},
                                         ticks);
                if (distance > 0) {
                    actor->motion.velocity.x += dx / distance * falloff / 12;
                    actor->motion.velocity.y += dy / distance * falloff / 12;
                }
                actor->burn_near(actor->motion.position, falloff, 10);
            }
            return 0;
        };
        match.visit_group(0, 5, burn);
        match.visit_group(1, 5, burn);
    }
    if (spell.stage == 1 && (match.random().next() & 63) <= ticks) {
        std::vector<CreatureActor*> targets(4, nullptr);
        const auto collect = [&](MatchObject& object) {
            if (auto* actor = living_unit(&object);
                actor && distance_squared(actor->motion.position, spell.motion.position) <=
                             spell.data->spread_limit * spell.data->spread_limit)
                targets.push_back(actor);
            return 0;
        };
        match.visit_group(0, 5, collect);
        match.visit_group(1, 5, collect);
        auto* victim = targets[match.random().next() % targets.size()];
        Vec3 at;
        if (victim)
            spell_target_point(at, victim);
        else {
            const float angle = match.random().next() * (6.28318f / 32768);
            const float radius = std::sqrt(match.random().next() / 32768.f) * spell.data->spread_limit;
            at = {spell.motion.position.x + std::sin(angle) * radius,
                  spell.motion.position.y + std::cos(angle) * radius, spell.motion.position.z};
            if (match.terrain)
                at.z = static_cast<float>(terrain_surface(*match.terrain, at.x, at.y));
        }
        auto& lava =
            launch_child(match, spell, SpellActor::Effect::volcano_lava, victim, spell.motion.position);
        lava.motion.position.z += 10 + match.random().next() * (212.f / 32768);
        lava.point = at;
        lava.run(match, 1);
    }
    if (spell.stage == 1 && (spell.remaining -= ticks) <= 0) {
        spell.stage = 240;
        spell.stop_sound_event(2);
        spell.sound_event(match, 16);
    }
    spell.age = spell.stage >= 240 ? std::max(0.f, spell.age - ticks / 60.f)
                                   : std::min(1.f, spell.age + ticks / 60.f);
    spell.surface.opacity = spell.age;
    // Frames advance .25 a tick and wrap at 'infr' (default 8); the light flickers between 'fint'
    // (default .8) and 1 of its strength.
    const int frames = spell.data->integer(FourCC::from_text("infr"), 8);
    spell.surface.frame += ticks * .25f;
    if (frames > 0 && spell.surface.frame >= frames)
        spell.surface.frame = 0;
    const float flicker = spell.data->real(FourCC::from_text("fint"), .8f);
    spell.light.intensity = 20 * spell.age * (flicker + match.visual_random.next() * (1 - flicker) / 32768);
    spell.light.position = spell.motion.position;
    if (spell.stage >= 240 && spell.age == 0)
        return remove_spell(spell);
    return 1;
}
int run_volcano(Match& match, SpellActor& spell, int ticks) {
    if (!spell.stage) {
        spell.motion.position = spell.point;
        spell.start_mound(match, FourCC::from_text("volc"), 33, 80.f, 164.f);
        spell.sound_event(match, 1);
        if (!spell.owner) // 0x44C140: without a caster the mound rises over two seconds instead.
            spell.stage = 2;
    }
    if (spell.stage == 2) {
        spell.age = std::min(1.f, spell.age + ticks / 120.f);
        if (spell.age >= 1)
            spell.stage = 16;
    }
    const bool waiting = wait_cast(match, spell) != 0;
    const bool moving_land = !spell.land_mound.samples.empty();
    if (waiting) {
        spell.age = std::clamp(spell.cast_alpha, 0.f, 1.f);
    } else if (spell.stage == 16) {
        spell.stop_sound_event(1);
        spell.age = 1.f;
        if (moving_land) {
            match.displace(spell.land_mound, spell.age);
            spell.motion.position.z = static_cast<float>(
                terrain_surface(*match.terrain, spell.motion.position.x, spell.motion.position.y));
        } else {
            aim_point(spell);
        }
        spell_damage(match, &spell, nullptr, spell.data->damage_type, spell.data->amount, 50.f,
                     spell.motion.position);
        spell_exclusion_field(match, spell.motion.position, 10.f, 50.f, 1.f / 3.f, 7, 5);
        match.dent(spell.motion.position, 25.f, 10.f); // 0x44d0b0(pos, 25, 10, 1).
        // 0x4523E0: weather 5 for 140 s (10 s fade) and rain for 1.2 x the duration (5 s fade).
        match.weather.set(5, 1, 10, 140, false);
        match.weather.set(4, 1, 5, spell.data->duration * 1.2f, false);
        SpellActor::launch_shockwave(match, spell, 50.f);
        attach_hit(match, spell, nullptr, spell.motion.position);
        auto& erupt =
            launch_child(match, spell, SpellActor::Effect::volcano_erupt, nullptr, spell.motion.position);
        erupt.next_pulse = 60;
        erupt.run(match, 1);
        auto& spatter =
            launch_child(match, spell, SpellActor::Effect::volcano_spatter, &erupt, spell.motion.position);
        spatter.remaining = static_cast<int>(spell.data->duration * 60.f);
        spatter.run(match, 0);
        if (!moving_land)
            return remove_spell(spell);
        spell.stage = 32;
    } else if (spell.stage == 32) {
        // 0x42a5f0 / 0x42a710 lower the mound over the eruption duration, leaving a quarter behind.
        spell.age = std::max(.25f, spell.age - .75f * ticks / std::max(1.f, spell.data->duration * 60.f));
    } else if (spell.stage == 240) {
        match.displace(spell.land_mound, 0);
        return remove_spell(spell);
    }
    const bool finished = spell.stage == 32 && spell.age == .25f;
    spell.next_pulse -= ticks;
    if (spell.next_pulse <= 0 || finished) {
        match.displace(spell.land_mound, spell.age);
        spell.next_pulse = 8;
    }
    if (finished) {
        spell.land_mound.samples.clear();
        return remove_spell(spell);
    }
    if (moving_land) {
        match.shake(spell.motion.position, static_cast<int>(std::sqrt(spell.age) * 16), 300.f);
        if (match.navigation) {
            constexpr std::array footprint{30.f, 100.f};
            match.navigation->stamp(3, spell.motion.position, footprint, 128);
        }
    }
    return 1;
}
bool bore_step(Match& match, SpellActor& spell, bool remove) {
    const float radius = std::max(0.f, spell.data->spread_limit - 5), end = radius * .628318f;
    // 0x442E60: the spiral angle (kept in age, since the cast wait owns cast_alpha) stops at its end.
    if (spell.age > end)
        spell.age = end;
    const float progress = end > 0 ? std::min(1.f, spell.age / end) : 1;
    const float distance = 5 + progress * radius;
    spell.motion.position = {spell.point.x + std::sin(spell.age) * distance,
                             spell.point.y + std::cos(spell.age) * distance, spell.point.z};
    if (match.terrain)
        spell.motion.position.z = static_cast<float>(
            terrain_surface(*match.terrain, spell.motion.position.x, spell.motion.position.y));
    if (remove) {
        const int x = static_cast<int>((spell.motion.position.x + 5) * .1f),
                  y = static_cast<int>((spell.motion.position.y + 5) * .1f);
        if (match.set_edge(x, y, 1, 1)) {
            spell.terrain_cells.push_back({static_cast<float>(x), static_cast<float>(y), 0});
            // 0x464BD0: LANDBLOCK::Create for the removed sample; only a block that draws falls away.
            // LANDBLOCK::Create places the block on its land sample; its geometry is relative to it.
            const Vec3 sample{static_cast<float>(x * 10), static_cast<float>(y * 10),
                              match.terrain ? match.terrain->at(static_cast<unsigned>(x), static_cast<unsigned>(y))
                                            : spell.motion.position.z};
            auto& block = launch_child(match, spell, SpellActor::Effect::removed_edge, nullptr, sample);
            if (!match.terrain || !block.build_land_block(*match.terrain, x, y))
                remove_spell(block);
        }
    }
    spell.age += spell.data->speed * (remove ? 1 : .9f) / distance;
    return progress >= 1;
}
int run_bore_land(Match& match, SpellActor& spell, int ticks) {
    if (!spell.stage) {
        spell.age = 0;
        spell.stage = 16;
    }
    if (spell.stage == 16) {
        for (int n = 0; n < ticks; ++n)
            if (bore_step(match, spell, true)) {
                spell.remaining = 180;
                spell.stage = 81;
                break;
            }
    } else if (spell.stage == 81 && (spell.remaining -= ticks) <= 0) {
        spell.stage = 96;
    } else if (spell.stage == 96 && (spell.remaining -= ticks) <= 0) {
        // 0x464D10 restores one removed LAND tile at a time, in reverse order, thirty ticks apart.
        if (spell.terrain_cells.empty())
            return remove_spell(spell);
        const Vec3 cell = spell.terrain_cells.back();
        spell.terrain_cells.pop_back();
        match.set_edge(static_cast<int>(cell.x), static_cast<int>(cell.y), 0, 1);
        spell.motion.position = {cell.x * 10, cell.y * 10, spell.point.z};
        match.shake(spell.motion.position, 16, 75);
        spell.sound_event(match, 8);
        spell.remaining = 30;
    }
    return 1;
}
int run_bore(Match& match, SpellActor& spell, int ticks) {
    if (!spell.stage) {
        SpellActor::launch_cast_hands(match, spell);
        spell.motion.position = spell.point;
        spell.remaining = 600;
        spell.age = 0;
        spell.set_projectile_model(match,
                                   FourCC{static_cast<unsigned>(spell.data->integer(
                                       FourCC::from_text("tmod"), FourCC::from_text("rock").value))},
                                   1);
        SpellActor::launch_ground_trail(match, spell, 0x0a011008,
                                        FourCC{static_cast<unsigned>(spell.data->integer(
                                            FourCC::from_text("ttrl"), FourCC::from_text("dirt").value))},
                                        {-.75f, -.75f, 0}, {.75f, .75f, .25f}, .75f, 2);
        SpellActor::launch_generic_trail(match, spell, 0x0a011008,
                                         FourCC{static_cast<unsigned>(spell.data->integer(
                                             FourCC::from_text("tbit"), FourCC::from_text("rock").value))},
                                         .15f, .25f, 1, .15f, .78539753f);
        spell.stage = 1;
        spell.sound_event(match, 1);
    }
    // BoreRun 0x443280: the spiral (0x443190) already runs during the cast, and the 600 ticks before the
    // land drops count from the start.
    if (spell.stage == 1) {
        if (const auto next = mid_cast(spell, 16, 240))
            spell.stage = static_cast<int>(next);
    }
    if (spell.stage == 16 && spell.remaining <= 0) {
        auto& land = launch_child(match, spell, SpellActor::Effect::bore_land, nullptr, spell.point);
        land.run(match, 0);
        spell.stage = 33;
    }
    if (spell.stage == 1 || spell.stage == 16 || spell.stage == 33) {
        spell.remaining -= ticks;
        for (int n = 0; n < ticks; ++n)
            if (bore_step(match, spell, false) && spell.stage == 33)
                spell.stage = 240;
        match.shake(spell.motion.position, 16, 100);
        if (distance_squared(spell.motion.position, spell.ability.origin) > 25) {
            spell.ability.origin = spell.motion.position;
            match.scar_requests.push_back(
                {FourCC{static_cast<unsigned>(
                     spell.data->integer(FourCC::from_text("tscr"), FourCC::from_text("quk1").value))},
                 spell.motion.position, match.clock().match, ++match.terrain_revision});
        }
    }
    if (spell.stage >= 240) {
        spell.stop_sound_event(1);
        return remove_spell(spell);
    }
    return 1;
}
int run_tornado(Match& match, SpellActor& spell, int ticks) {
    if (!spell.stage) {
        auto& cast =
            launch_child(match, spell, SpellActor::Effect::tornado_cast, &spell, spell.motion.position);
        cast.run(match, 0);
    }
    if (const auto waiting = wait_cast(match, spell))
        return waiting;
    if (spell.stage == 16) {
        aim_point(spell);
        for (unsigned n = 9; n < 13; ++n)
            spell.surface.parameters[n] = (match.random().next() - 16384) * .0001917474365f;
        spell.remaining = static_cast<int>(spell.data->duration * 60.f);
        spell.next_pulse = 16;
        auto& ground =
            launch_child(match, spell, SpellActor::Effect::tornado_ground, &spell, spell.motion.position);
        ground.remaining = spell.remaining;
        ground.run(match, 0);
        spell.stage = 17;
        spell.ability.origin = spell.motion.position;
        spell.ability.destination = spell.motion.position;
        spell.launch_projectile(match);
        spell.motion.collision_mask = 0;
    }
    if (spell.stage == 17) {
        spell.update_tornado_surface(ticks);
        const float dx = spell.motion.position.x - spell.ability.destination.x,
                    dy = spell.motion.position.y - spell.ability.destination.y;
        if (dx * dx + dy * dy <= 2) {
            const float reach = spell.data->spread_limit;
            spell.ability.destination = {
                spell.ability.origin.x + (match.random().next() / 16384.f - 1) * reach,
                spell.ability.origin.y + (match.random().next() / 16384.f - 1) * reach,
                spell.motion.position.z};
        }
        spell_approach_velocity(spell.motion, spell.ability.destination, ticks * spell.data->acceleration,
                                spell.data->speed, 3);
        if (match.terrain) {
            const float height = static_cast<float>(
                terrain_surface(*match.terrain, spell.motion.position.x, spell.motion.position.y));
            if (height > 0)
                spell.motion.position.z = height;
        }
        // 0x423FC0: TORNADO::GetCentre at the actor's height supplies the funnel's bent centre
        // and radius; units within radius + 30 orbit at radius + 15 and lift +2.
        for (unsigned group = 0; group < 2; ++group)
            match.visit_group(group, 5, [&](MatchObject& object) {
                auto* actor = living_unit(&object);
                if (!actor || (actor->motion.flags & 0x10000))
                    return 0;
                Vec2 centre{};
                float radius = 0;
                spell.tornado_centre(actor->motion.position.z, centre, radius);
                const float x = centre.x - actor->motion.position.x,
                            y = centre.y - actor->motion.position.y;
                const float distance = std::sqrt(x * x + y * y);
                if (distance <= radius + 30) {
                    // 0x424100: 0x410bc0(&unk_4DCA90, 286.47913f) rotates the inward vector by
                    // 286.47913 radians (~214 deg), placing the target 44 deg ahead on the radius + 15 circle.
                    const float c = std::cos(286.47913f), s = std::sin(286.47913f),
                                factor = distance > 0 ? (radius + 15.f) / distance : 0.f;
                    const Vec3 target{centre.x + (x * c - y * s) * factor,
                                      centre.y + (x * s + y * c) * factor,
                                      actor->motion.position.z + 2};
                    spell_approach_velocity(actor->motion, target, ticks * .05f, .5f);
                }
                return 0;
            });
        spell.remaining -= ticks;
        if (spell.remaining <= 0)
            spell.stage = 240;
    }
    if (spell.surface.opacity > .8f && match.navigation) {
        // 0x424100: a strong funnel stamps the AI cost disc (128 to 10, fading out to 50).
        constexpr std::array footprint{10.f, 50.f};
        match.navigation->stamp(3, spell.motion.position, footprint, 128);
    }
    if (spell.stage >= 240) {
        spell.motion.flags = 0;
        spell.motion.velocity = {};
        spell.update_tornado_surface(ticks);
        if (spell.surface.opacity == 0)
            return remove_spell(spell);
    }
    return 1;
}
int run_death(Match& match, SpellActor& spell, int ticks) {
    if (!spell.stage) {
        spell.set_projectile_model(match, FourCC::from_text("deth"), 1);
        aim_point(spell);
        spell.motion.position.x += 5;
        spell.motion.position.y += 5;
        spell.remaining = static_cast<int>(spell.data->duration * 60);
        spell.rate = spell.data->count;
        spell.cast_alpha = 0;
        spell.sound_event(match, 2);
        auto& cloud = spell.altar_cloud;
        cloud.enabled = true;
        // 0x42FB30 launches DeathPortalEffect 0x42F8E0 (setup 0x42F840): a 'tcld' cloud, additive.
        cloud.texture = FourCC{static_cast<unsigned>(
            spell.data->integer(FourCC::from_text("tcld"), FourCC::from_text("rif3").value))};
        cloud.material = 0x02011008;
        if (!match.cloud_noise)
            match.cloud_noise = std::make_shared<CloudNoise>(match.visual_random);
        cloud.noise = match.cloud_noise;
    }
    wait_cast(match, spell, 32, 240);
    if (spell.stage == 1 && !spell.owner) {
        spell.cast_alpha = std::min(1.f, spell.cast_alpha + ticks / 60.f);
        if (spell.cast_alpha == 1)
            spell.stage = 32;
    }
    if (spell.stage == 32) {
        spell.launch_projectile(match);
        spell.motion.collision_mask = 0;
        spell.cast_alpha = 1;
        spell.stage = 33;
    }
    if (spell.stage < 240 && ((spell.remaining -= ticks) <= 0 || spell.rate <= 0))
        spell.stage = 240;
    if (spell.stage == 33) {
        auto* target = living_unit(spell.effect_target);
        if (!target || target->named(hop_name(spell))) {
            target = nearest_unit(match, spell.motion.position, spell.data->spread_limit,
                                  (spell.owner ? spell.owner : &spell), true, &spell);
            spell.effect_target = target;
        }
        if (target) {
            spell.motion.flags |= 1;
            spell_target_point(spell.point, target);
            spell.point.z = target->motion.position.z;
            const float distance = spell_approach_velocity(
                spell.motion, spell.point, ticks * spell.data->acceleration, spell.data->speed, 7, 3);
            spell.motion.orientation = spell_orientation(
                {spell.point.x - spell.motion.position.x, spell.point.y - spell.motion.position.y, 0});
            if (distance <= 2) {
                spell.motion.flags = 0;
                spell.motion.velocity = {};
                spell.next_pulse = 74; // Attack morph reaches time 37 at half speed.
                spell.ability.clock = 2;
                spell.host.time = 0;
                spell.host.rate = spell.model_frame;
                spell.sound_event(match, 8);
                spell.stage = 49;
            }
        } else {
            // Remain available for a new target until the spell expires.
            spell.motion.flags = 0;
            spell.motion.velocity = {};
        }
    } else if (spell.stage == 49) {
        if ((spell.next_pulse -= ticks) <= 0) {
            if (auto* target = living_unit(spell.effect_target)) {
                // Stage 64: sound 4, 0x436390(5, victim, 15, 4): five 'ltng' arcs (0x4363D0 -> 0x4355E0,
                // deviation .2, .25 s, one or two strands, width .3) from random bones to points up to 15
                // away, then the name and 0x45A7C0 death.
                spell.sound_event(match, 4);
                for (int n = 0; n < 5 && target->bone_count(); ++n) {
                    Vec3 from = target->motion.position;
                    target->write_bone_point(from, match.random().next() % target->bone_count(), &match.random());
                    Vec3 to = from;
                    const float reach = 15;
                    const float ground =
                        match.terrain ? static_cast<float>(terrain_surface(*match.terrain, to.x, to.y)) : 0;
                    const auto pick = [&](float low, float high) {
                        return match.random().next() * (high - low) * .000030517578125f + low;
                    };
                    if ((match.random().next() & 1) || reach * .5f < to.z - ground) {
                        const float angle = match.random().next() * 6.2831802f * .000030517578125f - 3.14159f;
                        const float distance = pick(reach * .3f, reach);
                        to.x += std::sin(angle) * distance;
                        to.y += std::cos(angle) * distance;
                        to.z += pick(-reach * .3f, reach);
                        if (match.terrain)
                            to.z = std::max(to.z, static_cast<float>(terrain_surface(*match.terrain, to.x, to.y)));
                    } else {
                        to.x += pick(-reach * .3f, reach * .3f);
                        to.y += pick(-reach * .3f, reach * .3f);
                        if (match.terrain)
                            to.z = static_cast<float>(terrain_surface(*match.terrain, to.x, to.y));
                    }
                    SpellActor::launch_inert_lightning(match, spell, from, to, nullptr, .2f, .25f, 0,
                                                       (match.random().next() & 1) + 1, .3f, 4);
                }
                mark_hit(*target, spell);
                target->kill(match);
                spell.remaining += 120;
                --spell.rate;
            }
            spell.effect_target = nullptr;
            spell.next_pulse = 36;
            spell.stage = 65;
        }
    } else if (spell.stage == 65 && (spell.next_pulse -= ticks) <= 0) {
        spell.ability.clock = 1;
        spell.host.time = 0;
        spell.stage = 33;
    }
    if (match.navigation && (spell.stage == 33 || spell.stage == 49 || spell.stage == 65))
        match.navigation->stamp(3, spell.motion.position, std::array{15.f, 30.f}, 64); // 0x42FC90 danger.
    if (spell.stage >= 240) {
        if (spell.ability.clock != 3) {
            spell.ability.clock = 3;
            spell.host.time = 0;
            spell.host.rate = spell.model_frame;
        }
        spell.cast_alpha = std::max(0.f, spell.cast_alpha - ticks / 90.f);
        spell.motion.flags = 0;
        spell.motion.velocity = {};
    }
    // Native MORPHANIM tables: appearance 0..5, flight 5..9, attack 10..25, disappearance 4..0.
    spell.host.time += ticks * .5f;
    const float t = spell.host.time;
    if (spell.ability.clock == 0) {
        spell.model_frame = std::min(5.f, t / 6);
        if (t >= 30) {
            spell.ability.clock = 1;
            spell.host.time = 0;
        }
    } else if (spell.ability.clock == 1) {
        const float f = std::fmod(t, 30.f) / 6;
        spell.model_frame = f <= 4 ? 5 + f : 9 - (f - 4) * 4;
    } else if (spell.ability.clock == 2) {
        static constexpr std::pair<float, float> frames[]{
            {0, -1},  {3, 10},  {5, 11},  {10, 12}, {13, 13}, {15, 14}, {17, 15}, {19, 16}, {22, 17},
            {25, 18}, {30, 19}, {35, 20}, {37, 21}, {38, 22}, {40, 23}, {45, 24}, {50, 25}, {55, 5}};
        spell.model_frame = 5;
        for (unsigned i = 1; i < std::size(frames); ++i)
            if (t < frames[i].first) {
                const float a = i == 1 ? spell.host.rate : frames[i - 1].second;
                const float u = (t - frames[i - 1].first) / (frames[i].first - frames[i - 1].first);
                spell.model_frame = a + (frames[i].second - a) * u;
                break;
            }
    } else {
        spell.model_frame =
            t < 6 ? spell.host.rate + (4 - spell.host.rate) * t / 6 : std::max(0.f, 5 - t / 6);
    }
    auto& cloud = spell.altar_cloud;
    spell.age = std::fmod(spell.age + ticks * .026179917f, 3.14159f);
    // 0x42F8E0: the portal swells and shrinks with (1 - cos) x .75 of the body's half box (its height
    // x 1.1), over the box centre.
    const auto& m = spell.motion;
    const float swell = (1 - std::cos(spell.age)) * .75f;
    const float hx = (m.box_max.x - m.box_min.x) * .5f, hy = (m.box_max.y - m.box_min.y) * .5f,
                hz = (m.box_max.z - m.box_min.z) * .5f * 1.1f;
    cloud.radii = hx > 0 && hy > 0 && hz > 0 ? Vec3{swell * hx, swell * hy, swell * hz} : Vec3{2.5f, 2.5f, 4};
    cloud.opacity = std::sin(spell.age) * spell.cast_alpha * .5f;
    cloud.frame += ticks * .13333334f;
    cloud.phase.x += ticks * 8;
    cloud.phase.y += ticks * 8;
    cloud.phase.z += ticks * 8;
    return spell.stage >= 240 && spell.cast_alpha == 0 ? remove_spell(spell) : 1;
}
} // namespace
int SpellActor::run_ultimate(Match& match, int ticks) {
    if (!data)
        throw Error("Ultimate god spell requires its SPEL definition");
    switch (effect) {
    case Effect::charm:
        return run_charm(match, *this, ticks);
    case Effect::blind_rage:
        return run_blind_rage(match, *this, ticks);
    case Effect::bovine:
        return run_bovine(match, *this, ticks);
    case Effect::cloud_kill:
        return run_cloud_kill(match, *this, ticks);
    case Effect::intestinal_vap:
        return run_ivap(match, *this, ticks);
    case Effect::divine_retribution:
        return run_divine_retribution(match, *this);
    case Effect::volcano:
        return run_volcano(match, *this, ticks);
    case Effect::bore:
        return run_bore(match, *this, ticks);
    case Effect::tornado:
        return run_tornado(match, *this, ticks);
    case Effect::death:
        return run_death(match, *this, ticks);
    case Effect::charm_heart:
        return run_child_visual(match, *this, ticks);
    case Effect::ivap_sprite:
        return run_ivap_guts(match, *this, ticks);
    case Effect::volcano_spatter:
    case Effect::volcano_smoke:
        return run_volcano_visual(match, *this, ticks);
    case Effect::tornado_cast:
    case Effect::tornado_ground:
        return run_tornado_particles(match, *this, ticks);
    case Effect::bore_land:
        return run_bore_land(match, *this, ticks);
    case Effect::rage_creature:
        return run_rage_creature(match, *this, ticks);
    case Effect::cloud_kill_bolt:
        return run_cloud_bolt(match, *this, ticks);
    case Effect::volcano_lava:
        return run_lava(match, *this, ticks);
    case Effect::divine_pillar:
        return run_divine_pillar(match, *this, ticks);
    case Effect::volcano_erupt:
        return run_volcano_erupt(match, *this, ticks);
    default:
        throw Error("Invalid ultimate god spell");
    }
}
} // namespace sacrifice
