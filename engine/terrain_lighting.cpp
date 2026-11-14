#include "sacrifice/scene.hpp"
#include "sacrifice/building_actor.hpp"
#include "sacrifice/player_visuals.hpp"
#include <cmath>

namespace sacrifice {
namespace {
double dot(Vec3 a, Vec3 b) { return double(a.x) * b.x + double(a.y) * b.y + double(a.z) * b.z; }
Vec3 unit(Vec3 v) {
    const double length = std::sqrt(dot(v, v));
    return length ? Vec3{float(v.x / length), float(v.y / length), float(v.z / length)} : Vec3{};
}
struct ShadowCaster {
    const BuildingActor* building;
    Vec3 inverse(Vec3 p) const {
        const auto& o = building->motion.orientation;
        const float scale = building->model_scale;
        return {(p.x * o[0] + p.y * o[1] + p.z * o[2]) / scale,
                (p.x * o[3] + p.y * o[4] + p.z * o[5]) / scale,
                (p.x * o[6] + p.y * o[7] + p.z * o[8]) / scale};
    }
    bool bounds(Vec3 from, Vec3 ray) const {
        const auto& at = building->motion.position;
        const Vec3 local = inverse({from.x - at.x, from.y - at.y, from.z - at.z});
        const auto direction = inverse(ray);
        return motion_segment_contact(local, {direction.x * 30000, direction.y * 30000, direction.z * 30000},
                                      building->model()->bounds, 0) >= 0;
    }
    bool intersects(Vec3 from, Vec3 ray) const {
        const auto& at = building->motion.position;
        return building_intercept(*building->model(),
            inverse({from.x - at.x, from.y - at.y, from.z - at.z}), inverse(ray)) > 0;
    }
};
}
void bake_terrain_lighting(Terrain& terrain, const TerrainMaterials& materials, const MapScene& scene,
                           const Archive& map, const ResourceCatalog& catalog) {
    if (!scene.environment)
        return;
    const auto& environment = *scene.environment;
    const auto count = static_cast<std::size_t>(terrain.width) * terrain.height;
    std::vector<std::array<unsigned, 3>> glow(count);
    // PrepareTileLighting 0x1603f250: max the emitting tile's color into each of its four corners.
    // Join tiles select their two source groups independently at each corner.
    static constexpr unsigned joins[12][4] = {
        {0,0,1,1}, {1,1,0,0}, {0,1,0,1}, {1,0,1,0}, {1,0,0,0}, {0,1,0,0},
        {0,0,0,1}, {0,0,1,0}, {0,1,1,1}, {1,0,1,1}, {1,1,1,0}, {1,1,0,1}};
    for (unsigned y = 0; y < terrain.height; ++y)
        for (unsigned x = 0; x < terrain.width; ++x) {
            const auto tile = terrain.tiles[static_cast<std::size_t>(y) * terrain.width + x];
            const auto& properties = materials.tile_properties[tile];
            const auto& group = materials.groups[materials.tile_groups[tile]];
            for (unsigned corner = 0; corner < 4; ++corner) {
                const auto index = static_cast<std::size_t>(y + (corner >> 1)) * terrain.width + x + (corner & 1);
                if (index >= count)
                    continue;
                const auto* color = &properties;
                if (!(properties[0] & 8)) {
                    const auto offset = group.tiles.empty() ? 12 : unsigned(tile - group.tiles.front());
                    if (group.join && offset < 12)
                        color = &materials.groups[group.pair[joins[offset][corner]]].properties;
                    else
                        color = &group.properties;
                }
                if ((*color)[0] & 8)
                    for (unsigned channel = 0; channel < 3; ++channel)
                        glow[index][channel] = std::max(glow[index][channel], unsigned((*color)[8 + channel]));
            }
        }
    std::array<std::uint32_t, 256> striation;
    striation.fill(0xffffffff);
    Bytes stripes;
    if (const auto record = map.find(FourCC::from_text("DATA"), FourCC::from_text("STRI")))
        stripes = map.read(*record);
    else if (const auto land = catalog.find_chunk(FourCC::from_text("LAND"), terrain.tileset))
        if (const auto palette = catalog.find_chunk(FourCC::from_text("DATA"), FourCC::from_text("STRI"), land))
            stripes = catalog.read(*palette);
    if (!stripes.empty()) {
        if (stripes.size() != 1024)
            throw Error("STRI requires 256 terrain height colors");
        Reader reader(stripes, "STRI");
        for (auto& color : striation)
            color = reader.u32();
    }
    std::vector<ShadowCaster> casters;
    for (const auto& spawn : scene.spawns)
        if (const auto* building = dynamic_cast<const BuildingActor*>(spawn.object);
            building && building->model() && building->model_scale > 0)
            casters.push_back({building});
    // 0x4571d0 uses a center sample and four one-unit offsets, each weighted 0.2.
    static constexpr Vec3 samples[] = {{0,0,0}, {1,0,0}, {0,1,0}, {-1,0,0}, {0,-1,0}};
    const Vec3 sun = unit(environment.sun_direction);
    const std::array<double, 3> ambient{
        std::min(255.0, environment.ambient.x * environment.ambient_intensity * 255.0),
        std::min(255.0, environment.ambient.y * environment.ambient_intensity * 255.0),
        std::min(255.0, environment.ambient.z * environment.ambient_intensity * 255.0)};
    const std::array<double, 3> sunlight{
        std::clamp(environment.sun_color.x * environment.sun_intensity * 255.0, 0.0, 255.0),
        std::clamp(environment.sun_color.y * environment.sun_intensity * 255.0, 0.0, 255.0),
        std::clamp(environment.sun_color.z * environment.sun_intensity * 255.0, 0.0, 255.0)};
    const double gloss_limit = -std::cos(std::clamp(1.0 - environment.terrain_gloss_width, 0.0, 1.0) * 1.570795);
    const double gloss_scale = -1.0 / (gloss_limit + 1);
    unsigned minimum = 32767, maximum = 0;
    for (const auto height : terrain.heights) {
        minimum = std::min(minimum, unsigned(height & 0x7fff));
        maximum = std::max(maximum, unsigned(height & 0x7fff));
    }
    for (unsigned y = 0; y < terrain.height; ++y)
        for (unsigned x = 0; x < terrain.width; ++x) {
            const auto index = static_cast<std::size_t>(y) * terrain.width + x;
            Vec3 at{float(x * 10), float(y * 10), terrain.at(x, y) + .1f};
            double shadow = 0, diffuse = 0, specular = 0;
            if (x == 0 || y == 0 || x >= 255 || y >= 255 || (terrain.raw(x, y) & 0x8000)) {
                shadow = .5;
                diffuse = 1;
            } else {
                Vec3 hit;
                if (terrain_intercept(terrain, at, sun, hit, 9000000, true) > 0)
                    shadow = 1;
                else {
                    at.z -= .1f;
                    for (const auto& caster : casters) {
                        if (!caster.bounds(at, sun))
                            continue;
                        double amount = 0;
                        for (const auto offset : samples)
                            if (caster.intersects({at.x + offset.x, at.y + offset.y, at.z}, sun))
                                amount += .2;
                        if (amount > 0) {
                            shadow = amount;
                            break;
                        }
                    }
                    at.z += .1f;
                }
                if (shadow < 1) {
                    const Vec3 normal = unit({terrain.at(x - 1, y) - terrain.at(x + 1, y),
                                             terrain.at(x, y - 1) - terrain.at(x, y + 1), 40});
                    diffuse = dot(normal, sun);
                    const double reflection = sun.z - 2 * diffuse * normal.z - gloss_limit;
                    if (reflection < 0 && environment.terrain_gloss > 0 && gloss_limit > -1)
                        specular = std::pow(reflection * gloss_scale, 3) * (1 - shadow);
                    diffuse = std::max(diffuse, 0.0);
                }
            }
            const auto band = unsigned((terrain.raw(x, y) & 0x7fff) - minimum) * 256 / (maximum - minimum + 1);
            const auto stripe = striation[255 - std::min(band, 255u)];
            const auto filter = terrain.lightmap[index]; // Source LMAP is a little-endian RGB filter.
            unsigned rgb[3];
            for (unsigned channel = 0; channel < 3; ++channel) {
                double value = (int(ambient[channel]) + (1 - shadow) * diffuse * sunlight[channel]) *
                    ((filter >> (channel * 8)) & 255) * ((stripe >> ((2 - channel) * 8)) & 255) / 65535.0;
                value += specular * environment.terrain_gloss * ((environment.terrain_gloss_color >> (channel * 8)) & 255);
                // Runtime LAND stores A1R5G5B5, then expands by shifting each channel left three bits.
                rgb[channel] = std::max(glow[index][channel], unsigned(std::clamp(value, 0.0, 255.0))) & 248;
            }
            terrain.lightmap[index] = (rgb[0] << 16) | (rgb[1] << 8) | rgb[2];
        }
}
} // namespace sacrifice
