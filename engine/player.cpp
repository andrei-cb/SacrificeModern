#include "sacrifice/player.hpp"
#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/spell_registry.hpp"
#include "sacrifice/world_save.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace sacrifice {
namespace {
// 0x4cff08: order kind, acknowledgement, marker colour and flags (1 unused here, 2 member markers,
// 4 a leader marker, 0x200 per-slot nearest enemy, 0x400 target-relative formation offsets, 0x800 the
// target's own frame, 0x1000 circles face inward).
struct OrderType {
    int kind;
    FourCC acknowledgement;
    std::uint32_t colour, flags;
};
const FourCC confirm = FourCC::from_text("Xcnf"), attack_reply = FourCC::from_text("Xcat");
const std::array<OrderType, 8> order_types{{{34, confirm, 0x7f7f, 2},
                                             {1, confirm, 0x7f7f7f, 2},
                                             {2, confirm, 0x7f7f7f, 2},
                                             {4, confirm, 0xff, 0x802},
                                             {5, confirm, 0xff, 0xc02},
                                             {3, confirm, 0xff, 0xc02},
                                             {6, attack_reply, 0xff0000, 0x1202},
                                             {7, attack_reply, 0xff0000, 0x1002}}};
const OrderType* order_type(int kind) { // 0x46ccc0.
    for (const auto& type : order_types)
        if (type.kind == kind)
            return &type;
    return nullptr;
}
CreatureActor* creature(MatchObject* object) {
    return object && (object->kind_flags & 5) ? dynamic_cast<CreatureActor*>(object) : nullptr;
}
const CreatureActor* creature(const MatchObject* object) {
    return object && (object->kind_flags & 5) ? dynamic_cast<const CreatureActor*>(object) : nullptr;
}
int rank(const MatchObject* object) {
    const auto* actor = creature(object);
    return actor ? actor->definition().rank : 0;
}
// 0x493890 term: a quarter of the horizontal box diagonal over root two.
double radius(const MatchObject& object) {
    const auto* motion = object.source_motion();
    if (!motion)
        return 0;
    const double x = motion->box_min.x - motion->box_max.x, y = motion->box_min.y - motion->box_max.y;
    return std::sqrt(y * y + x * x) * .35355;
}
double wrap(double angle) { // The 0x46cd20 / 0x46cd90 loops.
    if (angle <= 3.14159)
        for (; angle <= -3.14159; angle += 6.28318)
            ;
    else
        do
            angle -= 6.28318;
        while (angle > 3.14159);
    return angle;
}
double spacing(double radius, double bias, double low, double high) {
    const double value = radius + radius + bias;
    return value > low && value >= high ? high : value <= low ? low : value;
}
Vec3 position(const MatchObject& object) {
    const auto* motion = object.source_motion();
    return motion ? motion->position : Vec3{};
}
bool hostile(const MatchSide* a, const MatchSide* b) {
    return a && b && ((a->enemies & b->identity) || (b->enemies & a->identity)) && a->identity != b->identity;
}
// 0x4018c0: squared distance from a point to an object's box.
double box_distance(Vec3 point, const ObjectMotion& motion) {
    const auto axis = [](float p, float low, float high) {
        return p < low ? low - p : p > high ? high - p : 0.0f;
    };
    const double x = axis(point.x, motion.position.x + motion.box_min.x, motion.position.x + motion.box_max.x);
    const double y = axis(point.y, motion.position.y + motion.box_min.y, motion.position.y + motion.box_max.y);
    const double z = axis(point.z, motion.position.z + motion.box_min.z, motion.position.z + motion.box_max.z);
    return x * x + y * y + z * z;
}
} // namespace

