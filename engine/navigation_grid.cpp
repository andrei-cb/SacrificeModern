#include "sacrifice/navigation.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
int integer(double value) {
    if (!std::isfinite(value) || value >= 9223372036854775808.0 || value < -9223372036854775808.0)
        return 0;
    return std::bit_cast<int>(static_cast<unsigned>(static_cast<std::int64_t>(value)));
}
// LAND::CreateConnectivityIsland uses a bounded 1024-span stack, not an unbounded flood queue.
void island(std::vector<std::uint8_t>& pixels, int x, int y, std::uint8_t old, std::uint8_t replacement) {
    struct Span {
        int y, left, right, direction;
    };
    std::array<Span, 1024> spans;
    unsigned count = 0;
    const auto push = [&](int row, int left, int right, int direction) {
        if (count < spans.size() && row + direction >= 0 && row + direction < 256)
            spans[count++] = {row, left, right, direction};
    };
    push(y, x, x, 1);
    push(y + 1, x, x, -1);
    while (count) {
        const auto span = spans[--count];
        const int row = span.y + span.direction;
        int at = span.left;
        const auto get = [&](int column) -> std::uint8_t& { return pixels[row * 256 + column]; };
        while (at >= 0 && get(at) == old)
            get(at--) = replacement;
        int left = at + 1;
        bool fill = at < span.left;
        if (fill) {
            if (left < span.left)
                push(row, left, span.left - 1, -span.direction);
            at = span.left + 1;
        } else {
            do {
                ++at;
            } while (at <= span.right && get(at) != old);
            left = at;
        }
        while (fill || at <= span.right) {
            fill = false;
            while (at < 256 && get(at) == old)
                get(at++) = replacement;
            push(row, left, at - 1, span.direction);
            if (at > span.right + 1)
                push(row, span.right + 1, at - 1, -span.direction);
            do {
                ++at;
            } while (at <= span.right && get(at) != old);
            left = at;
            if (at > span.right)
                break;
        }
    }
}
} // namespace
NavigationGrid NavigationGrid::build(const Terrain& terrain, const TerrainMaterials& materials) {
    if (terrain.width != width || terrain.height != width || terrain.heights.size() != cells)
        throw Error("Native match navigation requires a 256 by 256 terrain");
    if (!terrain.tiles.empty() && terrain.tiles.size() != cells)
        throw Error("Navigation tile count mismatch");
    if (!terrain.scar_layers.empty() && terrain.scar_layers.size() != cells)
        throw Error("Navigation scar count mismatch");
    NavigationGrid result;
    result.heights.assign(width * (width + 2) + 4, 0);
    std::copy(terrain.heights.begin(), terrain.heights.end(), result.heights.begin());
    std::copy_n(terrain.heights.begin(), width, result.heights.begin() + cells);
    result.heights[cells + width] = terrain.heights[0];
    result.regions.assign(result.heights.size(), 0);
    for (unsigned i = 0; i < cells + width; ++i)
        if (!(result.heights[i] & 0x8000))
            result.regions[i] = 255;
    int label = 254;
    for (unsigned y = 0; y < width; ++y)
        for (unsigned x = 0; x < width; ++x)
            if (result.regions[y * width + x] == 255 && label > 0)
                island(result.regions, static_cast<int>(x), static_cast<int>(y), 255,
                       static_cast<std::uint8_t>(label--));
    // Once all 254 labels are used, native fallback 255 leaves further islands unchanged.
    result.flags[0].resize(cells);
    for (unsigned y = 0; y < width; ++y)
        for (unsigned x = 0; x < width; ++x) {
            const auto i = y * width + x;
            const unsigned holes = (result.heights[i] >> 15) + (result.heights[i + 1] >> 15) +
                                   (result.heights[i + width] >> 15) + (result.heights[i + width + 1] >> 15);
            result.flags[0][i] = static_cast<std::uint8_t>((holes ? 1 : 0) | (holes > 1 ? 2 : 0));
        }
    for (unsigned level = 1; level < result.flags.size(); ++level) {
        const unsigned size = width >> level, child = size * 2;
        result.flags[level].resize(size * size);
        for (unsigned y = 0; y < size; ++y)
            for (unsigned x = 0; x < size; ++x) {
                const auto* p = &result.flags[level - 1][2 * y * child + 2 * x];
                result.flags[level][y * size + x] =
                    static_cast<std::uint8_t>(((p[0] | p[1] | p[child] | p[child + 1]) & 1) |
                                              (p[0] & p[1] & p[child] & p[child + 1] & 2));
            }
    }
    result.speeds.resize(cells);
    result.terrain_cost.resize(cells);
    for (unsigned i = 0; i < cells; ++i) {
        const unsigned tile = !terrain.scar_layers.empty() && terrain.scar_layers[i] ? terrain.scar_layers[i]
                              : terrain.tiles.empty()                                ? 0
                                                                                     : terrain.tiles[i];
        const std::array<std::uint8_t, 12>* properties = nullptr;
        if (tile < 256) {
            properties = &materials.tile_properties[tile];
        } else {
            if (tile - 256 >= materials.scars.size())
                throw Error("Navigation scar tile is missing");
            properties = &materials.scars[tile - 256].properties;
        }
        result.speeds[i] = properties ? (*properties)[1] : 0;
        result.terrain_cost[i] = properties ? (*properties)[3] : 0;
    }
    return result;
}
std::uint8_t NavigationGrid::region(Vec3 point) const {
    const int x = integer(point.x * 0.1), y = integer(point.y * 0.1);
    if (x < 0 || y < 0 || x >= 256 || y >= 256)
        return 0;
    const auto index = static_cast<unsigned>(x + y * 256);
    for (const auto offset : {0u, 1u, width, width + 1})
        if (regions[index + offset])
            return regions[index + offset];
    return 0;
}
bool NavigationGrid::connected(Vec3 from, Vec3 to) const {
    const auto a = region(from);
    return a && a == region(to);
}
std::uint16_t NavigationGrid::cell(Vec3 point) {
    return static_cast<std::uint16_t>(static_cast<unsigned>(integer(point.x * 0.1)) -
                                      width * static_cast<unsigned>(integer(point.y * -0.1)));
}
Navigation::Navigation(NavigationGrid grid)
    : grid_(std::move(grid)), buffers_(3 * NavigationGrid::cells), nodes_(search_capacity + 2),
      lookup_(NavigationGrid::cells, -1), routes_(route_capacity) {
    if (grid_.heights.size() < NavigationGrid::cells || grid_.flags[0].size() != NavigationGrid::cells ||
        grid_.flags[1].size() != 16384 || grid_.regions.size() < NavigationGrid::cells + 257 ||
        grid_.speeds.size() != NavigationGrid::cells || grid_.terrain_cost.size() != NavigationGrid::cells)
        throw Error("Invalid native navigation grid");
    for (unsigned i = 0; i < search_capacity; ++i)
        nodes_[i].next = i + 1 < search_capacity ? static_cast<int>(i + 1) : -1;
    nodes_[first].next = last;
    nodes_[first].score = 0;
    nodes_[last].previous = first;
    nodes_[last].score = 1000000;
    for (unsigned i = 0; i < route_capacity; ++i)
        routes_[i].next = i + 1 < route_capacity ? static_cast<int>(i + 1) : -1;
}
void Navigation::refresh_land(const Terrain& terrain, bool rebuild_regions) {
    if (terrain.width != NavigationGrid::width || terrain.height != NavigationGrid::width ||
        terrain.heights.size() != NavigationGrid::cells)
        return;
    std::copy(terrain.heights.begin(), terrain.heights.end(), grid_.heights.begin());
    std::copy_n(terrain.heights.begin(), NavigationGrid::width, grid_.heights.begin() + NavigationGrid::cells);
    grid_.heights[NavigationGrid::cells + NavigationGrid::width] = terrain.heights[0];
    if (!rebuild_regions)
        return;
    grid_.regions.assign(grid_.heights.size(), 0);
    for (unsigned i = 0; i < NavigationGrid::cells + NavigationGrid::width; ++i)
        if (!(grid_.heights[i] & 0x8000))
            grid_.regions[i] = 255;
    int label = 254;
    for (unsigned y = 0; y < NavigationGrid::width; ++y)
        for (unsigned x = 0; x < NavigationGrid::width; ++x)
            if (grid_.regions[y * NavigationGrid::width + x] == 255 && label > 0)
                island(grid_.regions, static_cast<int>(x), static_cast<int>(y), 255,
                       static_cast<std::uint8_t>(label--));
    grid_.flags[0].assign(NavigationGrid::cells, 0);
    for (unsigned y = 0; y < NavigationGrid::width; ++y)
        for (unsigned x = 0; x < NavigationGrid::width; ++x) {
            const auto i = y * NavigationGrid::width + x;
            const unsigned holes = (grid_.heights[i] >> 15) + (grid_.heights[i + 1] >> 15) +
                                   (grid_.heights[i + NavigationGrid::width] >> 15) +
                                   (grid_.heights[i + NavigationGrid::width + 1] >> 15);
            grid_.flags[0][i] = static_cast<std::uint8_t>((holes ? 1 : 0) | (holes > 1 ? 2 : 0));
        }
    for (unsigned level = 1; level < grid_.flags.size(); ++level) {
        const unsigned size = NavigationGrid::width >> level, child = size * 2;
        grid_.flags[level].assign(size * size, 0);
        for (unsigned y = 0; y < size; ++y)
            for (unsigned x = 0; x < size; ++x) {
                const auto* p = &grid_.flags[level - 1][2 * y * child + 2 * x];
                grid_.flags[level][y * size + x] =
                    static_cast<std::uint8_t>(((p[0] | p[1] | p[child] | p[child + 1]) & 1) |
                                              (p[0] & p[1] & p[child] & p[child + 1] & 2));
            }
    }
    dirty_static();
}
std::span<const std::uint8_t> Navigation::costs() const {
    return {buffers_.data() + visible_ * NavigationGrid::cells, NavigationGrid::cells};
}
std::span<const std::uint8_t> Navigation::base_costs() const {
    return {buffers_.data() + 2 * NavigationGrid::cells, NavigationGrid::cells};
}
void Navigation::advance(int ticks, bool force) {
    if (ticks < 0)
        throw Error("Negative navigation tick count");
    if (static_dirty_) {
        static_dirty_ = false;
        elapsed_ = 10000;
        dirty_ = 1;
        std::copy(grid_.terrain_cost.begin(), grid_.terrain_cost.end(),
                  buffers_.begin() + 2 * NavigationGrid::cells);
        if (rebuild_static)
            rebuild_static(*this);
        force = true;
    }
    if (dirty_) {
        elapsed_ = std::bit_cast<int>(static_cast<unsigned>(elapsed_) + static_cast<unsigned>(ticks));
        if (elapsed_ > 31)
            force = true;
    }
    if (visible_ != next_)
        visible_ = next_;
    if (force) {
        next_ = visible_ ^ 1;
        elapsed_ = dirty_ = 0;
        std::copy(base_costs().begin(), base_costs().end(), buffers_.begin() + next_ * NavigationGrid::cells);
    }
}
void Navigation::write(int map, std::int64_t index, int amount, bool maximum) {
    index += static_cast<std::int64_t>(map) * NavigationGrid::cells;
    if (index < 0 || index >= static_cast<std::int64_t>(buffers_.size()))
        throw Error("Navigation stamp leaves the native three-map allocation");
    auto& destination = buffers_[static_cast<std::size_t>(index)];
    if (!maximum || amount > destination)
        destination = static_cast<std::uint8_t>(amount);
}
} // namespace sacrifice
