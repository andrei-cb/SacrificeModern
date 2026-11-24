#include "sacrifice/spell_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/spatial.hpp"
#include "sacrifice/spell_data.hpp"
#include "sacrifice/spell_combat.hpp"
#include <algorithm>
#include <cmath>
#include <tuple>

namespace sacrifice {
namespace {
int add(int a, int b) {
    return std::bit_cast<int>(static_cast<unsigned>(a) + static_cast<unsigned>(b));
}
FourCC healing_name() {
    return FourCC::from_text("HEAL");
}
// Half the xy diagonal of an object's motion box (+596..+616), as the heal visuals size themselves.
double box_radius(const MatchObject& object) {
    const auto* source = object.source_motion();
    if (!source)
        return 0;
    const double dy = static_cast<double>(source->box_min.y) - source->box_max.y,
                 dx = static_cast<double>(source->box_min.x) - source->box_max.x;
    return std::sqrt(dy * dy + dx * dx) * 0.5;
}
} // namespace
void SpellActor::activated(Match& match) {
    if (spatial_)
        spatial_->unbind(motion);
    spatial_ = nullptr;
    model_.reset();
    stage = remaining = rate = next_pulse = linger = 0;
    age = emission = 0;
    live_particles = 0;
    motion = {};
    visual_requests.clear();
    cast_alpha = 0;
    point = {};
    flight_remaining = 0;
    network_identity = -1;
    had_target = false;
    const bool projectile = effect == Effect::fireball || effect == Effect::generic_shoot;
    friendly_ignore_time = projectile && match.projectile_friendly_fire ? 2.0f : 0;
    model_scale = model_frame = scale_target = scale_rate = 0;
    model_id = {};
    summon_id = {};
    summon_god = 1;
    clear_preview = {};
    shockwave = {};
    portal = {};
    beam = {};
    crossline = {};
    sprite = {};
    sprite_overlay = {};
    ability = {};
    bone_attachment = {};
    surface = {};
    revival_role = RevivalRole::flight;
    altar_ring = {};
    altar_cloud = {};
    altar_streamer = {};
    impact_target = nullptr;
    generic = {};
    poison_state = {};
    sacrifice = {};
    ruin = {};
    structure = {};
    relay = {};
    speedup = {};
    echo = {};
    cylinder = {};
    building_portal = {};
    teleport_radius = 0;
    spell_id = {};
    particle_systems.clear();
    terrain_cells.clear();
    land_block = {}; // A recycled INERT_RemovedEdge would otherwise draw its slab on the next spell.
    land_mound = {};
    land_wave = {};
    light = {};
    host = {};
    sound_requests.clear();
    if (projectile)
        motion.collision_mask |= 0x20000;
    bind_functions(match);
    static constexpr std::array<std::string_view, 5> handlers{"HealingRun", "HealRun", "FireballRun",
                                                              "GenericShootStart", "PoisonRun"};
    match.assign(
        *this, effect == Effect::summon                  ? summon_function
               : effect == Effect::corpse_fade           ? corpse_fade_function
               : effect == Effect::doctor_orifice        ? doctor_orifice_function
               : effect == Effect::doctor_beam           ? doctor_beam_function
               : effect == Effect::altar_process         ? altar_process_function
               : effect == Effect::altar_ruin            ? altar_ruin_function
               : effect == Effect::mana_relay            ? mana_relay_function
               : effect == Effect::manalith_host         ? manalith_host_function
               : effect == Effect::fountain_host         ? fountain_host_function
               : effect == Effect::altar_host            ? altar_host_function
               : effect == Effect::cast_hands            ? cast_hands_function
               : effect == Effect::glow                  ? glow_function
               : effect == Effect::fireball_trail        ? fireball_trail_function
               : effect == Effect::fireball_blast        ? fireball_blast_function
               : effect == Effect::fireball_smoke        ? fireball_smoke_function
               : effect == Effect::fireball_smolder      ? fireball_smolder_function
               : effect == Effect::shockwave             ? shockwave_function
               : effect == Effect::summon_column         ? summon_column_function
               : effect == Effect::summon_cloud          ? summon_cloud_function
               : effect == Effect::doctor_sparks         ? doctor_sparks_function
               : effect == Effect::altar_motes           ? altar_motes_function
               : effect == Effect::doctor_beam_sparks    ? doctor_beam_sparks_function
               : effect == Effect::desecration_globe     ? desecration_globe_function
               : effect == Effect::desecration_beam      ? desecration_beam_function
               : effect == Effect::altar_cloud           ? altar_cloud_function
               : effect == Effect::altar_streamer        ? altar_streamer_function
               : effect == Effect::altar_cloud_bolt      ? altar_cloud_bolt_function
               : effect == Effect::spell_hit             ? spell_hit_function
               : effect == Effect::ruin_ring             ? ruin_ring_function
               : effect == Effect::blood                 ? blood_function
               : effect == Effect::burning_limbs         ? burning_limbs_function
               : effect == Effect::respawn_body          ? respawn_body_function
               : effect == Effect::respawn_wisps         ? respawn_wisps_function
               : effect == Effect::speedup               ? spell_function_id("SpeedupStart")
               : effect == Effect::teleport              ? spell_function_id("TeleportSpell")
               : effect == Effect::guardian              ? spell_function_id("GuardianSpellRun")
               : effect == Effect::guardian_beam         ? spell_function_id("GuardianEffectRun")
               : effect == Effect::speedup_echo          ? speedup_echo_function
               : effect == Effect::teleport_portal       ? teleport_portal_function
               : effect == Effect::teleport_burst        ? teleport_burst_function
               : effect == Effect::teleport_sweep        ? teleport_sweep_function
               : effect == Effect::lightning_cast        ? spell_function_id("LightningSpellStart")
               : effect == Effect::lightning_bolt        ? spell_function_id("LightningSpellRun")
               : effect == Effect::flash_orb             ? spell_function_id("FlashOrbRun")
               : effect == Effect::rock                  ? spell_function_id("RockRun")
               : effect == Effect::insect_swarm          ? spell_function_id("InsectSwarmStart")
               : effect == Effect::boon                  ? spell_function_id("BoonRun")
               : effect == Effect::healing_shower        ? spell_function_id("HealingShowerProc")
               : effect == Effect::dust_cloud            ? spell_function_id("DustCloudProc")
               : effect == Effect::steam_cloud           ? spell_function_id("SteamCloudProc")
               : effect == Effect::poison_cloud          ? spell_function_id("PoisonCloudProc")
               : effect == Effect::gib                   ? spell_function_id("GibProc")
               : effect == Effect::gib_chunk             ? gib_chunk_function
               : effect == Effect::beacon                ? spell_function_id("BeaconRun")
               : effect == Effect::beacon_source         ? spell_function_id("BeaconSourceRun")
               : effect == Effect::ghost                 ? spell_function_id("GhostProc")
               : effect == Effect::remove_land           ? spell_function_id("RemoveLandProc")
               : effect == Effect::removed_edge          ? removed_edge_function
               : effect == Effect::ethereal_form         ? spell_function_id("EtherealFormRun")
               : effect == Effect::fireform              ? spell_function_id("FireformRun")
               : effect == Effect::skin_of_stone         ? spell_function_id("SkinOfStoneStart")
               : effect == Effect::air_shield            ? spell_function_id("RunAirShield")
               : effect == Effect::protective_swarm      ? spell_function_id("ProtectiveSwarmStart")
               : effect == Effect::grasping_vines        ? spell_function_id("GraspingVinesRun")
               : effect == Effect::chains_of_fire        ? spell_function_id("ChainsOfFireRun")
               : effect == Effect::freeze                ? spell_function_id("FreezeSpellStart")
               : effect == Effect::lethargy              ? spell_function_id("LethargyRun")
               : effect == Effect::decompose             ? spell_function_id("DecomposeStart")
               : effect == Effect::rainbow               ? spell_function_id("RainbowStart")
               : effect == Effect::dragonfire            ? spell_function_id("Dragonfire")
               : effect == Effect::erupt                 ? spell_function_id("EruptRun")
               : effect == Effect::chain_lightning       ? spell_function_id("ChainLightningStart")
               : effect == Effect::animate_dead          ? spell_function_id("AnimateDeadRun")
               : effect == Effect::rain_frogs            ? spell_function_id("RainFrogRun")
               : effect == Effect::pyro_explosion        ? spell_function_id("PyroExplosionRun")
               : effect == Effect::halo_of_earth         ? spell_function_id("HaloOfEarthRun")
               : effect == Effect::spirit_wind           ? spell_function_id("SpiritWindRun")
               : effect == Effect::demonic_rift          ? spell_function_id("DemonicRiftRun")
               : effect == Effect::rain_frog             ? spell_function_id("FrogRun")
               : effect == Effect::halo_rock             ? spell_function_id("HaloRockRun")
               : effect == Effect::rift_demon            ? spell_function_id("DemonicRiftDemonRun")
               : effect == Effect::cast_spiral           ? cast_spiral_function
               : effect == Effect::animate_dead_streamer ? spell_function_id("AnimateDeadStreamerRun")
               : effect == Effect::skin_of_stone_casting ? spell_function_id("SkinOfStoneCasting")
               : effect == Effect::turn_to_stone         ? spell_function_id("TurnToStoneRun")
               : effect == Effect::mrm_debris            ? spell_function_id("INERT_RunMRMDebris")
               : effect == Effect::spinny_particles      ? spell_function_id("INERT_RunSpinnyParticles")
               : effect == Effect::vines_visual          ? vines_visual_function
               : effect == Effect::freeze_ice            ? freeze_ice_function
               : effect == Effect::rock_fragments        ? rock_fragments_function
               : effect == Effect::healing_aura          ? spell_function_id("HealingAuraRun")
               : effect == Effect::fire_wall             ? spell_function_id("FireWallRun")
               : effect == Effect::spike_wall            ? spell_function_id("SpikeWallStart")
               : effect == Effect::frozen_ground         ? spell_function_id("FrozenGroundStart")
               : effect == Effect::wailing_wall          ? spell_function_id("WailingWallRun")
               : effect == Effect::vine_wall             ? spell_function_id("VineWallRun")
               : effect == Effect::rain_of_fire          ? spell_function_id("RainOfFireRun")
               : effect == Effect::fence                 ? spell_function_id("FenceRun")
               : effect == Effect::plague                ? spell_function_id("PlagueRun")
               : effect == Effect::meteor_shower         ? spell_function_id("MeteorShowerRun")
               : effect == Effect::fire_wall_bit         ? spell_function_id("FireWallBits")
               : effect == Effect::fire_wall_earth       ? spell_function_id("FireWallEarthEffect")
               : effect == Effect::wailing_demon         ? spell_function_id("WailingWallDemon")
               : effect == Effect::vine_wall_earth       ? spell_function_id("VineWallEarthEffect")
               : effect == Effect::spike_wall_hit        ? spell_function_id("SpikeWallHitNTT")
               : effect == Effect::frozen_ground_field   ? spell_function_id("FrozenGroundRun")
               : effect == Effect::plague_bomb           ? spell_function_id("PlagueBombRun")
               : effect == Effect::meteor                ? spell_function_id("MeteorRun")
               : effect == Effect::rain_fire_drop        ? rain_fire_drop_function
               : effect == Effect::fence_post            ? fence_post_function
               : effect == Effect::charm                 ? spell_function_id("CharmRun")
               : effect == Effect::blind_rage            ? spell_function_id("BlindRageRun")
               : effect == Effect::bovine                ? spell_function_id("BovineInterventionStart")
               : effect == Effect::cloud_kill            ? spell_function_id("CloudKillRun")
               : effect == Effect::intestinal_vap        ? spell_function_id("IVapRun")
               : effect == Effect::divine_retribution    ? spell_function_id("DivineRetributionStart")
               : effect == Effect::volcano               ? spell_function_id("VolcanoRun")
               : effect == Effect::bore                  ? spell_function_id("BoreRun")
               : effect == Effect::tornado               ? spell_function_id("TornadoRun")
               : effect == Effect::death                 ? spell_function_id("DeathSpellRun")
               : effect == Effect::charm_heart           ? spell_function_id("CharmHeartRun")
               : effect == Effect::rage_creature         ? spell_function_id("BlindRageCreatureRun")
               : effect == Effect::cloud_kill_bolt       ? spell_function_id("CloudKillBoltRun")
               : effect == Effect::ivap_sprite           ? spell_function_id("IVapSpriteRun")
               : effect == Effect::divine_pillar         ? spell_function_id("DivineRetributionRun")
               : effect == Effect::volcano_erupt         ? spell_function_id("VolcanoEruptRun")
               : effect == Effect::volcano_spatter       ? spell_function_id("VolcanoSpatterRun")
               : effect == Effect::volcano_smoke         ? spell_function_id("VolcanoSmokeRun")
               : effect == Effect::volcano_lava          ? spell_function_id("VolcanoLavaBallRun")
               : effect == Effect::tornado_cast          ? spell_function_id("TornadoCastRun")
               : effect == Effect::tornado_ground        ? spell_function_id("TornadoGroundRun")
               : effect == Effect::bore_land             ? spell_function_id("BoreLandFXRun")
               : effect == Effect::fire_bow              ? spell_function_id("FireBowStart")
               : effect == Effect::air_clawpult          ? spell_function_id("AirClawpultStart")
               : effect == Effect::fire_clawpult         ? spell_function_id("FireClawpultStart")
               : effect == Effect::rock_clawpult         ? spell_function_id("RockClawpultStart")
               : effect == Effect::poison_dart           ? spell_function_id("PoisonDartShoot")
               : effect == Effect::pyromaniac_rocket     ? spell_function_id("PyromaniacRocketShoot")
               : effect == Effect::gnome_shoot           ? spell_function_id("GnomeShoot")
               : effect == Effect::tick_vortex_missile   ? spell_function_id("TickVortexMissile")
               : effect == Effect::baby_chuck            ? spell_function_id("BabyChuckRun")
               : effect == Effect::gut_chuck             ? spell_function_id("GutChuckRun")
               : effect == Effect::hail_of_earth         ? spell_function_id("HailOfEarth")
               : effect == Effect::tick_vortex           ? spell_function_id("TickVortex")
               : effect == Effect::vacuum_effect         ? spell_function_id("VacuumEffectRun")
               : effect == Effect::gut_chuck_drip        ? spell_function_id("GutChuckDripStart")
               : effect == Effect::silverback            ? spell_function_id("SilverbackAbility")
               : effect == Effect::rhinoc                ? spell_function_id("RhinocAbility")
               : effect == Effect::quadrok               ? spell_function_id("QuadrokAbility")
               : effect == Effect::fire_troll            ? spell_function_id("FireTrollAbility")
               : effect == Effect::red_dragon            ? spell_function_id("RedDragonAbility")
               : effect == Effect::fire_stomper_head     ? spell_function_id("FireStomperHeadRun")
               : effect == Effect::earth_stomper         ? spell_function_id("EarthStomperRun")
               : effect == Effect::basilisk              ? spell_function_id("BasiliskRun")
               : effect == Effect::hellmouth             ? spell_function_id("HellMouthRun")
               : effect == Effect::boulder_squat         ? spell_function_id("BoulderSquatRun")
               : effect == Effect::lightning_charge      ? spell_function_id("LightningChargeAbility")
               : effect == Effect::call_lightning        ? spell_function_id("CallLightningRun")
               : effect == Effect::silverback_icespray   ? spell_function_id("SilverbackIcespray")
               : effect == Effect::klappon               ? spell_function_id("KlapponStart")
               : effect == Effect::klappon_fire          ? spell_function_id("KlapponFireStart")
               : effect == Effect::death_klappon         ? spell_function_id("DeathKlapponStart")
               : effect == Effect::gunhead_fuse          ? spell_function_id("GunheadFuseRun")
               : effect == Effect::gunhead_lightning     ? spell_function_id("GunheadLightningRun")
               : effect == Effect::gunhead_torch         ? spell_function_id("GunheadTorchRun")
               : effect == Effect::firewalk              ? spell_function_id("FirewalkRun")
               : effect == Effect::arachon_life          ? spell_function_id("ArachonLifeStart")
               : effect == Effect::arachon_fire          ? spell_function_id("ArachonFireStart")
               : effect == Effect::cage_pull             ? spell_function_id("CagePullRun")
               : effect == Effect::web_pull_hold         ? spell_function_id("WebPullHoldRun")
               : effect == Effect::bugzapper             ? spell_function_id("BugzapperRun")
               : effect == Effect::viscous_oil           ? spell_function_id("ViscousOilRun")
               : effect == Effect::viscous_oil_affect    ? spell_function_id("ViscousOilAffectNTT")
               : effect == Effect::soul_eater            ? spell_function_id("SoulEaterRun")
               : effect == Effect::gd_resurrect          ? spell_function_id("GD_ResurrectRun")
               : effect == Effect::scry                  ? spell_function_id("ScryStart")
               : effect == Effect::life_shield           ? spell_function_id("LifeShieldRun")
               : effect == Effect::protector             ? spell_function_id("ProtectorRun")
               : effect == Effect::run_away              ? spell_function_id("RunAwayStart")
               : effect == Effect::stealth               ? spell_function_id("StealthStart")
               : effect == Effect::rock_form             ? spell_function_id("RockFormRun")
               : effect == Effect::play_dead             ? spell_function_id("PlayDeadStart")
               : effect == Effect::icarus                ? spell_function_id("IcarusRun")
               : effect == Effect::portal_run            ? spell_function_id("PortalRun")
               : effect == Effect::pus_wart              ? spell_function_id("PusWartRun")
               : effect == Effect::mini_volcano          ? spell_function_id("MiniVolcanoRun")
               : effect == Effect::pyro_dynamo           ? spell_function_id("PyroDynamoRun")
               : effect == Effect::pyro_dynamo_flame     ? pyro_dynamo_flame_function
               : effect == Effect::mfry                  ? spell_function_id("MFryRun")
               : effect == Effect::mfry_bracket          ? spell_function_id("MFryBracketRun")
               : effect == Effect::mfry_splash           ? spell_function_id("MFrySplashDamage")
               : effect == Effect::machine               ? spell_function_id("MachineRun")
               : effect == Effect::machine_strut         ? spell_function_id("MachineStrutRun")
               : effect == Effect::machine_shot          ? machine_shot_function
               : effect == Effect::destroy_hut           ? spell_function_id("DestroyHut")
               : effect == Effect::destroy_manalith      ? spell_function_id("DestroyManalith")
               : effect == Effect::generic_trail         ? generic_trail_function
               : effect == Effect::generic_ground_trail  ? generic_ground_trail_function
               : effect == Effect::generic_moving        ? generic_moving_function
               : effect == Effect::generic_static        ? generic_static_function
               : effect == Effect::null_spell            ? null_spell_function
               : effect == Effect::heat_beam             ? spell_function_id("HeatBeamRun")
               : effect == Effect::inert_fire            ? spell_function_id("INERT_RunFire")
               : effect == Effect::dirt_spray            ? spell_function_id("DirtSprayRun")
               : effect == Effect::structure             ? structure_function
                                             : spell_function_id(handlers.at(static_cast<unsigned>(effect))));
}
void SpellActor::start_mound(Match& match, FourCC id, int size, float flat, float outer) {
    const auto profile = match.spell_mounds.find(id);
    if (!match.terrain || profile == match.spell_mounds.end())
        return;
    auto& p = motion.position;
    p.x = static_cast<int>((p.x + 5.0) * .1) * 10.f;
    p.y = static_cast<int>((p.y + 5.0) * .1) * 10.f;
    p.z = static_cast<float>(terrain_surface(*match.terrain, p.x, p.y));
    land_mound = match.terrain->mound(p, size, flat, outer, profile->second);
}
void SpellActor::bind_functions(Match& match) {
    match.functions.try_emplace(spell_function_id("ManahoarManaRun"), [&match](MatchObject& object, int) {
        auto& actor = dynamic_cast<CreatureActor&>(object);
        actor.effect_target = &actor;
        launch_mana_relay(match, actor, std::exchange(actor.initial_run_spell, {}));
        match.assign(actor, 0);
        return 1;
    });
    for (const auto& [name, effect] :
         {std::pair{"LightningSpellStart", Effect::lightning_cast},
          std::pair{"LightningSpellRun", Effect::lightning_bolt}, std::pair{"FlashOrbRun", Effect::flash_orb},
          std::pair{"RockRun", Effect::rock}, std::pair{"InsectSwarmStart", Effect::insect_swarm},
          std::pair{"InsectSwarmRun", Effect::insect_swarm}})
        match.functions.try_emplace(spell_function_id(name),
                                    [&match, effect](MatchObject& object, int ticks) {
                                        if (auto* spell = dynamic_cast<SpellActor*>(&object)) {
                                            spell->effect = effect;
                                            return spell->run_wizard_offense(match, ticks);
                                        }
                                        auto& actor = dynamic_cast<CreatureActor&>(object);
                                        launch_spell(match, actor, effect);
                                        match.assign(actor, 0);
                                        return 0;
                                    });
    for (const auto& [name, effect] :
         {std::pair{"BoonRun", Effect::boon}, std::pair{"HealingShowerProc", Effect::healing_shower},
          std::pair{"DustCloudProc", Effect::dust_cloud}, std::pair{"SteamCloudProc", Effect::steam_cloud},
          std::pair{"PoisonCloudProc", Effect::poison_cloud}, std::pair{"GibProc", Effect::gib},
          std::pair{"BeaconRun", Effect::beacon}, std::pair{"BeaconSourceRun", Effect::beacon_source},
          std::pair{"GhostProc", Effect::ghost}, std::pair{"RemoveLandProc", Effect::remove_land}})
        match.functions.try_emplace(
            spell_function_id(name), [&match, effect](MatchObject& object, int ticks) {
                if (auto* spell = dynamic_cast<SpellActor*>(&object)) {
                    spell->effect = effect;
                    return spell->run_procedures(match, ticks);
                }
                auto& actor = dynamic_cast<CreatureActor&>(object);
                launch_spell(match, actor, effect); // 0x44c1a0 → 0x441bf0 when the caster is not yet a spell.
                match.assign(actor, 0);
                return 0;
            });
    match.functions.try_emplace(gib_chunk_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_procedures(match, ticks);
    });
    match.functions.try_emplace(removed_edge_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_procedures(match, ticks);
    });
    for (const auto& [name, id, effect] :
         {std::tuple{"GenericTrailRun", generic_trail_function, Effect::generic_trail},
          std::tuple{"GenericGroundTrailRun", generic_ground_trail_function, Effect::generic_ground_trail},
          std::tuple{"GenericMovingPsysRun", generic_moving_function, Effect::generic_moving},
          std::tuple{"GenericStaticPsysRun", generic_static_function, Effect::generic_static},
          std::tuple{"NullSpellRun", null_spell_function, Effect::null_spell}}) {
        const auto run = [&match, effect](MatchObject& object, int ticks) {
            if (auto* spell = dynamic_cast<SpellActor*>(&object)) {
                spell->effect = effect;
                return spell->run_generic_particles(match, ticks);
            }
            auto& actor = dynamic_cast<CreatureActor&>(object);
            launch_spell(match, actor, effect);
            match.assign(actor, 0);
            return 0;
        };
        match.functions.try_emplace(spell_function_id(name), run);
        match.functions.try_emplace(id, run);
    }
    match.functions.try_emplace(spell_function_id("GenericOneShotPsysRun"),
                                [&match](MatchObject& object, int ticks) {
                                    return dynamic_cast<SpellActor&>(object).run_spell_hit(match, ticks);
                                });
    match.functions.try_emplace(spell_function_id("GenericDecayingPsysRun"),
                                [&match](MatchObject& object, int ticks) {
                                    auto& spell = dynamic_cast<SpellActor&>(object);
                                    auto& particles = spell.particles(match);
                                    spell.host.time += ticks;
                                    particles.advance(spell.host.time, spell.emission);
                                    spell.emission = std::max(0.f, spell.emission + ticks * spell.host.rate);
                                    if (spell.emission <= 0 && !particles.alive()) {
                                        spell.state_flags |= MatchObject::removed_flag;
                                        return -2;
                                    }
                                    return 1;
                                });
    match.functions.try_emplace(spell_function_id("INERT_RemovedEdge"),
                                [&match](MatchObject& object, int ticks) {
                                    return dynamic_cast<SpellActor&>(object).run_procedures(match, ticks);
                                });
    for (const auto& [name, effect] :
         {std::pair{"EtherealFormRun", Effect::ethereal_form}, std::pair{"FireformRun", Effect::fireform},
          std::pair{"SkinOfStoneStart", Effect::skin_of_stone}, std::pair{"RunAirShield", Effect::air_shield},
          std::pair{"ProtectiveSwarmStart", Effect::protective_swarm},
          std::pair{"ProtectiveSwarmRun", Effect::protective_swarm},
          std::pair{"SkinOfStoneCasting", Effect::skin_of_stone_casting},
          std::pair{"TurnToStoneRun", Effect::turn_to_stone},
          std::pair{"INERT_RunSpinnyParticles", Effect::spinny_particles},
          std::pair{"SpawnSpinnyParticles", Effect::spinny_particles}})
        match.functions.try_emplace(spell_function_id(name),
                                    [&match, effect](MatchObject& object, int ticks) {
                                        if (auto* spell = dynamic_cast<SpellActor*>(&object)) {
                                            spell->effect = effect;
                                            return spell->run_forms(match, ticks);
                                        }
                                        auto& actor = dynamic_cast<CreatureActor&>(object);
                                        launch_spell(match, actor, effect);
                                        match.assign(actor, 0);
                                        return 0;
                                    });
    for (const auto& [name, effect] :
         {std::pair{"GraspingVinesRun", Effect::grasping_vines},
          std::pair{"VinesHoldRun", Effect::grasping_vines},
          std::pair{"ChainsOfFireRun", Effect::chains_of_fire}, std::pair{"FreezeSpellStart", Effect::freeze},
          std::pair{"FreezeSpellRun", Effect::freeze}, std::pair{"LethargyRun", Effect::lethargy},
          std::pair{"DecomposeStart", Effect::decompose}})
        match.functions.try_emplace(spell_function_id(name),
                                    [&match, effect](MatchObject& object, int ticks) {
                                        if (auto* spell = dynamic_cast<SpellActor*>(&object)) {
                                            spell->effect = effect;
                                            return spell->run_control(match, ticks);
                                        }
                                        auto& actor = dynamic_cast<CreatureActor&>(object);
                                        launch_spell(match, actor, effect);
                                        match.assign(actor, 0);
                                        return 0;
                                    });
    match.functions.try_emplace(vines_visual_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_control(match, ticks);
    });
    match.functions.try_emplace(cast_spiral_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_midgame(match, ticks);
    });
    match.functions.try_emplace(freeze_ice_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_control(match, ticks);
    });
    match.functions.try_emplace(rock_fragments_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_wizard_offense(match, ticks);
    });
    for (const auto& [name, effect] :
         {std::pair{"RainbowStart", Effect::rainbow}, std::pair{"RainbowRun", Effect::rainbow},
          std::pair{"Dragonfire", Effect::dragonfire}, std::pair{"EruptRun", Effect::erupt},
          std::pair{"ChainLightningStart", Effect::chain_lightning},
          std::pair{"ChainLightningRun", Effect::chain_lightning},
          std::pair{"AnimateDeadRun", Effect::animate_dead}, std::pair{"RainFrogRun", Effect::rain_frogs},
          std::pair{"PyroExplosionRun", Effect::pyro_explosion},
          std::pair{"HaloOfEarthRun", Effect::halo_of_earth}, std::pair{"SpiritWindRun", Effect::spirit_wind},
          std::pair{"DemonicRiftRun", Effect::demonic_rift}, std::pair{"FrogRun", Effect::rain_frog},
          std::pair{"HaloRockRun", Effect::halo_rock}, std::pair{"DemonicRiftDemonRun", Effect::rift_demon},
          std::pair{"AnimateDeadStreamerRun", Effect::animate_dead_streamer},
          std::pair{"INERT_RunMRMDebris", Effect::mrm_debris}})
        match.functions.try_emplace(spell_function_id(name),
                                    [&match, effect](MatchObject& object, int ticks) {
                                        if (auto* spell = dynamic_cast<SpellActor*>(&object)) {
                                            spell->effect = effect;
                                            return spell->run_midgame(match, ticks);
                                        }
                                        auto& actor = dynamic_cast<CreatureActor&>(object);
                                        launch_spell(match, actor, effect);
                                        match.assign(actor, 0);
                                        return 0;
                                    });
    for (const auto& [name, effect] :
         {std::pair{"HealingAuraRun", Effect::healing_aura}, std::pair{"FireWallRun", Effect::fire_wall},
          std::pair{"SpikeWallStart", Effect::spike_wall},
          std::pair{"FrozenGroundStart", Effect::frozen_ground},
          std::pair{"WailingWallRun", Effect::wailing_wall}, std::pair{"VineWallRun", Effect::vine_wall},
          std::pair{"RainOfFireRun", Effect::rain_of_fire}, std::pair{"FenceRun", Effect::fence},
          std::pair{"PlagueRun", Effect::plague}, std::pair{"MeteorShowerRun", Effect::meteor_shower},
          std::pair{"FireWallBits", Effect::fire_wall_bit},
          std::pair{"FireWallEarthEffect", Effect::fire_wall_earth},
          std::pair{"WailingWallDemon", Effect::wailing_demon},
          std::pair{"VineWallEarthEffect", Effect::vine_wall_earth},
          std::pair{"SpikeWallHitNTT", Effect::spike_wall_hit},
          std::pair{"FrozenGroundRun", Effect::frozen_ground_field},
          std::pair{"PlagueBombRun", Effect::plague_bomb}, std::pair{"MeteorRun", Effect::meteor}})
        match.functions.try_emplace(spell_function_id(name),
                                    [&match, effect](MatchObject& object, int ticks) {
                                        if (auto* spell = dynamic_cast<SpellActor*>(&object)) {
                                            spell->effect = effect;
                                            return spell->run_siege(match, ticks);
                                        }
                                        auto& actor = dynamic_cast<CreatureActor&>(object);
                                        launch_spell(match, actor, effect);
                                        match.assign(actor, 0);
                                        return 0;
                                    });
    match.functions.try_emplace(rain_fire_drop_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_siege(match, ticks);
    });
    match.functions.try_emplace(fence_post_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_siege(match, ticks);
    });
    for (const auto& [name, effect] : {std::pair{"CharmRun", Effect::charm},
                                       std::pair{"BlindRageRun", Effect::blind_rage},
                                       std::pair{"BovineInterventionStart", Effect::bovine},
                                       std::pair{"CloudKillRun", Effect::cloud_kill},
                                       std::pair{"IVapRun", Effect::intestinal_vap},
                                       std::pair{"DivineRetributionStart", Effect::divine_retribution},
                                       std::pair{"DivineRetributionRun", Effect::divine_pillar},
                                       std::pair{"VolcanoRun", Effect::volcano},
                                       std::pair{"BoreRun", Effect::bore},
                                       std::pair{"TornadoRun", Effect::tornado},
                                       std::pair{"DeathSpellRun", Effect::death},
                                       std::pair{"CharmHeartRun", Effect::charm_heart},
                                       std::pair{"BlindRageCreatureRun", Effect::rage_creature},
                                       std::pair{"CloudKillBoltRun", Effect::cloud_kill_bolt},
                                       std::pair{"CloudKillBoltStart", Effect::cloud_kill_bolt},
                                       std::pair{"IVapSpriteRun", Effect::ivap_sprite},
                                       std::pair{"VolcanoEruptRun", Effect::volcano_erupt},
                                       std::pair{"VolcanoSpatterRun", Effect::volcano_spatter},
                                       std::pair{"VolcanoSmokeRun", Effect::volcano_smoke},
                                       std::pair{"VolcanoLavaBallRun", Effect::volcano_lava},
                                       std::pair{"TornadoCastRun", Effect::tornado_cast},
                                       std::pair{"TornadoGroundRun", Effect::tornado_ground},
                                       std::pair{"BoreLandFXRun", Effect::bore_land}})
        match.functions.try_emplace(spell_function_id(name),
                                    [&match, effect](MatchObject& object, int ticks) {
                                        if (auto* spell = dynamic_cast<SpellActor*>(&object)) {
                                            spell->effect = effect;
                                            return spell->run_ultimate(match, ticks);
                                        }
                                        auto& actor = dynamic_cast<CreatureActor&>(object);
                                        launch_spell(match, actor, effect);
                                        match.assign(actor, 0);
                                        return 0;
                                    });
    for (const auto& [name, effect] :
         {std::pair{"FireBowStart", Effect::fire_bow}, std::pair{"FireBowRun", Effect::fire_bow},
          std::pair{"AirClawpultStart", Effect::air_clawpult},
          std::pair{"FireClawpultStart", Effect::fire_clawpult},
          std::pair{"RockClawpultStart", Effect::rock_clawpult},
          std::pair{"PoisonDartShoot", Effect::poison_dart},
          std::pair{"PyromaniacRocketShoot", Effect::pyromaniac_rocket},
          std::pair{"GnomeShoot", Effect::gnome_shoot},
          std::pair{"TickVortexMissile", Effect::tick_vortex_missile},
          std::pair{"BabyChuckRun", Effect::baby_chuck}, std::pair{"GutChuckRun", Effect::gut_chuck},
          std::pair{"HailOfEarth", Effect::hail_of_earth}, std::pair{"TickVortex", Effect::tick_vortex},
          std::pair{"VacuumEffectRun", Effect::vacuum_effect},
          std::pair{"GutChuckDripStart", Effect::gut_chuck_drip}})
        match.functions.try_emplace(spell_function_id(name),
                                    [&match, effect](MatchObject& object, int ticks) {
                                        if (auto* spell = dynamic_cast<SpellActor*>(&object)) {
                                            spell->effect = effect;
                                            return spell->run_ability(match, ticks);
                                        }
                                        auto& actor = dynamic_cast<CreatureActor&>(object);
                                        launch_spell(match, actor, effect);
                                        match.assign(actor, 0);
                                        return 0;
                                    });
    for (const auto& [name, effect] :
         {std::pair{"SilverbackAbility", Effect::silverback}, std::pair{"RhinocAbility", Effect::rhinoc},
          std::pair{"QuadrokAbility", Effect::quadrok}, std::pair{"FireTrollAbility", Effect::fire_troll},
          std::pair{"RedDragonAbility", Effect::red_dragon},
          std::pair{"FireStomperHeadRun", Effect::fire_stomper_head},
          std::pair{"FireStomperRun", Effect::fire_stomper_head},
          std::pair{"EarthStomperRun", Effect::earth_stomper}, std::pair{"BasiliskRun", Effect::basilisk},
          std::pair{"HellMouthRun", Effect::hellmouth}, std::pair{"BoulderSquatRun", Effect::boulder_squat},
          std::pair{"BoulderSquatSpawn", Effect::boulder_squat},
          std::pair{"LightningChargeAbility", Effect::lightning_charge},
          std::pair{"CallLightningRun", Effect::call_lightning},
          std::pair{"SilverbackIcespray", Effect::silverback_icespray}})
        match.functions.try_emplace(spell_function_id(name),
                                    [&match, effect](MatchObject& object, int ticks) {
                                        if (auto* spell = dynamic_cast<SpellActor*>(&object)) {
                                            spell->effect = effect;
                                            return spell->run_melee(match, ticks);
                                        }
                                        auto& actor = dynamic_cast<CreatureActor&>(object);
                                        launch_spell(match, actor, effect);
                                        match.assign(actor, 0);
                                        return 0;
                                    });
    for (const auto& [name, effect] :
         {std::pair{"KlapponStart", Effect::klappon}, std::pair{"KlapponRun", Effect::klappon},
          std::pair{"KlapponFireStart", Effect::klappon_fire},
          std::pair{"KlapponFireRun", Effect::klappon_fire},
          std::pair{"DeathKlapponStart", Effect::death_klappon},
          std::pair{"GunheadFuseRun", Effect::gunhead_fuse},
          std::pair{"GunheadLightningRun", Effect::gunhead_lightning},
          std::pair{"GunheadTorchRun", Effect::gunhead_torch}, std::pair{"FirewalkRun", Effect::firewalk}})
        match.functions.try_emplace(spell_function_id(name),
                                    [&match, effect](MatchObject& object, int ticks) {
                                        if (auto* spell = dynamic_cast<SpellActor*>(&object)) {
                                            spell->effect = effect;
                                            return spell->run_klappon(match, ticks);
                                        }
                                        auto& actor = dynamic_cast<CreatureActor&>(object);
                                        launch_spell(match, actor, effect);
                                        match.assign(actor, 0);
                                        return 0;
                                    });
    for (const auto& [name, effect] :
         {std::pair{"ArachonLifeStart", Effect::arachon_life},
          std::pair{"ArachonFireStart", Effect::arachon_fire}, std::pair{"CagePullRun", Effect::cage_pull},
          std::pair{"WebPullHoldRun", Effect::web_pull_hold}, std::pair{"BugzapperRun", Effect::bugzapper},
          std::pair{"ViscousOilRun", Effect::viscous_oil},
          std::pair{"ViscousOilAffectNTT", Effect::viscous_oil_affect},
          std::pair{"SoulEaterRun", Effect::soul_eater}, std::pair{"GD_ResurrectRun", Effect::gd_resurrect},
          std::pair{"ScryStart", Effect::scry}})
        match.functions.try_emplace(spell_function_id(name),
                                    [&match, effect](MatchObject& object, int ticks) {
                                        if (auto* spell = dynamic_cast<SpellActor*>(&object)) {
                                            spell->effect = effect;
                                            return spell->run_support(match, ticks);
                                        }
                                        auto& actor = dynamic_cast<CreatureActor&>(object);
                                        launch_spell(match, actor, effect);
                                        match.assign(actor, 0);
                                        return 0;
                                    });
    for (const auto& [name, effect] :
         {std::pair{"LifeShieldRun", Effect::life_shield}, std::pair{"ProtectorRun", Effect::protector},
          std::pair{"RunAwayStart", Effect::run_away}, std::pair{"StealthStart", Effect::stealth},
          std::pair{"RockFormRun", Effect::rock_form}, std::pair{"PlayDeadStart", Effect::play_dead},
          std::pair{"IcarusRun", Effect::icarus}, std::pair{"IcarusSpawnedRun", Effect::icarus}})
        match.functions.try_emplace(spell_function_id(name),
                                    [&match, effect](MatchObject& object, int ticks) {
                                        if (auto* spell = dynamic_cast<SpellActor*>(&object)) {
                                            spell->effect = effect;
                                            return spell->run_defense(match, ticks);
                                        }
                                        auto& actor = dynamic_cast<CreatureActor&>(object);
                                        launch_spell(match, actor, effect);
                                        match.assign(actor, 0);
                                        return 0;
                                    });
    for (const auto& [name, effect] :
         {std::pair{"PortalRun", Effect::portal_run}, std::pair{"PusWartRun", Effect::pus_wart},
          std::pair{"MiniVolcanoRun", Effect::mini_volcano}, std::pair{"PyroDynamoRun", Effect::pyro_dynamo},
          std::pair{"MFryRun", Effect::mfry}, std::pair{"MFryBracketRun", Effect::mfry_bracket},
          std::pair{"MFrySplashDamage", Effect::mfry_splash}, std::pair{"MachineRun", Effect::machine},
          std::pair{"MachineStrutRun", Effect::machine_strut}, std::pair{"DestroyHut", Effect::destroy_hut},
          std::pair{"DestroyManalith", Effect::destroy_manalith}})
        match.functions.try_emplace(spell_function_id(name), [&match, effect](MatchObject& object,
                                                                              int ticks) {
            if (auto* spell = dynamic_cast<SpellActor*>(&object)) {
                spell->effect = effect;
                return spell->run_shr(match, ticks);
            }
            auto& actor = dynamic_cast<CreatureActor&>(object);
            launch_spell(match, actor, effect); // 0x441bf0 mode 1; buildings spawn these as type-32 children.
            match.assign(actor, 0);
            return 0;
        });
    match.functions.try_emplace(pyro_dynamo_flame_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_shr(match, ticks);
    });
    match.functions.try_emplace(machine_shot_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_shr(match, ticks);
    });
    match.functions.try_emplace(skin_absorb_function, [](MatchObject& object, int) {
        auto& spell = dynamic_cast<SpellActor&>(object);
        if (spell.stack.size() < 2)
            return 1;
        const int amount = std::bit_cast<int>(spell.stack[1]);
        spell.linger += amount;
        spell.stack[1] = static_cast<std::uint32_t>(static_cast<std::int64_t>(amount * .25));
        return 1;
    });
    match.functions.try_emplace(skin_death_function, [&match](MatchObject& object, int) {
        // 0x446a40 -> 0x446900, shared by Skin of Stone ('PROT') and Turn to Stone ('PARA').
        auto& spell = dynamic_cast<SpellActor&>(object);
        const auto name = FourCC::from_text(spell.effect == Effect::turn_to_stone ? "PARA" : "PROT");
        if (auto* target = dynamic_cast<CreatureActor*>(spell.effect_target)) {
            if (target->named(name) == &spell)
                target->grayscale = false;
            match.unwatch(*target, 0, spell.reference(), &spell);
            match.unwatch(*target, 2, spell.reference(), &spell);
            target->unname(name, &spell);
        }
        spell.stage = 240;
        return 1;
    });
    match.functions.try_emplace(stone_damage_function, [](MatchObject& object, int) {
        // 0x447880: bank the hit for the chip burst and let a fifth of it through.
        auto& spell = dynamic_cast<SpellActor&>(object);
        if (spell.stack.size() < 2)
            return 1;
        const int amount = std::bit_cast<int>(spell.stack[1]);
        spell.linger += amount;
        spell.stack[1] = static_cast<std::uint32_t>(static_cast<std::int64_t>(amount * .2));
        return 1;
    });
    match.functions.try_emplace(freeze_damage_function, [](MatchObject& object, int) {
        // 0x4216e0: any positive damage breaks Freeze on the next tick.
        auto& spell = dynamic_cast<SpellActor&>(object);
        if (spell.stack.size() >= 2 && std::bit_cast<int>(spell.stack[1]) > 0)
            spell.remaining = 0;
        return 1;
    });
    match.functions.try_emplace(air_shield_absorb_function, [](MatchObject& object, int) {
        auto& spell = dynamic_cast<SpellActor&>(object);
        if (spell.stack.size() < 2 || !spell.data)
            return 1;
        int amount = std::bit_cast<int>(spell.stack[1]);
        const int type = std::bit_cast<int>(spell.stack[0]);
        if ((type & 7) == 3 || (type & 7) == 4)
            amount = static_cast<int>(static_cast<std::int64_t>(amount * .5));
        amount = static_cast<int>(static_cast<std::int64_t>(amount * spell.data->speed));
        spell.stack[1] = static_cast<std::uint32_t>(amount);
        return 1;
    });
    match.functions.try_emplace(protective_swarm_absorb_function, [&match](MatchObject& object, int) {
        auto& spell = dynamic_cast<SpellActor&>(object);
        if (spell.stack.size() < 2 || !spell.data)
            return 1;
        // PSwarmTargetDamaged 0x433080: scale the damage by 'speed'; a hit with an attacker flags the block
        // burst at 0x44E530's point, the swarm's edge toward the attacker.
        const int amount = std::bit_cast<int>(spell.stack[1]);
        spell.stack[1] = static_cast<std::uint32_t>(static_cast<std::int64_t>(amount * spell.data->speed));
        const auto* attacker = spell.stack.size() >= 3 && spell.stack[2] ? match.resolve(spell.stack[2]) : nullptr;
        const auto* body = attacker ? attacker->source_motion() : nullptr;
        if (!body)
            return 1;
        const auto extent = [](const ObjectMotion& m) {
            return (m.box_max.x - m.box_min.x + m.box_max.y - m.box_min.y + m.box_max.z - m.box_min.z) / 6;
        };
        const auto* own = spell.effect_target ? spell.effect_target->source_motion() : nullptr;
        const float near = own ? extent(*own) : 0, far = extent(*body);
        Vec3 from = spell.motion.position, to = body->position;
        spell_target_point(from, spell.effect_target);
        spell_target_point(to, attacker);
        Vec3 d{to.x - from.x, to.y - from.y, to.z - from.z};
        const float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (length != 0)
            d = {d.x / length, d.y / length, d.z / length};
        const float gap = length - far, step = gap >= near ? near : (gap + near) * .5f;
        spell.point = {from.x + d.x * step, from.y + d.y * step, from.z + d.z * step};
        spell.host.fade = 1;
        return 1;
    });
    match.functions.try_emplace(speedup_echo_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_speedup_echo(match, ticks);
    });
    for (const auto id : {teleport_portal_function, teleport_burst_function, teleport_sweep_function})
        match.functions.try_emplace(id, [&match](MatchObject& object, int ticks) {
            return dynamic_cast<SpellActor&>(object).run_teleport_visual(match, ticks);
        });
    for (const auto id : {respawn_body_function, respawn_wisps_function})
        match.functions.try_emplace(id, [&match](MatchObject& object, int ticks) {
            return dynamic_cast<SpellActor&>(object).run_respawn_host(match, ticks);
        });
    match.functions.try_emplace(burning_limbs_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_burning_limbs(match, ticks);
    });
    match.functions.try_emplace(ruin_ring_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_ruin_ring(match, ticks);
    });
    match.functions.try_emplace(blood_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_blood(match, ticks);
    });
    match.functions.try_emplace(spell_hit_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_spell_hit(match, ticks);
    });
    match.functions.try_emplace(altar_cloud_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_altar_cloud(match, ticks);
    });
    match.functions.try_emplace(altar_streamer_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_altar_streamer(match, ticks);
    });
    match.functions.try_emplace(altar_cloud_bolt_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_altar_cloud_bolt(match, ticks);
    });
    match.functions.try_emplace(desecration_globe_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_desecration_globe(match, ticks);
    });
    match.functions.try_emplace(desecration_beam_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_desecration_beam(match, ticks);
    });
    match.functions.try_emplace(spell_function_id("HeatBeamRun"), [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_heat_beam(match, ticks);
    });
    match.functions.try_emplace(spell_function_id("INERT_RunFire"), [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_inert_fire(match, ticks);
    });
    match.functions.try_emplace(spell_function_id("DirtSprayRun"), [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_dirt_spray(match, ticks);
    });
    for (const auto id : {doctor_sparks_function, altar_motes_function, doctor_beam_sparks_function})
        match.functions.try_emplace(id, [&match](MatchObject& object, int ticks) {
            return dynamic_cast<SpellActor&>(object).run_sacrifice_host(match, ticks);
        });
    match.functions.try_emplace(shockwave_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_shockwave(match, ticks);
    });
    for (const auto id : {summon_column_function, summon_cloud_function})
        match.functions.try_emplace(id, [&match](MatchObject& object, int ticks) {
            return dynamic_cast<SpellActor&>(object).run_summon_host(match, ticks);
        });
    for (const auto id : {fireball_trail_function, fireball_blast_function, fireball_smoke_function,
                          fireball_smolder_function})
        match.functions.try_emplace(id, [&match](MatchObject& object, int ticks) {
            return dynamic_cast<SpellActor&>(object).run_fireball_host(match, ticks);
        });
    match.functions.try_emplace(structure_function, [&match](MatchObject& object, int ticks) {
        if (auto* spell = dynamic_cast<SpellActor*>(&object)) {
            spell->effect = Effect::structure;
            return spell->run_structure(match, ticks);
        }
        auto* actor = dynamic_cast<CreatureActor*>(&object);
        if (!actor)
            throw Error("Structure casting requires a creature or spell context");
        launch_structure(match, *actor); // 0x441bf0 mode 1.
        match.assign(*actor, 0);
        return 0;
    });
    match.functions.try_emplace(mana_relay_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_mana_relay(match, ticks);
    });
    for (const auto id : {manalith_host_function, fountain_host_function, altar_host_function})
        match.functions.try_emplace(id, [&match](MatchObject& object, int ticks) {
            return dynamic_cast<SpellActor&>(object).run_building_host(match, ticks);
        });
    match.functions.try_emplace(cast_hands_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_cast_hands(match, ticks);
    });
    match.functions.try_emplace(glow_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_glow(match, ticks);
    });
    match.functions.try_emplace(altar_ruin_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_altar_ruin(match, ticks);
    });
    match.functions.try_emplace(doctor_orifice_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_doctor_orifice(match, ticks);
    });
    match.functions.try_emplace(doctor_beam_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_doctor_beam(match, ticks);
    });
    match.functions.try_emplace(altar_process_function, [&match](MatchObject& object, int ticks) {
        return dynamic_cast<SpellActor&>(object).run_altar_process(match, ticks);
    });
    match.functions.try_emplace(spell_function_id("SacSpellStart"), [&match](MatchObject& object, int) {
        auto& actor = dynamic_cast<CreatureActor&>(object);
        launch_doctor_orifice(match, actor);
        match.assign(actor, 0);
        return 0;
    });
    match.functions.try_emplace(
        spell_function_id("SACDOCRunSacrifice"), [&match](MatchObject& object, int ticks) {
            return dynamic_cast<CreatureActor&>(object).run_doctor_circle(match, ticks);
        });
    match.functions.try_emplace(corpse_fade_function, [&match](MatchObject& object, int ticks) {
        auto* spell = dynamic_cast<SpellActor*>(&object);
        if (!spell)
            throw Error("Corpse retirement requires a spell context");
        return spell->run_corpse_fade(match, ticks);
    });
    match.functions.try_emplace(summon_function, [&match](MatchObject& object, int ticks) {
        if (auto* spell = dynamic_cast<SpellActor*>(&object)) {
            spell->effect = Effect::summon;
            return spell->run_summon(match, ticks);
        }
        auto* actor = dynamic_cast<CreatureActor*>(&object);
        if (!actor)
            throw Error("Summoning requires a creature or spell context");
        launch_summon(match, *actor);
        match.assign(*actor, 0);
        return 0;
    });
    const auto id = spell_function_id("HealingRun");
    match.functions.try_emplace(id, [&match](MatchObject& object, int ticks) {
        auto* spell = dynamic_cast<SpellActor*>(&object);
        if (!spell)
            throw Error("HealingRun requires a spell-object context");
        return spell->run_healing(match, ticks);
    });
    match.functions.try_emplace(spell_function_id("HealRun"), [&match](MatchObject& object, int ticks) {
        if (auto* spell = dynamic_cast<SpellActor*>(&object)) {
            spell->effect = Effect::heal_cast;
            return spell->run_heal_cast(match, ticks);
        }
        auto* actor = dynamic_cast<CreatureActor*>(&object);
        if (!actor)
            throw Error("HealRun requires a creature or spell context");
        launch_heal_cast(match, *actor);
        match.assign(*actor, 0);
        return 0;
    });
    match.functions.try_emplace(spell_function_id("FireballRun"), [&match](MatchObject& object, int ticks) {
        if (auto* spell = dynamic_cast<SpellActor*>(&object)) {
            spell->effect = Effect::fireball;
            return spell->run_fireball(match, ticks);
        }
        auto* actor = dynamic_cast<CreatureActor*>(&object);
        if (!actor)
            throw Error("FireballRun requires a creature or spell context");
        launch_fireball(match, *actor);
        match.assign(*actor, 0);
        return 0;
    });
    match.functions.try_emplace(spell_function_id("GenericShootStart"), [&match](MatchObject& object, int) {
        if (auto* spell = dynamic_cast<SpellActor*>(&object)) {
            spell->effect = Effect::generic_shoot;
            return spell->start_generic_shoot(match);
        }
        auto* actor = dynamic_cast<CreatureActor*>(&object);
        if (!actor)
            throw Error("GenericShootStart requires a creature or spell context");
        launch_generic_shoot(match, *actor);
        match.assign(*actor, 0);
        return 0;
    });
    for (const auto& [name, effect] :
         {std::pair{"SpeedupStart", Effect::speedup}, std::pair{"TeleportSpell", Effect::teleport},
          std::pair{"GuardianSpellRun", Effect::guardian}})
        match.functions.try_emplace(
            spell_function_id(name), [&match, effect](MatchObject& object, int ticks) {
                if (auto* spell = dynamic_cast<SpellActor*>(&object)) {
                    spell->effect = effect;
                    return spell->effect == Effect::speedup    ? spell->run_speedup(match, ticks)
                           : spell->effect == Effect::teleport ? spell->run_teleport(match, ticks)
                                                               : spell->run_guardian(match, ticks);
                }
                auto* actor = dynamic_cast<CreatureActor*>(&object);
                if (!actor)
                    throw Error("Spell start requires a creature or spell context");
                launch_spell(match, *actor, effect); // 0x441bf0 mode 1.
                match.assign(*actor, 0);
                return 0;
            });
    match.functions.try_emplace(spell_function_id("GuardianEffectRun"),
                                [&match](MatchObject& object, int ticks) {
                                    return dynamic_cast<SpellActor&>(object).run_guardian_beam(match, ticks);
                                });
    match.functions.try_emplace(spell_function_id("GenericShootRun"),
                                [&match](MatchObject& object, int ticks) {
                                    auto* spell = dynamic_cast<SpellActor*>(&object);
                                    if (!spell)
                                        throw Error("GenericShootRun requires a spell context");
                                    return spell->run_generic_shoot(match, ticks);
                                });
    match.functions.try_emplace(spell_function_id("PoisonRun"), [&match](MatchObject& object, int ticks) {
        auto* spell = dynamic_cast<SpellActor*>(&object);
        if (!spell)
            throw Error("PoisonRun requires a spell context");
        return spell->run_poison(match, ticks);
    });
}
SpellActor& SpellActor::launch_heal_cast(Match& match, CreatureActor& actor) {
    auto& object = match.create(
        MatchObject::spell, [] { return std::make_unique<SpellActor>(); }, &actor,
        [&](MatchObject& object) {
            auto* spell = dynamic_cast<SpellActor*>(&object);
            if (!spell)
                throw Error("Spell pool object has an incompatible host type");
            spell->effect = Effect::heal_cast;
            spell->data = actor.casting.data;
        });
    auto& spell = static_cast<SpellActor&>(object);
    spell.owner = actor.owner;
    spell.effect_target = actor.effect_target;
    spell.motion.position = actor.motion.position;
    spell.motion.yaw = actor.motion.yaw;
    spell.motion.orientation = actor.motion.orientation;
    spell.object_flags |= 0x10000;
    if (actor.allegiance)
        actor.allegiance->attach(spell);
    spell.run(match, 0); // Native mode 1 runs the child immediately, then clears the parent handler.
    return spell;
}
int SpellActor::run_heal_cast(Match& match, int ticks) {
    const auto remove = [&] {
        state_flags |= removed_flag;
        return -2;
    };
    if (!stage) {
        // HealRun 0x43f9c0 stage 0 needs both a caster (+460) and a target (+476); it reads the cast state
        // straight from the caster rather than through 0x441d20, so a casterless 0x453600 Heal does nothing.
        if (!owner || !effect_target)
            return remove();
        visual_requests.push_back({VisualRequest::Kind::cast_generic});
        visual_requests.push_back({VisualRequest::Kind::caster_glow});
        visual_requests.push_back({VisualRequest::Kind::target_glow});
        visual_requests.push_back({VisualRequest::Kind::cast_sound});
        launch_cast_hands(match, *this);
        const auto scale = [](double radius) {
            return static_cast<float>(std::clamp(radius * 0.2, 0.35, 1.0));
        };
        const double caster = owner ? box_radius(*owner) : 0, target = box_radius(*effect_target);
        if (owner)
            launch_glow(match, *this, 0x1a012008, "glo2", static_cast<float>(caster), 2, scale(caster), 2,
                        owner, 1);
        launch_glow(match, *this, 0x1a012008, "glo2", static_cast<float>(target + target), 0, scale(target),
                    2, effect_target, 1);
        stage = 1;
        cast_alpha = 0;
        light = {true, motion.position, {1, 1, 0}, 0, 1, 25}; // 0x44ca20 with unk_4CB8EC.
    }
    if (stage == 1) {
        auto* actor = dynamic_cast<CreatureActor*>(owner);
        if (!actor)
            stage = 240;
        else {
            const auto [state, alpha] = actor->cast_state();
            cast_alpha = alpha;
            if (state == 0x10)
                stage = 240;
            else if (state == 0x20000000)
                stage = 2;
        }
    } else if (stage == 2) {
        if (effect_target && (effect_target->kind_flags & 5)) {
            auto* victim = dynamic_cast<CreatureActor*>(effect_target);
            if (!victim || !data)
                throw Error("HealRun has an invalid target or definition");
            victim->heal(match, data->amount, 256, data->amount);
        }
        stage = 240;
    } else if (stage == 240) {
        const double alpha = static_cast<double>(cast_alpha) - ticks * (1.0 / 60.0);
        cast_alpha = static_cast<float>(alpha);
        if (alpha <= 0)
            return remove();
    }
    if (owner && owner->source_motion()) {
        const auto& source = *owner->source_motion();
        motion.position = {source.position.x + source.reference_center.x,
                           source.position.y + source.reference_center.y,
                           source.position.z + source.reference_center.z};
    }
    light.position = motion.position;
    light.intensity = cast_alpha;
    return 1;
}
SpellActor* SpellActor::heal(Match& match, CreatureActor& actor, int amount, int value_rate, int limit) {
    if (actor.stats.current[5] >= actor.stats.base[5] || !actor.alive())
        return nullptr;
    auto* named = actor.named(healing_name());
    auto* effect = dynamic_cast<SpellActor*>(named);
    if (named && !effect)
        throw Error("HEAL attachment has an incompatible host type");
    if (!effect) {
        auto& object = match.create(
            MatchObject::spell, [] { return std::make_unique<SpellActor>(); }, &actor,
            [](MatchObject& object) {
                auto* spell = dynamic_cast<SpellActor*>(&object);
                if (!spell)
                    throw Error("Spell pool object has an incompatible host type");
                spell->effect = Effect::healing;
                spell->data.reset();
            });
        effect = dynamic_cast<SpellActor*>(&object);
        if (!effect)
            throw Error("Spell pool object has an incompatible host type");
        effect->effect = Effect::healing;
        effect->owner = actor.owner;
        effect->effect_target = &actor;
        effect->motion.position = actor.motion.position;
        effect->motion.yaw = actor.motion.yaw;
        effect->motion.orientation = actor.motion.orientation;
        effect->object_flags |= 0x10000;
        if (actor.allegiance)
            actor.allegiance->attach(*effect);
        effect->run(match, 1); // 0x43f610 explicitly initializes the new effect with one tick.
    }
    if (limit) {
        if (effect->remaining < limit)
            effect->remaining = std::min(add(effect->remaining, amount), limit);
    } else
        effect->remaining = add(effect->remaining, amount);
    effect->rate = std::max(effect->rate, value_rate);
    return effect;
}
SpellActor* CreatureActor::heal(Match& match, int amount, int rate, int limit) {
    return SpellActor::heal(match, *this, amount, rate, limit);
}
int SpellActor::run_healing(Match& match, int ticks) {
    if (ticks < 0)
        throw Error("Negative healing tick count");
    auto* actor = dynamic_cast<CreatureActor*>(effect_target);
    if (stage) {
        if (!actor || !(actor->kind_flags & 5) || !actor->alive()) {
            effect_target = nullptr;
            actor = nullptr;
            stage = 240;
        } else if (remaining)
            stage = 1; // A new dose revives a fading effect without resetting its pulse deadline.
    }
    if (!stage) {
        if (!actor) {
            state_flags |= removed_flag;
            return -2;
        }
        visual_requests.push_back({VisualRequest::Kind::healing_particles});
        // 0x436910(psys, 0x1a012008, 'glo2', 1, size, 2, 1, 2, target): rising from the feet to twice
        // the box top over the two-second life, drawn in the effect's frame (flag 1 cleared).
        auto& system = particles(match);
        particle_material(match, system, 0x1a012008, "glo2");
        const float size = static_cast<float>(std::clamp(box_radius(*actor) * 0.5, 0.5, 2.5)), life = 2;
        system.set_property(129, &size, &life);
        const auto& box = actor->motion;
        const float low[3]{box.box_min.x, box.box_min.y, box.box_min.z},
            high[3]{box.box_max.x, box.box_max.y, box.box_min.z};
        system.set_property(2, low, high);
        const double rise = (2.0 * box.box_max.z - box.box_min.z) * 2 / ((life * 60.0) * (life * 60.0));
        const float accel_low[3]{0, 0, static_cast<float>(rise * 0.95)},
            accel_high[3]{0, 0, static_cast<float>(rise * 1.05)};
        system.set_property(4, accel_low, accel_high);
        system.set_flags(0, 1);
        stage = 1;
        next_pulse = add(phase, 16);
        actor->name_reference(healing_name(), this);
        visual_requests.push_back({VisualRequest::Kind::healing_sound});
    } else if (stage == 1) {
        if (phase >= next_pulse) {
            next_pulse = add(phase, 16); // At most one pulse; excess elapsed ticks are discarded.
            const int amount = std::min(rate >> 2, remaining);
            actor->stats.changed |= 0x20;
            const int health = add(actor->stats.current[5], amount);
            actor->stats.current[5] = static_cast<std::int16_t>(
                std::min(std::max(health, 0), static_cast<int>(actor->stats.base[5])));
            remaining = add(remaining, std::bit_cast<int>(0u - static_cast<unsigned>(amount)));
            if (actor->stats.current[5] == actor->stats.base[5])
                remaining = 0;
        }
        linger = 120;
        emission = 1;
        if (!remaining)
            stage = 240;
    } else if (stage == 240) {
        emission = 0;
        remaining = rate = 0;
        if (!live_particles) {
            if (!actor) {
                state_flags |= removed_flag;
                return -2;
            }
            linger = add(linger, -ticks);
            if (linger <= 0) {
                state_flags |= removed_flag;
                return -2;
            }
        }
    }
    if (actor)
        motion.position = actor->motion.position;
    age = static_cast<float>(static_cast<double>(age) + ticks);
    if (!particle_systems.empty())
        particle_systems.front().advance(age, emission);
    return 1;
}
} // namespace sacrifice
