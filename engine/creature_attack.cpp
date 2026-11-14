#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/fog.hpp"
#include "sacrifice/navigation.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/spatial.hpp"
#include "sacrifice/spell_combat.hpp"
#include "sacrifice/spell_registry.hpp"
#include <cmath>

namespace sacrifice {
namespace {
// Flee candidates at 0x4d26ac: four 25-unit then four 50-unit axis offsets.
constexpr std::array<std::array<float, 2>, 8> flee_offsets{
    {{0, 25}, {25, 0}, {0, -25}, {-25, 0}, {0, 50}, {50, 0}, {0, -50}, {-50, 0}}};
bool is_hostile(const MatchObject& self, const MatchObject& other) { // Vtable +108.
    return self.enemy(&other);
}
bool is_friendly(const MatchObject& self, const MatchObject& other) { // Vtable +112.
    return self.friendly(&other);
}
const CreatureActor* as_creature(const MatchObject* object) {
    return object && (object->kind_flags & 5) ? dynamic_cast<const CreatureActor*>(object) : nullptr;
}
const BuildingActor* as_building(const MatchObject* object) {
    return object && (object->kind_flags & 0x10) ? dynamic_cast<const BuildingActor*>(object) : nullptr;
}
bool gone(const MatchObject& object) { // Vtable +56: FSM state flag 0x4000.
    const auto* actor = as_creature(&object);
    return actor && actor->fsm().state() && (actor->fsm().state()->flags & 0x4000);
}
bool targetable(const MatchObject& object) { // Vtable +64.
    if (object.state_flags & MatchObject::removed_flag)
        return false;
    if (const auto* actor = as_creature(&object))
        return actor->alive() && !(actor->fsm().state()->flags & 0x10000);
    // Building vtable +64 is 0x481050 (true), including zero-health mana fountains and ruins.
    // Damageable primary parts are filtered separately when choosing an attack.
    return true;
}
int health(const MatchObject& object) { // Vtable +128 (0).
    if (const auto* actor = as_creature(&object))
        return actor->stats.current[CreatureStats::health];
    if (const auto* part = as_building(&object))
        return part->health;
    return 0;
}
// 0x493b70 building filter: primary parts (or secondaries naming themselves) without bit 8 and with 0x80.
bool attackable_building(const MatchObject& object) {
    const auto* part = as_building(&object);
    if (!part)
        return false;
    if (part->primary ? part->primary != part : (part->building_flags & 8) != 0)
        return false;
    return (part->building_flags & 0x80) != 0;
}
Vec3 position(const MatchObject& object) {
    const auto* motion = object.source_motion();
    return motion ? motion->position : Vec3{};
}
double squared(Vec3 a, Vec3 b) {
    const double x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return z * z + y * y + x * x;
}
// 0x48f080 is evaluated at the creature AND at each possible escape point.
float danger_at(const Match& match, const CreatureActor& self, Vec3 point) {
    if (!self.allegiance)
        return 0;
    const auto weigh = [&](Vec3 position) {
        const double distance = std::sqrt(squared(position, point));
        return distance <= 0 ? 1.0 : distance < 100 ? (100 - distance) / 100 : 0.0;
    };
    float enemy = 0, own = 0;
    for (const auto& entry : self.allegiance->known)
        if (const auto* actor = as_creature(entry.object); actor &&
                                                           match.resolve(entry.reference) == entry.object &&
                                                           self.enemy(actor) && targetable(*actor)) {
            const double weight = weigh(actor->motion.position);
            enemy = static_cast<float>((actor->stats.current[0] + actor->stats.current[1]) * weight * weight +
                                       enemy);
        }
    for (const auto& group : self.allegiance->objects)
        for (const auto* object : group)
            if (const auto* actor = as_creature(object);
                actor && self.friendly(actor) && actor->stats.current[2]) {
                const double weight = weigh(actor->motion.position);
                own = static_cast<float>(
                    (actor->stats.current[0] + actor->stats.current[1]) * weight * weight + own);
            }
    float value = enemy * .0005f;
    if (own * .0005 >= 1)
        value = static_cast<float>(value / (own * .0005));
    return self.named(FourCC::from_text("PROT")) ? value * .5f : value;
}
// The 0x493b70 / 0x494c90 rounded ten-unit cell of the shared 256-wide path maps.
std::int64_t path_cell(Vec3 p) {
    return static_cast<std::int64_t>((p.x + 5.0) * 0.1) -
           NavigationGrid::width * static_cast<std::int64_t>((p.y + 5.0) * -0.1);
}
int path_cost(const Match& match, Vec3 p) {
    if (!match.navigation)
        return 0;
    const auto costs = match.navigation->costs();
    const auto cell = path_cell(p);
    return cell >= 0 && cell < static_cast<std::int64_t>(costs.size()) ? costs[static_cast<std::size_t>(cell)] : 0;
}
// The seven SPEL evaluators (0x441fc0..0x4423f0, 0x411b00) on {definition, caster, target}.
double evaluate(const Match& match, const SpellDefinition& definition, const CreatureActor& caster,
                MatchObject& target) {
    const auto& spell = *definition.spell;
    const auto id = spell.evaluator_id;
    const auto* actor = as_creature(&target);
    const auto area = [&](float radius, float query, bool rain) {
        const double r2 = static_cast<double>(radius) * radius;
        const auto centre = position(target);
        int total = 0;
        for (const auto& object : match.objects()) {
            if (!(object->kind_flags & 21) || squared(position(*object), centre) > static_cast<double>(query) * query)
                continue;
            const auto* part = as_building(object.get());
            if (part && !(part->building_flags & 0x80))
                continue;
            const auto p = position(*object);
            const double x = p.x - centre.x, y = p.y - centre.y, z = p.z - centre.z;
            int amount = static_cast<int>((r2 - (z * z + y * y + x * x)) * spell.amount * (1 / r2));
            amount = std::min(amount, health(*object));
            if (is_hostile(caster, *object))
                total += amount;
            else if (rain && (object->kind_flags & 1))
                total -= amount + amount; // 0x442280 reads an unset local; the wizard weight is interim.
            else
                total -= amount; // 0x4420f0's friendly term reads float bits as an integer; interim.
        }
        return total;
    };
    if (id == spell_function_id("EvalSpell_Damage")) {
        if (!(target.kind_flags & 0x15))
            return 0;
        const int amount = std::min(spell.amount, health(target));
        return is_hostile(caster, target) ? amount : -amount;
    }
    if (id == spell_function_id("EvalSpell_Heal")) {
        if (!actor || target.named(FourCC::from_text("HEAL")))
            return 0;
        const int missing = std::min(actor->stats.base[5] - actor->stats.current[5], spell.amount);
        const int value = 3 * missing / 4;
        return is_hostile(caster, target) ? -value : value;
    }
    if (id == spell_function_id("EvalSpell_DamageArea"))
        return area(std::max(spell.spread_limit, spell.radius), std::max(spell.spread_limit, spell.radius), false);
    if (id == spell_function_id("EvalSpell_RainSpellEvaluate"))
        return area(definition.range, spell.spread_limit, true);
    if (id == spell_function_id("EvalSpell_Protect")) {
        if (!actor || !(target.kind_flags & 1) || target.named(FourCC::from_text("PROT")))
            return 0;
        const int missing = actor->stats.base[5] - actor->stats.current[5];
        return is_hostile(caster, target) ? -missing : missing;
    }
    if (id == spell_function_id("EvalSpell_Paralyze")) {
        if (!actor || (actor->fsm().state() && (actor->fsm().state()->flags & 0x20)))
            return 0;
        const int value = 3 * actor->stats.current[5] / 4;
        return is_hostile(caster, target) ? value : -value;
    }
    if (id == spell_function_id("EvaluatePanicSpell")) {
        if (&target != &caster || !actor || !actor->stats.base[5])
            return 0;
        const double ratio = static_cast<double>(actor->stats.current[5]) / actor->stats.base[5];
        if (ratio * ratio < .2)
            return 0;
        if (const auto* attacker = match.resolve(actor->credited_attacker)) {
            const auto a = position(*attacker), p = actor->motion.position;
            const double x = p.x - a.x, y = p.y - a.y;
            if (y * y + x * x > 3600)
                return 0;
        }
        return ratio * ratio <= .4;
    }
    return 0;
}
} // namespace

// 0x495320.
bool CreatureActor::think(Match& match) {
    if (gone(*this) || !fsm_->state() || (fsm_->state()->flags & 0x20000))
        return false;
    float range = static_cast<float>(stats.current[16] * .1);
    const float sight = range;
    const float fov =
        static_cast<float>(std::sqrt(stats.current[17] * .001) * 141.37155 * .005555555555555556);
    if (allegiance && allegiance->controller && ((kind_flags & 1) || ritual_building))
        range = 3620;
    // 0x46b150 / 0x4724e0: examine one side per call, round-robin through the side list.
    if (!(common_status & 0x100) && allegiance && !match.side_order.empty()) {
        auto& order = match.side_order;
        auto it = std::ranges::find(order, scan_side);
        MatchSide* side = it == order.end() ? order.front() : *it;
        const Vec3 eye{motion.position.x, motion.position.y, motion.position.z + motion.box_max.z};
        // 0x4724e0: allied propagation precedes the observer's own sight scan, gated by
        // session bit 0x40. It shares both terrain discovery and known-object sightings.
        const auto observe = [&](MatchSide& recipient) {
            if (recipient.fog && match.terrain)
                recipient.fog->reveal(*match.terrain, eye, motion.yaw, fov, concealment, sight);
            if (side == &recipient || !match.terrain)
                return;
            const double forward_x = -std::sin(motion.yaw), forward_y = std::cos(motion.yaw);
            const double limit = static_cast<double>(range) * range;
            for (auto& group : side->objects)
                for (auto* object : group) {
                    if (!(object->kind_flags & 23) ||
                        ((object->seen_by & recipient.identity) && !(object->hidden_from & recipient.identity)) ||
                        !targetable(*object) || gone(*object))
                        continue;
                    const auto* other = object->source_motion();
                    if (!other)
                        continue;
                    Vec3 delta{other->position.x - eye.x, other->position.y - eye.y,
                               other->position.z + other->box_max.z - eye.z};
                    double planar = static_cast<double>(delta.y) * delta.y + static_cast<double>(delta.x) * delta.x;
                    if (planar >= limit)
                        continue;
                    const double angle =
                        planar > 0 ? std::acos((delta.y * forward_y + delta.x * forward_x) / std::sqrt(planar)) : 0;
                    if (angle > fov) {
                        double reach = fov / angle;
                        if (concealment != 1)
                            reach = (1 - reach) * concealment + 1;
                        if (planar > reach * reach * limit)
                            continue;
                    }
                    if (match.spatial && match.spatial->collision_radius(*other) >= 4) {
                        const unsigned corner = match.random().next() & 3;
                        delta.x = (corner & 1 ? other->box_max.x : other->box_min.x) + other->position.x - eye.x;
                        delta.y = (corner >> 1 ? other->box_max.y : other->box_min.y) + other->position.y - eye.y;
                        planar = static_cast<double>(delta.y) * delta.y + static_cast<double>(delta.x) * delta.x;
                    }
                    Vec3 hit;
                    const float t =
                        terrain_intercept(*match.terrain, eye, delta, hit, static_cast<float>(planar), true);
                    if (t >= 1 || t <= 0)
                        recipient.see(*object);
                }
        };
        if (match.shared_vision)
            for (auto* recipient : order)
                if (recipient != allegiance && recipient != side &&
                    ((recipient->allies & allegiance->identity) || (allegiance->allies & recipient->identity) ||
                     recipient->identity == allegiance->identity))
                    observe(*recipient);
        observe(*allegiance);
        it = std::ranges::find(order, side);
        scan_side = it == order.end() || std::next(it) == order.end() ? nullptr : *std::next(it);
    }
    select_attack(match);
    return true;
}

// 0x493b70: score melee and abilities against the current order's engagement point, and queue a
// temporary order when the best choice beats the order's own weight.
void CreatureActor::select_attack(Match& match) {
    danger = status_flags & 0x8000 ? danger_at(match, *this, motion.position) : 0;
    ground_cost = path_cost(match, motion.position);
    // Vtable +56, network id +772 (always present offline) and the +568 bit 8 scan (0x493ad0) are gates.
    if (gone(*this) || !network_identity)
        return;
    // 0x493ad0 / 0x493ab0: flagged followers adopt a nearby allied creature's side.
    // This is a proximity transfer, distinct from changing diplomatic relations.
    if ((common_status & 8) && match.spatial) {
        bool transferred = false;
        for (unsigned group : {0u, 1u}) {
            match.visit_group(group, 5, [&](MatchObject& candidate) {
                if (!candidate.allegiance || candidate.allegiance == allegiance || !friendly(&candidate) ||
                    (candidate.state_flags & MatchObject::removed_flag) || !candidate.source_motion())
                    return 0;
                const float reach = match.spatial->collision_radius(motion) +
                                    match.spatial->collision_radius(*candidate.source_motion()) + 10;
                if (squared(motion.position, candidate.source_motion()->position) >= reach * reach)
                    return 0;
                candidate.allegiance->attach(*this);
                orders.clear();
                if (match.cinematic)
                    issue_order({CreatureOrders::cinematic_marker, this});
                transferred = true;
                return -1;
            });
            if (transferred)
                return;
        }
    }
    const auto previous = orders.current(false);
    orders.discard_temporary();
    if (orders.transient() || !fsm_->state() || !(fsm_->state()->flags & 1))
        return;
    if (peasant) { // 0x493b70: a behaviour controller (+2936) thinks instead.
        peasant_think(match);
        return;
    }
    if (state_flags & 0x1800) // Ritual doctors and other scripted movers select nothing.
        return;
    const auto current = orders.current(true);
    const int kind = current.kind;
    MatchObject* ordered = current.target;
    if (fsm_->state()->flags & 0x2000000) {
        if (kind != 6 || !ordered)
            return;
    } else if (kind >= 32)
        return;
    if (match.direct_control_target == this && (kind < 4 || kind > 7))
        return; // 0x493b70: the followed character keeps player steering unless explicitly guarding/attacking.
    Vec3 centre = motion.position;
    double threshold = .25, best = 0, weight = 0, range = 100, reach = 0;
    int choice = 0;
    MatchObject* chosen = nullptr;
    Vec3 point{};
    std::optional<BookSlot> slot;
    const double melee = stats.current[2] ? 1 : 0;
    unsigned mask = 21;
    auto capabilities = attack_capabilities;
    MatchObject* attacker = match.resolve(credited_attacker);
    const auto* registry = match.spell_registry.get();
    // The shared ability loop of 0x493b70 / 0x494d60.
    const auto abilities = [&](MatchObject& target, double bias, double factor, bool commanded) {
        const double distance = squared(centre, position(target));
        for (std::size_t i = 0; i < spells.entries.size(); ++i) {
            const auto& entry = spells.entries[i];
            const auto* definition = registry ? registry->find(entry.id) : nullptr;
            if (!definition)
                continue;
            if (definition->spell && (definition->spell->property_flags & 0x800000))
                continue;
            const double limit = (reach + definition->range) * (reach + definition->range);
            if (distance > limit || cast_check(entry.id).code > 1)
                continue;
            // 0x48ef50: kind 4 spells, target mask, self-only, then the 0x30000 visibility tests.
            if (definition->kind != 4 || !definition->spell)
                continue;
            const auto& spell = *definition->spell;
            if ((spell.targets & 0x781f) && !(spell.targets & 0x781f & target.kind_flags))
                continue;
            if ((spell.flags & 0x100000) && &target != this)
                continue;
            if (!cast_target(*definition, &target))
                continue;
            if (spell.flags & 0x30000) {
                int bits = terrain_visibility(match, &target);
                // 0x48ed80 aims at the 0x44e020 collision centre, not the base: a building's ground
                // point read as blocked from above and its ranged attackers fell back to melee.
                if (!(bits & 0x3e)) {
                    Vec3 aim;
                    spell_target_point(aim, &target);
                    bits |= object_visibility(match, aim, &target);
                }
                if (bits & 0x3a)
                    continue;
            }
            if (!CreatureActor::cast_supported(*definition))
                continue; // Bodies the reconstruction cannot cast yet stay unselected.
            if (!commanded && !(evaluate(match, *definition, *this, target) > 0))
                continue;
            // Self abilities such as absh have zero range. Native self scoring has no distance
            // penalty; 0/0 here made their score NaN and prevented automatic casting entirely.
            const double value = (limit > 0 ? 1 - distance / limit * .5 : 1) * bias;
            if (value * factor > best) {
                best = value * factor;
                slot = BookSlot{0, i};
                choice = 66;
                weight = value;
                chosen = &target;
                point = position(target);
            }
        }
    };
    // 0x494fd0: ground creatures cannot reach high flyers, and need a connected region unless flying.
    const auto reachable = [&](const MatchObject& target) {
        if (ritual_building && ritual_building->source_motion() &&
            squared(ritual_building->source_motion()->position, position(target)) > 2500)
            return false;
        if (const auto* actor = as_creature(&target);
            actor && actor->fsm().state() && (actor->fsm().state()->flags & 2) && !(status_flags & 0x200) &&
            match.terrain) {
            const double top = actor->motion.box_min.z + actor->motion.position.z;
            if (top - sacrifice::terrain_surface(*match.terrain, actor->motion.position.x, actor->motion.position.y) -
                    motion.box_max.z >=
                -.1)
                return false;
        }
        if (match.navigation && match.navigation->grid().connected(motion.position, position(target)))
            return true;
        return (status_flags & 0x200) != 0;
    };
    const auto melee_score = [&](MatchObject& target, double scale) {
        const double d = std::sqrt(squared(centre, position(target)));
        const double near = d < range ? 1 - d * .75 / range : d <= 0 ? 0 : range * .25 / d;
        double bias = target.kind_flags & 0x10 ? 1 : &target == attacker ? 1.2 : 1;
        if (previous.kind == 6 && previous.target == &target)
            bias *= 1.3;
        const int crowd = path_cost(match, position(target));
        const double value = (crowd < 9 ? near * near * melee : (1 - crowd * .00625) * near * near * melee) * bias;
        if (value * scale > best) {
            best = value * scale;
            choice = 6;
            chosen = &target;
            weight = value;
            point = position(target);
        }
    };
    switch (kind) {
    case 1:
    case 2:
    case 3:
        capabilities &= 0xc3;
        break;
    case 4: {
        centre = current.point;
        const double d = std::sqrt(squared(current.point, motion.position));
        threshold = 1 - (d > 0 ? (d < 100 ? (100 - d) * .0099999998 : 0) : 1);
        reach = 10;
        if (const auto* wizard = as_creature(controller); wizard && !wizard->computer_wizard)
            range = 50;
        break;
    }
    case 5: {
        if (ordered)
            centre = position(*ordered);
        const double d = std::sqrt(squared(centre, motion.position));
        const double near = d >= 100 ? (d <= 0 ? 0 : 25 / d) : 1 - d * .0075;
        reach = 10;
        threshold = 1 - near * near;
        if (const auto* wizard = as_creature(controller); wizard && !wizard->computer_wizard)
            range = 50;
        if (allegiance && allegiance->controller)
            threshold *= .33333334;
        break;
    }
    case 6:
        if (ordered) {
            threshold = 0;
            reach = 10;
            range = 150;
            mask = ordered->kind_flags & 0x10 ? 16 : 5;
            if (!(capabilities & 0x18)) {
                if ((as_creature(ordered) || attackable_building(*ordered)) && reachable(*ordered))
                    melee_score(*ordered, ordered->kind_flags & 0x10 ? 1 : 1.25);
            } else if (as_creature(ordered) || attackable_building(*ordered)) {
                double bias = as_creature(ordered) && ordered == attacker ? 1.2 : 1;
                if (previous.kind == 66 && previous.target == ordered)
                    bias *= 1.3;
                abilities(*ordered, bias, as_creature(ordered) ? 2 : 1, true);
            }
            capabilities &= 0xf3;
            break;
        }
        [[fallthrough]];
    case 7: {
        const Vec3 step{current.point.x - motion.position.x, current.point.y - motion.position.y,
                        current.point.z - motion.position.z};
        const double length = std::sqrt(static_cast<double>(step.x) * step.x + static_cast<double>(step.y) * step.y +
                                        static_cast<double>(step.z) * step.z);
        const double scale = length <= 15 ? 1 : 15 / length;
        centre = {static_cast<float>(step.x * scale + motion.position.x),
                  static_cast<float>(step.y * scale + motion.position.y),
                  static_cast<float>(step.z * scale + motion.position.z)};
        reach = 25;
        range = 150;
        threshold = .25;
        break;
    }
    default:
        break;
    }
    // 0x494466: +2848 is CURRENT MANA, not an active-ability handle. Ranged/support units
    // keep their spell attack while mana remains instead of choosing melee when a shot is blocked.
    if ((capabilities & 0x18) && mana)
        capabilities &= ~4u;
    if (kind_flags == 1)
        capabilities = allegiance && allegiance->controller ? (kind != 34 ? 3 : 2) : 2;
    if (capabilities & 0x20) { // Self-targeted abilities.
        double bias = this == attacker ? 1.2 : 1;
        if (previous.kind == 66 && previous.target == this)
            bias *= 1.3;
        abilities(*this, bias, 2, false);
    }
    if ((capabilities & 0x10) && allegiance) { // Support abilities on own objects.
        for (auto& group : allegiance->objects)
            for (auto* object : std::vector<MatchObject*>(group.begin(), group.end())) {
                if (!is_friendly(*this, *object) || !(object->kind_flags & mask) || !targetable(*object) ||
                    gone(*object))
                    continue;
                if (as_building(object) && !attackable_building(*object))
                    continue;
                double bias = as_creature(object) && object == attacker ? 1.2 : 1;
                if (previous.kind == 66 && previous.target == object)
                    bias *= 1.3;
                abilities(*object, bias, as_creature(object) ? 2 : 1, false);
            }
    }
    if ((capabilities & 0xc) && allegiance) { // Known hostile objects.
        for (const auto& entry : std::vector<MatchSide::Known>(allegiance->known)) {
            auto* object = entry.object;
            if (match.resolve(entry.reference) != object || !is_hostile(*this, *object) || !(object->kind_flags & mask) ||
                !targetable(*object) || gone(*object))
                continue;
            if (as_building(object) && !attackable_building(*object))
                continue;
            if ((capabilities & 4) && reachable(*object))
                melee_score(*object, object->kind_flags & 0x10 ? 1 : 1.25);
            if (capabilities & 8) {
                double bias = as_creature(object) && object == attacker ? 1.2 : 1;
                if (previous.kind == 66 && previous.target == object)
                    bias *= 1.3;
                abilities(*object, bias, as_creature(object) ? 2 : 1, false);
            }
        }
    }
    // Flight from costly ground or danger (0x4951b0) replaces the choice with a move.
    if ((((capabilities & 2) && ground_cost >= 9) || ((capabilities & 1) && danger > courage)) &&
        flee_point(match, point)) {
        choice = 2;
        chosen = nullptr;
    }
    if (!choice)
        return;
    if (weight > threshold &&
        (chosen != ordered || choice != kind ||
         (choice == 2 && (point.x != current.point.x || point.y != current.point.y || point.z != current.point.z)))) {
        if (choice == 66)
            casting.requested = slot;
        orders.internal({choice, chosen, point}, true); // 0x46da50: temporary flag 0x20.
    }
}
// 0x4951b0: the best of eight nearby cells with no higher path cost and no more danger.
bool CreatureActor::flee_point(const Match& match, Vec3& point) const {
    double escape = 0;
    for (const auto& offset : flee_offsets) {
        const Vec3 candidate{offset[0] + motion.position.x, offset[1] + motion.position.y, motion.position.z};
        const auto cell = path_cell(candidate);
        if (!match.navigation || cell < 0 || cell >= static_cast<std::int64_t>(NavigationGrid::cells) ||
            (match.navigation->grid().flags[0][static_cast<std::size_t>(cell)] & 1))
            continue;
        const int cost = path_cost(match, candidate);
        if (cost > ground_cost)
            continue;
        double value = danger_at(match, *this, candidate);
        if (value > danger)
            continue;
        if (danger != 0)
            value = (danger - value) / danger;
        if (ground_cost)
            value += static_cast<double>(ground_cost - cost) / ground_cost;
        if (value > escape) {
            escape = value;
            point = candidate;
        }
    }
    return escape != 0;
}
// 0x454060: the voice plays when its countdown runs out, then waits 120 + spread * rand() / 0x7fff calls.
void CreatureActor::peasant_voice(FourCC group, int spread) {
    if (peasant->voice_ticks-- > 0)
        return;
    combat_sound(group); // 0x45c600(group, -1).
    peasant->voice_ticks = spread * random_->next() / 0x7fff + 120;
}
// 0x4543c0, every 30 ticks: note danger, find the nearest shelter, then pick the mode.
void CreatureActor::peasant_evaluate(Match& match, int ticks) {
    auto& p = *peasant;
    if (p.evaluate_ticks > ticks) {
        p.evaluate_ticks -= ticks;
        return;
    }
    p.evaluate_ticks = 30;
    if (danger < .2)
        state_flags &= ~0x200000u;
    else
        state_flags |= 0x200000;
    p.shelter = 0;
    match.visit_group(2, ~0u, [&](MatchObject& object) {
        const auto* building = dynamic_cast<const BuildingActor*>(&object);
        if (building && (building->building_flags & 0x8000) && object.source_motion()) {
            const auto distance =
                static_cast<float>(squared(motion.position, object.source_motion()->position));
            if (!p.shelter || distance < p.shelter_distance) {
                p.shelter_distance = distance;
                p.shelter = object.reference();
            }
        }
        return 0;
    });
    if (danger < .5) {
        // 0x492d00(group 1, kind 1): the nearest living wizard within 40 (80 while following one).
        const double radius = p.leader ? 80 : 40;
        double nearest = radius * radius;
        MatchObject* found = nullptr;
        match.visit_group(1, 1, [&](MatchObject& object) {
            const auto* actor = dynamic_cast<const CreatureActor*>(&object);
            if (!actor || !actor->alive())
                return 0;
            const double distance = squared(motion.position, actor->motion.position);
            if (distance < nearest) {
                nearest = distance;
                found = &object;
            }
            return 0;
        });
        if (found && found->friendly(this)) { // Vtable +112 of the wizard on the peasant.
            p.leader = found->reference();
            p.mode = 1;
            return;
        }
        p.leader = 0;
    }
    if (danger >= .2 || ground_cost >= 9)
        p.mode = 0;
    else if (state_flags & 0x100040) {
        p.mode = 1;
        p.leader = 0;
    } else {
        p.leader = 0;
        p.mode = orders.current(true).kind ? 3 : 2;
    }
}
// 0x4540b0: carry out the mode with temporary orders (0x46da50) and voice cues.
void CreatureActor::peasant_think(Match& match) {
    auto& p = *peasant;
    const auto cower = FourCC::from_text("Xcow");
    switch (p.mode) {
    case 0: {
        if (!p.shelter || !(p.shelter_distance < 1600)) {
            peasant_voice(cower, 300);
            return;
        }
        Vec3 point{};
        if (flee_point(match, point)) {
            orders.internal({2, nullptr, point}, true);
            peasant_voice(FourCC::from_text("Xrun"), 240);
        }
        return;
    }
    case 1:
        if (auto* leader = match.resolve(p.leader))
            orders.internal({3, leader, {}}, true);
        return;
    case 2: {
        if (orders.kind() != 0)
            return;
        // Huddle at the centre of own-side creatures of this definition within 40; those within 80 also
        // share their friendly exception mask (+2896).
        double x = motion.position.x, y = motion.position.y, count = 1;
        if (allegiance)
            for (auto* object : allegiance->objects[0]) {
                auto* actor = dynamic_cast<CreatureActor*>(object);
                if (!actor || actor == this || actor->kind_flags == 8 || actor->definition().id != definition().id)
                    continue;
                const double distance = squared(actor->motion.position, motion.position);
                if (distance > 6400)
                    continue;
                friendly_exception_mask |= actor->friendly_exception_mask;
                if (distance <= 1600) {
                    x += actor->motion.position.x;
                    y += actor->motion.position.y;
                    count += 1;
                }
            }
        Vec3 centre{static_cast<float>(x / count), static_cast<float>(y / count), motion.position.z};
        if (match.terrain)
            centre.z = static_cast<float>(terrain_surface(*match.terrain, centre.x, centre.y));
        if (squared(motion.position, centre) > 100) {
            orders.internal({2, nullptr, centre}, true);
            return;
        }
        if (count > 1) {
            const bool afraid = state_flags & 0x200000;
            peasant_voice(afraid ? cower : FourCC::from_text("Xgat"), afraid ? 300 : 660);
        }
        return;
    }
    default:
        return;
    }
}
} // namespace sacrifice
