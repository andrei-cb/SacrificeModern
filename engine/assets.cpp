#include "sacrifice/assets.hpp"
#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace sacrifice {
namespace {
Bytes require(const Archive& a, std::string_view tag) {
    auto r = a.find(FourCC::from_text(tag));
    if (!r)
        throw Error(std::string(a.label()) + ": missing " + std::string(tag));
    return a.read(*r);
}
Vec3 normal(Vec3 a, Vec3 b, Vec3 c) {
    Vec3 u{b.x - a.x, b.y - a.y, b.z - a.z}, v{c.x - a.x, c.y - a.y, c.z - a.z};
    Vec3 n{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (len < 0.00001f)
        return {0, 1, 0};
    return {n.x / len, n.y / len, n.z / len};
}
std::uint32_t rgb565(std::uint16_t c) {
    auto r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return 0xff000000u | ((b * 255 / 31) << 16) | ((g * 255 / 63) << 8) | (r * 255 / 31);
}
std::uint16_t* land_sample(Terrain& terrain, int x, int y) {
    if (x < 0 || y < 0 || static_cast<unsigned>(x) > terrain.width ||
        static_cast<unsigned>(y) > terrain.height)
        return nullptr;
    const auto index =
        static_cast<std::size_t>(y) * terrain.width + static_cast<unsigned>(x), size = terrain.heights.size();
    if (index < size)
        return &terrain.heights[index];
    if (terrain.heights.empty())
        return nullptr;
    return &terrain.heights[index - size == terrain.width ? 0 : index - size];
}
const std::uint16_t* land_sample(const Terrain& terrain, int x, int y) {
    return land_sample(const_cast<Terrain&>(terrain), x, y);
}
} // namespace
Terrain Terrain::load(const Archive& map) {
    auto bytes = require(map, "HMAP");
    Reader reader(bytes, "HMAP");
    const auto width = reader.u16(), height = reader.u16();
    // Executable 0x4570e0 skips this four-byte dimension header before SetHeightMap.
    // The historical writer includes unused trailing allocation padding.
    if (width < 2 || width > 2048 || height != width || (width & (width - 1)) ||
        bytes.size() != 2ull * width * (height + 2))
        throw Error("Unsupported HMAP dimensions");
    Terrain terrain;
    terrain.width = width;
    terrain.height = height;
    terrain.minimum = std::numeric_limits<float>::max();
    terrain.maximum = 0;
    terrain.heights.reserve(static_cast<std::size_t>(width) * height);
    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i)
        terrain.heights.push_back(reader.u16());
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            auto h = terrain.at(x, y);
            terrain.minimum = std::min(terrain.minimum, h);
            terrain.maximum = std::max(terrain.maximum, h);
        }
    auto tiles = require(map, "TMAP");
    if (tiles.size() != 512ull + width * height)
        throw Error("TMAP does not match heightmap dimensions");
    terrain.tiles.reserve(width * height);
    for (std::size_t i = 512; i < tiles.size(); ++i) {
        const auto code = std::to_integer<unsigned>(tiles[i]);
        const auto explicit_tile = std::to_integer<unsigned>(tiles[code * 2]) == 2;
        terrain.tiles.push_back(explicit_tile ? std::to_integer<std::uint8_t>(tiles[code * 2 + 1]) : 0);
    }
    terrain.tile_map = std::move(tiles);
    auto lights = require(map, "LMAP");
    if (lights.size() != 4ull * width * height)
        throw Error("LMAP does not match heightmap dimensions");
    Reader lr(lights, "LMAP");
    while (lr.remaining())
        terrain.lightmap.push_back(lr.u32());
    auto level = require(map, "LEVL");
    Reader level_reader(level, "LEVL");
    if (level.size() != 44 || level_reader.u32() != 3)
        throw Error("Unsupported scenario header");
    for (auto& field : terrain.scenario)
        field = level_reader.u32();
    terrain.tileset = {level_reader.u32()};
    return terrain;
}
std::uint16_t Terrain::raw(unsigned x, unsigned y) const {
    if (x > width || y > height)
        throw Error("Height query outside map");
    const auto index = static_cast<std::size_t>(y) * width + x, size = heights.size();
    if (index < size)
        return heights[index];
    return heights[index - size == width ? 0 : index - size];
}
float Terrain::at(unsigned x, unsigned y) const {
    // LAND::Init at 0x16041650 builds LUT[i] = (i & 0x7fff) * 0.1.
    // The original computes the LUT with a double intermediate, then stores float.
    return static_cast<float>(static_cast<double>(raw(x, y) & 0x7fff) * 0.1);
}
bool Terrain::edge(int x, int y) const {
    const auto* sample = land_sample(*this, x, y);
    return sample && (*sample & 0x8000) != 0;
}
void Terrain::dent_map(Vec3 position, float radius, float depth) {
    // LAND::DentMap 0x160382f0. ClipRect 0x160376d0 takes (min, count), not (min, max).
    if (depth <= 0.f || heights.empty() || !width || !height)
        return;
    int min_x = static_cast<int>(static_cast<std::int64_t>((position.x - radius) * 0.1));
    int min_y = static_cast<int>(static_cast<std::int64_t>((position.y - radius) * 0.1));
    int count_x = static_cast<int>(static_cast<std::int64_t>((radius + position.x + 19.0) * 0.1));
    int count_y = static_cast<int>(static_cast<std::int64_t>((radius + position.y + 19.0) * 0.1));
    const auto clip = [](int& origin, int& count, int size) {
        if (origin < 0) {
            count -= origin;
            origin = 0;
        }
        if (origin + count > size)
            count = size - origin;
        return count > 0;
    };
    if (!clip(min_x, count_x, static_cast<int>(width)) || !clip(min_y, count_y, static_cast<int>(height)))
        return;
    const double radius_squared = static_cast<double>(radius) * radius;
    float dy = static_cast<float>(10 * min_y) - position.y;
    const float dx0 = static_cast<float>(10 * min_x) - position.x;
    for (int row = 0; row < count_y; ++row) {
        float dx = dx0;
        for (int column = 0; column < count_x; ++column) {
            const double distance_squared = static_cast<double>(dx) * dx + static_cast<double>(dy) * dy;
            if (distance_squared < radius_squared) {
                if (auto* sample = land_sample(*this, min_x + column, min_y + row)) {
                    const std::uint16_t hole = *sample & 0x8000;
                    // GetAnchorMap 0x16037750 returns 128 when LAND+72 is null.
                    const double delta = (distance_squared / radius_squared - 1.0) * depth *
                                         9.99999985098839 * 128.0;
                    int next = (*sample & 0x7fff) +
                               static_cast<int>(static_cast<std::int64_t>(delta + 64.0) >> 7);
                    if (next < 0)
                        next = 0;
                    else if (next > 0x7fff)
                        next = 0x7fff;
                    *sample = static_cast<std::uint16_t>(next) | hole;
                }
            }
            dx += 10.f;
        }
        dy += 10.f;
    }
}
Terrain::Displacement::Displacement(Vec3 center, int size)
    : x(static_cast<int>((center.x - (size - 1) * 5.0) * .1)),
      y(static_cast<int>((center.y - (size - 1) * 5.0) * .1)), size(size), samples(size * size) {}

