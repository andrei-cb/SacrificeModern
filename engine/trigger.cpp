#include "sacrifice/trigger.hpp"
#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/match.hpp"
#include "sacrifice/motion.hpp"
#include "sacrifice/scene.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/side_ai.hpp"
#include "sacrifice/soul_actor.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_combat.hpp"
#include "sacrifice/spell_registry.hpp"
#include "sacrifice/wizard_control.hpp"
#include "sacrifice/world_save.hpp"
#include <cmath>
#include <map>

namespace sacrifice {
namespace tc = trigger_code;
namespace {
struct Unsupported {
    std::uint32_t code;
};
// 0x476cf0: a script error is logged and clears instance flag 1, so the instance is dropped.
struct Disabled {};
// 0x47d050 compares the prefix with strnicmp, then skips spaces and tabs.
bool prefixed(std::string_view name, std::string_view prefix) {
    return name.size() >= prefix.size() &&
           std::ranges::equal(name.substr(0, prefix.size()), prefix, [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           });
}
std::string unprefixed(const std::string& name, std::size_t length) {
    const auto start = name.find_first_not_of(" \t", length);
    return start == std::string::npos ? std::string{} : name.substr(start);
}
// 0x477af0.
bool compare(std::uint32_t comparison, std::int32_t count, std::int32_t value, std::int32_t maximum = 0) {
    switch (comparison) {
    case tc::ConstraintNone:
        return count == 0;
    case tc::ConstraintAtLeast:
        return count >= value;
    case tc::ConstraintAtMost:
        return count <= value;
    case tc::ConstraintExactly:
        return count == value;
    case tc::ConstraintAll:
        return count == maximum; // 0x477af0 compares the pair of native count words.
    case tc::ConstraintTrue:
        return count != 0;
    case tc::ConstraintFalse:
        return count == 0;
    default:
        return false; // 22 compares a second word that counts leave zero.
    }
}
// Creature vtable +104 -> +100 / 0x469540: relink side membership/souls/controller and clear orders.
void transfer_side(MatchObject& object, MatchSide& side, bool cinematic) {
    side.attach(object);
    if (auto* actor = dynamic_cast<CreatureActor*>(&object)) {
        actor->orders.clear();
        if (cinematic)
            actor->issue_order({CreatureOrders::cinematic_marker, actor});
    }
    log(std::format("Script side transfer: object {} to side {}", object.script_id, side.definition.index));
}
// SIDE +176 / +184, recomputed from the live membership lists (0x472080).
int population(const MatchSide& side, bool structures) {
    if (!structures) {
        int count = static_cast<int>(side.objects[1].size());
        for (const auto* object : side.objects[0]) count += object->kind_flags != 8;
        return count;
    }
    int count = 0;
    for (const auto* object : side.objects[2]) {
        const auto* building = dynamic_cast<const BuildingActor*>(object);
        count += building && (!building->primary || building->primary == building);
    }
    return count;
}
bool enemies(const MatchSide& a, const MatchSide& b) {
    return ((a.enemies & b.identity) || (b.enemies & a.identity)) && a.identity != b.identity;
}
bool allies(const MatchSide& a, const MatchSide& b) {
    return (a.allies & b.identity) || (a.identity & b.allies) || a.identity == b.identity;
}
bool stock_skirmish_result(const TriggerDefinition& definition) {
    if (definition.sides != 3 || definition.flags != (tc::FlagEnabled | tc::FlagRunOnce) ||
        definition.action.size() != 1 || definition.condition.size() != 9)
        return false;
    const auto action = definition.action.front();
    if (action != tc::ActionEndMissionInVictory && action != tc::ActionEndMissionInDefeat)
        return false;
    const auto type = definition.condition.back();
    const bool altar = type == tc::EntityTypeAltars;
    if (!altar && type != tc::EntityTypeCreatures && type != tc::EntityTypeWizards &&
        type != tc::EntityTypeWizardsAndCreatures)
        return false;
    const std::array<std::uint32_t, 9> expected{
        tc::ConditionIfSideSpecCommandsNtts,
        action == tc::ActionEndMissionInVictory ? tc::SideSpecEnemySides : tc::SideSpecCurrentSide,
        altar ? tc::NttsQuantityStructureType : tc::NttsQuantityCreatureType,
        tc::ConstraintAtMost, tc::QuantityInteger, tc::ValueInteger, 0,
        altar ? tc::ValueStructureType : tc::ValueCreatureType, type};
    return std::ranges::equal(definition.condition, expected);
}
} // namespace

std::vector<TriggerDefinition> TriggerDefinition::decode(ByteView bytes, std::uint32_t side_override) {
    Reader r(bytes, "TRIG");
    const auto count = r.u32();
    std::vector<TriggerDefinition> result;
    const auto words = [&](std::uint32_t size) {
        if (size % 4)
            throw Error("Invalid TRIG code size");
        std::vector<std::uint32_t> code(size / 4);
        for (auto& word : code)
            word = r.u32();
        return code;
    };
    for (std::uint32_t i = 0; i < count; ++i) {
        TriggerDefinition definition;
        definition.flags = r.u32();
        definition.sides = r.u32();
        if (definition.sides != 0x80000000 && side_override)
            definition.sides = side_override;
        const auto condition = r.u32(), action = r.u32();
        definition.condition = words(condition);
        definition.action = words(action);
        result.push_back(std::move(definition));
    }
    return result;
}

Triggers::Triggers(std::vector<TriggerDefinition> definitions, MatchSides& sides,
                   std::span<const std::string> variables, std::vector<TriggerMarker> markers)
    : sides_(sides), markers_(std::move(markers)) {
    for (auto& definition : definitions) {
        const auto mask = definition.sides;
        triggers_.push_back({std::move(definition), mask, {}});
    }
    for (const auto& name : variables) {
        Variable variable{name, name};
        if (prefixed(name, "[SIDE]")) {
            variable.name = unprefixed(name, 6);
            variable.flags |= 4;
        }
        variables_.push_back(std::move(variable));
    }
}
void Triggers::bind_presentation(const ResourceCatalog& catalog) {
    text = [&catalog](FourCC id) {
        if (const auto record = catalog.find(FourCC::from_text("TEXT"), id)) {
            const auto bytes = catalog.read(*record);
            return std::string(reinterpret_cast<const char*>(bytes.data()), strnlen(
                reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        }
        return std::string{};
    };
    sample_seconds = [&catalog, cache = std::map<FourCC, float>{}](FourCC id) mutable {
        if (const auto found = cache.find(id); found != cache.end())
            return found->second;
        float seconds = 0;
        if (const auto record = catalog.find(FourCC::from_text("SAMP"), id)) {
            const auto sample = decode_sample(catalog.read(*record));
            auto size = sample.audio.size();
            // Embedded RIFF's fact chunk owns the decoded frame count for compressed map samples.
            if (sample.wave_container) {
                Reader r(sample.audio, "dialogue WAVE");
                r.take(12);
                size = 0;
                while (r.remaining() >= 8) {
                    const auto tag = r.fixed_string(4);
                    const auto length = r.u32();
                    const auto chunk = r.take(length);
                    if (tag == "fact" && length >= 4) {
                        Reader fact(chunk, "dialogue fact");
                        seconds = static_cast<float>(fact.u32()) / sample.sample_rate;
                    } else if (tag == "data")
                        size = length;
                    if ((length & 1) && r.remaining())
                        r.take(1);
                }
            }
            if (seconds == 0 && sample.byte_rate)
                seconds = static_cast<float>(size) / sample.byte_rate;
        }
        cache[id] = seconds;
        return seconds;
    };
}
void Triggers::seed_camera(Vec3 eye, Vec3 aim) {
    if (!cinematic_) {
        camera_eye.point = camera_eye.start = camera_eye.target = eye;
        camera_aim.point = camera_aim.start = camera_aim.target = aim;
        camera_eye.object = camera_aim.object = 0;
    }
}
void Triggers::seed_zoom(float zoom, float target) {
    if (!cinematic_) {
        camera_zoom = zoom;
        camera_zoom_target = target;
    }
}
void Triggers::move_camera(CameraTrack& track, const Location& where, unsigned ticks) {
    track.start = track.point;
    track.elapsed = 0;
    track.duration = static_cast<float>(ticks);
    track.object = where.object ? where.object->reference() : 0;
    if (where.marker) {
        track.target = where.marker->position;
        track.target.z += where.height;
    } else if (where.object)
        track.target = camera_object(*where.object, &track == &camera_eye);
    if (!ticks)
        track.point = track.target;
}
Vec3 Triggers::camera_object(const MatchObject& object, bool eye) const {
    const auto* motion = object.source_motion();
    if (!motion)
        return eye ? camera_eye.target : camera_aim.target;
    const auto* actor = dynamic_cast<const CreatureActor*>(&object);
    if (eye && match_->terrain) {
        // 0x47f9e0 (mode flag 0x40): behind the object along the yaw from it to the aim, pitch 0 (0x410920).
        FollowCamera view;
        view.zoom = camera_zoom;
        const auto dx = camera_aim.point.x - motion->position.x, dy = camera_aim.point.y - motion->position.y;
        const Vec2 angles{dx == 0 && dy == 0 ? 0.0f : -std::atan2(dx, dy), 0};
        if (actor)
            view.place(*actor, *match_->terrain, angles);
        else // 0x47fca0 without a SAXS model: the model bounds give the distance and height.
            view.place(motion->position,
                       {std::hypot(motion->box_min.x, motion->box_min.y) + 1, (motion->box_max.z - motion->box_min.z) * .9f},
                       *match_->terrain, angles);
        return view.position;
    }
    if (!eye && actor && actor->camera_offset().y > 0) {
        auto point = motion->position;
        point.z += actor->camera_offset().y * .5f; // 0x47f950.
        return point;
    }
    // 0x47f950 otherwise aims at 0x44e020: the collision boxes' centre, or the visual bounds' centre.
    auto point = motion->position;
    spell_target_point(point, &object);
    return point;
}
std::uint32_t Triggers::camera_target() const {
    // The view's followed object (+124): the local wizard, dword_4dabb0.
    const auto* wizard = match_ && match_->local_side ? match_->local_side->wizard() : nullptr;
    return wizard ? wizard->reference() : 0;
}
void Triggers::dismiss_dialogue(bool force) {
    const auto recording =
        match_ ? std::make_unique<WorldJournal::Scope>(*match_, WorldJournal::Command::dismiss,
                                                       nlohmann::json::array({force}))
               : nullptr;
    if (!dialogue.empty() && dialogue.front().started) {
        auto& line = dialogue.front();
        if (force || line.ending == 2 || line.ending == 3)
            line.remaining = 0;
    }
}
void Triggers::answer_choice(unsigned answer) {
    const auto recording = match_
                               ? std::make_unique<WorldJournal::Scope>(*match_, WorldJournal::Command::choice,
                                                                       nlohmann::json::array({answer}))
                               : nullptr;
    if (choices.empty() || answer < 1 || answer > 2)
        return;
    const auto choice = choices.front();
    choices.pop_front();
    for (auto& trigger : triggers_)
        for (auto& instance : trigger.instances)
            if (instance.choice == choice.id) {
                value(variables_.at(choice.variable), instance) = static_cast<std::int32_t>(answer);
                instance.flags &= ~tc::FlagSuspended;
                instance.choice = 0;
            }
}
std::string Triggers::speaker_name(const Dialogue& line) const {
    static constexpr const char* gods[]{"Persephone", "James", "Charnel", "Stratos", "Pyro"};
    if (line.god >= 0 && line.god < static_cast<int>(std::size(gods)))
        return gods[line.god];
    return line.name.value && text ? text(line.name) : std::string{};
}
void Triggers::advance_presentation() {
    const auto now = match_->clock().match;
    const auto ticks = now - presentation_time_;
    presentation_time_ = now;
    const auto approach = [](float value, float target, float step) {
        return value < target ? std::min(value + step, target) : std::max(value - step, target);
    };
    fade.alpha = approach(fade.alpha, fade.target, fade.rate * ticks);
    if (game_timer > 0) {
        game_timer = std::max<std::int32_t>(0, game_timer - static_cast<std::int32_t>(ticks));
        if (game_timer == 0) timer_visible = false;
    }
    for (auto& beacon : beacons) {
        if (auto* target = match_->resolve(beacon.object)) {
            if (match_->local_side) match_->local_side->see(*target);
        } else beacon.target = 0;
        beacon.alpha = approach(beacon.alpha, beacon.target, ticks / 60.0f);
        beacon.rotation += ticks * .02617991666666667f;
    }
    std::erase_if(beacons, [](const Beacon& beacon) { return beacon.target == 0 && beacon.alpha == 0; });
    camera_zoom = approach(camera_zoom, camera_zoom_target, .03f * ticks);
    for (auto* track : {&camera_eye, &camera_aim}) {
        if (const auto* object = match_->resolve(track->object))
            track->target = camera_object(*object, track == &camera_eye);
        track->elapsed += ticks;
        const float progress = track->duration > 0 ? std::min(track->elapsed / track->duration, 1.0f) : 1;
        // 0x47f390 eases both tracks by 0x43a240: the cubic Bezier over 0, 0.1, 0.9, 1 (0x4d2400) with the
        // basis at 0x4caf80, -1.4t^3 + 2.1t^2 + 0.3t.
        const float t = progress >= 1 ? 1 : ((-1.4f * progress + 2.1f) * progress + .3f) * progress;
        track->point = {track->start.x + (track->target.x - track->start.x) * t,
                        track->start.y + (track->target.y - track->start.y) * t,
                        track->start.z + (track->target.z - track->start.z) * t};
    }
    // Mode flag 0x10 then keeps the eye half a unit over the land (0x44df50 with 0x44ecd0), so markers at
    // or below ground height (ch, et, py01 and pe01 use heights 0, -5 and -10) stay above it.
    if (match_->terrain) {
        auto& eye = camera_eye.point;
        const float floor = terrain_height(*match_->terrain, eye.x, eye.y, true) + .5f;
        if (eye.z < floor)
            eye.z = floor;
    }
    const auto advance = [&](std::deque<Dialogue>& queue, bool parallel = false) {
        for (auto it = queue.begin(); it != queue.end();) {
            auto& line = *it;
            if (!line.started) {
                line.started = true;
                const auto seconds = line.sample.value && sample_seconds ? sample_seconds(line.sample) : 0;
                line.remaining = seconds > 0 ? seconds * 60 : static_cast<float>(6 * (line.caption.size() + 10));
                if (line.ending == 1)
                    line.remaining = std::max(0.0f, line.remaining - line.time);
                else if (line.ending == 4)
                    line.remaining = static_cast<float>(line.time);
                if (auto* speaker = dynamic_cast<CreatureActor*>(match_->resolve(line.speaker))) {
                    // 0x473a70: release scripted hold and enter the stock Talking FSM animation.
                    speaker->state_flags = (speaker->state_flags & ~0x100000u) | 0x40u;
                    speaker->fsm().event(59);
                }
                if (!parallel)
                    return;
                ++it;
                continue;
            }
            if (line.ending != 2 && line.ending != 3)
                line.remaining -= ticks;
            if (line.remaining <= 0) {
                const auto id = line.id;
                // Talking-sequence destruction (0x4734b0 -> 0x474940), including silent credits.
                // Independent samples/creature voices do not create talking-sequence history nodes.
                if (&queue == &dialogue)
                    journal.push_back({line.text, line.sample, speaker_name(line).substr(0, 63),
                                       line.caption.substr(0, 1023)});
                if (auto* speaker = match_->resolve(line.speaker))
                    speaker->state_flags &= ~0x40u; // 0x4737e0.
                it = queue.erase(it);
                for (auto& trigger : triggers_)
                    for (auto& instance : trigger.instances)
                        if (instance.dialogue == id) {
                            instance.flags &= ~tc::FlagSuspended;
                            instance.dialogue = 0;
                        }
            } else
                ++it;
            if (!parallel)
                return;
        }
    };
    advance(dialogue);
    advance(voices, true);
    advance(sounds, true);
}
void Triggers::bind_campaign(const std::function<std::int32_t(std::string_view)>& flag) {
    for (auto& variable : variables_)
        if (prefixed(variable.raw, "[CAMPAIGN]")) {
            variable.name = unprefixed(variable.raw, 10);
            variable.flags |= 2;
            variable.value = flag(variable.name);
        }
}
std::vector<std::pair<std::string, std::int32_t>> Triggers::campaign_values() const {
    std::vector<std::pair<std::string, std::int32_t>> result;
    for (const auto& variable : variables_)
        if (variable.flags & 2)
            result.emplace_back(variable.name, variable.value);
    return result;
}
std::vector<std::tuple<std::size_t, std::string, std::int32_t>> Triggers::variable_values(int side) const {
    std::vector<std::tuple<std::size_t, std::string, std::int32_t>> result;
    for (std::size_t id = 0; id < variables_.size(); ++id) {
        const auto& v = variables_[id];
        const auto value = (v.flags & 4) && side >= 0 && side < 32 ? v.sides[static_cast<std::size_t>(side)] : v.value;
        if (value)
            result.emplace_back(id, v.name, value);
    }
    return result;
}
Triggers::Variable& Triggers::variable(Instance& instance) {
    if (next(instance) != tc::ValueVariable)
        throw Error("Invalid variable code in trigger");
    const auto id = next(instance);
    if (id >= variables_.size())
        throw Error("Missing variable id " + std::to_string(id));
    return variables_[id];
}
std::int32_t& Triggers::value(Variable& variable, const Instance& instance) {
    if (!(variable.flags & 4))
        return variable.value;
    const auto* side = sides_.get(instance.side);
    return variable.sides[side ? side->definition.index & 31 : 0];
}
const TriggerMarker& Triggers::marker(Instance& instance) {
    if (next(instance) != tc::ValueMarkerId)
        throw Disabled{}; // "Invalid location code"
    const auto id = next(instance);
    // 0x47de40 hashes by the low six bits and walks newest first; ids are unique in stock maps.
    for (auto it = markers_.rbegin(); it != markers_.rend(); ++it)
        if (it->id == id)
            return *it;
    throw Error("Error reading marker " + std::to_string(id));
}
Triggers::Location Triggers::location(Instance& instance, int mode) {
    Location result{mode};
    if (mode == 1) {
        switch (next(instance)) {
        case tc::LocationStructure: // A building, within 25.
            result.object = object(instance, nullptr, 16);
            result.radius = 25;
            break;
        case tc::LocationCreature: // A wizard, creature or corpse, within 10.
            result.object = object(instance, nullptr, 13);
            result.radius = 10;
            break;
        case tc::LocationMarker:
            result.mode = 2;
            result.marker = &marker(instance);
            break;
        case tc::LocationMarkerAtHeight: // A marker and a number the region test does not read.
            result.mode = 2;
            result.marker = &marker(instance);
            result.height = static_cast<float>(number(instance));
            break;
        default:
            throw Disabled{};
        }
        if (result.mode == 1 && !result.object)
            throw Disabled{};
    } else if (mode == 2) { // A distance, then any object.
        result.mode = 1;
        result.radius = static_cast<float>(number(instance));
        result.object = object(instance, nullptr, 29);
        if (!result.object)
            throw Disabled{};
    } else if (mode != 0)
        throw Disabled{};
    return result;
}
bool Triggers::inside(const MatchObject& object, const Location& location) {
    if (location.mode == 0)
        return true;
    const auto* motion = object.source_motion();
    if (!motion)
        return false;
    const auto& p = motion->position;
    if (location.mode == 1) {
        if (location.radius < 0)
            return true;
        const auto* other = location.object->source_motion();
        if (!other)
            return false;
        // 0x4028e0: squared xy distance from the position to the other footprint grown by this one.
        const auto& o = other->position;
        const float low_x = o.x + other->box_min.x - motion->box_max.x;
        const float low_y = other->box_min.y + o.y - motion->box_max.y;
        const float high_x = other->box_max.x + o.x - motion->box_min.x;
        const float high_y = other->box_max.y + o.y - motion->box_min.y;
        const double dx = p.x < low_x ? low_x - p.x : p.x > high_x ? high_x - p.x : 0.0;
        const double dy = p.y < low_y ? low_y - p.y : p.y > high_y ? high_y - p.y : 0.0;
        return static_cast<double>(location.radius) * location.radius > dx * dx + dy * dy;
    }
    const auto& m = *location.marker;
    const double dx = static_cast<double>(p.x) - m.position.x, dy = static_cast<double>(p.y) - m.position.y;
    if (m.shape == 0x4000000)
        return static_cast<double>(m.extent.x) * m.extent.x > dx * dx + dy * dy;
    if (m.shape != 0x8000000)
        return false;
    // 0x410bc0 builds the rotation by -angle; 0x411900 multiplies the offset as a row vector.
    const auto angle = static_cast<float>(-m.angle * 0.01745327777777778);
    const double c = std::cos(angle), s = std::sin(angle);
    const double x = dx * c - dy * s, y = dx * s + dy * c;
    return std::abs(m.extent.x) >= std::abs(x) && std::abs(m.extent.y) >= std::abs(y);
}
Triggers::Order Triggers::order(Instance& instance) {
    Order result;
    const auto code = next(instance);
    // 0x4782b0: an object location targets the object at its position, a marker only its point.
    const auto destination = [&](int at_object, int at_marker) {
        const auto where = location(instance, 1);
        if (where.mode == 1) {
            result.kind = at_object;
            result.target = where.object;
            if (const auto* motion = where.object->source_motion())
                result.point = motion->position;
        } else {
            result.kind = at_marker;
            result.point = where.marker->position;
        }
    };
    switch (code) {
    case tc::OrderCast: { // 0x478350: kind 65, SPEL at +32, then a cast subcode.
        result.kind = 65;
        const auto sub = next(instance);
        switch (sub) {
        case tc::SpellSpecSpellAtNtt:
            result.spell = spell(instance);
            result.target = object(instance, nullptr, 29);
            // Documented repair: st02's Acheron casts 'sacc' (a soul-only target mask) at Surtur's corpse,
            // and the mission's doctor triggers (T0, T47) need that cast to start. SacSpellStart already
            // resolves a corpse to its soul; resolve it here too, before cast validation (0x450390) sees
            // a corpse against a soul-only mask.
            if (result.target && (result.target->kind_flags & 8) && result.spell) {
                const auto* data = result.spell->kind == 4 ? result.spell->spell.get() : nullptr;
                const auto mask = data ? data->targets : result.spell->targets;
                if ((mask & 2) && !(mask & 8))
                    match_->visit_group(3, 2, [&](MatchObject& candidate) {
                        if (auto* soul = dynamic_cast<SoulActor*>(&candidate); soul && soul->anchor == result.target) {
                            result.target = soul;
                            return -1;
                        }
                        return 0;
                    });
            }
            if (result.target && result.target->source_motion())
                result.point = result.target->source_motion()->position;
            break;
        case tc::SpellSpecSpellAtLocation: {
            result.spell = spell(instance);
            const auto where = location(instance, 1);
            if (where.mode == 1) {
                result.target = where.object;
                if (const auto* motion = where.object->source_motion())
                    result.point = motion->position;
            } else
                result.point = where.marker->position;
            break;
        }
        case tc::SpellSpecSpell:
        case tc::SpellSpecCreatureSpell:
            result.spell = spell(instance);
            break;
        case tc::SpellSpecSpellAtSoulOfNtt:
            result.spell = spell(instance);
            result.target = object(instance, nullptr, 12);
            if (result.target && result.target->source_motion())
                result.point = result.target->source_motion()->position;
            else {
                result.spell = nullptr;
                result.target = nullptr;
            }
            break;
        default:
            throw Disabled{}; // "Invalid cast code"
        }
        break;
    }
    case tc::OrderPassiveNotify: // Hold: no order, state bit 0x100000 set.
        result.kind = -1;
        result.set = result.clear = 0x100000;
        break;
    case tc::OrderNotify: // Follow a named wizard or creature.
        result.kind = 3;
        result.target = object(instance, nullptr, 5);
        if (!result.target)
            return {-1}; // 0x478350 fails quietly: no order and no state change.
        if (const auto* motion = result.target->source_motion())
            result.point = motion->position;
        result.set = result.clear = 0x100000;
        break;
    case tc::OrderCancelOrders: // Stop, releasing a hold.
        result.clear = 0x100000;
        break;
    case tc::OrderTurnToFaceLocation:
        destination(35, 36);
        break;
    case tc::OrderDie: // Die.
        result.kind = 37;
        break;
    case tc::OrderAttack:
        result.target = object(instance, nullptr, 21);
        result.kind = result.target ? 6 : 0;
        break;
    case tc::OrderGoToLocation:
    case tc::OrderGoToLocationInFormation:
        destination(3, 2);
        break;
    case tc::OrderAttackLocation:
    case tc::OrderAttackLocationInFormation:
        destination(7, 7);
        break;
    case tc::OrderGuardLocation:
    case tc::OrderGuardLocationInFormation:
        destination(5, 4);
        break;
    default:
        throw Disabled{}; // "Invalid order"
    }
    if (code >= tc::OrderGoToLocationInFormation && code <= tc::OrderGuardLocationInFormation)
        next(instance); // A formation code (270..277, 'fline'...) the issue path does not read.
    return result;
}
void Triggers::apply(MatchObject& object, const Order& order) {
    object.state_flags = (object.state_flags & ~order.clear) | order.set;
    auto* creature = dynamic_cast<CreatureActor*>(&object);
    if (order.kind == -1 || !creature)
        return;
    if (order.kind == 37) {
        creature->kill(*match_);
        return;
    }
    if (order.kind == 65 && order.spell) {
        // 0x478a70 → 0x45c180: store the book slot and queue kind 65. Missing book entries are
        // inserted so start_cast can find the definition the script named.
        if (!creature->find_spell(order.spell->id)) {
            if (creature->type == MatchObject::wizard && match_->spell_registry)
                match_->spell_registry->add_to_books(creature->wizard_spells, order.spell->id, true);
            if (!creature->find_spell(order.spell->id))
                creature->spells.add(order.spell->id, order.spell->priority, true);
        }
        auto point = order.point;
        if (!order.target && point.x == 0 && point.y == 0 && point.z == 0)
            point = creature->motion.position;
        creature->issue_spell_order(order.spell->id, order.target, point);
        return;
    }
    creature->issue_order({order.kind, order.target, order.point});
}
MatchObject* Triggers::object(Instance& instance, MatchSide* owner, std::uint32_t mask) {
    const auto code = next(instance);
    bool accepted = false;
    switch (code) {
    case tc::ValueCreatureId:
        accepted = mask & 0xd;
        break;
    case tc::ValueStructureId:
        accepted = mask & 0x10;
        break;
    case tc::ValueNttId:
        accepted = mask & 0x1d;
        break;
    case tc::ValueWizardId:
        accepted = mask & 1;
        break;
    default:
        throw Error("Invalid ntt id code in trigger");
    }
    if (!accepted)
        return nullptr; // The id word stays unread, as in the original.
    const auto id = next(instance);
    if (id == tc::EntityCurrentWizard) {
        const auto* side = sides_.get(instance.side);
        return side ? side->wizard() : nullptr;
    }
    // dword_4DABB0 is the object the player controls; while unset 0x457c30 answers the local side's
    // wizard, and possession awaits the player controller.
    if (id == tc::EntitySingleplayerWizard)
        return match_->local_side ? match_->local_side->wizard() : nullptr;
    static constexpr std::array<unsigned, 5> group_masks{4, 1, 16, 130, 96}; // 0x4c0468.
    // 0x491e50 walks the whole side and keeps the last match.
    const auto search = [&](MatchSide& side) -> MatchObject* {
        MatchObject* found = nullptr;
        for (unsigned group = 0; group < group_masks.size(); ++group)
            if (mask & group_masks[group])
                for (auto* candidate : side.objects[group])
                    if ((candidate->kind_flags & mask) && candidate->script_id == id)
                        found = candidate;
        return found;
    };
    if (owner)
        return search(*owner);
    for (auto* side : match_->side_order)
        if (auto* found = search(*side))
            return found;
    return nullptr;
}
const SpellDefinition* Triggers::spell(Instance& instance) {
    const auto code = next(instance);
    if (code != tc::ValueSpell && code != tc::ValueAutoSpell)
        throw Disabled{}; // "Invalid spell code"
    const FourCC id{next(instance)};
    const auto* definition = match_->spell_registry ? match_->spell_registry->find(id) : nullptr;
    if (!definition)
        throw Disabled{}; // "Cannot find spell"
    return definition;
}
void Triggers::start(std::uint32_t skirmish_sides) {
    const bool solo_skirmish = skirmish_sides && (skirmish_sides & (skirmish_sides - 1)) == 0;
    for (auto& trigger : triggers_) {
        // The stock result template is assigned only to sides 0 and 1, even on larger maps.
        const bool stock_result = stock_skirmish_result(trigger.definition);
        if (solo_skirmish && stock_result && trigger.definition.action.front() == tc::ActionEndMissionInVictory) {
            trigger.pending = 0;
            continue;
        }
        const bool expanded = skirmish_sides && stock_result;
        if (expanded)
            trigger.pending = skirmish_sides;
        for (int i = 0; i < 32; ++i)
            if (trigger.pending & (1u << i)) {
                trigger.instances.push_back({i, trigger.definition.flags | tc::FlagEnabled,
                                             expanded ? skirmish_sides : 0});
                trigger.pending &= ~(1u << i);
            }
    }
}
std::size_t Triggers::instances() const {
    std::size_t count = 0;
    for (const auto& trigger : triggers_)
        count += trigger.instances.size();
    return count;
}
void Triggers::tick(Match& match) {
    match_ = &match;
    advance_presentation();
    for (auto& trigger : triggers_)
        run(trigger);
}
// 0x47cd10.
void Triggers::run(Trigger& trigger) {
    const auto time = match_->clock().match;
    for (auto it = trigger.instances.begin(); it != trigger.instances.end();) {
        auto& instance = *it;
        auto* side = sides_.get(instance.side);
        bool remove = false;
        std::size_t step = 0; // Start of the instruction being run, for error reports.
        try {
            if (!side)
                throw Error("Trigger instance without a side");
            if (!(instance.flags & tc::FlagConditionsMet)) {
                if (time < instance.wait) {
                    ++it;
                    continue;
                }
                instance.code = &trigger.definition.condition;
                instance.pc = 0;
                instance.wait = 0;
                instance.locals = {};
                bool met = false;
                if (instance.flags & tc::FlagEnabled) { // 0x47cc50.
                    met = true;
                    while ((step = instance.pc) != instance.code->size())
                        if (!condition(*side, instance) || !(instance.flags & tc::FlagEnabled)) {
                            met = false;
                            break;
                        }
                }
                if (!met) {
                    if (instance.flags & tc::FlagEnabled)
                        instance.wait = time + 30;
                    else
                        remove = true;
                } else {
                    instance.flags |= tc::FlagConditionsMet;
                    instance.code = &trigger.definition.action;
                    instance.pc = 0;
                    instance.wait = 0;
                    instance.locals = {};
                }
            }
            if (!remove && (instance.flags & tc::FlagConditionsMet) && !(instance.flags & tc::FlagSuspended)) {
                // 0x47b7b0: actions sleep until their wait time and run until one yields.
                bool finished = true;
                if (time < instance.wait)
                    finished = false;
                else
                    while ((step = instance.pc) != instance.code->size() && (instance.flags & tc::FlagEnabled))
                        if (!action(*side, instance) || (instance.flags & tc::FlagSuspended)) {
                            finished = false;
                            break;
                        }
                if (finished) {
                    if ((instance.flags & tc::FlagRunOnce) || !(instance.flags & tc::FlagEnabled))
                        remove = true;
                    else {
                        instance.flags &= ~tc::FlagConditionsMet;
                        instance.wait = time;
                    }
                }
            }
        } catch (const Unsupported& code) {
            unsupported_.insert(code.code);
            log(std::format("Unsupported trigger instruction {}: trigger {}, side {}, word {}", code.code,
                &trigger - triggers_.data(), instance.side, instance.pc));
            remove = true;
        } catch (const Disabled&) {
            log(std::format("Trigger script error: trigger {}, side {}, stream {}, word {}, instruction {} at word {}",
                &trigger - triggers_.data(), instance.side,
                instance.code == &trigger.definition.action ? "action" : "condition", instance.pc,
                step < instance.code->size() ? (*instance.code)[step] : 0, step));
            remove = true;
        }
        if (remove)
            it = trigger.instances.erase(it);
        else
            ++it;
    }
}
std::uint32_t Triggers::next(Instance& instance) {
    if (instance.pc >= instance.code->size())
        throw Error("Trigger code overrun");
    return (*instance.code)[instance.pc++];
}
bool Triggers::condition(MatchSide& side, Instance& instance) {
    const auto code = next(instance);
    switch (code) {
    case tc::ConditionIfSideSpecCommandsNtts:
        return count_condition(side, instance, 0);
    case tc::ConditionIfSideSpecCommandsNttsAtLocation:
        return count_condition(side, instance, 1);
    case tc::ConditionIfElapsedGameTimeIsFrames: {
        std::int32_t value{};
        const auto op = comparison(instance, value);
        return compare(op, static_cast<std::int32_t>(match_->clock().match), value);
    }
    case tc::ConditionIfGameTimerIsFrames: {
        std::int32_t limit{};
        const auto op = comparison(instance, limit);
        return compare(op, game_timer, limit);
    }
    case tc::ConditionIfCurrentSideIsAnAiSide:
        return side.controller != nullptr;
    case tc::ConditionIfCurrentSideIsAPlayerSide: {
        const auto* wizard = dynamic_cast<CreatureActor*>(side.wizard());
        return wizard && !wizard->computer_wizard;
    }
    case tc::ConditionIfPlayerHasCreaturesSelected: {
        const auto list = object_list(instance);
        return is_selected && !list.empty() && std::ranges::all_of(list, [&](const auto* target) {
            return target && is_selected(*target);
        });
    }
    case tc::ConditionIfCreatureHasOrders: {
        auto* target = dynamic_cast<CreatureActor*>(object(instance, nullptr, 5));
        const auto given = order(instance);
        if (!target) return false;
        const auto current = target->orders.current();
        return current.kind == given.kind && ((current.target && current.target == given.target) ||
            (current.point.x == given.point.x && current.point.y == given.point.y && current.point.z == given.point.z));
    }
    case tc::ConditionIfStructureActiveValueCondition: {
        auto* target = dynamic_cast<BuildingActor*>(object(instance, nullptr, 16));
        std::int32_t limit{};
        const auto op = comparison(instance, limit);
        if (!target) return false;
        if (target->primary) target = target->primary;
        return compare(op, target->completed, limit);
    }
    case tc::ConditionIfCreatureHasStat: {
        const auto* target = dynamic_cast<CreatureActor*>(object(instance, nullptr, 5));
        std::int32_t limit{};
        const auto op = comparison(instance, limit), kind = next(instance);
        return target && (op == tc::ConstraintAll || compare(op, stat(*target, kind), limit));
    }
    case tc::ConditionIfSideSpecSeesNtts: {
        const auto selector = next(instance);
        const auto* chosen = selector == tc::SideSpecSide ? this->side(instance) : &side;
        const auto filter = this->filter(instance);
        const auto sees = [&](const MatchSide& observer) {
            if (filter.code == tc::NttsCreature || filter.code == tc::NttsStructure)
                return filter.object && filter.object->allegiance &&
                    count(*filter.object->allegiance, filter, {}, nullptr, nullptr, nullptr, &observer);
            int total = 0;
            for (const auto& candidate : sides_.instances()) {
                int subtotal = 0;
                count(*candidate, filter, {}, nullptr, &subtotal, nullptr, &observer);
                total += subtotal;
            }
            int maximum = 0;
            for (const auto& candidate : sides_.instances()) maximum += population(*candidate, filter.code == tc::NttsQuantityStructureType);
            return compare(filter.comparison, total, filter.value, maximum);
        };
        if (selector == tc::SideSpecSide || selector == tc::SideSpecCurrentSide)
            return chosen && sees(*chosen);
        if (selector < tc::SideSpecEnemySides || selector > tc::SideSpecNeutralSides) throw Disabled{};
        for (const auto& other : sides_.instances()) {
            const bool related = selector == tc::SideSpecEnemySides ? enemies(side, *other) :
                selector == tc::SideSpecFriendlySides ? allies(side, *other) : !enemies(side, *other) && !allies(side, *other);
            if (related && !sees(*other)) return false;
        }
        return true;
    }
    case tc::ConditionAlways:
        return true;
    case tc::ConditionIfNttSeesNtts: { // 0x47cc30: count objects with an unobstructed terrain ray from the observer.
        const auto* observer = object(instance, nullptr, 21);
        const auto selected = filter(instance);
        if (!observer)
            return false;
        if (selected.code == tc::NttsCreature || selected.code == tc::NttsStructure)
            return selected.object && selected.object->allegiance &&
                count(*selected.object->allegiance, selected, {}, observer);
        // Native null-side counts span all sides, then compare the combined total once.
        int total = 0;
        for (const auto& candidate : sides_.instances()) {
            int side_total = 0;
            count(*candidate, selected, {}, observer, &side_total);
            total += side_total;
        }
        int maximum = 0;
        for (const auto& candidate : sides_.instances()) maximum += population(*candidate, selected.code == tc::NttsQuantityStructureType);
        return compare(selected.comparison, total, selected.value, maximum);
    }
    case tc::ConditionIfSideSpecIsAttackedBySide: { // 0x47c1c0: a side has received damage from the selected side(s).
        // Side and current side name the receiver and then read the attacker's side id; the
        // enemy/friendly/neutral specs test the current side against every related side.
        const auto selector = next(instance);
        if (selector == tc::SideSpecCurrentSide) {
            const auto* attacker = this->side(instance);
            return attacker && (side.incoming_mask & attacker->identity);
        }
        const auto* receiver = selector == tc::SideSpecSide ? this->side(instance) : &side;
        return mask_condition(side, instance, receiver ? receiver->incoming_mask : 0, selector);
    }
    case tc::ConditionIfNttHasResource: { // 0x47c330: current/max mana (130), health (131), or wizard experience (132).
        auto* target = object(instance, nullptr, 29);
        std::int32_t limit{};
        const auto op = comparison(instance, limit), stat = next(instance);
        if (!target)
            return false;
        std::int32_t current = 0, maximum = 0;
        if (auto* actor = dynamic_cast<CreatureActor*>(target)) {
            if (stat == tc::ResourceMana) { current = actor->mana; maximum = actor->maximum_mana; }
            else if (stat == tc::ResourceHealth) { current = actor->stats.current[5]; maximum = actor->stats.base[5]; }
            else if (stat == tc::ResourceExperience && actor->kind_flags == MatchObject::wizard)
                current = actor->progression.experience;
            else throw Disabled{};
        } else if (auto* building = dynamic_cast<BuildingActor*>(target); building && stat == tc::ResourceHealth) {
            current = building->health; maximum = building->maximum_health;
        } else throw Disabled{};
        return op == tc::ConstraintAll ? current == maximum : compare(op, current, limit);
    }
    case tc::ConditionIfCreatureIsAttackedBySideSpec: { // +2896 friendly-exception mask (also the conversion/ownership checks in ja01).
        const auto* target = dynamic_cast<CreatureActor*>(object(instance, &side, 5));
        const auto selector = next(instance);
        // Consume a literal side even when the named actor is missing.
        return mask_condition(side, instance, target ? target->friendly_exception_mask : 0, selector);
    }
    case tc::ConditionIfVariableCondition: {
        auto& entry = variable(instance);
        std::int32_t value{};
        const auto op = comparison(instance, value);
        return compare(op, this->value(entry, instance), value);
    }
    case tc::ConditionIfNttExists:
        return object(instance, nullptr, 29) != nullptr;
    case tc::ConditionIfNttExistsOnTheCurrentSide:
        return object(instance, &side, 29) != nullptr;
    case tc::ConditionIfCreatureIsAlive: { // 0x47c330: positive current health and the creature's vtable +52 alive test.
        const auto* actor = dynamic_cast<CreatureActor*>(object(instance, nullptr, 5));
        return actor && actor->stats.current[CreatureStats::health] > 0 && actor->alive();
    }
    case tc::ConditionIfSideSpecDoesNotCommandNtts:
        return !count_condition(side, instance, 0);
    case tc::ConditionIfSideSpecDoesNotCommandNttsAtLocation:
        return !count_condition(side, instance, 1);
    case tc::ConditionOr: // Native or: a true first operand leaves the second unread.
        return condition(side, instance) || condition(side, instance);
    case tc::ConditionAnd: {
        const bool first = condition(side, instance);
        const bool second = condition(side, instance);
        return first && second;
    }
    case tc::ConditionNot:
        return !condition(side, instance);
    case tc::ConditionIfSideSpecCommandsNttsWithinMetersOfNtt:
        return count_condition(side, instance, 2);
    default:
        throw Unsupported{code};
    }
}
void Triggers::state(MatchObject* target, std::uint32_t status, bool on) {
    std::uint32_t bit = 0;
    switch (status) {
    case tc::NttStateRescuable: bit = 8; break;
    case tc::NttStateCannotGib: bit = 1; break;
    case tc::NttStateCannotDamage: bit = 4; break;
    case tc::NttStateCannotDestroy: bit = 2; break;
    case tc::NttStateWalking: bit = 32; break;
    case tc::NttStateCollectsRedSouls: bit = 64; break;
    case tc::NttStateNotOnMinimap: bit = 256; break;
    case tc::NttStateDestroyed:
        if (on) {
            if (auto* actor = dynamic_cast<CreatureActor*>(target)) actor->kill(*match_);
            else if (auto* building = dynamic_cast<BuildingActor*>(target))
                building->destroy(*match_);
        }
        break;
    case tc::NttStateDamaged:
        if (auto* actor = dynamic_cast<CreatureActor*>(target)) {
            auto& stats = actor->stats;
            stats.current[5] = stats.target[5] = on ? stats.base[5] / 10 : stats.base[5];
            if (on)
                stats.changed |= 32;
        } else if (auto* building = dynamic_cast<BuildingActor*>(target); building && on)
            building->health = building->maximum_health / 10;
        break;
    default: throw Disabled{};
    }
    if (target && bit)
        target->common_status = on ? target->common_status | bit : target->common_status & ~bit;
}
std::uint32_t Triggers::resource_id(Instance& instance, std::uint32_t tag) {
    if (next(instance) != tag) throw Disabled{};
    return next(instance);
}
std::uint32_t Triggers::ntt_type(Instance& instance) {
    const auto tag = next(instance);
    if (tag != tc::ValueCreatureType && tag != tc::ValueStructureType && tag != tc::ValueNttType &&
        tag != tc::ValueText && tag != tc::ValueFilename) throw Disabled{};
    return next(instance);
}
std::vector<MatchObject*> Triggers::object_list(Instance& instance) {
    std::vector<MatchObject*> result;
    std::uint32_t link;
    do {
        link = next(instance);
        if (link != tc::ListOneElement && link != tc::ListMoreElements) throw Disabled{};
        auto* target = object(instance, nullptr, 5);
        if (std::ranges::find(result, target) == result.end()) result.push_back(target);
    } while (link == tc::ListMoreElements);
    return result;
}
std::vector<MatchObject*> Triggers::quantified(MatchSide& side, Instance& instance) {
    const auto mode = next(instance);
    if (mode < tc::QuantifiedNttCreatureType || mode > tc::QuantifiedNttStructureTypeAtLocation) throw Disabled{};
    const auto type = ntt_type(instance);
    const auto where = location(instance, mode >= tc::QuantifiedNttCreatureTypeAtLocation ? 1 : 0);
    std::vector<MatchObject*> result;
    count(side, {tc::NttsQuantityCreatureType, tc::ConstraintTrue, type}, where, nullptr, nullptr, &result);
    return result;
}
std::int32_t Triggers::stat(const CreatureActor& target, std::uint32_t stat) {
    if (stat >= tc::CreatureStatSpeed && stat <= tc::CreatureStatResistanceToSplashRanged)
        return target.stats.base[stat - tc::CreatureStatSpeed + 3];
    if (stat == tc::CreatureStatMaxMana) return target.maximum_mana;
    if (stat == tc::CreatureStatKills) return target.credit_count_80;
    if (stat == tc::CreatureStatGibs) return target.credit_count_20;
    throw Disabled{};
}
std::int32_t Triggers::resource(const MatchObject& target, std::uint32_t resource) {
    if (const auto* actor = dynamic_cast<const CreatureActor*>(&target)) {
        if (resource == tc::ResourceMana) return actor->mana;
        if (resource == tc::ResourceHealth) return actor->stats.current[CreatureStats::health];
        if (resource == tc::ResourceExperience && (actor->kind_flags & 5)) return actor->progression.experience;
    } else if (const auto* building = dynamic_cast<const BuildingActor*>(&target); building && resource == tc::ResourceHealth)
        return building->health;
    return 0;
}
void Triggers::give_resource(MatchObject& target, std::uint32_t resource, std::int32_t amount) {
    auto* actor = dynamic_cast<CreatureActor*>(&target);
    if (!actor || !(actor->kind_flags & 5)) return;
    if (resource == tc::ResourceMana) {
        auto fixed = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(amount) << 16);
        actor->change_mana_fixed(fixed); // Applies mana affinity once, then the shared 16.16 fraction.
    } else if (resource == tc::ResourceHealth) actor->heal(*match_, amount, 256);
    else if (resource == tc::ResourceSouls && actor->kind_flags == MatchObject::wizard) actor->change_souls(amount);
}
void Triggers::teleport(MatchObject& target, const Location* where) {
    auto* actor = dynamic_cast<CreatureActor*>(&target);
    if (!actor) return;
    if (!where) {
        SpellActor::launch_teleport_burst(*match_, *actor, true);
        actor->state_flags |= MatchObject::removed_flag;
        return;
    }
    const auto* beside = where->object ? where->object->source_motion() : nullptr;
    if (!beside && !where->marker) return;
    const auto centre = beside ? beside->position : where->marker->position;
    auto point = centre;
    if (open_point(*match_, point, centre, 20, 31)) actor->relocate(*match_, point);
}
std::vector<Triggers::DisplayValue> Triggers::display_values() const {
    std::vector<DisplayValue> result;
    const auto* local = match_ ? match_->local_side : nullptr;
    if (!local) return result;
    for (const auto& variable : variables_)
        if (variable.displayed & local->identity)
            result.push_back({variable.name, variable.flags & 4 ? variable.sides[local->definition.index & 31] : variable.value});
    return result;
}
bool Triggers::action(MatchSide& side, Instance& instance) {
    const auto code = next(instance);
    switch (code) {
    case tc::ActionDisplayText:
        resource_id(instance, tc::ValueText);
        return false; // 0x479390: obsolete native instruction consumes TEXT but creates no display.
    case tc::ActionClearText: {
        for (const auto& line : dialogue) {
            if (auto* speaker = match_->resolve(line.speaker)) speaker->state_flags &= ~0x40u;
            for (auto& trigger : triggers_) for (auto& owner : trigger.instances)
                if (owner.dialogue == line.id) { owner.dialogue = 0; owner.flags &= ~tc::FlagSuspended; }
        }
        dialogue.clear();
        return false;
    }
    case tc::ActionPlaySample: {
        Dialogue sound;
        sound.id = ++dialogue_serial_;
        sound.sample = FourCC{resource_id(instance, tc::ValueSample)};
        instance.dialogue = sound.id;
        instance.flags |= tc::FlagSuspended;
        sounds.push_back(sound);
        return false;
    }
    case tc::ActionCreateCreatureTypeAtLocation:
    case tc::ActionCreateCreatureTypeOnSideAtLocation:
    case tc::ActionCreateMultipleCreatureTypeOnSideAtLocation: {
        const auto quantity = code == tc::ActionCreateMultipleCreatureTypeOnSideAtLocation ? number(instance) : 1;
        const FourCC type{ntt_type(instance)};
        auto* owner = code == tc::ActionCreateCreatureTypeAtLocation ? &side : this->side(instance);
        const auto where = location(instance, 1);
        const auto* motion = where.object ? where.object->source_motion() : nullptr;
        if (!owner || (!motion && !where.marker) || !match_->create_script_creature) return false;
        const auto centre = motion ? motion->position : where.marker->position;
        const auto orientation = motion ? motion->orientation : motion_orientation(where.marker->angle);
        for (int n = 0; n < quantity; ++n) {
            auto point = centre;
            const auto* definition = match_->creature_definition ? match_->creature_definition(type) : nullptr;
            if (match_->prepare_creature) match_->prepare_creature(type);
            if (!definition && match_->creature_definition) definition = match_->creature_definition(type);
            if (!definition || definition->kind != 2) throw Disabled{};
            if (!find_creature_point(*match_, point, centre, orientation, true) ||
                !match_->create_script_creature(*owner, type, point)) throw Disabled{};
        }
        return true;
    }
    case tc::ActionOrderList: {
        const auto targets = object_list(instance);
        const auto given = order(instance);
        for (auto* target : targets) if (target) apply(*target, given);
        return true;
    }
    case tc::ActionTeleportAllOfTypeAtLocationToLocation:
    case tc::ActionTeleportAllOfTypeAtLocationAway: {
        const auto type = ntt_type(instance);
        const auto from = location(instance, 1);
        const auto to = code == tc::ActionTeleportAllOfTypeAtLocationToLocation ? location(instance, 1) : Location{};
        std::vector<MatchObject*> targets;
        count(side, {tc::NttsQuantityCreatureType, tc::ConstraintTrue, type}, from, nullptr, nullptr, &targets);
        for (auto* target : targets) teleport(*target, code == tc::ActionTeleportAllOfTypeAtLocationToLocation ? &to : nullptr);
        return false;
    }
    case tc::ActionSetGameTimer:
        game_timer = number(instance);
        return true;
    case tc::ActionDisplayGameTimer:
        timer_visible = true;
        return true;
    case tc::ActionHideGameTimer:
        timer_visible = false;
        return true;
    case tc::ActionGiveCreatureResource: {
        auto* target = object(instance, nullptr, 5); // Native grants search every side, unlike orders and stat changes.
        const auto amount = number(instance);
        const auto kind = next(instance);
        if (target) give_resource(*target, kind, amount);
        return false;
    }
    case tc::ActionGiveAllOfTypeAtLocationResource: {
        const auto type = ntt_type(instance);
        const auto where = location(instance, 1);
        const auto amount = number(instance);
        const auto kind = next(instance);
        std::vector<MatchObject*> targets;
        count(side, {tc::NttsQuantityCreatureType, tc::ConstraintTrue, type}, where, nullptr, nullptr, &targets);
        for (auto* target : targets) give_resource(*target, kind, amount);
        return false;
    }
    case tc::ActionSetVariableToResourceOfNtt: {
        auto& entry = variable(instance);
        const auto kind = next(instance);
        const auto* target = object(instance, &side, 13);
        if (target) value(entry, instance) = resource(*target, kind);
        return true;
    }
    case tc::ActionSetVariableToResourceOfNtts: {
        auto& entry = variable(instance);
        const auto kind = next(instance);
        const auto targets = quantified(side, instance);
        std::uint32_t sum = 0;
        for (const auto* target : targets) sum += static_cast<std::uint32_t>(resource(*target, kind));
        value(entry, instance) = std::bit_cast<std::int32_t>(sum);
        return false;
    }
    case tc::ActionSetAllOfTypeState:
    case tc::ActionSetAllOfTypeAtLocationState: {
        const auto type = ntt_type(instance);
        const auto where = location(instance, code == tc::ActionSetAllOfTypeAtLocationState ? 1 : 0);
        const auto status = next(instance), setting = next(instance);
        if (setting != tc::SwitchOn && setting != tc::SwitchOff) throw Disabled{};
        std::vector<MatchObject*> targets;
        count(side, {tc::NttsQuantityCreatureType, tc::ConstraintTrue, type}, where, nullptr, nullptr, &targets);
        for (auto* target : targets) state(target, status, setting == tc::SwitchOn);
        return false;
    }
    case tc::ActionSetStructureActiveLevel: {
        auto* target = dynamic_cast<BuildingActor*>(object(instance, &side, 16));
        const auto amount = number(instance);
        if (target) { if (target->primary) target = target->primary; target->completed = amount; }
        return false;
    }
    case tc::ActionGiveWizardSpell: {
        auto* target = dynamic_cast<CreatureActor*>(object(instance, &side, 1));
        const auto* given = spell(instance);
        if (target && match_->spell_registry) {
            match_->spell_registry->add_to_books(target->wizard_spells, given->id, true);
            for (auto& book : target->wizard_spells) book.level(target->progression.level);
        }
        return false;
    }
    case tc::ActionSetNextMissionFilename: {
        const FourCC id{resource_id(instance, tc::ValueFilename)};
        if (filename && next_mission) next_mission(filename(id));
        return true;
    }
    case tc::ActionShowRandomTip:
        if (show_tip) show_tip({});
        return true;
    case tc::ActionShowTipText: {
        const FourCC id{resource_id(instance, tc::ValueText)};
        if (show_tip) show_tip(id);
        return true;
    }
    case tc::ActionMusicReset:
        match_->music(-1, 0);
        return true;
    case tc::ActionMusicTurn: {
        const auto setting = next(instance);
        if (setting != tc::SwitchOn && setting != tc::SwitchOff) throw Disabled{};
        match_->music(-2, setting == tc::SwitchOn ? 1.0f : 0.0f);
        return true;
    }
    case tc::ActionMusicAddToMood: {
        const auto amount = number(instance);
        if (next(instance) != tc::MoodBattle) throw Disabled{};
        match_->music(1, static_cast<float>(amount));
        return true;
    }
    case tc::ActionSpeechCreatureSaysTextWithSample:
    case tc::ActionSpeechCreatureAsksTextWithSample:
    case tc::ActionSpeechCreatureExclaimsTextWithSample:
    case tc::ActionSpeechGodSaysTextWithSample:
    case tc::ActionSpeechNarratorNarratesTextWithSample: {
        Dialogue line;
        line.id = ++dialogue_serial_;
        bool valid = true;
        if (code == tc::ActionSpeechGodSaysTextWithSample) {
            switch (next(instance)) {
            case tc::GodPersephone: line.god = 0; break;
            case tc::GodJames: line.god = 1; break;
            case tc::GodCharnel: line.god = 2; break;
            case tc::GodStratos: line.god = 3; break;
            case tc::GodPyro: line.god = 4; break;
            default: throw Disabled{};
            }
        } else if (code == tc::ActionSpeechNarratorNarratesTextWithSample) {
            switch (next(instance)) {
            case tc::NarratorPlayerWizard:
                if (const auto* wizard = dynamic_cast<CreatureActor*>(match_->local_side ? match_->local_side->wizard() : nullptr))
                    line.name = wizard->definition().id;
                break;
            case tc::NarratorFamiliar: line.name = FourCC::from_text("fmlr"); break;
            case tc::NarratorSage: line.name = FourCC::from_text("sage"); break;
            default: throw Disabled{};
            }
        } else {
            const auto* speaker = dynamic_cast<CreatureActor*>(object(instance, nullptr, 5));
            valid = speaker != nullptr;
            if (speaker) { line.speaker = speaker->reference(); line.name = speaker->definition().id; }
            line.style = code == tc::ActionSpeechCreatureAsksTextWithSample ? 1 :
                code == tc::ActionSpeechCreatureExclaimsTextWithSample ? 2 : 0;
        }
        line.text = FourCC{resource_id(instance, tc::ValueText)};
        line.sample = FourCC{resource_id(instance, tc::ValueSample)};
        if (!valid) return true;
        if (text) line.caption = text(line.text);
        instance.dialogue = line.id;
        instance.flags |= tc::FlagSuspended;
        dialogue.push_back(line);
        return false;
    }
    case tc::ActionSpeechWithText: { // 0x4732d0: queue a serial talking sequence and suspend its owning instance.
        Dialogue line;
        line.id = ++dialogue_serial_;
        const auto read_text = [&] {
            const auto tag = next(instance); // 0x4776d0, a typed resource id.
            if (tag != tc::ValueCreatureType && tag != tc::ValueStructureType && tag != tc::ValueNttType && tag != tc::ValueText && tag != tc::ValueFilename)
                throw Disabled{};
            return FourCC{next(instance)};
        };
        switch (next(instance)) {
        case tc::ActorCreature:
            if (auto* speaker = dynamic_cast<CreatureActor*>(object(instance, nullptr, 5))) {
                line.speaker = speaker->reference();
                line.name = speaker->definition().id;
            }
            break;
        case tc::ActorGod: {
            const auto god = next(instance);
            switch (god) {
            case tc::GodPersephone: line.god = 0; break;
            case tc::GodPyro: line.god = 4; break;
            case tc::GodJames: line.god = 1; break;
            case tc::GodCharnel: line.god = 2; break;
            case tc::GodStratos: line.god = 3; break;
            default: throw Disabled{};
            }
            break;
        }
        case tc::ActorName:
            if (next(instance) == tc::ActorNamePlayerWizard) {
                if (auto* speaker = dynamic_cast<CreatureActor*>(
                        match_->local_side ? match_->local_side->wizard() : nullptr)) {
                    line.speaker = speaker->reference();
                    line.name = speaker->definition().id;
                }
            } else
                line.name = read_text(); // 180: a literal speaker-name TEXT id.
            break;
        default: throw Disabled{};
        }
        const auto style = next(instance);
        switch (style) {
        case tc::SpeaksSays: case tc::SpeaksSaysWithEmotion: line.style = 0; break;
        case tc::SpeaksAsks: case tc::SpeaksAsksWithEmotion: line.style = 1; break;
        case tc::SpeaksExclaims: case tc::SpeaksExclaimsWithEmotion: line.style = 2; break;
        case tc::SpeaksNarrates: line.style = 3; break;
        default: throw Disabled{};
        }
        if (style >= tc::SpeaksSaysWithEmotion && style <= tc::SpeaksExclaimsWithEmotion) {
            const auto emotion = next(instance);
            if (emotion < tc::EmotionAnger || emotion > tc::EmotionSadness)
                throw Disabled{};
            line.emotion = emotion - tc::EmotionAnger + 1;
        }
        line.text = read_text();
        if (text)
            line.caption = text(line.text);
        if (line.style == 3)
            line.caption = '"' + line.caption + '"';
        switch (next(instance)) {
        case tc::TimingAutoTimed: line.ending = 0; break;
        case tc::TimingSampleLengthMinusFrames: line.ending = 1; line.time = number(instance); break;
        case tc::TimingPersistent: line.ending = 3; break;
        case tc::TimingPauseForInput: line.ending = 2; break;
        case tc::TimingForFrames: line.ending = 4; line.time = number(instance); break;
        default: throw Disabled{};
        }
        if (next(instance) == tc::SampleSpecWithSample) {
            if (next(instance) != tc::ValueSample)
                throw Disabled{};
            line.sample = FourCC{next(instance)};
        }
        instance.dialogue = line.id;
        instance.flags |= tc::FlagSuspended;
        dialogue.push_back(std::move(line));
        return false;
    }
    case tc::ActionOrderAllOfType:
    case tc::ActionOrderAllOfTypeAtLocation: { // 0x478b20: collect a side's matching objects, optionally in a region, then order them.
        const auto tag = next(instance);
        if (tag != tc::ValueCreatureType && tag != tc::ValueStructureType && tag != tc::ValueNttType && tag != tc::ValueText && tag != tc::ValueFilename)
            throw Disabled{};
        const auto type = next(instance);
        const auto where = location(instance, code == tc::ActionOrderAllOfTypeAtLocation ? 1 : 0);
        const auto given = order(instance);
        std::vector<MatchObject*> selected;
        count(side, {tc::NttsQuantityCreatureType, tc::ConstraintTrue, type}, where, nullptr, nullptr, &selected);
        for (auto* target : selected)
            apply(*target, given);
        return false;
    }
    case tc::ActionChangeSide: { // Change one named object's side (0x479390 -> vtable +104).
        auto* target = object(instance, &side, 29);
        auto* destination = this->side(instance);
        if (target && destination)
            transfer_side(*target, *destination, cinematic_);
        return false;
    }
    case tc::ActionChangeSideAllOfType:
    case tc::ActionChangeSideAllOfTypeAtLocation: { // 0x47b3f0: transfer the matching side members, optionally inside a region.
        const auto tag = next(instance);
        if (tag != tc::ValueCreatureType && tag != tc::ValueStructureType && tag != tc::ValueNttType && tag != tc::ValueText && tag != tc::ValueFilename)
            throw Disabled{};
        const auto type = next(instance);
        const auto where = location(instance, code == tc::ActionChangeSideAllOfTypeAtLocation ? 1 : 0);
        auto* destination = this->side(instance);
        if (!destination)
            return false;
        std::vector<MatchObject*> selected;
        count(side, {tc::NttsQuantityCreatureType, tc::ConstraintTrue, type}, where, nullptr, nullptr, &selected);
        // Membership changes invalidate the source lists; native callbacks also keep the next link.
        for (auto* target : selected)
            transfer_side(*target, *destination, cinematic_);
        return false;
    }
    case tc::ActionDeclareStanceToSide: { // Set mutual enemy (33), ally (34) or neutral (35) relations; notify AI event 19.
        const auto stance = next(instance);
        auto* other = this->side(instance);
        if (!other || stance < tc::StanceHostile || stance > tc::StanceNeutral)
            throw Disabled{};
        side.allies &= ~other->identity;
        other->allies &= ~side.identity;
        side.enemies &= ~other->identity;
        other->enemies &= ~side.identity;
        if (stance == tc::StanceHostile) { side.enemies |= other->identity; other->enemies |= side.identity; }
        if (stance == tc::StanceFriendly) { side.allies |= other->identity; other->allies |= side.identity; }
        for (auto* changed : match_->side_order)
            if (changed->controller)
                changed->controller->dirty |= 0x13;
        return false;
    }
    case tc::ActionSpeechCreatureSpeaksSample: { // Sample speech: independent per creature, with an owning-instance completion callback.
        auto* speaker = dynamic_cast<CreatureActor*>(object(instance, nullptr, 5));
        const auto style = next(instance);
        if (next(instance) != tc::ValueSample)
            throw Disabled{};
        const FourCC sample{next(instance)};
        if (!speaker || !sample.value)
            return false;
        Dialogue line;
        line.id = ++dialogue_serial_;
        line.speaker = speaker->reference();
        line.name = speaker->definition().id;
        line.style = style == tc::SpeaksAsks || style == tc::SpeaksAsksWithEmotion ? 1 : style == tc::SpeaksExclaims || style == tc::SpeaksExclaimsWithEmotion ? 2 : 0;
        line.sample = sample;
        instance.dialogue = line.id;
        instance.flags |= tc::FlagSuspended;
        voices.push_back(line);
        return false;
    }
    case tc::ActionEndMissionInDefeat:
        side.end_game(*match_, -1, 1);
        return false;
    case tc::ActionEndMissionInVictory:
        side.end_game(*match_, 1, 1);
        return false;
    case tc::ActionPause: { // Wait: 0x476d50 reads a constant, 0x47de80 sets the instance wake time (+20).
        if (next(instance) != tc::ValueInteger)
            throw Error("Invalid number code in trigger");
        instance.wait = match_->clock().match + next(instance);
        return false;
    }
    case tc::ActionSetVariable: { // Set a variable.
        auto& entry = variable(instance);
        value(entry, instance) = number(instance);
        return true;
    }
    case tc::ActionSetVariableToNumberOfNtts: { // Store the side's object count in a variable (0x47bb30), e.g. rescued peasants.
        auto& entry = variable(instance);
        const auto mode = next(instance);
        if (mode < tc::QuantifiedNttCreatureType || mode > tc::QuantifiedNttStructureTypeAtLocation)
            throw Disabled{};
        const auto tag = next(instance);
        if (tag != tc::ValueCreatureType && tag != tc::ValueStructureType && tag != tc::ValueNttType && tag != tc::ValueText && tag != tc::ValueFilename)
            throw Disabled{};
        const auto type = next(instance);
        const auto where = location(instance, mode >= tc::QuantifiedNttCreatureTypeAtLocation ? 1 : 0);
        int total = 0;
        count(side, {tc::NttsQuantityCreatureType, tc::ConstraintTrue, type}, where, nullptr, &total);
        value(entry, instance) = total;
        return false;
    }
    case tc::ActionSetVariableToRandomNumberInRange: { // A CRT rand() value in [low, high].
        auto& entry = variable(instance);
        const auto low = number(instance);
        const auto range = number(instance) - low + 1;
        value(entry, instance) = static_cast<std::int32_t>(static_cast<std::int64_t>(
            static_cast<double>(match_->random().next()) * range * 0.000030518509 + low));
        return true;
    }
    case tc::ActionDisplayVariable: // Show/hide a named script variable in the native side's value display.
    case tc::ActionHideVariable:
        {
            auto& entry = variable(instance);
            if (code == tc::ActionDisplayVariable) {
                if (entry.flags & 4) entry.displayed |= side.identity;
                else if (!entry.displayed) entry.displayed = side.identity;
            } else if (entry.flags & 4) entry.displayed &= ~side.identity;
            else entry.displayed = 0;
        }
        return true;
    case tc::ActionSetObjectiveComplete:
    case tc::ActionEnableObjective:
    case tc::ActionDisableObjective: {
        if (next(instance) != tc::ValueMissionObjective)
            throw Disabled{};
        const auto id = next(instance);
        if (id >= objectives.size())
            throw Error("Missing mission objective " + std::to_string(id));
        auto& flags = objectives[id].flags;
        if (code == tc::ActionSetObjectiveComplete) flags |= 2;
        else if (code == tc::ActionEnableObjective) flags |= 1;
        else flags &= ~1u;
        return true;
    }
    case tc::ActionSetAiSetting: { // Computer-side settings: 101/102 set or clear its flag 8, 238 a 0..100 aggression.
        if (!side.controller)
            throw Disabled{}; // Native script error, never a fatal engine error.
        const auto setting = next(instance);
        if (setting == tc::AiSettingAggression)
            side.controller->scale = std::clamp(number(instance), 0, 100) * .0099999998f;
        else if (setting != tc::AiSettingPaused && setting != tc::AiSettingUnpaused)
            throw Error("Invalid AI setting in trigger");
        if (setting == tc::AiSettingPaused) side.controller->flags |= 8;
        if (setting == tc::AiSettingUnpaused) side.controller->flags &= ~8u;
        return false;
    }
    case tc::ActionTurnBeaconAtLocation: { // 0x453b20: object beacon highlight, animated by the presentation host.
        const auto setting = next(instance);
        const auto where = location(instance, 1);
        if ((setting != tc::SwitchOn && setting != tc::SwitchOff) || where.mode != 1)
            throw Disabled{};
        auto found = std::ranges::find(beacons, where.object->reference(), &Beacon::object);
        if (found == beacons.end()) {
            if (setting == tc::SwitchOn) beacons.push_back({where.object->reference()});
        } else found->target = setting == tc::SwitchOn ? 1.0f : 0.0f;
        return false;
    }
    case tc::ActionCinematicsStart: // 0x453450: cinematic mode on / off.
    case tc::ActionCinematicsEnd: {
        const bool enabled = code == tc::ActionCinematicsStart;
        if (enabled != cinematic_) {
            if (enabled) {
                // Mode 2 starts with the aim held 50 units along the view (0x47f110, seeded by the host) and
                // the eye on the view's followed object at once (0x47f220), so it stays behind the wizard
                // while the script walks it until the eye is moved.
                cinematic_target_ = camera_target();
                camera_eye.start = camera_eye.point;
                camera_eye.elapsed = camera_eye.duration = 0;
                camera_eye.object = cinematic_target_;
                if (const auto* target = match_->resolve(cinematic_target_))
                    camera_eye.point = camera_eye.target = camera_object(*target, true);
            } else if (cinematic_target_ && camera_eye.object == cinematic_target_) {
                // 0x47f2c0 refuses a view already on the wizard, so 0x480190 turns the wizard's view angles
                // (+744 / +748) to the scene's direction, less the zoom's pitch: play resumes looking the
                // same way.
                if (auto* wizard = dynamic_cast<CreatureActor*>(match_->resolve(cinematic_target_))) {
                    const Vec3 forward{camera_aim.point.x - camera_eye.point.x, camera_aim.point.y - camera_eye.point.y,
                                       camera_aim.point.z - camera_eye.point.z};
                    const float flat = std::hypot(forward.x, forward.y);
                    wizard->view_yaw = flat == 0 ? 0.0f : std::atan2(-forward.x, forward.y);
                    wizard->view_pitch = (forward.z == 0 ? 0.0f : std::atan2(forward.z, flat)) - follow_curve(camera_zoom)[2];
                }
            }
        }
        cinematic_ = enabled;
        match_->set_cinematic(cinematic_);
        return true;
    }
    case tc::ActionSetViewZoomLevel: { // View +224: 122..126 give 0, 0.25, 0.5, 0.75, 1.
        const auto level = next(instance);
        if (level < tc::ZoomLevelClosest || level > tc::ZoomLevelFarthest)
            throw Disabled{};
        camera_zoom_target = static_cast<float>(level - tc::ZoomLevelClosest) * .25f;
        ++camera_zoom_revision;
        return true;
    }
    case tc::ActionSetViewSpeed: // Instance +52: camera move time in 1/60 s for actions 111..113.
        instance.locals[7] = number(instance);
        return true;
    case tc::ActionSuppressTipbox: // Closes the in-game FORM 'bkit' when it is open.
        close_spell_info = true;
        return true;
    case tc::ActionSetViewOriginFromLocation: // Cinematic camera eye (111), look-at (112) or both (113) to locations, over action 141's time.
    case tc::ActionSetViewFocusToLocation:
    case tc::ActionSetViewOriginFromLocationFocusToLocation:
        if (!cinematic_)
            throw Disabled{};
        move_camera(code == tc::ActionSetViewFocusToLocation ? camera_aim : camera_eye, location(instance, 1), instance.locals[7]);
        if (code == tc::ActionSetViewOriginFromLocationFocusToLocation)
            move_camera(camera_aim, location(instance, 1), instance.locals[7]);
        return false;
    case tc::ActionOrder: { // 0x4789c0: order a named wizard or creature.
        auto* target = object(instance, &side, 5);
        const auto given = order(instance);
        if (target)
            apply(*target, given);
        return false;
    }
    case tc::ActionCastSpellAtLocation: { // 0x453600: casterless side cast at a location.
        const auto* definition = spell(instance);
        const auto where = location(instance, 1);
        MatchObject* target = nullptr;
        Vec3 point{};
        if (where.mode == 1) {
            target = where.object;
            if (const auto* motion = where.object->source_motion())
                point = motion->position;
        } else
            point = where.marker->position;
        SpellActor::launch_side_cast(*match_, side, *definition, target, point);
        return false;
    }
    case tc::ActionTeleportCreatureToLocation: { // 0x47b440: teleport a named wizard or creature next to an object or marker.
        auto* target = object(instance, &side, 5);
        const auto where = location(instance, 1);
        auto* creature = dynamic_cast<CreatureActor*>(target);
        if (!creature)
            return false; // "Attempt to teleport" a missing object is only logged.
        const auto* beside = where.mode == 1 ? where.object->source_motion() : nullptr;
        if (where.mode == 1 && !beside)
            return false;
        const auto centre = beside ? beside->position : where.marker->position;
        auto point = centre;
        if (open_point(*match_, point, centre, 20, 31))
            creature->relocate(*match_, point);
        return false;
    }
    case tc::ActionTeleportCreatureAway: { // 0x47b440(null location): teleport burst, then 0x44d7a0 removes the actor.
        if (auto* actor = dynamic_cast<CreatureActor*>(object(instance, &side, 5))) {
            SpellActor::launch_teleport_burst(*match_, *actor, true);
            actor->state_flags |= MatchObject::removed_flag;
        }
        return false;
    }
    case tc::ActionSetNttState: { // Object status (+96): 0x4791c0 -> 0x45d100 / 0x46b0c0.
        auto* target = object(instance, &side, 21);
        const auto status = next(instance), setting = next(instance);
        if (setting != tc::SwitchOn && setting != tc::SwitchOff)
            throw Disabled{};
        const bool on = setting == tc::SwitchOn;
        state(target, status, on);
        return true;
    }
    case tc::ActionSetCreatureStat: { // 0x4770a0: set a creature's runtime base stat, preserving current/target ratios.
        auto* actor = dynamic_cast<CreatureActor*>(object(instance, &side, 5));
        const auto stat = next(instance);
        const auto amount = number(instance);
        if (!actor)
            return false;
        if (stat >= tc::CreatureStatSpeed && stat <= tc::CreatureStatResistanceToSplashRanged) {
            const unsigned index = stat - tc::CreatureStatSpeed + 3;
            auto& stats = actor->stats;
            const double old_base = stats.base[index];
            const float current_ratio = static_cast<float>(stats.current[index] / old_base);
            const double target_ratio = stats.target[index] / old_base;
            const auto narrow = [](double value) {
                if (!std::isfinite(value) || value < -0x1p63 || value >= 0x1p63)
                    return std::int16_t{};
                return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(static_cast<std::int64_t>(value)));
            };
            stats.base[index] = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(amount));
            stats.current[index] = narrow(amount * static_cast<double>(current_ratio));
            stats.target[index] = narrow(amount * target_ratio);
            stats.changed |= 1u << index;
        } else if (stat == tc::CreatureStatMaxMana)
            actor->maximum_mana = amount;
        else if (stat == tc::CreatureStatKills)
            actor->credit_count_80 = amount;
        else if (stat == tc::CreatureStatGibs)
            actor->credit_count_20 = amount;
        else throw Disabled{};
        return false;
    }
    case tc::ActionSetWeather: { // LAND::SetEffect(kind 1..6, enabled, transition 5 s).
        const auto effect = next(instance), setting = next(instance);
        if (effect < tc::WeatherRain || effect > tc::WeatherInsects || (setting != tc::SwitchOn && setting != tc::SwitchOff))
            throw Disabled{};
        match_->weather.set(effect - tc::WeatherRain + 1, setting == tc::SwitchOn ? 1.0f : 0.0f, 5);
        return false;
    }
    case tc::ActionEndMission:
        if (mission_over)
            mission_over();
        return false;
    case tc::ActionFadeToColorOverFrames: { // Screen fade: 192 black, 193 white, 194 red, 195 clear, over a number of 1/60 s.
        const auto colour = next(instance);
        if (colour < tc::ColorBlack || colour > tc::ColorClear)
            throw Error("Invalid color code in trigger");
        const auto duration = number(instance);
        fade.rgb = colour == tc::ColorWhite ? 0xffffffu : colour == tc::ColorRed ? 0xff0000u : 0;
        fade.target = colour == tc::ColorClear ? 0.0f : 1.0f;
        fade.rate = duration > 0 ? 1.0f / duration : 0;
        if (duration <= 0)
            fade.alpha = fade.target;
        return false;
    }
    case tc::ActionScreenShakeAtLocation: { // 0x453260: brief shake at an object or marker, amplitude 16, radius 140.
        const auto where = location(instance, 1);
        const auto at = where.mode == 1 ? where.object->source_motion()->position : where.marker->position;
        match_->shake_requests.push_back({at, 16, 140});
        return false;
    }
    case tc::ActionTurnInterfaceElement: { // Interface locks: 199..203 name bits 1..16; switch 108 (off) sets the bit.
        const auto lock = next(instance);
        const std::uint32_t bit = lock >= tc::InterfaceElementSelectionList && lock <= tc::InterfaceElementSoulCounter ? 1u << (lock - tc::InterfaceElementSelectionList) : 0;
        const auto setting = next(instance); // 0x479330.
        if (setting == tc::SwitchOff)
            interface_locks |= bit;
        else if (setting == tc::SwitchOn)
            interface_locks &= ~bit;
        else
            throw Error("Invalid switch code in trigger");
        return true;
    }
    case tc::ActionAddTextWithSampleToSpeechHistory: // 0x474940 appends title/text/sample to the journal; sample vtable +16 returns its ID.
    case tc::ActionAddIntroSampleToSpeechHistory: { // Recording only. Playback belongs to the journal UI, not map startup.
        JournalEntry entry;
        if (text)
            entry.title = text(FourCC::from_text("SPHt"));
        bool sound = true;
        if (code == tc::ActionAddTextWithSampleToSpeechHistory) {
            next(instance); // Typed TEXT id, as in 0x4778b0.
            entry.text = FourCC{next(instance)};
            if (text)
                entry.caption = text(entry.text);
            sound = next(instance) == tc::SampleSpecWithSample;
        }
        if (sound) {
            if (next(instance) != tc::ValueSample)
                throw Error("Invalid sample code in trigger");
            entry.sample = FourCC{next(instance)};
        }
        log("Script journal: " + entry.caption + " [" + entry.sample.text() + "]");
        journal.push_back(std::move(entry));
        return false;
    }
    case tc::ActionAskText:
    case tc::ActionAskTextWithSample: { // 0x47dea0: show FORM:chos and suspend only the owning trigger.
        Choice choice;
        choice.id = ++dialogue_serial_;
        const auto label = [&]() {
            if (next(instance) != tc::ValueText)
                throw Error("Invalid choice text code");
            return FourCC{next(instance)};
        };
        choice.question = label();
        if (code == tc::ActionAskTextWithSample) {
            const auto sound = next(instance);
            if (sound == tc::SampleSpecWithSample) {
                if (next(instance) != tc::ValueSample)
                    throw Error("Invalid choice sample code");
                choice.sample = FourCC{next(instance)};
            } else if (sound != tc::SampleSpecWithSampleNone)
                throw Error("Invalid choice sound mode");
        }
        choice.first = label();
        choice.second = label();
        choice.variable = static_cast<std::size_t>(&variable(instance) - variables_.data());
        instance.choice = choice.id;
        instance.flags |= tc::FlagSuspended;
        choices.push_back(choice);
        return false;
    }
    case tc::ActionSetNextMissionId: { // 0x479390: the operand names the god whose mission follows this one.
        const auto operand = next(instance);
        unsigned choice{};
        switch (operand) {
        case tc::GodPersephone: choice = 1; break;
        case tc::GodPyro: choice = 2; break;
        case tc::GodJames: choice = 3; break;
        case tc::GodCharnel: choice = 5; break;
        case tc::GodStratos: choice = 4; break;
        case tc::MissionIdEtherealRealm: choice = 0; break;
        default:
            throw Error("Invalid mission code in trigger");
        }
        if (mission_choice)
            mission_choice(choice);
        return true;
    }
    default:
        throw Unsupported{code + 0x10000}; // Action opcodes are reported above the condition range.
    }
}
bool Triggers::mask_condition(MatchSide& side, Instance& instance, std::uint32_t mask, std::uint32_t selector) {
    if (selector == tc::SideSpecSide) {
        const auto* chosen = this->side(instance);
        return chosen && (mask & chosen->identity);
    }
    if (selector == tc::SideSpecCurrentSide)
        return (mask & side.identity) != 0;
    if (selector < tc::SideSpecEnemySides || selector > tc::SideSpecNeutralSides)
        throw Disabled{};
    for (const auto& other : sides_.instances()) {
        const bool related = selector == tc::SideSpecEnemySides ? enemies(side, *other) : selector == tc::SideSpecFriendlySides ? allies(side, *other) :
            !enemies(side, *other) && !allies(side, *other);
        if (related && (mask & other->identity))
            return true;
    }
    return false;
}
Triggers::Filter Triggers::filter(Instance& instance) {
    Filter filter{next(instance)};
    if (filter.code == tc::NttsQuantityCreatureType || filter.code == tc::NttsQuantityStructureType) {
        filter.comparison = comparison(instance, filter.value);
        const auto tag = next(instance); // 0x4776d0.
        if (tag != tc::ValueCreatureType && tag != tc::ValueStructureType && tag != tc::ValueNttType && tag != tc::ValueText && tag != tc::ValueFilename)
            throw Error("Invalid ntt tag code in trigger");
        filter.type = next(instance);
    } else if (filter.code == tc::NttsCreature || filter.code == tc::NttsStructure)
        filter.object = object(instance, nullptr, filter.code == tc::NttsCreature ? 5 : 16);
    else
        throw Disabled{};
    return filter;
}
bool Triggers::count_condition(MatchSide& side, Instance& instance, int mode) {
    const auto selector = next(instance);
    MatchSide* chosen = nullptr;
    if (selector == tc::SideSpecSide)
        chosen = this->side(instance);
    else if (selector < tc::SideSpecEnemySides || selector > tc::SideSpecCurrentSide)
        return false;
    // 0x47ba90: 15 counts wizards and creatures (side +176), 16 primary buildings (+184); 17/18 name one
    // object.
    const auto filter = this->filter(instance);
    const auto where = location(instance, mode);
    if (selector == tc::SideSpecSide) {
        if (!chosen)
            throw Error("Trigger names a missing side");
        return count(*chosen, filter, where);
    }
    if (selector == tc::SideSpecCurrentSide)
        return count(side, filter, where);
    for (int i = 0; i < 32; ++i) {
        auto* other = sides_.get(i);
        if (!other || other->definition.index != static_cast<unsigned>(i))
            continue;
        // An empty slot or an already-defeated player cannot keep a skirmish running.
        if (instance.skirmish_sides && (!(other->identity & instance.skirmish_sides) || other->outcome < 0))
            continue;
        bool related = false;
        if (selector == tc::SideSpecEnemySides)
            related = enemies(side, *other);
        else if (selector == tc::SideSpecFriendlySides)
            related = allies(side, *other);
        else
            related = !enemies(side, *other) && !allies(side, *other);
        if (related && !count(*other, filter, where))
            return false;
    }
    return true;
}
bool Triggers::count(MatchSide& side, const Filter& filter, const Location& where, const MatchObject* observer, int* result, std::vector<MatchObject*>* selected, const MatchSide* viewer) {
    const auto matches = [&](MatchObject& target) {
        if (!inside(target, where))
            return false;
        if (viewer && target.allegiance != viewer) {
            const auto known = std::ranges::find(viewer->known, &target, &MatchSide::Known::object);
            if (known == viewer->known.end() || known->reference != target.reference() ||
                known->stamp + 12 <= viewer->stamp)
                return false; // 0x472420: recently seen, not merely remembered on the minimap.
        }
        if (!observer) {
            if (selected) selected->push_back(&target);
            return true;
        }
        if (const auto* actor = dynamic_cast<const CreatureActor*>(observer))
            return actor->terrain_visibility(*match_, &target) == 1;
        if (const auto* actor = dynamic_cast<const CreatureActor*>(&target);
            actor && actor->fsm().state() && (actor->fsm().state()->flags & 0x10000))
            return false;
        Vec3 from, to, hit;
        spell_target_point(from, observer);
        spell_target_point(to, &target);
        const Vec3 ray{to.x - from.x, to.y - from.y, to.z - from.z};
        const float squared = ray.x * ray.x + ray.y * ray.y + ray.z * ray.z;
        const auto result = terrain_intercept(*match_->terrain, from, ray, hit, squared, true);
        return result <= 0 || result >= 1;
    };
    if (filter.code == tc::NttsCreature || filter.code == tc::NttsStructure) // 0x47bc70: the named object, on that side.
        return filter.object && filter.object->allegiance == &side && matches(*filter.object);
    // 0x477a90 first compares the side's cached total; that can only decide what the full count decides
    // while the totals are current, so the reconstruction always counts (0x47bb30 / 0x477e30).
    unsigned groups = 31;
    switch (filter.type) {
    case tc::EntityTypeManafonts:
    case tc::EntityTypeAltars:
    case tc::EntityTypeManaliths:
        groups = 16;
        break;
    case tc::EntityTypeCreatures:
    case tc::EntityTypeFlyingCreatures:
    case tc::EntityTypeMeleeCreatures:
    case tc::EntityTypeCorpses:
    case tc::EntityTypeRangedCreatures:
        groups = 4;
        break;
    case tc::EntityTypeControllableCreatures:
    case tc::EntityTypeWizardsAndCreatures:
        groups = 5;
        break;
    case tc::EntityTypeWizards:
        groups = 1;
        break;
    default:
        break; // 0x477e30: any other word is a definition id searched over every group.
    }
    const bool by_definition = groups == 31;
    static constexpr std::array<unsigned, 5> group_masks{4, 1, 16, 130, 96}; // 0x4c0468.
    std::int32_t total = 0;
    for (unsigned group = 0; group < 5; ++group) {
        if (!(groups & group_masks[group]))
            continue;
        for (auto* object : side.objects[group]) {
            const auto kind = object->kind_flags;
            auto* building = dynamic_cast<BuildingActor*>(object);
            if (kind == MatchObject::building && building && building->primary && building->primary != building)
                continue; // 0x477c40: secondary building parts never count.
            bool counted = kind != 8;
            if (by_definition) {
                // Wizards and creatures compare their definition's id (+1052 -> +16), buildings their BLDG
                // id (+1160); corpses and other kinds never match.
                const auto* creature = dynamic_cast<const CreatureActor*>(object);
                counted = (kind == MatchObject::wizard || kind == MatchObject::creature)
                              ? creature && creature->definition().id.value == filter.type
                              : kind == MatchObject::building && building &&
                                    building->definition_id.value == filter.type;
                total += counted && matches(*object);
                continue;
            }
            switch (filter.type) {
            case tc::EntityTypeManafonts:
                counted = building && !building->primary && (building->building_flags & 0x10);
                break;
            case tc::EntityTypeManaliths:
                counted = building && !building->primary && (building->building_flags & 0x55a0) == 0x55a0 &&
                          building->reveal == 1;
                break;
            case tc::EntityTypeAltars:
                counted = building && (building->building_flags & 1);
                break;
            case tc::EntityTypeCorpses:
                counted = kind == 8;
                break;
            case tc::EntityTypeFlyingCreatures: {
                const auto* actor = dynamic_cast<const CreatureActor*>(object);
                counted = kind != 8 && actor && actor->stats.base[4] > 0;
                break;
            }
            case tc::EntityTypeControllableCreatures: {
                const auto* actor = dynamic_cast<const CreatureActor*>(object);
                counted = kind != 8 && actor && (kind == MatchObject::wizard ||
                    (!(actor->status_flags & 0x100) && (!actor->fsm().state() || !(actor->fsm().state()->flags & 0x2000))));
                break;
            }
            default:
                break;
            }
            total += counted && matches(*object); // 0x47bb70.
        }
    }
    if (result)
        *result = total;
    return compare(filter.comparison, total, filter.value, population(side, filter.code == tc::NttsQuantityStructureType));
}
std::uint32_t Triggers::comparison(Instance& instance, std::int32_t& value) {
    const auto op = next(instance);
    if (op >= tc::ConstraintAtLeast && op <= tc::ConstraintExactly)
        value = number(instance);
    return op;
}
std::int32_t Triggers::number(Instance& instance) {
    const auto code = next(instance);
    switch (code) {
    case tc::QuantityAdd:
    case tc::QuantitySub:
    case tc::QuantityMul:
    case tc::QuantityDiv: {
        const auto a = number(instance);
        const auto b = number(instance);
        const auto ua = static_cast<std::uint32_t>(a), ub = static_cast<std::uint32_t>(b);
        if (code == tc::QuantityAdd)
            return std::bit_cast<std::int32_t>(ua + ub);
        if (code == tc::QuantitySub)
            return std::bit_cast<std::int32_t>(ua - ub);
        if (code == tc::QuantityMul)
            return std::bit_cast<std::int32_t>(ua * ub);
        if (!b)
            throw Error("Trigger division by zero");
        return a / b;
    }
    case tc::QuantityInteger: // 0x476d50.
        if (next(instance) != tc::ValueInteger)
            throw Error("Invalid number code in trigger");
        return std::bit_cast<std::int32_t>(next(instance));
    case tc::QuantityTrue:
        return 1;
    case tc::QuantityFalse:
        return 0;
    case tc::QuantityVariable: { // 0x4777d0 / 0x47dcd0.
        auto& entry = variable(instance);
        return value(entry, instance);
    }
    case tc::QuantityStat: { // 0x476f70: runtime base stats, maximum mana and credit counts.
        const auto stat = next(instance);
        const auto* actor = dynamic_cast<CreatureActor*>(object(instance, nullptr, 5));
        if (!actor)
            return 0;
        if (stat >= tc::CreatureStatSpeed && stat <= tc::CreatureStatResistanceToSplashRanged)
            return actor->stats.base[stat - tc::CreatureStatSpeed + 3];
        if (stat == tc::CreatureStatMaxMana) return actor->maximum_mana;
        if (stat == tc::CreatureStatKills) return actor->credit_count_80;
        if (stat == tc::CreatureStatGibs) return actor->credit_count_20;
        throw Disabled{};
    }
    default:
        throw Error("Invalid numeric code in trigger");
    }
}
MatchSide* Triggers::side(Instance& instance) {
    if (next(instance) != tc::ValueSideId)
        throw Error("Invalid side code in trigger");
    return sides_.get(std::bit_cast<std::int32_t>(next(instance)));
}
} // namespace sacrifice
