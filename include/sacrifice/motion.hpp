#pragma once
#include "assets.hpp"
#include <functional>

namespace sacrifice {
// LAND::GetPlane keeps the plane even for a triangle touching a hole. Its height remains useful to
// GetTrueHeight; GetHeight/GetHeightAndPlane return zero on that triangle.
struct TerrainPlane {
    Vec3 normal{0, 0, 1};
    float distance{};
    bool solid{};
    double height(float x, float y) const;
};
// False outside GetPlane's truncated cell range; leaves `plane` unchanged in that case.
bool terrain_plane(const Terrain& terrain, float x, float y, TerrainPlane& plane);
double terrain_surface(const Terrain& terrain, float x, float y, TerrainPlane* plane = nullptr);
double terrain_true_height(const Terrain& terrain, float x, float y);
float terrain_intercept(const Terrain& terrain, Vec3 from, Vec3 delta, Vec3& point, float maximum_squared = 0,
                        bool solid_only = true); // LAND::GetIntercept 0x16041080.
bool terrain_edge_slide(const Terrain& terrain, Vec3 from, Vec3 to, Vec3& result);

// Row-vector rotation in the original Z-up world (0x410bc0/0x410c10).
using Orientation = std::array<float, 9>;
Orientation motion_orientation(float yaw, float pitch = 0, float roll = 0);
Orientation motion_yaw_pitch(float yaw, float pitch); // 0x410ca0, separate native float-store timing.
float motion_wrap(double angle);
void motion_approach(float& value, float target, float increase, float decrease);
void motion_approach_angle(float& value, float target, float step);
void motion_damp(Vec3& velocity, float amount, int ticks); // 0x469700, constant factor per call.
// MODEL::GetWorldBBox uses eight corners; collision mode 1 uses six center-axis endpoints instead.
Box3 motion_box(const Box3& box, const Orientation& orientation, bool axial = false);
// Point/expanded-box contact (0x401dd0): -1 no hit, [0,1] contact, 2 escaping an existing overlap.
double motion_box_contact(Vec3 from, Vec3 delta, const Box3& box, Vec3* hit, Vec3* slide);
// 0x4024c0 / 0x401d00: finite segment with a scalar expansion, used by the melee sweep.
double motion_segment_contact(Vec3 from, Vec3 delta, const Box3& box, float radius);
double motion_box_separation(Vec3 position, const Box3& box, Vec3 other_position,
                             const Box3& other); // 0x4027c0, squared gap.

// Common object motion, 0x469760. Positions/velocities use world X,Y,Z, in units per game tick.
// The spatial host supplies ordered candidates to the native contact/dispatch routines below.
struct ObjectMotion {
    Vec3 position, velocity, local_velocity, angular_velocity;
    float yaw{}, pitch{}, roll{};
    Orientation orientation{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::uint32_t flags{}, collision_mask{};
    TerrainPlane plane;
    float ground_height{};
    int airborne_ticks{};
    Vec3 displacement;
    Vec3 box_min, box_max;
    std::vector<Box3> collision_boxes; // Object +624: orientation-transformed, position-relative boxes.
    // 0x44e020: mean of the model reference box centers, relative to object position. The model host
    // supplies this; a point object has a zero offset.
    Vec3 reference_center;
    const ObjectMotion* collided{};
    struct Hooks {
        // Rewrites delta; fraction may be null on the second call, which ignores the first hit.
        std::function<const ObjectMotion*(ObjectMotion&, Vec3& delta, float* fraction,
                                          const ObjectMotion* ignore)>
            collide;
        std::function<void(int)> death; // Vtable +204: boundary 1, falling below a hole 0.
    };
    void advance(const Terrain& terrain, std::uint32_t& state_flags, int ticks, const Hooks& hooks = {});
    void push_from(Vec3 point, float amount, unsigned axes);          // 0x44dc00.
    void push_direction(Vec3 direction, float amount, unsigned axes); // 0x44dd30.
    void creature_flags(std::uint32_t fsm_flags, std::uint32_t properties, std::uint32_t& state_flags);
    double contact(const ObjectMotion& other, Vec3 delta, Vec3& hit, Vec3* slide) const; // 0x469ff0.
};
bool motion_contains_point(const ObjectMotion& object, Vec3 point); // 0x469040, strict box faces, Z -1/+2.
// 0x469e20 dispatch after the spatial query: each candidate sees the previously shortened/slid delta.
const ObjectMotion* motion_collide(ObjectMotion& object, Vec3& delta, float* fraction,
                                   const ObjectMotion* ignore,
                                   std::span<const ObjectMotion* const> candidates);
} // namespace sacrifice