Terrain::Displacement Terrain::mound(Vec3 center, int size, float flat, float outer, ByteView profile) const {
    Displacement patch(center, size);
    if (profile.size() != patch.samples.size())
        return {};
    const int cx = patch.x + size / 2, cy = patch.y + size / 2;
    const auto* middle = land_sample(*this, cx, cy);
    if (!middle)
        return {};
    const int level = *middle & 0x7fff;
    // LAND::CreateFlattenDMap (0x16037c30), then 0x464760 adds twice each DATA byte.
    for (int row = 0; row < size; ++row)
        for (int column = 0; column < size; ++column) {
            const int index = row * size + column;
            const auto* sample = land_sample(*this, patch.x + column, patch.y + row);
            if (!sample)
                continue;
            const int old_height = *sample & 0x7fff;
            const float distance = 10.f * std::max(std::abs(column - size / 2), std::abs(row - size / 2));
            const double weight = std::clamp((outer - distance) / (outer - flat), 0.f, 1.f);
            const int flatten = static_cast<int>((level - old_height) * weight * 128 + 64) >> 7;
            patch.samples[index] = static_cast<std::int16_t>(flatten + 2 * std::to_integer<int>(profile[index]));
        }
    return patch;
}

bool Terrain::apply_displacement(Displacement& patch, float scale) {
    if (patch.applied == scale || patch.samples.empty())
        return false;
    // LAND::ApplyDisplacementMapTime (0x160380f0) adds the difference of the rounded offsets.
    bool changed = false;
    for (int row = 0; row < patch.size; ++row)
        for (int column = 0; column < patch.size; ++column) {
            const int x = patch.x + column, y = patch.y + row;
            if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height))
                continue;
            auto& sample = heights[y * width + x];
            const int delta = patch.samples[row * patch.size + column];
            const int previous = static_cast<int>(delta * static_cast<double>(patch.applied) * 128 + 64) >> 7;
            const int next = static_cast<int>(delta * static_cast<double>(scale) * 128 + 64) >> 7;
            const int old_height = sample & 0x7fff;
            const int adjusted = std::clamp(old_height + std::clamp(old_height + next, 0, 32767) -
                                                std::clamp(old_height + previous, 0, 32767), 0, 32767);
            changed |= adjusted != old_height;
            sample = static_cast<std::uint16_t>((sample & 0x8000) | adjusted);
        }
    patch.applied = scale;
    return changed;
}

