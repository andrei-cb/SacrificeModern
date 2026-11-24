#pragma once
#include "effect_primitives.hpp"
#include "lighting.hpp"
#include "match.hpp"
#include "motion.hpp"
#include "particles.hpp"

namespace sacrifice {
class CreatureActor;
class BuildingActor;
class SpatialWorld;
struct CreatureClip;
struct SpellData;
struct SpellDefinition;
struct MatchSide;
// Native type-32 spell object. Effect models/audio are still requests; health logic runs inline.
class SpellActor : public MatchObject {
  public:
    enum class Effect {
        healing,
        heal_cast,
        fireball,
        generic_shoot,
        poison,
        summon,
        corpse_fade,
        doctor_orifice,
        doctor_beam,
        altar_process,
        altar_ruin,
        structure,
        mana_relay,
        manalith_host,
        fountain_host,
        altar_host,
        cast_hands,
        glow,
        fireball_trail,
        fireball_blast,
        fireball_smoke,
        fireball_smolder,
        shockwave,
        summon_column,
        summon_cloud,
        doctor_sparks,
        altar_motes,
        doctor_beam_sparks,
        desecration_globe,
        desecration_beam,
        altar_cloud,
        altar_streamer,
        altar_cloud_bolt,
        spell_hit,
        ruin_ring,
        blood,
        burning_limbs,
        respawn_body,
        respawn_wisps,
        speedup,
        teleport,
        guardian,
        guardian_beam,
        speedup_echo,
        teleport_portal,
        teleport_burst,
        teleport_sweep,
        lightning_cast,
        lightning_bolt,
        flash_orb,
        rock,
        insect_swarm,
        boon,
        healing_shower,
        dust_cloud,
        steam_cloud,
        poison_cloud,
        gib,
        gib_chunk,
        beacon,
        beacon_source,
        ghost,
        remove_land,
        ethereal_form,
        fireform,
        skin_of_stone,
        air_shield,
        protective_swarm,
        grasping_vines,
        chains_of_fire,
        freeze,
        lethargy,
        decompose,
        rainbow,
        dragonfire,
        erupt,
        chain_lightning,
        animate_dead,
        rain_frogs,
        pyro_explosion,
        halo_of_earth,
        spirit_wind,
        demonic_rift,
        rain_frog,
        halo_rock,
        rift_demon,
        cast_spiral,
        animate_dead_streamer,
        skin_of_stone_casting,
        spinny_particles,
        vines_visual,
        freeze_ice,
        rock_fragments,
        healing_aura,
        fire_wall,
        spike_wall,
        frozen_ground,
        wailing_wall,
        vine_wall,
        rain_of_fire,
        fence,
        plague,
        meteor_shower,
        fire_wall_bit,
        fire_wall_earth,
        wailing_demon,
        vine_wall_earth,
        spike_wall_hit,
        frozen_ground_field,
        plague_bomb,
        meteor,
        rain_fire_drop,
        fence_post,
        charm,
        blind_rage,
        bovine,
        cloud_kill,
        intestinal_vap,
        divine_retribution,
        volcano,
        bore,
        tornado,
        death,
        charm_heart,
        rage_creature,
        cloud_kill_bolt,
        ivap_sprite,
        divine_pillar,
        volcano_erupt,
        volcano_spatter,
        volcano_smoke,
        volcano_lava,
        tornado_cast,
        tornado_ground,
        bore_land,
        fire_bow,
        air_clawpult,
        fire_clawpult,
        rock_clawpult,
        poison_dart,
        pyromaniac_rocket,
        gnome_shoot,
        tick_vortex_missile,
        baby_chuck,
        gut_chuck,
        hail_of_earth,
        tick_vortex,
        vacuum_effect,
        gut_chuck_drip,
        silverback,
        rhinoc,
        quadrok,
        fire_troll,
        red_dragon,
        fire_stomper_head,
        earth_stomper,
        basilisk,
        hellmouth,
        boulder_squat,
        lightning_charge,
        call_lightning,
        silverback_icespray,
        klappon,
        klappon_fire,
        death_klappon,
        gunhead_fuse,
        gunhead_lightning,
        gunhead_torch,
        firewalk,
        arachon_life,
        arachon_fire,
        cage_pull,
        web_pull_hold,
        bugzapper,
        viscous_oil,
        viscous_oil_affect,
        soul_eater,
        gd_resurrect,
        scry,
        life_shield,
        protector,
        run_away,
        stealth,
        rock_form,
        play_dead,
        icarus,
        portal_run,
        pus_wart,
        mini_volcano,
        pyro_dynamo,
        pyro_dynamo_flame,
        mfry,
        mfry_bracket,
        mfry_splash,
        machine,
        machine_strut,
        machine_shot,
        destroy_hut,
        destroy_manalith,
        removed_edge,
        generic_trail,
        generic_ground_trail,
        generic_moving,
        generic_static,
        null_spell,
        heat_beam,
        inert_fire,
        dirt_spray,
        turn_to_stone, // TurnToStoneRun 0x4478b0 (@stn, Basilisk petrify).
        mrm_debris     // INERT_RunMRMDebris 0x438590: a tumbling model chunk with a trail (0x438600).
    };
    // Private native handler identity, used only as a key in the reconstructed dispatcher.
    static constexpr std::uintptr_t summon_function = 0x435000;
    static constexpr std::uintptr_t corpse_fade_function = 0x4348d0;
    static constexpr std::uintptr_t doctor_orifice_function = 0x44ac90;
    static constexpr std::uintptr_t doctor_beam_function = 0x4495e0;
    static constexpr std::uintptr_t altar_process_function = 0x4484e0;
    static constexpr std::uintptr_t altar_ruin_function = 0x43caf0;
    static constexpr std::uintptr_t structure_function = 0x43c370;
    static constexpr std::uintptr_t mana_relay_function = 0x4911c0;
    static constexpr std::uintptr_t manalith_host_function = 0x43b5c0;
    static constexpr std::uintptr_t fountain_host_function = 0x43b8c0;
    static constexpr std::uintptr_t altar_host_function = 0x43bb10;
    static constexpr std::uintptr_t cast_hands_function = 0x435c60;
    static constexpr std::uintptr_t glow_function = 0x436660;
    static constexpr std::uintptr_t fireball_trail_function = 0x437eb0;
    static constexpr std::uintptr_t fireball_blast_function = 0x43b160;
    static constexpr std::uintptr_t fireball_smoke_function = 0x43b030;
    static constexpr std::uintptr_t fireball_smolder_function = 0x43b4c0;
    static constexpr std::uintptr_t shockwave_function = 0x439d60;
    static constexpr std::uintptr_t summon_column_function = 0x436840;
    static constexpr std::uintptr_t summon_cloud_function = 0x436f00;
    static constexpr std::uintptr_t doctor_sparks_function = 0x44aa90;
    static constexpr std::uintptr_t altar_motes_function = 0x4490d0;
    static constexpr std::uintptr_t doctor_beam_sparks_function = 0x449770;
    static constexpr std::uintptr_t desecration_globe_function = 0x44b350;
    static constexpr std::uintptr_t desecration_beam_function = 0x44b210;
    static constexpr std::uintptr_t altar_cloud_function = 0x448dd0;
    static constexpr std::uintptr_t altar_streamer_function = 0x449330;
    static constexpr std::uintptr_t altar_cloud_bolt_function = 0x437270;
    static constexpr std::uintptr_t spell_hit_function = 0x4388b0;
    static constexpr std::uintptr_t ruin_ring_function = 0x437730;
    static constexpr std::uintptr_t blood_function = 0x45c790;
    static constexpr std::uintptr_t burning_limbs_function = 0x45c8b0;
    static constexpr std::uintptr_t respawn_body_function = 0x43ad00;
    static constexpr std::uintptr_t respawn_wisps_function = 0x43aee0;
    static constexpr std::uintptr_t speedup_echo_function = 0x441310;
    static constexpr std::uintptr_t teleport_portal_function = 0x43a100;
    static constexpr std::uintptr_t teleport_burst_function = 0x4389d0;
    static constexpr std::uintptr_t teleport_sweep_function = 0x43a370;
    static constexpr std::uintptr_t gib_chunk_function = 0x4398a0; // GibProc children; not a named SPEL run.
    static constexpr std::uintptr_t skin_absorb_function = 0x446a60;
    static constexpr std::uintptr_t skin_death_function = 0x446a40;
    static constexpr std::uintptr_t stone_damage_function = 0x447880; // TurnToStoneDamageInterrupt.
    static constexpr std::uintptr_t air_shield_absorb_function = 0x41f6e0;
    static constexpr std::uintptr_t vines_visual_function = 0x42bd10;
    static constexpr std::uintptr_t freeze_ice_function = 0x421700;
    static constexpr std::uintptr_t cast_spiral_function = 0x43a840; // Caster-orbit streamer (0x43A760).
    static constexpr std::uintptr_t rock_fragments_function = 0x439270;
    static constexpr std::uintptr_t rain_fire_drop_function = 0x428e40;
    static constexpr std::uintptr_t fence_post_function = 0x420ff0;
    static constexpr std::uintptr_t protective_swarm_absorb_function = 0x433080;
    static constexpr std::uintptr_t pyro_dynamo_flame_function = 0x43d140; // unnamed PyroDynamo child.
    static constexpr std::uintptr_t machine_shot_function = 0x43ee40;      // unnamed MachineRun child.
    static constexpr std::uintptr_t removed_edge_function = 0x43ac70;      // INERT_RemovedEdge.
    static constexpr std::uintptr_t generic_trail_function = 0x437920;     // GenericTrailRun.
    static constexpr std::uintptr_t generic_ground_trail_function = 0x4379d0;
    static constexpr std::uintptr_t generic_moving_function = 0x438960; // GenericMovingPsysRun.
    static constexpr std::uintptr_t generic_static_function = 0x438900; // GenericStaticPsysRun.
    static constexpr std::uintptr_t null_spell_function = 0x435b30;     // NullSpellRun.
    ~SpellActor() override;
    Effect effect = Effect::healing;
    ObjectMotion motion;
    const ObjectMotion* source_motion() const override {
        return &motion;
    }
    // HealingRun context offsets relative to object +24, separate from the common object clock.
    int stage{}, remaining{}, rate{}, next_pulse{}, linger{};
    float age{}, emission{};
    // The eventual particle host supplies its current active-particle count. Without a host it is 0.
    unsigned live_particles{};
    std::shared_ptr<const SpellData> data;
    float cast_alpha{};
    Vec3 point;
    int flight_remaining{}, network_identity{-1};
    bool had_target{};
    enum class RevivalRole { flight, body, streamer } revival_role{};
    float friendly_ignore_time{}, model_scale{}, model_frame{}, scale_target{}, scale_rate{};
    FourCC model_id;
    FourCC summon_id;
    int summon_god{1};
    std::function<void()> clear_preview;
    struct Shockwave {
        FourCC texture;
        std::uint32_t material{}, color{0xffffffff};
        float progress{}, radius{1}, end_radius{}, start_alpha{}, end_alpha{}, frame{};
        unsigned frames{1};
        Vec3 axes{1, 1, 1}; // SPHERE::SetParams accepts separate ellipsoid radii.
    } shockwave;
    // SPHERE::Draw / GenerateSphereUV: renderer-space mesh; MODEL quality controls native tessellation.
    Mesh shockwave_mesh(const TextureHeader& texture, float projected_scale) const;
    struct BuildingPortal {
        bool enabled{};
        FourCC texture;
        float radius{6}, opacity{}, spin{};
        std::array<float, 4> phase{}, speed{};
    } building_portal;
    Mesh building_portal_mesh() const; // PortalRun's native type-51 BEZIERCYLINDER.
    struct Portal {
        bool enabled{}, sparks{};
        std::array<float, 8> parameters{}; // SACPRIM radii, ring scales and two texture frames.
        float phase_a{}, phase_b{}, opacity{};
    } portal;
    struct Beam {
        bool enabled{};
        FourCC texture;
        std::uint32_t material{0x1a012008};
        float frame{}, hold{};
        float deviation{}; // 0x4355A0 shape: context +56 deviation and +76 strands; width is always 0.5.
        int strands{1};
        LightningPrimitive primitive;
    } beam;
    struct Crossline {
        // muzzle: points[0] is the flash centre (width fMFx) and points[1] its aim point (MUZZLE primitive).
        bool enabled{}, continuous{}, muzzle{};
        FourCC texture;
        std::uint32_t material{0x1a012008};
        float frame{};
        std::vector<LightningPrimitive::Point> points;
    } crossline;
    struct CurvedSurface {
        enum Kind { none, lathe, tornado, cuboid, ground, rays, vines, tentacle, spikes,
                    fire_wall, cylinder, ring } kind{};
        struct Tentacle {
            std::array<Vec3, 4> curve;
            std::array<float, 4> radii;
        };
        std::array<Tentacle, 2> tentacles;
        struct Vine {
            std::array<Vec3, 4> curve;
            float radius{}, progress{}, speed{1.f / 60};
        };
        std::vector<Vine> vines_data;
        struct Ray {
            float phase{}, speed{};
            std::array<Vec3, 4> base{};
            Vec3 axis;
        };
        std::vector<Ray> rays_data;
        std::array<float, 92> parameters{};
        std::array<float, 8> phases{};
        std::array<FourCC, 3> textures{};
        float opacity{}, frame{};
        std::uint32_t material{0x02011008};
        std::uint32_t color{0xffffffff};
    } surface;
    Mesh curved_surface_mesh(unsigned layer, const TextureHeader& texture, float detail = 1,
                             const Terrain* terrain = nullptr, std::optional<Vec3> eye = {},
                             std::optional<Vec3> camera_right = {}) const;
    void update_tornado_surface(int ticks);
    void tornado_centre(float world_z, Vec2& centre, float& radius) const; // TORNADO::GetCentre 0x1600df90.
    void initialize_spike_surface(Match& match, Vec3 half_extents);
    void add_vine(Vec3 root, Vec3 tip, float radius);
    bool advance_vines(int ticks, float goal);
    struct Sprite {
        bool enabled{};
        FourCC texture;
        std::uint32_t material{0x0a011008}, color{0xffffffff};
        float size{1}, frame{};
        // A game3d FLAG (type 37) instead of a quad: `size` is its height, .75 x as wide, waving with
        // the two phases (FLAG::SetParams), screen oriented.
        bool flag{};
        float phase_x{}, phase_y{};
    } sprite, sprite_overlay;
    struct AbilityFlight {
        float draw_back{}, length{1};
        Vec3 origin, destination;
        int clock{};
    } ability;
    struct BoneAttachment {
        unsigned bone{};
        Vec3 offset;
        bool bound{};
    } bone_attachment;
    std::vector<Vec3> terrain_cells;
    // LANDBLOCK::Create 0x16017990 (game3d.dll): the piece of land that falls out of a removed edge, kept
    // relative to the motion position (identity orientation) and drawn by LANDBLOCK::Draw 0x16018130.
    struct LandBlock {
        struct Point {
            Vec3 position;
            std::uint32_t color{};
        };
        std::array<Point, 9> points{};      // Offset table 0x160553C0: the 8 neighbours, then the centre.
        std::array<std::size_t, 4> cells{}; // Each quadrant's map texture: the cells of points 0, 1, 7, 8.
        struct Face {
            std::uint8_t quadrant{};
            std::array<std::uint8_t, 3> points{}, uvs{};
        };
        std::vector<Face> faces;
        std::vector<std::array<std::uint8_t, 2>> walls; // Perimeter edges, 50 deep, with the edge texture.
    } land_block;
    // Builds `land_block` for land sample (x, y) and moves the spell to it; false when nothing would draw.
    bool build_land_block(const Terrain& terrain, int x, int y);
    // Renderer-space mesh of one quadrant's top face (0-3) or, for 4, the walls.
    [[nodiscard]] Mesh land_block_mesh(unsigned part) const;
    Terrain::Displacement land_mound, land_wave;
    void start_mound(Match& match, FourCC id, int size, float flat, float outer);
    // 0x4355A0 flags: 1 adds the white dword_4CB8F8 light, 2 retraces the target, 4 / 8 pick materials
    // 0x1A010401 / 0x0A011008 instead of 0x1A012008.
    void start_beam(Match& match, Vec3 from, Vec3 to, float deviation, int strands, unsigned flags);
    int advance_beam(Match& match, int ticks); // 0x435840: 1 grow/hold, 2 contact, 3 retract, 0 done.
    struct AltarRing {
        bool enabled{};
        FourCC texture;
        float frame{};
        float radius{}, height{32}, end_radius{};
        // RINGEXP material 1 (0x437730's sixth argument, only Blind Rage's 'trg2'): a flat band from the
        // radius in to `inner`, fading to zero alpha there.
        FourCC second;
        float inner{};
    } altar_ring;
    struct AltarCloud {
        bool enabled{};
        FourCC texture;
        std::uint32_t material{0x0a011008};
        float opacity{}, frame{};
        Vec3 phase, radii;
        int lightning_ticks{300};
        std::shared_ptr<CloudNoise> noise;
    } altar_cloud;
    // INERT_RunLightning 0x437270 setup (0x4355E0): the altar cloud's bolt by default.
    struct InertBolt {
        float deviation{1.5f}, width{.75f}, rate{1 / 30.f}, hold{.5f};
        int strands{2};
        unsigned flags{16}; // 1: light, 4/8: material, 16: hit effect at the end.
    } inert_bolt;
    struct AltarStreamer {
        bool enabled{};
        FourCC texture;
        std::uint32_t material{};
        float phase{};
        int ticks{};
        std::array<Vec3, 4> controls{};
        StreamerPrimitive primitive;
    } altar_streamer;
    int run_altar_cloud(Match& match, int ticks);
    int run_altar_streamer(Match& match, int ticks);
    int run_altar_cloud_bolt(Match& match, int ticks);
    int run_spell_hit(Match& match, int ticks);
    int run_ruin_ring(Match& match, int ticks);
    int run_blood(Match& match, int ticks);
    int run_burning_limbs(Match& match, int ticks);
    static SpellActor& launch_burning_limbs(Match& match, CreatureActor& victim);
    // 0x455e80 (WizardStartRespawnEffect) attaches both hosts with 0x44c230; 0x455ec0 bursts 0x438d60.
    static SpellActor& launch_respawn_host(Match& match, CreatureActor& wizard, Effect effect);
    static SpellActor& launch_respawn_pulse(Match& match, CreatureActor& wizard);
    int run_respawn_host(Match& match, int ticks);
    static SpellActor* emit_blood(Match& match, CreatureActor& victim, MatchObject* source, int count,
                                  float speed);
    Mesh altar_ring_mesh(const TextureHeader& texture) const;
    Mesh altar_ring_band_mesh(const TextureHeader& texture) const;
    void update_altar_ring(Match& match, int ticks);
    int run_desecration_globe(Match& match, int ticks);
    int run_desecration_beam(Match& match, int ticks);
    int run_heat_beam(Match& match, int ticks); // HeatBeamRun 0x41d180.
    Mesh portal_mesh(unsigned material, const TextureHeader& texture, Vec3 eye, Vec3 up) const;
    void update_orifice_visuals(Match& match, int ticks);
    int run_sacrifice_host(Match& match, int ticks);
    static SpellActor& launch_sacrifice_host(Match& match, SpellActor& source, Effect effect);
    // 0x437370 / 0x436160: an INERT_RunLightning arc from `from` to `to` (or `target`), growing over
    // `seconds`, held `hold` seconds, then retracting as fast.
    static SpellActor& launch_inert_lightning(Match& match, SpellActor& source, Vec3 from, Vec3 to,
                                              MatchObject* target, float deviation, float seconds, float hold,
                                              int strands, float width, unsigned flags);
    // 0x435B50(chance) / 0x436160: a moving caster (chance x 546 x speed in 32768 per call) throws a thin
    // spark up from a random ground point inside its box.
    static void caster_sparks(Match& match, SpellActor& spell, float chance);
    MatchObject* impact_target{};
    struct GenericProjectile {
        FourCC type, movement, hit;
        float collision_radius{}, spiral_radius{}, spiral_max{0.25f}, angle{}, angle_step{};
        int mana_rate{};
        bool initialized{};
        Vec3 particle_position;
    } generic;
    struct Poison {
        int resistance_delta{}, accuracy_delta{}, damage_rate{}, damage_type{}, scan_clock{};
        float regeneration{1}, spread_limit{};
    } poison_state;
    struct Sacrifice {
        BuildingActor* altar{};
        FourCC action;
        std::array<CreatureActor*, 4> doctors{};
        Vec3 victim_origin;
        unsigned flags{};
        float lift_angle{}, angular_speed{}, alpha{};
        int outcome{};
    } sacrifice;
    struct Ruin {
        bool tower{};
        float progress{}, scale{1};
    } ruin;
    // 0x43c370 context: stage +20 (shared `stage`), progress +24, building +28 (a cleared reference
    // slot), particle setup +32 and god +36. The site is the spell's effect target.
    struct Structure {
        FourCC definition;
        float progress{}, time{}, parameter{}; // parameter: +4, the particle emission scale.
        BuildingActor* building{};
        bool configured{};
        int god{1};
    } structure;
    // 0x4911c0 context: accumulated ticks a1[4], supply a1[3], shown beam a1[2] and target beam a1[7].
    struct Relay {
        int ticks{};
        float supply{}, beam{}, shown{}, time{}; // a1[6] emitter time.
        std::uint32_t color{};
        bool initialized{};
    } relay;
    // The object's particle primitive (+1056) and any extra systems an effect keeps; drawn by the viewer.
    std::vector<ParticleSystem> particle_systems;
    ParticleSystem& particles(Match& match, unsigned index = 0);
    void particle_material(Match& match, ParticleSystem& system, std::uint32_t flags, const char* texture);
    void particle_material(Match& match, ParticleSystem& system, std::uint32_t flags, FourCC texture);
    // The object's point light (0x4685c0): vtable +20 position, +52 colour, +56 intensity, +28 ranges, +4 on.
    using Light = PointLight;
    Light light;
    // 0x43b5c0 / 0x43b8c0 / 0x43bb10 contexts: emitter time and rate, light fade and the altar's velocity
    // box. 0x435c60 / 0x436660 contexts: time a1[1], rate a1[2] and the cast hands' tick parity counter
    // a1[5].
    struct Host {
        bool initialized{};
        float time{}, rate{}, fade{};
        int ticks{};
    } host;
    bool run(Match& match, int ticks) override;
    void reference_event(Match& match, MatchObject& object, int code) override;
    struct VisualRequest {
        enum class Kind {
            healing_particles,
            healing_sound,
            cast_generic,
            caster_glow,
            target_glow,
            cast_sound,
            fireball_model,
            fireball_light,
            fireball_trail,
            fireball_blast,
            fireball_decal,
            remove_widgets,
            light_remove,
            generic_projectile,
            generic_hit,
            generic_heat,
            generic_swarm,
            poison_particles,
            summon_model,
            summon_effects,
            corpse_particles,
            doctor_orifice,
            doctor_beam,
            doctor_globe,
            altar_ritual,
            altar_demolition,
            structure_cast,
            structure_particles,
            rock_debris
        } kind;
    };
    std::vector<VisualRequest> visual_requests;
    struct SoundRequest {
        FourCC group, sample;
        std::uint32_t emitter{}, flags{};
        bool stop{};
    };
    std::vector<SoundRequest> sound_requests;
    int run_healing(Match& match, int ticks);
    int run_heal_cast(Match& match, int ticks);
    int run_fireball(Match& match, int ticks);
    int start_generic_shoot(Match& match);
    int run_generic_shoot(Match& match, int ticks);
    int run_poison(Match& match, int ticks);
    int run_summon(Match& match, int ticks);
    int run_corpse_fade(Match& match, int ticks);
    int run_doctor_orifice(Match& match, int ticks);
    int run_doctor_beam(Match& match, int ticks);
    int run_altar_process(Match& match, int ticks);
    int run_altar_ruin(Match& match, int ticks);
    int run_structure(Match& match, int ticks);
    int run_mana_relay(Match& match, int ticks);
    int run_building_host(Match& match, int ticks);
    int run_cast_hands(Match& match, int ticks);
    int run_glow(Match& match, int ticks);
    int run_fireball_host(Match& match, int ticks);
    int run_shockwave(Match& match, int ticks);
    int run_summon_host(Match& match, int ticks);
    static SpellActor& launch_shockwave(Match& match, SpellActor& source, float radius);
    // 0x439270: shared rock shatter (scorch, dust splatter and gravity fragments) at the source.
    static void launch_rock_shatter(Match& match, SpellActor& source);
    // 0x439440: gravity fragments thrown from the source body box (or `low`..`high` around it); flags 2 throws
    // them radially, 4 keeps them opaque, 8 shrinks them away (0x439400 is 'shrd', 64, 10, flags 14).
    static void launch_fragment_burst(Match& match, SpellActor& source, FourCC texture, int count, float speed,
                                      unsigned flags = 14, const Vec3* low = nullptr, const Vec3* high = nullptr);
    // 0x439060: one particle every `spacing` around a ground ring of `radius` (least..most), each in a +-width
    // box rising at low..high over `life` seconds.
    static void launch_ground_ring(Match& match, SpellActor& source, std::uint32_t material, FourCC texture,
                                   float radius, float width, float spacing, float low, float high, float life,
                                   int least, int most);
    // 0x439830 / 0x439690: decaying splatter in a +-half box (life and drain split by `fraction`).
    static void launch_dust_splatter(Match& match, SpellActor& source, std::uint32_t material, FourCC texture,
                                     Vec3 centre, Vec3 half, float seconds, float fraction);
    static void launch_god_cast(Match& match, SpellActor& source, int god, unsigned flags);
    // 0x441D20 aborts a cast without a wizard owner (0x453600 side casts have none). Handlers that start
    // with 0x44C0D0 and a separate casterless stage skip the wait instead; this lists their effects.
    static bool casterless_skips_cast(Effect effect);
    // 0x436AC0 rising column around the owner (0x436840 host): drains once the owner's cast ends.
    static SpellActor& launch_caster_column(Match& match, SpellActor& source, std::uint32_t material,
                                            FourCC texture, float rate, float size, float life, float spread,
                                            float height);
    static SpellActor& launch_fireball_visual(Match& match, SpellActor& source, Effect effect);
    static void bind_functions(Match& match);
    static SpellActor& launch_heal_cast(Match& match, CreatureActor& actor);
    // SpeedupStart/SpeedupRun (0x441870/0x441600) and TeleportSpell (0x442b80); engine/spell_buffs.cpp.
    static SpellActor& launch_spell(Match& match, CreatureActor& actor, Effect effect);
    // 0x453600: casterless type-32 from a side trigger (action 134). Owner stays null.
    static SpellActor* launch_side_cast(Match& match, MatchSide& side, const SpellDefinition& definition,
                                        MatchObject* target, Vec3 point);
    // Walls and fence posts stamp navigation occupancy the way buildings do.
    bool stamp_navigation(class Navigation& navigation) const;
    int run_speedup(Match& match, int ticks);
    int run_teleport(Match& match, int ticks);
    int run_guardian(Match& match, int ticks);
    int run_guardian_beam(Match& match, int ticks);
    static SpellActor& launch_guardian_beam(Match& match, CreatureActor& guard);
    int run_speedup_echo(Match& match, int ticks);
    int run_teleport_visual(Match& match, int ticks);
    static SpellActor& launch_teleport_burst(Match& match, CreatureActor& actor, bool arrival);
    void launch_teleport_portals(Match& match);
    struct Echo {
        unsigned lag{}, length{};
        std::shared_ptr<const CreatureClip> clip;
        unsigned frame{};
        int alpha{};
        bool hidden{true};
        std::uint64_t revision{};
    } echo;
    struct Cylinder {
        bool enabled{};
        FourCC texture;
        float radius{}, top{30}, bottom{-5}, alpha{}, frame{};
    } cylinder;
    Mesh cylinder_mesh(const TextureHeader& texture) const;
    int run_wizard_offense(Match& match, int ticks);
    int run_lightning(Match& match, int ticks);
    int run_rock(Match& match, int ticks);
    int run_flash_orb(Match& match, int ticks);
    int run_insect_swarm(Match& match, int ticks);
    // @PRC bodies: BoonRun 0x440b00, HealingShowerProc 0x440a60, DustCloudProc 0x440e10,
    // SteamCloudProc 0x440920, PoisonCloudProc 0x440c00, GibProc 0x440d90,
    // BeaconRun 0x453b70 / BeaconSourceRun 0x453e90, GhostProc 0x4404f0,
    // RemoveLandProc 0x440640, INERT_RemovedEdge 0x43ac70.
    int run_procedures(Match& match, int ticks);
    // Level-2 god forms: EtherealFormRun 0x42b5b0, FireformRun 0x427590, SkinOfStoneStart 0x446d10,
    // RunAirShield 0x41f870, ProtectiveSwarmStart 0x433470 / ProtectiveSwarmRun 0x433120.
    int run_forms(Match& match, int ticks);
    // Level-3 god control: GraspingVinesRun 0x42c2f0 / VinesHoldRun 0x42c090, ChainsOfFireRun 0x4258a0,
    // FreezeSpellStart 0x421b00 / FreezeSpellRun 0x4217c0, LethargyRun 0x432550, DecomposeStart 0x444060.
    int run_control(Match& match, int ticks);
    int run_soul_path(Match& match, int ticks);
    // Level-4/5 god spells: RainbowStart 0x42d070 / RainbowRun 0x42ce40, Dragonfire 0x426430,
    // EruptRun 0x444b70, ChainLightningStart 0x420320 / ChainLightningRun 0x420220,
    // AnimateDeadRun 0x42f4f0, RainFrogRun 0x42d650, PyroExplosionRun 0x426910,
    // HaloOfEarthRun 0x4458d0, SpiritWindRun 0x423590, DemonicRiftRun 0x430840.
    // Children: FrogRun 0x42d390, HaloRockRun 0x445590, DemonicRiftDemonRun 0x430e60,
    // AnimateDeadStreamerRun 0x42f040.
    int run_midgame(Match& match, int ticks);
    // Level-6/7 god spells: HealingAuraRun 0x42c590, FireWallRun 0x4282b0,
    // SpikeWallStart 0x447390, FrozenGroundStart 0x422050, WailingWallRun 0x433d60,
    // VineWallRun 0x42eda0, RainOfFireRun 0x429210, FenceRun 0x4212f0, PlagueRun 0x432eb0,
    // MeteorShowerRun 0x445fd0. Children: FireWallBits 0x427d80, FireWallEarthEffect
    // 0x427fb0, WailingWallDemon 0x433760, VineWallEarthEffect 0x42e990,
    // SpikeWallHitNTT 0x4477a0, FrozenGroundRun 0x4225f0, PlagueBombRun 0x432c10,
    // MeteorRun 0x445d80, rain-of-fire drop 0x428e40, fence post 0x420ff0.
    int run_siege(Match& match, int ticks);
    // Level-8/9 god spells: CharmRun 0x42ae00, BlindRageRun 0x424e60,
    // BovineInterventionStart 0x4437a0, CloudKillRun 0x420a30, IVapRun 0x431ce0,
    // DivineRetributionStart 0x42d840, VolcanoRun 0x42a710, BoreRun 0x443280,
    // TornadoRun 0x4243b0, DeathSpellRun 0x4300a0. Children: CharmHeartRun 0x42b260,
    // BlindRageCreatureRun 0x424b70, CloudKillBoltRun 0x420600, IVapSpriteRun 0x4319f0,
    // DivineRetributionRun 0x42e000, VolcanoEruptRun 0x42a260, VolcanoSpatterRun
    // 0x4295f0, VolcanoSmokeRun 0x429d00, VolcanoLavaBallRun 0x429a60, TornadoCastRun
    // 0x423b50, TornadoGroundRun 0x423dc0, BoreLandFXRun 0x442f00.
    int run_ultimate(Match& match, int ticks);
    // Creature projectiles: FireBowStart 0x415c10 / FireBowRun 0x415a00, AirClawpultStart
    // 0x418aa0, FireClawpultStart 0x419050, RockClawpultStart 0x418d70, PoisonDartShoot
    // 0x416240, PyromaniacRocketShoot 0x415d90, GnomeShoot 0x4164a0, TickVortexMissile
    // 0x41d420, BabyChuckRun 0x419ad0, GutChuckRun 0x419810, HailOfEarth 0x41a7e0.
    // Children: TickVortex 0x41d860, VacuumEffectRun 0x4187f0, GutChuckDripStart 0x4196c0.
    int run_ability(Match& match, int ticks);
    // Melee and specials: SilverbackAbility 0x414160, RhinocAbility 0x414f50,
    // QuadrokAbility 0x41f0f0, FireTrollAbility 0x41efc0, RedDragonAbility 0x413bd0,
    // FireStomperHeadRun 0x41e960, EarthStomperRun 0x41ed50, BasiliskRun 0x41ce20,
    // HellMouthRun 0x414400, BoulderSquatRun 0x4169a0 / BoulderSquatSpawn 0x41acf0,
    // LightningChargeAbility 0x41f410, CallLightningRun 0x41f1e0.
    // Child: SilverbackIcespray 0x413dd0.
    int run_melee(Match& match, int ticks);
    // Klappons and Gunheads: KlapponStart 0x41ada0 / KlapponRun 0x41acf0,
    // KlapponFireStart 0x41afc0 / KlapponFireRun 0x41aea0, DeathKlapponStart
    // 0x413a50, GunheadFuseRun 0x416c90, GunheadTorchRun 0x416b60, FirewalkRun
    // 0x417aa0.
    int run_klappon(Match& match, int ticks);
    // Arachons and support: ArachonLifeStart 0x41cb40, ArachonFireStart 0x41c880,
    // CagePullRun 0x41d990 / WebPullHoldRun 0x41df10, BugzapperRun 0x419e10,
    // ViscousOilRun 0x413790 / ViscousOilAffectNTT 0x413730, SoulEaterRun 0x41e1f0,
    // GD_ResurrectRun 0x41bbb0, ScryStart 0x41c1e0.
    int run_support(Match& match, int ticks);
    // Defensive and AI: LifeShieldRun 0x415370, ProtectorRun 0x41e180,
    // RunAwayStart 0x415340, StealthStart 0x41c060, RockFormRun 0x41aa00,
    // PlayDeadStart 0x41a8b0, IcarusRun 0x412f10 / IcarusSpawnedRun.
    int run_defense(Match& match, int ticks);
    // @SHR: PortalRun 0x43ef50, PusWartRun 0x43f400, MiniVolcanoRun 0x43e3e0,
    // PyroDynamoRun 0x43ce20 / flame 0x43d140, MFryRun 0x43d770 / MFryBracketRun
    // 0x43d480 / MFrySplashDamage 0x43e230, MachineRun 0x43e9c0 / MachineStrutRun
    // 0x43e780 / shot 0x43ee40, DestroyHut 0x43cd50, DestroyManalith 0x43cda0.
    int run_shr(Match& match, int ticks);
    // Generic particle hosts: GenericTrailRun 0x437920, GenericGroundTrailRun 0x4379d0,
    // GenericMovingPsysRun 0x438960, GenericStaticPsysRun 0x438900, NullSpellRun 0x435b30.
    // GenericOneShotPsysRun is spell_hit 0x4388b0; GenericDecayingPsysRun is 0x4389d0.
    int run_generic_particles(Match& match, int ticks);
    // 0x437DD0: GenericTrailRun child with preset 131.
    static SpellActor* launch_generic_trail(Match& match, SpellActor& source, std::uint32_t material,
                                            FourCC texture, float size, float emission, float life,
                                            float speed, float angle);
    // 0x438050 / 0x437EB0: a GenericTrailRun child sized from the source body box (half-diagonal r):
    // preset 129 life `life`, sizes clamp(r * size_factor, .25, 2) * [.9, 1.08] in a +-r * box_factor cube.
    static SpellActor* launch_model_trail(Match& match, SpellActor& source, std::uint32_t material, FourCC texture,
                                          float emission, float life, float box_factor = .3f,
                                          float size_factor = .5f, bool resize = false);
    // 0x437C10 / 0x437A70: a GenericTrailRun child with preset 129 {size, life} in a +-half box (no source
    // body), emitting at `rate`.
    static SpellActor* launch_box_trail(Match& match, SpellActor& source, std::uint32_t material, FourCC texture,
                                        float half, float size, float life, float rate);
    // Ground-following dust (0x437D70 / 0x437C70): bounds, preset 129 and emission rate.
    static SpellActor* launch_ground_trail(Match& match, SpellActor& source, std::uint32_t material,
                                           FourCC texture, Vec3 low, Vec3 high, float emission, float life);
    static void launch_earth_burst(Match& match, SpellActor& source, Vec3 position, float radius,
                                   unsigned count = 48);
    // 0x4375A0: INERT_RunFire (0x437470) one-shot burst with preset 128 and a fading fire light.
    // The 0x435c00 gate always passes for a spell source, whose +20 activation phase trails its +16 phase.
    static SpellActor* launch_inert_fire(Match& match, SpellActor& source, FourCC texture, float size,
                                         float height, float life, float emission, float seconds,
                                         float spread = 0);
    int run_inert_fire(Match& match, int ticks);
    // 0x436DF0 / 0x436B40: type-42 dirt spray under a unit, preset 129 {size, 1.5} in a +-box square,
    // emitting while the owner casts and for `seconds` after release (an abort stops it at once).
    static SpellActor* launch_dirt_spray(Match& match, SpellActor& source, MatchObject* target, FourCC texture,
                                         bool blend, float box, float size, float seconds);
    int run_dirt_spray(Match& match, int ticks);
    // 0x43A5E0: GenericMovingPsysRun child with preset 134.
    static SpellActor* launch_generic_moving(Match& match, SpellActor& source, std::uint32_t material,
                                             FourCC texture, const Vec3& from, const Vec3& to, float speed,
                                             float rise, float radius, float size_end, float life,
                                             float emission, float seconds);
    void launch_projectile(Match& match);           // 0x441da0.
    void steer_projectile(Match& match, int ticks); // 0x441ea0.
    void capture_collision(Match& match);
    // Creature launch setup (0x412300/0x412440) and contact/fuse handling (0x41acf0).
    void start_creature_projectile(Match& match, int lifetime, FourCC model = {}, float scale = 1,
                                   bool ballistic = true, bool muzzle = true);
    int fly_creature_projectile(Match& match, int ticks);
    void start_return_projectile(Match& match, Vec3 destination, float speed);
    void set_projectile_model(Match& match, FourCC model, float scale);
    void sound_event(Match& match, unsigned mask);
    void stop_sound_event(unsigned mask);
    struct Speedup {
        float factor{};          // a1[6]: 'flum' - 1.
        int duration{};          // a1[5] ticks.
        std::vector<Vec3> trail; // a1[11..]: 'pagi' x ('muni' + 1) recent target positions.
        unsigned head{};
    } speedup;
    float teleport_radius{}; // a1[17]: the spread limit the gathering circle reaches.
    FourCC spell_id;         // Definition +16: the name each teleported unit gives this spell.
    void teleport_unit(Match& match, MatchObject& object); // 0x4429e0.
    static SpellActor& launch_fireball(Match& match, CreatureActor& actor);
    static SpellActor& launch_generic_shoot(Match& match, CreatureActor& actor);
    static SpellActor& launch_summon(Match& match, CreatureActor& actor);
    static SpellActor& launch_corpse_fade(Match& match, MatchObject& actor);
    static SpellActor& launch_doctor_orifice(Match& match, CreatureActor& actor);
    static SpellActor& launch_doctor_beam(Match& match, CreatureActor& actor);
    static SpellActor* launch_altar_process(Match& match, CreatureActor& wizard, BuildingActor& altar);
    static SpellActor& launch_altar_ruin(Match& match, BuildingActor& part);
    static SpellActor& launch_structure(Match& match, CreatureActor& actor);
    static SpellActor& launch_building_host(Match& match, BuildingActor& building, Effect effect);
    // 0x435ec0 / 0x435d80: particles streaming from the caster's hands while the spell charges.
    static SpellActor* launch_cast_hands(Match& match, SpellActor& spell);
    static SpellActor* launch_cast_hands(Match& match, SpellActor& spell, FourCC texture,
                                         std::uint32_t material, float life, float size, float rate,
                                         float gravity, float speed);
    // 0x4367c0: a glow around `target` (the spell's owner when null) until the cast resolves.
    static SpellActor* launch_glow(Match& match, SpellActor& spell, std::uint32_t material,
                                   const char* texture, float half, float speed, float scale, float life,
                                   MatchObject* target, float rate);
    static SpellActor& launch_mana_relay(Match& match, CreatureActor& hoar,
                                         std::shared_ptr<const SpellData> data);
    static SpellActor* heal(Match& match, CreatureActor& actor, int amount, int rate, int limit);
    static SpellActor* poison(Match& match, CreatureActor& actor, MatchObject* source, float seconds,
                              int amount, int damage_type, int resistance_delta, float regeneration,
                              int accuracy_delta, float spread_limit);
    static SpellActor* poison(Match& match, CreatureActor& actor, MatchObject* source, const SpellData& data);

    const MrmGeometry* model_geometry() const { return model_.get(); }

  protected:
    void activated(Match& match) override;
    void deactivated(Match& match) override;
    void sync_model(Match& match) override;
    void update(Match& match, int ticks) override;

  private:
    SpatialWorld* spatial_{};
    std::shared_ptr<const MrmGeometry> model_;
    void bind_world(Match& match);
    bool generic_hit(Match& match, int allow_damage);
};
} // namespace sacrifice