bool PlayerControl::List::contains(const MatchObject* object) const {
    return std::ranges::any_of(entries_, [&](const Entry& e) { return e.object == object; });
}
bool PlayerControl::List::insert(MatchObject* object, int value) {
    for (auto& entry : entries_)
        if (entry.object == object) {
            entry.value = value; // 0x493770 overwrites even an existing node's value.
            return true;
        }
    if (capacity > 0 && static_cast<int>(entries_.size()) >= capacity)
        return false;
    entries_.insert(entries_.begin(), {object, value});
    return true;
}
void PlayerControl::List::add(MatchObject* object, int value) {
    for (auto& entry : entries_)
        if (entry.object == object) {
            entry.value = std::max(entry.value, value);
            return;
        }
    if (capacity > 0 && static_cast<int>(entries_.size()) >= capacity && !entries_.empty()) { // 0x4936b0.
        auto lowest = entries_.begin();
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
            if (it->value < lowest->value)
                lowest = it;
        entries_.erase(lowest);
    }
    insert(object, value);
}
void PlayerControl::List::remove(const MatchObject* object) {
    std::erase_if(entries_, [&](const Entry& e) { return e.object == object; });
}
void PlayerControl::List::clear() {
    entries_.clear();
}
void PlayerControl::List::copy(const List& source) {
    clear();
    capacity = source.capacity; // 0x4935f0.
    for (const auto& entry : source.entries_)
        insert(entry.object, entry.value);
}
std::vector<MatchObject*> PlayerControl::List::objects() const {
    std::vector<MatchObject*> result;
    for (const auto& entry : entries_)
        result.push_back(entry.object);
    return result;
}

PlayerControl::PlayerControl(Match& match, MatchSide& side) : match_(match), side_(side) {}
CreatureActor* PlayerControl::wizard() const {
    return dynamic_cast<CreatureActor*>(side_.wizard());
}
// Creature vtable +72 (0x46ad40); the wizard override (0x489b40) always returns false.
bool PlayerControl::selectable(const MatchObject& object) const {
    const auto* actor = creature(&object);
    const auto* state = actor ? actor->fsm().state() : nullptr;
    return actor && actor->type != MatchObject::wizard && actor->allegiance == &side_ && state &&
           !(actor->status_flags & 0x100) && !(state->flags & 0x2000) &&
           !(object.state_flags & MatchObject::removed_flag);
}
MatchObject* PlayerControl::toggle(MatchObject* object, int mode, MatchObject* leader) {
    if (!object || !creature(object) || !selectable(*object))
        return leader;
    if (mode == 0 || (mode == -1 && selection.contains(object))) {
        selection.remove(object);
        return leader;
    }
    selection.add(object, stamp);
    return !leader || rank(leader) < rank(object) ? object : leader;
}
void PlayerControl::acknowledge(MatchObject* leader, FourCC id, bool repeat) {
    if (!leader)
        return;
    if (repeat && id == FourCC::from_text("Xcnt") && last_leader_ == leader) {
        if (repeats_ >= 3)
            id = FourCC::from_text("Xann");
        else
            ++repeats_;
    } else
        repeats_ = 1;
    last_leader_ = leader;
    feedback[2] = {leader, id};
}
void PlayerControl::voice(FourCC prefix, MatchObject* target, unsigned mask) {
    unsigned classes = 16;
    if (target) {
        if (target->kind_flags & 0x10)
            classes = 17;
        else if (target->kind_flags & 1)
            classes = target != static_cast<MatchObject*>(wizard()) ? 26 : 22;
        else if (target->kind_flags & 5)
            classes = 18;
    }
    const unsigned bits = mask & classes;
    const char letter = bits & 1 ? 'B' : bits & 4 ? 'M' : bits & 8 ? 'W' : bits & 2 ? 'U' : bits & 0x10 ? 'G' : 0;
    if (!letter)
        return;
    prefix.value = (prefix.value & ~0xffu) | static_cast<unsigned char>(letter);
    feedback[1] = {wizard(), prefix};
    feedback[2] = {};
}
std::vector<PlayerControl::SoundRule> PlayerControl::decode_sound_rules(ByteView data) {
    std::vector<SoundRule> rules;
    for (std::size_t at = 4; at + 16 <= data.size(); at += 16) {
        const auto word = [&](std::size_t offset) {
            std::uint32_t value{};
            std::memcpy(&value, data.data() + at + offset, 4);
            return FourCC{value};
        };
        if (!word(0).value)
            break;
        rules.push_back({word(0), word(4), word(8)});
    }
    return rules;
}
void PlayerControl::click(FourCC command, FourCC result) {
    // 0x461160: the first row whose command and result match, either one -1 as a wildcard.
    for (const auto& rule : sound_rules)
        if ((rule.command.value == 0xffffffffu || rule.command == command) &&
            (rule.result.value == 0xffffffffu || rule.result == result)) {
            if (rule.sound != FourCC::from_text("----"))
                interface_sounds.push_back(rule.sound);
            return;
        }
}
void PlayerControl::advise(FourCC id) {
    feedback[0] = {nullptr, id};
    feedback[1] = feedback[2] = {};
}
bool PlayerControl::cancel_target() {
    if (!pending.value)
        return false;
    pending = {};
    click(FourCC::from_text("ctrg"));
    return true;
}
void PlayerControl::mark(Vec3 point, std::uint32_t colour) {
    markers.push_back({point, colour, 0});
}
void PlayerControl::tick(int ticks) {
    for (auto& marker : markers)
        marker.age += ticks * .03333333333333333f;
    std::erase_if(markers, [](const Marker& m) { return m.age >= 1; });
}

