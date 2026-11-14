#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/soul_actor.hpp"
#include "sacrifice/spatial.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include <cmath>

namespace sacrifice {
namespace {
FourCC cc(std::string_view value) {
    return FourCC::from_text(value);
}
int wrap_add(int a, int b) {
    return std::bit_cast<int>(static_cast<unsigned>(a) + static_cast<unsigned>(b));
}
std::pair<SoulActor*, CreatureActor*> resolve_target(Match& match, MatchObject* target) {
    if (auto* soul = dynamic_cast<SoulActor*>(target))
        return {soul, dynamic_cast<CreatureActor*>(soul->anchor)};
    auto* creature = dynamic_cast<CreatureActor*>(target);
    SoulActor* soul = nullptr;
    if (creature && (creature->kind_flags & 0xd))
        match.visit_group(3, 2, [&](MatchObject& object) {
            auto* candidate = dynamic_cast<SoulActor*>(&object);
            if (candidate && candidate->anchor == creature) {
                soul = candidate;
                return -1;
            }
            return 0;
        });
    return {soul, creature};
}
float radius(Match& match, const ObjectMotion& motion) {
    return match.spatial ? match.spatial->collision_radius(motion) : 0;
}
} // namespace
void CreatureActor::refresh_cast_requirements(Match& match) {
    casting.requirements = {};
    if (!allegiance)
        return;
    std::array<float, 3> distances{5625, 25000000, 2500};
    match.visit_group(2, 16, [&](MatchObject& object) {
        auto* building = dynamic_cast<BuildingActor*>(&object);
        if (!building || building->reveal != 1 || (building->primary && building->primary != building) ||
            (!(building->building_flags & 2) &&
             (building->primary || (building->building_flags & 0x55a0) != 0x55a0)))
            return 0;
        const auto point = building->motion.position;
        if (match.navigation && !match.navigation->grid().connected(motion.position, point))
            return 0;
        const double x = static_cast<double>(motion.position.x) - point.x,
                     y = static_cast<double>(motion.position.y) - point.y,
                     z = static_cast<double>(motion.position.z) - point.z;
        const double squared = z * z + y * y + x * x;
        const float value = static_cast<float>(squared);
        if (building->allegiance == allegiance) {
            if ((building->building_flags & 2) && value < static_cast<double>(distances[1])) {
                distances[1] = static_cast<float>(squared);
                casting.requirements[1] = building;
            }
            if (value < static_cast<double>(distances[2])) {
                distances[2] = static_cast<float>(squared);
                casting.requirements[2] = building;
            }
        } else if ((building->building_flags & 1) && value < static_cast<double>(distances[0]) &&
                   enemy(building)) {
            distances[0] = static_cast<float>(squared);
            casting.requirements[0] = building;
        }
        return 0;
    });
}
bool CreatureActor::doctor_valid() const {
    if (!effect_target)
        return false;
    if (!network_identity)
        return true;
    if (!doctor_altar || !controller)
        return false;
    if (doctor_action == cc("sacu")) {
        if (!(effect_target->kind_flags & 4) || effect_target->allegiance != controller->allegiance)
            return false;
    } else if (auto* soul = dynamic_cast<const SoulActor*>(effect_target); soul && controller->allegiance) {
        const auto* wizard = dynamic_cast<const CreatureActor*>(controller);
        if (wizard && wizard->alive() &&
            ((wizard->common_status & 0x40) || (soul->eligible_sides & controller->allegiance->identity)))
            return false;
    }
    return !(kind_flags & 4) || effect_target->named(doctor_action) == this;
}
void CreatureActor::initialize_doctor(MatchObject* target, BuildingActor* altar, FourCC action,
                                      float height) {
    common_status |= 1;
    fsm_->event(std::bit_cast<int>(cc("BRTH").value));
    state_flags |= 0x1800;
    effect_target = target;
    doctor_altar = altar;
    fade = 1;
    fade_target = 0;
    fade_step = std::bit_cast<float>(1015580809u);
    motion.position.z = height;
    doctor_action = action;
    doctor_permission = 0; // Offline dword_4dbb0d is zero.
    if (target && target->source_motion()) {
        const auto p = target->source_motion()->position;
        motion.yaw = static_cast<float>(std::atan2(static_cast<double>(motion.position.x) - p.x,
                                                   -(static_cast<double>(motion.position.y) - p.y)));
        motion.orientation = motion_orientation(motion.yaw);
        const auto [soul, creature] = resolve_target(*match_, target);
        if (soul)
            soul->name_reference(action, this);
        if (creature)
            creature->name_reference(action, this);
    }
    motion.velocity = {static_cast<float>(motion.orientation[3] * (1.0 / 6.0)),
                       static_cast<float>(motion.orientation[4] * (1.0 / 6.0)),
                       static_cast<float>(motion.orientation[8] * (1.0 / 24.0))};
}
int CreatureActor::doctor_handler(std::string_view name, int ticks) {
    const auto abort = [&] { return fsm_->event(std::bit_cast<int>(cc("abrt").value)); };
    if (name == "CreatureRevive4SacDoc") {
        kind_flags = (kind_flags & ~0xcu) | 4;
        const auto health = static_cast<std::int16_t>(std::max(400, stats.base[5] / 2));
        stats.current[5] = stats.target[5] = health;
        stats.changed |= 0x20;
        model_flags &= ~4u;
        return 0;
    }
    if (name == "CreatureFloatNearSacDoc") {
        auto* doctor = dynamic_cast<CreatureActor*>(effect_target);
        if (!doctor)
            return abort();
        Vec3 point;
        doctor->write_reference_point(point, 2, false);
        spell_approach_velocity(motion, point, static_cast<float>(ticks * .01), .2f, 7, 4);
        const double dx = static_cast<double>(motion.position.x) - point.x,
                     dy = static_cast<double>(motion.position.y) - point.y,
                     dz = static_cast<double>(motion.position.z) - point.z;
        const double length = std::sqrt(dz * dz + dx * dx + dy * dy);
        if (length > 5)
            motion.position = {static_cast<float>(dx * (5 / length) + point.x),
                               static_cast<float>(dy * (5 / length) + point.y),
                               static_cast<float>(dz * (5 / length) + point.z)};
        return 0;
    }
    if (name == "SACDOC_Abort") {
        auto* target = ritual_abort_target ? ritual_abort_target
                       : ritual_abort_owner_enabled
                           ? dynamic_cast<CreatureActor*>(effect_target ? effect_target : ritual_abort_owner)
                           : nullptr;
        if (target && (target->kind_flags & 0xd))
            target->fsm().event(std::bit_cast<int>(cc("abrt").value));
        ritual_abort_target = nullptr;
        return 0;
    }
    if (name == "SACDOC_Exit") {
        ritual_abort_target = ritual_abort_owner = nullptr;
        effect_target = nullptr;
        fade_target = 1;
        fade_step = std::bit_cast<float>(1015580809u);
        if (fade == 1)
            state_flags |= removed_flag;
        return 0;
    }
    if (name == "SACDOC_EndProd") {
        ritual_abort_target = nullptr;
        return 0;
    }
    if (name == "SACDOC_StartProd") {
        SpellActor::launch_doctor_beam(*match_, *this);
        return 0;
    }
    if (name == "SACDOC_MoveTowardsCorpse") {
        const auto [soul, target] = resolve_target(*match_, effect_target);
        (void)soul;
        if (!doctor_valid() || !target)
            return abort();
        effect_target = target;
        if (!(fsm_->state()->flags & 1))
            return 0;
        Vec3 point;
        spell_target_point(point, target);
        if (match_->terrain)
            point.z = static_cast<float>(terrain_surface(*match_->terrain, point.x, point.y));
        const float sum = static_cast<float>(radius(*match_, motion) + radius(*match_, target->motion));
        const bool alive = target->alive();
        const float distance = alive ? static_cast<float>(sum + 5.0) : static_cast<float>(sum * .25 + 1.0);
        const bool touching = alive && melee_target(distance, 5) == target;
        if ((touching || approach_order(*match_, point, distance)) && face_order_point(point) < .1)
            return fsm_->event(std::bit_cast<int>(cc(doctor_action == cc("sacc") ? "fspr" : "lift").value));
        return 0;
    }
    if (name == "SACDOC_ForceSpiritInit") {
        if (!doctor_valid())
            return abort();
        const auto [soul, target] = resolve_target(*match_, effect_target);
        if (!soul || !target)
            return abort();
        if (!ritual_abort_owner_enabled) {
            if (soul->reconstitute(*match_, *target)) {
                ritual_abort_owner_enabled = true;
                doctor_spirit_ticks =
                    std::bit_cast<int>(600u * (static_cast<unsigned>(soul->count) + 1u)) / 2;
                fsm_->event(std::bit_cast<int>(cc("fsok").value));
            } else
                fsm_->event(std::bit_cast<int>(cc("fsno").value));
        }
        effect_target = target;
        return 0;
    }
    if (name == "SACDOC_ForceSpiritWait") {
        if (!doctor_valid())
            return abort();
        if (ritual_abort_owner_enabled) {
            doctor_spirit_ticks = wrap_add(doctor_spirit_ticks, -ticks);
            if (doctor_spirit_ticks <= 0)
                return fsm_->event(std::bit_cast<int>(cc("fsdn").value));
        }
        return 0;
    }
    if (name == "SACDOC_StartLiftBeam") {
        if (!doctor_valid())
            return abort();
        const auto [soul, target] = resolve_target(*match_, effect_target);
        (void)soul;
        if (!target)
            return abort();
        if (network_identity) {
            target->effect_target = this;
            const int result = target->fsm().event(42);
            fsm_->event(std::bit_cast<int>(cc(result == 2 ? "__go" : "__no").value));
        }
        return 0;
    }
    if (name == "SACDOC_MakeLiftBeam") {
        const auto [soul, target] = resolve_target(*match_, effect_target);
        (void)soul;
        if (!ritual_abort_target) {
            ritual_abort_target = target;
            effect_target = target;
            SpellActor::launch_doctor_beam(*match_, *this);
            cast_visual_requests.push_back({cc("lftb")});
        }
        return 0;
    }
    if (name == "SACDOC_MoveTowardsAltar") {
        ritual_abort_owner_enabled = false;
        if (!doctor_valid())
            return abort();
        if (fsm_->state()->flags & 1) {
            const auto to = doctor_altar->motion.position;
            const double dx = static_cast<double>(motion.position.x) - to.x,
                         dy = static_cast<double>(motion.position.y) - to.y,
                         dz = static_cast<double>(motion.position.z) - to.z;
            const double length = std::sqrt(dz * dz + dx * dx + dy * dy),
                         factor = length != 0 ? 15 / length : 1;
            const Vec3 point{static_cast<float>(dx * factor + to.x), static_cast<float>(dy * factor + to.y),
                             static_cast<float>(dz * factor + to.z)};
            if (approach_order(*match_, point, 5, 0, 8192) && !doctor_altar->ritual.victim)
                fsm_->event(29);
        }
        return 0;
    }
    if (name == "SACDOC_RequestSacrifice") {
        auto* victim = dynamic_cast<CreatureActor*>(effect_target);
        auto* wizard = dynamic_cast<CreatureActor*>(controller);
        if (victim && doctor_altar)
            doctor_altar->request_sacrifice(doctor_action, victim, this, wizard);
        else
            fsm_->event(45);
        return 0;
    }
    if (name == "SACDOC_AbortSacrifice") {
        if (doctor_altar)
            doctor_altar->request_sacrifice({}, dynamic_cast<CreatureActor*>(effect_target), nullptr,
                                            nullptr);
        return 0;
    }
    if (name == "SACDOC_StartSacrifice") {
        ritual_abort_target = nullptr;
        ritual_abort_owner_enabled = false;
        if (doctor_altar) {
            if (auto* wizard = dynamic_cast<CreatureActor*>(controller))
                SpellActor::launch_altar_process(*match_, *wizard, *doctor_altar);
            if (auto* victim = dynamic_cast<CreatureActor*>(effect_target))
                victim->fsm().event(29);
        }
        return 0;
    }
    throw Error("Unimplemented sacrificial doctor handler: " + std::string(name));
}
int CreatureActor::run_doctor_circle(Match& match, int) {
    motion.flags |= 4;
    auto& circle = doctor_circle;
    if (fsm_->state()->flags & 1) {
        const double x = static_cast<double>(circle.center.x) - motion.position.x,
                     y = static_cast<double>(circle.center.y) - motion.position.y;
        circle.current_angle = static_cast<float>(std::atan2(x, -y));
        circle.distance_squared = static_cast<float>(y * y + x * x);
        double delta = circle.angle - circle.current_angle;
        while (delta > 3.14159)
            delta -= 6.28318;
        while (delta <= -3.14159)
            delta += 6.28318;
        float distance = circle.maximum;
        double angle;
        if (std::abs(delta) >= .2617991666666666)
            angle = circle.current_angle + (delta < 0 ? -.2617991666666666 : .2617991666666666);
        else {
            const float absolute = static_cast<float>(std::abs(delta));
            distance = static_cast<float>(absolute * 3.819721860586519 * (circle.maximum - circle.minimum) +
                                          circle.minimum);
            angle = circle.angle;
        }
        Vec3 point{static_cast<float>(circle.center.x - std::sin(angle) * distance),
                   static_cast<float>(circle.center.y + std::cos(angle) * distance), 0};
        if (match.terrain)
            point.z = static_cast<float>(terrain_surface(*match.terrain, point.x, point.y));
        motion.collision_mask &= ~0x10u; // 0x44a54d: +588, omit buildings while circling the altar.
        circle.arrived = approach_order(match, point, 1) != 0;
    }
    if (!effect_target)
        fsm_->event(std::bit_cast<int>(cc("abrt").value));
    return 1;
}
} // namespace sacrifice
