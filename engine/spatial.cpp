#include "sacrifice/spatial.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace sacrifice {
namespace {
constexpr std::array<std::uint32_t, 5> group_masks{4, 1, 16, 130, 96};
// _ftol returns a 64-bit truncated integer; callers consume its low 32 bits. Indefinite integer is
// INT64_MIN, whose low word is zero. Keep invalid SAXS bone coordinates out of C++ conversion UB.
std::int32_t integer(double value) {
    if (!std::isfinite(value) || value >= 9223372036854775808.0 || value < -9223372036854775808.0)
        return 0;
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::int64_t>(value)));
}
int lower(double value) {
    return std::max(0, integer(value * 0.05));
}
int upper(double value) {
    const auto cell = std::uint32_t{1} - std::bit_cast<std::uint32_t>(integer(value * -0.05));
    return std::min(128, std::bit_cast<std::int32_t>(cell));
}
std::int32_t flattened(int x, int y) {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(x) + static_cast<std::uint32_t>(y) * 128);
}
float radius(std::span<const Box3> boxes) {
    double x = 0, y = 0, z = 0;
    for (const auto& b : boxes) {
        x = std::max(x, std::max(static_cast<double>(b.minimum.x) * b.minimum.x,
                                 static_cast<double>(b.maximum.x) * b.maximum.x));
        y = std::max(y, std::max(static_cast<double>(b.minimum.y) * b.minimum.y,
                                 static_cast<double>(b.maximum.y) * b.maximum.y));
        z = std::max(z, std::max(static_cast<double>(b.minimum.z) * b.minimum.z,
                                 static_cast<double>(b.maximum.z) * b.maximum.z));
    }
    return static_cast<float>(std::sqrt(x + y + z));
}
} // namespace
const SpatialWorld::Entry& SpatialWorld::entry(const ObjectMotion* motion) const {
    const auto found = entries_.find(const_cast<ObjectMotion*>(motion));
    if (found == entries_.end())
        throw Error("Spatial index has a cell entry for an unbound motion");
    return found->second;
}
void SpatialWorld::bind(ObjectMotion& object, unsigned group, Provider info) {
    if (group >= group_masks.size() || !info || entries_.contains(&object))
        throw Error("Invalid spatial object binding");
    Entry entry;
    entry.group = group;
    entry.info = std::move(info);
    entries_.emplace(&object, std::move(entry));
}
void SpatialWorld::remove_cell(ObjectMotion& object, unsigned group, unsigned cell) {
    const auto found = cells_.find(cell);
    if (found == cells_.end())
        throw Error("Missing spatial membership cell");
    auto& list = found->second[group];
    const auto member = std::ranges::find(list, &object);
    if (member == list.end())
        throw Error("Missing spatial membership object");
    list.erase(member);
    if (std::ranges::all_of(found->second, [](const auto& items) { return items.empty(); }))
        cells_.erase(found);
}
void SpatialWorld::unbind(ObjectMotion& object) {
    const auto found = entries_.find(&object);
    if (found == entries_.end())
        return;
    for (const auto cell : found->second.cells)
        remove_cell(object, found->second.group, cell);
    entries_.erase(found);
    // A native pooled object stays allocated; this host also permits owners to destroy bindings.
    // Drop only a cache that could otherwise contain a dangling motion pointer.
    if (cached_object_ == &object || std::ranges::find(candidates_, &object) != candidates_.end()) {
        cached_object_ = nullptr;
        candidates_.clear();
    }
}
void SpatialWorld::sync(ObjectMotion& object, std::uint32_t& state) {
    const auto found = entries_.find(&object);
    if (found == entries_.end())
        throw Error("Synchronizing an unbound spatial object");
    auto& entry = found->second;
    entry.radius = radius(object.collision_boxes);
    if (object.collision_boxes.empty()) {
        for (const auto cell : entry.cells)
            remove_cell(object, entry.group, cell);
        entry.cells.clear();
        entry.first = entry.last = -1;
        return;
    }
    auto bounds = object.collision_boxes.front();
    for (std::size_t b = 1; b < object.collision_boxes.size(); ++b) {
        const auto& box = object.collision_boxes[b];
        if (box.minimum.x < bounds.minimum.x)
            bounds.minimum.x = box.minimum.x;
        if (box.minimum.y < bounds.minimum.y)
            bounds.minimum.y = box.minimum.y;
        if (box.maximum.x > bounds.maximum.x)
            bounds.maximum.x = box.maximum.x;
        if (box.maximum.y > bounds.maximum.y)
            bounds.maximum.y = box.maximum.y;
    }
    const int x0 = lower(static_cast<double>(bounds.minimum.x) + object.position.x),
              y0 = lower(static_cast<double>(bounds.minimum.y) + object.position.y),
              x1 = upper(static_cast<double>(bounds.maximum.x) + object.position.x),
              y1 = upper(static_cast<double>(bounds.maximum.y) + object.position.y);
    sync_rectangle(object, state, x0, y0, x1, y1);
}
void SpatialWorld::sync_radius(ObjectMotion& object, std::uint32_t& state, float distance) {
    sync_rectangle(object, state, lower(static_cast<double>(object.position.x) - distance),
                   lower(static_cast<double>(object.position.y) - distance),
                   upper(static_cast<double>(object.position.x) + distance),
                   upper(static_cast<double>(object.position.y) + distance));
}
void SpatialWorld::sync_rectangle(ObjectMotion& object, std::uint32_t& state, int x0, int y0, int x1,
                                  int y1) {
    const auto found = entries_.find(&object);
    if (found == entries_.end())
        throw Error("Updating an unbound spatial rectangle");
    auto& entry = found->second;
    const auto first = flattened(x0, y0), last = flattened(x1, y1);
    if (entry.first == first && entry.last == last)
        return;
    entry.first = first;
    entry.last = last;
    if (x0 >= x1 || y0 >= y1 || static_cast<std::int64_t>(x1 - x0) * (y1 - y0) > 64) {
        state |= 0x800000;
        return;
    }
    state &= ~0x800000u;
    std::vector<unsigned> cells;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            cells.push_back(static_cast<unsigned>(x + y * 128));
    for (const auto old : entry.cells)
        if (!std::ranges::binary_search(cells, old))
            remove_cell(object, entry.group, old);
    for (const auto cell : cells)
        if (!std::ranges::binary_search(entry.cells, cell)) {
            auto& list = cells_[cell][entry.group];
            list.insert(list.begin(), &object);
        }
    entry.cells = std::move(cells);
}
bool SpatialWorld::occupied(Vec3 point, unsigned mask, const ObjectMotion* ignore_a,
                            const ObjectMotion* ignore_b) const {
    const int x = std::clamp(integer(point.x * 0.05), 0, 128),
              y = std::clamp(integer(point.y * 0.05), 0, 128);
    const auto found = cells_.find(static_cast<unsigned>(x + y * 128));
    if (found == cells_.end())
        return false;
    for (unsigned group = 0; group < group_masks.size(); ++group)
        if (mask & group_masks[group])
            for (const auto* object : found->second[group])
                if (object != ignore_a && object != ignore_b &&
                    (entry(object).info().kind_flags & mask) &&
                    motion_contains_point(*object, point))
                    return true;
    return false;
}
std::size_t SpatialWorld::memberships(const ObjectMotion& object) const {
    const auto found = entries_.find(const_cast<ObjectMotion*>(&object));
    return found == entries_.end() ? 0 : found->second.cells.size();
}
float SpatialWorld::collision_radius(const ObjectMotion& object) const {
    const auto found = entries_.find(const_cast<ObjectMotion*>(&object));
    return found == entries_.end() ? radius(object.collision_boxes) : found->second.radius;
}
int SpatialWorld::visit_ray(Vec3 from, Vec3 to, unsigned mask,
                            const std::function<int(const ObjectMotion&)>& callback) const {
    const Vec3 delta{to.x - from.x, to.y - from.y, to.z - from.z};
    const auto cell = [](float p) { return std::clamp(integer(p * 0.05), 0, 128); };
    int at = cell(from.y) * 128 + cell(from.x), end = cell(to.y) * 128 + cell(to.x);
    const int x = at % 128, y = (at / 128) % 128;
    std::array<int, 2> remaining{std::abs(x - end % 128), std::abs(at / 128 - end / 128)},
        step{delta.x < 0   ? -1
             : delta.x > 0 ? 1
                           : 0,
             delta.y < 0   ? -128
             : delta.y > 0 ? 128
                           : 0};
    std::array<float, 2> increment{
        delta.x == 0 ? 1.0e14f : static_cast<float>(20.0 / std::abs(static_cast<double>(delta.x))),
        delta.y == 0 ? 1.0e14f : static_cast<float>(20.0 / std::abs(static_cast<double>(delta.y)))};
    std::array<float, 2> next{
        delta.x == 0 ? 1.0e14f : static_cast<float>(((x + (delta.x > 0)) * 20.0 - from.x) / delta.x),
        delta.y == 0 ? 1.0e14f : static_cast<float>(((y + (delta.y > 0)) * 20.0 - from.y) / delta.y)};
    while (true) {
        const auto found = cells_.find(static_cast<unsigned>(at));
        if (found != cells_.end())
            for (unsigned group = 0; group < group_masks.size(); ++group)
                if (mask & group_masks[group])
                    for (const auto* object : found->second[group])
                        if ((entry(object).info().kind_flags & mask) &&
                            callback(*object) == -1)
                            return -1;
        if (at == end)
            return 0;
        const unsigned axis = next[0] >= next[1];
        if (remaining[axis]-- <= 0)
            return 0;
        at += step[axis];
        next[!axis] = static_cast<float>(static_cast<double>(next[!axis]) - next[axis]);
        next[axis] = increment[axis];
    }
}
std::span<const ObjectMotion* const> SpatialWorld::query(ObjectMotion& object, float length) {
    const auto found = entries_.find(&object);
    if (found == entries_.end())
        throw Error("Querying an unbound spatial object");
    const auto self = found->second.info();
    if (cached_object_ == &object && cached_phase_ == self.phase && !(length > cached_length_))
        return candidates_;
    cached_object_ = &object;
    cached_phase_ = self.phase;
    cached_length_ = length;
    ++query_revision_;
    candidates_.clear();
    if (found->second.cells.empty())
        return candidates_;
    const auto mask = object.collision_mask;
    const auto kinds = mask & 0xffff;
    const float range = static_cast<float>(length * 1.2);
    const int x0 = lower(static_cast<double>(object.position.x) - range),
              y0 = lower(static_cast<double>(object.position.y) - range),
              x1 = upper(static_cast<double>(object.position.x) + range),
              y1 = upper(static_cast<double>(object.position.y) + range);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const auto cell = cells_.find(static_cast<unsigned>(x + y * 128));
            if (cell == cells_.end())
                continue;
            for (unsigned group = 0; group < group_masks.size(); ++group) {
                if (!(kinds & group_masks[group]))
                    continue;
                for (const auto* candidate : cell->second[group]) {
                    if (candidate == &object)
                        continue;
                    const auto& other = entry(candidate);
                    const auto info = other.info();
                    if (!(info.kind_flags & kinds) || (candidate->flags & 0x2000000) ||
                        (info.status_flags & 0x80))
                        continue;
                    if (candidate != self.special) {
                        if ((mask & 0x10000) && candidate == self.owner)
                            continue;
                        if (mask & 0x20000) {
                            if (info.side == self.side)
                                continue;
                        } else if ((mask & 0x40000) && info.side != self.side)
                            continue;
                        if ((mask & 0x100000) && !info.alive)
                            continue;
                    }
                    const double radius_sum =
                                     static_cast<double>(found->second.radius) + other.radius + range,
                                 dx = static_cast<double>(candidate->position.x) - object.position.x,
                                 dy = static_cast<double>(candidate->position.y) - object.position.y,
                                 dz = static_cast<double>(candidate->position.z) - object.position.z;
                    if (!(dx * dx + dy * dy + dz * dz <= radius_sum * radius_sum))
                        continue;
                    candidates_.push_back(candidate);
                    if (candidates_.size() == candidate_capacity)
                        return candidates_;
                }
            }
        }
    return candidates_;
}
int SpatialWorld::visit_members(const ObjectMotion& object, unsigned mask,
                                const std::function<int(const ObjectMotion&)>& callback) const {
    const auto found = entries_.find(const_cast<ObjectMotion*>(&object));
    if (found == entries_.end())
        throw Error("Member scan requires a bound spatial object");
    for (unsigned group = 0; group < group_masks.size(); ++group) {
        if (!(group_masks[group] & mask))
            continue;
        for (const auto cell : found->second.cells)
            for (const auto* candidate : cells_.at(cell)[group])
                if ((entry(candidate).info().kind_flags & mask) &&
                    callback(*candidate) == -1)
                    return -1;
    }
    return 0;
}
const ObjectMotion* SpatialWorld::melee(const ObjectMotion& object, float length, unsigned mask) const {
    const auto found = entries_.find(const_cast<ObjectMotion*>(&object));
    if (found == entries_.end())
        throw Error("Melee query requires a bound spatial object");
    if (object.collision_boxes.empty())
        return nullptr;
    const auto& first = object.collision_boxes.front();
    const double reach = static_cast<double>(length) +
                         (object.orientation[3] < 0 ? -first.minimum.x : first.maximum.x) +
                         (object.orientation[4] < 0 ? -first.minimum.y : first.maximum.y);
    const double dx = static_cast<double>(first.minimum.x) - first.maximum.x,
                 dy = static_cast<double>(first.minimum.y) - first.maximum.y;
    const float width = static_cast<float>(std::sqrt(dx * dx + dy * dy) * 0.25);
    const float bottom = object.box_min.z + object.position.z, top = object.box_max.z + object.position.z;
    const Vec3 from{object.position.x, object.position.y,
                    static_cast<float>((static_cast<double>(top) + bottom) * 0.5)};
    Vec3 delta{static_cast<float>(reach * object.orientation[3]),
               static_cast<float>(reach * object.orientation[4]), 0};
    const ObjectMotion* result = nullptr;
    float nearest = 1;
    for (unsigned group = 0; group < group_masks.size(); ++group) {
        if (!(group_masks[group] & mask))
            continue;
        for (const auto cell : found->second.cells) {
            const auto& list = cells_.at(cell)[group];
            for (const auto* candidate : list) {
                if (candidate == &object ||
                    !(entry(candidate).info().kind_flags & mask))
                    continue;
                for (const auto& box : candidate->collision_boxes) {
                    const float low = box.minimum.z + candidate->position.z,
                                high = box.maximum.z + candidate->position.z;
                    if (!(bottom <= high && top >= low))
                        continue;
                    delta.z = static_cast<float>((static_cast<double>(low) + high) * 0.5 - from.z);
                    const Vec3 relative{from.x - candidate->position.x, from.y - candidate->position.y,
                                        from.z - candidate->position.z};
                    const double time = motion_segment_contact(relative, delta, box, width);
                    if (time >= 0 && time <= nearest) {
                        nearest = static_cast<float>(time);
                        result = candidate;
                    }
                }
            }
        }
    }
    return result;
}
const ObjectMotion* SpatialWorld::collide(ObjectMotion& object, Vec3& delta, float* fraction,
                                          const ObjectMotion* ignore) {
    const float length = static_cast<float>(std::sqrt(static_cast<double>(delta.x) * delta.x +
                                                      static_cast<double>(delta.y) * delta.y +
                                                      static_cast<double>(delta.z) * delta.z));
    return motion_collide(object, delta, fraction, ignore, query(object, length));
}
} // namespace sacrifice
