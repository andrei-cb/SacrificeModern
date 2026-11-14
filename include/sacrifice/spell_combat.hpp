#pragma once
#include "match.hpp"
#include "motion.hpp"
#include "spell_data.hpp"

namespace sacrifice {
// Shared native spell helpers; all positions/velocities remain in Z-up map coordinates.
void spell_target_point(Vec3& point,
                        const MatchObject* target); // 0x44e020, collision centers then visual box.
void spell_lead_target(Vec3& point, const MatchObject* target, float speed, const ObjectMotion& source,
                       const Terrain& terrain); // 0x44e0e0.
float spell_approach_velocity(ObjectMotion& motion, Vec3 point, float acceleration, float speed,
                              unsigned axes = 7, float slowdown_radius = 0);                    // 0x44d620.
double spell_box_distance_squared(Vec3 point, const ObjectMotion& target, float range_squared); // 0x4019c0.
// 0x44d810: groups 4/1/16, splash for damage kinds ending in 2/4, then the direct target in full.
MatchObject* spell_damage(Match& match, MatchObject* source, MatchObject* direct, int type, int amount,
                          float radius, std::optional<Vec3> point = {});
int evaluate_spell_damage(const SpellData& spell, const MatchObject& caster, const MatchObject& target);
void spell_scatter(Vec3& velocity, int accuracy, CrtRandom& random); // 0x44d550, two shared CRT draws.
Orientation spell_orientation(Vec3 velocity); // 0x410d40 / 0x4109a0, native forward/basis construction.
Vec3 spell_ballistic_velocity(Vec3 from, Vec3 to, float speed, int mode = 0); // 0x44d120.
MatchObject* spell_trace_ray(Match& match, const MatchObject& source, Vec3 from, Vec3& endpoint,
                             unsigned mask = 21); // 0x44cd10: nearest object or land intercept.
double spell_box_separation(const ObjectMotion& a, const ObjectMotion& b); // 0x4027c0, first boxes.
// 0x44d9a0: blends creature velocities on `axes` toward `speed` away from
// `center`, fully within `inner` and with a fourth-power falloff to `outer`, in native group order.
// When filter/mode are supplied, mode bit 1 selects enemies; otherwise select friends.
void spell_exclusion_field(Match& match, Vec3 center, float inner, float outer, float speed, unsigned axes,
                           unsigned mask, const MatchObject* filter = nullptr, unsigned mode = 0);
} // namespace sacrifice