bool PlayerControl::select(MatchObject* object) {
    if (!object || !creature(object) || object->allegiance != &side_ || !selectable(*object))
        return false;
    MatchObject* leader = nullptr;
    if (append)
        leader = toggle(object, -1, nullptr);
    else {
        selection.clear();
        leader = toggle(object, 1, nullptr);
    }
    click(FourCC::from_text("ssel"));
    acknowledge(leader, FourCC::from_text("Xcnt"), true);
    return true;
}
// 0x492bc0 with mask 5: the nearest living creature or wizard.
bool PlayerControl::select_near(Vec3 point) {
    MatchObject* best = nullptr;
    double limit = 10000.0 * 10000.0;
    for (const auto& object : match_.objects()) {
        const auto* actor = creature(object.get());
        if (!actor || !actor->alive())
            continue;
        const auto p = actor->motion.position;
        const double x = point.x - p.x, y = point.y - p.y, z = point.z - p.z;
        const double d = z * z + y * y + x * x;
        if (d < limit)
            limit = d, best = object.get();
    }
    return select(best);
}
bool PlayerControl::select_type(MatchObject& object) {
    if (!creature(&object) || object.allegiance != &side_ || !selectable(object))
        return false;
    if (!append)
        selection.clear();
    // 0x461d40 / 0x461ce0: the same ritual binding and, when unbound, the same definition.
    const auto* source = creature(&object);
    const auto origin = source->motion.position;
    MatchObject* leader = nullptr;
    for (const auto& candidate : match_.objects()) {
        const auto* actor = creature(candidate.get());
        if (!actor)
            continue;
        const auto p = actor->motion.position;
        const double x = p.x - origin.x, y = p.y - origin.y, z = p.z - origin.z;
        if (x * x + y * y + z * z > 50.0 * 50.0 || actor->ritual_building != source->ritual_building)
            continue;
        if (source->ritual_building || actor->definition().id == source->definition().id ||
            !source->definition().id.value)
            leader = toggle(candidate.get(), 1, leader);
    }
    click(FourCC::from_text("selt"));
    acknowledge(leader, FourCC::from_text("Xcnt"), false);
    return true;
}
void PlayerControl::begin_box() {
    if (append)
        box_baseline_.copy(selection);
    else
        box_baseline_.clear();
    selecting_box_ = true;
}
void PlayerControl::update_box(std::span<MatchObject* const> visible) {
    if (!selecting_box_)
        return;
    selection.copy(box_baseline_);
    for (auto* object : visible)
        if (object && selectable(*object))
            selection.insert(object, 1); // 0x4813e0 uses insert, not stamped add/eviction.
}
void PlayerControl::end_box() {
    selecting_box_ = false;
    box_baseline_.clear();
}
void PlayerControl::cancel_box() {
    if (!selecting_box_)
        return;
    selection.copy(box_baseline_);
    end_box();
}
void PlayerControl::clear() {
    end_box();
    selection.clear();
    click(FourCC::from_text("selz"));
}
bool PlayerControl::select_group(int group) {
    if (group < 0 || group > 9 || groups[group].empty())
        return false; // 'nvld'.
    click(FourCC::from_text("selg"));
    selection.clear();
    MatchObject* leader = nullptr;
    for (auto* object : groups[group].objects())
        leader = toggle(object, 1, leader);
    FourCC id = FourCC::from_text("grp_");
    id.value = (id.value & ~0xffu) | static_cast<unsigned char>('0' + group);
    feedback[1] = {wizard(), id};
    acknowledge(leader, FourCC::from_text("Xcnt"), false);
    return true;
}
// 0x461e00: joining any group removes the member from group 0 first.
bool PlayerControl::assign_group(int group) {
    if (group < 0 || group > 9 || selection.empty())
        return false;
    groups[group].clear();
    for (auto* object : selection.objects()) {
        groups[0].remove(object);
        groups[group].add(object, stamp);
    }
    groups[group].copy(selection);
    click(FourCC::from_text("assg"));
    FourCC id = FourCC::from_text("asg_");
    id.value = (id.value & ~0xffu) | static_cast<unsigned char>('0' + group);
    feedback[1] = {wizard(), id};
    return true;
}
bool PlayerControl::add_group(int group) {
    if (group < 0 || group > 9 || selection.empty())
        return false;
    for (auto* object : selection.objects()) {
        groups[0].remove(object);
        groups[group].add(object, stamp);
    }
    selection.copy(groups[group]);
    click(FourCC::from_text("assg")); // 'addg' reports as 'assg'.
    FourCC id = FourCC::from_text("asg_");
    id.value = (id.value & ~0xffu) | static_cast<unsigned char>('0' + group);
    feedback[1] = {wizard(), id};
    return true;
}
void PlayerControl::formation(FourCC id) {
    WorldJournal::Scope recording(match_, WorldJournal::Command::formation, {id.value});
    if (selection.empty())
        return;
    for (auto* object : selection.objects())
        if (auto* actor = creature(object))
            actor->orders.mode = id;
    feedback[1] = {wizard(), id};
}
bool PlayerControl::move(MatchObject* target, Vec3 point) {
    if (selection.empty())
        return false;
    voice(FourCC::from_text("mov0"), target, 16);
    click(FourCC::from_text("goto"));
    order(2, target, point);
    return true;
}
bool PlayerControl::guard(MatchObject* target, Vec3 point) {
    if (selection.empty())
        return false;
    voice(FourCC::from_text("grd0"), target, 23);
    click(FourCC::from_text("gurd"));
    order(target ? 5 : 4, target, point);
    return true;
}
bool PlayerControl::attack(MatchObject* target, Vec3 point) {
    if (selection.empty())
        return false;
    voice(FourCC::from_text("att0"), target, 19);
    click(FourCC::from_text("atck"));
    order(target ? 6 : 7, target, point);
    return true;
}
bool PlayerControl::collect(Vec3 point) {
    if (selection.empty())
        return false;
    click(FourCC::from_text("clct"));
    order(34, nullptr, point);
    return true;
}
bool PlayerControl::protect(MatchObject& target) {
    if (selection.empty())
        return false;
    voice(FourCC::from_text("grd0"), &target, 23);
    click(FourCC::from_text("prot"));
    order(5, &target, position(target));
    return true;
}
// 0x46d5f0: each idle-capable, unbound side creature replaces its orders; no acknowledgement.
void PlayerControl::protect_wizard() {
    WorldJournal::Scope recording(match_, WorldJournal::Command::protect_wizard, nlohmann::json::array());
    auto* own = wizard();
    if (!own)
        return;
    voice(FourCC::from_text("grd0"), own, 23);
    click(FourCC::from_text("pwiz"));
    for (const auto& object : match_.objects()) {
        auto* actor = creature(object.get());
        if (!actor || actor->allegiance != &side_ || actor == own || actor->type == MatchObject::wizard ||
            actor->ritual_building)
            continue;
        actor->issue_order({5, own, own->motion.position}, 0);
    }
}
// 0x454790 / 0x45c180: the explicit caster, else each selected creature, casts through its internal
// spell order (kind 65).
bool PlayerControl::cast(FourCC spell, MatchObject* target, std::optional<Vec3> point) {
    WorldJournal::Scope recording(match_, WorldJournal::Command::cast,
                                  {spell.value, WorldJournal::reference(target),
                                   point ? WorldJournal::point(*point) : nlohmann::json{}});
    bool issued = false;
    if (auto* own = wizard(); own && own->find_spell(spell))
        return own->issue_spell_order(spell, target, point);
    for (auto* object : selection.objects())
        if (auto* actor = creature(object); actor && actor->find_spell(spell))
            issued |= actor->issue_spell_order(spell, target, point);
    return issued;
}
bool PlayerControl::spell_key(FourCC spell) {
    WorldJournal::Scope recording(match_, WorldJournal::Command::spell_key, {spell.value});
    auto* own = wizard();
    const auto* definition = match_.spell_registry ? match_.spell_registry->find(spell) : nullptr;
    const auto slot = own ? own->find_spell(spell) : std::nullopt;
    if (pending.value || !definition || !slot)
        return false;
    if (slot->book)
        spell_tab = static_cast<int>(slot->book) - 1;
    selection_spell_ = false;
    return request_spell(*own, *definition);
}
bool PlayerControl::ability_key() {
    WorldJournal::Scope recording(match_, WorldJournal::Command::ability_key, nlohmann::json::array());
    if (pending.value || !match_.spell_registry)
        return false;
    const SpellDefinition* chosen = nullptr;
    CreatureActor* leader = nullptr;
    for (const auto& entry : selection.entries()) {
        auto* actor = creature(entry.object);
        if (!actor || (actor->kind_flags & 1) || actor->ritual_building)
            continue;
        for (const auto& spell : actor->spells.entries) {
            const auto* definition = match_.spell_registry->find(spell.id);
            if (!definition || (definition->spell && (definition->spell->property_flags & 0x1000000)))
                continue;
            if (!leader || leader->definition().rank < actor->definition().rank) {
                leader = actor;
                chosen = definition;
            }
        }
    }
    if (!chosen)
        return false;
    selection_spell_ = true;
    if ((chosen->kind & 0x14) && (chosen->targets & 0x781f)) {
        pending = FourCC::from_text("cast");
        pending_spell = chosen->id;
        pending_mask = chosen->targets & 0x781f;
        pending_range = 0; // A selected group has no single caster's range gate.
        return true;
    }
    bool issued = false;
    for (const auto& entry : selection.entries())
        if (auto* actor = creature(entry.object); actor && actor->find_spell(chosen->id))
            issued |= actor->issue_spell_order(chosen->id, actor, actor->motion.position);
    return issued;
}
bool PlayerControl::cast_slot(int book, int number) {
    WorldJournal::Scope recording(match_, WorldJournal::Command::cast_slot, {book, number});
    auto* own = wizard();
    if (pending.value || !own || book < 0 || book > 2)
        return false;
    spell_tab = book;
    selection_spell_ = false;
    const auto& entries = own->wizard_spells[static_cast<std::size_t>(book)].entries;
    if (number < 1 || static_cast<std::size_t>(number) > entries.size())
        return true;
    const auto* definition =
        match_.spell_registry ? match_.spell_registry->find(entries[static_cast<std::size_t>(number - 1)].id) : nullptr;
    return definition && request_spell(*own, *definition);
}
bool PlayerControl::choose_spell(FourCC spell) {
    const bool same = pending == FourCC::from_text("cast") && pending_spell == spell && !selection_spell_;
    if (cancel_target() && same)
        return false;
    return spell_key(spell);
}
bool PlayerControl::choose_ability() {
    const bool same = pending == FourCC::from_text("cast") && selection_spell_;
    if (cancel_target() && same)
        return false;
    return ability_key();
}
void PlayerControl::forget(const MatchObject& object) {
    selection.remove(&object);
    box_baseline_.remove(&object);
    for (auto& group : groups)
        group.remove(&object);
    if (last_leader_ == &object)
        last_leader_ = nullptr;
}
void PlayerControl::open_tab(unsigned tab) {
    spell_tab = tab > 2 ? (spell_tab + 1) % 3 : static_cast<int>(tab);
}
bool PlayerControl::request_spell(CreatureActor& own, const SpellDefinition& definition) {
    const auto spell = definition.id;
    const auto mask = definition.targets & 0x781f;
    if ((definition.kind & 0x14) && mask) {
        click(FourCC::from_text("seli"), FourCC::from_text("targ")); // 0x454700's selection sounds.
        pending = FourCC::from_text("cast");
        pending_spell = spell;
        pending_mask = mask;
        pending_range = definition.range;
        return true;
    }
    pending = {};
    click(FourCC::from_text("seli"), FourCC::from_text("cast"));
    return own.issue_spell_order(spell, &own, own.motion.position); // 0x454790 on the caster.
}
std::uint32_t PlayerControl::target_mask() const {
    if (pending == FourCC::from_text("cast"))
        return pending_mask;
    if (pending == FourCC::from_text("prot"))
        return 5;
    return 0x4015; // tgot/tatt/tgua: living creatures, structures and land (0x46351c).
}
int PlayerControl::target_check(const MatchObject* target, Vec3 point, bool ground) const {
    const auto mask = target_mask();
    if (target ? !(target->kind_flags & mask) : !ground || !(mask & 0x4000))
        return -1;
    const auto* own = wizard();
    if (pending == FourCC::from_text("cast")) {
        if (own && pending_range > 0) {
            const auto& from = own->motion.position;
            const double x = point.x - from.x, y = point.y - from.y, z = point.z - from.z;
            if (x * x + y * y + z * z > static_cast<double>(pending_range) * pending_range)
                return -2;
        }
        const auto* definition = match_.spell_registry ? match_.spell_registry->find(pending_spell) : nullptr;
        const CreatureActor* caster = own;
        if (selection_spell_)
            for (const auto& entry : selection.entries())
                if (const auto* actor = creature(entry.object); actor && actor->find_spell(pending_spell)) {
                    caster = actor;
                    break;
                }
        if (!caster || !definition)
            return -1;
        try {
            if (!caster->cast_target(*definition, target))
                return -1;
        } catch (const Error&) {
            return -1; // Target rules without a reconstructed host cannot be confirmed by the pointer.
        }
    } else if (const auto* building = dynamic_cast<const BuildingActor*>(target);
               building && !(building->building_flags & 0x55b7))
        return -1;
    return 0;
}
bool PlayerControl::resolve_target(MatchObject* target, Vec3 point) {
    WorldJournal::Scope recording(match_, WorldJournal::Command::resolve_target,
                                  {WorldJournal::reference(target), WorldJournal::point(point)});
    if (!pending.value)
        return false;
    const auto check = target_check(target, point);
    if (check) {
        advise(FourCC::from_text(check == -2 ? "orng" : "invl"));
        return false;
    }
    const auto command = pending;
    pending = {};
    if (command == FourCC::from_text("cast")) {
        auto* own = wizard();
        if (!own && !selection_spell_) {
            pending = command; // 0x461950: -1 'invl', -2 'orng'; the target mode stays.
            advise(FourCC::from_text("invl"));
            return false;
        }
        click(FourCC::from_text("tsel"));
        if (selection_spell_) {
            bool issued = false;
            for (const auto& entry : selection.entries())
                if (auto* actor = creature(entry.object); actor && actor->find_spell(pending_spell))
                    issued |= actor->issue_spell_order(pending_spell, target, point);
            return issued;
        }
        return own->issue_spell_order(pending_spell, target, point);
    }
    click(FourCC::from_text("tsel"));
    if (command == FourCC::from_text("goto"))
        return move(target, point);
    if (command == FourCC::from_text("gurd"))
        return guard(target, point);
    if (command == FourCC::from_text("atck"))
        return attack(target, point);
    if (command == FourCC::from_text("prot") && target && creature(target))
        return protect(*target);
    return false;
}

