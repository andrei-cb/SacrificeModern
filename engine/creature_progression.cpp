#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/spell_registry.hpp"

namespace sacrifice {
void CreatureActor::grant_level(WizardProgression& state) {
    if (match_->spell_registry)
        match_->spell_registry->grant(state);
    else if (state.stats[18])
        grant_requests.push_back({state.level, state.stats[18]});
}
void CreatureActor::level_changed(int change) {
    replication_flags &= ~0x2000u;
    const int event = change > 0 ? 37 : 40;
    level_requests.push_back({change, progression.level, event});
    fsm_->event(event);
    if (match_->spell_registry)
        match_->spell_registry->synchronize(progression, wizard_spells);
    for (auto& book : wizard_spells)
        book.level(progression.level);
    if (allegiance && (!(fsm_->state()->flags & 0x8000000) || allegiance != match_->local_side))
        allegiance->event(*this, nullptr, 22);
    progression.synchronize(stats, mana, maximum_mana);
    if (allegiance)
        allegiance->event(*this, nullptr, 23, change);
}
void CreatureActor::set_experience(int level, int experience) {
    if (type != MatchObject::wizard)
        throw Error("Only wizards have experience");
    const auto previous = progression.level;
    progression.set(level, experience, [this](auto& state) { grant_level(state); });
    // Explicit mode 0 refreshes even if the resulting level did not change (event 40).
    level_changed(progression.level - previous);
}
} // namespace sacrifice
