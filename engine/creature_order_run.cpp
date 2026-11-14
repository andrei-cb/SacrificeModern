#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/spatial.hpp"
#include "sacrifice/spell_combat.hpp"
#include "sacrifice/spell_registry.hpp"
#include <cmath>
#include <utility>

namespace sacrifice {
namespace {
bool valid_target(MatchObject* target, const MatchObject* self) {
    if (!target)
        return false;
    if (target != self && (target->kind_flags & 5)) {
        if (auto* c = dynamic_cast<CreatureActor*>(target);
            c && c->fsm().state() && !(c->fsm().state()->flags & 0x4000))
            return true;
    }
    const auto* b = dynamic_cast<BuildingActor*>(target);
    return (target->kind_flags & 16) && b && (b->building_flags & 0x55b7) && (b->building_flags & 0x80);
}
float radius(Match& match, const ObjectMotion& motion) {
    if (match.spatial)
        return match.spatial->collision_radius(motion);
    double x = 0, y = 0, z = 0;
    for (const auto& b : motion.collision_boxes) {
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
int CreatureActor::terrain_visibility(Match& match, MatchObject* target, std::optional<Vec3> point) const {
    if (auto* actor = dynamic_cast<CreatureActor*>(target);
        actor && actor->fsm().state() && (actor->fsm().state()->flags & 0x10000))
        return 2;
    Vec3 from;
    spell_target_point(from, this);
    Vec3 to;
    if (target)
        spell_target_point(to, target);
    else {
        if (!point)
            return 1;
        to = *point;
        to.z = static_cast<float>(to.z + 1.0);
    }
    const Vec3 delta{to.x - from.x, to.y - from.y, to.z - from.z};
    const float squared =
        static_cast<float>(static_cast<double>(delta.z) * delta.z + static_cast<double>(delta.y) * delta.y +
                           static_cast<double>(delta.x) * delta.x);
    if (!match.terrain)
        throw Error("Order visibility requires terrain");
    Vec3 hit;
    const auto result = terrain_intercept(*match.terrain, from, delta, hit, squared, true);
    return result > 0 && result < 1 ? 2 : 1;
}
int CreatureActor::object_visibility(Match& match, Vec3 point, MatchObject* ignore) const {
    if (!match.spatial)
        throw Error("Order visibility requires spatial services");
    Vec3 from;
    spell_target_point(from, this);
    const Vec3 delta{point.x - from.x, point.y - from.y, point.z - from.z};
    MatchObject* hit = nullptr;
    match.spatial->visit_ray(from, point, 0x15, [&](const ObjectMotion& other) {
        if (&other == &motion || (ignore && &other == ignore->source_motion()))
            return 0;
        const Vec3 relative{from.x - other.position.x, from.y - other.position.y, from.z - other.position.z};
        const double squared = static_cast<double>(relative.z) * relative.z +
                               static_cast<double>(relative.y) * relative.y +
                               static_cast<double>(relative.x) * relative.x;
        const float expansion = squared >= 100 ? 0 : static_cast<float>(squared * 0.001 - 0.1);
        for (const auto& box : other.collision_boxes) {
            const double contact = motion_segment_contact(relative, delta, box, expansion);
            if (contact >= 0 && contact <= 1) {
                for (const auto& object : match.objects())
                    if (object->source_motion() == &other) {
                        hit = object.get();
                        return -1;
                    }
                throw Error("Visibility object has no match host");
            }
        }
        return 0;
    });
    if (!hit)
        return 4;
    if (hit->kind_flags & 16)
        return 8;
    return friendly(hit) ? 16 : 32;
}
bool CreatureActor::issue_spell_order(FourCC id, MatchObject* target, std::optional<Vec3> point) {
    const auto entry = find_spell(id);
    if (!entry)
        return false;
    casting.requested = entry;
    casting.accepted = false;
    casting.error = {};
    if (type == wizard || point) {
        if (!point)
            point = target && target->source_motion() ? target->source_motion()->position : motion.position;
        orders.internal({65, target, point});
    } else
        orders.internal({66, target});
    return true;
}
void CreatureActor::execute_orders(Match& match) {
    const auto flags = fsm_->state() ? fsm_->state()->flags : 0;
    if (!(flags & 1))
        return;
    if (input_provider || direct_controls || (flags & 0x20000000)) {
        orders.clear();
        return;
    }
    const auto order = orders.current();
    auto* target = order.target;
    const bool settled = std::exchange(follow_settled, false);
    switch (order.kind) {
    case 1:
    case 2:
    case 34:
        if (approach_order(match, order.point, .5f, order.yaw, .087266386f))
            orders.pop(true);
        break;
    case 3:
    case 5:
        if (target && target != this && (target->kind_flags & 0x1f) && target->source_motion()) {
            const Vec3 point = order.formation ? order.point : target->source_motion()->position;
            const float arrive = order.formation ? .5f : radius(match, *target->source_motion()) + 10;
            const float face = order.formation ? .087266386f : 0;
            // Port repair: natively an arrived follower re-steers as soon as its point is past `arrive`. A
            // leader creeping slowly (a wizard pushed by the manalith field, 0x44d9a0) then keeps it at
            // that edge, flipping between run and stand every few ticks. Once arrived it holds until the
            // point is a unit further out.
            if (settled && network_identity && !(state_flags & 0x80000)) {
                const double dx = static_cast<double>(point.x) - motion.position.x,
                             dy = static_cast<double>(point.y) - motion.position.y,
                             hold = static_cast<double>(arrive) + 1;
                if (dx * dx + dy * dy < hold * hold) {
                    if (face > 0)
                        face_order_yaw(order.yaw);
                    orders.pop(false);
                    follow_settled = orders.kind() == order.kind && orders.target() == target;
                    break;
                }
            }
            const int result = approach_order(match, point, arrive, order.yaw, face);
            if (result)
                orders.pop(false);
            follow_settled =
                result > 0 && orders.kind() == order.kind && orders.target() == target;
        } else if (order.kind == 5)
            orders.rewrite(5, 4);
        else
            orders.pop(true);
        break;
    case 4:
    case 7:
        if (approach_order(match, order.point, .5f, order.yaw, .087266386f))
            orders.pop(false);
        break;
    case 6: {
        if (!valid_target(target, this) || kind_flags == 1) {
            orders.rewrite(6, 7);
            break;
        }
        const auto& other = *target->source_motion();
        const auto* creature = dynamic_cast<CreatureActor*>(target);
        if ((target->kind_flags & 4) && (status_flags & 0x200) && creature &&
            creature->fsm().state() && (creature->fsm().state()->flags & 2) && !(flags & 2))
            fsm_->event(16);
        const double x = static_cast<double>(motion.position.x) - other.position.x,
                     y = static_cast<double>(motion.position.y) - other.position.y,
                     z = static_cast<double>(motion.position.z) - other.position.z;
        if (radius(match, motion) + radius(match, other) <= std::sqrt(x * x + y * y + z * z) ||
            face_order_point(other.position) >= .1745327777777778 || melee_target(.25f) != target) {
            approach_order(match, other.position, .1f);
            break;
        }
        if (fsm_->state() && (fsm_->state()->flags & 2)) {
            if (ritual_building && ritual_building->source_motion()) {
                const auto p = ritual_building->source_motion()->position;
                const double dx = static_cast<double>(p.x) - other.position.x,
                             dy = static_cast<double>(p.y) - other.position.y,
                             dz = static_cast<double>(p.z) - other.position.z;
                if (dx * dx + dy * dy + dz * dz > 2500)
                    break;
            }
            if (motion.box_min.z + motion.position.z > other.box_max.z + other.position.z - .1) {
                controls[1] = -127;
                break;
            }
            if (motion.box_max.z + motion.position.z < other.box_min.z + other.position.z + .1) {
                controls[1] = 127;
                break;
            }
            controls[1] = 0;
        }
        effect_target = target;
        fsm_->event(4);
        break;
    }
    case 35:
        if (!valid_target(target, this))
            orders.pop(true);
        else {
            // Notification followers such as pe01's opening peasant approach their addressee
            // while the camera turns to them. Preserve that noncombat staging beneath a facing
            // command; the pause marker still blocks ordinary movement and combat orders.
            const auto suspended = orders.suspended_cinematic_order();
            if (match.cinematic && !stats.base[2] && spells.entries.empty() &&
                suspended.kind == 3 && suspended.target == target)
                approach_order(match, target->source_motion()->position,
                               radius(match, *target->source_motion()) + 10);
            face_order_point(target->source_motion()->position);
        }
        break;
    case 36:
        face_order_point(order.point);
        break;
    case 65:
    case 66: {
        const auto* entry = casting.requested ? cast_entry(*casting.requested) : nullptr;
        const auto* definition =
            entry && match.spell_registry ? match.spell_registry->find(entry->id) : nullptr;
        if (!definition) {
            orders.pop(true);
            break;
        }
        if (order.kind == 65) {
            casting.point = order.point;
            effect_target = target;
            if (kind_flags & 1) {
                if (!cast_check(entry->id).code) {
                    const bool running = definition->kind == 4 && definition->spell &&
                                         !(definition->spell->property_flags & 0x2000000);
                    if (fsm_->event(running ? 41 : 12) == 2)
                        orders.pop(true);
                }
                break;
            }
        }
        execute_spell_order(match, *definition,
                            order.kind == 65 ? std::optional<Vec3>(order.point) : std::nullopt, target);
        break;
    }
    default:
        break;
    }
}
int CreatureActor::execute_spell_order(Match& match, const SpellDefinition& definition,
                                       std::optional<Vec3> point, MatchObject* target) {
    const auto properties = definition.kind == 4 && definition.spell ? definition.spell->property_flags : 0;
    if ((!point && !target)) {
        orders.pop(true);
        return -1;
    }
    if (properties & 0x80000) {
        casting.point = point ? *point : target->source_motion()->position;
        effect_target = target;
        start_cast();
        orders.pop(true);
        return 1;
    }
    if (!point && !valid_target(target, this)) {
        orders.pop(true);
        return -1;
    }
    float squared;
    if (!target || target == this) {
        casting.point = *point;
        const auto& p = casting.point;
        const double x = static_cast<double>(motion.position.x) - p.x,
                     y = static_cast<double>(motion.position.y) - p.y,
                     z = static_cast<double>(motion.position.z) - p.z;
        squared = static_cast<float>(z * z + y * y + x * x);
    } else {
        spell_target_point(casting.point, target);
        const auto& other = *target->source_motion();
        squared = static_cast<float>(motion_box_separation(motion.position, {motion.box_min, motion.box_max},
                                                           other.position, {other.box_min, other.box_max}));
    }
    const float range = static_cast<float>(definition.range * .75);
    if (fsm_->state() && (fsm_->state()->flags & 2) && match.terrain &&
        motion.position.z - terrain_surface(*match.terrain, motion.position.x, motion.position.y) < 5)
        controls[1] = 127;
    if (target != this) {
        if (static_cast<double>(range) * range < squared || (terrain_visibility(match, target, point) & 2)) {
            approach_order(match, casting.point, .5f);
            return 0;
        }
        if (face_order_point(casting.point) > .08726638888888888)
            return 0;
        if (object_visibility(match, casting.point, target) & 0x10)
            return 0;
    }
    const auto check = cast_check(definition.id);
    if (check.code != 2) {
        if (check.code)
            return 0;
        effect_target = target;
        casting.active = casting.requested;
        fsm_->event(13);
        if (properties & 0x40000)
            fsm_->event(54);
        if (target && target != this && !(properties & 0x800000))
            return 1;
    }
    orders.pop(true);
    return 1;
}
} // namespace sacrifice
