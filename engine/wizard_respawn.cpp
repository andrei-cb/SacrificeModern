#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/spatial.hpp"
#include "sacrifice/spell_actor.hpp"

namespace sacrifice {
namespace {
FourCC cc(std::string_view text) {
    return FourCC::from_text(text);
}
std::int16_t word(int value) {
    return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(value));
}
} // namespace
int CreatureActor::request_respawn() {
    if (!network_identity)
        return -1;
    if (!allegiance || !allegiance->wizard() || allegiance->outcome == -1 || !home_altar ||
        (home_altar->common_status & 0x100) || home_altar->ritual.action == cc("sacu"))
        return 0;
    orders.clear();
    match_->notify_reference(*this, 14);
    mana = 0;
    state_flags &= ~0x2000u;
    stats.current[CreatureStats::health] = 0;
    return 1;
}
int CreatureActor::wizard_handler(std::string_view name, int) {
    const auto fade_to = [&](float value) {
        fade_target = value;
        fade_step = std::bit_cast<float>(1015580809u);
    };
    if (name == "WizardRequestRespawn") {
        const auto result = request_respawn();
        if (result == 0)
            return fsm_->event(33);
        if (result == 1)
            return fsm_->event(31);
        return 0;
    }
    if (name == "WizardRespawnDenied") {
        fade_to(1);
        if (fade == 1)
            eliminate_wizard(*match_);
        return 0;
    }
    if (name == "WizardStartRespawnEffect") {
        fade_to(.75f);
        SpellActor::launch_respawn_host(*match_, *this, SpellActor::Effect::respawn_body);
        SpellActor::launch_respawn_host(*match_, *this, SpellActor::Effect::respawn_wisps);
        return 0;
    }
    if (name == "WizardRespawnRun") {
        fade_to(.75f);
        return 0;
    }
    if (name == "WizardRespawnDisappearEffect" || name == "WizardRespawnReappearEffect") {
        SpellActor::launch_respawn_pulse(*match_, *this);
        combat_sound(FourCC{1483502702u}); // Creature-table group, selected inline like Xact/Xdam.
        fade_to(name == "WizardRespawnReappearEffect" ? 0.0f : .75f);
        return 0;
    }
    throw Error("Unimplemented wizard respawn handler: " + std::string(name));
}
void CreatureActor::run_wizard(Match& match) {
    if (fsm_->state()->flags & 0x40000) {
        const bool desecrated = home_altar && home_altar->ritual.action == cc("sacu");
        if (!desecrated && (!allegiance || allegiance->outcome != -1)) {
            if (mana < maximum_mana / 4) {
                const int product =
                    std::bit_cast<int>(static_cast<unsigned>(mana) * 4u *
                                       static_cast<unsigned>(static_cast<int>(stats.base[5])));
                stats.current[5] = word(product / maximum_mana);
            } else {
                stats.current[5] = stats.base[5];
                fsm_->event(32);
            }
        } else
            fsm_->event(32);
        stats.changed |= 8;
        stats.target[3] = word(std::max(static_cast<int>(stats.target[3]) * 2, 0));
    }
    // Virtual +56 tests the corpse bit, not alive(): spirits keep scanning and receiving mana.
    // Diagnostic/NPC hosts may have no home altar until full player setup binds one.
    if (!(fsm_->state()->flags & 0x4000) && match.spatial)
        refresh_cast_requirements(match);
}
void CreatureActor::eliminate_wizard(Match& match) {
    if (home_altar)
        home_altar->destroy(match);
    soul_count = 0; // Native direct store; side elimination bookkeeping remains separate.
    // 0x482460: controller ownership is independent of the creature's current allegiance.
    match.visit_group(0, MatchObject::creature, [&](MatchObject& object) {
        if (object.controller == this)
            object.state_flags |= 0x40000;
        return 0;
    });
    complete_corpse(match);
    state_flags |= removed_flag;
}
bool CreatureActor::relocate(Match& match, Vec3 point) {
    if (ritual_building)
        return false;
    cast_visual_requests.push_back({FourCC{1081370990u}, motion.position});
    SpellActor::launch_teleport_burst(match, *this, false);
    const Vec3 delta{point.x - motion.position.x, point.y - motion.position.y, point.z - motion.position.z};
    for (auto& entry : named_references)
        if (entry.name == cc("PARA"))
            if (auto* effect = dynamic_cast<SpellActor*>(entry.object)) {
                effect->motion.position.x += delta.x;
                effect->motion.position.y += delta.y;
                effect->motion.position.z += delta.z;
                effect->point.x += delta.x;
                effect->point.y += delta.y;
                effect->point.z += delta.z;
            }
    motion.position = point;
    motion.velocity = motion.local_velocity = {};
    cast_visual_requests.push_back({FourCC{1081372533u}, motion.position});
    SpellActor::launch_teleport_burst(match, *this, true);
    orders.discard_temporary();
    if (orders.kind() != 5)
        orders.clear();
    path.clear();
    approach_blocker = nullptr;
    approach_result = 0;
    fade = 1;
    fade_target = 0;
    fade_step = std::bit_cast<float>(1015580809u);
    return true;
}
} // namespace sacrifice
