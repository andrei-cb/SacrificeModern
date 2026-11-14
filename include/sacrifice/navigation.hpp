#pragma once
#include "motion.hpp"
#include <functional>

namespace sacrifice {
// LAND connectivity / height-mipmap inputs, plus the game's 256x256 path grid (0x456570).
struct NavigationGrid {
    static constexpr unsigned width = 256, cells = width * width;
    std::vector<std::uint16_t> heights; // LAND pitch 256, two padded rows plus four zero words.
    std::array<std::vector<std::uint8_t>, 3> flags;
    std::vector<std::uint8_t> regions, speeds, terrain_cost;
    static NavigationGrid build(const Terrain& terrain, const TerrainMaterials& materials);
    std::uint8_t region(Vec3 point) const;    // 0x470120: first nonzero of four sample corners.
    bool connected(Vec3 from, Vec3 to) const; // 0x4700e0: region identity, not a line-of-sight test.
    static std::uint16_t cell(Vec3 point);    // Native low-word coordinate arithmetic.
};
class Navigation;
// Creature +2900. Navigation must outlive paths bound to it; paths release their shared pool nodes.
class NavigationPath {
  public:
    NavigationPath() = default;
    ~NavigationPath();
    NavigationPath(const NavigationPath&) = delete;
    NavigationPath& operator=(const NavigationPath&) = delete;
    Vec3 target;
    float unused{}, avoidance{}, elapsed{}; // +16, +20 obstruction score, +24 elapsed seconds.
    void clear();
    void advance(int ticks); // 0x45a300: elapsed advances even with no current route.
  private:
    friend class Navigation;
    Navigation* owner_{};
    int head_{-1};
};
class Navigation {
  public:
    static constexpr unsigned search_capacity = 8192, route_capacity = 4096;
    explicit Navigation(NavigationGrid grid);
    ~Navigation() = default;
    Navigation(const Navigation&) = delete;
    Navigation& operator=(const Navigation&) = delete;
    const NavigationGrid& grid() const {
        return grid_;
    }
    struct Waypoint {
        Vec2 position;
        std::uint16_t cell{};
        std::uint8_t clearance{}, maximum_cost{};
    };
    struct Result {
        float cost{};
        unsigned considered{}, allocated{}, expanded{}, compressed{};
        bool reached{}, exhausted{}, route_exhausted{};
        std::uint16_t end{};
    };
    // 0x46e960: no implicit clearing of an existing route. Cost zero is also a valid native result.
    Result search(Vec3 from, Vec3 to, NavigationPath* route = nullptr, int budget = 0);
    // 0x46fd10: -1 disconnected/no usable path, 0 direct approach, 1 cached waypoint,
    // 2 cached waypoint after advancing a segment, 3 newly generated waypoint. Writes only X/Y.
    int step(NavigationPath& path, Vec3 from, Vec3 to, Vec3& waypoint, int budget = 0);
    bool near_goal(const NavigationPath& path) const; // 0x470090.
    std::vector<Waypoint> waypoints(const NavigationPath& path) const;
    unsigned free_waypoints() const;
    void clear(NavigationPath& path);
    void
    discard_waypoint(NavigationPath& path); // 0x46fb90, avoidance pops one head without resetting the cache.
    // 0x4701c0: delayed double-buffer swap. The initial rebuilt map becomes visible next call.
    void advance(int ticks, bool force = false);
    void dirty_static() {
        static_dirty_ = true;
    }
    // Recopy LAND heights after deformation. Changes to hole flags also rebuild regions and mips.
    void refresh_land(const Terrain& terrain, bool rebuild_regions = true);
    // LAND::SetTile: speeds are read live; terrain costs are read again by the next static rebuild.
    void retile(unsigned cell, const std::array<std::uint8_t, 12>& properties) {
        grid_.speeds.at(cell) = properties[1];
        grid_.terrain_cost.at(cell) = properties[3];
    }
    std::function<void(Navigation&)> rebuild_static; // 0x470260: terrain costs, then building callbacks.
    std::span<const std::uint8_t> costs() const;
    std::span<const std::uint8_t> base_costs() const;
    // 0x4702f0: rectangle dimensions, collision box, annulus radii, or stroked vector/radius.
    void stamp(unsigned shape, Vec3 position, std::span<const float> parameters, int amount,
               bool base = false);
    unsigned clearance(std::uint16_t cell) const; // Native 0/1/2 route compression level.
  private:
    friend class NavigationPath;
    struct Node {
        int next{-1}, previous{-1};
        std::uint16_t cell{}, parent{}, generation{};
        std::uint8_t weight{};
        float cost{}, heuristic{}, score{};
    };
    struct RouteNode {
        int next{-1};
        Waypoint value;
    };
    static constexpr int first = search_capacity, last = search_capacity + 1;
    NavigationGrid grid_;
    std::vector<std::uint8_t> buffers_; // Two cost maps followed by the static base; flat native indexing.
    unsigned visible_{}, next_{};
    int dirty_{}, elapsed_{};
    bool static_dirty_{true};
    std::vector<Node> nodes_;
    std::vector<int> lookup_;
    int free_node_{};
    std::vector<RouteNode> routes_;
    int free_route_{};
    std::uint16_t generation_{}, start_cell_{}, goal_cell_{};
    float heuristic_weight_{}, band_{}, bias_{};
    Result result_;
    void reset_search();
    int relax(std::uint16_t cell, float cost, std::uint16_t parent, bool starting = false);
    void insert(int node);
    void reposition(int node);
    void detach(int node);
    double distance(std::uint16_t a, std::uint16_t b) const;
    int allocate_route(NavigationPath& path, std::uint16_t cell);
    void pop_route(NavigationPath& path);
    float finish(int node, Vec3 from, Vec3 to, NavigationPath* path);
    std::uint16_t group(std::uint16_t cell, unsigned level) const;
    void write(int map, std::int64_t cell, int amount, bool maximum);
};
} // namespace sacrifice
