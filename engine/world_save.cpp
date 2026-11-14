#include "sacrifice/world_save.hpp"
#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/soul_actor.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/trigger.hpp"
#include "sacrifice/wizard_control.hpp"
#include "sacrifice/compat.hpp"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <utility>
#include <zlib.h>

namespace sacrifice {
using Json = nlohmann::json;
namespace {
constexpr std::size_t limit = 256u * 1024 * 1024;
Vec3 vector(const Json& v) {
    return {v.at(0).get<float>(), v.at(1).get<float>(), v.at(2).get<float>()};
}
Json list(const PlayerControl::List& value) {
    auto result = Json::array();
    for (const auto& entry : value.entries())
        result.push_back({WorldJournal::reference(entry.object), entry.value});
    return result;
}
void list(PlayerControl::List& result, const Json& value, Match& match) {
    result.clear();
    for (auto i = value.rbegin(); i != value.rend(); ++i)
        if (auto* object = match.resolve(i->at(0).get<std::uint32_t>()))
            result.add(object, i->at(1).get<int>());
}
std::string hex(std::span<const std::uint8_t> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (auto byte : bytes) {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}
std::vector<std::uint8_t> unhex(const std::string& value) {
    if (value.size() % 2 || value.size() > limit * 2)
        throw Error("Invalid world save data length");
    const auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        throw Error("Invalid world save data");
    };
    std::vector<std::uint8_t> result(value.size() / 2);
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<std::uint8_t>(digit(value[i * 2]) * 16 + digit(value[i * 2 + 1]));
    return result;
}
std::uint32_t checksum(const std::filesystem::path& path) {
    const auto bytes = read_file(path);
    return static_cast<std::uint32_t>(
        crc32(0, reinterpret_cast<const Bytef*>(bytes.data()), static_cast<uInt>(bytes.size())));
}
} // namespace
WorldJournal::WorldJournal(Match& match, PlayerControl* player) : match_(match), player_(player) {}
Json WorldJournal::point(Vec3 p) {
    return {p.x, p.y, p.z};
}
WorldJournal::Scope::Scope(Match& match, Command command, Json arguments)
    : journal(match.world_journal.get()) {
    if (journal && journal->depth_++ == 0 && !journal->ticking_ && !journal->replaying_)
        journal->append({1, static_cast<unsigned>(command), journal->context(), journal->controls(),
                         std::move(arguments)});
}
WorldJournal::Scope::~Scope() {
    if (journal)
        --journal->depth_;
}
void WorldJournal::append(const Json& event) {
    if (!failure_.empty())
        return;
    const auto data = Json::to_cbor(event);
    if (tape_.size() + data.size() + 4 > limit) {
        failure_ = "World continuation exceeds storage limit";
        return;
    }
    const auto length = static_cast<std::uint32_t>(data.size());
    for (unsigned i = 0; i < 4; ++i)
        tape_.push_back(static_cast<std::uint8_t>(length >> (i * 8)));
    tape_.insert(tape_.end(), data.begin(), data.end());
}
Json WorldJournal::context() const {
    auto actors = Json::array();
    for (const auto& object : match_.objects())
        if (const auto* actor = dynamic_cast<const CreatureActor*>(object.get());
            actor &&
            (actor->type == MatchObject::wizard || actor->direct_controls || actor->input_provider)) {
            Json direct;
            if (actor->direct_controls)
                direct = *actor->direct_controls;
            actors.push_back({actor->reference(), actor->orders.mode.value, actor->view_yaw,
                              actor->view_pitch, direct, !!actor->input_provider});
        }
    auto selected = Json::array();
    if (match_.triggers && match_.triggers->is_selected)
        for (const auto& object : match_.objects())
            if (match_.triggers->is_selected(*object))
                selected.push_back(object->reference());
    const auto& c = match_.clock_;
    Json camera;
    if (match_.triggers)
        camera = {point(match_.triggers->camera_eye.point), point(match_.triggers->camera_aim.point)};
    return {match_.random().state,
            match_.visual_random.state,
            {c.fraction, c.total, c.match},
            match_.running,
            match_.paused,
            reference(match_.direct_control_target),
            actors,
            selected,
            camera,
            {match_.friendly_damage_scale, match_.projectile_friendly_fire, match_.allied_soul_pickup,
             match_.shared_vision}};
}
void WorldJournal::context(const Json& value) {
    match_.random().state = value.at(0).get<std::uint32_t>();
    match_.visual_random.state = value.at(1).get<std::uint32_t>();
    const auto& c = value.at(2);
    match_.clock_ = {c.at(0).get<float>(), c.at(1).get<std::uint32_t>(), c.at(2).get<std::uint32_t>()};
    match_.running = value.at(3).get<bool>();
    match_.paused = value.at(4).get<bool>();
    match_.direct_control_target = match_.resolve(value.at(5).get<std::uint32_t>());
    for (const auto& a : value.at(6)) {
        auto* actor = dynamic_cast<CreatureActor*>(match_.resolve(a.at(0).get<std::uint32_t>()));
        if (!actor)
            throw Error("World continuation lost a creature reference");
        actor->orders.mode = FourCC{a.at(1).get<std::uint32_t>()};
        actor->view_yaw = a.at(2).get<float>();
        actor->view_pitch = a.at(3).get<float>();
        actor->direct_controls.reset();
        if (!a.at(4).is_null())
            actor->direct_controls = a.at(4).get<std::array<int, 3>>();
        actor->input_provider = {};
        if (a.at(5).get<bool>()) {
            const auto inputs = frame_.is_array() ? frame_.at(3) : Json::array();
            actor->input_provider = [inputs, ref = actor->reference()] {
                for (const auto& input : inputs)
                    if (input.at(0) == ref)
                        return input.at(1).get<std::array<int, 3>>();
                return std::array<int, 3>{};
            };
        }
    }
    if (match_.triggers) {
        const auto selected = value.at(7).get<std::vector<std::uint32_t>>();
        match_.triggers->is_selected = [selected](const MatchObject& object) {
            return std::ranges::find(selected, object.reference()) != selected.end();
        };
        const auto& camera = value.at(8);
        if (!camera.is_null())
            match_.triggers->seed_camera(vector(camera.at(0)), vector(camera.at(1)));
    }
    const auto& options = value.at(9);
    match_.friendly_damage_scale = options.at(0).get<std::uint8_t>();
    match_.projectile_friendly_fire = options.at(1).get<bool>();
    match_.allied_soul_pickup = options.at(2).get<bool>();
    match_.shared_vision = options.at(3).get<bool>();
}
Json WorldJournal::controls() const {
    if (!player_)
        return {};
    auto groups = Json::array();
    for (const auto& group : player_->groups)
        groups.push_back(list(group));
    return {list(player_->selection),
            groups,
            player_->stamp,
            player_->append,
            point(player_->camera_right),
            point(player_->camera_forward),
            player_->pending.value,
            player_->pending_spell.value,
            player_->pending_mask,
            player_->pending_range,
            player_->spell_tab,
            player_->selection_spell_,
            reference(player_->last_leader_),
            player_->repeats_,
            list(player_->box_baseline_),
            player_->selecting_box_};
}
void WorldJournal::controls(const Json& v) {
    if (!player_ || v.is_null())
        return;
    list(player_->selection, v.at(0), match_);
    for (unsigned i = 0; i < 10; ++i)
        list(player_->groups[i], v.at(1).at(i), match_);
    player_->stamp = v.at(2).get<int>();
    player_->append = v.at(3).get<bool>();
    player_->camera_right = vector(v.at(4));
    player_->camera_forward = vector(v.at(5));
    player_->pending = FourCC{v.at(6).get<std::uint32_t>()};
    player_->pending_spell = FourCC{v.at(7).get<std::uint32_t>()};
    player_->pending_mask = v.at(8).get<std::uint32_t>();
    player_->pending_range = v.at(9).get<float>();
    player_->spell_tab = v.at(10).get<int>();
    player_->selection_spell_ = v.at(11).get<bool>();
    player_->last_leader_ = match_.resolve(v.at(12).get<std::uint32_t>());
    player_->repeats_ = v.at(13).get<int>();
    list(player_->box_baseline_, v.at(14), match_);
    player_->selecting_box_ = v.at(15).get<bool>();
}
void WorldJournal::begin_tick(int ticks) {
    ticking_ = true;
    if (!replaying_)
        frame_ = Json::array({0, ticks, context(), Json::array()});
}
void WorldJournal::end_tick() {
    // The view refreshes animation bounds (prepare_model) once per drawn frame, and soul anchors, spell
    // targets and collision read them. Settle them at the end of every tick so a replay, which draws
    // nothing, sees the same bounds as the live match did however often it was drawn.
    for (const auto& object : match_.objects())
        if (auto* actor = dynamic_cast<CreatureActor*>(object.get()); actor && actor->has_model())
            actor->prepare_model();
    if (!replaying_)
        append(frame_);
    ticking_ = false;
}
void WorldJournal::input(std::uint32_t object, std::array<int, 3> axes) {
    if (!replaying_ && ticking_)
        frame_.at(3).push_back({object, axes});
}
Json WorldJournal::fingerprint() const {
    auto state = Json::array();
    for (const auto& object : match_.objects()) {
        Json row = {object->reference(),          object->type,  object->kind_flags,
                    object->state_flags,          object->timer, reference(object->owner),
                    reference(object->controller)};
        if (const auto* m = object->source_motion())
            row.push_back({point(m->position), m->yaw});
        if (const auto* c = dynamic_cast<const CreatureActor*>(object.get())) {
            row.push_back({c->stats.base, c->stats.current, c->mana, c->maximum_mana, c->progression.level,
                           c->progression.experience});
            row.push_back(c->fsm().state() ? c->fsm().state()->id.value : 0u);
            auto orders = Json::array();
            for (auto n = c->orders.active_; n != -1; n = c->orders.nodes_[n].next) {
                const auto& order = c->orders.nodes_[n].value;
                orders.push_back({order.kind, reference(order.target), point(order.point), order.yaw});
            }
            row.push_back(orders);
        }
        state.push_back(row);
    }
    if (match_.triggers) {
        const auto& t = *match_.triggers;
        for (const auto& variable : t.variables_)
            state.push_back({variable.value, variable.sides, variable.displayed});
        for (const auto& trigger : t.triggers_)
            for (const auto& i : trigger.instances)
                state.push_back({i.side, i.flags, i.pc, i.wait, i.locals, i.dialogue, i.choice});
        state.push_back({t.game_timer, t.interface_locks, t.cinematic_});
    }
    for (const auto* side : match_.side_order)
        state.push_back({side->definition.index, side->outcome});
    const auto text = state.dump();
    return {match_.clock().match, match_.objects().size(),
            crc32(0, reinterpret_cast<const Bytef*>(text.data()), static_cast<uInt>(text.size()))};
}
void WorldJournal::dispatch(Command command, const Json& a) {
    const auto object = [&](std::size_t i) { return match_.resolve(a.at(i).get<std::uint32_t>()); };
    if (command == Command::debug_wizard) {
        auto* actor = dynamic_cast<CreatureActor*>(object(0));
        if (!actor)
            throw Error("Saved wizard is missing");
        configure_debug_wizard(match_, *actor, a.at(1).get<int>(), a.at(2).get<int>());
        return;
    }
    if (command == Command::dismiss) {
        match_.triggers->dismiss_dialogue(a.at(0).get<bool>());
        return;
    }
    if (command == Command::choice) {
        match_.triggers->answer_choice(a.at(0).get<unsigned>());
        return;
    }
    if (!player_)
        throw Error("World continuation has no local player");
    switch (command) {
    case Command::order:
        player_->order(a.at(0).get<int>(), object(1), vector(a.at(2)), a.at(3).get<unsigned>());
        break;
    case Command::protect_wizard:
        player_->protect_wizard();
        break;
    case Command::cast:
        player_->cast(FourCC{a.at(0).get<std::uint32_t>()}, object(1),
                      a.at(2).is_null() ? std::nullopt : std::optional(vector(a.at(2))));
        break;
    case Command::spell_key:
        player_->spell_key(FourCC{a.at(0).get<std::uint32_t>()});
        break;
    case Command::ability_key:
        player_->ability_key();
        break;
    case Command::cast_slot:
        player_->cast_slot(a.at(0).get<int>(), a.at(1).get<int>());
        break;
    case Command::resolve_target:
        player_->resolve_target(object(0), vector(a.at(1)));
        break;
    case Command::formation:
        player_->formation(FourCC{a.at(0).get<std::uint32_t>()});
        break;
    default:
        throw Error("Unknown world continuation command");
    }
}
Json WorldJournal::save() const {
    if (!failure_.empty())
        throw Error(failure_);
    if (ticking_ || replaying_)
        throw Error("Cannot save during a world update");
    uLongf length = compressBound(static_cast<uLong>(tape_.size()));
    std::vector<std::uint8_t> compressed(length);
    if (compress2(compressed.data(), &length, tape_.data(), static_cast<uLong>(tape_.size()), Z_BEST_SPEED) !=
        Z_OK)
        throw Error("Cannot compress world save");
    compressed.resize(length);
    auto pending = Json::array();
    for (const auto& object : match_.objects())
        if (const auto* actor = dynamic_cast<const CreatureActor*>(object.get());
            actor && actor->input_provider)
            pending.push_back({actor->reference(), actor->input_provider()});
    return {{"inputs", pending}, {"bytes", tape_.size()},  {"tape", hex(compressed)},
            {"tail", context()}, {"controls", controls()}, {"check", fingerprint()}};
}
void WorldJournal::restore(const Json& document) {
    const auto size = document.at("bytes").get<std::size_t>();
    if (size > limit)
        throw Error("World continuation exceeds storage limit");
    auto compressed = unhex(document.at("tape").get<std::string>());
    std::vector<std::uint8_t> tape(std::max<std::size_t>(size, 1));
    uLongf length = static_cast<uLongf>(tape.size());
    if (uncompress(tape.data(), &length, compressed.data(), static_cast<uLong>(compressed.size())) != Z_OK ||
        length != size)
        throw Error("Corrupt world continuation");
    tape.resize(size);
    auto selection = match_.triggers ? match_.triggers->is_selected : decltype(Triggers::is_selected){};
    auto flush =
        std::exchange(match_.flush_spell_sounds, [](SpellActor& spell) { spell.sound_requests.clear(); });
    replaying_ = true;
    try {
        std::size_t offset{};
        while (offset < tape.size()) {
            if (tape.size() - offset < 4)
                throw Error("Truncated world continuation");
            std::uint32_t count{};
            for (unsigned i = 0; i < 4; ++i)
                count |= static_cast<std::uint32_t>(tape[offset++]) << (i * 8);
            if (count > tape.size() - offset)
                throw Error("Truncated world continuation event");
            const auto event = Json::from_cbor(tape.begin() + offset, tape.begin() + offset + count);
            offset += count;
            if (event.at(0) == 0) {
                frame_ = event;
                context(event.at(2));
                match_.tick(event.at(1).get<int>());
            } else if (event.at(0) == 1) {
                context(event.at(2));
                controls(event.at(3));
                dispatch(event.at(1).get<Command>(), event.at(4));
            } else
                throw Error("Unknown world continuation event");
            for (const auto& object : match_.objects()) {
                if (auto* spell = dynamic_cast<SpellActor*>(object.get()))
                    spell->sound_requests.clear();
                if (auto* soul = dynamic_cast<SoulActor*>(object.get()))
                    soul->sounds.clear();
                if (auto* actor = dynamic_cast<CreatureActor*>(object.get())) {
                    actor->cues.clear();
                    actor->incantation_requested = false;
                    actor->voice_handle = 0;
                    std::erase_if(actor->combat_requests, [](const auto& request) {
                        return request.kind == CreatureActor::CombatRequest::Kind::sound;
                    });
                }
            }
            match_.ambient_cues.clear();
            match_.weather_sounds.clear();
            match_.music_requests.clear();
            match_.shake_requests.clear();
        }
        frame_ = Json::array({0, 0, document.at("tail"), document.at("inputs")});
        context(document.at("tail"));
        controls(document.at("controls"));
        if (fingerprint() != document.at("check"))
            throw Error(
                "World save did not reconstruct exactly; this save may require its original engine build");
        tape_ = std::move(tape);
    } catch (...) {
        match_.flush_spell_sounds = std::move(flush);
        replaying_ = ticking_ = false;
        if (match_.triggers)
            match_.triggers->is_selected = std::move(selection);
        throw;
    }
    match_.flush_spell_sounds = std::move(flush);
    replaying_ = ticking_ = false;
    if (match_.triggers)
        match_.triggers->is_selected = std::move(selection);
    if (player_) {
        player_->feedback = {};
        player_->interface_sounds.clear();
        player_->markers.clear();
    }
    frame_ = Json{};
}
bool WorldSave::valid_name(std::string_view name) {
    if (name.empty() || name.size() > name_limit || name.front() == ' ' || name.back() == ' ' || name.back() == '.')
        return false;
    for (const auto c : name)
        if (static_cast<unsigned char>(c) < 32 || std::string_view("\\/:*?\"<>|").find(c) != std::string_view::npos)
            return false;
    return true;
}
std::filesystem::path WorldSave::directory() {
    return application_data() / "saves/games";
}
std::filesystem::path WorldSave::path(std::string_view name, const std::filesystem::path& folder) {
    if (!valid_name(name))
        throw Error("Invalid saved game name");
    return folder / std::filesystem::path(std::u8string(name.begin(), name.end()) + u8".sacsave.json");
}
std::vector<std::string> WorldSave::names(const std::filesystem::path& folder) {
    std::error_code error;
    // The first build kept one continuation at saves/World/quick.sacsave.json; it becomes "Quicksave".
    if (folder == directory()) {
        const auto legacy = application_data() / "saves/World/quick.sacsave.json";
        if (std::filesystem::exists(legacy, error) && !std::filesystem::exists(path(quick_name, folder), error)) {
            std::filesystem::create_directories(folder, error);
            std::filesystem::rename(legacy, path(quick_name, folder), error);
        }
    }
    std::vector<std::string> result;
    if (!std::filesystem::is_directory(folder, error))
        return result;
    constexpr std::string_view suffix = ".sacsave.json";
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (!entry.is_regular_file(error))
            continue;
        const auto file = utf8(entry.path().filename());
        if (file.size() <= suffix.size() ||
            !ascii_iequals(std::string_view(file).substr(file.size() - suffix.size()), suffix))
            continue;
        if (auto name = file.substr(0, file.size() - suffix.size()); valid_name(name))
            result.push_back(std::move(name));
    }
    std::ranges::sort(result, [](const std::string& a, const std::string& b) {
        return std::ranges::lexicographical_compare(a, b, [](char x, char y) {
            return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
        });
    });
    return result;
}
bool WorldSave::exists(std::string_view name, const std::filesystem::path& folder) {
    std::error_code error;
    return valid_name(name) && std::filesystem::is_regular_file(path(name, folder), error);
}
void WorldSave::remove(std::string_view name, const std::filesystem::path& folder) {
    if (!valid_name(name))
        return;
    std::error_code error;
    std::filesystem::remove(path(name, folder), error);
}
WorldSave::Header WorldSave::header() const {
    Header result;
    if (const auto found = setup.find("header"); found != setup.end() && found->is_object()) {
        result.profile = found->value("profile", std::string{});
        result.game_type = found->value("game_type", std::string{});
        result.map = found->value("map", std::string{});
        result.time = found->value("time", std::int64_t{});
    }
    if (result.map.empty() && setup.contains("map"))
        result.map = setup.at("map").at("path").get<std::string>();
    return result;
}
void WorldSave::header(const Header& value) {
    setup["header"] = {{"profile", value.profile}, {"game_type", value.game_type}, {"map", value.map},
                       {"time", value.time}};
}
void WorldSave::stamp(std::string profile, std::string game_type) {
    auto map = setup.at("map").at("path").get<std::string>();
    std::ranges::replace(map, '/', '\\');
    header({std::move(profile), std::move(game_type), std::move(map), static_cast<std::int64_t>(std::time(nullptr))});
}
Image WorldSave::scale_thumbnail(const Image& frame) {
    // The native thumbnail is a 128x128 render of the whole view; the sgam/lgam 'thmb' widget stretches it
    // back over its 170x128 rectangle. Box-filter the captured frame down to the same square.
    Image result{thumbnail_size, thumbnail_size, std::vector<std::uint32_t>(thumbnail_size * thumbnail_size)};
    if (!frame.width || !frame.height || frame.rgba.size() < std::size_t{frame.width} * frame.height)
        return result;
    for (unsigned y = 0; y < thumbnail_size; ++y)
        for (unsigned x = 0; x < thumbnail_size; ++x) {
            const unsigned x0 = x * frame.width / thumbnail_size;
            const unsigned x1 = std::max(x0 + 1, (x + 1) * frame.width / thumbnail_size);
            const unsigned y0 = y * frame.height / thumbnail_size;
            const unsigned y1 = std::max(y0 + 1, (y + 1) * frame.height / thumbnail_size);
            std::array<std::uint64_t, 3> sum{};
            for (unsigned v = y0; v < y1; ++v)
                for (unsigned u = x0; u < x1; ++u) {
                    const auto p = frame.rgba[std::size_t{v} * frame.width + u];
                    sum[0] += p & 0xff, sum[1] += (p >> 8) & 0xff, sum[2] += (p >> 16) & 0xff;
                }
            const auto count = std::uint64_t{x1 - x0} * (y1 - y0);
            result.rgba[std::size_t{y} * thumbnail_size + x] =
                static_cast<std::uint32_t>(sum[0] / count) | static_cast<std::uint32_t>(sum[1] / count) << 8 |
                static_cast<std::uint32_t>(sum[2] / count) << 16 | 0xff000000u;
        }
    return result;
}
void WorldSave::thumbnail(const Image& image) {
    const auto scaled =
        image.width == thumbnail_size && image.height == thumbnail_size ? image : scale_thumbnail(image);
    std::vector<std::uint8_t> bytes;
    bytes.reserve(scaled.rgba.size() * 2);
    for (const auto p : scaled.rgba) { // RGB565, as the native 16-bit surface.
        const auto value =
            static_cast<std::uint16_t>(((p & 0xf8) << 8) | (((p >> 8) & 0xfc) << 3) | ((p >> 19) & 0x1f));
        bytes.push_back(static_cast<std::uint8_t>(value));
        bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    }
    setup["thumbnail"] = hex(bytes);
}
std::optional<Image> WorldSave::thumbnail() const {
    const auto found = setup.find("thumbnail");
    if (found == setup.end() || !found->is_string())
        return {};
    const auto bytes = unhex(found->get<std::string>());
    if (bytes.size() != thumbnail_size * thumbnail_size * 2)
        return {};
    Image result{thumbnail_size, thumbnail_size, {}};
    result.rgba.reserve(thumbnail_size * thumbnail_size);
    for (std::size_t i = 0; i < bytes.size(); i += 2) {
        const unsigned v = bytes[i] | bytes[i + 1] << 8;
        const unsigned r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
        result.rgba.push_back((r << 3 | r >> 2) | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2) << 16 | 0xff000000u);
    }
    return result;
}
WorldSave WorldSave::capture(const std::filesystem::path& root, const std::filesystem::path& path,
                             SceneMatch& scene, std::span<const SkirmishSlot> slots,
                             const std::optional<Character>& starting_character) {
    if (!scene.match().world_journal)
        throw Error("World recording is not available for this match");
    auto players = Json::array();
    for (const auto& slot : slots)
        players.push_back({static_cast<unsigned>(slot.kind), slot.wizard.value, slot.god, slot.team});
    Json character;
    if (starting_character) {
        const auto bytes = starting_character->encode();
        character =
            std::vector<std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                      reinterpret_cast<const std::uint8_t*>(bytes.data() + bytes.size()));
    }
    WorldSave save{{{"map", map(root, path)},
                    {"seed", scene.initial_random_state},
                    {"slots", players},
                    {"character", character}},
                   scene.match().world_journal->save()};
    if (scene.match().campaign_god)
        save.setup["campaign_god"] = *scene.match().campaign_god;
    return save;
}
std::vector<SkirmishSlot> WorldSave::slots() const {
    std::vector<SkirmishSlot> result;
    for (const auto& slot : setup.at("slots")) {
        const auto kind = slot.at(0).get<unsigned>();
        if (kind > static_cast<unsigned>(SkirmishSlot::Kind::observer) || result.size() >= 5)
            throw Error("Invalid saved player slot");
        result.push_back({static_cast<SkirmishSlot::Kind>(kind), FourCC{slot.at(1).get<std::uint32_t>()},
                          slot.at(2).get<std::uint8_t>(), slot.at(3).get<std::uint8_t>()});
    }
    return result;
}
std::optional<Character> WorldSave::character() const {
    if (setup.at("character").is_null())
        return {};
    const auto bytes = setup.at("character").get<std::vector<std::uint8_t>>();
    return Character::decode({reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()});
}
std::optional<WorldSave> WorldSave::read(std::string_view name, const std::filesystem::path& folder) {
    if (!valid_name(name))
        return {};
    const auto data = DocumentStore(path(name, folder), "sacrifice.world", 1).read();
    if (!data)
        return {};
    return WorldSave{data->at("setup"), data->at("world")};
}
void WorldSave::write(std::string_view name, const std::filesystem::path& folder) const {
    DocumentStore(path(name, folder), "sacrifice.world", 1).write({{"setup", setup}, {"world", world}});
}
Json WorldSave::map(const std::filesystem::path& root, const std::filesystem::path& path) {
    const auto relative = std::filesystem::relative(path, root);
    for (const auto& part : relative)
        if (part == "..")
            throw Error("World save map is outside the game directory");
    return {{"path", relative.generic_string()}, {"crc", checksum(path)}};
}
std::filesystem::path WorldSave::mission(const std::filesystem::path& root) const {
    const auto relative = std::filesystem::path(setup.at("map").at("path").get<std::string>());
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
        relative.extension() != ".scp")
        throw Error("Invalid world save map path");
    for (const auto& part : relative)
        if (part == "..")
            throw Error("Invalid world save map path");
    const auto path = root / relative;
    if (checksum(path) != setup.at("map").at("crc").get<std::uint32_t>())
        throw Error("The installed map differs from the saved map");
    return path;
}
} // namespace sacrifice