FourCC PlayerControl::common_formation() const {
    std::vector<std::pair<FourCC, int>> counts;
    std::size_t best = 0;
    for (auto* object : selection.objects()) {
        const auto* actor = creature(object);
        if (!actor)
            continue;
        auto it = std::ranges::find_if(counts, [&](const auto& c) { return c.first == actor->orders.mode; });
        if (it == counts.end())
            it = counts.insert(counts.end(), {actor->orders.mode, 0});
        if (++it->second > (best < counts.size() ? counts[best].second : 0))
            best = static_cast<std::size_t>(it - counts.begin());
    }
    return best < counts.size() ? counts[best].first : FourCC{};
}
std::vector<PlayerControl::Slot> PlayerControl::layout(FourCC formation, int count, float selection_radius,
                                                       const MatchObject* target, unsigned flags) {
    std::vector<Slot> slots;
    const double r = selection_radius, t = target ? radius(*target) : 0;
    const auto add = [&](double x, double y, double yaw = 0) {
        slots.push_back({{static_cast<float>(x), static_cast<float>(y), 0}, static_cast<float>(yaw)});
    };
    const auto id = formation.text();
    int n = count;
    if (id == "flin") { // 0x46c390.
        double size = r, depth = 0;
        int gap = -1;
        if (target) {
            if (flags & 0x800) {
                if (size <= t)
                    size = t;
                gap = ++n >> 1;
            } else
                depth = (t + size) * -2;
        }
        const double step = spacing(size, 1, 3, 10);
        double x = gap == -1 ? -((n - 1) * step) * .5 : -((n >> 1) * step);
        for (int i = 0; i < n; ++i, x += step)
            if (i != gap)
                add(x, depth);
    } else if (id == "fskr") { // 0x46c920.
        const double step = spacing(r, 3, 5, 15) * .7071;
        double depth = 0;
        if (target) {
            depth = (t + step) * -2;
            if (flags & 0x800)
                depth = -depth;
        }
        double x = -((n - 1) * step) * .5;
        for (int i = 0; i < n; ++i, x += step)
            add(x, depth - (i & 1) * step);
    } else if (id == "fpha") { // 0x46c800.
        const double step = spacing(r, 1, 3, 10);
        const double depth = target ? (t + step) * -2 : 0;
        for (int i = 0; i < n; ++i) {
            const int row = i / 4, width = std::min(n - 4 * row, 4);
            add((i & 3) * step - (width - 1) * step * .5, depth - row * step);
        }
    } else if (id == "fsem" || id == "fcir") { // 0x46ca30 / 0x46cb70.
        const bool semicircle = id == "fsem";
        const double step = spacing(r, 1, 3, 10);
        const double least = target ? t + step + step : step;
        double ring = (semicircle ? (n + 2) * step * 2 : n * step) * .1591550775244383;
        if (ring <= least)
            ring = least;
        const double turn = semicircle ? 0 : flags & 0x1000 ? 0 : 3.1415901;
        const double arc = semicircle ? 3.14159 / (n + 2) : 6.28318 / n;
        double angle = semicircle ? arc - 1.570795 : arc * .5;
        for (int i = 0; i < n; ++i, angle += arc)
            add(std::sin(angle) * ring, std::cos(angle) * -ring, semicircle ? angle : angle + turn);
    } else if (id == "fwdj") { // 0x46c6e0.
        const double step = spacing(r, 1, 3, 10);
        double depth = 0;
        if (target) {
            depth = (t + step) * -2;
            if (flags & 0x800)
                depth = -depth;
        }
        const double diagonal = step * .7071;
        double rise = diagonal, y = depth - (n / 2) * diagonal, x = (n / -2) * diagonal;
        for (int i = 0; i < n; ++i, x += diagonal, y += rise) {
            if (i == n / 2)
                rise = -rise;
            add(x, y);
        }
    } else if (id == "ffll" || id == "fflr") { // 0x46c4f0 / 0x46c5f0.
        const double step = spacing(std::max(r, t), 1, 3, 10);
        double x = id == "ffll" ? -(n * step) : step;
        for (int i = 0; i < n; ++i, x += step)
            add(x, 0);
    }
    return slots;
}

