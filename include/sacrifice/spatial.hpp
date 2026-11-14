#pragma once
#include "motion.hpp"
#include <map>

namespace sacrifice {
// Original shared spatial host: 20-unit cells, 128x128, five activation-group head lists.
class SpatialWorld {
  public:
    static constexpr unsigned candidate_capacity = 64; // 0x468440 allocates 0x100 bytes.
    struct Info {
        std::uint32_t kind_flags{}, status_flags{};
        int phase{};
        std::uintptr_t side{};         // Identity only; zero is the original null side.
        const ObjectMotion* owner{};   // Object +460.
        const ObjectMotion* special{}; // +476: bypass owner/side/alive filters for this target.
        bool alive{true};
    };
    using Provider = std::function<Info()>;
    void bind(ObjectMotion& object, unsigned group, Provider info);
    void unbind(ObjectMotion& object);
    // Common model/spatial sync, before the class movement update. Invalid rectangles leave previous
    // membership intact and set common state flag 0x800000, as 0x468b30 does.
    void sync(ObjectMotion& object, std::uint32_t& state_flags);
    void sync_radius(ObjectMotion& object, std::uint32_t& state_flags, float radius); // 0x468a70.
    std::span<const ObjectMotion* const> query(ObjectMotion& object, float length);
    // 0x46a940: scans this object's existing cells, group first, without query-cache filters/cap.
    const ObjectMotion* melee(const ObjectMotion& object, float length = 1, unsigned mask = 0x15) const;
    // 0x492020: group first, existing member cells only, duplicate visits, callback -1 stops.
    int visit_members(const ObjectMotion& object, unsigned mask,
                      const std::function<int(const ObjectMotion&)>& callback) const;
    // 0x469100: one flattened/clamped point cell, group/kind masks and two excluded objects.
    bool occupied(Vec3 point, unsigned mask, const ObjectMotion* ignore_a = nullptr,
                  const ObjectMotion* ignore_b = nullptr) const;
    int visit_ray(Vec3 from, Vec3 to, unsigned mask,
                  const std::function<int(const ObjectMotion&)>& callback) const; // 0x4918d0/0x491b60.
    float collision_radius(const ObjectMotion& object) const;
    const ObjectMotion* collide(ObjectMotion& object, Vec3& delta, float* fraction,
                                const ObjectMotion* ignore = nullptr);
    std::size_t memberships(const ObjectMotion& object) const;
    std::uint64_t query_revision() const {
        return query_revision_;
    }

  private:
    struct Entry {
        unsigned group{};
        Provider info;
        std::vector<unsigned> cells;
        std::int32_t first{-1}, last{-1}; // Cached flattened rectangle, even after an invalid update.
        float radius{};
    };
    using Cell = std::array<std::vector<ObjectMotion*>, 5>;
    std::map<ObjectMotion*, Entry> entries_;
    const Entry& entry(const ObjectMotion* motion) const; // Throws naming a motion missing from the index.
    std::map<unsigned, Cell> cells_;
    std::vector<const ObjectMotion*> candidates_;
    ObjectMotion* cached_object_{};
    int cached_phase_{};
    float cached_length_{};
    std::uint64_t query_revision_{};
    void remove_cell(ObjectMotion& object, unsigned group, unsigned cell);
    void sync_rectangle(ObjectMotion& object, std::uint32_t& state, int x0, int y0, int x1, int y1);
};
} // namespace sacrifice