int Terrain::set_edge(int x, int y, int flags, int size) {
    // LAND::SetEdge 0x1603dad0 with a5=`size`. MAPT properties byte 0 bit 0 is still open.
    if (size <= 0 || heights.empty())
        return 0;
    const int half = size / 2, extra = size % 2;
    const int x0 = x - half, x1 = x + half + extra;
    const int y0 = y - half, y1 = y + half + extra;
    int written = 0;
    for (int row = y0; row < y1; ++row) {
        for (int column = x0; column < x1; ++column) {
            auto* sample = land_sample(*this, column, row);
            if (!sample)
                continue;
            const bool hole = (*sample & 0x8000) != 0;
            if (flags & 1) {
                if (hole)
                    continue;
                *sample |= 0x8000;
                ++written;
            } else if (hole) {
                *sample &= static_cast<std::uint16_t>(~0x8000);
                ++written;
            }
        }
    }
    return written;
}
bool Terrain::main_diagonal(unsigned x, unsigned y) const {
    // Cells alternate diagonals by (x ^ y) parity; holes (bit 15) on a diagonal's endpoints flip it.
    const auto x1 = x + 1, y1 = y + 1;
    if ((x ^ y) & 1)
        return ((raw(x1, y) | raw(x, y1)) & 0x8000) != 0;
    return ((raw(x, y) | raw(x1, y1)) & 0x8000) == 0;
}
Mesh Terrain::mesh(bool tiled) const {
    Mesh m;
    m.vertices.resize(static_cast<std::size_t>(width) * height);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            auto& v = m.vertices[static_cast<std::size_t>(y) * width + x];
            // Original coordinates are X/Y horizontal and Z up; renderer uses Y up.
            v.position = {x * 10.0f, at(x, y), y * 10.0f};
            v.uv = {static_cast<float>(x) / (width - 1), 1.0f - static_cast<float>(y) / (height - 1)};
            const auto light = lightmap[static_cast<std::size_t>(y) * width + x];
            v.color = 0xff000000u | ((light & 0xff) << 16) | (light & 0xff00) | ((light >> 16) & 0xff);
            auto x0 = x ? x - 1 : 0, x1 = std::min(x + 1, width - 1), y0 = y ? y - 1 : 0,
                 y1 = std::min(y + 1, height - 1);
            const float dx = at(x1, y) - at(x0, y), dz = at(x, y1) - at(x, y0);
            float len = std::sqrt(dx * dx + 400 + dz * dz);
            v.normal = {-dx / len, 20.0f / len, -dz / len};
        }
    if (tiled) {
        auto grid = std::move(m.vertices);
        m.vertices.clear();
        m.vertices.reserve(static_cast<std::size_t>(width - 1) * (height - 1) * 4);
        for (unsigned y = 0; y + 1 < height; ++y)
            for (unsigned x = 0; x + 1 < width; ++x) {
                const auto cell = static_cast<std::size_t>(y) * width + x;
                const auto layer =
                    !scar_layers.empty() && scar_layers[cell] ? scar_layers[cell] : tiles[cell];
                auto a = grid[y * width + x], b = grid[y * width + x + 1], c = grid[(y + 1) * width + x],
                     d = grid[(y + 1) * width + x + 1];
                // 0x1603a070: near tiles map a 10-unit cell to texels 0.5..63.5 ((p - origin) x 0.0984375
                // + 0.0078125), keeping bilinear filtering inside the tile.
                constexpr float lo = 0.0078125f, hi = 1 - lo;
                a.uv = {lo, lo};
                b.uv = {hi, lo};
                c.uv = {lo, hi};
                d.uv = {hi, hi};
                // High half: the base TMAP tile, whose properties pick the detail texture (sub_16039E30).
                a.texture_layer = b.texture_layer = c.texture_layer = d.texture_layer =
                    layer | static_cast<std::uint32_t>(tiles[cell]) << 16;
                auto base = static_cast<std::uint32_t>(m.vertices.size());
                m.vertices.insert(m.vertices.end(), {a, b, c, d});
                // sub_160391E0 drops every face with a hole (bit 15) corner from the draw lists.
                const bool hole[] = {(raw(x, y) & 0x8000) != 0, (raw(x + 1, y) & 0x8000) != 0,
                                     (raw(x, y + 1) & 0x8000) != 0, (raw(x + 1, y + 1) & 0x8000) != 0};
                const auto face = [&](unsigned i, unsigned j, unsigned k) {
                    if (!hole[i] && !hole[j] && !hole[k])
                        m.indices.insert(m.indices.end(), {base + i, base + j, base + k});
                };
                if (main_diagonal(x, y)) {
                    face(0, 2, 3);
                    face(0, 3, 1);
                } else {
                    face(0, 2, 1);
                    face(1, 2, 3);
                }
            }
        return m;
    }
    for (unsigned y = 0; y + 1 < height; ++y)
        for (unsigned x = 0; x + 1 < width; ++x) {
            const auto a = y * width + x, b = a + 1, c = a + width, d = c + 1;
            if (main_diagonal(x, y))
                m.indices.insert(m.indices.end(), {a, c, d, a, d, b});
            else
                m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
    return m;
}
TerrainTile decode_terrain_tile(ByteView data, const std::array<std::uint32_t, 256>& palette) {
    // 0x409c70 consumes 64x64 pixels; LAND::AddTexture consumes 1368 mip bytes and 12 properties.
    if (data.size() != 5476)
        throw Error("MAPT requires exactly 5476 bytes");
    Reader reader(data, "MAPT");
    TerrainTile tile;
    for (unsigned mip = 0; mip < 7; ++mip) {
        auto& image = tile.mipmaps[mip];
        image.width = image.height = 64u >> mip;
        const auto count = static_cast<std::size_t>(image.width) * image.height;
        image.rgba.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
            image.rgba.push_back(palette[reader.u8()]);
        if (count < 4)
            (void)reader.take(4 - count);
    }
    for (auto& value : tile.properties)
        value = reader.u8();
    if (reader.remaining())
        throw Error("Unconsumed MAPT bytes");
    return tile;
}
TerrainMaterials TerrainMaterials::load(const ResourceCatalog& catalog, FourCC id) {
    auto location = catalog.find_chunk(FourCC::from_text("LAND"), id);
    if (!location)
        throw Error("Cannot resolve tileset " + id.text());
    const auto& archive = catalog.archive(*location);
    TerrainMaterials materials;
    materials.tileset = id;
    for (const auto* name : {"JA_A", "char", "PYRO"})
        if (id == FourCC::from_text(name))
            materials.detail_bump_scale = 1.0f / 64;
    auto palette_location =
        catalog.find_chunk(FourCC::from_text("PALT"), FourCC::from_text("LAND"), location);
    if (!palette_location)
        throw Error("Tileset has no LAND palette: " + id.text());
    auto palette = decode_palette(catalog.read(*palette_location));
    materials.palette = palette;
    std::vector<unsigned> group_order;
    const auto end = archive.records()[location->record].subtree_end;
    for (std::size_t i = location->record + 1; i < end; ++i) {
        const auto& record = archive.records()[i];
        if (record.tag == FourCC::from_text("MAPG")) {
            // 0x409c00 parses the last two ID characters as the group number.
            const auto name = record.name.text();
            if (name.size() != 4 || name[2] < '0' || name[2] > '9' || name[3] < '0' || name[3] > '9')
                throw Error("Unsupported MAPG identifier " + name);
            const unsigned index = (name[2] - '0') * 10u + (name[3] - '0');
            if (index >= materials.groups.size())
                throw Error("MAPG group outside LAND table");
            const auto bytes = archive.read(i);
            if (bytes.size() != 60)
                throw Error("MAPG requires exactly 60 bytes");
            Reader reader(bytes, "MAPG");
            auto& group = materials.groups[index];
            group_order.push_back(index);
            group.present = true;
            for (auto& value : group.height)
                value = reader.f32();
            for (auto& value : group.slope)
                value = reader.f32();
            group.join = reader.u8() == 1;
            const auto count = reader.u8();
            if (count > 14)
                throw Error("MAPG tile count exceeds record");
            for (unsigned t = 0; t < count; ++t)
                group.tiles.push_back(reader.u8());
            Reader pair(bytes, "MAPG");
            group.pair = {pair.u32(), pair.u32()};
            Reader overrides(ByteView(bytes).subspan(48), "MAPG");
            for (auto& value : group.properties)
                value = overrides.u8();
            if (group.join && (group.pair[0] >= 16 || group.pair[1] >= 16 || group.tiles.size() != 12))
                throw Error("Malformed MAPG join group");
            for (auto tile : group.tiles)
                materials.tile_groups[tile] = static_cast<std::uint8_t>(index);
            if (!group.join && index >= materials.group_count)
                materials.group_count = index + 1;
            continue;
        }
        if (record.tag == FourCC::from_text("TXTR")) {
            const auto name = record.name.text();
            if (name.size() != 4 || name[0] != 'D' || name[1] != 'T' || name[2] < '0' || name[2] > '9' ||
                name[3] < '0' || name[3] > '9')
                continue;
            const unsigned slot = (name[2] - '0') * 10u + (name[3] - '0');
            if (slot >= materials.details.size())
                continue;
            const auto bytes = archive.read(i);
            const auto header = decode_texture_header(bytes);
            if (header.width != 256 || header.height != 256 || header.frames > 1)
                throw Error("Detail texture must be a single 256x256 frame");
            const auto pixels = ByteView(bytes).subspan(36, 256 * 256);
            auto& detail = materials.details[slot];
            detail.resize(pixels.size());
            std::ranges::transform(pixels, detail.begin(),
                                   [](std::byte b) { return std::to_integer<std::uint8_t>(b); });
            continue;
        }
        if (record.tag != FourCC::from_text("MAPT"))
            continue;
        auto name = record.name.text();
        unsigned tile_id = 0;
        for (char c : name) {
            if (c < '0' || c > '9')
                throw Error("Non-numeric MAPT identifier");
            tile_id = tile_id * 10 + (c - '0');
        }
        if (tile_id >= materials.tiles.size())
            throw Error("MAPT identifier outside base tileset");
        if (!materials.tiles[tile_id])
            materials.tiles[tile_id] = decode_terrain_tile(archive.read(i), palette);
    }
    for (unsigned tile = 0; tile < 256; ++tile)
        if (materials.tiles[tile])
            materials.tile_properties[tile] = materials.tiles[tile]->properties;
    // 0x456eb0 loads every MAPT before MAPG. SetMapGroup (0x160404b0) derives join properties,
    // then conditionally replaces fields using the tile's flags before OR-ing the group's flags.
    for (const auto index : group_order) {
        auto& group = materials.groups[index];
        auto& p = group.properties;
        if (group.join) {
            const auto& a = materials.groups[group.pair[0]].properties;
            const auto& b = materials.groups[group.pair[1]].properties;
            p[0] = a[0] | b[0];
            for (const auto field : {1u, 3u, 8u, 9u, 10u, 11u})
                p[field] = static_cast<std::uint8_t>((unsigned(a[field]) + b[field]) / 2);
            p[2] = a[2];
        }
        for (const auto tile : group.tiles) {
            auto& properties = materials.tile_properties[tile];
            const auto flags = properties[0];
            if (!(flags & 0x40))
                properties[1] = p[1];
            if (!(flags & 0x80))
                properties[7] = p[7];
            if (!(flags & 8))
                std::copy_n(p.begin() + 8, 4, properties.begin() + 8);
            if (!(flags & 0x10)) {
                properties[2] = p[2];
                properties[3] = p[3];
            }
            properties[0] |= p[0];
        }
    }
    for (unsigned tile = 0; tile < 256; ++tile)
        materials.tile_details[tile] = materials.tile_properties[tile][7];
    return materials;
}
unsigned TerrainMaterials::create_tile_map(Terrain& terrain) const {
    const unsigned width = terrain.width, height = terrain.height;
    if (terrain.tile_map.size() != 512ull + width * height)
        throw Error("TMAP does not match heightmap dimensions");
    // msvcrt rand() is shared with the executable and never seeded for maps (0x4565f0 passes seed 0 to
    // LAND::Init), so its state here depends on the session; this uses the CRT's initial state instead.
    CrtRandom random;
    const auto rand15 = [&] { return static_cast<unsigned>(random.next()); };
    // 0x160437b0 fills the 32x32x32 noise table from rand() on the first LAND::Init.
    std::vector<std::uint16_t> noise(32768);
    for (auto& value : noise)
        value = static_cast<std::uint16_t>(rand15());
    const auto pick = [&](unsigned g) {
        if (groups[g].tiles.empty())
            throw Error("TMAP selects empty tile group " + std::to_string(g));
        return groups[g].tiles[rand15() % groups[g].tiles.size()];
    };
    const auto& source = terrain.tile_map;
    bool procedural = false;
    for (std::size_t i = 0; i < terrain.tiles.size(); ++i) {
        const auto code = std::to_integer<unsigned>(source[512 + i]);
        const auto mode = std::to_integer<unsigned>(source[code * 2]),
                   index = std::to_integer<unsigned>(source[code * 2 + 1]);
        if (mode == 1) {
            if (index >= groups.size())
                throw Error("TMAP selects tile group outside LAND table");
            terrain.tiles[i] = pick(index);
        } else if (mode == 2) {
            terrain.tiles[i] = static_cast<std::uint8_t>(index);
        } else {
            terrain.tiles[i] = 0; // The zeroed LAND tile map; the procedural pass fills every tile-0 cell.
            procedural = true;
        }
    }
    if (procedural) {
        // 0x16043800: trilinear noise over 320-unit cells of the wrapped table, 0..1.
        const auto sample = [&](const std::array<float, 3>& p) {
            const double fx = p[0] * 0.003125, fy = p[1] * 0.003125;
            const auto fz = static_cast<float>(p[2] * 0.003125);
            auto ix = static_cast<std::int64_t>(fx), iy = static_cast<std::int64_t>(fy),
                 iz = static_cast<std::int64_t>(fz);
            if (p[0] < 0)
                --ix;
            if (p[1] < 0)
                --iy;
            if (p[2] < 0)
                --iz;
            const auto at = [&](std::int64_t x, std::int64_t y, std::int64_t z) {
                return static_cast<double>(noise[((x & 31) * 32 + (y & 31)) * 32 + (z & 31)]);
            };
            const double ty = fy - static_cast<double>(iy);
            const auto tx = static_cast<float>(fx - static_cast<double>(ix));
            const auto lerp_y = [&](std::int64_t x, std::int64_t z) {
                return static_cast<float>((at(x, iy + 1, z) - at(x, iy, z)) * ty + at(x, iy, z));
            };
            const float x0z1 = lerp_y(ix, iz + 1), x1z1 = lerp_y(ix + 1, iz + 1), x0z0 = lerp_y(ix, iz);
            const double z0 = (lerp_y(ix + 1, iz) - x0z0) * tx + x0z0;
            return (z0 + ((x1z1 - x0z1) * tx + x0z1 - z0) * (fz - static_cast<double>(iz))) *
                   0.000030517578125;
        };
        // Height trapezoids test the noisy mean height, slope trapezoids the cell's height spread x 16.
        const auto weight = [](double v, const std::array<float, 4>& t) {
            if (v < t[0] || v > t[3])
                return 0.0;
            if (v < t[1])
                return (v - t[0]) / (t[1] - t[0]);
            if (v <= t[2])
                return 1.0;
            return (t[3] - v) / (t[3] - t[2]);
        };
        // CalculateHeightMinMax 0x1603deb0 uses masked heights; CreateTileMap normalizes the raw samples.
        int minimum = terrain.heights.front() & 0x7fff, maximum = minimum;
        for (auto h : terrain.heights) {
            minimum = std::min(minimum, h & 0x7fff);
            maximum = std::max(maximum, h & 0x7fff);
        }
        const auto range = static_cast<float>(maximum - minimum);
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x) {
                auto& cell = terrain.tiles[static_cast<std::size_t>(y) * width + x];
                if (cell)
                    continue;
                float lowest{}, highest{}, sum = 0;
                bool first = true;
                for (auto [dx, dy] :
                     {std::pair{0u, 0u}, std::pair{1u, 0u}, std::pair{0u, 1u}, std::pair{1u, 1u}}) {
                    const auto h = static_cast<float>(
                        static_cast<double>(terrain.raw(x + dx, y + dy) - minimum) / range);
                    lowest = first ? h : std::min(lowest, h);
                    highest = first ? h : std::max(highest, h);
                    sum += h;
                    first = false;
                }
                const float mean = sum * 0.25f;
                const double slope = std::min((static_cast<double>(highest) - lowest) * 16.0, 1.0);
                const double level =
                    std::clamp((sample({x * 50.0f, y * 50.0f, mean * 512.0f}) - 0.5) * 0.25 + mean, 0.0, 1.0);
                unsigned best = 0;
                double best_weight = -1;
                for (unsigned g = 0; g < group_count; ++g) {
                    const auto w = weight(level, groups[g].height) * weight(slope, groups[g].slope);
                    if (w > best_weight) {
                        best_weight = w;
                        best = g;
                    }
                }
                cell = pick(best);
            }
    }
    return join_tiles(terrain);
}
unsigned TerrainMaterials::join_tiles(Terrain& terrain) const {
    // LAND::FindJoinGroup 0x1603ea10: join groups from group_count on; side 1 when the pair is reversed.
    const auto find = [&](unsigned a, unsigned b, unsigned& side) -> unsigned {
        for (unsigned g = group_count; g < groups.size(); ++g) {
            const auto& group = groups[g];
            if (!group.join)
                continue;
            if (group.pair[0] == a && group.pair[1] == b) {
                side = 0;
                return g;
            }
            if (group.pair[1] == a && group.pair[0] == b) {
                side = 1;
                return g;
            }
        }
        return 0;
    };
    // byte_160570a4: join tile by side and the mask of differing neighbours (1 right, 2 below, 4 diagonal).
    static constexpr std::uint8_t join_tile[2][8] = {{0, 5, 7, 8, 6, 2, 0, 8}, {0, 9, 11, 4, 10, 3, 1, 4}};
    const auto group_of = [&](std::uint8_t tile) { return tile_groups[tile]; };
    const auto first_tile = [&](unsigned g) {
        if (groups[g].tiles.empty())
            throw Error("Joined tile group " + std::to_string(g) + " has no tiles");
        return groups[g].tiles.front();
    };
    const auto before = terrain.tiles;
    const unsigned width = terrain.width;
    for (unsigned y = 0; y + 1 < terrain.height; ++y)
        for (unsigned x = 0; x + 1 < width; ++x) {
            auto& cell = terrain.tiles[y * width + x];
            const auto right = terrain.tiles[y * width + x + 1];
            auto& below = terrain.tiles[(y + 1) * width + x];
            auto& diagonal = terrain.tiles[(y + 1) * width + x + 1];
            unsigned join = 0, side = 0, mask = 0;
            if (group_of(cell) != group_of(right)) {
                join = find(group_of(cell), group_of(right), side);
                mask = join != 0;
            }
            for (auto [neighbour, bit] : {std::pair{&below, 2u}, std::pair{&diagonal, 4u}}) {
                if (group_of(cell) == group_of(*neighbour) ||
                    (!join && !(join = find(group_of(cell), group_of(*neighbour), side))))
                    continue;
                // The neighbour becomes the first tile of the pair's other group unless already in it.
                const auto other = groups[join].pair[side ^ 1];
                if (other != group_of(*neighbour))
                    *neighbour = first_tile(other);
                mask |= bit;
            }
            if (mask)
                cell = groups[join].tiles[join_tile[side][mask]];
        }
    unsigned changed = 0;
    for (std::size_t i = 0; i < before.size(); ++i)
        changed += before[i] != terrain.tiles[i];
    return changed;
}
void TerrainMaterials::validate(const Terrain& terrain) const {
    if (tileset != terrain.tileset)
        throw Error("Terrain and materials use different tilesets");
    for (auto index : terrain.tiles)
        if (!tiles[index])
            throw Error("Missing terrain tile " + std::to_string(index) + " in " + tileset.text());
}
std::vector<EntityRecord> decode_entities(ByteView data) {
    Reader r(data, "NTTS");
    const auto count = r.u32();
    if (count > 100000 || count > r.remaining() / 8)
        throw Error("Invalid NTTS count");
    std::vector<EntityRecord> entities;
    entities.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        auto offset = r.position();
        const auto type = r.u32(), size = r.u32();
        if (size < 8)
            throw Error("Invalid NTTS record size");
        auto payload = r.take(size - 8);
        entities.push_back({type, offset, Bytes(payload.begin(), payload.end())});
    }
    if (r.remaining())
        throw Error("Unconsumed NTTS data");
    return entities;
}
Image decode_preview(ByteView bytes) {
    if (bytes.size() != 128 * 128 * 2)
        throw Error("Unsupported PREV dimensions");
    Image image{128, 128, {}};
    image.rgba.reserve(128 * 128);
    Reader r(bytes, "PREV");
    while (r.remaining())
        image.rgba.push_back(rgb565(r.u16()));
    return image;
}
Image decode_tga(ByteView bytes) {
    Reader r(bytes, "TGA");
    auto id_length = r.u8(), color_map = r.u8(), type = r.u8();
    (void)r.take(9);
    auto w = r.u16(), h = r.u16();
    auto bits = r.u8(), descriptor = r.u8();
    if (color_map || (type != 2 && type != 10) || (bits != 24 && bits != 32) || !w || !h ||
        static_cast<std::size_t>(w) * h > 64 * 1024 * 1024)
        throw Error("Unsupported TGA format");
    (void)r.take(id_length);
    Image image{w, h, std::vector<std::uint32_t>(static_cast<std::size_t>(w) * h)};
    auto pixel = [&]() {
        auto b = r.u8(), g = r.u8(), red = r.u8();
        auto a = bits == 32 ? r.u8() : 255;
        return static_cast<std::uint32_t>(red | (g << 8) | (b << 16) | (a << 24));
    };
    std::size_t pos = 0;
    while (pos < image.rgba.size()) {
        unsigned packet = type == 10 ? r.u8() : 0;
        auto n = type == 10 ? 1 + (packet & 127) : 1;
        if (n > image.rgba.size() - pos)
            throw Error("TGA packet overrun");
        auto repeated = (packet & 128) != 0;
        auto color = repeated ? pixel() : 0;
        for (unsigned i = 0; i < n; ++i) {
            auto x = pos % w, y = pos / w;
            if (!(descriptor & 32))
                y = h - 1 - y;
            if (descriptor & 16)
                x = w - 1 - x;
            image.rgba[y * w + x] = repeated ? color : pixel();
            ++pos;
        }
    }
    return image;
}
Sample decode_sample(ByteView bytes) {
    Reader r(bytes, "SAMP");
    Sample s;
    s.parameter = r.f32();
    s.near_range = r.f32();
    s.far_range = r.f32();
    auto length = r.u32();
    // The sample envelope is followed by WAVEFORMATEX and exactly length audio bytes.
    s.format = r.u16();
    s.channels = r.u16();
    s.sample_rate = r.u32();
    s.byte_rate = r.u32();
    s.block_align = r.u16();
    s.bits = r.u16();
    if (!s.channels || s.channels > 8 || !s.sample_rate || s.sample_rate > 384000 || !s.block_align)
        throw Error("Invalid sample format");
    if (r.remaining() >= 12 &&
        Reader(bytes.subspan(r.position()), "sample signature").fixed_string(4) == "RIFF") {
        // Map-local ADPCM samples embed a full RIFF after the 32-byte envelope.
        // The envelope's length can contain a stale serialized value; RIFF owns its bounds.
        auto wave = r.take(r.remaining());
        Reader wr(wave, "sample RIFF");
        (void)wr.take(4);
        auto riff_size = wr.u32();
        if (wr.fixed_string(4) != "WAVE" || static_cast<std::size_t>(riff_size) + 8 > wave.size())
            throw Error("Invalid embedded sample RIFF");
        s.wave_container = true;
        s.audio.assign(wave.begin(), wave.begin() + riff_size + 8);
        return s;
    }
    if (r.remaining() != length) {
        auto extra = r.u16();
        auto ext = r.take(extra);
        s.format_extra.assign(ext.begin(), ext.end());
    }
    auto audio = r.take(length);
    s.audio.assign(audio.begin(), audio.end());
    if (r.remaining())
        throw Error("Unconsumed SAMP data");
    return s;
}
TextureHeader decode_texture_header(ByteView bytes) {
    Reader r(bytes, "TXTR");
    TextureHeader h;
    h.width = r.u32();
    h.height = r.u32();
    h.palette = {r.u32()};
    h.flags = r.u32();
    h.key = r.u32();
    h.frames = r.u32();
    h.frame_width = r.u32();
    h.frame_height = r.u32();
    const auto hotspot = r.u32();
    h.hotspot_x = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(hotspot));
    h.hotspot_y = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(hotspot >> 16));
    if (!h.width || !h.height || h.width > 16384 || h.height > 16384 ||
        static_cast<std::size_t>(h.width) * h.height > r.remaining())
        throw Error("Invalid indexed texture size");
    if (h.frames <= 1) {
        h.frame_width = h.width;
        h.frame_height = h.height;
    } else if (!h.frame_width || !h.frame_height || h.frame_width > h.width || h.frame_height > h.height)
        throw Error("Invalid texture animation frame layout");
    const auto required = static_cast<std::size_t>(h.width) * h.height * ((h.flags & 0x10000) ? 2u : 1u);
    if (required > r.remaining())
        throw Error("Truncated texture pixel/alpha planes");
    h.trailing_bytes = r.remaining() - required;
    return h;
}
std::array<std::uint32_t, 256> decode_palette(ByteView bytes) {
    // Sacrifice.exe 0x409390 reads 776 bytes and passes bytes [8,776) to PALETTE.
    Reader r(bytes, "PALT");
    (void)r.take(8);
    std::array<std::uint32_t, 256> result{};
    for (auto& color : result) {
        auto red = r.u8(), green = r.u8(), blue = r.u8();
        color = 0xff000000u | red | (green << 8) | (blue << 16);
    }
    return result;
}
Image decode_texture(ByteView bytes, const std::array<std::uint32_t, 256>& palette) {
    auto h = decode_texture_header(bytes);
    const auto count = static_cast<std::size_t>(h.width) * h.height;
    Reader r(bytes.subspan(36), "TXTR pixels");
    auto indices = r.take(count);
    Image image{h.width, h.height, std::vector<std::uint32_t>(count)};
    ByteView alpha;
    if (h.flags & 0x10000)
        alpha = r.take(count);
    // The legacy callback consumes only the declared pixel/alpha planes. The header reports
    // any remainder; stock wwsp contains two unused extra planes.
    // Chroma keys are RGB colors. PALETTE::FindRGB/GetMappedRGB use first minimum L1 distance.
    unsigned key_index = 0, best = std::numeric_limits<unsigned>::max();
    if (h.flags & 4)
        for (unsigned i = 0; i < 256; ++i) {
            const auto c = palette[i];
            auto difference = static_cast<unsigned>(
                std::abs(static_cast<int>(c & 255) - static_cast<int>((h.key >> 16) & 255)) +
                std::abs(static_cast<int>((c >> 8) & 255) - static_cast<int>((h.key >> 8) & 255)) +
                std::abs(static_cast<int>((c >> 16) & 255) - static_cast<int>(h.key & 255)));
            if (difference < best) {
                best = difference;
                key_index = i;
            }
        }
    for (std::size_t i = 0; i < count; ++i) {
        auto color = palette[std::to_integer<unsigned>(indices[i])];
        if (!alpha.empty())
            color = (color & 0xffffffu) | (std::to_integer<std::uint32_t>(alpha[i]) << 24);
        if ((h.flags & 4) && std::to_integer<unsigned>(indices[i]) == key_index)
            color &= 0xffffffu;
        image.rgba[i] = color;
    }
    return image;
}
Image load_texture(const Archive& a, std::size_t index) {
    auto bytes = a.read(index);
    auto h = decode_texture_header(bytes);
    std::array<std::uint32_t, 256> palette{};
    bool found = false;
    const auto parent = a.records().at(index).parent;
    if (h.palette != FourCC::from_text("GrAy") && parent < a.records().size())
        if (auto p = a.find_chunk(FourCC::from_text("PALT"), h.palette, parent)) {
            palette = decode_palette(a.read(*p));
            found = true;
        }
    // Original callback 0x409420 uses the grayscale provider when a palette cannot be found.
    if (!found)
        for (unsigned i = 0; i < 256; ++i)
            palette[i] = 0xff000000u | i | (i << 8) | (i << 16);
    return decode_texture(bytes, palette);
}
Image load_texture(const ResourceCatalog& catalog, ResourceLocation location) {
    return load_texture(catalog.archive(location), location.record);
}
Image texture_frame(const Image& image, const TextureHeader& header, unsigned frame) {
    if (image.width != header.width || image.height != header.height ||
        image.rgba.size() != static_cast<std::size_t>(image.width) * image.height)
        throw Error("Texture frame source dimensions mismatch");
    if (frame >= std::max(header.frames, 1u) || !header.frame_width || !header.frame_height ||
        header.frame_width > image.width || header.frame_height > image.height)
        throw Error("Texture frame outside atlas");
    const auto columns = image.width / header.frame_width;
    const auto x = (frame % columns) * header.frame_width;
    const auto y = static_cast<std::uint64_t>(frame / columns) * header.frame_height;
    if (y > image.height - header.frame_height)
        throw Error("Texture frame outside atlas");
    Image result{header.frame_width, header.frame_height, {}};
    result.rgba.reserve(static_cast<std::size_t>(result.width) * result.height);
    for (unsigned row = 0; row < result.height; ++row) {
        auto start = image.rgba.begin() + static_cast<std::size_t>(y + row) * image.width + x;
        result.rgba.insert(result.rgba.end(), start, start + result.width);
    }
    return result;
}
ModelAsset ModelAsset::load(const ResourceCatalog& catalog, ResourceLocation model) {
    const auto& archive = catalog.archive(model);
    const auto& record = archive.records()[model.record];
    if (record.tag != FourCC::from_text("MRMM"))
        throw Error("Model handle is not MRMM");
    const auto context = std::string(archive.label()) + record.path;
    if (record.parent >= archive.records().size())
        throw Error(context + ": model has no material scope");
    ModelAsset asset;
    asset.geometry = MrmGeometry::decode(catalog.read(model));
    asset.mesh = asset.geometry.sample();
    for (const auto& material : asset.mesh.materials) {
        if (!material.texture.value) {
            asset.textures.push_back({});
            continue;
        }
        const auto source = catalog.find_chunk(FourCC::from_text("TXTR"), material.texture,
                                               ResourceLocation{model.archive, record.parent});
        if (!source)
            throw Error(context + ": missing scoped TXTR:" + material.texture.text());
        try {
            auto header = decode_texture_header(catalog.read(*source));
            auto image = load_texture(catalog, *source);
            asset.textures.push_back(Texture{header, texture_frame(image, header, 0), *source});
        } catch (const std::exception& e) {
            throw Error(context + ": TXTR:" + material.texture.text() + ": " + e.what());
        }
    }
    return asset;
}
TerrainLod TerrainLod::build(const Terrain& terrain) {
    TerrainLod lod;
    const unsigned w = terrain.width;
    if (w != 1u << levels || terrain.height != w)
        return lod;
    lod.size = w;
    // LAND's height buffer: rows of w samples followed by a copy of row 0 and its first sample.
    const auto height = [&](long long index) {
        if (index < 0 || index > static_cast<long long>(w) * (w + 1))
            throw Error("Variance sample outside height buffer");
        auto x = static_cast<unsigned>(index % w), y = static_cast<unsigned>(index / w);
        if (y > w) {
            x += w;
            --y;
        }
        return terrain.at(x, y);
    };
    // UpdateEdgeMipMaps: per cell, bit 0 when any corner is a hole and bit 1 when two or more are; coarser
    // levels OR bit 0 and AND bit 1 over their four children.
    lod.flags[0].resize(static_cast<std::size_t>(w) * w);
    for (unsigned y = 0; y < w; ++y)
        for (unsigned x = 0; x < w; ++x) {
            const unsigned holes = (terrain.raw(x, y) >> 15) + (terrain.raw(x + 1, y) >> 15) +
                                   (terrain.raw(x, y + 1) >> 15) + (terrain.raw(x + 1, y + 1) >> 15);
            lod.flags[0][static_cast<std::size_t>(y) * w + x] =
                static_cast<std::uint8_t>((holes ? 1 : 0) | (holes > 1 ? 2 : 0));
        }
    for (unsigned level = 1; level < levels; ++level) {
        const unsigned n = w >> level, child = n * 2;
        auto& out = lod.flags[level];
        const auto& in = lod.flags[level - 1];
        out.resize(static_cast<std::size_t>(n) * n);
        for (unsigned y = 0; y < n; ++y)
            for (unsigned x = 0; x < n; ++x) {
                const auto* c = &in[static_cast<std::size_t>(2 * y) * child + 2 * x];
                const std::uint8_t a = c[0], b = c[1], d = c[child], e = c[child + 1];
                out[static_cast<std::size_t>(y) * n + x] =
                    static_cast<std::uint8_t>(((a | b | d | e) & 1) | (a & b & d & e & 2));
            }
    }
    lod.holes.resize(static_cast<std::size_t>(w + 1) * (w + 1));
    for (unsigned y = 0; y <= w; ++y)
        for (unsigned x = 0; x <= w; ++x)
            lod.holes[static_cast<std::size_t>(y) * (w + 1) + x] =
                static_cast<std::uint8_t>(terrain.raw(x, y) >> 15);
    lod.heights.resize(lod.holes.size());
    for (unsigned y = 0; y <= w; ++y)
        for (unsigned x = 0; x <= w; ++x)
            lod.heights[static_cast<std::size_t>(y) * (w + 1) + x] =
                height(static_cast<long long>(y) * w + x);
    // UpdateVariance, including the terms where the original reads one endpoint twice.
    const auto full = [&](long long s, long long k) {
        return std::fabs(height(s) - (height(s + 2 * k) + height(s + k)) * 0.5f);
    };
    const auto twice = [&](long long s, long long k) { return std::fabs(height(s) - height(s + 2 * k)); };
    const long long W = w;
    {
        const unsigned n = w >> 1;
        auto& out = lod.variance[1];
        out.resize(static_cast<std::size_t>(n) * n);
        for (unsigned by = 0; by < n; ++by)
            for (unsigned bx = 0; bx < n; ++bx) {
                const long long s = 2ll * bx + 2ll * by * W;
                float v = full(s, twice(s, W) >= twice(s, 1) ? W : 1);
                v = std::max(v, full(s, W + 1));
                if (bx) {
                    v = std::max(v, full(s, -1));
                    v = std::max(v, full(s, W - 1));
                    if (by)
                        v = std::max(v, full(s, -W - 1));
                }
                if (by) {
                    v = std::max(v, full(s, -W));
                    v = std::max(v, full(s, 1 - W));
                }
                out[static_cast<std::size_t>(by) * n + bx] = v;
            }
    }
    for (unsigned level = 2; level < levels; ++level) {
        const unsigned n = w >> level, child = n * 2;
        const long long h = 1ll << (level - 1);
        auto& out = lod.variance[level];
        const auto& in = lod.variance[level - 1];
        out.resize(static_cast<std::size_t>(n) * n);
        for (unsigned by = 0; by < n; ++by)
            for (unsigned bx = 0; bx < n; ++bx) {
                // LAND::CalculateVariance 0x1603d0b0.
                const long long s = 2 * h * bx + 2 * h * W * by;
                const long long pick = full(s, W * h) >= full(s, h) ? W * h : h;
                float v = twice(s, pick);
                v = std::max(v, twice(s, W * h + h));
                if (bx) {
                    v = std::max(v, full(s, -h));
                    v = std::max(v, full(s, W * h - h));
                    if (by)
                        v = std::max(v, full(s, -W * h - h));
                }
                if (by) {
                    v = std::max(v, twice(s, -W * h));
                    v = std::max(v, full(s, h - W * h));
                }
                const auto* c = &in[static_cast<std::size_t>(2 * by) * child + 2 * bx];
                out[static_cast<std::size_t>(by) * n + bx] =
                    std::max({v, std::max(c[0], c[1]), std::max(c[child], c[child + 1])});
            }
    }
    return lod;
}
TerrainLod::View TerrainLod::classify(const Camera& camera, float range) const {
    View view;
    if (!size)
        return view;
    const float eye_x = camera.eye[0], eye_y = camera.eye[1], forward_x = camera.forward[0],
                forward_y = camera.forward[1];
    // sub_16046380: window i spans 8 << i cells at 32 >> i texels; layer 5 is the whole map at one texel.
    // UpdateTextureWindow places window i at the eye cell less a forward offset of up to the full window.
    const auto eye_cell = [](float p) { return -static_cast<int>(p * -0.1f); };
    const auto place = [&](float forward, float eye, unsigned shift, unsigned cells) {
        const auto offset = (static_cast<int>(128.0f - forward * 128.0f) & (-1 << shift)) >> shift;
        return std::clamp(eye_cell(eye) - offset, 0, static_cast<int>(size - cells));
    };
    for (unsigned i = 0; i < 5; ++i) {
        const unsigned shift = 5 - i, cells = 8u << i;
        view.windows[i] = {place(forward_x, eye_x, shift, cells), place(forward_y, eye_y, shift, cells),
                           cells, 1u << shift};
    }
    view.windows[5] = {0, 0, size, 1};
    // ILANDVIEW::SetQuality halves 0.25 down the table, so a level L quad splits below 2^(L-9).
    std::array<float, levels> threshold{};
    threshold[levels - 1] = 0.25f;
    for (unsigned level = levels - 1; level-- > 1;)
        threshold[level] = threshold[level + 1] * 0.5f;
    const float inverse = 1.0f / (range * 256.0f);

    // The land view's triangle bintree (sub_16038BD0). Quads carry the split decision; faces are triangles
    // whose edge 0 is the hypotenuse. Edge types 4-7 are quad diagonals (their halves stay in the quad) and
    // 0-3 quad sides (their halves move to the child quads the table names).
    struct Quad {
        unsigned level, flags;
        int x, y;
        float variance;
        int window; // -1: near tile batches.
        int children{-1};
    };
    struct Edge {
        int twin, face;
        unsigned point, type;
    };
    struct Face {
        std::array<int, 3> edges;
        int quad;
        unsigned flags; // Bit 0 culled, bit 1 split.
    };
    // 0x16056DD8: types of the two new edges and, for side splits, the child quads of the two halves.
    static constexpr std::array<std::array<unsigned, 4>, 8> table{{{1, 2, 0, 1},
                                                                   {3, 0, 1, 3},
                                                                   {0, 3, 2, 0},
                                                                   {2, 1, 3, 2},
                                                                   {5, 6, 0, 0},
                                                                   {7, 4, 0, 0},
                                                                   {4, 7, 0, 0},
                                                                   {6, 5, 0, 0}}};
    const unsigned pitch = size + 1;
    std::vector<Quad> quads;
    std::vector<Edge> edges;
    std::vector<Face> faces;
    quads.push_back({levels, 2, 0, 0, 10000.0f, 5});
    // sub_16039950: the four children of a quad, with their split flags and texture windows.
    const auto children = [&](int parent) {
        if (quads[parent].children >= 0)
            return quads[parent].children;
        const auto first = static_cast<int>(quads.size());
        const Quad p = quads[parent];
        const unsigned level = p.level - 1, cells = 1u << level;
        for (unsigned i = 0; i < 4; ++i) {
            Quad q{level, 0, p.x * 2 + static_cast<int>(i & 1), p.y * 2 + static_cast<int>(i >> 1), 0, -1};
            if (level) {
                const auto index = static_cast<std::size_t>(q.y) * (size >> level) + q.x;
                const auto flag = flags[level][index];
                q.variance = variance[level][index];
                q.window = p.window;
                if (flag & 2)
                    q.flags = 1;
                else {
                    const float dx = eye_x - q.x * cells * 10.0f, dy = eye_y - q.y * cells * 10.0f;
                    const float distance = std::sqrt(dy * dy + dx * dx);
                    const float far = distance >= cells * 14.2f ? distance * inverse : 0.0f;
                    float scale = 1.0f / (q.variance * 60.0f / distance + 1.0f);
                    if ((flag & 1) && scale > 0.25f)
                        scale = 0.25f;
                    if (scale * far < threshold[level])
                        q.flags = 2;
                    // Step to finer windows while the next one contains the quad.
                    const int x0 = q.x * static_cast<int>(cells), y0 = q.y * static_cast<int>(cells),
                              extent = static_cast<int>(cells);
                    while (q.window > 0) {
                        const auto& finer = view.windows[q.window - 1];
                        const int span = static_cast<int>(finer.cells);
                        if (x0 < finer.x || y0 < finer.y || x0 + extent > finer.x + span ||
                            y0 + extent > finer.y + span)
                            break;
                        --q.window;
                    }
                }
            }
            quads.push_back(q);
        }
        quads[parent].children = first;
        return first;
    };
    // Root: corners P0..P3 = (0,0), (size,0), (0,size), (size,size), split along P0-P3.
    const unsigned corner[] = {0, size, size * pitch, size * pitch + size};
    edges = {{-1, 0, corner[0], 0}, {-1, 0, corner[1], 1}, {-1, 1, corner[2], 2},
             {-1, 1, corner[3], 3}, {5, 1, corner[0], 4},  {4, 0, corner[3], 7}};
    faces = {{{5, 0, 1}, 0, 2}, {{4, 3, 2}, 0, 2}};
    // sub_160388C0: five inward planes (left, right, top, bottom, near) from the camera rows, widened by 10
    // degrees (at most 80). Each plane passes through a near point 15 units behind the eye, raised by 50 when
    // its normal faces down, and its fifth value -|n.z| lets a point sink 2 x variance below it.
    std::array<std::array<float, 5>, 5> planes{};
    {
        const auto side = [&](float degrees, const std::array<float, 3>& axis, float sign) {
            const double angle = std::min(degrees + 10.0, 80.0) * 0.01745277777777778;
            const float s = static_cast<float>(std::sin(angle)), c = static_cast<float>(std::cos(angle));
            return std::array<float, 5>{axis[0] * c * sign + camera.forward[0] * s,
                                        axis[1] * c * sign + camera.forward[1] * s,
                                        axis[2] * c * sign + camera.forward[2] * s};
        };
        planes = {side(camera.half_width, camera.right, 1), side(camera.half_width, camera.right, -1),
                  side(camera.half_height, camera.up, 1), side(camera.half_height, camera.up, -1),
                  std::array<float, 5>{camera.forward[0], camera.forward[1], camera.forward[2]}};
        const float near[3]{camera.eye[0] - camera.forward[0] * 15.0f,
                            camera.eye[1] - camera.forward[1] * 15.0f,
                            camera.eye[2] - camera.forward[2] * 15.0f};
        for (auto& plane : planes) {
            const float lift = plane[2] <= 0 ? 50.0f : 0.0f;
            plane[4] = plane[2] <= 0 ? plane[2] : -plane[2];
            plane[3] = -(plane[0] * near[0] + plane[2] * (near[2] + lift) + near[1] * plane[1]);
        }
    }
    std::vector<std::uint8_t> outcodes(heights.size());
    const auto outcode = [&](unsigned point, float slack) {
        const float x = static_cast<float>(point % pitch) * 10.0f,
                    y = static_cast<float>(point / pitch) * 10.0f, z = heights[point];
        std::uint8_t code = 0;
        for (unsigned k = 0; k < planes.size(); ++k) {
            const auto& n = planes[k];
            if (slack * n[4] + slack * n[4] > x * n[0] + z * n[2] + y * n[1] + n[3])
                code = static_cast<std::uint8_t>(code | 1u << k);
        }
        outcodes[point] = code;
    };
    for (const auto point : corner)
        outcode(point, 10000.0f);
    // sub_160398D0: faces of hole quads, or with all three points outside one plane, are culled and unlinked
    // from their neighbours.
    const auto cull = [&](int face) {
        auto& f = faces[face];
        if (!(f.flags & 1) && !(outcodes[edges[f.edges[0]].point] & outcodes[edges[f.edges[1]].point] &
                                outcodes[edges[f.edges[2]].point]))
            return;
        f.flags = 1;
        for (const auto e : f.edges)
            if (const auto twin = edges[e].twin; twin >= 0) {
                edges[twin].twin = -1;
                edges[e].twin = -1;
            }
    };
    // Bisects `face` along edge 0 at `middle` (sub_16039520's body; sub_16039CE0 for the neighbour). Returns
    // the new edge from the midpoint to the end of the old edge 0.
    const auto bisect = [&](int face, unsigned middle) {
        const int e0 = faces[face].edges[0], e1 = faces[face].edges[1], e2 = faces[face].edges[2];
        const auto type = edges[e0].type;
        const auto& row = table[type];
        const auto added = static_cast<int>(faces.size()), a = static_cast<int>(edges.size()), b = a + 1,
                   c = a + 2;
        edges.push_back({b, added, edges[e2].point, row[1]});
        edges.push_back({a, face, middle, row[0]});
        edges.push_back({edges[e0].twin, added, middle, type});
        edges[e1].face = added;
        faces[face].edges = {e2, e0, b};
        faces.push_back({{e1, a, c}, faces[face].quad, 0});
        if (type & 4)
            faces[added].flags = quads[faces[added].quad].flags;
        else {
            const auto first = children(faces[face].quad);
            faces[added].quad = first + static_cast<int>(row[3]);
            faces[face].quad = first + static_cast<int>(row[2]);
            faces[added].flags = quads[faces[added].quad].flags;
            faces[face].flags = quads[faces[face].quad].flags;
        }
        return c;
    };
    const auto split = [&](auto& self, int face) -> void {
        const int e0 = faces[face].edges[0];
        // Force the neighbour across the hypotenuse to the same level first, so no crack opens.
        for (auto twin = edges[e0].twin; twin >= 0 && faces[edges[twin].face].edges[0] != twin;
             twin = edges[e0].twin)
            self(self, edges[twin].face);
        const auto twin = edges[e0].twin;
        const unsigned p = edges[e0].point, q = edges[faces[face].edges[1]].point;
        const unsigned middle = (p % pitch + q % pitch) / 2 + (p / pitch + q / pitch) / 2 * pitch;
        const auto mine = bisect(face, middle);
        outcode(middle, quads[faces[face].quad].variance);
        const auto added = edges[mine].face;
        if (twin >= 0) {
            const auto theirs = bisect(edges[twin].face, middle);
            edges[e0].twin = theirs;
            edges[twin].twin = mine;
            edges[theirs].twin = e0;
            cull(edges[twin].face);
            cull(edges[theirs].face);
        }
        cull(face);
        cull(added);
    };
    // sub_160391A0: split faces in creation order until their quads stop asking.
    for (std::size_t i = 0; i < faces.size(); ++i)
        while (faces[i].flags & 2)
            split(split, static_cast<int>(i));
    // sub_160391E0: a face with one hole vertex, not its third, flips the diagonal it shares with a later
    // diamond partner whose far vertex is solid; faces still touching a hole are dropped.
    const auto hole = [&](unsigned point) { return holes[point] != 0; };
    std::vector<int> drawn;
    for (std::size_t j = 0; j < faces.size(); ++j) {
        auto& f = faces[j];
        if (f.flags & 1)
            continue;
        const auto count = [&] {
            return hole(edges[f.edges[0]].point) + hole(edges[f.edges[1]].point) +
                   hole(edges[f.edges[2]].point);
        };
        auto holes_here = count();
        while (holes_here == 1 && !hole(edges[f.edges[2]].point)) {
            const int e0 = f.edges[0], twin = edges[e0].twin;
            if (twin < 0)
                break;
            const auto other = edges[twin].face;
            auto& g = faces[other];
            if (other < static_cast<int>(j) || g.edges[0] != twin || hole(edges[g.edges[2]].point))
                break;
            edges[e0].type ^= 2;
            edges[twin].type ^= 2;
            edges[e0].point = edges[g.edges[2]].point;
            edges[twin].point = edges[f.edges[2]].point;
            const auto mine = f.edges[1], theirs = g.edges[1];
            f.edges[1] = f.edges[2];
            g.edges[1] = g.edges[2];
            f.edges[2] = theirs;
            g.edges[2] = mine;
            holes_here = count();
        }
        if (holes_here) {
            for (const auto e : f.edges)
                if (const auto twin = edges[e].twin; twin >= 0)
                    edges[twin].twin = -1;
            continue;
        }
        drawn.push_back(static_cast<int>(j));
        const auto window = quads[f.quad].window;
        view.triangles.push_back({{edges[f.edges[0]].point, edges[f.edges[1]].point, edges[f.edges[2]].point},
                                  window < 0 ? near_tiles : static_cast<std::uint8_t>(window)});
    }
    // sub_160391E0's line list: every unshared edge of a drawn face (holes, culled neighbours, the map
    // border).
    for (const auto j : drawn)
        for (unsigned k = 0; k < 3; ++k)
            if (edges[faces[j].edges[k]].twin < 0)
                view.edges.push_back(
                    {edges[faces[j].edges[k]].point, edges[faces[j].edges[(k + 1) % 3]].point});
    return view;
}
} // namespace sacrifice
