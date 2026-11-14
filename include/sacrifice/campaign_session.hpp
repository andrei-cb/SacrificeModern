#pragma once
#include "campaign.hpp"
#include "character.hpp"
#include "scene_match.hpp"

namespace sacrifice {
// Mission lifecycle shared by every frontend. The host decides when pages and scene loads occur.
struct CampaignSession {
    bool active{};
    int outcome{-1}; // -2 restart, -1 undecided, 0 defeat, 1 victory.
    CreatureActor* wizard{};
    std::optional<Character> snapshot;
    void enter(Character& character, Match& match);
    bool result(Character& character, bool won);
    bool finish(Character& character, const Triggers& triggers, CampaignHost& host, bool restart);
    std::optional<std::filesystem::path> next(Character& character, CampaignHost& host,
                                              const std::filesystem::path& current);

  private:
    bool transition_pending_{};
};
// 0x450f70: disable result-menu commands during a 60-tick fade, then dispatch once.
struct MissionEndTransition {
    FourCC action;
    float alpha{};
    void start(FourCC command);
    std::optional<FourCC> advance(float seconds);
    void reset();
};
void present_mission_result(Match& match, bool won);
void bind_campaign_triggers(Triggers& triggers, Character& character, CampaignHost& host);
} // namespace sacrifice
