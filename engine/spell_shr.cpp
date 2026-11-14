#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include "sacrifice/spell_data.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
FourCC cc(std::string_view text) {
    return FourCC::from_text(text);
}
int remove_spell(SpellActor& spell) {
    spell.state_flags |= MatchObject::removed_flag;
    return -2;
}
double distance_squared(Vec3 a, Vec3 b) {
    const double dx = static_cast<double>(a.x) - b.x, dy = static_cast<double>(a.y) - b.y,
                 dz = static_cast<double>(a.z) - b.z;
    return dx * dx + dy * dy + dz * dz;
}
BuildingActor* building_of(SpellActor& spell) {
    auto* building = dynamic_cast<BuildingActor*>(spell.effect_target);
    if (!building)
        building = dynamic_cast<BuildingActor*>(spell.owner);
    return building && (building->kind_flags & MatchObject::building) ? building : nullptr;
}
Vec3 building_point(BuildingActor& building, bool sacrifice) {
    return sacrifice ? building.sacrifice_point() : building.effect_point();
}
void follow_building(SpellActor& spell, bool sacrifice = false) {
    if (auto* building = building_of(spell))
        spell.motion.position = building_point(*building, sacrifice);
}
int persist_building(SpellActor& spell, bool sacrifice = false) {
    auto* building = building_of(spell);
    if (!building || (building->state_flags & MatchObject::removed_flag))
        return remove_spell(spell);
    follow_building(spell, sacrifice);
    return 1;
}
SpellActor& launch_child(Match& match, SpellActor& source, SpellActor::Effect effect, MatchObject* target,
                         Vec3 position) {
    auto& child = SpellActor::launch_sacrifice_host(match, source, effect);
    child.effect_target = target;
    child.owner = source.owner;
    child.data = source.data;
    child.motion.position = position;
    if (source.allegiance)
        source.allegiance->attach(child);
    child.run(match, 0);
    return child;
}
CreatureActor* nearest_foe(Match& match, SpellActor& spell, float range) {
    CreatureActor* found = nullptr;
    double best = static_cast<double>(range) * range;
    const auto visit = [&](MatchObject& object) {
        auto* actor = dynamic_cast<CreatureActor*>(&object);
        if (!actor || !(actor->kind_flags & 5) || !actor->alive() || !actor->source_motion())
            return 0;
        if (!actor->enemy(&spell) && !(spell.owner && actor->enemy(spell.owner)))
            return 0;
        const double d = distance_squared(spell.motion.position, actor->source_motion()->position);
        if (d <= best) {
            best = d;
            found = actor;
        }
        return 0;
    };
    match.visit_group(0, 5, visit);
    match.visit_group(1, 5, visit);
    return found;
}
int run_destroy_hut(Match& match, SpellActor& spell) {
    // DestroyHut 0x43cd50: 0x44CBF0, 0x43AFE0, 0x439F10(2, 15), then 0x44B770(0).
    SpellActor::launch_shockwave(match, spell, 15.f);
    return remove_spell(spell);
}
int run_destroy_manalith(Match& match, SpellActor& spell) {
    // DestroyManalith 0x43cda0: hut path plus 0x438860 burst.
    SpellActor::launch_shockwave(match, spell, 25.f);
    auto& hit = SpellActor::launch_sacrifice_host(match, spell, SpellActor::Effect::spell_hit);
    hit.motion.position = spell.motion.position;
    hit.run(match, 0);
    return remove_spell(spell);
}
int run_portal(Match& match, SpellActor& spell, int ticks) {
    // PortalRun 0x43ef50: type-51 BEZIERCYLINDER, not the sacrifice-vortex SACPRIM.
    auto* building = building_of(spell);
    auto& portal = spell.building_portal;
    if (!spell.stage) {
        if (!building)
            return remove_spell(spell);
        portal.enabled = true;
        portal.texture = FourCC{static_cast<std::uint32_t>(spell.data ?
            spell.data->integer(cc("ttex"), cc("spkd").value) : cc("spkd").value)};
        portal.radius = spell.data ? spell.data->real(cc("frad"), 6) : 6;
        spell.motion.orientation = motion_orientation(building->motion.yaw, 1.5707951f);
        for (auto& phase : portal.phase)
            phase = match.random().next() * 6.2831802f / 32768;
        for (auto& speed : portal.speed)
            speed = (match.random().next() * 6.2831802f / 32768 - 3.1415901f) / 60;
        spell.stage = 1;
    }
    if (!building || (building->state_flags & MatchObject::removed_flag))
        return remove_spell(spell);
    follow_building(spell, true);
    const auto* root = building->primary ? building->primary : building;
    const float target = root->completed ? 1.f : 0.f, step = ticks / 60.f;
    portal.opacity = portal.opacity < target ? std::min(target, portal.opacity + step)
                                            : std::max(target, portal.opacity - step);
    portal.spin += ticks * .052359834f;
    for (unsigned i = 0; i < 4; ++i)
        portal.phase[i] += ticks * portal.speed[i];
    return 1;
}
int run_pus_wart(Match& match, SpellActor& spell) {
    // PusWartRun 0x43f400: MRM morph, 0x492E10(5, spread, 15, PUSS).
    if (persist_building(spell) < 0)
        return -2;
    auto* building = building_of(spell);
    if (!building || !building->completed || !spell.data)
        return 1;
    const float spread = std::max(spell.data->spread_limit, 15.f);
    match.visit_group(0, 5, [&](MatchObject& object) {
        auto* actor = dynamic_cast<CreatureActor*>(&object);
        if (!actor || !(actor->kind_flags & 5) || !actor->alive() || actor->named(cc("PUSS")))
            return 0;
        if (!actor->source_motion())
            return 0;
        if (!actor->enemy(&spell) && !(spell.owner && actor->enemy(spell.owner)))
            return 0;
        if (distance_squared(spell.motion.position, actor->source_motion()->position) >
            static_cast<double>(spread) * spread)
            return 0;
        actor->name_reference(cc("PUSS"), &spell);
        SpellActor::poison(match, *actor, spell.owner ? spell.owner : &spell, *spell.data);
        return 0;
    });
    match.visit_group(1, 5, [&](MatchObject& object) {
        auto* actor = dynamic_cast<CreatureActor*>(&object);
        if (!actor || !(actor->kind_flags & 5) || !actor->alive() || actor->named(cc("PUSS")))
            return 0;
        if (!actor->source_motion())
            return 0;
        if (!actor->enemy(&spell) && !(spell.owner && actor->enemy(spell.owner)))
            return 0;
        if (distance_squared(spell.motion.position, actor->source_motion()->position) >
            static_cast<double>(spread) * spread)
            return 0;
        actor->name_reference(cc("PUSS"), &spell);
        SpellActor::poison(match, *actor, spell.owner ? spell.owner : &spell, *spell.data);
        return 0;
    });
    return 1;
}
int run_mini_volcano(Match& match, SpellActor& spell, int ticks) {
    // MiniVolcanoRun 0x43e3e0: particle host on the building; random stage-16 pulse.
    (void)ticks;
    (void)match;
    return persist_building(spell);
}
int run_pyro_flame(SpellActor& spell) {
    return persist_building(spell);
}
int run_pyro_dynamo(Match& match, SpellActor& spell, int ticks) {
    // PyroDynamoRun 0x43ce20: two 0x43D140 children, raise +1280, 0x453260 at stage 48.
    auto* building = building_of(spell);
    if (!spell.stage) {
        if (!building)
            return remove_spell(spell);
        launch_child(match, spell, SpellActor::Effect::pyro_dynamo_flame, building, spell.motion.position);
        launch_child(match, spell, SpellActor::Effect::pyro_dynamo_flame, building, spell.motion.position);
        spell.age = 0;
        spell.stage = 1;
    }
    if (persist_building(spell) < 0)
        return -2;
    building = building_of(spell);
    if (building && building->completed) {
        spell.age += static_cast<float>(ticks) * 0.052359834f;
        building->motion.position.z += std::sin(spell.age);
        if (spell.stage == 1) {
            match.shake(spell.motion.position, 16, spell.data ? spell.data->spread_limit : 50.f);
            spell.stage = 2;
        }
    }
    return 1;
}
int run_mfry_bracket(SpellActor& spell) {
    // MFryBracketRun 0x43d480: object_flags 65544, copy primary yaw, 0x465090.
    if (auto* building = building_of(spell))
        building->object_flags |= 0x10008;
    return persist_building(spell);
}
int run_mfry_splash(SpellActor& spell) {
    return persist_building(spell);
}
int run_mfry(Match& match, SpellActor& spell, int ticks) {
    // MFryRun 0x43d770: aim, 0x44CD10 beam, damage amount/duration, 0x45CB90, splash 0x43E230.
    auto* building = building_of(spell);
    if (!spell.stage) {
        if (!building)
            return remove_spell(spell);
        launch_child(match, spell, SpellActor::Effect::mfry_bracket, building, spell.motion.position);
        spell.stage = 1;
    }
    if (persist_building(spell) < 0)
        return -2;
    if (!spell.data)
        return 1;
    const float range = std::max(spell.data->range, 1.f);
    auto* victim = nearest_foe(match, spell, range);
    if (!victim || !(building_of(spell) && building_of(spell)->completed))
        return 1;
    const int rate = spell.data->duration > 0.f
                         ? static_cast<int>(static_cast<double>(spell.data->amount) / spell.data->duration)
                         : spell.data->amount;
    spell_damage(match, spell.owner ? spell.owner : &spell, victim, spell.data->damage_type,
                 rate > 0 ? rate : spell.data->amount, 0.f, victim->motion.position);
    victim->burn_near(victim->motion.position, 10.f, 60);
    if (!spell.linger) {
        launch_child(match, spell, SpellActor::Effect::mfry_splash, victim, victim->motion.position);
        spell.linger = 1;
    }
    (void)ticks;
    return 1;
}
int run_machine_strut(SpellActor& spell) {
    return persist_building(spell);
}
int run_machine_shot(Match& match, SpellActor& spell) {
    // 0x43EE40: 0x4355A0 / 0x435840 then impact. Shots resolve on spawn.
    auto* victim = dynamic_cast<CreatureActor*>(spell.effect_target);
    if (victim && spell.data)
        spell_damage(match, spell.owner ? spell.owner : &spell, victim, spell.data->damage_type,
                     spell.data->amount, spell.data->radius, victim->motion.position);
    return remove_spell(spell);
}
int run_machine(Match& match, SpellActor& spell, int ticks) {
    // MachineRun 0x43e9c0: strut morph, 0x492FA0, 0x44D430 / 0x44E740 shot 0x43EE40.
    auto* building = building_of(spell);
    if (!spell.stage) {
        if (!building)
            return remove_spell(spell);
        launch_child(match, spell, SpellActor::Effect::machine_strut, building, spell.motion.position);
        spell.next_pulse = 0;
        spell.stage = 1;
    }
    if (persist_building(spell) < 0)
        return -2;
    building = building_of(spell);
    spell.next_pulse += ticks;
    if (!building || !building->completed || !spell.data || spell.next_pulse < 30)
        return 1;
    spell.next_pulse = 0;
    const float range = std::max(spell.data->range, 1.f);
    if (auto* victim = nearest_foe(match, spell, range))
        launch_child(match, spell, SpellActor::Effect::machine_shot, victim, victim->motion.position);
    return 1;
}
} // namespace
int SpellActor::run_shr(Match& match, int ticks) {
    switch (effect) {
    case Effect::destroy_hut:
        sound_event(match, 2);
        return run_destroy_hut(match, *this);
    case Effect::destroy_manalith:
        sound_event(match, 2);
        return run_destroy_manalith(match, *this);
    case Effect::portal_run:
        return run_portal(match, *this, ticks);
    case Effect::pus_wart:
        return run_pus_wart(match, *this);
    case Effect::mini_volcano:
        return run_mini_volcano(match, *this, ticks);
    case Effect::pyro_dynamo: {
        const int result = run_pyro_dynamo(match, *this, ticks);
        if (stage == 2 && linger != 4) {
            sound_event(match, 4);
            linger = 4;
        }
        return result;
    }
    case Effect::pyro_dynamo_flame:
        return run_pyro_flame(*this);
    case Effect::mfry:
        return run_mfry(match, *this, ticks);
    case Effect::mfry_bracket:
        return run_mfry_bracket(*this);
    case Effect::mfry_splash:
        return run_mfry_splash(*this);
    case Effect::machine:
        return run_machine(match, *this, ticks);
    case Effect::machine_strut:
        return run_machine_strut(*this);
    case Effect::machine_shot:
        sound_event(match, 8);
        return run_machine_shot(match, *this);
    default:
        throw Error("Invalid structure spell");
    }
}
// BEZIERCYLINDER::Draw 0x1600b0a0, defaults 0x4cbaa8: cubic centres, radii and alpha.
Mesh SpellActor::building_portal_mesh() const {
    Mesh mesh;
    const auto& p = building_portal;
    if (!p.enabled || p.opacity <= 0)
        return mesh;
    constexpr unsigned columns = 24, rows = 16;
    const float r = p.radius * p.opacity;
    const std::array<Vec3, 4> centres{{{},
        {std::sin(p.phase[2]) * r * .25f, std::sin(p.phase[3]) * r * .25f, -.5f},
        {std::sin(p.phase[0]) * r * .5f, std::sin(p.phase[1]) * r * .5f, -1},
        {std::cos(p.phase[2] + p.phase[1]) * r * .75f,
         std::cos(p.phase[0] + p.phase[3]) * r * .75f, -1.5f}}};
    for (unsigned row = 0; row <= rows; ++row) {
        const float t = static_cast<float>(row) / rows, u = 1 - t;
        const std::array<float, 4> b{u*u*u, 3*u*u*t, 3*u*t*t, t*t*t};
        Vec3 centre{};
        for (unsigned i = 0; i < 4; ++i) {
            centre.x += b[i] * centres[i].x;
            centre.y += b[i] * centres[i].y;
            centre.z += b[i] * centres[i].z;
        }
        const float radius = r * (b[0] + b[1] * .5f + b[2] * .25f + b[3] * .05f);
        // Keep the entrance translucent: the original swirl lets the dark doorway show through.
        const auto color = 0xffffffu | static_cast<std::uint32_t>(80 * p.opacity * (1 - b[3])) << 24;
        for (unsigned column = 0; column <= columns; ++column) {
            const float angle = p.spin + column * 6.2831802f / columns;
            const Vec3 q{centre.x + radius * std::sin(angle), centre.y + radius * std::cos(angle), centre.z};
            const auto& o = motion.orientation;
            mesh.vertices.push_back({{motion.position.x + o[0]*q.x + o[3]*q.y + o[6]*q.z,
                                      motion.position.z + o[2]*q.x + o[5]*q.y + o[8]*q.z,
                                      motion.position.y + o[1]*q.x + o[4]*q.y + o[7]*q.z}, {},
                                     {static_cast<float>(column) / columns, t}, color});
        }
    }
    for (unsigned row = 0; row < rows; ++row)
        for (unsigned column = 0; column < columns; ++column) {
            const auto a = row * (columns + 1) + column, b = a + columns + 1;
            mesh.indices.insert(mesh.indices.end(), {a, a+1, b, a+1, b+1, b});
        }
    return mesh;
}
} // namespace sacrifice
