#include "sacrifice/campaign_session.hpp"
#include "sacrifice/music.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/spell_registry.hpp"
#include <utility>

namespace sacrifice {
void CampaignSession::enter(Character& character, Match& match) {
    active = true;
    snapshot = character;
    outcome = -1;
    transition_pending_ = false;
    wizard = match.local_side ? dynamic_cast<CreatureActor*>(match.local_side->wizard()) : nullptr;
    if (!wizard)
        throw Error("Campaign mission has no local wizard");
    character.enter_mission(match.campaign_god.value_or(match.mission_god), match.minimum_wizard_level,
                            match.random());
    auto carried = character.progression();
    if (match.minimum_wizard_level > carried.level)
        carried.set(match.minimum_wizard_level, 0, [&](WizardProgression& state) {
            if (match.spell_registry)
                match.spell_registry->grant(state);
        });
    wizard->adopt_progression(match, carried);
}
bool CampaignSession::result(Character& character, bool won) {
    if (!active || outcome != -1)
        return false;
    outcome = won ? 1 : 0;
    if (won && wizard) {
        auto finished = wizard->progression;
        finished.stats = wizard->stats.base;
        finished.maximum_mana = wizard->maximum_mana;
        finished.credit_count_80 += wizard->credit_count_80;
        finished.credit_count_20 += wizard->credit_count_20;
        character.absorb(finished);
    } else if (snapshot)
        character = *snapshot;
    return true;
}
bool CampaignSession::finish(Character& character, const Triggers& triggers, CampaignHost& host,
                             bool restart) {
    if (!active)
        return false;
    if (restart)
        outcome = -2;
    if (outcome == 1)
        for (const auto& [key, value] : triggers.campaign_values())
            character.set_flag(key, value);
    if (outcome < 0 && snapshot)
        character = *snapshot;
    snapshot.reset();
    wizard = nullptr;
    active = false;
    if (outcome == 0)
        host.begin_mission();
    transition_pending_ = outcome != 0;
    return transition_pending_;
}
std::optional<std::filesystem::path> CampaignSession::next(Character& character, CampaignHost& host,
                                                           const std::filesystem::path& current) {
    if (!std::exchange(transition_pending_, false))
        return {};
    return outcome == -2 ? std::optional(current) : host.victory(character);
}
void MissionEndTransition::start(FourCC command) {
    if (!action.value) {
        action = command;
        alpha = 0;
    }
}
std::optional<FourCC> MissionEndTransition::advance(float seconds) {
    if (!action.value)
        return {};
    alpha = std::clamp(alpha + std::max(seconds, 0.f), 0.f, 1.f);
    if (alpha < 1)
        return {};
    return std::exchange(action, FourCC{});
}
void MissionEndTransition::reset() {
    action = {};
    alpha = 0;
}
void present_mission_result(Match& match, bool won) {
    match.paused = true;
    match.music(won ? MusicDirector::victory : MusicDirector::defeat, 1);
}
void bind_campaign_triggers(Triggers& triggers, Character& character, CampaignHost& host) {
    triggers.bind_campaign([&character](std::string_view key) { return character.flag(key); });
    triggers.mission_choice = [&character, &host](unsigned choice) {
        if (host.choose(character, choice))
            log("Campaign portal choice: " + std::string(CampaignHost::choices.at(choice)));
    };
    triggers.next_mission = [&host](std::string file) { host.choose_filename(std::move(file)); };
}
} // namespace sacrifice