// 0x46d370 with the frame 0x46cd90, slot targets 0x46d180 and the greedy assignment 0x46d0b0.
void PlayerControl::order(int kind, MatchObject* target, Vec3 point, unsigned mode) {
    WorldJournal::Scope recording(match_, WorldJournal::Command::order,
                                  {kind, WorldJournal::reference(target), WorldJournal::point(point), mode});
    const auto* type = order_type(kind);
    const auto members = selection.objects();
    if (!type || members.empty())
        return;
    MatchObject* leader = nullptr;
    const auto note = [&](MatchObject* member, Vec3 at) { // 0x46d320.
        if (!leader || rank(leader) < rank(member))
            leader = member;
        if (type->flags & 2)
            mark(at, type->colour);
    };
    const int insertion = append ? 1 : 0;
    const auto form = common_formation();
    double selection_radius = 0;
    for (auto* member : members)
        selection_radius = std::max(selection_radius, radius(*member));
    auto slots = layout(form, static_cast<int>(members.size()), static_cast<float>(selection_radius), target,
                        type->flags);
    if ((mode & 2) && !slots.empty() && members.size() <= 48) {
        for (auto* member : members)
            if (auto* actor = creature(member))
                actor->orders.mode = form;
        // Frame: the target's facing (0x800), the camera for the local side, else centroid to point.
        Vec3 right, forward;
        const auto* target_creature = creature(target);
        if ((type->flags & 0x800) && target_creature) {
            const auto& o = target_creature->motion.orientation;
            right = {o[0], o[1], 0};
            forward = {o[3], o[4], 0};
        } else if (members.front()->allegiance == match_.local_side) {
            right = {camera_right.x, camera_right.y, 0};
            forward = {camera_forward.x, camera_forward.y, 0};
        } else { // 0x410e60 with the world up axis.
            Vec3 centre{};
            for (auto* member : members) {
                const auto p = position(*member);
                centre = {centre.x + p.x, centre.y + p.y, centre.z + p.z};
            }
            const float inverse = 1.0f / static_cast<float>(members.size());
            forward = {point.x - centre.x * inverse, point.y - centre.y * inverse, 0};
            if (forward.x * forward.x + forward.y * forward.y == 0)
                forward = {1, 0, 0};
            right = {forward.y, -forward.x, 0};
        }
        const auto normalise = [](Vec3& v) {
            const double length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            if (length > 0)
                v = {static_cast<float>(v.x / length), static_cast<float>(v.y / length),
                     static_cast<float>(v.z / length)};
        };
        normalise(right);
        normalise(forward);
        const double frame_yaw =
            forward.x * forward.x + forward.y * forward.y == 0 ? 0 : std::atan2(-forward.x, forward.y);
        struct Record {
            Vec3 point;
            float yaw{};
            int member{-2}; // -2 needs a candidate, -1 assigned.
            double score{};
            MatchObject* target{};
        };
        std::vector<Record> records(members.size());
        for (std::size_t i = 0; i < records.size(); ++i) {
            auto& slot = slots[i];
            auto& record = records[i];
            record.point = {point.x + slot.offset.x * right.x + slot.offset.y * forward.x,
                            point.y + slot.offset.x * right.y + slot.offset.y * forward.y, 0};
            if (match_.terrain)
                record.point.z = static_cast<float>(terrain_true_height(*match_.terrain, record.point.x,
                                                                        record.point.y));
            record.yaw = static_cast<float>(wrap(frame_yaw + slot.yaw));
            record.target = target;
            if (target) { // Rewrite the slot as a target-local offset (0x411420).
                if (const auto* motion = target->source_motion()) {
                    const auto& o = motion->orientation;
                    const double x = record.point.x - motion->position.x, y = record.point.y - motion->position.y,
                                 z = record.point.z - motion->position.z;
                    slot.offset = {static_cast<float>(o[0] * x + o[1] * y + o[2] * z),
                                   static_cast<float>(o[3] * x + o[4] * y + o[5] * z), 0};
                    slot.yaw = static_cast<float>(wrap(record.yaw - motion->yaw));
                }
            }
        }
        if ((type->flags & 0x200) && target && (target->kind_flags & 5)) { // 0x46d180.
            const auto* own = members.front()->allegiance;
            for (auto& record : records) {
                const int cx = std::clamp(static_cast<int>(record.point.x * .05), 0, 128);
                const int cy = std::clamp(static_cast<int>(record.point.y * .05), 0, 128);
                double best = 100000000.0;
                for (const auto& object : match_.objects()) {
                    const auto* actor = creature(object.get());
                    if (!actor || !hostile(own, actor->allegiance))
                        continue;
                    const auto& p = actor->motion.position;
                    if (std::clamp(static_cast<int>(p.x * .05), 0, 128) != cx ||
                        std::clamp(static_cast<int>(p.y * .05), 0, 128) != cy)
                        continue;
                    const double d = box_distance(record.point, actor->motion);
                    if (d < best)
                        best = d, record.target = object.get();
                }
            }
        }
        std::vector<int> assigned(members.size(), -1); // Node +12: the member's slot.
        int taken = -2;
        for (std::size_t round = 0; round < records.size(); ++round) {
            int chosen = -1;
            for (std::size_t k = 0; k < records.size(); ++k) {
                auto& record = records[k];
                if (record.member == -1)
                    continue;
                if (record.member == taken) { // 0x46d040 / 0x46d000.
                    record.score = 1;
                    record.member = -2;
                    for (std::size_t m = 0; m < members.size(); ++m) {
                        if (assigned[m] != -1)
                            continue;
                        const auto p = position(*members[m]);
                        const double x = p.y - record.point.y, y = p.x - record.point.x;
                        const double value = -std::sqrt(x * x + y * y);
                        if (record.member == -2 || value > record.score)
                            record.score = value, record.member = static_cast<int>(m);
                    }
                }
                if (chosen < 0 || record.score > records[chosen].score)
                    chosen = static_cast<int>(k);
            }
            if (chosen < 0)
                break;
            taken = records[chosen].member;
            if (taken >= 0)
                assigned[taken] = chosen;
            records[chosen].member = -1;
        }
        for (std::size_t m = 0; m < members.size(); ++m) {
            auto* actor = creature(members[m]);
            const int k = assigned[m];
            if (!actor || k < 0)
                continue;
            const auto& record = records[k];
            CreatureOrders::Input input{kind, record.target, record.point, record.yaw, {}};
            if (type->flags & 0x400)
                input.formation = CreatureOrders::Formation{slots[k].offset, slots[k].yaw};
            if (actor->issue_order(input, insertion) && (mode & 1))
                note(members[m], record.point);
        }
    } else {
        for (auto* member : members)
            if (auto* actor = creature(member)) {
                if (mode & 1)
                    note(member, point);
                actor->issue_order({kind, target, point}, insertion);
            }
    }
    if (leader && (mode & 1)) {
        if (type->flags & 4)
            mark(point, type->colour);
        acknowledge(leader, type->acknowledgement, false);
    }
}
} // namespace sacrifice
