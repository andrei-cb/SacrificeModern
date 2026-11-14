#include "sacrifice/scene_effects.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/saxs.hpp"
#include "sacrifice/scene.hpp"
#include "sacrifice/soul_actor.hpp"
#include "sacrifice/spell_actor.hpp"
#include "remaster_tuning.hpp"
#include <algorithm>
#include <cmath>
#include <map>
namespace sacrifice {
PointLight soul_light(const SoulActor& soul) {
    // 0x475c90 starts the soul's light through 0x44ca20 (on, ranges 1 / 25); each 0x475ee0 run sets
    // LIGHT3D::SetBaseColor(a x 255, 192 - a x 192, 255 - a x 255) / 256 for alignment a (blue for the
    // local side, red otherwise) and SetPosAndIntensity(position, growth x 0.5).
    const double a = soul.visual.alignment;
    PointLight light;
    light.enabled = true;
    light.position = soul.motion.position;
    light.color = {static_cast<float>(a * 255 * .00390625), static_cast<float>((192 - a * 192) * .00390625),
                   static_cast<float>((255 - a * 255) * .00390625)};
    light.intensity = static_cast<float>(soul.visual.growth * .5);
    light.inner = 1;
    light.outer = 25;
    return light;
}
void append_creature_shadows(Match& match, const Camera& camera, const RenderDetail& detail,
                             std::vector<ParticleBatch>& batches) {
    // DrawFaces 0x16028bf0 draws the shadow triangles with material 0x18000000: opaque, two-sided, black.
    ParticleBatch batch{0x18000000u, nullptr, {}, {}};
    auto& mesh = batch.mesh;
    const Vec3 forward{std::sin(camera.yaw) * std::cos(camera.pitch), std::sin(camera.pitch),
                       std::cos(camera.yaw) * std::cos(camera.pitch)};
    for (const auto& object : match.objects()) {
        auto* creature = dynamic_cast<CreatureActor*>(object.get());
        if (!creature || !creature->has_model() || (creature->state_flags & MatchObject::removed_flag) ||
            creature->fade != 0 || (creature->state_flags & 4))
            continue;
        const auto& plane = creature->motion.plane;
        if (plane.distance == 0 || plane.normal.z == 0)
            continue;
        const auto* bounds = creature->prepared_bounds();
        if (!bounds)
            continue;
        const auto shown = match.presented(*creature);
        const auto& m = shown.orientation;
        const auto& at = shown.position;
        // SAXSINSTANCE::Draw passes 1/sqrt(view detail), clamped to [1, 100]; SetupShadow needs < 3.33333.
        const auto& box = bounds->visual;
        const float dx = box.maximum.x - box.minimum.x, dy = box.maximum.y - box.minimum.y,
                    dz = box.maximum.z - box.minimum.z;
        const float radius = std::max(.5f * std::sqrt(dx * dx + dy * dy + dz * dz), .01f);
        const float depth = (at.x - camera.position.x) * forward.x + (at.z - camera.position.y) * forward.y +
                            (at.y - camera.position.z) * forward.z;
        if (depth + radius < 0 || std::sqrt(std::max(detail.quality(depth - radius, radius), 0.f)) <= .3f)
            continue;
        // The ground point under the model origin, 0.025 above the plane, and the plane in local space.
        const Vec3 ground{at.x, at.y, static_cast<float>(plane.height(at.x, at.y) + .025)};
        const auto to_local = [&](Vec3 v) {
            return Vec3{v.x * m[0] + v.y * m[1] + v.z * m[2], v.x * m[3] + v.y * m[4] + v.z * m[5],
                        v.x * m[6] + v.y * m[7] + v.z * m[8]};
        };
        const auto normal = to_local(plane.normal);
        const auto origin = to_local({ground.x - at.x, ground.y - at.y, ground.z - at.z});
        const double distance = double(normal.x) * origin.x + double(normal.y) * origin.y + double(normal.z) * origin.z;
        const auto points = saxs_shadow(creature->body()->model, creature->model_pose(), normal, distance);
        for (std::size_t first = 0; first + 8 <= points.size(); first += 8) {
            const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
            for (std::size_t i = 0; i < 8; ++i) {
                const auto& p = points[first + i];
                // Local Z-up row vector times the orientation, then renderer (x, z, y).
                const Vec3 w{p.x * m[0] + p.y * m[3] + p.z * m[6] + at.x, p.x * m[1] + p.y * m[4] + p.z * m[7] + at.y,
                             p.x * m[2] + p.y * m[5] + p.z * m[8] + at.z};
                mesh.vertices.push_back({{w.x, w.z, w.y}, {0, 1, 0}, {}, 0xff000000u});
            }
            for (const auto index : saxs_shadow_band)
                mesh.indices.push_back(base + index);
        }
    }
    if (!mesh.indices.empty())
        batches.push_back(std::move(batch));
}
void update_weather_look(WeatherLook& look, const Match& match) {
    const auto tick = match.clock().total;
    const float seconds = look.tick == UINT64_MAX || tick < look.tick ? 0 : (tick - look.tick) / 60.0f;
    const bool first = look.tick == UINT64_MAX;
    look.tick = tick;
    const auto& effects = match.weather.effects;
    if (first) { // Start settled: a game loaded mid-storm already has wet ground or snow.
        look.wet = effects[0].intensity;
        look.snow = effects[1].intensity;
    }
    const auto toward = [&](float& value, float target, float rise, float fall) {
        value = value < target ? std::min(target, value + seconds / rise)
                               : std::max(target, value - seconds / fall);
    };
    toward(look.wet, effects[0].intensity, tuning::wet_rise_seconds, tuning::wet_dry_seconds);
    toward(look.snow, effects[1].intensity, tuning::snow_rise_seconds, tuning::snow_melt_seconds);
    look.ash = effects[4].intensity;
    look.flash = effects[0].flash / 15.0f;
    look.storm = std::max(effects[0].intensity, effects[2].intensity); // Rain or hail.
}
namespace {
// A small hash stream for drawing-only variation (never the match's random stream).
struct SplashHash {
    unsigned state;
    float operator()() {
        state ^= state >> 15;
        state *= 0x2c1b3c6du;
        state ^= state >> 12;
        state *= 0x297a2d39u;
        state ^= state >> 15;
        return (state & 0xffff) / 65535.0f;
    }
};
// One remastered rain splash at `phase` (0..1) of its life: a widening, fading ring on the ground with
// droplets thrown up early on.
void draw_splash(Mesh& mesh, float x, float y, float ground, float phase, SplashHash& next) {
    constexpr unsigned segments = 12;
    const float angle = next() * 6.2831853f;
    ground += .03f;
    const float radius = tuning::splash_size * (.3f + .7f * std::sqrt(phase)) * (.7f + .6f * next());
    const auto alpha = static_cast<std::uint32_t>(
        std::clamp(std::lround(255 * tuning::splash_opacity * (1 - phase) * (1 - phase)), 0L, 255L));
    if (!alpha)
        return;
    const std::uint32_t colour = 0xf0e8e0u | alpha << 24, clear = 0x00f0e8e0u;
    const auto base = static_cast<unsigned>(mesh.vertices.size());
    const float inner = radius * .55f;
    for (unsigned i = 0; i < segments; ++i) { // The ring: an inner (clear) and outer (opaque) loop.
        const float a = 6.2831853f * i / segments, c = std::cos(a), s = std::sin(a);
        mesh.vertices.push_back({{x + c * inner, ground, y + s * inner}, {}, {}, clear});
        mesh.vertices.push_back({{x + c * radius, ground, y + s * radius}, {}, {}, colour});
    }
    for (unsigned i = 0; i < segments; ++i) {
        const unsigned a = base + i * 2, b = base + (i + 1) % segments * 2;
        mesh.indices.insert(mesh.indices.end(), {a, a + 1, b + 1, a, b + 1, b});
    }
    if (phase < .5f) { // Droplets thrown up and out, falling back.
        const float t = phase / .5f, height = tuning::splash_size * .8f * 4 * t * (1 - t);
        for (unsigned i = 0; i < 3; ++i) {
            const float a = angle + i * 2.0943951f + next(), c = std::cos(a), s = std::sin(a);
            const float out = radius * .8f, top = ground + height, half = .02f;
            const auto at = static_cast<unsigned>(mesh.vertices.size());
            mesh.vertices.push_back({{x + c * out - s * half, top, y + s * out + c * half}, {}, {}, colour});
            mesh.vertices.push_back({{x + c * out + s * half, top, y + s * out - c * half}, {}, {}, colour});
            mesh.vertices.push_back({{x + c * out * .6f, ground, y + s * out * .6f}, {}, {}, clear});
            mesh.indices.insert(mesh.indices.end(), {at, at + 1, at + 2});
        }
    }
}
// The splash of one drawn raindrop (map coordinates, Z up), drawing only. Rain falls straight down
// through the ground, so a drop that has just passed below it splashes at its column; how far below
// gives the splash's age. `fall` is the drop's fall per tick.
void append_drop_splash(const Match& match, Vec3 at, float fall, unsigned seed, Mesh& mesh) {
    const float ground = terrain_height(*match.terrain, at.x, at.y);
    const float life = std::max(tuning::splash_seconds, .05f) * 60 * fall;
    const float depth = ground - at.z + match.presentation_blend() * fall;
    if (depth < 0 || depth >= life)
        return;
    SplashHash next{seed * 0x9e3779b9u + 0x7f4a7c15u};
    if (next() >= tuning::splash_share)
        return;
    draw_splash(mesh, at.x, at.y, ground, depth / life, next);
}
// Remastered far rain splashes, drawing only, for ground the drawn drops don't reach: beyond the drop
// volume (`box` around the eye) or below it. The ground is divided into fixed world cells, so splashes
// stay where they land as the camera moves; the cells within splash_radius of a point ahead of the camera
// each hold a few slots. Each slot replays a splash at a hashed spot in its cell per cycle (from the
// match clock, not the random stream).
void append_rain_splashes(const Match& match, Vec3 eye, float yaw, float intensity, float box, Mesh& mesh) {
    constexpr float cell = 4.0f;
    const float reach = tuning::splash_radius;
    const float density = tuning::splash_count * intensity * cell * cell / (3.1415927f * reach * reach);
    if (density <= 0)
        return;
    const float cx = eye.x + std::sin(yaw) * reach * .8f, cy = eye.y + std::cos(yaw) * reach * .8f;
    const int x0 = static_cast<int>(std::floor((cx - reach) / cell)),
              y0 = static_cast<int>(std::floor((cy - reach) / cell));
    const auto nx = static_cast<unsigned>(std::floor((cx + reach) / cell) - x0 + 1),
               ny = static_cast<unsigned>(std::floor((cy + reach) / cell) - y0 + 1),
               slots = static_cast<unsigned>(std::ceil(density));
    const float now = (match.clock().total + match.presentation_blend()) / 60.0f;
    const float life = std::max(tuning::splash_seconds, .05f);
    for (unsigned item = 0; item < nx * ny * slots; ++item) {
        const int gx = x0 + static_cast<int>(item % nx), gy = y0 + static_cast<int>(item / nx % ny);
        const unsigned slot = item / (nx * ny);
        SplashHash next{static_cast<unsigned>(gx) * 0x9e3779b9u ^ static_cast<unsigned>(gy) * 0x85ebca77u ^
                        (slot + 1) * 0xc2b2ae3du};
        if (next() >= density - slot) // The last slot covers the fraction left over.
            continue;
        const float cycle = now / life + next(); // Slots start out of step.
        const auto round = static_cast<unsigned>(cycle);
        const float phase = cycle - static_cast<float>(round);
        next.state ^= round * 0x27d4eb2fu;
        const float x = (gx + next()) * cell, y = (gy + next()) * cell;
        if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > reach * reach)
            continue;
        const float ground = terrain_height(*match.terrain, x, y);
        // Ground the drawn drops reach splashes from them instead.
        if (std::fabs(x - eye.x) < box && std::fabs(y - eye.y) < box && ground > eye.z - box)
            continue;
        draw_splash(mesh, x, y, ground, phase, next);
    }
}
} // namespace
void append_match_effects(Match& match, const Camera& camera, const EffectTextureResolver& texture_for,
                          std::vector<ParticleBatch>& batches, std::vector<PointLight>& lights, bool remastered) {
    {
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> index;
        const auto byte = [](float v) {
            return static_cast<std::uint32_t>(std::clamp(std::lround(v), 0L, 255L));
        };
        for (const auto& object : match.objects()) {
            if (const auto* soul = dynamic_cast<const SoulActor*>(object.get())) {
                if (!(soul->state_flags & MatchObject::removed_flag))
                    lights.push_back(soul_light(*soul));
                continue;
            }
            auto* spell = dynamic_cast<SpellActor*>(object.get());
            if (!spell)
                continue;
            // Presentation only (not native): the spell's drawn pose between ticks (Match::presented). Its
            // light, sprites, local-frame particles and rigid meshes move with it; world particles are
            // drawn back along their velocity by the part of the tick not yet reached.
            const auto shown = match.presented(*spell);
            const Vec3 shift{shown.position.x - spell->motion.position.x,
                             shown.position.y - spell->motion.position.y,
                             shown.position.z - spell->motion.position.z};
            const float behind = 1 - match.presentation_blend();
            const auto moved = [&](Mesh& mesh) {
                if (shift.x || shift.y || shift.z)
                    for (auto& vertex : mesh.vertices)
                        vertex.position = {vertex.position.x + shift.x, vertex.position.y + shift.z,
                                           vertex.position.z + shift.y};
            };
            if (spell->light.enabled && !(spell->state_flags & MatchObject::removed_flag)) {
                auto light = spell->light;
                light.position = {light.position.x + shift.x, light.position.y + shift.y,
                                  light.position.z + shift.z};
                lights.push_back(light);
            }
            if (!(spell->state_flags & MatchObject::removed_flag)) {
                if (spell->surface.kind != SpellActor::CurvedSurface::none) {
                    const unsigned layers = spell->surface.kind == SpellActor::CurvedSurface::tornado ? 3u
                                            : spell->surface.kind == SpellActor::CurvedSurface::lathe ||
                                                    spell->surface.kind == SpellActor::CurvedSurface::tentacle
                                                ? 2u
                                                : 1u;
                    for (unsigned layer = 0; layer < layers; ++layer) {
                        const auto& texture = texture_for(spell->surface.textures[layer]);
                        if (texture.view) {
                            ParticleBatch batch{spell->surface.material, texture.view, {}, {}};
                            batch.mesh = spell->curved_surface_mesh(
                                layer, texture.header, 1, match.terrain,
                                Vec3{camera.position.x, camera.position.z, camera.position.y},
                                Vec3{std::cos(camera.yaw), -std::sin(camera.yaw), 0});
                            batches.push_back(std::move(batch));
                        }
                    }
                }
                if (!spell->land_block.faces.empty()) {
                    // LANDBLOCK::Draw 0x16018130: each quadrant with its cell's map texture (material
                    // 65537), then the walls with the edge texture.
                    for (unsigned part = 0; part < 5; ++part) {
                        if (part == 4 && spell->land_block.walls.empty())
                            continue;
                        const FourCC id{part < 4 ? effect_land_tile |
                                                       static_cast<std::uint32_t>(spell->land_block.cells[part])
                                                 : effect_land_edge};
                        const auto& texture = texture_for(id);
                        if (!texture.view)
                            continue;
                        // The walls' winding is not checked against D3D's, so they draw both sides.
                        ParticleBatch batch{part < 4 ? 0x00010001u : 0x08010001u, texture.view, {}, {}};
                        batch.mesh = spell->land_block_mesh(part);
                        moved(batch.mesh);
                        if (!batch.mesh.indices.empty())
                            batches.push_back(std::move(batch));
                    }
                }
                if (spell->altar_cloud.enabled && spell->altar_cloud.noise &&
                    spell->altar_cloud.opacity > 0) {
                    const auto& cloud = spell->altar_cloud;
                    const auto& texture = texture_for(cloud.texture);
                    if (texture.view || !cloud.texture.value) {
                        const auto& pos = spell->motion.position;
                        const Vec3 eye{camera.position.x, camera.position.z, camera.position.y};
                        const double fx = std::sin(camera.yaw) * std::cos(camera.pitch),
                                     fy = std::cos(camera.yaw) * std::cos(camera.pitch),
                                     fz = std::sin(camera.pitch);
                        const double nearest =
                            (pos.x - eye.x) * fx + (pos.y - eye.y) * fy + (pos.z - eye.z) * fz -
                            1.15 * (std::abs(fx) * cloud.radii.x + std::abs(fy) * cloud.radii.y +
                                    std::abs(fz) * cloud.radii.z);
                        const auto detail =
                            static_cast<float>(nearest <= 10 ? 1 : std::pow(.5, (nearest - 10) / 10));
                        ParticleBatch batch{cloud.material, texture.view, {}, {}};
                        auto header = texture.header;
                        if (!cloud.texture.value)
                            header.width = header.height = header.frames = header.frame_width =
                                header.frame_height = 1;
                        batch.mesh = cloud_mesh(*cloud.noise, cloud.radii, cloud.phase, cloud.opacity, pos,
                                                eye, header, static_cast<unsigned>(cloud.frame), detail);
                        batches.push_back(std::move(batch));
                    }
                }
                if (spell->altar_streamer.enabled) {
                    const auto& streamer = spell->altar_streamer;
                    const auto& texture = texture_for(streamer.texture);
                    if (texture.view) {
                        ParticleBatch batch{streamer.material, texture.view, {}, {}};
                        batch.mesh = streamer.primitive.mesh(texture.header);
                        batches.push_back(std::move(batch));
                    }
                }
                for (const auto* layer : {&spell->sprite, &spell->sprite_overlay}) {
                    const auto& sprite = *layer;
                    if (!sprite.enabled)
                        continue;
                    const auto& texture = texture_for(sprite.texture);
                    if (texture.view) {
                        const auto& h = texture.header;
                        const unsigned columns = std::max(h.width / std::max(h.frame_width, 1u), 1u);
                        const auto frame = static_cast<unsigned>(sprite.frame) % std::max(h.frames, 1u);
                        const float su = static_cast<float>(h.frame_width) / h.width,
                                    sv = static_cast<float>(h.frame_height) / h.height;
                        const float u = (frame % columns) * su, v = (frame / columns) * sv;
                        ParticleBatch batch{sprite.material, texture.view, {}, {}};
                        if (sprite.flag) {
                            // FLAG::Draw grid (as for souls), rotated into the camera's right/up/forward.
                            SoulState state;
                            state.growth = 1;
                            state.scale = sprite.size / 1.75f;
                            state.phase_x = sprite.phase_x;
                            state.phase_y = sprite.phase_y;
                            state.frame = static_cast<float>(frame);
                            batch.mesh = soul_mesh(state, h);
                            const Vec3 right{std::cos(camera.yaw), -std::sin(camera.yaw), 0},
                                up{-std::sin(camera.yaw) * std::sin(camera.pitch),
                                   -std::cos(camera.yaw) * std::sin(camera.pitch), std::cos(camera.pitch)},
                                forward{std::sin(camera.yaw) * std::cos(camera.pitch),
                                        std::cos(camera.yaw) * std::cos(camera.pitch), std::sin(camera.pitch)};
                            const auto& c = shown.position;
                            for (auto& vertex : batch.mesh.vertices) {
                                const auto l = vertex.position;
                                const Vec3 w{c.x + right.x * l.x + up.x * l.y + forward.x * l.z,
                                             c.y + right.y * l.x + up.y * l.y + forward.y * l.z,
                                             c.z + right.z * l.x + up.z * l.y + forward.z * l.z};
                                vertex.position = {w.x, w.z, w.y};
                                vertex.color = sprite.color;
                            }
                        } else
                            batch.quads.push_back(
                                {shown.position, sprite.size, sprite.color, {u, v}, {u + su, v + sv}});
                        batches.push_back(std::move(batch));
                    }
                }
                if (spell->crossline.enabled) {
                    const auto& line = spell->crossline;
                    const auto& texture = texture_for(line.texture);
                    if (texture.view) {
                        ParticleBatch batch{line.material, texture.view, {}, {}};
                        batch.mesh = line.muzzle && line.points.size() >= 2
                                         ? muzzle_mesh(line.points[0].position, line.points[1].position,
                                                       line.points[0].width, texture.header,
                                                       static_cast<unsigned>(line.frame))
                                         : crossline_mesh(line.points, texture.header,
                                                          static_cast<unsigned>(line.frame), line.continuous);
                        batches.push_back(std::move(batch));
                    }
                }
                if (spell->beam.enabled) {
                    const auto& texture = texture_for(spell->beam.texture);
                    if (texture.view) {
                        ParticleBatch batch{spell->beam.material, texture.view, {}, {}};
                        batch.mesh = spell->beam.primitive.mesh(texture.header,
                                                                static_cast<unsigned>(spell->beam.frame));
                        batches.push_back(std::move(batch));
                    }
                }
                if (spell->altar_ring.enabled && spell->altar_ring.radius > 0) {
                    const auto& texture = texture_for(spell->altar_ring.texture);
                    if (texture.view) {
                        ParticleBatch batch{0x1a012008, texture.view, {}, {}};
                        batch.mesh = spell->altar_ring_mesh(texture.header);
                        moved(batch.mesh);
                        batches.push_back(std::move(batch));
                    }
                    if (spell->altar_ring.second.value)
                        if (const auto& band = texture_for(spell->altar_ring.second); band.view) {
                            ParticleBatch batch{0x1a012008, band.view, {}, {}};
                            batch.mesh = spell->altar_ring_band_mesh(band.header);
                            moved(batch.mesh);
                            batches.push_back(std::move(batch));
                        }
                }
                if (spell->cylinder.enabled && spell->cylinder.radius > 0) {
                    const auto& texture = texture_for(spell->cylinder.texture);
                    if (texture.view) {
                        ParticleBatch batch{0x1a012008, texture.view, {}, {}};
                        batch.mesh = spell->cylinder_mesh(texture.header);
                        moved(batch.mesh);
                        batches.push_back(std::move(batch));
                    }
                }
            }
            if (spell->building_portal.enabled && spell->building_portal.opacity > 0 &&
                !(spell->state_flags & MatchObject::removed_flag)) {
                const auto& texture = texture_for(spell->building_portal.texture);
                if (texture.view) {
                    ParticleBatch batch{0x0a011008, texture.view, {}, {}};
                    batch.mesh = spell->building_portal_mesh();
                    batches.push_back(std::move(batch));
                }
            }
            if (spell->portal.enabled && spell->portal.opacity > 0 &&
                !(spell->state_flags & MatchObject::removed_flag)) {
                const Vec3 eye{camera.position.x, camera.position.z, camera.position.y},
                    up{-std::sin(camera.yaw) * std::sin(camera.pitch),
                       -std::cos(camera.yaw) * std::sin(camera.pitch), std::cos(camera.pitch)};
                // SACPRIM draws the inner fan first, then the two outer strips.
                for (const auto layer : {1u, 0u}) {
                    const auto& texture = texture_for(FourCC::from_text(layer ? "vtx2" : "vtx1"));
                    if (texture.view) {
                        ParticleBatch batch{0x1a012008, texture.view, {}, {}};
                        batch.mesh = spell->portal_mesh(layer, texture.header, eye, up);
                        batches.push_back(std::move(batch));
                    }
                }
            }
            if ((spell->effect == SpellActor::Effect::shockwave ||
                 spell->effect == SpellActor::Effect::life_shield ||
                 spell->effect == SpellActor::Effect::web_pull_hold) &&
                !(spell->state_flags & MatchObject::removed_flag)) {
                const auto& texture = texture_for(spell->shockwave.texture);
                if (texture.view) {
                    const auto& pos = spell->motion.position;
                    const double dx = pos.x - camera.position.x, dy = pos.y - camera.position.z,
                                 dz = pos.z - camera.position.y;
                    const double fx = std::sin(camera.yaw) * std::cos(camera.pitch),
                                 fy = std::cos(camera.yaw) * std::cos(camera.pitch),
                                 fz = std::sin(camera.pitch);
                    const double nearest =
                        dx * fx + dy * fy + dz * fz -
                        spell->shockwave.radius * (std::abs(fx) + std::abs(fy) + std::abs(fz));
                    // PRIMITIVE keeps MODEL's detail radius 1; DefaultGFXDetail is (.5, 10, 1, 1).
                    const auto scale =
                        static_cast<float>(nearest <= 10 ? 1 : std::pow(.5, (nearest - 10) / 10));
                    ParticleBatch batch{spell->shockwave.material, texture.view, {}, {}};
                    batch.mesh = spell->shockwave_mesh(texture.header, scale);
                    batches.push_back(std::move(batch));
                }
            }
            for (auto& system : spell->particle_systems) {
                // Draw (0x16020f30) runs the particles whenever time has passed since the last run,
                // so they move every frame rather than in SetParams' 16-tick batches.
                if (system.elapsed() > 0)
                    system.run();
                if (!system.alive())
                    continue;
                const auto& texture = texture_for(system.texture());
                if (!texture.view)
                    continue;
                const auto [slot, added] =
                    index.try_emplace({system.material_flags(), system.texture().value}, batches.size());
                if (added)
                    batches.push_back({system.material_flags(), texture.view, {}, {}});
                auto& quads = batches[slot->second].quads;
                const auto& h = texture.header;
                const unsigned columns = std::max(h.width / std::max(h.frame_width, 1u), 1u);
                const float su = static_cast<float>(h.frame_width) / h.width,
                            sv = static_cast<float>(h.frame_height) / h.height;
                // Draw: without flag 1 the particles live in the host model's frame (+404 rows, +440).
                const bool world = system.flags & 1;
                const auto& m = shown.orientation;
                const auto& origin = shown.position;
                for (const auto& p : system.particles()) {
                    const auto frame =
                        static_cast<unsigned>(static_cast<std::int64_t>(p.frame)) % std::max(h.frames, 1u);
                    const float u0 = static_cast<float>(frame % columns) * su,
                                v0 = static_cast<float>(frame / columns) * sv;
                    const Vec3 l = world ? Vec3{p.position.x - p.velocity.x * behind,
                                                p.position.y - p.velocity.y * behind,
                                                p.position.z - p.velocity.z * behind}
                                         : p.position;
                    const Vec3 at = world ? l
                                          : Vec3{l.x * m[0] + l.y * m[3] + l.z * m[6] + origin.x,
                                                 l.x * m[1] + l.y * m[4] + l.z * m[7] + origin.y,
                                                 l.x * m[2] + l.y * m[5] + l.z * m[8] + origin.z};
                    quads.push_back({at,
                                     p.size,
                                     byte(p.color[0]) | byte(p.color[1]) << 8 | byte(p.color[2]) << 16 |
                                         byte(p.color[3]) << 24,
                                     {u0, v0},
                                     {u0 + su, v0 + sv}});
                    if (system.flags & 0x40000) {
                        // DrawGenPntAligned 0x160215c0 rotates the local X/Z quad by
                        // the generator frame. The construction ring must not billboard.
                        auto& q = quads.back();
                        q.generator_aligned = true;
                        const auto& rotation = system.current.rotation;
                        const auto axis = [&](unsigned row) {
                            const Vec3 v{rotation[row], rotation[row + 1], rotation[row + 2]};
                            return world ? v
                                         : Vec3{v.x * m[0] + v.y * m[3] + v.z * m[6],
                                                v.x * m[1] + v.y * m[4] + v.z * m[7],
                                                v.x * m[2] + v.y * m[5] + v.z * m[8]};
                        };
                        q.right = axis(0);
                        q.up = axis(6);
                    }
                }
            }
        }
    }
    const Vec3 eye{camera.position.x, camera.position.z, camera.position.y};
    const Vec3 right{std::cos(camera.yaw), 0, -std::sin(camera.yaw)}; // Renderer space.
    for (unsigned n = 0; n < match.weather.effects.size(); ++n) {
        const auto& e = match.weather.effects[n];
        if (e.drops.empty())
            continue;
        const auto& p = LandscapeEffects::parameters(n + 1);
        ParticleBatch batch{p.material, nullptr, {}, {}};
        Vec2 uv{1, 1};
        if (p.texture.value) {
            const auto& texture = texture_for(p.texture);
            if (!texture.view)
                continue;
            batch.texture = texture.view;
            uv = {static_cast<float>(texture.header.frame_width) / texture.header.width,
                  static_cast<float>(texture.header.frame_height) / texture.header.height};
        }
        unsigned drawn = 0;
        // The original draws a third of the drops. Remastered draws them all, each with drawing-only copies
        // at fixed hashed offsets inside the wrapping volume (no random numbers are drawn).
        const unsigned copies = remastered ? std::max(1u, static_cast<unsigned>(tuning::weather_density + .5f)) : 1,
                       limit = remastered ? static_cast<unsigned>(p.maximum) : static_cast<unsigned>(p.maximum / 3);
        unsigned index = 0;
        for (const auto& drop : e.drops) {
        if (drawn >= limit * copies)
            break;
        for (unsigned copy = 0; copy < copies; ++copy, ++drawn) {
            auto d = drop;
            if (copy) {
                auto hash = (index * 0x9e3779b9u) ^ (copy * 0x85ebca6bu);
                const auto next = [&] {
                    hash ^= hash >> 15;
                    hash *= 0x2c1b3c6du;
                    hash ^= hash >> 12;
                    return (hash & 0xffff) / 65535.0f * 2 - 1;
                };
                d.point.x += next() * p.radius;
                d.point.y += next() * p.radius;
                d.point.z += next() * p.radius;
                d.size *= .85f + .3f * (next() * .5f + .5f);
            }
            const auto at = LandscapeEffects::wrapped(d.point, eye, p.radius);
            if (n == 0 && remastered && match.terrain)
                append_drop_splash(match, at, -p.velocity.z, index * 7 + copy, batch.mesh);
            // Remastered rain: the original's faint hairline streaks, wider, longer and more opaque.
            const bool streak = n == 0 && remastered;
            const float opacity = streak ? tuning::rain_opacity : 1, width = streak ? tuning::rain_width : 1,
                        length = streak ? tuning::rain_length : 1;
            const auto alpha =
                static_cast<unsigned>(std::clamp(std::lround(p.alpha * d.alpha * opacity), 0L, 255L));
            if (n == 3 && remastered) {
                // Remastered embers: larger, with a faint wide halo so they glow and feed the bloom.
                const float size = d.size * p.size * tuning::ember_size;
                batch.quads.push_back({at, size, 0xffffffu | alpha << 24, {}, uv});
                const auto halo = static_cast<unsigned>(std::clamp(std::lround(alpha * tuning::ember_halo), 0L, 255L));
                if (halo)
                    batch.quads.push_back({at, size * tuning::ember_halo_size, 0xffffffu | halo << 24, {}, uv});
            } else if (n)
                batch.quads.push_back({at, d.size * p.size, 0xffffffu | alpha << 24, {}, uv});
            else {
                const Vec3 origin{at.x, at.z, at.y};
                const float size = d.size * p.size * width;
                auto& mesh = batch.mesh;
                const auto base = static_cast<unsigned>(mesh.vertices.size());
                mesh.vertices.push_back({{origin.x - right.x * size, origin.y, origin.z - right.z * size},
                                         {},
                                         {},
                                         0xffffffu | alpha << 24});
                mesh.vertices.push_back({{origin.x + right.x * size, origin.y, origin.z + right.z * size},
                                         {},
                                         {},
                                         0xffffffu | alpha << 24});
                mesh.vertices.push_back(
                    {{origin.x - p.velocity.x * 8 * length, origin.y - p.velocity.z * 8 * length,
                      origin.z - p.velocity.y * 8 * length},
                     {},
                     {},
                     0x003f3f3f});
                mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2});
            }
        }
        ++index;
        }
        if (n == 0 && remastered && match.terrain)
            append_rain_splashes(match, eye, camera.yaw, e.intensity, p.radius, batch.mesh);
        batches.push_back(std::move(batch));
    }
}
} // namespace sacrifice
