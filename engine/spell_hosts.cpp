#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_data.hpp"
#include "sacrifice/spell_registry.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
// Side colour bytes (+32: R in byte 2) as 0..1 floats, as the hosts pass them to the light.
Vec3 side_color(const MatchSide* side) {
    const auto c = side ? side->definition.color : 0xffffffu;
    return {static_cast<float>((c >> 16 & 255) * 0.0039215689),
            static_cast<float>((c >> 8 & 255) * 0.0039215689), static_cast<float>((c & 255) * 0.0039215689)};
}
// The particle colour from a light colour: × 255, at least 64 per channel.
std::array<float, 3> particle_color(const Vec3& color) {
    return {std::max(color.x * 255.0f, 64.0f), std::max(color.y * 255.0f, 64.0f),
            std::max(color.z * 255.0f, 64.0f)};
}
// The hosts move their light intensity 0.01 per tick towards the target, turning it on from zero and off at
// zero.
void fade_light(SpellActor::Light& light, float& value, float target, float step) {
    if (value == target)
        return;
    if (value == 0)
        light.enabled = true;
    value = value < target ? std::min(value + step, target) : std::max(value - step, target);
    if (value == 0)
        light.enabled = false;
}
} // namespace
ParticleSystem& SpellActor::particles(Match& match, unsigned index) {
    while (particle_systems.size() <= index) {
        auto& system = particle_systems.emplace_back(match.visual_random);
        if (match.terrain)
            system.set_ground([&match](const Vec3& point, Vec3* normal) {
                TerrainPlane plane;
                const auto height = terrain_surface(*match.terrain, point.x, point.y, &plane);
                if (normal)
                    *normal = plane.normal;
                return static_cast<float>(height);
            });
        system.set_scar([&match](Vec3 position) {
            match.scar_requests.push_back(
                {FourCC::from_text("blud"), position, match.clock().match, ++match.terrain_revision, 8, 8});
        });
    }
    return particle_systems[index];
}
void SpellActor::particle_material(Match& match, ParticleSystem& system, std::uint32_t flags,
                                   const char* texture) {
    particle_material(match, system, flags, FourCC::from_text(texture));
}
void SpellActor::particle_material(Match& match, ParticleSystem& system, std::uint32_t flags,
                                   FourCC texture) {
    system.set_material(flags, texture);
    system.set_frames(match.texture_frames ? match.texture_frames(texture) : 1);
}
namespace {
// 0x44c230(parent, 42, handler, argument): a type 32 child activated after its parent.
SpellActor& spawn_child(Match& match, const MatchObject& parent, SpellActor::Effect effect) {
    auto& object = match.create(
        MatchObject::spell, [] { return std::make_unique<SpellActor>(); }, &parent,
        [&](MatchObject& object) {
            auto* spell = dynamic_cast<SpellActor*>(&object);
            if (!spell)
                throw Error("Spell pool object has an incompatible host type");
            spell->effect = effect;
        });
    auto& spell = static_cast<SpellActor&>(object);
    spell.object_flags |= 0x10000;
    return spell;
}
// 0x44e020: the centre of an object's collision boxes (or visual bounds), in world coordinates.
Vec3 object_center(const MatchObject& object) {
    const auto* source = object.source_motion();
    if (!source)
        return {};
    return {source->position.x + source->reference_center.x, source->position.y + source->reference_center.y,
            source->position.z + source->reference_center.z};
}
// 0x410DD0: orthonormal frame looking from `from` toward `to`, with Z-up.
void look_between(Orientation& o, Vec3& position, Vec3 from, Vec3 to) {
    Vec3 forward{to.x - from.x, to.y - from.y, to.z - from.z};
    const double length =
        std::sqrt(static_cast<double>(forward.x) * forward.x + static_cast<double>(forward.y) * forward.y +
                  static_cast<double>(forward.z) * forward.z);
    if (length == 0) {
        o = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        position = from;
        return;
    }
    forward = {static_cast<float>(forward.x / length), static_cast<float>(forward.y / length),
               static_cast<float>(forward.z / length)};
    Vec3 up{0, 0, 1};
    if (std::abs(forward.z) > 0.999f)
        up = {0, 1, 0};
    Vec3 right{up.y * forward.z - up.z * forward.y, up.z * forward.x - up.x * forward.z,
               up.x * forward.y - up.y * forward.x};
    const double right_length =
        std::sqrt(static_cast<double>(right.x) * right.x + static_cast<double>(right.y) * right.y +
                  static_cast<double>(right.z) * right.z);
    if (right_length != 0)
        right = {static_cast<float>(right.x / right_length), static_cast<float>(right.y / right_length),
                 static_cast<float>(right.z / right_length)};
    up = {forward.y * right.z - forward.z * right.y, forward.z * right.x - forward.x * right.z,
          forward.x * right.y - forward.y * right.x};
    o = {right.x, right.y, right.z, forward.x, forward.y, forward.z, up.x, up.y, up.z};
    position = from;
}
bool snap_true_height(Match& match, Vec3& position) {
    if (!match.terrain)
        return false;
    TerrainPlane plane;
    if (!terrain_plane(*match.terrain, position.x, position.y, plane))
        return false;
    position.z = static_cast<float>(plane.height(position.x, position.y));
    return true;
}
int retire_empty(SpellActor& spell, ParticleSystem& system) {
    if (system.alive())
        return 1;
    spell.state_flags |= MatchObject::removed_flag;
    return -2;
}
} // namespace
bool SpellActor::casterless_skips_cast(Effect effect) {
    // 0x44C0D0 callers whose casterless stage differs from the caster stage: Scry, Cloud Kill, Tornado,
    // Pyro Explosion, Grasping Vines, Healing Aura, Rainbow, Charm, Meanstalks, Death, Intestinal
    // Vaporization, Lethargy, Bore, Bovine Intervention, Halo of Earth, Meteor Shower and Guardian.
    switch (effect) {
    case Effect::scry:
    case Effect::cloud_kill:
    case Effect::tornado:
    case Effect::pyro_explosion:
    case Effect::grasping_vines:
    case Effect::healing_aura:
    case Effect::rainbow:
    case Effect::charm:
    case Effect::divine_retribution:
    case Effect::death:
    case Effect::intestinal_vap:
    case Effect::lethargy:
    case Effect::bore:
    case Effect::bovine:
    case Effect::halo_of_earth:
    case Effect::meteor_shower:
    case Effect::guardian:
        return true;
    default:
        return false;
    }
}
SpellActor* SpellActor::launch_side_cast(Match& match, MatchSide& side, const SpellDefinition& definition,
                                         MatchObject* target, Vec3 point) {
    // 0x453600: type-32 at `point`, +476 target, +288 definition, owner 0, +480 bit 0x10000,
    // then 0x44b770 with SPEL +314. Missing run functions skip the spawn the way a zero +314 does.
    if (!definition.spell || !definition.spell->run_id)
        return nullptr;
    SpellActor::bind_functions(match);
    if (!match.functions.contains(definition.spell->run_id))
        return nullptr;
    auto& object = match.create(MatchObject::spell, [] { return std::make_unique<SpellActor>(); });
    auto& spell = static_cast<SpellActor&>(object);
    spell.data = definition.spell;
    spell.spell_id = definition.id;
    if (definition.spell->run_id == null_spell_function ||
        definition.spell->run_id == spell_function_id("NullSpellRun"))
        spell.effect = Effect::null_spell;
    spell.effect_target = target;
    spell.point = point;
    spell.motion.position = point;
    spell.object_flags |= 0x10000;
    side.attach(spell);
    match.assign(spell, definition.spell->run_id);
    spell.run(match, 0);
    return &spell;
}
SpellActor& SpellActor::launch_building_host(Match& match, BuildingActor& building, Effect effect) {
    auto& spell = spawn_child(match, building, effect);
    spell.owner = &building;
    spell.effect_target = &building;
    spell.motion.position = building.motion.position;
    spell.motion.orientation = building.motion.orientation;
    return spell;
}
SpellActor& SpellActor::launch_fireball_visual(Match& match, SpellActor& source, Effect effect) {
    auto& child = spawn_child(match, source, effect);
    child.owner = source.owner;
    child.data = source.data;
    child.motion.position = object_center(source);
    child.motion.orientation = source.motion.orientation;
    if (effect == Effect::fireball_trail)
        child.effect_target = &source;
    return child;
}
SpellActor& SpellActor::launch_sacrifice_host(Match& match, SpellActor& source, Effect effect) {
    auto& child = spawn_child(match, source, effect);
    child.owner = source.owner;
    child.effect_target = effect == Effect::altar_motes || effect == Effect::altar_cloud ||
                                  effect == Effect::altar_streamer || effect == Effect::altar_cloud_bolt ||
                                  effect == Effect::spell_hit
                              ? source.effect_target
                              : &source;
    child.data = source.data;
    child.motion.position = source.motion.position;
    child.motion.orientation = source.motion.orientation;
    return child;
}
SpellActor& SpellActor::launch_shockwave(Match& match, SpellActor& source, float radius) {
    auto& wave = spawn_child(match, source, Effect::shockwave);
    wave.owner = source.owner;
    wave.motion.position = object_center(source);
    const auto integer = [&](const char* key, int fallback) {
        return source.data ? source.data->integer(FourCC::from_text(key), fallback) : fallback;
    };
    const auto alpha = [&](const char* key, float fallback) {
        return std::clamp(source.data ? source.data->real(FourCC::from_text(key), fallback) : fallback, 0.0f,
                          1.0f) *
               255;
    };
    auto& s = wave.shockwave;
    s.texture = FourCC{static_cast<std::uint32_t>(integer("tXSt", FourCC::from_text("fwal").value))};
    const auto blend = integer("iXS+", 1);
    s.material = blend == 2 ? 0x1a010401 : blend == 1 ? 0x1a012008 : 0x02011008;
    s.start_alpha = alpha("fXSa", .25f);
    s.end_alpha = alpha("fXSe", 0);
    s.end_radius = radius;
    s.frames = match.texture_frames ? match.texture_frames(s.texture) : 1;
    return wave;
}
// 0x439d60: the full fireball sphere lasts 30 ticks, radius 1 -> SPEL radius, RGBA fading as t cubed.
int SpellActor::run_shockwave(Match&, int ticks) {
    auto& s = shockwave;
    if (s.progress >= 1) {
        state_flags |= removed_flag;
        return -2;
    }
    s.frame += ticks * .5f;
    if (s.frame >= s.frames)
        s.frame = 0;
    s.progress = std::min(1.0f, s.progress + ticks * (host.rate > 0 ? host.rate : 1.f / 30));
    s.radius = 1 + (s.end_radius - 1) * s.progress;
    const auto byte = static_cast<std::uint32_t>(
        (s.end_alpha - s.start_alpha) * s.progress * s.progress * s.progress + s.start_alpha);
    s.color = byte * 0x01010101u;
    return 1;
}
Mesh SpellActor::shockwave_mesh(const TextureHeader& texture, float projected_scale) const {
    // SPHERE::Draw 0x16024950: screen-error tessellation, with an odd ring count for spherical UVs.
    const auto& s = shockwave;
    const float bound = s.radius * std::max({s.axes.x, s.axes.y, s.axes.z});
    unsigned rings =
        static_cast<unsigned>(std::clamp(static_cast<int>(2 * bound * projected_scale / .25f), 5, 16));
    const auto columns = static_cast<unsigned>(
        std::clamp(static_cast<int>(2 * bound * projected_scale * 3.14159 / .25), 8, 32));
    if (!(rings & 1))
        ++rings;
    const auto frame = static_cast<unsigned>(s.frame) % std::max(texture.frames, 1u);
    const auto across = std::max(texture.width / std::max(texture.frame_width, 1u), 1u);
    const float su = static_cast<float>(texture.frame_width) / std::max(texture.width, 1u),
                sv = static_cast<float>(texture.frame_height) / std::max(texture.height, 1u);
    const Vec2 center{(frame % across + .5f) * su, (frame / across + .5f) * sv};
    Mesh mesh;
    const auto vertex = [&](Vec3 p, Vec2 uv) {
        const auto& origin = motion.position;
        mesh.vertices.push_back(
            {{p.x * s.axes.x + origin.x, p.z * s.axes.z + origin.z, p.y * s.axes.y + origin.y},
             {},
             uv,
             s.color});
    };
    vertex({0, 0, s.radius}, center);
    const double step = 3.14159 / (rings + 1);
    for (unsigned ring = 1; ring <= rings; ++ring) {
        const double latitude = 1.570795 + step * ring;
        const float z = static_cast<float>(std::sin(latitude) * s.radius),
                    r = static_cast<float>(std::cos(latitude) * s.radius);
        const float uv_radius =
            std::min(static_cast<float>(ring) / (rings + 1), 1 - static_cast<float>(ring) / (rings + 1)) *
            .8f;
        for (unsigned j = 0; j < columns; ++j) {
            const double angle = -6.28318 * j / (columns - 1);
            const float x = static_cast<float>(std::sin(angle)), y = static_cast<float>(std::cos(angle));
            vertex({r * x, r * y, z}, {center.x + uv_radius * su * x, center.y + uv_radius * sv * y});
        }
    }
    const auto bottom = static_cast<std::uint32_t>(mesh.vertices.size());
    vertex({0, 0, -s.radius}, center);
    for (unsigned j = 0; j + 1 < columns; ++j) {
        mesh.indices.insert(mesh.indices.end(), {0, j + 2, j + 1});
        for (unsigned ring = 0; ring + 1 < rings; ++ring) {
            const auto a = 1 + ring * columns + j, b = a + columns;
            mesh.indices.insert(mesh.indices.end(), {a, b + 1, b, a, a + 1, b + 1});
        }
        const auto a = 1 + (rings - 1) * columns + j;
        mesh.indices.insert(mesh.indices.end(), {a, a + 1, bottom});
    }
    return mesh;
}
// 0x436AC0 / 0x436910 in rising mode: particles rise from the owner's feet to height x its top.
SpellActor& SpellActor::launch_caster_column(Match& match, SpellActor& source, std::uint32_t material,
                                             FourCC texture, float rate, float size, float life, float spread,
                                             float height) {
    auto& column = spawn_child(match, source, Effect::summon_column);
    column.owner = source.owner;
    column.effect_target = source.owner;
    auto& p = column.particles(match);
    column.particle_material(match, p, material, texture);
    p.set_property(129, &size, &life);
    const auto* body = source.owner ? source.owner->source_motion() : nullptr;
    const Vec3 low = body ? body->box_min : Vec3{-spread, -spread, 0},
               high = body ? body->box_max : Vec3{spread, spread, height};
    const float a[3]{spread * low.x, spread * low.y, low.z}, b[3]{spread * high.x, spread * high.y, low.z};
    p.set_property(2, a, b);
    const double frames = life * 60.0;
    const auto acceleration = static_cast<float>(2 * (height * high.z - low.z) / (frames * frames));
    const float g0[3]{0, 0, acceleration * .95f}, g1[3]{0, 0, acceleration * 1.05f};
    p.set_property(4, g0, g1);
    column.host.rate = rate;
    return column;
}
// 0x434ab0 flags 7: god-specific hand sparks, a column around the caster and a rotating target cloud.
void SpellActor::launch_god_cast(Match& match, SpellActor& source, int god, unsigned flags) {
    static constexpr std::array<std::uint32_t, 5> textures{1668510828, 1668510832, 1668510826, 1668510835,
                                                           1718186617};
    const FourCC texture{textures.at(god - 1)};
    const auto material = god == 5 ? 0x1a010401u : 0x1a012008u;
    if (flags & 1)
        launch_cast_hands(match, source, texture, material, 1, .35f, 1, 0, .25f);
    launch_caster_column(match, source, material, texture, 1, 0, 1, 1.5f, 2);
    const float life = 1;
    if (!(flags & 4) || !source.effect_target)
        return;
    auto& cloud = spawn_child(match, source, Effect::summon_cloud);
    cloud.owner = source.owner;
    cloud.effect_target = source.effect_target;
    cloud.motion.position = source.motion.position;
    cloud.host.rate = 1;
    auto& c = cloud.particles(match);
    cloud.particle_material(match, c, material, texture);
    const float scale = .5f;
    c.set_property(132, &scale, &life);
}
int SpellActor::run_summon_host(Match& match, int ticks) {
    auto& p = particles(match);
    auto* caster = dynamic_cast<CreatureActor*>(owner);
    if (stage) {
        host.rate = 0;
        if (!p.alive()) {
            state_flags |= removed_flag;
            return -2;
        }
    } else {
        const auto state = caster ? caster->cast_state() : std::pair<unsigned, float>{1, 0};
        cast_alpha = state.second;
        if (!owner || !effect_target || state.first)
            stage = 240;
    }
    if (effect == Effect::summon_column) {
        if (const auto* target = effect_target ? effect_target->source_motion() : nullptr) {
            motion.position = target->position;
            p.set_position(motion.position);
        }
    } else {
        if (const auto* target = effect_target ? effect_target->source_motion() : nullptr) {
            const auto& low = target->box_min;
            const auto& high = target->box_max;
            const double dx = high.x - low.x, dy = high.y - low.y;
            const float radius =
                            static_cast<float>(std::sqrt(dx * dx + dy * dy) * .5 + 2.5 * (1 - cast_alpha)),
                        height = high.z - low.z + 1.5f * (1 - cast_alpha);
            const float a[3]{radius, radius, height}, b[3]{-radius, -radius, 0};
            p.set_property(2, a, b);
        }
        host.fade += ticks * (.01f + (.05f - .01f) * cast_alpha);
        motion.orientation = motion_orientation(host.fade);
    }
    host.time += ticks;
    p.advance(host.time, host.rate);
    return 1;
}
int SpellActor::run_fireball_host(Match& match, int ticks) {
    auto& system = particles(match);
    const auto texture = [&](const char* key, const char* fallback) {
        return FourCC{static_cast<std::uint32_t>(
            data ? data->integer(FourCC::from_text(key),
                                 static_cast<std::int32_t>(FourCC::from_text(fallback).value))
                 : FourCC::from_text(fallback).value)};
    };
    if (!host.initialized) {
        host.initialized = true;
        system.set_position(motion.position, &motion.position);
        if (effect == Effect::fireball_trail) { // 0x438050 / 0x437eb0.
            particle_material(match, system, 0x1a012008, texture("ttex", "fbal"));
            const float size = .5f, seconds = 1.5f;
            system.set_property(129, &size, &seconds);
            host.rate = 1.25f; // 0x4270AD: 0x438050 emission 1.25.
        } else if (effect == Effect::fireball_blast) { // 0x43b280 -> 0x43b160.
            particle_material(match, system, 0x0a011008, texture("tXbt", "frck"));
            const float size = .75f, speed = -16;
            system.set_property(130, &size, &speed);
            light = {true, motion.position, {1, .5f, 0}, 1, 1, 75};
            remaining = 120;
            launch_fireball_visual(match, *this, Effect::fireball_smoke);
            // 0x43b280 with the ordinary fireball's zero blast flags: only ground impacts scar.
            if (match.terrain &&
                motion.position.z - terrain_surface(*match.terrain, motion.position.x, motion.position.y) <
                    10)
                match.scar_requests.push_back({texture("tXsc", "scar"), motion.position,
                                               match.clock().match - static_cast<std::uint32_t>(ticks),
                                               ++match.terrain_revision});
        } else if (effect == Effect::fireball_smoke) { // 0x43b090 -> 0x43b030.
            particle_material(match, system, 0x1a012008, texture("tXcn", "exp2"));
            const float size = 7, seconds = 1;
            system.set_property(129, &size, &seconds);
            remaining = 120;
            host.rate = 2;
        } else { // 0x43b510 -> 0x43b4c0: delayed dark smoke.
            particle_material(match, system, 0x1a010401, "fb_g");
            system.cycles = 5;
            const float size = .2f, extent = 2.5f, seconds = 4;
            system.set_property(135, &size, &extent);
            system.fade(seconds, seconds);
            remaining = 200;
        }
    }
    bool switch_trail = false;
    if (effect == Effect::fireball_trail) {
        const auto* parent = dynamic_cast<const SpellActor*>(effect_target);
        if (parent && parent->model_ && !(parent->state_flags & removed_flag)) {
            const auto& box = parent->model_->bounds;
            const double x = box.maximum.x - box.minimum.x, y = box.maximum.y - box.minimum.y,
                         z = box.maximum.z - box.minimum.z;
            const auto radius =
                static_cast<float>(std::sqrt(x * x + y * y + z * z) * parent->model_scale * .5);
            const float extent = radius * .3f, low[3]{-extent, -extent, -extent},
                        high[3]{extent, extent, extent};
            const float minimum = radius * .9f, maximum = minimum * 1.2f;
            motion.position = parent->motion.position;
            system.set_position(motion.position);
            system.set_property(2, low, high);
            system.set_property(5, &minimum, &maximum);
            switch_trail = true;
        } else {
            effect_target = nullptr;
            host.rate = 0;
        }
    } else if (effect == Effect::fireball_blast) {
        light.intensity = std::max(0.0f, light.intensity - ticks * .01f);
        light.enabled = light.intensity > 0;
        if (remaining <= 90 && !host.ticks) {
            host.ticks = 1;
            if (!data || data->integer(FourCC::from_text("iXsm"), 1))
                launch_fireball_visual(match, *this, Effect::fireball_smolder);
        }
    } else if (effect == Effect::fireball_smoke && remaining < 100)
        host.rate = 0;
    host.time += static_cast<float>(ticks);
    system.advance(host.time, host.rate);
    if (effect == Effect::fireball_trail ? !effect_target && !system.alive() : (remaining -= ticks) <= 0) {
        state_flags |= removed_flag;
        return -2;
    }
    if (switch_trail) {
        // 0x437eb0: after the first follow, 0x44B770 switches to GenericTrailRun 0x437920.
        emission = host.rate;
        effect = Effect::generic_trail;
        match.assign(*this, generic_trail_function);
    }
    return 1;
}
SpellActor* SpellActor::launch_cast_hands(Match& match, SpellActor& spell) {
    // 0x435ec0 reads the spell's cast look: 'tcst' texture, 'iCH+' additive, 'fCHr' size, 'fCHv' rate,
    // 'iCHg' gravity and 'fCHs' speed.
    const auto* data = spell.data.get();
    const auto real = [&](const char* name, float fallback) {
        return data ? data->real(FourCC::from_text(name), fallback) : fallback;
    };
    const auto integer = [&](const char* name, std::int32_t fallback) {
        return data ? data->integer(FourCC::from_text(name), fallback) : fallback;
    };
    const float speed = real("fCHs", 0);
    const auto gravity = static_cast<float>(integer("iCHg", 0));
    const float rate = real("fCHv", 1), size = real("fCHr", 0.35f);
    const auto material = integer("iCH+", 1) ? 0x1a012008u : 0x0a011008u;
    const auto glow = static_cast<std::int32_t>(FourCC::from_text("glo2").value);
    const FourCC texture{static_cast<std::uint32_t>(integer("tcst", glow))};
    return launch_cast_hands(match, spell, texture, material, 1, size, rate, gravity, speed);
}
SpellActor* SpellActor::launch_cast_hands(Match& match, SpellActor& spell, FourCC texture,
                                          std::uint32_t material, float life, float size, float rate,
                                          float gravity, float speed) {
    if (!spell.owner)
        return nullptr;
    auto& hands = spawn_child(match, spell, Effect::cast_hands);
    hands.owner = spell.owner;
    hands.effect_target = spell.owner; // +476 = +460: the caster.
    hands.motion.position = spell.motion.position;
    auto& system = hands.particles(match);
    hands.particle_material(match, system, material, texture);
    hands.host.rate = rate;
    system.set_property(129, &size, &life);
    if (speed != 0) {
        const float v = static_cast<float>(speed * 0.08333333333333333);
        const float low[3]{-v, -v, -v}, high[3]{v, v, v};
        system.set_property(3, low, high);
    }
    if (gravity != 0) {
        const float g = gravity == 2 ? 0.002725f : -0.002725f;
        const float low[3]{0, 0, g}, high[3]{0, 0, g};
        system.set_property(4, low, high);
    }
    return &hands;
}
// 0x435c60: the elapsed ticks are split in two halves, emitting alternately from the caster's
// reference points 0 and 1 (the hands), until the cast resolves; then the particles drain.
int SpellActor::run_cast_hands(Match& match, int ticks) {
    auto& system = particles(match);
    auto* caster = dynamic_cast<CreatureActor*>(effect_target);
    if (caster) {
        unsigned hand = static_cast<unsigned>(host.ticks) & 1;
        for (const int half : {ticks >> 1, ticks - (ticks >> 1)}) {
            host.ticks += half;
            caster->write_reference_point(motion.position, hand, true);
            system.set_position(motion.position, &motion.position);
            host.time += static_cast<float>(half);
            system.advance(host.time, host.rate);
            hand ^= 1;
        }
        if (caster->cast_state().first)
            effect_target = nullptr;
    } else {
        host.rate = 0;
        host.time += static_cast<float>(ticks);
        system.advance(host.time, host.rate);
        if (!system.alive()) {
            state_flags |= removed_flag;
            return -2;
        }
    }
    return 1;
}
SpellActor* SpellActor::launch_glow(Match& match, SpellActor& spell, std::uint32_t material,
                                    const char* texture, float half, float speed, float scale, float life,
                                    MatchObject* target, float rate) {
    auto& glow = spawn_child(match, spell, Effect::glow);
    glow.owner = spell.owner;
    glow.effect_target = target ? target : spell.owner;
    glow.motion.position = spell.motion.position;
    // 0x436710.
    auto& system = glow.particles(match);
    glow.particle_material(match, system, material, texture);
    const float preset[3]{scale, half, life};
    system.set_property(137, preset, preset);
    if (speed > 0) {
        const float velocity[3]{speed, 0, 0};
        system.set_property(3, velocity, velocity);
        system.set_flags(0x200, 0x80000);
    }
    glow.host.rate = rate;
    return &glow;
}
// 0x436660: the glow follows its target's centre (drawn in its own frame) until the owner's cast resolves.
int SpellActor::run_glow(Match& match, int ticks) {
    auto& system = particles(match);
    if (stage == 0) {
        stage = 1;
        host.time = 0;
    }
    if (stage == 1) {
        auto* caster = dynamic_cast<CreatureActor*>(owner);
        if (!owner || !effect_target || (caster && caster->cast_state().first))
            stage = 255;
    } else if (stage == 255) {
        host.rate = 0;
        if (!system.alive()) {
            state_flags |= removed_flag;
            return -2;
        }
    }
    if (effect_target)
        motion.position = object_center(*effect_target);
    host.time += static_cast<float>(ticks);
    system.advance(host.time, host.rate);
    return 1;
}
// 0x43b5c0 manalith, 0x43b8c0 fountain, 0x43bb10 altar: particles and a light at the building's effect point.
int SpellActor::run_building_host(Match& match, int ticks) {
    auto* building = owner && effect_target ? dynamic_cast<BuildingActor*>(effect_target) : nullptr;
    auto& system = particles(match);
    if (!building) {
        host.rate = 0;
        light.enabled = false;
        if (!system.alive()) {
            state_flags |= removed_flag;
            return -2;
        }
    } else {
        if (!host.initialized) {
            host.initialized = true;
            host.time = host.fade = 0;
            host.rate = 1;
            light = {true, building->effect_point(), {}, 0, 5, 50};
            if (effect == Effect::manalith_host) {
                if (!building->allegiance)
                    throw Error("Manalith is not owned");
                light.color = side_color(building->allegiance);
                particle_material(match, system, 0x1a012009, "fb_g");
                const float preset[3]{4.75f, 25, 2};
                system.set_property(128, preset, preset);
                const auto color = particle_color(light.color);
                system.set_property(12, color.data(), color.data());
            } else if (effect == Effect::fountain_host) {
                light.color = {0, 0.5f, 1}; // unk_4CB904.
                particle_material(match, system, 0x1a012008, "elec");
                system.set_frame({motion.orientation, motion.position});
                const float scale = 3.5f, seconds = 3;
                system.set_property(131, &scale, &seconds);
                const float low[3]{0.1f, 0.1f, 0.2f}, high[3]{-0.1f, -0.1f, 0.3f};
                system.set_property(3, low, high);
            } else {
                const auto where = motion.position;
                light.color = side_color(building->allegiance);
                light.inner = 1;
                light.outer = 30;
                particle_material(match, system, 0x1a012009, "fb_g");
                const float preset[3]{1.5f, 2.5f, 2};
                system.set_property(128, preset, preset);
                const float position[3]{where.x, where.y, where.z};
                system.set_property(0, position, position);
                const auto color = particle_color(light.color);
                system.set_property(12, color.data(), color.data());
            }
        }
        if (effect == Effect::altar_host) {
            const bool built = building->reveal == 1;
            host.rate = built ? 1.0f : 0.0f;
            // 0x4654e0: a ritual on the altar brightens it fully, otherwise a quarter.
            const float target = built ? (building->ritual.victim ? 1.0f : 0.25f) : 0.0f;
            const float before = host.fade;
            fade_light(light, host.fade, target, ticks * 0.01f);
            if (before != host.fade || target != before) {
                light.inner = 1;
                light.outer = host.fade * 20 + 30;
                const float rise = target * 12.5f + 2.5f;
                const float low[3]{0, 0, rise * 0.005555555555555556f},
                    high[3]{0, 0, rise * 0.008333333333333333f};
                system.set_property(3, low, high);
            }
            light.intensity =
                static_cast<float>((match.visual_random.next() * 0.00000762939453125 + 0.75) * host.fade);
        } else {
            const auto where = building->effect_point();
            motion.position = where;
            light.position = where;
            const float position[3]{where.x, where.y, where.z};
            system.set_property(0, position, nullptr);
            const bool active = effect == Effect::manalith_host ? building->reveal == 1 : !building->reserved;
            host.rate = active ? 1.0f : 0.0f;
            fade_light(light, host.fade, host.rate, ticks * 0.01f);
            light.intensity = host.fade;
        }
    }
    host.time += static_cast<float>(ticks);
    system.advance(host.time, host.rate);
    return 1;
}
SpellActor& SpellActor::launch_respawn_host(Match& match, CreatureActor& wizard, Effect effect) {
    auto& host = spawn_child(match, wizard, effect);
    host.owner = &wizard;
    host.effect_target = &wizard;
    host.motion.position = wizard.motion.position;
    return host;
}
// 0x438d60(wizard, 0x1a012008, 'glo2', 'Xltn', 64, 0.5, 2, 1, 1, 2, wizard) with the 0x4388b0 run: 0x438c00
// fills the wizard's box with 64 glows that flag 0x200 drives outward to twice their offset over a life,
// falling with the shared particle gravity. The 'Xltn' sound stays with the handler's combat request.
SpellActor& SpellActor::launch_respawn_pulse(Match& match, CreatureActor& wizard) {
    auto& pulse = spawn_child(match, wizard, Effect::spell_hit);
    pulse.owner = &wizard;
    pulse.motion.position = wizard.motion.position;
    auto& p = pulse.particles(match);
    p.set_position(pulse.motion.position);
    pulse.particle_material(match, p, 0x1a012008, "glo2");
    const float size = .5f, life = 2;
    p.set_property(129, &size, &life);
    const auto& box = wizard.motion;
    const float low[3]{box.box_min.x, box.box_min.y, box.box_min.z},
        high[3]{box.box_max.x, box.box_max.y, box.box_max.z};
    p.set_property(2, low, high);
    const float speed[3]{2, 0, 0}, none[3]{};
    p.set_property(3, speed, none);
    const float gravity[3]{0, 0, -.002725f};
    p.set_property(4, gravity, gravity);
    p.set_flags(0x10200, 0);
    for (int i = 0; i < 64; ++i)
        p.emit(0, 1);
    pulse.stage = 1; // Configured here; 0x4388b0 only advances and retires the burst.
    return pulse;
}
// 0x43ad00 (yellow light and box glows) and 0x43aee0 (falling wisps from the centre plane) follow a visible
// spirit; once it is gone or restored (stage 255) they stop emitting and retire when their particles are.
int SpellActor::run_respawn_host(Match& match, int ticks) {
    auto& p = particles(match);
    auto* wizard = dynamic_cast<CreatureActor*>(effect_target);
    const bool body = effect == Effect::respawn_body;
    if (stage == 255) {
        host.rate = 0;
        if (!p.alive()) {
            state_flags |= removed_flag;
            return -2;
        }
    } else if (!stage) {
        if (body) {
            if (wizard)
                motion.position = wizard->motion.position;
            light.intensity = 0;
            light.position = motion.position;
            light.color = {1, 1, 0}; // 0x4cb8ec.
            light.inner = 1;
            light.outer = 50;
            light.enabled = true;
            particle_material(match, p, 0x1a012008, "glo2");
            const float size = .5f, life = 2;
            p.set_property(129, &size, &life);
            if (wizard) {
                const auto& box = wizard->motion;
                const float low[3]{box.box_min.x, box.box_min.y, box.box_min.z},
                    high[3]{box.box_max.x, box.box_max.y, box.box_max.z};
                p.set_property(2, low, high);
            }
        } else {
            if (wizard)
                motion.position = object_center(*wizard);
            particle_material(match, p, 0x1a012008, "glo2");
            const float size = .35f, life = 1;
            p.set_property(129, &size, &life);
            if (wizard) {
                const auto& box = wizard->motion;
                const float low[3]{box.box_min.x, box.box_min.y, 0}, high[3]{box.box_max.x, box.box_max.y, 0};
                p.set_property(2, low, high);
            }
            const float gravity[3]{0, 0, -.002725f};
            p.set_property(4, gravity, gravity);
        }
        stage = 1;
        host.rate = 1;
    }
    const auto* state = wizard && (wizard->state_flags & ran_flag) ? wizard->fsm().state() : nullptr;
    const auto fsm = state ? state->flags : 0u;
    // 0x459b30: hidden spirits of other sides and suppressed objects show nothing.
    const bool visible = !((fsm & 0x80000) && wizard->allegiance != match.local_side) && !(fsm & 0x1000000) &&
                         !(wizard && (wizard->state_flags & 0x1000000));
    if (state && (fsm & 0x40000) && (body ? visible : !(fsm & 0x80000))) {
        if (body) {
            motion.position = wizard->motion.position;
            p.set_position(motion.position);
            const auto& box = wizard->motion;
            const float low[3]{box.box_min.x, box.box_min.y, box.box_min.z},
                high[3]{box.box_max.x, box.box_max.y, box.box_max.z};
            p.set_property(2, low, high);
            light.position = motion.position;
        } else {
            motion.position = object_center(*wizard);
            p.set_position(motion.position);
        }
    } else
        stage = 255;
    if (body) { // The light intensity follows the emission rate by 1/60 per tick.
        const float step = ticks / 60.0f;
        light.intensity = light.intensity < host.rate ? std::min(light.intensity + step, host.rate)
                                                      : std::max(light.intensity - step, host.rate);
    }
    host.time += static_cast<float>(ticks);
    p.advance(host.time, host.rate);
    return 1;
}
SpellActor* SpellActor::launch_generic_trail(Match& match, SpellActor& source, std::uint32_t material,
                                             FourCC texture, float size, float emission_rate, float life,
                                             float speed, float angle) {
    // 0x437DD0: type-32 GenericTrailRun with identity frame, preset 131, and a yaw-scaled velocity box.
    auto& child = spawn_child(match, source, Effect::generic_trail);
    child.owner = source.owner;
    child.effect_target = &source;
    child.data = source.data;
    child.motion.position = source.motion.position;
    child.motion.orientation = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    auto& system = child.particles(match);
    child.particle_material(match, system, material, texture);
    system.set_property(131, &size, &life);
    const float lateral = std::abs(std::sin(angle)) * speed, along = std::abs(std::cos(angle)) * speed;
    const float low[3]{-lateral, -lateral, along}, high[3]{lateral, lateral, speed};
    system.set_property(3, low, high);
    const ParticleSystem::Frame frame{child.motion.orientation, child.motion.position};
    system.set_frame(frame, &frame);
    child.emission = emission_rate;
    child.host.rate = emission_rate;
    return &child;
}
namespace {
// 0x437EB0: r is half the diagonal of the source model's bounds (its MRM instance, so scaled), falling
// back to the collision box; sizes clamp(r * size, .25, 2) * [.9, 1.08] in a +-r * box cube.
void size_model_trail(SpellActor& trail, const SpellActor& source, ParticleSystem& system) {
    Vec3 low = source.motion.box_min, high = source.motion.box_max;
    if (const auto* model = source.model_geometry()) {
        const float scale = source.model_scale;
        low = {model->bounds.minimum.x * scale, model->bounds.minimum.y * scale, model->bounds.minimum.z * scale};
        high = {model->bounds.maximum.x * scale, model->bounds.maximum.y * scale, model->bounds.maximum.z * scale};
    }
    const float r = std::sqrt((high.x - low.x) * (high.x - low.x) + (high.y - low.y) * (high.y - low.y) +
                              (high.z - low.z) * (high.z - low.z)) * .5f;
    const float size = std::clamp(r * trail.ability.draw_back, .25f, 2.f), small = size * .9f, large = small * 1.2f;
    const float half = r * trail.ability.length, box_low[3]{-half, -half, -half}, box_high[3]{half, half, half};
    system.set_property(2, box_low, box_high);
    system.set_property(5, &small, &large);
}
} // namespace
SpellActor* SpellActor::launch_model_trail(Match& match, SpellActor& source, std::uint32_t material,
                                           FourCC texture, float emission, float life, float box_factor,
                                           float size_factor, bool resize) {
    auto& child = spawn_child(match, source, Effect::generic_trail);
    child.owner = source.owner;
    child.effect_target = &source;
    child.data = source.data;
    child.motion.position = source.motion.position;
    auto& system = child.particles(match);
    child.particle_material(match, system, material, texture);
    system.set_property(129, &size_factor, &life);
    // ability.length / draw_back keep the box and size factors; clock 1 sizes on the first run (native
    // sizes there, after the source's model is in place), 2 keeps re-sizing each tick (0x438050 flag 1).
    child.ability.length = box_factor;
    child.ability.draw_back = size_factor;
    child.ability.clock = resize ? 3 : 1;
    size_model_trail(child, source, system);
    system.set_position(child.motion.position);
    child.emission = child.host.rate = emission;
    return &child;
}
SpellActor* SpellActor::launch_box_trail(Match& match, SpellActor& source, std::uint32_t material,
                                         FourCC texture, float half, float size, float life, float rate) {
    auto& child = spawn_child(match, source, Effect::generic_trail);
    child.owner = source.owner;
    child.effect_target = &source;
    child.data = source.data;
    child.motion.position = source.motion.position;
    auto& system = child.particles(match);
    child.particle_material(match, system, material, texture);
    system.set_property(129, &size, &life);
    const float box_low[3]{-half, -half, -half}, box_high[3]{half, half, half};
    system.set_property(2, box_low, box_high);
    system.set_position(child.motion.position);
    child.emission = child.host.rate = rate;
    return &child;
}
SpellActor* SpellActor::launch_ground_trail(Match& match, SpellActor& source, std::uint32_t material,
                                            FourCC texture, Vec3 low, Vec3 high, float emission_rate,
                                            float life) {
    auto& child = spawn_child(match, source, Effect::generic_ground_trail);
    child.owner = source.owner;
    child.effect_target = &source;
    child.data = source.data;
    child.motion.position = source.motion.position;
    child.motion.orientation = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    auto& system = child.particles(match);
    child.particle_material(match, system, material, texture);
    const float x = high.x - low.x, y = high.y - low.y, z = high.z - low.z;
    const float size = std::clamp(std::sqrt(x * x + y * y + z * z) * .75f * .5f, .25f, 2.f);
    system.set_property(129, &size, &life);
    const float box_low[]{low.x, low.y, low.z}, box_high[]{high.x, high.y, high.z};
    system.set_property(2, box_low, box_high);
    // The host follows the terrain; preset 129's small Z offsets straddle it.
    // Particle flag 0x1000 adds an entire sprite radius and makes this dust float.
    snap_true_height(match, child.motion.position);
    system.set_position(child.motion.position);
    child.emission = child.host.rate = emission_rate;
    return &child;
}
void SpellActor::launch_earth_burst(Match& match, SpellActor& source, Vec3 position, float radius,
                                    unsigned count) {
    auto& child = spawn_child(match, source, Effect::generic_static);
    child.owner = source.owner;
    child.motion.position = position;
    auto& system = child.particles(match);
    child.particle_material(match, system, 0x0a011008,
                            FourCC{static_cast<unsigned>(source.data->integer(
                                FourCC::from_text("tcld"), FourCC::from_text("dust").value))});
    const float size = radius * .5f, life = 1.5f;
    system.set_property(131, &size, &life);
    const float low[]{-radius * .5f, -radius * .5f, 0.f}, high[]{radius * .5f, radius * .5f, radius * .25f};
    system.set_property(2, low, high);
    const float slow[]{-.06f, -.06f, .01f}, fast[]{.06f, .06f, .12f};
    system.set_property(3, slow, fast);
    system.set_position(position);
    for (unsigned n = 0; n < count; ++n)
        system.emit(0, 1);
}
SpellActor* SpellActor::launch_inert_fire(Match& match, SpellActor& source, FourCC texture, float size,
                                          float height, float life, float emission, float seconds,
                                          float spread) {
    // 0x4375A0: type-42 child at the source, additive material, preset 128 {size, height, life}, then
    // INERT_RunFire with emission rate +32 and duration +44. 0x44ca20 adds a dword_4CB8E0 light that
    // starts dark and reaches max(spread, height) * 5.
    auto& child = spawn_child(match, source, Effect::inert_fire);
    child.owner = source.owner;
    child.data = source.data;
    child.motion.position = source.motion.position;
    child.motion.orientation = source.motion.orientation;
    auto& system = child.particles(match);
    child.particle_material(match, system, 0x1a012008, texture);
    const float preset[3]{size, height, life};
    system.set_property(128, preset, preset);
    const float at[3]{child.motion.position.x, child.motion.position.y, child.motion.position.z};
    system.set_property(0, at, at);
    if (spread != 0) {
        const float low[3]{-spread, -spread, -spread * .25f}, high[3]{spread, spread, spread * .25f};
        system.set_property(2, low, high);
    }
    child.emission = child.host.rate = emission;
    child.remaining = static_cast<int>(seconds * 60.0);
    child.host.fade = 0; // Context +48: light intensity.
    child.light = {true, child.motion.position, {1, .5f, 0}, 0, 0, std::max(spread, height) * 5};
    return &child;
}
int SpellActor::run_inert_fire(Match& match, int ticks) {
    // INERT_RunFire 0x437470: brighten at 1/30 per tick while the burst lasts, then stop emitting, dim at
    // 1/60 per tick and retire once the light is dark and the particles are gone.
    auto& system = particles(match);
    remaining -= ticks;
    if (remaining > 0)
        host.fade = static_cast<float>(std::min(ticks * 0.03333333333333333 + host.fade, 1.0));
    else {
        emission = 0;
        host.fade = static_cast<float>(std::max(host.fade - ticks * 0.01666666666666667, 0.0));
        if (!system.alive() && host.fade == 0) {
            light.enabled = false;
            state_flags |= removed_flag;
            return -2;
        }
    }
    light.intensity = host.fade;
    host.time += ticks;
    system.advance(host.time, emission);
    return 1;
}
SpellActor* SpellActor::launch_generic_moving(Match& match, SpellActor& source, std::uint32_t material,
                                              FourCC texture, const Vec3& from, const Vec3& to, float speed,
                                              float rise, float radius, float size_end, float life,
                                              float emission_rate, float seconds) {
    // 0x43A5E0: type-32 GenericMovingPsysRun with preset 134, oriented from `from` toward `to`.
    auto& child = spawn_child(match, source, Effect::generic_moving);
    child.owner = source.owner;
    child.effect_target = &source;
    child.data = source.data;
    look_between(child.motion.orientation, child.motion.position, from, to);
    auto& system = child.particles(match);
    child.particle_material(match, system, material, texture);
    system.set_property(134, &radius, &life);
    const ParticleSystem::Frame frame{child.motion.orientation, child.motion.position};
    system.set_frame(frame, &frame);
    system.set_property(6, &size_end, &size_end);
    const float vertical = life != 0 ? rise / (life * 60.f) : 0;
    const float velocity[3]{0, vertical, 0};
    system.set_property(3, velocity, velocity);
    if (speed != 0 && seconds != 0) {
        const float scale = speed / (seconds * 60.f);
        const auto& o = child.motion.orientation;
        child.object_flags |= 1;
        child.motion.velocity = {scale * o[3], scale * o[4], scale * o[5]};
    }
    child.remaining = static_cast<int>(seconds * 60.f);
    child.emission = emission_rate;
    child.host.rate = emission_rate;
    return &child;
}
int SpellActor::run_generic_particles(Match& match, int ticks) {
    if (effect == Effect::null_spell) {
        launch_cast_hands(match, *this);
        match.assign(*this, 0);
        return 1;
    }
    auto& system = particles(match);
    if (effect == Effect::generic_trail || effect == Effect::generic_ground_trail) {
        auto* target = effect_target;
        if (target && !(target->state_flags & removed_flag)) {
            if (const auto* body = target->source_motion())
                motion.position = body->position;
            if (auto* source = dynamic_cast<SpellActor*>(target); source && (ability.clock & 3)) {
                size_model_trail(*this, *source, system);
                ability.clock &= ~1;
            }
            if (effect == Effect::generic_ground_trail)
                emission = snap_true_height(match, motion.position) ? host.rate : 0;
            system.set_position(motion.position);
        } else {
            effect_target = nullptr;
            emission = 0;
            if (int gone = retire_empty(*this, system); gone == -2)
                return gone;
        }
        host.time += ticks;
        system.advance(host.time, emission);
        return 1;
    }
    host.time += ticks;
    system.advance(host.time, emission);
    if (effect == Effect::generic_moving)
        system.set_position(motion.position);
    remaining -= ticks;
    if (remaining <= 0) {
        emission = 0;
        return retire_empty(*this, system);
    }
    return 1;
}
} // namespace sacrifice
