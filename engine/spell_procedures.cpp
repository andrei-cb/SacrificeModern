#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include <algorithm>
#include <bit>
#include <cmath>

namespace sacrifice {
bool SpellActor::build_land_block(const Terrain& terrain, int x, int y) {
    // LANDBLOCK::Create 0x16017990: the eight neighbours (0x160553C0) and the centre of land sample (x, y),
    // each 10 units apart and relative to the centre's height. Holes are left out, except the centre.
    static constexpr std::array<std::array<int, 2>, 9> offsets{
        {{-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {0, 0}}};
    // 0x16055408: per quadrant, three alternatives of (required points, triangle points, uv corners); the
    // first whose points are all present is that quadrant's top face.
    static constexpr std::array<std::array<std::uint16_t, 7>, 12> faces{{{386, 1, 8, 7, 1, 3, 2},
                                                                         {385, 0, 8, 7, 0, 3, 2},
                                                                         {259, 0, 1, 8, 0, 1, 3},
                                                                         {266, 1, 3, 8, 0, 3, 2},
                                                                         {262, 1, 2, 8, 0, 1, 2},
                                                                         {268, 2, 3, 8, 1, 3, 2},
                                                                         {416, 8, 5, 7, 1, 3, 0},
                                                                         {352, 8, 5, 6, 1, 3, 2},
                                                                         {448, 8, 6, 7, 1, 2, 0},
                                                                         {296, 8, 3, 5, 0, 1, 2},
                                                                         {280, 8, 3, 4, 0, 1, 3},
                                                                         {304, 8, 4, 5, 0, 3, 2}}};
    land_block = {};
    if (!terrain.width || !terrain.height || terrain.heights.empty())
        return false;
    const auto sample = [](int value, unsigned size) {
        return static_cast<unsigned>(std::clamp(value, 0, static_cast<int>(size) - 1));
    };
    const unsigned cx = sample(x, terrain.width), cy = sample(y, terrain.height);
    const float base = terrain.at(cx, cy);
    std::array<std::size_t, 9> cells{};
    unsigned present = 0;
    for (unsigned i = 0; i < offsets.size(); ++i) {
        const unsigned px = sample(x + offsets[i][0], terrain.width),
                       py = sample(y + offsets[i][1], terrain.height);
        cells[i] = static_cast<std::size_t>(py) * terrain.width + px;
        if (i != 8 && (terrain.raw(px, py) & 0x8000))
            continue;
        present |= 1u << i;
        std::uint32_t color = 0xffffffff;
        if (cells[i] < terrain.lightmap.size()) {
            const auto sample_light = terrain.lightmap[cells[i]];
            color = 0xff000000u | ((sample_light & 0xff) << 16) | (sample_light & 0xff00) |
                    ((sample_light >> 16) & 0xff);
        }
        land_block.points[i] = {
            {static_cast<float>(offsets[i][0] * 10), static_cast<float>(offsets[i][1] * 10),
             terrain.at(px, py) - base},
            color};
    }
    land_block.cells = {cells[0], cells[1], cells[7], cells[8]};
    unsigned used = 0;
    for (unsigned quadrant = 0; quadrant < 4; ++quadrant)
        for (unsigned option = 0; option < 3; ++option) {
            const auto& face = faces[quadrant * 3 + option];
            if ((present & face[0]) != face[0])
                continue;
            LandBlock::Face top;
            top.quadrant = static_cast<std::uint8_t>(quadrant);
            for (unsigned i = 0; i < 3; ++i) {
                top.points[i] = static_cast<std::uint8_t>(face[1 + i]);
                top.uvs[i] = static_cast<std::uint8_t>(face[4 + i]);
            }
            land_block.faces.push_back(top);
            used |= face[0];
            break;
        }
    if (land_block.faces.empty())
        return false;
    // Walls: walk the perimeter from point 1, cutting through the centre past a missing edge midpoint, and
    // hang a quad below every step until a point comes round again.
    unsigned previous = 0, visited = 0, step = 1;
    for (int guard = 0; guard < 64; ++guard) {
        const unsigned bit = 1u << step;
        const bool skip = !(bit & used) && !(step & 1);
        const unsigned next = bit & used ? step : 8;
        if (!skip && previous != next) {
            if ((1u << previous) & visited)
                land_block.walls.push_back(
                    {static_cast<std::uint8_t>(previous), static_cast<std::uint8_t>(next)});
            previous = next;
            if (bit & visited)
                break;
            visited |= bit | 1u << next;
        }
        step = (step + 1) & 7;
    }
    motion.position = {static_cast<float>(x * 10), static_cast<float>(y * 10), base};
    motion.orientation = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    return true;
}
Mesh SpellActor::land_block_mesh(unsigned part) const {
    // 0x16055558: tile corners inset half a texel, as the near land tiles are.
    static constexpr std::array<Vec2, 4> uvs{
        {{.0078125f, .0078125f}, {.9921875f, .0078125f}, {.0078125f, .9921875f}, {.9921875f, .9921875f}}};
    Mesh mesh;
    const auto& o = motion.position;
    const auto vertex = [&](Vec3 r, Vec2 uv, std::uint32_t color) {
        mesh.vertices.push_back({{o.x + r.x, o.z + r.z, o.y + r.y}, {}, uv, color});
    };
    const auto& points = land_block.points;
    if (part < 4) {
        for (const auto& face : land_block.faces)
            if (face.quadrant == part) {
                const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
                for (unsigned i = 0; i < 3; ++i)
                    vertex(points[face.points[i]].position, uvs[face.uvs[i]], points[face.points[i]].color);
                mesh.indices.insert(mesh.indices.end(), {base + 2, base + 1, base});
                mesh.triangle_materials.push_back(0);
            }
        return mesh;
    }
    for (const auto& [from, to] : land_block.walls) {
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        const auto a = points[from].position, b = points[to].position;
        vertex(a, uvs[0], points[from].color);
        vertex(b, uvs[1], points[to].color);
        vertex({a.x, a.y, a.z - 50}, uvs[2], 0xffffffff);
        vertex({b.x, b.y, b.z - 50}, uvs[3], 0xffffffff);
        mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base + 1, base + 3, base + 2});
        mesh.triangle_materials.insert(mesh.triangle_materials.end(), 2, 0);
    }
    return mesh;
}
namespace {
FourCC cc(std::string_view text) {
    return FourCC::from_text(text);
}
int remove_spell(SpellActor& spell) {
    spell.state_flags |= MatchObject::removed_flag;
    return -2;
}
float length(Vec3 v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}
Vec3 box_center(const ObjectMotion& motion) { // 0x44e350.
    return {motion.position.x + (motion.box_min.x + motion.box_max.x) * .5f,
            motion.position.y + (motion.box_min.y + motion.box_max.y) * .5f,
            motion.position.z + (motion.box_min.z + motion.box_max.z) * .5f};
}
Vec3 box_half(const ObjectMotion& motion) {
    return {(motion.box_max.x - motion.box_min.x) * .5f, (motion.box_max.y - motion.box_min.y) * .5f,
            (motion.box_max.z - motion.box_min.z) * .5f};
}
// 0x44ca80's result is the preset-129 scale. The decompiler stored it as the negative Z extent; the call's
// return is otherwise unused, which is the FPU value actually written before SetProperty.
float cloud_scale(Vec3 half) {
    return std::clamp(length(half) * .3f, .25f, 2.f);
}
void set_box(ParticleSystem& particles, Vec3 half) {
    const float low[3]{-half.x, -half.y, -half.z}, high[3]{half.x, half.y, half.z};
    particles.set_property(2, low, high);
}
// 0x492300: HealingShowerProc heals only when the spell has a side. Friendlies are that side and its allies.
void healing_shower(Match& match, SpellActor& spell) {
    const float radius = spell.data->spread_limit;
    const float limit = radius * radius;
    const auto heal_one = [&](MatchObject& object) {
        auto* actor = dynamic_cast<CreatureActor*>(&object);
        if (!actor || !actor->friendly(&spell))
            return 0;
        const auto& at = actor->motion.position;
        const double dx = static_cast<double>(at.x) - spell.motion.position.x,
                     dy = static_cast<double>(at.y) - spell.motion.position.y,
                     dz = static_cast<double>(at.z) - spell.motion.position.z;
        if (dz * dz + dy * dy + dx * dx > limit)
            return 0;
        actor->heal(match, actor->stats.current[5], 256, 0); // 0x440a30: amount is current health, rate 256.
        return 0;
    };
    for (const unsigned group : {0u, 1u})
        match.visit_group(group, 5, heal_one);
}
void poison_cloud(Match& match, SpellActor& spell) {
    const float spread = spell.data->spread_limit;
    const float limit = spread * spread;
    MatchObject* source = spell.owner && (spell.owner->kind_flags & 5) ? spell.owner : &spell;
    const auto poison_one = [&](MatchObject& object) {
        auto* actor = dynamic_cast<CreatureActor*>(&object);
        if (!actor || actor->named(cc("HEAL")) || actor->named(cc("POIS")))
            return 0;
        if (spell_box_distance_squared(spell.motion.position, actor->motion, limit) > limit)
            return 0;
        SpellActor::poison(match, *actor, source, spell.data->duration, spell.data->amount, spell.data->damage_type,
                           spell.data->integer(cc("ires"), 0), spell.data->real(cc("freg"), 1),
                           spell.data->integer(cc("iacc"), 0), spread);
        return 0;
    };
    for (const unsigned group : {0u, 1u})
        match.visit_group(group, 5, poison_one); // 0x492230 mask 5, then 0x440b70 / 0x43fd90.
}
int advance_cloud(SpellActor& spell, Match& match, int ticks) { // 0x4389d0.
    auto& particles = spell.particles(match);
    spell.host.time += ticks;
    particles.advance(spell.host.time, spell.emission);
    spell.emission = std::max(0.f, spell.emission + ticks * spell.host.rate);
    if (spell.emission <= 0 && !particles.alive())
        return remove_spell(spell);
    return 1;
}
int follow_marked(SpellActor& spell) {
    auto* target = spell.effect_target;
    if (!target || (target->state_flags & MatchObject::removed_flag))
        return remove_spell(spell);
    if (const auto* body = target->source_motion())
        spell.motion.position = box_center(*body);
    return 1;
}
int run_beacon(Match& match, SpellActor& spell, int ticks) {
    // BeaconRun 0x453b70: name BECN, spawn BeaconSourceRun 0x453e90, follow the target box.
    auto* target = spell.effect_target;
    if (!spell.stage) {
        if (!target)
            return remove_spell(spell);
        auto& source = SpellActor::launch_sacrifice_host(match, spell, SpellActor::Effect::beacon_source);
        source.effect_target = target;
        if (spell.allegiance)
            spell.allegiance->attach(source);
        source.run(match, 0);
        spell.stage = 1;
    }
    (void)ticks;
    if (!target || (target->state_flags & MatchObject::removed_flag))
        return remove_spell(spell);
    target->name_reference(cc("BECN"), &spell);
    return follow_marked(spell);
}
int run_ghost(Match& match, SpellActor& spell, int ticks) {
    // GhostProc 0x4404f0: owner from +476, pulse creature +2212, +568 |= 7, +480 |= 0x10000.
    if (!spell.stage) {
        if (spell.effect_target)
            spell.owner = spell.effect_target;
        spell.stage = 1;
    }
    auto* target = dynamic_cast<CreatureActor*>(spell.owner);
    if (!target || !(target->kind_flags & 5) || (target->state_flags & MatchObject::removed_flag))
        return remove_spell(spell);
    spell.age += static_cast<float>(ticks) * 0.052359833f;
    const auto pulse = static_cast<float>(std::sin(spell.age) * 0.1 + 0.75);
    spell.emission = pulse;
    target->fade_target = pulse;
    target->fade_step = std::bit_cast<float>(1015580809u);
    target->common_status |= 7;
    target->object_flags |= 0x10000;
    if (target->source_motion())
        spell.motion.position = box_center(*target->source_motion());
    (void)match;
    return 1;
}
int run_removed_edge(SpellActor& spell, int ticks) {
    // INERT_RemovedEdge 0x43ac70: identity orientation, motion flags 65541 (velocity and gravity) from
    // rest, and retire once the land block has dropped more than 50.
    if (!spell.stage) {
        spell.motion.orientation = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        spell.point = spell.motion.position;
        spell.motion.flags = 65541;
        spell.motion.collision_mask = 0;
        spell.motion.velocity = {};
        spell.state_flags &= ~3u;
        spell.remaining = 1200; // Not native: a guard should the drop stall.
        spell.stage = 1;
    }
    spell.remaining -= ticks;
    if (spell.point.z - spell.motion.position.z > 50.f || spell.remaining <= 0)
        return remove_spell(spell);
    return 1;
}
int run_remove_land(Match& match, SpellActor& spell, int ticks) {
    // RemoveLandProc 0x440640: grow to spread_limit, 0x464BD0 / 0x453260 on a 10-unit grid.
    if (!spell.stage) {
        spell.age = 0;
        if (spell.point.x || spell.point.y || spell.point.z)
            spell.motion.position = spell.point;
        spell.stage = 1;
    }
    const float spread = spell.data->spread_limit > 0.f ? spell.data->spread_limit : 10.f;
    const float speed = spell.data->speed > 0.f ? spell.data->speed : 0.5f;
    spell.age += static_cast<float>(ticks) * speed;
    if (spell.age >= spread)
        spell.age = spread;
    const int n = static_cast<int>(static_cast<std::int64_t>(spell.age * 0.1 + 1.0));
    const Vec3 origin = spell.motion.position;
    for (int y = -n; y <= n; ++y) {
        for (int x = -n; x <= n; ++x) {
            const Vec3 at{origin.x + static_cast<float>(x * 10), origin.y + static_cast<float>(y * 10),
                          origin.z};
            const int cell_x = static_cast<int>(static_cast<std::int64_t>((at.x + 5.0) * 0.1));
            const int cell_y = static_cast<int>(static_cast<std::int64_t>((at.y + 5.0) * 0.1));
            if (cell_x < 0 || cell_y < 0)
                continue;
            if (!match.set_edge(cell_x, cell_y, 1, 1))
                continue;
            match.shake(spell.motion.position, 16, 50.f); // 0x453260(pos, 16, 50).
            auto& edge = SpellActor::launch_sacrifice_host(match, spell, SpellActor::Effect::removed_edge);
            edge.motion.position = {static_cast<float>(cell_x * 10), static_cast<float>(cell_y * 10), at.z};
            if (match.terrain && !edge.build_land_block(*match.terrain, cell_x, cell_y))
                remove_spell(edge);
        }
    }
    if (spell.age >= spread)
        return remove_spell(spell);
    return 1;
}
} // namespace
int SpellActor::run_procedures(Match& match, int ticks) {
    if (!data && effect != Effect::gib_chunk && effect != Effect::beacon_source &&
        effect != Effect::removed_edge)
        throw Error("Spell procedure requires its SPEL definition");
    switch (effect) {
    case Effect::boon:
    case Effect::healing_shower: {
        // 0x438e30 moves the burst onto the target; 0x4388b0 then only advances it.
        if (effect_target && effect_target->source_motion())
            motion.position = box_center(*effect_target->source_motion());
        if (effect == Effect::healing_shower && allegiance)
            healing_shower(match, *this);
        sound_event(match, 2); // 0x44cbf0 mode 2.
        effect = Effect::spell_hit;
        match.assign(*this, spell_hit_function);
        return 0;
    }
    case Effect::dust_cloud:
    case Effect::steam_cloud:
    case Effect::poison_cloud: {
        if (!stage) {
            const auto* target = effect_target ? effect_target->source_motion() : nullptr;
            Vec3 half{data->spread_limit, data->spread_limit, data->spread_limit};
            Vec3 center = point.x || point.y || point.z ? point : motion.position;
            if (target && effect == Effect::dust_cloud) {
                center = box_center(*target);
                half = box_half(*target);
            } else if (target) {
                spell_target_point(center, effect_target); // 0x44e020.
                const float diagonal = .5f * length({target->box_max.x - target->box_min.x,
                                                     target->box_max.y - target->box_min.y,
                                                     target->box_max.z - target->box_min.z});
                // Poison's compare was dropped by the decompiler. A positive body size replaces the spread
                // cube; a zero box keeps the spread. Steam always uses the body, or spread/4 with no target.
                const float extent = effect == Effect::steam_cloud ? (diagonal > 0 ? diagonal : data->spread_limit * .25f)
                                     : diagonal > 0                 ? diagonal
                                                                    : data->spread_limit;
                half = {extent, extent, extent};
            } else if (effect == Effect::steam_cloud)
                half = {data->spread_limit * .25f, data->spread_limit * .25f, data->spread_limit * .25f};
            motion.position = center;
            const char* fallback = effect == Effect::poison_cloud ? "pois" : "dirt";
            auto& particles = this->particles(match);
            particle_material(match, particles, effect == Effect::steam_cloud ? 0x1a012008u : 0x0a011008u,
                              FourCC{static_cast<unsigned>(data->integer(cc("ttex"), cc(fallback).value))});
            const float scale = cloud_scale(half), life = data->duration * .5f;
            particles.set_property(129, &scale, &life); // 0x439690.
            set_box(particles, half);
            particles.set_position(center);
            emission = 1;
            host.time = 0;
            const float fade = data->duration * .5f;
            host.rate = fade > 0 ? -1.f / (fade * 60.f) : -1.f;
            if (effect == Effect::steam_cloud)
                spell_damage(match, this, nullptr, data->damage_type, data->amount, data->radius); // 0x44d7e0.
            if (effect == Effect::poison_cloud)
                poison_cloud(match, *this);
            if (effect != Effect::dust_cloud)
                sound_event(match, 2);
            stage = 1;
        }
        return advance_cloud(*this, match, ticks);
    }
    case Effect::gib: {
        if (!effect_target || !effect_target->source_motion())
            return remove_spell(*this);
        if (!stage) {
            sound_event(match, 2);
            for (int i = 0; i < 8; ++i) {
                auto& chunk = static_cast<SpellActor&>(match.create(
                    MatchObject::spell, [] { return std::make_unique<SpellActor>(); }, this,
                    [&](MatchObject& object) { static_cast<SpellActor&>(object).effect = Effect::gib_chunk; }));
                chunk.owner = owner;
                chunk.effect_target = effect_target;
                chunk.motion.position = motion.position;
                chunk.object_flags |= 0x10000;
                if (allegiance)
                    allegiance->attach(chunk);
                chunk.run(match, 1); // GibProc runs each 0x4398a0 child immediately.
            }
            const auto& body = *effect_target->source_motion();
            motion.position = body.position; // 0x439ad0 copies the target position, not the box center.
            const float span = (body.box_max.x - body.box_min.x) + (body.box_max.y - body.box_min.y) +
                               (body.box_max.z - body.box_min.z);
            const float extent = span * .3333f; // dbl_4BFD18.
            auto& particles = this->particles(match);
            particle_material(match, particles, 0x0a011008, "blud"); // 0x40cbf0 texture dword_4C7890.
            const float scale = std::max(.5f, extent * .3f), life = 3.f;
            const float slow[3]{-.0005f, -.0005f, -.001f}, fast[3]{.0005f, .0005f, 0};
            particles.set_property(129, &scale, &life);
            particles.set_property(3, slow, fast);
            particles.set_position(motion.position);
            const float low[3]{body.box_min.x, body.box_min.y, body.box_min.z},
                high[3]{body.box_max.x, body.box_max.y, body.box_max.z};
            particles.set_property(2, low, high);
            // ftol(max(extent, 5) * 10): a body no larger than 5 emits 50 particles.
            const int count = static_cast<int>(std::max(extent, 5.f) * 10.f);
            for (int n = 0; n < count; ++n)
                particles.emit(0, 1);
            host.time = 0;
            emission = .1f;
            stage = 1;
            sound_requests.push_back({cc("Xgib"), cc("Xgib"), reference(), 0x40004}); // 0x44de40.
        }
        auto& particles = this->particles(match);
        host.time += ticks;
        emission = std::max(0.f, emission - ticks * .005f);
        particles.advance(host.time, emission);
        particles.set_position(motion.position);
        if (!particles.alive())
            return remove_spell(*this);
        return 1;
    }
    case Effect::gib_chunk: {
        auto& particles = this->particles(match);
        if (!stage) {
            if (effect_target && effect_target->source_motion())
                motion.position = box_center(*effect_target->source_motion());
            particle_material(match, particles, 0x0a011008, "blud");
            particles.set_position(motion.position);
            host.time = 0;
            emission = 1;
            const float slow[3]{-.01f, -.01f, -.01f}, fast[3]{.01f, .01f, .01f};
            particles.set_property(3, slow, fast);
            const float preset[3]{.25f, 0, std::bit_cast<float>(match.blood_solid ? 1u : 0u)};
            particles.set_property(133, preset, preset); // 0x40cbf0(0.25, 0).
            stage = 1;
        } else if (stage == 1 && (state_flags & 2)) {
            stage = 2;
        } else if (stage == 2) {
            const float slow[3]{-.05f, -.05f, 0}, fast[3]{.05f, .05f, .05f};
            particles.set_property(3, slow, fast);
            if (emission > .25f) {
                const int count = static_cast<int>(emission * 32.f);
                const float preset[3]{.25f, std::bit_cast<float>(count),
                                     std::bit_cast<float>(match.blood_solid ? 1u : 0u)};
                particles.set_property(133, preset, preset);
            }
            emission = 0;
            stage = 3;
        }
        particles.set_position(motion.position);
        host.time += ticks;
        emission = std::max(0.f, emission - ticks * .01f);
        particles.advance(host.time, emission);
        if (!particles.alive())
            return remove_spell(*this);
        return 1;
    }
    case Effect::beacon:
        return run_beacon(match, *this, ticks);
    case Effect::ghost:
        return run_ghost(match, *this, ticks);
    case Effect::remove_land:
        return run_remove_land(match, *this, ticks);
    case Effect::removed_edge:
        // 0x464BD0's 0x4691F0 gives the block its LANDBLOCK model, which puts it in the world: without that
        // it never moves.
        if (!stage && match.terrain && match.spatial)
            bind_world(match);
        return run_removed_edge(*this, ticks);
    case Effect::beacon_source:
        return follow_marked(*this);
    default:
        throw Error("Invalid spell procedure");
    }
}
} // namespace sacrifice
