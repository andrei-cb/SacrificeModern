#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/soul_actor.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_registry.hpp"
#include <cmath>
#include <format>
#include <utility>

namespace sacrifice {
namespace {
int add(int a, int b) {
    return std::bit_cast<int>(static_cast<unsigned>(a) + static_cast<unsigned>(b));
}
int product(int a, int b) {
    return std::bit_cast<int>(static_cast<unsigned>(a) * static_cast<unsigned>(b));
}
FourCC cc(std::string_view text) {
    return FourCC::from_text(text);
}
} // namespace
const CreatureSpellBook::Entry* CreatureActor::cast_entry(BookSlot slot) const {
    if (slot.book > 3)
        return nullptr;
    const auto& book = slot.book ? wizard_spells[slot.book - 1] : spells;
    return slot.entry < book.entries.size() ? &book.entries[slot.entry] : nullptr;
}
CreatureSpellBook::Entry* CreatureActor::cast_entry(BookSlot slot) {
    return const_cast<CreatureSpellBook::Entry*>(std::as_const(*this).cast_entry(slot));
}
std::optional<CreatureActor::BookSlot> CreatureActor::find_spell(FourCC id) const {
    for (unsigned b = 0; b < (type == MatchObject::wizard ? 4u : 1u); ++b) {
        const auto& book = b ? wizard_spells[b - 1] : spells;
        for (std::size_t i = 0; i < book.entries.size(); ++i)
            if (book.entries[i].id == id)
                return BookSlot{b, i};
    }
    return {};
}
std::pair<int, int> CreatureActor::cast_cost(const SpellDefinition& definition) const {
    const int mana_cost = definition.mana_cost ? definition.mana_cost : 1;
    int ticks = definition.fast_cast_ticks;
    if (type == MatchObject::wizard) {
        const int difference =
            std::clamp(progression.level - static_cast<int>(definition.priority / 100), 0, 5);
        ticks = static_cast<int>(
            difference * 0.2 *
                (static_cast<double>(definition.fast_cast_ticks) - definition.slow_cast_ticks) +
            definition.slow_cast_ticks);
    }
    return {mana_cost, ticks ? ticks : 1};
}
CreatureActor::CastCheck CreatureActor::cast_check(FourCC id) const {
    const auto slot = find_spell(id);
    const auto* registry = match_->spell_registry.get();
    const auto* entry = slot ? cast_entry(*slot) : nullptr;
    const auto* definition = entry && registry ? registry->find(entry->id) : nullptr;
    if (!entry)
        return {2, cc(type == MatchObject::wizard ? "_XP_" : "mana")};
    if (!definition)
        return {2, cc("mana")};
    if (type == MatchObject::wizard) {
        if (entry->flags & 2)
            return {2, cc("_XP_")};
        if (definition->kind == 2) {
            if (soul_count < definition->required_souls)
                return {2, cc("spir")};
        } else if (definition->spell) {
            const auto flags = definition->spell->property_flags;
            if ((flags & 0x400) && !casting.requirements[1])
                return {2, cc("obld")};
            if ((flags & 0x200) && !casting.requirements[0])
                return {2, cc("ealt")};
            if ((flags & 0x100) && !casting.requirements[2])
                return {2, cc("obld")};
        }
        if (entry->remaining)
            return {2, cc("nred")};
    } else {
        if (definition->kind == 2 || definition->kind == 16)
            return {2, cc("mana")};
        if (ritual_building && (!definition->spell || !(definition->spell->property_flags & 0x1000000)))
            return {2, cc("grdn")};
        if (cast_cost(*definition).first > mana)
            return {1, cc("mana")};
        if (entry->remaining)
            return {1, cc("time")};
    }
    return cast_cost(*definition).first <= mana ? CastCheck{} : CastCheck{2, cc("mana")};
}
bool CreatureActor::cast_target(const SpellDefinition& definition) const {
    return cast_target(definition, effect_target);
}
bool CreatureActor::cast_target(const SpellDefinition& definition, const MatchObject* candidate) const {
    const auto* data = definition.kind == 4 ? definition.spell.get() : nullptr;
    auto mask = data ? data->targets : definition.targets;
    if (!mask) {
        if (!candidate)
            return true;
        mask = candidate->kind_flags;
    }
    // 0x450390 tests individual rules, rather than rejecting other bits in the definition.
    // Its 0x20000 anchor >= 96 check passes with LAND's default 128: the port, like the
    // installed initial maps, has no mutable anchor buffer yet (also used by DentMap).
    if (!candidate)
        return (mask & 0x4000) != 0;
    if (!(mask & 0x1f))
        return true;
    if (!(mask & candidate->kind_flags))
        return false;
    const auto* victim = dynamic_cast<const CreatureActor*>(candidate);
    const auto* structure = dynamic_cast<const BuildingActor*>(candidate);
    const auto* soul = dynamic_cast<const SoulActor*>(candidate);
    if (!victim && !structure && !soul)
        throw Error("Spell target class requires an unimplemented host");
    if ((victim && (victim->fsm().state()->flags & 0x10000000)) ||
        (!soul && ((candidate->object_flags & 0x2000000) || (candidate->common_status & 0x80))))
        return false;
    if ((mask & 0x10000) && !(candidate->state_flags & 2)) {
        const auto* candidate_motion = candidate->source_motion();
        if (!match_->terrain || !candidate_motion ||
            candidate_motion->position.z - terrain_surface(*match_->terrain, candidate_motion->position.x, candidate_motion->position.y) > 2)
            return false;
    }
    if ((mask & 0x100000) && victim && (victim->status_flags & 0x40))
        return false;
    if ((mask & 0x40000) && (!victim || victim->type == MatchObject::wizard ||
                             (victim->status_flags & 0x100) || (victim->fsm().state()->flags & 0x2000)))
        return false;
    if (data)
        for (const auto name : data->blocked_effects) {
            if (!name.value)
                break;
            if (candidate->named(name))
                return false;
        }
    // Bit 0x800 (structure sites): an unreserved primary building with definition bit 0x10.
    if ((mask & 0x800) &&
        (!structure || structure->primary || !(structure->building_flags & 0x10) || structure->reserved))
        return false;
    if (mask & 0x81000) {
        if (soul) {
            if ((soul->soul_flags & 0xc0000000) || !allegiance ||
                !(soul->eligible_sides & allegiance->identity))
                return false;
        } else if (mask & 0x80000) {
            if (candidate->allegiance != allegiance)
                return false;
        } else if (!friendly(candidate))
            return false;
    }
    if (mask & 0x2000) {
        if (soul) {
            if ((soul->soul_flags & 0xc0000000) ||
                (allegiance && (soul->eligible_sides & allegiance->identity)))
                return false;
        } else if (friendly(candidate))
            return false;
    }
    if ((mask & 0x83000) && structure && !(structure->building_flags & 0x55b7))
        return false; // The native friendly/enemy building filter excludes scenery structures.
    return true;
}
bool CreatureActor::request_cast(FourCC id, MatchObject* target, std::optional<Vec3> point, bool moving) {
    if (type != MatchObject::wizard)
        throw Error("Wizard cast requests require a wizard");
    casting.requested = find_spell(id);
    casting.accepted = false;
    casting.error = {};
    effect_target = target;
    if (point)
        casting.point = *point;
    else if (target && target->source_motion())
        casting.point = target->source_motion()->position;
    fsm_->event(moving ? 41 : 12);
    return casting.accepted;
}
bool CreatureActor::request_ability(FourCC id, MatchObject* target, std::optional<Vec3> point) {
    casting.requested = find_spell(id);
    casting.accepted = false;
    casting.error = {};
    if (!casting.requested || casting.requested->book)
        return false;
    effect_target = target;
    if (point)
        casting.point = *point;
    else if (target && target->source_motion())
        casting.point = target->source_motion()->position;
    return fsm_->event(13) != 0; // Ability initiation; the animation cue starts the actual spell.
}
bool CreatureActor::issue_order(const CreatureOrders::Input& input, int insertion, bool marker_mode) {
    const bool accepted =
        orders.issue(input, insertion, stats.base[0], marker_mode || (match_ && match_->cinematic));
    if (accepted && input.kind)
        direct_controls.reset();
    return accepted;
}
bool CreatureActor::cast_supported(const SpellDefinition& definition) {
    if (definition.kind == 2 || definition.kind == 16)
        return true;
    const auto* spell = definition.spell.get();
    return spell && (spell->run_id == spell_function_id("HealRun") || spell->run_id == spell_function_id("FireballRun") ||
                     spell->run_id == spell_function_id("GenericShootStart") ||
                     spell->run_id == spell_function_id("SacSpellStart") ||
                     spell->run_id == spell_function_id("SpeedupStart") ||
                     spell->run_id == spell_function_id("GuardianSpellRun") ||
                     spell->run_id == spell_function_id("LightningSpellStart") ||
                     spell->run_id == spell_function_id("FlashOrbRun") ||
                     spell->run_id == spell_function_id("RockRun") ||
                     spell->run_id == spell_function_id("InsectSwarmStart") ||
                     spell->run_id == spell_function_id("TeleportSpell") ||
                     spell->run_id == spell_function_id("EtherealFormRun") ||
                     spell->run_id == spell_function_id("FireformRun") ||
                     spell->run_id == spell_function_id("SkinOfStoneStart") ||
                     spell->run_id == spell_function_id("RunAirShield") ||
                     spell->run_id == spell_function_id("ProtectiveSwarmStart") ||
                     spell->run_id == spell_function_id("GraspingVinesRun") ||
                     spell->run_id == spell_function_id("ChainsOfFireRun") ||
                     spell->run_id == spell_function_id("FreezeSpellStart") ||
                     spell->run_id == spell_function_id("LethargyRun") ||
                     spell->run_id == spell_function_id("DecomposeStart") ||
                     spell->run_id == spell_function_id("RainbowStart") ||
                     spell->run_id == spell_function_id("Dragonfire") ||
                     spell->run_id == spell_function_id("EruptRun") ||
                     spell->run_id == spell_function_id("ChainLightningStart") ||
                     spell->run_id == spell_function_id("AnimateDeadRun") ||
                     spell->run_id == spell_function_id("RainFrogRun") ||
                     spell->run_id == spell_function_id("PyroExplosionRun") ||
                     spell->run_id == spell_function_id("HaloOfEarthRun") ||
                     spell->run_id == spell_function_id("SpiritWindRun") ||
                     spell->run_id == spell_function_id("DemonicRiftRun") ||
                     spell->run_id == spell_function_id("HealingAuraRun") ||
                     spell->run_id == spell_function_id("FireWallRun") ||
                     spell->run_id == spell_function_id("SpikeWallStart") ||
                     spell->run_id == spell_function_id("FrozenGroundStart") ||
                     spell->run_id == spell_function_id("WailingWallRun") ||
                     spell->run_id == spell_function_id("VineWallRun") ||
                     spell->run_id == spell_function_id("RainOfFireRun") ||
                     spell->run_id == spell_function_id("FenceRun") ||
                     spell->run_id == spell_function_id("PlagueRun") ||
                     spell->run_id == spell_function_id("MeteorShowerRun") ||
                     spell->run_id == spell_function_id("CharmRun") ||
                     spell->run_id == spell_function_id("BlindRageRun") ||
                     spell->run_id == spell_function_id("BovineInterventionStart") ||
                     spell->run_id == spell_function_id("CloudKillRun") ||
                     spell->run_id == spell_function_id("IVapRun") ||
                     spell->run_id == spell_function_id("DivineRetributionStart") ||
                     spell->run_id == spell_function_id("VolcanoRun") ||
                     spell->run_id == spell_function_id("BoreRun") ||
                     spell->run_id == spell_function_id("TornadoRun") ||
                     spell->run_id == spell_function_id("DeathSpellRun") ||
                     spell->run_id == spell_function_id("FireBowStart") ||
                     spell->run_id == spell_function_id("AirClawpultStart") ||
                     spell->run_id == spell_function_id("FireClawpultStart") ||
                     spell->run_id == spell_function_id("RockClawpultStart") ||
                     spell->run_id == spell_function_id("PoisonDartShoot") ||
                     spell->run_id == spell_function_id("PyromaniacRocketShoot") ||
                     spell->run_id == spell_function_id("GnomeShoot") ||
                     spell->run_id == spell_function_id("TickVortexMissile") ||
                     spell->run_id == spell_function_id("BabyChuckRun") ||
                     spell->run_id == spell_function_id("GutChuckRun") ||
                     spell->run_id == spell_function_id("HailOfEarth") ||
                     spell->run_id == spell_function_id("SilverbackAbility") ||
                     spell->run_id == spell_function_id("RhinocAbility") ||
                     spell->run_id == spell_function_id("QuadrokAbility") ||
                     spell->run_id == spell_function_id("FireTrollAbility") ||
                     spell->run_id == spell_function_id("RedDragonAbility") ||
                     spell->run_id == spell_function_id("FireStomperHeadRun") ||
                     spell->run_id == spell_function_id("EarthStomperRun") ||
                     spell->run_id == spell_function_id("BasiliskRun") ||
                     spell->run_id == spell_function_id("HellMouthRun") ||
                     spell->run_id == spell_function_id("BoulderSquatRun") ||
                     spell->run_id == spell_function_id("LightningChargeAbility") ||
                     spell->run_id == spell_function_id("CallLightningRun") ||
                     spell->run_id == spell_function_id("KlapponStart") ||
                     spell->run_id == spell_function_id("KlapponFireStart") ||
                     spell->run_id == spell_function_id("DeathKlapponStart") ||
                     spell->run_id == spell_function_id("GunheadFuseRun") ||
                     spell->run_id == spell_function_id("GunheadTorchRun") ||
                     spell->run_id == spell_function_id("FirewalkRun") ||
                     spell->run_id == spell_function_id("ArachonLifeStart") ||
                     spell->run_id == spell_function_id("ArachonFireStart") ||
                     spell->run_id == spell_function_id("CagePullRun") ||
                     spell->run_id == spell_function_id("BugzapperRun") ||
                     spell->run_id == spell_function_id("ViscousOilRun") ||
                     spell->run_id == spell_function_id("SoulEaterRun") ||
                     spell->run_id == spell_function_id("GD_ResurrectRun") ||
                     spell->run_id == spell_function_id("ScryStart") ||
                     spell->run_id == spell_function_id("LifeShieldRun") ||
                     spell->run_id == spell_function_id("ProtectorRun") ||
                     spell->run_id == spell_function_id("RunAwayStart") ||
                     spell->run_id == spell_function_id("StealthStart") ||
                     spell->run_id == spell_function_id("RockFormRun") ||
                     spell->run_id == spell_function_id("PlayDeadStart") ||
                     spell->run_id == spell_function_id("IcarusRun"));
}
bool CreatureActor::start_cast() {
    const auto reject = [&](FourCC reason) {
        if (trace_ai() && (kind_flags & 1))
            log(std::format("Cast rejected: {} side {} reason {}", definition().id.text(),
                            allegiance ? allegiance->identity : 0u, reason.text()));
        casting.error = reason;
        fsm_->event(std::bit_cast<int>(cc("abrt").value));
        return false;
    };
    const auto* entry = casting.requested ? cast_entry(*casting.requested) : nullptr;
    if (!entry || !match_->spell_registry)
        return reject(cc("mana"));
    const auto* definition = match_->spell_registry->find(entry->id);
    const bool summon = definition && definition->kind == 2;
    const bool structure = definition && definition->kind == 16;
    if (!definition || !cast_supported(*definition))
        throw Error("Spell body is not implemented for casting: " + entry->id.text());
    if (summon) {
        if (!match_->prepare_creature || !match_->create_creature)
            throw Error("Summoning requires bound creature resources and factory");
        match_->prepare_creature(definition->id);
    }
    if (structure && (!match_->create_building || !match_->terrain))
        throw Error("Structure casting requires a bound building factory and terrain");
    if (definition->spell && definition->spell->run_id == spell_function_id("SacSpellStart")) {
        if (!match_->prepare_creature || !match_->create_creature || !match_->terrain)
            throw Error("SacSpellStart requires bound doctor resources, factory and terrain");
        match_->prepare_creature(cc("sacd"));
    }
    if (definition->spell && definition->spell->run_id == spell_function_id("FireballRun")) {
        const FourCC model{static_cast<unsigned>(definition->spell->integer(cc("tmod"), cc("bold").value))};
        if (!match_->terrain || !match_->spatial || !match_->spell_models.contains(model) ||
            !match_->spell_models.at(model))
            throw Error("FireballRun requires bound terrain, spatial and model services");
    }
    if (definition->spell && definition->spell->run_id == spell_function_id("GenericShootStart") &&
        (!match_->terrain || !match_->spatial))
        throw Error("GenericShootStart requires bound terrain and spatial services");
    if (network_identity) {
        if (!cast_target(*definition))
            return reject(cc("targ"));
        const auto check = cast_check(entry->id);
        if (check.code)
            return reject(check.reason);
    }
    state_flags = (state_flags & 0x1fffffe7) | 8;
    casting.active = casting.requested;
    casting.definition = definition->id;
    casting.data = definition->spell;
    const auto [cost, ticks] = cast_cost(*definition);
    if (type == MatchObject::wizard) {
        casting.remaining = casting.total = ticks;
        casting.cost = cost;
    } else {
        mana = std::max(add(mana, std::bit_cast<int>(0u - static_cast<unsigned>(cost))), 0);
    }
    const auto reload = [&](FourCC id) {
        const auto* item = match_->spell_registry->find(id);
        return item ? item->cooldown : 0;
    };
    if (type == MatchObject::wizard) {
        const auto extra = add(casting.total, 120);
        spells.cooldown(definition->id, 60, extra, reload);
        for (auto& book : wizard_spells)
            book.cooldown(definition->id, 60, extra, reload);
    } else
        spells.cooldown(definition->id, 0, 0, reload);
    owner = this;
    if (!(definition->spell ? definition->spell->targets : definition->targets) && !effect_target)
        effect_target = this;
    stack.clear();
    if (summon && !find_summon_point(*match_, casting.point)) {
        fsm_->event(7);
        return false;
    }
    if (definition->spell && (definition->spell->property_flags & 0x80000))
        release_cast();
    SpellActor::bind_functions(*match_);
    match_->assign(*this, summon      ? SpellActor::summon_function
                          : structure ? SpellActor::structure_function
                                      : definition->spell->run_id);
    cast_visual_requests.push_back({definition->id});
    casting.accepted = true;
    return true;
}
void CreatureActor::tick_cast(int ticks) {
    if (ticks < 0)
        throw Error("Negative casting tick count");
    // 0x4823c0, called by WizardRunSpellcasting: the audio host starts Xinc only once the
    // previous creature voice has finished. This is a sequence of one-shots, not a loop.
    if (casting.active && casting.definition.value)
        incantation_requested = true;
    const int previous = std::max(casting.remaining, 0);
    casting.remaining = add(casting.remaining, -ticks);
    const int current = std::max(casting.remaining, 0);
    if (current >= previous)
        return;
    if (!casting.total)
        throw Error("Zero native cast duration");
    int delta =
        add(product(current, casting.cost) / casting.total,
            std::bit_cast<int>(0u - static_cast<unsigned>(product(previous, casting.cost) / casting.total)));
    delta = std::bit_cast<int>(static_cast<unsigned>(delta) << 16);
    change_mana_fixed(delta);
}
void CreatureActor::change_mana_fixed(int delta) {
    if (delta > 0)
        delta = std::bit_cast<int>(
            static_cast<unsigned>(static_cast<std::int64_t>(stats.current[20] * 0.001 * delta)));
    casting.mana_fraction = add(casting.mana_fraction, delta);
    if (static_cast<unsigned>(casting.mana_fraction) & 0xffff0000) {
        mana = std::min(std::max(add(mana, casting.mana_fraction >> 16), 0), maximum_mana);
        casting.mana_fraction &= 0xffff;
    }
}
int CreatureActor::knock_down(int mode, float seconds) {
    if (mode != 1 && knockdown_cooldown > 0)
        return knockdown_cooldown;
    const int result = fsm_->event(10);
    if (result == 2) {
        const double ticks = seconds * 60.0;
        knockdown_cooldown =
            !std::isfinite(ticks) || ticks >= 9223372036854775808.0 || ticks < -9223372036854775808.0
                ? 0
                : std::bit_cast<int>(static_cast<unsigned>(static_cast<std::int64_t>(ticks)));
        return knockdown_cooldown;
    }
    return result;
}
void CreatureActor::abort_cast() {
    state_flags = (state_flags & 0x1fffffe7) | 0x10;
}
void CreatureActor::release_cast() {
    state_flags = (state_flags & ~0x18u) | 0x20000000;
}
std::pair<std::uint32_t, float> CreatureActor::cast_state() const {
    const auto state = state_flags & 0x20000010;
    if (type != MatchObject::wizard || (state & 0x20000000))
        return {state, 1.0f};
    if (state)
        return {state, 0.0f};
    const int total = add(casting.total, 90);
    const int elapsed =
        add(total, std::bit_cast<int>(0u - static_cast<unsigned>(add(casting.remaining, 90))));
    const double value = static_cast<double>(elapsed) / total;
    return {state, static_cast<float>(value >= 1 ? 1 : value)};
}
} // namespace sacrifice
