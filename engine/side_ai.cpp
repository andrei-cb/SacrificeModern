#include "sacrifice/side_ai.hpp"
#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/navigation.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/soul_actor.hpp"
#include "sacrifice/spell_registry.hpp"
#include "sacrifice/spell_combat.hpp"
#include <algorithm>
#include <cmath>
#include <format>

namespace sacrifice {
namespace {
const FourCC mano_id = FourCC::from_text("maho"); // 1835100271, the manahoar summon and creature.
const FourCC doctor_id = FourCC::from_text("sacd"); // 1935762276: scorers weigh sacrifice doctors heavily.
constexpr std::array<int, 4> squad_modes{1, 2, 0, 4}; // 0x486060 per category.
// Native falloff used throughout the agent helpers: 1 inside `inner`, linear to 0 at `outer`.
double falloff(double distance, double inner, double outer) {
    if (distance <= inner)
        return 1;
    return distance < outer ? (outer - distance) / (outer - inner) : 0;
}
double distance_squared(Vec3 a, Vec3 b) {
    const double x = static_cast<double>(a.x) - b.x, y = static_cast<double>(a.y) - b.y,
                 z = static_cast<double>(a.z) - b.z;
    return x * x + y * y + z * z;
}
double distance(Vec3 a, Vec3 b) {
    return std::sqrt(distance_squared(a, b));
}
double lerp(double from, double weight, double to) { // 0x48d850.
    return (to - from) * weight + from;
}
Vec3 position_of(const MatchObject& object) {
    const auto* motion = object.source_motion();
    return motion ? motion->position : Vec3{};
}
// 0x4654e0 == 'sacu': the altar's ritual (+1112 victim, +1120 action) is a desecration.
bool desecrating(const MatchObject* altar) {
    const auto* building = dynamic_cast<const BuildingActor*>(altar);
    return building && building->ritual.victim && building->ritual.action == FourCC::from_text("sacu");
}
// Wizard +2972: the home altar of the side's wizard is being desecrated.
bool home_desecrated(const MatchObject* object) {
    const auto* actor = dynamic_cast<const CreatureActor*>(object);
    return actor && actor->type == MatchObject::wizard && actor->home_altar && desecrating(actor->home_altar);
}
bool spirit(const MatchObject* object) { // Wizard FSM state flag 0x40000 (native +1536 +48).
    const auto* actor = dynamic_cast<const CreatureActor*>(object);
    return actor && actor->type == MatchObject::wizard && actor->fsm().state() &&
           (actor->fsm().state()->flags & 0x40000);
}
// 0x489830 relation flags and 0x489ac0 category: own, allied (or same identity), enemy, else neutral.
std::uint32_t relation(const MatchSide* object, const MatchSide& own) {
    if (object == &own)
        return 1;
    if (!object)
        return 0;
    if ((object->allies & own.identity) || (object->identity & own.allies) || object->identity == own.identity)
        return 2;
    if (((object->enemies & own.identity) || (object->identity & own.enemies)) && object->identity != own.identity)
        return 4;
    return 0;
}
int category_of(std::uint32_t relation) {
    return relation & 1 ? 0 : relation & 2 ? 1 : relation & 4 ? 3 : 2;
}
// The side +16 / +24 test shared by the scorers and 0x486870: enemies and not the same identity.
bool hostile(const MatchSide* a, const MatchSide* b) {
    return a && b && ((a->identity & b->enemies) || (b->identity & a->enemies)) && a->identity != b->identity;
}
// Object vtable +128 (0 current, 1 maximum health) for creatures and buildings.
std::pair<int, int> health_of(const MatchObject& object) {
    if (const auto* actor = dynamic_cast<const CreatureActor*>(&object))
        return {actor->stats.current[CreatureStats::health], actor->stats.base[CreatureStats::health]};
    if (const auto* building = dynamic_cast<const BuildingActor*>(&object))
        return {building->health, building->maximum_health};
    return {};
}
bool doctor(const MatchObject& object) {
    const auto* actor = dynamic_cast<const CreatureActor*>(&object);
    return actor && actor->definition().id == doctor_id;
}
// 0x48c240 scorer functions: the 0x4d25b0 id table first, then the spell flags.
enum Scorer : int {
    none,
    nearby_enemies, // anmo 0x48c3e0
    guardian,       // grdn 0x48c700
    healing,        // heal 0x48c7d0
    speed,          // spup 0x48c920
    teleport,       // tele 0x48ca70
    conversion,     // sacc 0x48c7c0
    death,          // deat 0x48cbd0
    damage,         // 0x48c460
    area_damage,    // 0x48c540
    weaken,         // 0x48cb70
    protect,        // 0x48c8a0
};
int scorer_function(const SpellDefinition& definition) {
    static const std::array<std::pair<FourCC, int>, 7> table{{{FourCC::from_text("anmo"), nearby_enemies},
                                                              {FourCC::from_text("grdn"), guardian},
                                                              {FourCC::from_text("heal"), healing},
                                                              {FourCC::from_text("spup"), speed},
                                                              {FourCC::from_text("tele"), teleport},
                                                              {FourCC::from_text("sacc"), conversion},
                                                              {FourCC::from_text("deat"), death}}};
    for (const auto& [id, function] : table)
        if (id == definition.id)
            return function;
    const auto flags = definition.spell->flags;
    if (flags & 0x40000000)
        return (flags & 0x400000) && definition.spell->spread_limit > 0 ? area_damage : damage;
    if (flags & 0x80000000)
        return healing;
    if (flags & 0x8000000)
        return weaken;
    if (flags & 0x4000000)
        return protect;
    return none;
}
// 0x48be60: spell power is discounted as its mana cost approaches the available mana.
double spell_strength(const SpellData& spell, int mana) {
    const double fraction = mana > spell.mana_cost ? static_cast<double>(spell.mana_cost) / mana : 1;
    return (1 - fraction) * spell.range * spell.ai_power * .04;
}
double ranged_scale(const std::array<std::int16_t, 24>& stats) {
    return stats[0] * .001 * stats[8] * (stats[4] == 0 ? .001 : .001020408163265306);
}
} // namespace

float SideController::Strength::total() const {
    return (v[2] + v[3]) * .5f + (v[0] + v[1]) * 2;
}
void SideController::Strength::add(float self, const Strength& other, float factor) {
    for (std::size_t i = 0; i < v.size(); ++i)
        v[i] = factor * other.v[i] + self * v[i];
}
void SideController::Strength::divide(float value) {
    if (value != 0)
        for (std::size_t i = 0; i < 4; ++i)
            v[i] /= value;
}
// 0x48bd80 with the definition stats (0x48bad0), plus the spell terms of 0x48be60.
SideController::Strength SideController::definition_strength(const CreatureDefinition& d,
                                                             const SpellRegistry* registry) {
    Strength s;
    const auto& t = d.stats;
    s.v[0] = static_cast<float>(t[0] * .001 * d.melee_damage);
    const double base = (static_cast<double>(t[7]) + t[7] + t[6]) * .001 * (t[1] * .001 * t[5]);
    const double resistance = t[9] ? t[9] : 1, speed = t[12] ? t[12] : 1;
    s.v[2] = static_cast<float>(1000 / resistance * base);
    if (t[4] > 0)
        s.v[2] *= 4;
    s.v[3] = static_cast<float>(1000 / speed * base);
    s.v[4] = static_cast<float>((t[3] <= t[4] ? t[4] : t[3]) * .001);
    if (registry)
        for (const auto id : d.spells) {
            if (!id.value)
                break;
            const auto* definition = registry->find(id);
            if (!definition)
                break;
            if (definition->spell)
                s.v[1] += static_cast<float>(ranged_scale(t) * spell_strength(*definition->spell, d.mana));
        }
    return s;
}
// 0x48b7e0: the same terms from the live stats (+2244.., current health +2254) for CRE8/WIZD bodies.
SideController::Strength SideController::creature_strength(const CreatureActor& actor,
                                                           const SpellRegistry* registry) {
    Strength s;
    const auto& t = actor.stats.current;
    const auto& d = actor.definition();
    if (d.kind != 2 && d.kind != 8)
        return s;
    s.v[0] = static_cast<float>(t[0] * .001 * d.melee_damage);
    const double base = (static_cast<double>(t[7]) + t[7] + t[6]) * .001 * (t[5] * (t[1] * .001));
    const double resistance = t[9] ? t[9] : 1, speed = t[12] ? t[12] : 1;
    s.v[2] = static_cast<float>(1000 / resistance * base);
    if (t[4] > 0)
        s.v[2] *= 4;
    s.v[3] = static_cast<float>(1000 / speed * base);
    s.v[4] = static_cast<float>((t[3] > t[4] ? t[3] : t[4]) * .001);
    if (registry) {
        for (const auto& entry : actor.spells.entries) {
            const auto* definition = registry->find(entry.id);
            if (definition && definition->kind == 4 && definition->spell &&
                (definition->spell->flags & 0x40000000) && (definition->spell->targets & 4))
                s.v[1] +=
                    static_cast<float>(ranged_scale(t) * spell_strength(*definition->spell, actor.mana));
        }
        if (actor.type == MatchObject::wizard)
            for (const auto& entry : actor.wizard_spells[1].entries) {
                const auto* definition = registry->find(entry.id);
                if (definition && definition->kind == 4 && definition->spell &&
                    (definition->spell->flags & 0xfc000000))
                    s.v[1] += static_cast<float>(spell_strength(*definition->spell, actor.mana));
            }
    }
    return s;
}
float SideController::compare(const Strength& a, const Strength& b) {
    const double own = std::max(a.v[0] - b.v[2] * .1, 0.0) + std::max(a.v[1] - b.v[3] * .1, 0.0);
    const double other = std::max(b.v[0] - a.v[2] * .1, 0.0) + std::max(b.v[1] - a.v[3] * .1, 0.0);
    return static_cast<float>(own - other + (a.v[4] - b.v[4]) * 10.0);
}
int SideController::Agent::category() const {
    return category_of(flags);
}
CreatureActor* SideController::Agent::actor() const {
    return kind == Kind::unit ? dynamic_cast<CreatureActor*>(object) : nullptr;
}
BuildingActor* SideController::Agent::building() const {
    return kind == Kind::building ? dynamic_cast<BuildingActor*>(object) : nullptr;
}
std::pair<int, int> SideController::Agent::mana() const { // 0x48a090; buildings and souls 0x489a60.
    // Manahoar vtable +20 is 0x48a890 -> 0x489a60: it supplies mana, not demand.
    if (const auto* unit = actor(); unit && !(status & 0x10))
        return {unit->mana, unit->maximum_mana};
    return {};
}
std::pair<int, int> SideController::Agent::souls_pair() const { // 0x48a7a0 / 0x490f10 (+188, +192).
    if (const auto* unit = actor())
        return unit->type == MatchObject::wizard ? std::pair{0, souls} : std::pair{unit->soul_count, 0};
    return {};
}
// 0x48e9b0: within 30 * min(1 + count / 10, 1.8) of the mean (+28), measured from the object position.
bool SideController::Squad::accepts(const Agent& agent) const {
    const double radius = std::min(count * .1 + 1, 1.799999952316284) * 30;
    const auto p = position_of(*agent.object);
    const double x = p.x - mean.x, y = p.y - mean.y, z = p.z - mean.z;
    return x * x + y * y + z * z <= radius * radius;
}

// 0x4837c0 / 0x483aa0; the task table is 0x483590 (period, dirty mask, member).
SideController::SideController(Match& match, MatchSide& side, std::uint32_t value, float factor)
    : flags(value), scale(factor), match_(match), side_(side),
      schedule_{{{12, 0xe, &SideController::regroup},
                 {60, 0xf, &SideController::strategy},
                 {6, 1, &SideController::run_agents},
                 {6, 1, &SideController::run_squads},
                 {60, 0x1f, &SideController::replan},
                 {12, 1, &SideController::run_tasks}}} {
    side.event_handler = [this](MatchObject& target, MatchObject* source, int code, int) {
        event(target, source, code);
    };
}
SideController::~SideController() {
    side_.event_handler = nullptr;
}
std::vector<const SideController::Task*> SideController::tasks() const {
    std::vector<const Task*> result;
    for (const auto& planner : planners_)
        for (const auto& task : planner)
            result.push_back(task.get());
    return result;
}
void SideController::tick(int ticks) {
    if (match_.cinematic || (flags & 8)) // 0x485b30 also stops the AI schedule clock in cinematic mode.
        return;
    // Port repair: rendered frames here can advance no match ticks. Running the schedule then fires
    // run_agents with zero ticks and moves its slot on, so at high frame rates agent clocks never
    // advance and the wizard's plan/react clocks never come due (it idles at its first site).
    if (ticks <= 0)
        return;
    handled_ = 0;
    for (auto& task : schedule_) {
        if (task.next > clock_) {
            if (dirty & task.mask)
                (this->*task.run)(ticks);
        } else {
            (this->*task.run)(ticks);
            task.next += task.period;
        }
    }
    dirty &= ~handled_;
    clock_ += static_cast<unsigned>(ticks);
}
void SideController::event(MatchObject& target, MatchObject* source, int code) {
    switch (code) {
    case 1:
    case 2:
        dirty |= 3;
        break;
    case 6:
        drop(*source);
        break;
    case 11:
        if (source && source->allegiance == &side_ && accepts(target))
            dirty |= 7;
        break;
    case 8:
    case 18:
        if (accepts(target))
            dirty |= 7;
        break;
    case 12:
    case 13:
    case 15:
        dirty |= 1;
        break;
    case 16:
        dirty |= 2;
        break;
    case 19:
        dirty |= 0x13;
        break;
    case 22:
        if (auto* agent = find(target); agent && agent->kind == Kind::unit && target.type == MatchObject::wizard)
            agent->rebuild = true; // 0x490ab0.
        break;
    default:
        break;
    }
}
bool SideController::accepts(const MatchObject& object) const {
    if (!(object.kind_flags & 0x17) || (object.state_flags & MatchObject::removed_flag))
        return false;
    // 0x48aac0: only plan for objects in the wizard's connected LAND region.
    if (const auto* wizard = side_.wizard(); wizard && match_.navigation) {
        const auto* from = object.source_motion();
        const auto* to = wizard->source_motion();
        if (!from || !to || !match_.navigation->grid().connected(from->position, to->position))
            return false;
    }
    if (object.kind_flags == 16) {
        const auto* building = dynamic_cast<const BuildingActor*>(&object);
        return building && (!building->primary || building->primary == building) &&
               (building->building_flags & 0xc17);
    }
    if (object.kind_flags & 5) {
        const auto* actor = dynamic_cast<const CreatureActor*>(&object);
        return actor && actor->fsm().state() && !(actor->fsm().state()->flags & 0x4000);
    }
    return true;
}
SideController::Agent* SideController::find(const MatchObject& object) const {
    for (const auto& agent : agents_)
        if (agent->object == &object)
            return agent.get();
    return nullptr;
}
SideController::Agent* SideController::add(MatchObject& object, int mode) {
    if (!accepts(object) || !object.allegiance && object.kind_flags != 16 && object.kind_flags != 2)
        return nullptr;
    auto agent = std::make_unique<Agent>();
    agent->object = &object;
    agent->reference = object.reference();
    agent->flags = static_cast<std::uint32_t>(mode) | relation(object.allegiance, side_);
    agent->position = agent->predicted = agent->previous = position_of(object);
    if (object.kind_flags & 5) {
        agent->kind = Kind::unit;
        agent->flags |= 0x10 | 0x40; // 0x489ef0.
        auto* actor = dynamic_cast<CreatureActor*>(&object);
        if (actor) {
            if (actor->stats.current[2] || !actor->spells.entries.empty())
                agent->status |= 1;
            if (actor->status_flags & 0x200)
                agent->status |= 0x2000;
            // +96: the lowest ability cost bounded by the maximum mana, or 0 without abilities.
            agent->threshold = actor->spells.entries.empty() ? 0.0f : static_cast<float>(actor->maximum_mana);
            for (const auto& entry : actor->spells.entries)
                if (const auto* definition = match_.spell_registry ? match_.spell_registry->find(entry.id) : nullptr)
                    agent->threshold = std::min(agent->threshold, static_cast<float>(definition->mana_cost));
            if (actor->type == MatchObject::wizard) { // 0x48f390.
                agent->flags |= 0x100;
                agent->status = (agent->status & ~1u) | 0x2000e;
                agent->threshold = static_cast<float>(actor->maximum_mana / 4);
                if (agent->flags & 1)
                    actor->status_flags |= 0x8000; // Enable the native computer wizard's threat/retreat scan.
            } else if (actor->definition().id == mano_id) // 0x48a7f0.
                agent->status |= 0x10;
            agent->strength = creature_strength(*actor, match_.spell_registry.get());
            agent->power = agent->strength.total();
        }
    } else if (object.kind_flags == 16) {
        agent->kind = Kind::building;
        agent->flags |= 0x20; // 0x489bd0.
        if (const auto* building = agent->building()) {
            if (building->building_flags & 1)
                agent->flags |= 0x300;
            if (building->building_flags & 0xc17)
                agent->flags |= 0x100;
        }
    } else {
        agent->kind = Kind::soul;
        agent->flags |= 8; // 0x489db0.
    }
    auto& result = *agents_.emplace_back(std::move(agent));
    const int category = result.category();
    all_[category].push_back(&result); // 0x484110 and the per-kind lists 0x4842b0 / 0x484470 / 0x484610.
    (result.kind == Kind::unit ? units_ : result.kind == Kind::building ? buildings_ : souls_)[category].push_back(
        &result);
    run(result, 0);
    dirty |= 1;
    return &result;
}
void SideController::drop(MatchObject& object) {
    if (auto* agent = find(object)) {
        remove(*agent);
        dirty |= 1;
    }
}
void SideController::remove(Agent& agent) {
    for (auto& planner : planners_)
        for (auto& task : planner) {
            std::erase(task->members, &agent);
            if (task->leader == &agent)
                task->leader = nullptr;
            if (task->target == &agent)
                task->target = nullptr;
        }
    for (auto* lists : {&squads_, &clusters_})
        for (auto& category : *lists)
            for (auto& squad : category)
                std::erase(squad->members, &agent);
    for (auto* lists : {&all_, &units_, &buildings_, &souls_})
        for (auto& list : *lists)
            std::erase(list, &agent);
    for (auto& other : agents_) {
        if (std::erase_if(other->plans, [&](const Agent::Plan& p) { return p.target == &agent || p.point == &agent; }))
            other->plan_clock = other->clock;
        for (auto& plan : other->issued_plans) {
            if (plan.target == &agent)
                plan.target = nullptr;
            if (plan.point == &agent)
                plan.point = nullptr;
        }
    }
    agents_.remove_if([&](const std::unique_ptr<Agent>& a) { return a.get() == &agent; });
}
bool SideController::valid(const Agent& agent) const {
    return match_.resolve(agent.reference) == agent.object &&
           !(agent.object->state_flags & MatchObject::removed_flag);
}
bool SideController::alive(const Agent& agent) const {
    if (!valid(agent))
        return false;
    const auto* actor = agent.actor();
    return !actor || actor->alive();
}

void SideController::regroup(int) {
    if (dirty & 0x10) {
        std::vector<Agent*> changed;
        for (auto& agent : agents_)
            if (agent->category() &&
                category_of(relation(agent->object->allegiance, side_)) != agent->category())
                changed.push_back(agent.get());
        for (auto* agent : changed)
            remove(*agent);
        handled_ |= 0x10;
    }
    if (dirty & 4) { // Scan 23 (0x491e50): register the side roster.
        for (const auto& group : side_.objects) {
            for (auto* object : group) {
                if (!(object->kind_flags & 23))
                    continue;
                auto* agent = find(*object);
                if (agent && agent->category() != 0) {
                    remove(*agent);
                    agent = nullptr;
                }
                if (!agent)
                    add(*object);
            }
        }
        handled_ |= 4;
    }
    if ((dirty & 8) && (flags & 0x10)) { // Scan 21 (0x491dd0): buildings, wizards, status bit 0x10.
        for (const auto& object : match_.objects())
            if ((object->kind_flags == 16 || object->kind_flags == 1 || (object->common_status & 0x10)) &&
                !find(*object))
                add(*object);
        handled_ |= 8;
    }
    if (dirty & 2) {
        count_units();
        handled_ |= 2;
    }
}
void SideController::count_units() {
    int total = 0;
    for (const auto& agent : agents_) {
        if (agent->kind == Kind::soul)
            ++total;
        else {
            const auto [bound, free] = agent->souls_pair(); // Unit vtable +84.
            total += bound + free;
        }
    }
    units_maximum = std::max(units_maximum, total);
    fountains = 0;
    for (const auto& agent : agents_)
        if (const auto* building = agent->building(); building && agent->category() == 2 && !building->primary &&
                                                       (building->building_flags & 0x10))
            ++fountains;
}
// 0x486870 with the reset 0x486660 and the squad sums 0x4867e0.
void SideController::strategy(int) {
    statistics_ = {};
    enemy_wizards = 0;
    for (auto* other : match_.side_order)
        if (hostile(other, &side_) && other->wizard())
            ++enemy_wizards;
    for (int c = 0; c < 4; ++c) {
        auto& block = statistics_[c];
        for (const auto& squad : squads_[c]) {
            block.units += squad->count;
            block.mana += squad->mana;
            block.maximum += squad->maximum;
            block.strength.add(1, squad->strength, 1);
        }
        for (const auto* agent : units_[c]) {
            const auto [first, second] = agent->souls_pair();
            block.souls[0] += first;
            block.souls[1] += second;
            block.souls[2] += first + second;
        }
        for (const auto* agent : buildings_[c])
            if (agent->status & 0x4000)
                ++block.manaliths; // 0x487060.
        for (int k = 0; k < 3; ++k)
            block.ratio[k] = c ? static_cast<float>(static_cast<double>(statistics_[0].souls[k]) /
                                                    (block.souls[k] + statistics_[0].souls[k]))
                               : 1.0f;
    }
    int altars = 0, seen_altars = 0, sites = 0, seen_sites = 0;
    for (const auto* agent : buildings_[3])
        if (agent->flags & 0x200) {
            ++altars;
            seen_altars += (agent->flags & 0x80) != 0;
        }
    for (const auto* agent : buildings_[2])
        if (agent->flags & 0x100) {
            ++sites;
            seen_sites += (agent->flags & 0x80) != 0;
        }
    knowledge = altars + sites ? static_cast<float>(static_cast<double>(seen_altars + seen_sites) / (altars + sites))
                               : 0.0f; // Native divides 0 by 0 here.
    friendly_average = {};
    friendly_average.add(1, statistics_[0].strength, 1);
    friendly_average.add(1, statistics_[1].strength, 1);
    friendly_average.divide(static_cast<float>(statistics_[1].units + statistics_[0].units));
    enemy_average = {};
    enemy_average.add(1, statistics_[3].strength, 1);
    enemy_average.divide(static_cast<float>(statistics_[3].units));
}
void SideController::run_agents(int ticks) {
    std::vector<Agent*> stale;
    for (auto& agent : agents_)
        if (!valid(*agent))
            stale.push_back(agent.get());
    for (auto* agent : stale)
        remove(*agent);
    for (auto& agent : agents_)
        run(*agent, ticks);
}
void SideController::run_squads(int) {
    for (auto& category : squads_)
        for (auto& squad : category)
            update(*squad);
}
void SideController::update(Squad& squad) const {
    squad.previous = squad.mean;
    squad.mean = squad.velocity = squad.deviation = {};
    squad.count = 0;
    squad.status = 0;
    double n = 0;
    Vec3 squares{};
    for (auto* member : std::vector<Agent*>(squad.members)) {
        if (!alive(*member)) {
            std::erase(squad.members, member);
            if (member->squad == &squad)
                member->squad = nullptr;
            continue;
        }
        ++squad.count;
        squad.status |= member->status;
        if (member->status & 3) {
            ++n;
            const auto& v = member->velocity;
            const auto& p = member->position;
            squad.velocity = {squad.velocity.x + v.x, squad.velocity.y + v.y, squad.velocity.z + v.z};
            squad.mean = {squad.mean.x + p.x, squad.mean.y + p.y, squad.mean.z + p.z};
            squares = {squares.x + p.x * p.x, squares.y + p.y * p.y, squares.z + p.z * p.z};
        }
    }
    if (n) {
        const double r = 1 / n;
        squad.velocity = {static_cast<float>(r * squad.velocity.x), static_cast<float>(r * squad.velocity.y),
                          static_cast<float>(r * squad.velocity.z)};
        squad.mean = {static_cast<float>(r * squad.mean.x), static_cast<float>(r * squad.mean.y),
                      static_cast<float>(r * squad.mean.z)};
        squad.deviation = {static_cast<float>(std::sqrt(std::fabs(r * squares.x - squad.mean.x * squad.mean.x))),
                           static_cast<float>(std::sqrt(std::fabs(r * squares.y - squad.mean.y * squad.mean.y))),
                           static_cast<float>(std::sqrt(std::fabs(r * squares.z - squad.mean.z * squad.mean.z)))};
    }
    squad.center = {squad.mean.x + squad.velocity.x, squad.mean.y + squad.velocity.y,
                    squad.mean.z + squad.velocity.z};
    squad.strength = {};
    squad.mana = squad.maximum = squad.health = squad.health_maximum = squad.sources = squad.hoars = squad.souls = 0;
    for (const auto* member : squad.members) {
        if (!member->status)
            continue;
        const auto [mana, maximum] = member->mana();
        squad.mana += mana;
        squad.maximum += maximum;
        const auto [health, health_maximum] = health_of(*member->object);
        squad.health += health;
        squad.health_maximum += health_maximum;
        squad.strength.add(1, member->strength, 1);
        if (member->status & 0x1000)
            ++squad.sources;
        if (member->status & 0x10)
            ++squad.hoars;
        const auto [first, second] = member->souls_pair();
        squad.souls += first + second;
    }
}
// 0x4894d0: soul agents carry no status bits, so native clusters keep a zero mean.
void SideController::update_cluster(Squad& cluster) const {
    cluster.previous = cluster.mean;
    cluster.mean = cluster.velocity = cluster.deviation = {};
    cluster.count = 0;
    cluster.status = 0;
    double n = 0;
    for (auto* member : std::vector<Agent*>(cluster.members)) {
        if (!valid(*member)) {
            std::erase(cluster.members, member);
            if (member->squad == &cluster)
                member->squad = nullptr;
            continue;
        }
        ++cluster.count;
        cluster.status |= member->status;
        if (member->status & 3) {
            ++n;
            const auto& p = member->position;
            cluster.mean = {cluster.mean.x + p.x, cluster.mean.y + p.y, cluster.mean.z + p.z};
        }
    }
    if (n)
        cluster.mean = {static_cast<float>(cluster.mean.x / n), static_cast<float>(cluster.mean.y / n),
                        static_cast<float>(cluster.mean.z / n)};
    cluster.center = cluster.mean;
}
void SideController::run(Agent& agent, int ticks) {
    if (agent.flags & 0x81) { // 0x489980.
        agent.clock += static_cast<unsigned>(ticks);
        agent.position = position_of(*agent.object);
        agent.velocity = {agent.position.x - agent.previous.x, agent.position.y - agent.previous.y,
                          agent.position.z - agent.previous.z};
        if (ticks) {
            const float t = static_cast<float>(ticks);
            agent.velocity = {agent.velocity.x / t, agent.velocity.y / t, agent.velocity.z / t};
        }
        agent.previous = agent.position;
        agent.predicted = {agent.position.x + agent.velocity.x, agent.position.y + agent.velocity.y,
                           agent.position.z + agent.velocity.z};
    }
    if (agent.kind == Kind::building) { // 0x489c70.
        if (agent.clock >= agent.strength_clock) {
            agent.strength_clock = agent.clock + 32;
            // 0x48b660: an attackable building contributes maximum health * 10 to ranged defence.
            if (const auto* building = agent.building(); building && (building->building_flags & 0x80)) {
                agent.strength.v[2] = 0;
                agent.strength.v[3] = static_cast<float>(building->maximum_health * 10.0);
            }
            agent.power = agent.strength.total();
        }
        if (agent.clock < agent.status_clock)
            return;
        agent.status_clock = agent.clock + 8;
        const auto* building = agent.building();
        if (!building)
            return;
        const auto f = building->building_flags;
        if (!building->primary && (f & 0x10))
            agent.status = building->reserved ? agent.status & ~0x8000u : agent.status | 0x8000;
        const bool mana = !building->reserved && !building->primary && building->reveal == 1 && (f & 0xc00) &&
                          building->mana_rate;
        agent.status = mana ? agent.status | 0x1000 : agent.status & ~0x1000u;
        agent.status = !building->primary && (f & 0x4000) ? agent.status | 0x4000 : agent.status & ~0x4000u;
        agent.status = f & 2 ? agent.status | 0x40000 : agent.status & ~0x40000u;
        return;
    }
    if (agent.kind != Kind::unit)
        return;
    if (agent.clock >= agent.strength_clock) { // 0x48a140 / 0x48a050.
        agent.strength_clock = agent.clock + 16;
        update_strength(agent);
    }
    if (agent.object->type != MatchObject::wizard || !(agent.flags & 1))
        return;
    if (agent.clock >= agent.weigh_clock) { // 0x4908e0, dword_4d2604.
        agent.weigh_clock = agent.clock + 8;
        weigh(agent);
    }
    if (agent.rebuild) {
        prepare_wizard(agent);
        agent.rebuild = false;
    }
    if (agent.clock >= agent.soul_clock) { // dword_4d2628.
        agent.soul_clock = agent.clock + 32;
        if (const auto* actor = agent.actor())
            agent.souls = actor->soul_count;
    }
}
void SideController::update_strength(Agent& agent) {
    if (const auto* actor = agent.actor()) {
        agent.strength = creature_strength(*actor, match_.spell_registry.get());
        agent.power = agent.strength.total();
    }
}
// 0x4908e0: enemy squad strength near the wizard against the own, allied and enemy sums (10..280).
void SideController::weigh(Agent& agent) {
    agent.danger = 0;
    Strength own, allied, enemy;
    presence(0, agent.predicted, 10, 280, &own);
    presence(1, agent.predicted, 10, 280, &allied);
    presence(3, agent.predicted, 10, 280, &enemy);
    agent.danger = enemy.total();
    if (agent.danger != 0)
        agent.danger = agent.danger / (allied.total() + (enemy.total() + own.total()));
    if (const auto* actor = agent.actor())
        agent.threshold = static_cast<float>(actor->maximum_mana / 4);
}
// 0x48f460 (creature summons from book 0), 0x48f710 (scorers from books 1 and 2), 0x48f5e0 (mano,
// manalith, sacc, sacu).
void SideController::prepare_wizard(Agent& agent) {
    agent.summons.clear();
    agent.scorers.clear();
    agent.mano = agent.manalith = agent.convert = agent.desecrate = {};
    const auto* actor = agent.actor();
    if (!actor || !match_.spell_registry)
        return;
    for (const auto& entry : actor->wizard_spells[0].entries)
        if (const auto* definition = match_.spell_registry->find(entry.id);
            definition && definition->kind == 2 && entry.id != mano_id && match_.prepare_creature)
            agent.summons.push_back({entry.id, {}, definition});
    for (const auto& book : actor->wizard_spells)
        for (const auto& entry : book.entries)
            if (const auto* definition = match_.spell_registry->find(entry.id);
                definition && definition->kind == 16 && !agent.manalith.value)
                agent.manalith = entry.id;
    for (auto& summon : agent.summons)
        if (const auto* creature = match_.creature_definition ? match_.creature_definition(summon.id) : nullptr)
            summon.strength = definition_strength(*creature, match_.spell_registry.get());
    // 0x48c1d0 creates scorers for spells with flags 0xfc000000; spells whose body the reconstruction
    // cannot cast yet are skipped so plans never reach an unimplemented host.
    for (int b = 1; b <= 2; ++b)
        for (std::size_t e = 0; e < actor->wizard_spells[b].entries.size(); ++e) {
            const auto& entry = actor->wizard_spells[b].entries[e];
            const auto* definition = match_.spell_registry->find(entry.id);
            if (!definition || definition->kind != 4 || !definition->spell ||
                !(definition->spell->flags & 0xfc000000) || !CreatureActor::cast_supported(*definition))
                continue;
            agent.scorers.push_back({entry.id, b, static_cast<int>(e), definition, scorer_function(*definition)});
        }
    if (actor->find_spell(mano_id))
        agent.mano = mano_id;
    if (actor->find_spell(FourCC::from_text("sacc")))
        agent.convert = FourCC::from_text("sacc");
    if (actor->find_spell(FourCC::from_text("sacu")))
        agent.desecrate = FourCC::from_text("sacu");
}

// 0x486060: refresh (0x4870b0), release squads (0x483ee0), reorder and regroup each category
// (0x487450 squads, 0x487ab0 soul clusters), build tasks (0x487fb0) and assign units (0x487250).
void SideController::replan(int) {
    // 0x4870b0 refreshes sightings from SIDE's known list, not from the global object list.
    for (const auto& sighting : side_.known)
        if (match_.resolve(sighting.reference) == sighting.object && accepts(*sighting.object)) {
            auto* agent = find(*sighting.object);
            if (agent && agent->category() != category_of(relation(sighting.object->allegiance, side_))) {
                remove(*agent);
                agent = nullptr;
            }
            if (!agent)
                agent = add(*sighting.object);
            if (agent) {
                agent->flags |= 0x80;
                agent->seen = agent->clock;
            }
        }
    std::vector<Agent*> stale;
    for (auto& agent : agents_)
        if (!(agent->flags & 1) &&
            (spirit(agent->object) || !valid(*agent) ||
             ((agent->flags & 0x80) && agent->seen < agent->clock && !(agent->object->common_status & 0x10))))
            stale.push_back(agent.get());
    for (auto* agent : stale)
        remove(*agent);
    for (auto& agent : agents_)
        agent->squad = nullptr;
    for (int c = 0; c < 4; ++c) {
        squads_[c].clear();
        clusters_[c].clear();
        sort_units(units_[c]);
        build_squads(c);
        build_clusters(c);
    }
    for (auto& agent : agents_)
        agent->task = nullptr;
    plan_sites();
    plan_guards();
    plan_idle();
    std::vector<Agent*> available; // 0x487250: own units, linked through +152.
    for (auto* agent : units_[0]) {
        // Object vtable +76 (0x46ad40): not status 0x100 and no FSM state flag 0x2000. Wizard
        // spirits pass, so a dead computer wizard keeps a task and visit_mana takes it home.
        const auto* actor = agent->actor();
        if (valid(*agent) && (!actor || (!(actor->status_flags & 0x100) && actor->fsm().state() &&
                                         !(actor->fsm().state()->flags & 0x2000))))
            available.push_back(agent);
    }
    assign_sites(available);
    assign_guards(available);
    assign_idle(available);
    handled_ |= 1;
}
// The 0x486060 pass moves each unit tailward past stronger ones: a stable descending sort by +124.
void SideController::sort_units(std::vector<Agent*>& list) const {
    std::stable_sort(list.begin(), list.end(), [](const Agent* a, const Agent* b) { return a->power > b->power; });
}
void SideController::build_squads(int category) {
    std::vector<Agent*> pending = units_[category];
    while (!pending.empty()) {
        auto squad = std::make_unique<Squad>();
        squad->mode = squad_modes[category];
        squad->category = category;
        squad->index = static_cast<int>(squads_[category].size());
        auto* first = pending.front();
        pending.erase(pending.begin());
        squad->members.push_back(first); // Vtable +12 (0x4896b0) counts each addition.
        ++squad->count;
        first->squad = squad.get();
        for (;;) { // 0x4879b0 refreshes the squad, then absorbs the first accepted unit and recurses.
            update(*squad);
            const auto it = std::ranges::find_if(pending, [&](const Agent* a) { return squad->accepts(*a); });
            if (it == pending.end())
                break;
            (*it)->squad = squad.get();
            squad->members.push_back(*it);
            ++squad->count;
            pending.erase(it);
        }
        update(*squad);
        squads_[category].push_back(std::move(squad));
    }
}
void SideController::build_clusters(int category) {
    std::vector<Agent*> pending = souls_[category];
    while (!pending.empty()) {
        auto cluster = std::make_unique<Squad>();
        cluster->mode = squad_modes[category];
        cluster->category = category;
        auto* first = pending.front();
        pending.erase(pending.begin());
        cluster->members.push_back(first);
        ++cluster->count;
        first->squad = cluster.get();
        for (;;) { // 0x487ed0 refreshes only after each absorption.
            const auto it = std::ranges::find_if(pending, [&](const Agent* a) { return cluster->accepts(*a); });
            if (it == pending.end())
                break;
            (*it)->squad = cluster.get();
            cluster->members.push_back(*it);
            ++cluster->count;
            pending.erase(it);
            update_cluster(*cluster);
        }
        update_cluster(*cluster);
        clusters_[category].push_back(std::move(cluster));
    }
}
std::unique_ptr<SideController::Task> SideController::make_task(Agent* target, int mode) const {
    auto task = std::make_unique<Task>();
    task->target = target;
    task->point = target ? target->position : Vec3{};
    task->mode = mode;
    return task;
}
// 0x48d3b0: before the first task with a lower score.
void SideController::insert(std::vector<std::unique_ptr<Task>>& tasks, std::unique_ptr<Task> task) const {
    const auto it = std::ranges::find_if(tasks, [&](const auto& other) { return task->score > other->score; });
    tasks.insert(it, std::move(task));
}
void SideController::finish(std::vector<std::unique_ptr<Task>>& tasks, float total) const {
    if (total > 0)
        for (auto& task : tasks)
            task->score /= total;
}
float SideController::presence(int category, Vec3 point, float inner, float outer, Strength* record) const {
    double sum = 0;
    for (const auto& squad : squads_[category]) {
        if (!(squad->status & 3))
            continue;
        const double f = falloff(distance(point, squad->mean), inner, outer);
        if (f != 0) {
            if (record)
                record->add(1, squad->strength, static_cast<float>(f));
            sum += squad->souls * f;
        }
    }
    return static_cast<float>(sum / statistics_[category].souls[2]);
}
float SideController::soul_presence(Vec3 point, float inner, float outer) const {
    double sum = 0;
    for (int i = 0; i < 2; ++i) {
        for (const auto& cluster : clusters_[i])
            sum += cluster->count * falloff(distance(point, cluster->mean), inner, outer);
        for (const auto& squad : squads_[i + 2])
            if (squad->souls)
                sum += squad->souls * falloff(distance(point, squad->mean), inner, outer);
    }
    return units_maximum ? static_cast<float>(sum / units_maximum) : 0.0f; // Native divides by +640.
}
float SideController::buildings_near(Vec3 point, int category, std::uint32_t mask, float inner, float outer,
                                     const Agent* skip) const {
    double p = 0;
    for (const auto* agent : buildings_[category])
        if (agent != skip && (agent->flags & mask)) {
            const double f = falloff(distance(agent->position, point), inner, outer);
            p = f * f * (1 - p) + p;
        }
    return static_cast<float>(p);
}
float SideController::friendly_buildings(const Agent& agent) const {
    double p = 0;
    for (int c = 0; c < 2; ++c)
        for (const auto* other : buildings_[c])
            if (other != &agent) {
                const double f =
                    falloff(distance(other->position, agent.position), 10, other->status & 0x4000 ? 1000 : 50);
                p = f * f * (1 - p) + p;
            }
    return static_cast<float>(p);
}
float SideController::site_score(float base, const Agent& agent) const {
    double s = lerp(base, .44999999, soul_presence(agent.position, 10, 280));
    s = lerp(s, .30000001, buildings_near(agent.position, 3, 0x200, 10, 3620, &agent));
    s = lerp(s, .5, buildings_near(agent.position, 0, 0x200, 10, 3620, &agent));
    return static_cast<float>(s);
}
// Planner type 2 (0x48dbf0, mode 2): free sites, enemy manaliths and buildings, enemy wizards and altars.
void SideController::plan_sites() {
    auto& tasks = planners_[0];
    tasks.clear();
    const int total = statistics_[0].manaliths + statistics_[1].manaliths, enemy = statistics_[3].manaliths;
    double expand = 1, raid = 0;
    if (total && (enemy + 1.0) / total >= 1) {
        expand = 1;
        raid = fountains ? static_cast<double>(enemy) / fountains : 0; // Native divides by +644 regardless.
    } else if (fountains) {
        expand = 1 - static_cast<double>(total) / fountains;
        raid = total + enemy ? static_cast<double>(enemy) / (total + enemy) : 0;
    }
    const double share = statistics_[3].ratio[0];
    for (int c = 2; c < 4; ++c)
        for (auto* agent : all_[c]) {
            if (!alive(*agent) || !accepts(*agent->object))
                continue;
            const auto f = agent->flags, st = agent->status;
            int kind = 0;
            double base = 0, weight = 0;
            if ((f & 0x100) && (st & 0x8000)) {
                base = site_score(static_cast<float>(expand), *agent);
                kind = 2;
                weight = (1 - scale) * .8 + .2;
            } else if (c != 3)
                continue;
            else if ((f & 0x20) && ((st & 0x4000) || ((st & 0x40000) && !(f & 0x200)))) {
                if (!(friendly_buildings(*agent) > 0 || total <= 0))
                    continue;
                base = site_score(static_cast<float>(std::max(expand, raid)), *agent) * .99 * scale;
                kind = 1;
                weight = (1 - scale) * .9 + .1;
            } else if ((f & 0x10) && (st & 8)) {
                // An enemy wizard whose home altar is being desecrated is worth a full attack.
                base = (home_desecrated(agent->object) ? 1 : buildings_near(agent->position, 0, 0x20, 10, 180, agent)) *
                       scale;
                kind = 3;
                weight = (1 - scale) * .8 + .2;
            } else if (f & 0x200) {
                if (!(friendly_buildings(*agent) > 0))
                    continue;
                if (desecrating(agent->object)) { // A desecration in progress becomes a building task.
                    base = scale * scale * .40000001;
                    kind = 1;
                    weight = 1 - scale;
                } else {
                    base = scale * scale * std::max((share - .30000001) * 2, 0.0);
                    kind = 9;
                    weight = std::max(1.0 - scale, .4);
                }
            } else
                continue;
            auto task = make_task(agent, 2);
            task->kind = kind;
            if (agent->kind == Kind::unit)
                task->need.add(1, agent->strength, 1);
            const double open = 1 - presence(3, task->point, 10, 140, &task->need);
            task->score = static_cast<float>(lerp(base, weight, open));
            if (task->score != 0)
                insert(tasks, std::move(task));
        }
    double factor = 1, sum = 0;
    for (auto& task : tasks) {
        task->score = static_cast<float>(task->score * factor);
        factor *= .5;
        sum += task->score;
    }
    finish(tasks, static_cast<float>(sum));
}
// Planner type 1 (0x48d560, mode 1): guard the wizard and own mana sources while enemies are near.
void SideController::plan_guards() {
    auto& tasks = planners_[1];
    tasks.clear();
    double sum = 0;
    for (auto* agent : all_[0]) {
        double base;
        if (agent->status & 0xc)
            base = agent->danger * scale * .60000002 + .40000001;
        else if ((agent->flags & 0x20) && (agent->status & 0x1000)) {
            if (agent->flags & 0x200) {
                if (!desecrating(agent->object))
                    continue; // Own altars are guarded only while being desecrated (0x4654e0 'sacu').
                base = 1;
            } else {
                base = lerp(.40000001, .5, buildings_near(agent->position, 0, 0x200, 10, 3620, agent));
                base = lerp(base, .2, buildings_near(agent->position, 3, 0x200, 10, 3620, agent));
                base = lerp(base, .2, soul_presence(agent->position, 10, 160));
            }
        } else
            continue;
        auto task = make_task(agent, 1);
        const double enemies = presence(3, task->point, 10, 3620, &task->need);
        task->score = static_cast<float>(lerp(base, .60000002, enemies));
        if (enemies == 0 || task->score == 0)
            continue;
        sum += task->score;
        insert(tasks, std::move(task));
    }
    finish(tasks, static_cast<float>(sum));
}
void SideController::plan_idle() { // Planner type 0 (0x48e3e0, mode 0): one untargeted task.
    planners_[2].clear();
    planners_[2].push_back(make_task(nullptr, 0));
}
int SideController::join(Task& task, std::vector<Agent*>& available, Agent& agent) {
    std::erase(available, &agent);
    task.assigned.add(1, agent.strength, 1);
    task.members.push_back(&agent);
    agent.task = &task;
    if ((agent.status & 0x20000) && !task.leader)
        task.leader = &agent;
    const auto [first, second] = agent.souls_pair();
    return first + second;
}
namespace {
bool bound(const SideController::Agent& agent) { // Object +1480: ritual-bound units are not assigned.
    const auto* actor = agent.actor();
    return actor && actor->ritual_building;
}
} // namespace
// 0x48e140: the first task the free units can cover takes members up to the soul budget.
void SideController::assign_sites(std::vector<Agent*>& available) {
    if (home_desecrated(side_.wizard())) // 0x472410 +2972: defend instead while the home altar falls.
        return;
    const double budget = statistics_[0].souls[0] * statistics_[3].ratio[0] * .89999998 * scale;
    if (budget == 0)
        return;
    std::uint32_t status = 0;
    Strength pool;
    for (const auto* agent : available)
        if (!bound(*agent)) {
            status |= agent->status;
            pool.add(1, agent->strength, 1);
        }
    for (auto& task : planners_[0]) {
        const auto required = static_cast<std::uint32_t>(task->kind);
        if ((status & required) < required || compare(pool, task->need) < 0)
            continue;
        double assigned = 0;
        for (auto* agent : std::vector<Agent*>(available)) {
            if (assigned > budget)
                break;
            if (!bound(*agent) && (agent->status & 9))
                assigned += join(*task, available, *agent);
        }
        return;
    }
}
// 0x48d870: each guard task takes the units bound to it, then the best-matched units near it.
void SideController::assign_guards(std::vector<Agent*>& available) {
    const double budget = statistics_[0].souls[0];
    double total = 0;
    for (auto& task : planners_[1]) {
        double taken = 0;
        if (task->target && task->target->kind == Kind::building)
            for (auto* agent : std::vector<Agent*>(available))
                if (const auto* actor = agent->actor(); actor && actor->ritual_building == task->target->object) {
                    const int souls = join(*task, available, *agent);
                    taken += souls;
                    total += souls;
                }
        if (total > budget)
            continue;
        while (task->score * budget >= taken + .5) {
            Agent* best = nullptr;
            double value = 0;
            for (auto* agent : available) {
                if (agent == task->target || !(agent->status & 3) || bound(*agent))
                    continue;
                const double d = distance(agent->predicted, task->point);
                const double f = d <= 40 ? 1 : d < 3620 ? (3620 - d) * 0.0002793296 : 0;
                if (f == 0)
                    continue;
                const double c = compare(agent->strength, task->need);
                const double v = c <= 0 ? c / (f * f) : c * (f * f);
                if (!best || value < v)
                    value = v, best = agent;
            }
            if (!best)
                break;
            taken += join(*task, available, *best);
        }
        total += taken;
    }
}
void SideController::assign_idle(std::vector<Agent*>& available) { // 0x48e560.
    if (planners_[2].empty())
        return;
    for (auto* agent : std::vector<Agent*>(available))
        join(*planners_[2].front(), available, *agent);
}
void SideController::run_tasks(int) { // Planner vtable +24 for types 2, 1, 0: 0x48ce30 per task.
    for (auto& planner : planners_)
        for (auto& task : planner)
            if (!task->members.empty())
                execute(*task);
}
// 0x48ce30 with the member statistics of 0x48ea30 (mean of members with status bits 3).
void SideController::execute(Task& task) {
    std::erase_if(task.members, [&](Agent* member) {
        // Agent vtable +32 (0x48a120): valid and not a corpse (object +56, FSM flag 0x4000); wizard
        // spirits stay members.
        const auto* actor = member->actor();
        if (valid(*member) && (!actor || (actor->fsm().state() && !(actor->fsm().state()->flags & 0x4000))))
            return false;
        if (member->task == &task)
            member->task = nullptr;
        if (task.leader == member)
            task.leader = nullptr;
        return true;
    });
    double x = 0, y = 0, z = 0, xx = 0, yy = 0, zz = 0, n = 0;
    for (auto* member : task.members)
        if (member->status & 3) {
            x += member->position.x, y += member->position.y, z += member->position.z;
            xx += static_cast<double>(member->position.x) * member->position.x;
            yy += static_cast<double>(member->position.y) * member->position.y;
            zz += static_cast<double>(member->position.z) * member->position.z;
            ++n;
        }
    task.mean = n ? Vec3{static_cast<float>(x / n), static_cast<float>(y / n), static_cast<float>(z / n)}
                  : Vec3{};
    // 0x48a210 / 0x490e00: preserve an existing chase, follow, move or pickup while regrouping.
    const auto regroup = [&](Agent& member) {
        const int kind = member.actor()->orders.current(true).kind;
        if (spirit(member.object) || kind == 2 || kind == 3 || kind == 6 || kind == 34)
            decide(member, task.mode, &task);
        else
            order_point(member, 2, task.mean, 5);
    };
    // 0x48cff0: a leaderless capture army contracts when moderately scattered, then resumes its task.
    if (!task.leader && (task.mode & 2) && task.members.size() != 1 && n) {
        const double dx = task.point.x - task.mean.x, dy = task.point.y - task.mean.y;
        const double d2 = dx * dx + dy * dy;
        const double spread = std::fabs(xx / n - static_cast<double>(task.mean.x) * task.mean.x) +
                              std::fabs(yy / n - static_cast<double>(task.mean.y) * task.mean.y) +
                              std::fabs(zz / n - static_cast<double>(task.mean.z) * task.mean.z);
        const double radius = 30 * std::min(1 + task.members.size() * .1, 1.799999952316284);
        if (d2 * d2 > 10000 && spread >= radius * radius && spread <= 16 * radius * radius) {
            for (auto* member : task.members)
                if (member->status && member->actor())
                    regroup(*member);
            return;
        }
    }
    const std::vector<Agent*> members = task.members;
    for (auto* member : members) {
        if (!member->status || !member->actor())
            continue;
        if (!task.leader) {
            decide(*member, task.mode, &task);
            continue;
        }
        if (member != task.leader) {
            order_point(*member, 7, task.leader->predicted, 0); // Protect leader.
            continue;
        }
        const double dx = member->position.x - task.mean.x, dy = member->position.y - task.mean.y;
        if (dx * dx + dy * dy <= 10000 || spirit(member->object)) {
            decide(*member, task.mode, &task);
            continue;
        }
        regroup(*member);
    }
}
void SideController::decide(Agent& agent, int mode, Task* task) {
    if (agent.object->type == MatchObject::wizard)
        decide_wizard(agent, mode, task);
    else if (agent.status & 0x10)
        follow(agent, mode, task);
    else
        decide_creature(agent, mode, task);
}
int SideController::decide_creature(Agent& agent, int mode, Task* task) {
    if (visit_mana(agent))
        return 1;
    if (!task)
        return 0; // Error.
    if (mode == 1 && task->target) {
        order_target(agent, 5, *task->target, 0); // Guard.
        return 1;
    }
    if (mode == 2) {
        order_capture(agent, task->point, task->target, (1 - agent.danger) * 160); // Capture.
        return 1;
    }
    return 1; // Idle, or the base 0x489b30.
}
// 0x48a8f0: manahoars follow the most mana-starved member of the neediest own squad.
int SideController::follow(Agent& agent, int mode, Task* task) {
    const Squad* best = nullptr;
    double value = 0;
    for (const auto& squad : squads_[0]) {
        double need = 2.0 * squad->maximum - squad->mana;
        if (squad->status & 8)
            need += need;
        const double d = distance(agent.predicted, squad->center);
        double v = need * (d <= 10 ? 1 : d < 3620 ? (3620 - d) * 0.00027700831 : 0);
        if (squad->sources > 1)
            v /= squad->sources;
        if (value < v)
            value = v, best = squad.get();
    }
    if (!best)
        return decide_creature(agent, mode, task);
    const Agent* target = nullptr;
    int most = 0;
    for (const auto* member : best->members) {
        if (!member->status)
            continue;
        const auto [mana, maximum] = member->mana();
        if (2 * maximum - mana > most)
            most = 2 * maximum - mana, target = member;
    }
    if (!target)
        return decide_creature(agent, mode, task);
    order(agent, 3, target->object, std::nullopt); // Follow.
    return 1;
}
// 0x488400: the own non-wizard, unbound unit with the most souls weighted by distance to the altar
// (10..3620); it leaves its squad.
SideController::Agent* SideController::choose_victim(const Agent& altar) {
    Agent* best = nullptr;
    double most = 0;
    for (auto* unit : units_[0]) {
        if ((unit->object->kind_flags & 1) || bound(*unit))
            continue;
        const double weight = unit->souls_pair().first * falloff(distance(unit->predicted, altar.position), 10, 3620);
        if (most < weight)
            most = weight, best = unit;
    }
    if (best && best->squad) {
        std::erase(best->squad->members, best); // Squad vtable +16.
        best->squad = nullptr;
    }
    return best;
}
int SideController::decide_wizard(Agent& agent, int mode, Task* task) {
    auto* actor = agent.actor();
    if (!actor)
        return 1;
    if (spirit(actor))
        return decide_creature(agent, mode, task);
    const auto unavailable = [&](const Agent* target) {
        return target && (!alive(*target) || !accepts(*target->object));
    };
    if (std::erase_if(agent.plans, [&](const Agent::Plan& plan) {
            return unavailable(plan.target) || (plan.point != plan.target && unavailable(plan.point));
        }))
        agent.plan_clock = agent.clock;
    if (task && unavailable(task->target)) {
        task = nullptr;
        mode = 0;
    }
    if (const void* traced = task ? static_cast<const void*>(task->target) : nullptr;
        trace_ai() && (mode != agent.traced_mode || traced != agent.traced_task)) {
        agent.traced_mode = mode;
        agent.traced_task = traced;
        log(std::format("AI side {} wizard mode {} task kind {} target flags {:#x} status {:#x} at {:.0f},{:.0f}",
                        side_.identity, mode, task ? task->kind : -1,
                        task && task->target ? task->target->flags : 0u,
                        task && task->target ? task->target->status : 0u, task ? task->point.x : 0.f,
                        task ? task->point.y : 0.f));
    }
    if (agent.clock >= agent.plan_clock) { // dword_4d2610.
        agent.react_clock = agent.clock - 1;
        agent.plan_clock = agent.clock + 16;
        const auto weights = plan_weights(mode);
        // 0x48f8d0 moves all unissued plans to the issued/free list in their current order.
        agent.issued_plans.insert(agent.issued_plans.end(), agent.plans.begin(), agent.plans.end());
        agent.plans.clear();
        plan_attack(agent, weights);
        plan_support(agent, weights);
        plan_convert(agent, weights);
        plan_summons(agent, mode == 2 ? task : nullptr);
    }
    if (auto* soul = best_soul(agent, (1 - agent.danger) * 100 + 80, true)) {
        if (!execute_plans(agent, true))
            order_target(agent, 34, *soul, 0); // Spirit.
        return 1;
    }
    if (task && mode == 2 && task->target && agent.clock >= agent.react_clock) {
        agent.react_clock = agent.clock + 8; // dword_4d261c.
        auto& target = *task->target;
        if (target.flags & 0x200) {
            // Desecrate a hostile altar: bring the best own victim (0x488400) to the wizard, then cast.
            const auto* own = actor->allegiance;
            const auto* other = target.object->allegiance;
            if (agent.desecrate.value && own && other &&
                ((own->identity & other->enemies) || (other->identity & own->enemies)) &&
                other->identity != own->identity && !desecrating(target.object))
                if (auto* victim = choose_victim(target);
                    victim && !victim->object->named(FourCC::from_text("sacu")) &&
                    actor->casting.requirements[0] == target.object)
                    if (const auto* definition = match_.spell_registry->find(agent.desecrate);
                        definition && order_target(*victim, 3, agent, definition->range * .9f)) {
                        add_plan(agent, {true, 0, victim, victim, definition->range, definition}); // 0x48fbe0.
                        if (execute_plans(agent, false)) {
                            agent.react_clock += 7 * 8;
                            return 2;
                        }
                    }
        } else if ((target.flags & 0x100) && (target.status & 0x8000) && plan_manalith(agent, target))
            return 2;
    }
    if (execute_plans(agent, false))
        return 1;
    stop_cast_approach(agent);
    return decide_creature(agent, mode, task);
}
// 0x48f800: attack, support and conversion weights from the enemy soul share (+604) and the scale.
std::array<float, 3> SideController::plan_weights(int mode) const {
    const double share = statistics_[3].ratio[0];
    double attack, support, convert;
    if (mode == 2) {
        attack = scale * share * .5;
        support = share * .3333333333333333;
    } else {
        attack = scale * share * .1666666666666667;
        support = share;
    }
    convert = (1 - share) * scale;
    const double sum = attack + support + convert;
    if (sum <= 0)
        return {.33329999f, .33329999f, .33329999f};
    return {static_cast<float>(attack / sum), static_cast<float>(support / sum), static_cast<float>(convert / sum)};
}
// 0x4901a0: prefer the most exposed enemy unit near the wizard.
void SideController::plan_attack(Agent& agent, const std::array<float, 3>& weights) {
    Agent* best = nullptr;
    const SpellDefinition* definition = nullptr;
    double best_weight = 0, total = 0;
    const double reach = (1 - agent.danger) * 280.0;
    for (auto* target : units_[3]) {
        const double d = distance(agent.predicted, target->predicted);
        double w = d <= 10 ? 1 : d < reach ? (reach - d) / (reach - 10) : 0;
        if (w == 0)
            continue;
        if (!(target->status & 6))
            w *= .5;
        if (!(target->status & 0x1800))
            w *= .33333334;
        if (doctor(*target->object))
            w *= 10;
        // Keep native priorities, but consider the next target when the preferred one cannot be cast on.
        if (w > best_weight) {
            const SpellDefinition* candidate = nullptr;
            if (best_spell(agent, *target, 0x58000000, &candidate) > 0) {
                best_weight = w;
                best = target;
                definition = candidate;
            }
        }
        total += w;
    }
    if (best)
        add_plan(agent, {false, static_cast<float>(best_weight * weights[0] / total), best, best,
                         definition->range, definition});
}
// 0x490320: the own unit that gains most from a support spell.
void SideController::plan_support(Agent& agent, const std::array<float, 3>& weights) {
    Agent* best = nullptr;
    const SpellDefinition* chosen = nullptr;
    double best_score = 0, total = 0;
    for (auto* target : units_[0]) {
        if (spirit(target->object))
            continue;
        const SpellDefinition* definition = nullptr;
        const double s = best_spell(agent, *target, 0xa4000000, &definition);
        if (best_score < s)
            best_score = s, best = target, chosen = definition;
        total += s;
    }
    if (total != 0 && best_score != 0 && best) {
        // 0x48c3b0: Guardian approaches the structure in the recipient's task.
        auto* point = best;
        if (chosen->id == FourCC::from_text("grdn") && best->task && best->task->target)
            point = best->task->target;
        add_plan(agent, {false, static_cast<float>(best_score * weights[1] / total), best, point,
                         chosen->range, chosen});
    }
}
// 0x4906d0: convert a hostile soul when the own altar requirement (+2964) holds.
void SideController::plan_convert(Agent& agent, const std::array<float, 3>& weights) {
    const auto* actor = agent.actor();
    // Object +2964 own altar; skipped while it is the home altar (+2972) under desecration.
    if (!actor->casting.requirements[1] || !agent.convert.value ||
        (actor->casting.requirements[1] == actor->home_altar && desecrating(actor->home_altar)))
        return;
    const double range = (1 - statistics_[3].ratio[0]) * (1 - agent.danger) * scale * 500.0;
    if (range <= 10)
        return;
    auto* soul = best_soul(agent, static_cast<float>(range), false);
    if (!soul)
        return;
    if (const auto* owner = soul->object->allegiance ? soul->object->allegiance->wizard() : nullptr) {
        const auto [health, maximum] = health_of(*owner);
        const auto& p = position_of(*owner);
        const double ox = p.x - soul->position.x, oy = p.y - soul->position.y;
        const double sx = actor->motion.position.x - soul->position.x, sy = actor->motion.position.y - soul->position.y;
        if (maximum && static_cast<double>(health) / maximum * (sx * sx + sy * sy) * .5 > ox * ox + oy * oy)
            return;
    }
    if (const auto* definition = match_.spell_registry->find(agent.convert))
        add_plan(agent, {false, static_cast<float>((1 - agent.danger) * weights[2] * .89999998), soul, soul,
                         definition->range, definition});
}
// 0x490410: manahoars while they are few, otherwise the creature that best answers the expected enemy.
void SideController::plan_summons(Agent& agent, Task* task) {
    const auto* actor = agent.actor();
    if (!actor || !actor->maximum_mana || !actor->soul_count)
        return;
    const auto& own = statistics_[0];
    const int sites = own.manaliths + statistics_[1].manaliths;
    bool creatures = agent.danger * 1.3 > match_.random().next() * 0.000030518509 || sites <= 0 || !agent.mano.value;
    if (!creatures) {
        int hoars = 0;
        for (const auto& squad : squads_[0])
            hoars += squad->hoars;
        creatures = hoars >= own.souls[2] / 2 ||
                    (hoars && (2.0 * own.maximum - own.mana) / (static_cast<double>(sites) * hoars) <= 1400);
        if (!creatures) {
            if (const auto* definition = match_.spell_registry->find(agent.mano))
                add_plan(agent, {true, 0, nullptr, nullptr, 0, definition});
            return;
        }
    }
    if (agent.danger >= .6 || !(sites * actor->maximum_mana))
        return;
    const double share = statistics_[3].ratio[0];
    const SpellDefinition* best = nullptr;
    double best_score = 0;
    for (const auto& summon : agent.summons) {
        if (summon.definition->required_souls > actor->soul_count)
            continue;
        Strength reference;
        // Buildings whose +1156 low byte is non-negative (bit 0x80 clear) take the side averages.
        const auto* building = task && task->target ? task->target->building() : nullptr;
        if (!task || (building && (task->target->flags & 0x20) && !(building->building_flags & 0x80))) {
            reference.add(1, friendly_average, static_cast<float>(share));
            reference.add(1, enemy_average, static_cast<float>(1 - share));
        } else
            reference.add(1, task->need, 1);
        const double value = compare(summon.strength, reference);
        const double fraction = summon.definition->mana_cost / (static_cast<double>(sites) * actor->maximum_mana);
        const double score = value <= 0 ? fraction * value : (1 - fraction) * value;
        if (!best || best_score < score)
            best = summon.definition, best_score = score;
    }
    if (best)
        add_plan(agent, {true, 0, nullptr, nullptr, 0, best});
}
bool SideController::plan_manalith(Agent& agent, Agent& site) {
    const auto* actor = agent.actor();
    const auto* definition = agent.manalith.value ? match_.spell_registry->find(agent.manalith) : nullptr;
    if (trace_ai()) {
        const auto reason = FourCC::from_text(!definition                           ? "none"
                                              : agent.danger >= .9                   ? "dngr"
                                              : actor->mana < definition->mana_cost ? "mana"
                                                                                     : "plan");
        if (reason != agent.traced_manalith) {
            agent.traced_manalith = reason;
            log(std::format("AI side {} manalith site {:.0f},{:.0f}: {} (mana {}, danger {:.2f})", side_.identity,
                            site.position.x, site.position.y, reason.text(), actor->mana, agent.danger));
        }
    }
    if (!definition || agent.danger >= .9 || actor->mana < definition->mana_cost)
        return false;
    add_plan(agent, {true, 0, &site, &site, definition->range, definition}); // 0x48fc10.
    return execute_plans(agent, false);
}
// 0x490020: the best matching scorer, discounted by its cost against the manalith-backed mana.
float SideController::best_spell(Agent& agent, Agent& target, std::uint32_t mask,
                                 const SpellDefinition** definition) const {
    if (!alive(target) || !accepts(*target.object))
        return 0;
    const auto* actor = agent.actor();
    const double budget =
        static_cast<double>(actor->maximum_mana) * (statistics_[0].manaliths + statistics_[1].manaliths);
    // 0x490020 supplies a relaxed distance to the scorer and leaves sight checks to 0x48fc40.
    const double scaled_distance = distance_squared(agent.position, target.position) * .71428573;
    const auto scoring_distance = scaled_distance < 1600 ? 0.f : static_cast<float>(scaled_distance);
    double best = 0;
    for (const auto& scorer : agent.scorers) {
        const auto& entry = actor->wizard_spells[scorer.book].entries;
        if (scorer.entry >= static_cast<int>(entry.size()) || (entry[scorer.entry].flags & 2) ||
            !(scorer.definition->spell->flags & mask)) // 0x48c300.
            continue;
        const double range = scorer.definition->range;
        if (scoring_distance > range * range)
            continue;
        double s = score(agent, scorer, target);
        if (budget > 0)
            s *= 1 - scorer.definition->mana_cost / budget;
        if (s > best) {
            best = s;
            *definition = scorer.definition;
        }
    }
    return static_cast<float>(best);
}
// 0x48c330: the 0x48ef50 target check, then the scorer function.
float SideController::score(const Agent& agent, const Agent::Scorer& scorer, const Agent& target) const {
    const auto& definition = *scorer.definition;
    const auto& spell = *definition.spell;
    const auto& object = *target.object;
    if ((spell.targets & 0x781f) && !(spell.targets & 0x781f & object.kind_flags))
        return 0;
    if ((spell.flags & 0x100000) && &object != agent.object)
        return 0;
    const auto* caster = agent.actor();
    if (!caster || !caster->cast_target(definition, &object))
        return 0;
    const auto* side = agent.object->allegiance;
    switch (scorer.function) {
    case nearby_enemies: {
        if (!(target.flags & 0x10))
            return 0;
        const auto* unit = target.actor();
        const double reach = (unit ? unit->stats.current[3] : 0) * spell.duration * 0.0001681802892700975;
        double s = presence(3, target.position, 0, static_cast<float>(reach), nullptr) - target.power;
        return static_cast<float>(target.status & 8 ? s + s : s);
    }
    case healing: {
        if (hostile(side, object.allegiance) || object.named(FourCC::from_text("HEAL")))
            return 0;
        const auto [health, maximum] = health_of(object);
        if (!maximum)
            return 0;
        const int amount = std::min(spell.amount, maximum - health);
        return static_cast<float>(target.power * (static_cast<double>(amount) / maximum));
    }
    case protect: {
        if (!(object.kind_flags & 5) || &object != agent.object || object.named(FourCC::from_text("PROT")) ||
            !health_of(object).second)
            return 0;
        return target.power * target.danger;
    }
    case weaken:
        return (object.kind_flags & 5) && hostile(side, object.allegiance) ? target.power : 0;
    case damage: {
        if (!(object.kind_flags & 0x15) || !hostile(side, object.allegiance))
            return 0;
        const auto [health, maximum] = health_of(object);
        if (!maximum)
            return 0;
        const double s = target.power * (static_cast<double>(std::min(spell.ai_power, health)) / maximum);
        return static_cast<float>(doctor(object) ? s * 1000 : s);
    }
    case area_damage:
    case death: {
        if (!(object.kind_flags & 0x15))
            return 0;
        const double radius = std::max(spell.radius, spell.spread_limit);
        const double r2 = radius * radius;
        double s = target.kind == Kind::unit ? target.strength.total() : 0;
        if (doctor(object))
            s *= 1000;
        for (int c = 0; c < 4; ++c) {
            if (!((1 << c) & 0xb))
                continue;
            for (const auto& squad : squads_[c]) {
                const double x = squad->mean.x - target.position.x, y = squad->mean.y - target.position.y,
                             z = squad->mean.z - target.position.z;
                const double d2 = x * x + y * y + z * z;
                if (d2 > r2 || !squad->health_maximum)
                    continue;
                const double hit = std::min((r2 - d2) * spell.amount / r2, static_cast<double>(squad->health));
                const double part = static_cast<double>(static_cast<std::int64_t>(hit)) / squad->health_maximum * d2;
                s = c == 3 ? s + part : s - part;
            }
        }
        if (scorer.function == death && s != 0) { // 0x48cbd0; raw 0x48cc76 compares squared distance.
            if (target.kind == Kind::unit && (object.kind_flags & 1))
                s -= target.strength.total();
            for (const auto* wizard : units_[3])
                if (wizard->object->kind_flags == 1) {
                    const double x = wizard->position.x - target.position.x,
                                 y = wizard->position.y - target.position.y,
                                 z = wizard->position.z - target.position.z;
                    if (x * x + y * y + z * z <= r2)
                        s -= wizard->strength.total();
                }
        }
        return static_cast<float>(s);
    }
    case guardian: { // 0x48c700
        const auto* unit = target.actor();
        if (!(object.kind_flags & 4) || !unit || unit->ritual_building || !(target.status & 0x10000) ||
            !target.task)
            return 0;
        const auto [flags, targets] = abilities(*unit);
        if (!(flags & 0x40000000) || !(targets & 4))
            return 0;
        const double enemies = presence(3, target.task->point, 10, 280, nullptr);
        return static_cast<float>(target.power * (target.task->score + 1.0) * (enemies + 1) * 4);
    }
    case speed: { // 0x48c920: units walking far to an attack (kind 7) or a collection (kind 34).
        auto* unit = target.actor();
        if (!(object.kind_flags & 5) || !unit)
            return 0;
        const int kind = unit->orders.kind();
        double reach;
        if (kind == 7) {
            if (object.kind_flags == 1 || agent.danger == 0)
                return 0;
            const auto [flags, targets] = abilities(*unit);
            if (flags || targets)
                return 0;
            reach = 60;
        } else if (kind == 34)
            reach = 40;
        else
            return 0;
        const auto order = unit->orders.current();
        const auto goal = order.target ? position_of(*order.target) : order.point;
        const double x = target.position.x - goal.x, y = target.position.y - goal.y, z = target.position.z - goal.z;
        const double d2 = x * x + y * y + z * z;
        return d2 >= 100 ? static_cast<float>(target.power * (d2 / (reach * reach))) : 0;
    }
    case teleport: { // 0x48ca70: an own-side building task target; own squads within twice the spread limit.
        // Agent vtable +56 is non-zero only for building agents (0x4c0c14 -> 0x4bd0b0; others 0x484d30).
        const auto* goal = target.task ? target.task->target : nullptr;
        if (!(object.kind_flags & 1) || !goal || goal->object->allegiance != object.allegiance ||
            goal->kind != Kind::building)
            return 0;
        const double limit = static_cast<double>(spell.spread_limit) * spell.spread_limit * 4;
        double s = 0;
        for (const auto& squad : squads_[0]) {
            const double x = squad->mean.x - target.position.x, y = squad->mean.y - target.position.y,
                         z = squad->mean.z - target.position.z;
            if (x * x + y * y + z * z <= limit)
                s += squad->strength.total();
        }
        return static_cast<float>(s);
    }
    default:
        return 0; // sacc (0x48c7c0) natively scores 0.
    }
}
// 0x48a0b0: the union of the unit's book spell flags (definition +96) and targets (+40).
std::pair<std::uint32_t, std::uint32_t> SideController::abilities(const CreatureActor& unit) const {
    std::uint32_t flags = 0, targets = 0;
    if (match_.spell_registry)
        for (const auto& entry : unit.spells.entries)
            if (const auto* definition = match_.spell_registry->find(entry.id)) {
                targets |= definition->targets;
                if (definition->spell)
                    flags |= definition->spell->flags;
            }
    return {flags, targets};
}
void SideController::add_plan(Agent& agent, Agent::Plan plan) {
    const bool existing = std::ranges::any_of(agent.plans, [&](const Agent::Plan& p) { return p.definition == plan.definition; });
    if (!existing && !agent.issued_plans.empty())
        agent.issued_plans.pop_back(); // 0x48fa60 reuses the tail record before allocating another.
    std::erase_if(agent.plans, [&](const Agent::Plan& p) { return p.definition == plan.definition; });
    auto it = agent.plans.begin();
    for (; it != agent.plans.end(); ++it)
        if ((it->immediate || !plan.immediate) && (!it->immediate || plan.immediate) ? it->score <= plan.score
                                                                                      : !it->immediate)
            break;
    agent.plans.insert(it, plan);
}
// 0x48fc40: move into range, wait while the cast is refused, then issue the wizard spell order. A probe
// (pending soul pickup) still casts but issues no movement and skips immediate targeted plans.
bool SideController::execute_plans(Agent& agent, bool probe) {
    auto* actor = agent.actor();
    const int sites = statistics_[0].manaliths + statistics_[1].manaliths;
    for (auto it = agent.plans.begin(); it != agent.plans.end(); ++it) {
        auto& plan = *it;
        if (plan.immediate && plan.target && probe)
            continue;
        if (!sites && plan.definition->mana_cost > actor->mana) {
            if (plan.immediate)
                return false;
            continue;
        }
        if (plan.point) {
            if (spirit(plan.target ? plan.target->object : nullptr))
                continue;
            if (distance(agent.position, plan.point->position) > plan.range) {
                if (probe)
                    continue;
                order_point(agent, 2, plan.point->position, plan.range); // Spell.
                agent.cast_approach = actor->orders.current(true);
                return true;
            }
        }
        if (const auto check = actor->cast_check(plan.definition->id); check.code) { // Vtable +268.
            if (trace_ai() && plan.definition->kind == 16 && check.reason != agent.traced_manalith) {
                agent.traced_manalith = check.reason;
                log(std::format("AI side {} manalith cast refused: {}", side_.identity, check.reason.text()));
            }
            if (plan.immediate) {
                if (!plan.point)
                    return false;
                if (!probe) {
                    order_target(agent, 3, *plan.point, plan.range);
                    agent.cast_approach = actor->orders.current(true);
                    return true;
                }
            }
            continue;
        }
        // A bound/absorbing or not-yet-grown soul moves closer first (+1080 and FLAG +1100).
        const auto* soul = plan.target ? dynamic_cast<const SoulActor*>(plan.target->object) : nullptr;
        if (soul && plan.point && ((soul->soul_flags & 0xc0000000) || soul->visual.growth < 1)) {
            if (probe)
                continue;
            order_point(agent, 2, plan.point->position, plan.range);
            agent.cast_approach = actor->orders.current(true);
            return true;
        }
        if (plan.point) {
            Vec3 point;
            spell_target_point(point, plan.point->object);
            const auto* data = plan.definition->spell.get();
            const bool objects_clear = !data || !(data->flags & 0x30000) ||
                actor->object_visibility(match_, point, plan.point->object) == 4; // 0x48ed80.
            const auto* own = actor->allegiance;
            const auto* other = plan.target ? plan.target->object->allegiance : nullptr;
            const bool hostile = own && other && own->identity != other->identity &&
                ((own->enemies & other->identity) || (other->enemies & own->identity));
            const bool terrain_clear = !hostile || actor->terrain_visibility(match_, plan.point->object) == 1; // 0x48a6a0.
            if (!objects_clear || !terrain_clear) {
                if (probe)
                    continue;
                order_target(agent, 3, *plan.point, static_cast<float>(distance(agent.position, plan.point->position) * .5));
                agent.cast_approach = actor->orders.current(true);
                return true;
            }
        }
        const auto id = plan.definition->id;
        auto* target = plan.target ? plan.target->object : nullptr;
        agent.issued_plans.push_back(plan);
        agent.plans.erase(it);
        stop_cast_approach(agent);
        if (trace_ai())
            log(std::format("AI side {} casts {} at kind {:#x} {:.0f},{:.0f} (mana {}, souls {})", side_.identity,
                            id.text(), target ? target->kind_flags : 0u,
                            plan.point ? plan.point->position.x : 0.f, plan.point ? plan.point->position.y : 0.f,
                            actor->mana, actor->soul_count));
        actor->issue_spell_order(id, target);
        return true;
    }
    return false;
}
void SideController::stop_cast_approach(Agent& agent) {
    if (!agent.cast_approach)
        return;
    auto& orders = agent.actor()->orders;
    const auto current = orders.current();
    const auto& approach = *agent.cast_approach;
    if (current.kind == approach.kind && current.target == approach.target &&
        distance_squared(current.point, approach.point) == 0)
        orders.pop(true);
    agent.cast_approach.reset();
}
SideController::Agent* SideController::best_soul(const Agent& agent, float range, bool own) const {
    Agent* best = nullptr;
    double value = 0;
    for (const auto& soul : agents_) {
        if (soul->kind != Kind::soul || !(((own ? 5 : 12) >> soul->category()) & 1))
            continue;
        // Unit vtable +68 on the soul's side mask (+1084): pickup needs an eligible soul, conversion
        // (also blocked while a 'sacc' reference is held) an ineligible one.
        const auto* actor = dynamic_cast<const SoulActor*>(soul->object);
        if (!actor || ((actor->eligible_sides & side_.identity) != 0) != own ||
            (!own && agent.object->named(FourCC::from_text("sacc"))))
            continue;
        // 0x488190: do not abandon the current task for a soul in another LAND region.
        if (match_.navigation && !match_.navigation->grid().connected(soul->position, agent.position))
            continue;
        const double d = distance(soul->predicted, agent.position);
        const double v = d > 0 ? (d < range ? (range - d) / range : 0) : 1;
        if (value < v)
            value = v, best = soul.get();
    }
    return best;
}
SideController::Agent* SideController::best_mana(Vec3 point, float range) const {
    Agent* best = nullptr;
    double value = 0;
    for (const auto& agent : agents_) {
        if (agent->category() > 1 || !(agent->status & 0x1000))
            continue;
        if (match_.navigation && !match_.navigation->grid().connected(point, agent->position))
            continue; // 0x4885e0: seek a mana source in the current LAND region.
        const double f = falloff(distance(point, agent->position), 1, range);
        double v = f * 1000;
        if (const auto* building = agent->building())
            v = (building->reserved || building->primary || building->reveal != 1 ||
                 !(building->building_flags & 0xc00)
                     ? 0
                     : building->mana_rate) *
                f;
        if (value < v)
            value = v, best = agent.get();
    }
    return best;
}
int SideController::mana_reserve(const Agent& agent) const {
    const auto* actor = agent.actor();
    if (actor->type != MatchObject::wizard)
        return static_cast<int>(agent.threshold); // 0x48a040.
    if (!home_desecrated(actor) && !agent.plans.empty() && agent.plans.front().definition->kind == 2 &&
        actor->soul_count > agent.plans.front().definition->required_souls)
        return 5 * agent.plans.front().definition->mana_cost / 4;
    return static_cast<int>(agent.threshold); // 0x490ac0: defend a desecrated home instead of saving for summons.
}
bool SideController::visit_mana(Agent& agent) {
    const auto* actor = agent.actor();
    if (!actor || !actor->maximum_mana)
        return false;
    if (actor->mana > mana_reserve(agent) && !spirit(actor))
        return false;
    auto* source = best_mana(agent.position, 3620);
    if (!source)
        return false;
    order(agent, 5, source->object, std::nullopt); // Mana.
    return true;
}
void SideController::order(Agent& agent, int kind, MatchObject* target, std::optional<Vec3> point) {
    auto* actor = agent.actor();
    if (!actor)
        return;
    const auto current = actor->orders.current(true);
    if (current.kind == kind && (!target || current.target == target) &&
        (!point ||
         (current.point.x == point->x && current.point.y == point->y && current.point.z == point->z)))
        return;
    // 0x48a6c0: a guard task at a friendly building makes the unit eligible for Guardian.
    const auto* site = agent.task && agent.task->target ? agent.task->target->building() : nullptr;
    if (kind == 5 && site && site->friendly(actor))
        agent.status |= 0x10000;
    else
        agent.status &= ~0x10000u;
    agent.cast_approach.reset();
    actor->issue_order({kind, target, point}, 0);
}
bool SideController::order_target(Agent& agent, int kind, Agent& target, float range) {
    if (range <= 0 || distance(agent.position, target.position) > range) {
        order(agent, kind, target.object, std::nullopt);
        return false;
    }
    order(agent, kind, nullptr, agent.position);
    return true;
}
bool SideController::order_point(Agent& agent, int kind, Vec3 point, float range) {
    if (distance(agent.position, point) > range) {
        order(agent, kind, nullptr, point);
        return false;
    }
    order(agent, kind, nullptr, agent.position);
    return true;
}
void SideController::order_capture(Agent& agent, Vec3 point, Agent* target, float range) {
    if (distance(agent.position, point) > range) {
        order(agent, 4, nullptr, point);
        return;
    }
    if (const auto* building = target ? target->building() : nullptr) {
        if (!(building->building_flags & 0x80))
            order(agent, 5, target->object, std::nullopt);
        else if (static_cast<int>(building->stage()) <= 1)
            order(agent, 6, target->object, point);
        else
            order(agent, 7, nullptr, point);
        return;
    }
    order(agent, 7, nullptr, point);
}
} // namespace sacrifice
