#include "sacrifice/navigation.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
constexpr float increment = std::bit_cast<float>(953267991u);
constexpr float bias_step = std::bit_cast<float>(973279855u);
constexpr float diagonal = std::bit_cast<float>(1068827861u);
int integer(double value) {
    if (!std::isfinite(value) || value >= 9223372036854775808.0 || value < -9223372036854775808.0)
        return 0;
    return std::bit_cast<int>(static_cast<unsigned>(static_cast<std::int64_t>(value)));
}
} // namespace
NavigationPath::~NavigationPath() {
    clear();
}
void NavigationPath::clear() {
    if (owner_)
        owner_->clear(*this);
    else {
        head_ = -1;
        target = {};
        unused = avoidance = elapsed = 0;
    }
}
void NavigationPath::advance(int ticks) {
    if (ticks < 0)
        throw Error("Negative cached-path tick count");
    elapsed = static_cast<float>(elapsed + ticks * (1.0 / 60.0));
}
void Navigation::clear(NavigationPath& path) {
    if (path.owner_ && path.owner_ != this)
        throw Error("Cached path belongs to another navigation service");
    while (path.head_ != -1)
        pop_route(path);
    path.target = {};
    path.unused = path.avoidance = path.elapsed = 0;
    path.owner_ = nullptr;
}
void Navigation::pop_route(NavigationPath& path) {
    if (path.head_ == -1)
        return;
    const int index = path.head_;
    path.head_ = routes_[index].next;
    routes_[index].next = free_route_;
    free_route_ = index;
}
void Navigation::discard_waypoint(NavigationPath& path) {
    if (path.owner_ && path.owner_ != this)
        throw Error("Cached path belongs to another navigation service");
    pop_route(path);
}
unsigned Navigation::free_waypoints() const {
    unsigned count = 0;
    for (int at = free_route_; at != -1; at = routes_[at].next)
        ++count;
    return count;
}
std::vector<Navigation::Waypoint> Navigation::waypoints(const NavigationPath& path) const {
    if (path.owner_ && path.owner_ != this)
        throw Error("Cached path belongs to another navigation service");
    std::vector<Waypoint> result;
    for (int at = path.head_; at != -1; at = routes_[at].next)
        result.push_back(routes_[at].value);
    return result;
}
unsigned Navigation::clearance(std::uint16_t cell) const {
    if (grid_.flags[0][cell] & 1)
        return 0;
    const auto sample = [&](int offset) {
        const auto index = static_cast<std::int64_t>(visible_) * NavigationGrid::cells + cell + offset;
        if (index < 0 || index >= static_cast<std::int64_t>(buffers_.size()))
            throw Error("Navigation clearance leaves the native cost allocation");
        return buffers_[static_cast<std::size_t>(index)];
    };
    const auto value = sample(0);
    if (value != sample(1) || value != sample(-1) || value != sample(256) || value != sample(-256))
        return 0;
    const unsigned mip = ((cell & 0xfe) >> 1) | ((cell & 0xfe00) >> 2);
    return grid_.flags[1][mip] & 1 ? 1 : 2; // The native cap is 2; mip 2 is not tested here.
}
std::uint16_t Navigation::group(std::uint16_t cell, unsigned level) const {
    const unsigned low = (1u << level) - 1;
    return static_cast<std::uint16_t>(cell & ~(low | (low << 8)));
}
int Navigation::allocate_route(NavigationPath& path, std::uint16_t cell) {
    if (free_route_ == -1) {
        result_.route_exhausted = true;
        return -1;
    }
    const int at = free_route_;
    free_route_ = routes_[at].next;
    auto& route = routes_[at];
    route.next = path.head_;
    path.head_ = at;
    route.value = {Vec2{static_cast<float>((cell & 255) * 10 + 5), static_cast<float>((cell >> 8) * 10 + 5)},
                   cell, static_cast<std::uint8_t>(clearance(cell)), costs()[cell]};
    return at;
}
void Navigation::reset_search() {
    if (nodes_[first].next != last) {
        nodes_[nodes_[last].previous].next = free_node_;
        free_node_ = nodes_[first].next;
        nodes_[first].next = last;
        nodes_[last].previous = first;
    }
    if (nodes_[last].next != -1) {
        int tail = nodes_[last].next;
        while (nodes_[tail].next != -1)
            tail = nodes_[tail].next;
        nodes_[tail].next = free_node_;
        free_node_ = nodes_[last].next;
        nodes_[last].next = -1;
    }
}
double Navigation::distance(std::uint16_t a, std::uint16_t b) const {
    const int x = static_cast<int>(a & 255) - static_cast<int>(b & 255),
              y = static_cast<int>(a >> 8) - static_cast<int>(b >> 8);
    return std::sqrt(static_cast<double>(x * x + y * y));
}
void Navigation::detach(int index) {
    const auto& node = nodes_[index];
    nodes_[node.previous].next = node.next;
    nodes_[node.next].previous = node.previous;
}
void Navigation::insert(int index) {
    int next = nodes_[first].next;
    while (nodes_[next].score < nodes_[index].score)
        next = nodes_[next].next;
    auto& node = nodes_[index];
    node.next = next;
    node.previous = nodes_[next].previous;
    nodes_[node.previous].next = index;
    nodes_[next].previous = index;
}
void Navigation::reposition(int index) {
    auto& node = nodes_[index];
    if (node.score >= nodes_[node.previous].score) {
        if (node.score > nodes_[node.next].score) {
            int next = node.next;
            detach(index);
            do {
                next = nodes_[next].next;
            } while (node.score > nodes_[next].score);
            node.next = next;
            node.previous = nodes_[next].previous;
            nodes_[node.previous].next = index;
            nodes_[next].previous = index;
        }
    } else {
        int previous = node.previous;
        detach(index);
        do {
            previous = nodes_[previous].previous;
        } while (node.score < nodes_[previous].score);
        node.previous = previous;
        node.next = nodes_[previous].next;
        nodes_[node.next].previous = index;
        nodes_[previous].next = index;
    }
}
int Navigation::relax(std::uint16_t cell, float cost, std::uint16_t parent, bool starting) {
    int index = lookup_[cell];
    if (index != -1 && nodes_[index].cell == cell && nodes_[index].generation == generation_) {
        auto& node = nodes_[index];
        if (node.score >= 1000000)
            return index;
        const double next = static_cast<double>(node.weight) + cost;
        if (next >= node.cost)
            return index;
        node.cost = static_cast<float>(next);
        node.parent = parent;
        node.score = static_cast<float>(static_cast<double>(node.heuristic) + bias_ + next);
        reposition(index);
    } else {
        if (free_node_ == -1) {
            result_.exhausted = true;
            return -1;
        }
        index = free_node_;
        free_node_ = nodes_[index].next;
        lookup_[cell] = index;
        auto& node = nodes_[index];
        node.cell = cell;
        node.parent = parent;
        node.generation = generation_;
        node.weight = costs()[cell];
        node.cost = static_cast<float>(static_cast<double>(node.weight) + cost);
        const double weight = static_cast<double>(heuristic_weight_) + increment;
        heuristic_weight_ = static_cast<float>(weight);
        // 0x46e960's inlined first insertion reloads the stored float weight before its distance
        // helper; 0x46f340 uses the unrounded increment for subsequent insertions.
        const double h =
            (starting ? static_cast<double>(heuristic_weight_) : weight) * distance(cell, goal_cell_);
        node.heuristic = static_cast<float>(h);
        node.score = starting ? static_cast<float>(static_cast<double>(node.cost) + bias_ + h)
                              : static_cast<float>(h + node.cost + bias_);
        insert(index);
        ++result_.allocated;
    }
    ++result_.considered;
    bias_ = static_cast<float>(static_cast<double>(bias_) - bias_step);
    return index;
}
float Navigation::finish(int index, Vec3 from, Vec3 to, NavigationPath* path) {
    if (index == -1)
        return 0;
    result_.end = nodes_[index].cell;
    result_.reached = result_.end == goal_cell_;
    std::uint16_t previous_group = 0;
    if (result_.reached && path) {
        const int allocated = allocate_route(*path, goal_cell_);
        if (allocated == -1)
            return 0;
        routes_[allocated].value.position = {to.x, to.y};
        previous_group = group(goal_cell_, routes_[allocated].value.clearance);
    }
    const float result = nodes_[index].cost;
    unsigned threshold = (3u * nodes_[index].weight) >> 1;
    unsigned level = 0;
    unsigned walked = 0;
    while (nodes_[index].cell != start_cell_) {
        if (++walked > search_capacity)
            throw Error("Native path parent chain did not terminate");
        const auto& node = nodes_[index];
        bool keep = node.weight > threshold;
        if (!keep) {
            level = clearance(node.cell);
            keep = group(node.cell, level) != previous_group;
            if (!keep && path) {
                if (path->head_ == -1)
                    throw Error("Native partial route has no segment to coalesce");
                if (node.weight > routes_[path->head_].value.maximum_cost)
                    routes_[path->head_].value.maximum_cost = node.weight;
            }
        }
        if (keep) {
            if (path && allocate_route(*path, node.cell) == -1)
                return result;
            threshold = (3u * node.weight) >> 1;
            previous_group = group(node.cell, level);
            ++result_.compressed;
        }
        index = lookup_[node.parent];
        if (index == -1)
            throw Error("Missing native path parent");
    }
    if (path) {
        const int at = allocate_route(*path, start_cell_);
        if (at != -1)
            routes_[at].value.position = {from.x, from.y};
    }
    return result;
}
Navigation::Result Navigation::search(Vec3 from, Vec3 to, NavigationPath* route, int budget) {
    if (route) {
        if (route->owner_ && route->owner_ != this)
            throw Error("Cached path belongs to another navigation service");
        route->owner_ = this;
    }
    ++generation_;
    heuristic_weight_ = 0.5f;
    band_ = 1;
    bias_ = 3;
    result_ = {};
    reset_search();
    start_cell_ = NavigationGrid::cell(from);
    goal_cell_ = NavigationGrid::cell(to);
    int limit = std::min(budget ? budget : 1536, 8160), hard = grid_.connected(from, to) ? 8160 : 1024;
    if (grid_.flags[0][goal_cell_] & 1)
        limit = hard = std::min(integer((std::abs(static_cast<double>(from.x) - to.x) +
                                         std::abs(static_cast<double>(from.y) - to.y)) *
                                        0.4),
                                1024);
    const int start = relax(start_cell_, 0, start_cell_, true);
    if (start == -1)
        return result_;
    const float start_h = nodes_[start].heuristic;
    int selected = -1;
    static constexpr std::array<int, 8> delta{-257, -256, -255, 1, 257, 256, 255, -1};
    while (true) {
        selected = nodes_[first].next;
        if (nodes_[selected].score >= 1000000) {
            selected = -1;
            break;
        }
        const float boundary = static_cast<float>(static_cast<double>(nodes_[selected].score) + band_);
        for (int at = nodes_[selected].next; nodes_[at].score < boundary; at = nodes_[at].next)
            if (nodes_[at].heuristic < nodes_[selected].heuristic)
                selected = at;
        detach(selected);
        nodes_[selected].next = nodes_[last].next;
        nodes_[last].next = selected;
        nodes_[selected].score = 1000000;
        band_ = static_cast<float>(static_cast<double>(band_) + increment);
        ++result_.expanded;
        if (nodes_[selected].cell == goal_cell_)
            break;
        if (static_cast<int>(result_.considered) > limit) {
            int best = nodes_[first].next;
            if (nodes_[best].score < 1000000) {
                for (int at = nodes_[best].next; nodes_[at].score < 1000000; at = nodes_[at].next)
                    if (nodes_[at].heuristic < nodes_[best].heuristic)
                        best = at;
            } else
                best = -1;
            if (limit >= hard ||
                (best != -1 && limit * 0.0125 < nodes_[best].cost && nodes_[best].heuristic < start_h)) {
                selected = best;
                break;
            }
            limit = std::min(limit + 1024, hard);
        }
        const auto cell = nodes_[selected].cell;
        const int height = grid_.heights[cell] & 0x7fff;
        const float speed = grid_.speeds[cell] ? static_cast<float>(64.0 / grid_.speeds[cell]) : 1;
        for (unsigned direction = 0; direction < delta.size(); ++direction) {
            const auto neighbor = static_cast<std::uint16_t>(cell + delta[direction]);
            if (neighbor != goal_cell_ && (grid_.flags[0][neighbor] & 1))
                continue;
            const int difference = (grid_.heights[neighbor] & 0x7fff) - height;
            const double slope = std::min(difference * difference * 0.0004000000119209291, 16.0);
            const float cost =
                static_cast<float>((direction & 1 ? 1.0 : diagonal) * speed + nodes_[selected].cost + slope);
            if (relax(neighbor, cost, cell) == -1)
                return result_;
        }
    }
    result_.cost = finish(selected, from, to, route);
    return result_;
}
int Navigation::step(NavigationPath& path, Vec3 from, Vec3 to, Vec3& waypoint, int budget) {
    if (path.owner_ && path.owner_ != this)
        throw Error("Cached path belongs to another navigation service");
    int status = 1;
    bool reuse =
        path.head_ != -1 && routes_[path.head_].next != -1 && !(path.avoidance >= 60 && path.elapsed > 1);
    if (reuse) {
        const double dx = static_cast<double>(to.x) - path.target.x,
                     dy = static_cast<double>(to.y) - path.target.y;
        const double moved = dx * dx + dy * dy;
        reuse = !(moved > 400);
        if (reuse && path.elapsed > 1) {
            const double fx = static_cast<double>(from.x) - path.target.x,
                         fy = static_cast<double>(from.y) - path.target.y;
            reuse = !(moved > (fx * fx + fy * fy) / (static_cast<double>(path.elapsed) * path.elapsed));
            if (reuse) {
                const auto& a = routes_[path.head_].value;
                const auto& b = routes_[routes_[path.head_].next].value;
                reuse = clearance(a.cell) == a.clearance && clearance(b.cell) == b.clearance &&
                        costs()[a.cell] <= a.maximum_cost && costs()[b.cell] <= b.maximum_cost;
            }
        }
    }
    if (!reuse) {
        clear(path);
        if (search(from, to, &path, budget).cost == 0)
            return 0;
        path.target = to;
        path.avoidance = path.elapsed = 0;
        status = 3;
    }
    while (path.head_ != -1) {
        const int next = routes_[path.head_].next;
        if (next == -1)
            break;
        const auto p = routes_[next].value.position;
        const double dx = static_cast<double>(from.x) - p.x, dy = static_cast<double>(from.y) - p.y;
        const double squared = dx * dx + dy * dy;
        if ((squared > 16 && routes_[next].next != -1) || squared > 1) {
            waypoint.x = p.x;
            waypoint.y = p.y;
            return status;
        }
        pop_route(path);
        if (status == 1)
            status = 2;
    }
    return grid_.connected(from, to) ? 0 : -1;
}
bool Navigation::near_goal(const NavigationPath& path) const {
    if (path.owner_ && path.owner_ != this)
        throw Error("Cached path belongs to another navigation service");
    if (path.head_ != -1 && routes_[path.head_].next != -1 && routes_[routes_[path.head_].next].next != -1) {
        const auto point = routes_[path.head_].value.position;
        const double dx = static_cast<double>(path.target.x) - point.x,
                     dy = static_cast<double>(path.target.y) - point.y;
        if (dx * dx + dy * dy >= 100)
            return false;
    }
    return true;
}
} // namespace sacrifice
