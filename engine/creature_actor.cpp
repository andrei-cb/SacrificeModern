#include "sacrifice/creature_actor.hpp"
#include "sacrifice/building_actor.hpp"
#include "sacrifice/side.hpp"
#include "sacrifice/sound.hpp"
#include "sacrifice/spatial.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/spell_registry.hpp"
#include "sacrifice/world_save.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace sacrifice {
CreatureActorResources CreatureActorResources::load(const ResourceCatalog& catalog,
                                                    CreatureDefinition definition, unsigned bones) {
    CreatureActorResources result;
    result.definition = std::move(definition);
    result.bones = bones;
    const auto machine = catalog.find_chunk(FourCC::from_text("FSMC"), result.definition.animation_fsm);
    if (!machine)
        throw Error("Missing creature FSMC:" + result.definition.animation_fsm.text());
    result.fsm = CreatureFsm::decode(catalog.read(*machine));
    for (const auto id : result.definition.sounds) {
        const auto set = catalog.find_chunk(FourCC::from_text("SSET"), id);
        if (set)
            result.sound_sets.push_back(std::make_shared<SoundSet>(SoundSet::decode(catalog.read(*set))));
    }
    std::map<FourCC, std::shared_ptr<const CreatureClip>> loaded;
    for (unsigned slot = 0; slot < result.clips.size(); ++slot) {
        const auto id = result.definition.animations[slot];
        if (!id.value)
            continue;
        auto [found, inserted] = loaded.try_emplace(id);
        if (inserted) {
            const auto skel = catalog.find_chunk(FourCC::from_text("SKEL"), id);
            const auto sxsk = catalog.find_chunk(FourCC::from_text("SXSK"), id);
            // FindAnim resolves the SKEL registry first; absent entries produce a null clip.
            if (skel && sxsk) {
                auto clip = std::make_shared<CreatureClip>();
                clip->id = id;
                clip->sequence = SaxsSequence::decode(catalog.read(*skel));
                clip->animation = SaxsAnimation::decode(catalog.read(*sxsk));
                clip->rate = static_cast<float>(clip->sequence.framerate / 60.0);
                unsigned attacks = 0;
                for (const auto& trigger : clip->sequence.triggers)
                    attacks += trigger.type == FourCC::from_text("atck").value;
                clip->attack_seconds =
                    static_cast<float>(clip->animation.frames.size() /
                                       (static_cast<double>(clip->rate) * 60) / std::max(attacks, 1u));
                found->second = std::move(clip);
            }
        }
        result.clips[slot] = found->second;
    }
    return result;
}
CreatureActor::CreatureActor(std::shared_ptr<const CreatureActorResources> resources) {
    configure(std::move(resources));
}
CreatureActor::~CreatureActor() {
    if (spatial_)
        spatial_->unbind(motion);
}
void CreatureActor::deactivated(Match&) {
    if (auto* building = dynamic_cast<BuildingActor*>(ritual_building))
        building->remove_guard(*this);
    path.clear(); // 0x458bd0 returns every cached waypoint to the shared navigation pool.
    if (spatial_)
        spatial_->unbind(motion);
    spatial_ = nullptr;
    motion_hooks.collide = {};
    match_ = nullptr;
}
void CreatureActor::bind_spatial(SpatialWorld& world) {
    if (spatial_)
        spatial_->unbind(motion);
    spatial_ = &world;
    world.bind(motion, MatchObject::group(type), [this] {
        return SpatialWorld::Info{kind_flags,
                                  common_status,
                                  phase,
                                  allegiance ? reinterpret_cast<std::uintptr_t>(allegiance) : spatial_side,
                                  spatial_owner,
                                  spatial_special,
                                  fsm_->state() && !(fsm_->state()->flags & 0x44000)};
    });
    motion_hooks.collide = [this](ObjectMotion& object, Vec3& delta, float* fraction,
                                  const ObjectMotion* ignore) {
        return spatial_->collide(object, delta, fraction, ignore);
    };
}
void CreatureActor::configure(std::shared_ptr<const CreatureActorResources> resources) {
    if (!resources)
        throw Error("Missing creature actor resources");
    resources_ = std::move(resources);
    fsm_ = std::make_unique<CreatureFsmRunner>(resources_->fsm);
    fsm_->animation = [this](int slot, std::uint32_t flags) { select_animation(slot, flags); };
    fsm_->handler = [this](auto&, std::string_view name, int ticks) { return native_handler(name, ticks); };
    fsm_->notified = [this](int id) { notify_event(id, this); };
    fsm_->changed = [this](const auto*, const auto& next) {
        motion.creature_flags(next.flags, resources_->definition.flags, state_flags);
    };
}
void CreatureActor::activated(Match& match) {
    path.clear(); // 0x458c20 zeroes the cache on activation.
    approach_blocker = nullptr;
    approach_result = 0;
    if (spatial_)
        spatial_->unbind(motion);
    spatial_ = nullptr;
    spatial_side = 0;
    spatial_owner = spatial_special = nullptr;
    random_ = &match.random();
    match_ = &match;
    orders.bind(this, match.terrain);
    order_requests.clear();
    view_yaw = view_pitch = 0; // 0x4681f0 activation.
    danger = 0; // 0x458dd0 actor initialization.
    courage = 1.2f;
    ground_cost = 0;
    concealment = 1;
    orders.changed = [this](bool schedule) {
        if (orders.kind()) {
            input_provider = {};
            direct_controls.reset();
        }
        order_requests.push_back({orders.kind() != 0, schedule, schedule ? -15 : 0});
    };
    kind_flags = type;
    owner = this; // 0x458dd0: creatures/wizards own their ordinary effects unless rebound.
    friendly_exception_mask = 0;
    ritual_building = nullptr;
    home_altar = doctor_altar = nullptr;
    doctor_action = {};
    doctor_spirit_ticks = doctor_beam_ready = doctor_permission = 0;
    blood_host = nullptr;
    permanent_burns = temporary_burns = 0;
    burn_ticks = 0;
    flame_host = nullptr;
    doctor_circle = {};
    credit_requests.clear();
    progression = {};
    const auto wizard_profile = initial_wizard_profile;
    initial_wizard_profile.reset();
    if (type == MatchObject::wizard && wizard_profile) {
        progression.stats[18] = wizard_profile->god;
        std::fill_n(progression.gods.begin(), 20, wizard_profile->god);
        progression.set(wizard_profile->level, 0, [&](auto& state) {
            if (match.spell_registry)
                match.spell_registry->grant(state);
        });
    }
    level_requests.clear();
    grant_requests.clear();
    credit_count_80 = credit_count_20 = 0;
    status_flags = resources_->definition.flags; // 0x458fd0: cached CRE8/WIZD properties (+2872).
    if (status_flags & 0x20)
        common_status |= 0x80;
    // 0x458dd0 -> 0x4545b0: only 'peas' names a behaviour controller; its init 0x454020 enables the
    // danger scan (+2872 0xc000).
    peasant.reset();
    if (resources_->definition.behaviour_fsm == FourCC::from_text("peas")) {
        peasant.emplace();
        status_flags |= 0xc000;
    }
    knockdown_cooldown = 0;
    burn_requests.clear();
    stats.reset(resources_->definition.stats);
    // 0x44f2f0 overrides the serialized wizard stats from 0x4ce828 before creation.
    if (type == MatchObject::wizard)
        stats.reset(progression.stats);
    mana = maximum_mana =
        type == MatchObject::wizard ? progression.maximum_mana : resources_->definition.mana;
    casting = {};
    cast_visual_requests.clear();
    incantation_requested = false;
    voice_handle = 0;
    regeneration = {};
    spells = {};
    wizard_spells = {};
    if (type == MatchObject::wizard && wizard_profile) {
        if (match.spell_registry)
            match.spell_registry->synchronize(progression, wizard_spells);
        for (auto& book : wizard_spells)
            book.level(progression.level);
    }
    attack_capabilities = 6;
    initial_run_spell.reset();
    if (match.spell_registry)
        for (const auto id : resources_->definition.spells) {
            if (!id.value)
                break; // 0x458fd0 stops at the first zero, with at most eight entries.
            const auto* definition = match.spell_registry->find(id);
            if (!definition)
                throw Error("Missing creature spell: " + id.text());
            spells.add(id, definition->priority);
            const auto* data = definition->spell.get();
            if (data && (data->property_flags & 0x800000))
                continue;
            if (definition->targets & 0x781f) {
                const unsigned flags = data ? data->flags : 0;
                if (flags & 0xA4000000)
                    attack_capabilities |= 0x10;
                if (flags & 0x58000000)
                    attack_capabilities |= 8;
            } else
                attack_capabilities |= 0x20;
        }
    // 0x458dd0 binds the serialized +124 run spell as the creature's initial behaviour.
    if (const auto id = resources_->definition.run_spell; id.value && match.spell_registry)
        if (const auto* definition = match.spell_registry->find(id);
            definition && definition->spell && definition->spell->run_id == spell_function_id("ManahoarManaRun"))
            initial_run_spell = definition->spell;
    animation_multiplier = 1;
    motion = {};
    controls = {};
    input_provider = {};
    direct_controls.reset();
    previous_turn = 0;
    terrain = nullptr;
    motion_hooks = {};
    model_bounds.reset();
    body_.reset();
    posed_clip_.reset();
    pose_.reset();
    bounds_.reset();
    camera_offset_ = {2, 1.75f};
    bounds_orientation_ = {};
    posed_frame_ = 0;
    bone_scales_ = {};
    bounds_dirty_ = pose_changed_ = false;
    model_flags = 3;
    texture_override = {};
    grayscale = false;
    death_requests.clear();
    fall_damage.clear();
    damage_multiplier = 1000;
    network_identity = -1;
    replication_flags = 0;
    friendly_damage_scale = gore_level = 0;
    force_gibs = false;
    friendly_source = {};
    damage_source = {};
    attribute_damage = {};
    last_damage_source = credited_attacker = accumulated_source_ = 0;
    accumulated_damage = damage_clock = 0;
    soul_count = resources_->definition.stats[19]; // Definition +90 -> object +2884.
    combat_requests.clear();
    ritual_abort_target = ritual_abort_owner = nullptr;
    ritual_abort_owner_enabled = false;
    fade = fade_target = fade_step = 0;
    attack_seconds = 0;
    requested_animation.reset();
    clip_.reset();
    frame_ = rate_ = 0;
    cues.clear();
    spell_requests.clear();
    ++pose_revision_;
    fsm_->start();
    if (initial_run_spell) {
        effect_target = this;
        SpellActor::bind_functions(match);
        match.assign(*this, initial_run_spell->run_id);
    }
    if (match.cinematic) // 0x458dd0 also suspends creatures spawned during a cutscene.
        issue_order({CreatureOrders::cinematic_marker, this});
}
void CreatureActor::adopt_progression(Match& match, const WizardProgression& carried) {
    if (kind_flags != MatchObject::wizard)
        return;
    progression = carried;
    progression.stats[18] = progression.gods[static_cast<std::size_t>(std::clamp(progression.level, 0, 19))];
    stats.reset(progression.stats);
    mana = maximum_mana = progression.maximum_mana;
    wizard_spells = {};
    if (match.spell_registry)
        match.spell_registry->synchronize(progression, wizard_spells);
    for (auto& book : wizard_spells)
        book.level(progression.level);
}
bool CreatureActor::select_animation(int slot, std::uint32_t flags) {
    if (slot < 0 || slot >= static_cast<int>(resources_->clips.size()))
        throw Error("Creature animation slot outside definition");
    return select_clip(resources_->clips[slot], flags);
}
void CreatureActor::bind_body(std::shared_ptr<const SaxsAsset> body) {
    if (!body || body->model.bones.size() != resources_->bones)
        throw Error("Creature body does not match definition skeleton");
    body_ = std::move(body);
    posed_clip_.reset();
    pose_.reset();
    bounds_.reset();
    camera_offset_ = {2, 1.75f};
    model_bounds.reset();
    bounds_dirty_ = pose_changed_ = false;
    bounds_orientation_ = {};
}
void CreatureActor::update_pose() {
    if (!body_ || !clip_)
        throw Error("Creature model requires a body and animation");
    const auto frame = static_cast<unsigned>(frame_);
    if (pose_ && ((model_flags & 4) || (posed_clip_ == clip_ && posed_frame_ == frame)))
        return;
    pose_ = saxs_pose(body_->model, clip_->animation, static_cast<float>(frame));
    copy_bone_scales();
    posed_clip_ = clip_;
    posed_frame_ = frame;
    pose_changed_ = true;
}
const SaxsPose& CreatureActor::model_pose() {
    update_pose();
    return *pose_;
}
void CreatureActor::prepare_model() {
    update_pose();
    if (bounds_ && (!(model_flags & 1) || !pose_changed_))
        return;
    bounds_ = saxs_bounds(body_->model, body_->descriptor, *pose_);
    if (!model_bounds) {
        model_bounds = bounds_->collision; // First UpdateBBox copies MODEL +40..155 to +200..315.
        // 0x16028b40: camera dimensions use the initial local visual box (+348..368),
        // before root translation and world rotation. Later animation bounds never change them.
        const auto& box = bounds_->visual;
        const double x = static_cast<double>(box.minimum.x) + pose_->root[0],
                     y = static_cast<double>(box.minimum.y) + pose_->root[2];
        camera_offset_ = {static_cast<float>(std::sqrt(x * x + y * y) + 1),
                          box.maximum.z - box.minimum.z + .5f};
    }
    pose_changed_ = false;
    bounds_dirty_ = true;
}
void CreatureActor::initialize_model(Match& match) {
    prepare_model();
    sync_model(match);
}
Vec3 CreatureActor::model_reference_point(unsigned index, bool animation) {
    update_pose();
    if (animation) {
        if (index >= clip_->sequence.references.size())
            throw Error("Animation reference point index outside 0..1");
        return saxs_reference_point(*pose_, clip_->sequence.references[index]);
    }
    if (index >= body_->descriptor.references.size())
        throw Error("Model reference point index outside 0..3");
    return saxs_reference_point(*pose_, body_->descriptor.references[index]);
}
double CreatureActor::pick_model(Vec3 from, Vec3 ray) {
    if (!body_)
        return -1;
    update_pose();
    const auto& o = motion.orientation;
    const auto inverse = [&](Vec3 p) {
        return Vec3{p.x * o[0] + p.y * o[1] + p.z * o[2],
                    p.x * o[3] + p.y * o[4] + p.z * o[5],
                    p.x * o[6] + p.y * o[7] + p.z * o[8]};
    };
    from = {from.x - motion.position.x, from.y - motion.position.y, from.z - motion.position.z};
    return saxs_intercept(body_->model, *pose_, inverse(from), inverse(ray));
}
bool CreatureActor::write_reference_point(Vec3& point, unsigned index, bool animation) {
    if (!body_)
        return false; // The native helper leaves its output unchanged when there is no SAXS model.
    const auto local = model_reference_point(index, animation);
    const auto& o = motion.orientation;
    point = {static_cast<float>(static_cast<double>(local.x) * o[0] + static_cast<double>(local.y) * o[3] +
                                static_cast<double>(local.z) * o[6] + motion.position.x),
             static_cast<float>(static_cast<double>(local.x) * o[1] + static_cast<double>(local.y) * o[4] +
                                static_cast<double>(local.z) * o[7] + motion.position.y),
             static_cast<float>(static_cast<double>(local.x) * o[2] + static_cast<double>(local.y) * o[5] +
                                static_cast<double>(local.z) * o[8] + motion.position.z)};
    return true;
}
void CreatureActor::sync_model(Match&) {
    if (bounds_ && (bounds_dirty_ || bounds_orientation_ != motion.orientation)) {
        const auto visual = motion_box(bounds_->visual, motion.orientation);
        motion.box_min = visual.minimum;
        motion.box_max = visual.maximum;
        motion.collision_boxes.clear();
        // 0x44e020 falls back to visual bounds center (0x44e2f0) when there are no collision records.
        motion.reference_center = {
            static_cast<float>((static_cast<double>(visual.minimum.x) + visual.maximum.x) * 0.5),
            static_cast<float>((static_cast<double>(visual.minimum.y) + visual.maximum.y) * 0.5),
            static_cast<float>((static_cast<double>(visual.minimum.z) + visual.maximum.z) * 0.5)};
        if (bounds_->collidable) {
            const auto box = motion_box(bounds_->collision, motion.orientation, true);
            motion.collision_boxes.push_back(box);
            motion.reference_center = {
                static_cast<float>((static_cast<double>(box.minimum.x) + box.maximum.x) * 0.5),
                static_cast<float>((static_cast<double>(box.minimum.y) + box.maximum.y) * 0.5),
                static_cast<float>((static_cast<double>(box.minimum.z) + box.maximum.z) * 0.5)};
        }
        bounds_orientation_ = motion.orientation;
        bounds_dirty_ = false;
    }
    if (spatial_)
        spatial_->sync(motion, state_flags);
}
bool CreatureActor::select_clip(std::shared_ptr<const CreatureClip> clip, std::uint32_t flags) {
    if (!clip || clip->animation.bones != resources_->bones)
        return false;
    if (clip->animation.frames.empty())
        throw Error("Empty creature animation clip");
    clip_ = std::move(clip);
    rate_ = clip_->rate;
    frame_ =
        flags & 0x400 ? std::clamp(frame_, 0.0f, static_cast<float>(clip_->animation.frames.size() - 1)) : 0;
    ++pose_revision_;
    return true;
}
int CreatureActor::native_handler(std::string_view name, int ticks) {
    if (name.find("Respawn") != std::string_view::npos)
        return wizard_handler(name, ticks);
    if (name.starts_with("SACDOC_") || name == "CreatureRevive4SacDoc" || name == "CreatureFloatNearSacDoc")
        return doctor_handler(name, ticks);
    if (name == "CreatureStartAbility") {
        if (!network_identity)
            return 0;
        if (!spells.entries.empty() && casting.requested) {
            start_cast();
            return 0;
        }
        return fsm_->event(2);
    }
    if (name == "CreatureAbortAbility") {
        abort_cast();
        return 0;
    }
    if (name == "CreatureTriggerAbility") {
        release_cast();
        return 0;
    }
    if (name == "CreatureChooseAttackAnim") {
        auto selected = 0x61746b32u - ((random_->next() & 1) != 0); // atk2 / atk1, even on a miss.
        const auto* target = melee_target();
        const auto* other = target ? target->source_motion() : nullptr;
        if (other) {
            const double dx = static_cast<double>(other->position.x) - motion.position.x -
                              (static_cast<double>(motion.box_max.x) + motion.box_min.x - other->box_min.x -
                               other->box_max.x) *
                                  0.5,
                         dy = static_cast<double>(other->position.y) - motion.position.y -
                              (static_cast<double>(motion.box_max.y) + motion.box_min.y - other->box_min.y -
                               other->box_max.y) *
                                  0.5,
                         dz = static_cast<double>(other->position.z) - motion.position.z -
                              (static_cast<double>(motion.box_max.z) + motion.box_min.z - other->box_min.z -
                               other->box_max.z) *
                                  0.5;
            if (-std::sqrt(dx * dx + dy * dy) > dz * 5)
                selected = 0x61746b6c; // atkl, target below.
        }
        return fsm_->event(std::bit_cast<int>(selected));
    }
    if (name == "CreatureDoAttack") {
        auto* target = melee_target();
        if (!target && (fsm_->state()->flags & 2))
            target = effect_target;
        if (target)
            melee_hit(*target, attack_seconds);
        return 0;
    }
    if (name == "CreatureAttackSound") {
        combat_sound(FourCC::from_text("Xact"));
        return 0;
    }
    if (name == "WizardStartSpell") {
        if (network_identity)
            start_cast();
        return 0;
    }
    if (name == "WizardRunSpellcasting") {
        if (kind_flags & MatchObject::wizard)
            tick_cast(ticks);
        return 0;
    }
    if (name == "WizardAbortSpell") {
        abort_cast();
        return 0;
    }
    if (name == "WizardCheckCastState")
        return casting.remaining > 0 ? 0 : fsm_->event(19);
    if (name == "WizardReleaseSpell") {
        release_cast();
        return 0;
    }
    const auto current_side_mode = side_mode.value_or(allegiance ? allegiance->outcome : 0);
    const auto event = [&](std::uint32_t id) { return fsm_->event(std::bit_cast<std::int32_t>(id)); };
    if (name == "NoCode" || name == "PeasantSetWorkAnimation")
        return 0; // Both map to 0x489b40 in the original registry.
    if (name == "CreatureSetAnimation") {
        select_clip(requested_animation);
        return 0;
    }
    if (name == "CreatureChooseRunAnim")
        return common_status & 0x20 ? event(62) : 0;
    if (name == "CreatureSelectDeathAnim")
        return event(0x64746831u + random_->next() % 3); // dth1/dth2/dth3.
    if (name == "CreaturePlayDeadStart") {
        // 0x455c70: stop the animation at its last frame and empty the order queue.
        // Both Play Dead and Earthfling Rock Form enter this shared resting state.
        freeze_animation();
        orders.clear();
        return 0;
    }
    if (name == "CreatureClearOrders") {
        orders.clear(); // 0x455cc0: Stealth's resting animation uses the ordinary queue clear.
        return 0;
    }
    if (name == "CreaturePlayDeadRun") {
        // 0x455c90: an ordinary new order wakes the creature; formation marker 33 does not.
        if (orders.kind() != 0 && orders.kind() != 33)
            event(0x61627274); // abrt
        return 0;
    }
    if (name == "CreatureMakeMeACorpse") {
        make_corpse(*match_);
        return 0;
    }
    if (name == "CreatureGetDamageDirection") {
        static constexpr std::array<std::uint32_t, 5> directions{0x64616d66, 0x64616d72, 0x64616d62,
                                                                 0x64616d6c, 0x64616d74};
        const auto* source_object = match_ ? match_->resolve(last_damage_source) : nullptr;
        const auto* source = damage_source   ? damage_source(last_damage_source)
                             : source_object ? source_object->source_motion()
                                             : nullptr;
        if (!source)
            return event(directions[random_->next() % 5]);
        const Vec3 delta{static_cast<float>(static_cast<double>(source->position.x) - motion.position.x -
                                            (static_cast<double>(motion.box_min.x) + motion.box_max.x -
                                             source->box_min.x - source->box_max.x) *
                                                0.5),
                         static_cast<float>(static_cast<double>(source->position.y) - motion.position.y -
                                            (static_cast<double>(motion.box_min.y) + motion.box_max.y -
                                             source->box_min.y - source->box_max.y) *
                                                0.5),
                         static_cast<float>(static_cast<double>(source->position.z) - motion.position.z -
                                            (static_cast<double>(motion.box_min.z) + motion.box_max.z -
                                             source->box_min.z - source->box_max.z) *
                                                0.5)};
        const auto& o = motion.orientation;
        const Vec3 local{
            static_cast<float>(static_cast<double>(o[0]) * delta.x + o[1] * static_cast<double>(delta.y) +
                               o[2] * static_cast<double>(delta.z)),
            static_cast<float>(static_cast<double>(o[3]) * delta.x + o[4] * static_cast<double>(delta.y) +
                               o[5] * static_cast<double>(delta.z)),
            static_cast<float>(static_cast<double>(o[6]) * delta.x + o[7] * static_cast<double>(delta.y) +
                               o[8] * static_cast<double>(delta.z))};
        if (std::sqrt(static_cast<double>(local.x) * local.x + static_cast<double>(local.y) * local.y) <
            static_cast<double>(local.z) * 3)
            return event(directions[4]);
        const auto sector = static_cast<std::uint8_t>(
            static_cast<std::int64_t>((std::atan2(local.x, local.y) + 3.9269875) * -0.6366203100977531));
        return event(directions[(-2 - sector) & 3]);
    }
    if (name == "CreatureCheckTumble")
        return event((static_cast<double>(motion.displacement.x) * motion.displacement.x +
                      static_cast<double>(motion.displacement.y) * motion.displacement.y) /
                                 ticks >
                             0.010000000000000002
                         ? 60
                         : 61);
    if (name == "CreatureChooseFlyAnim")
        return event(stats.current[4] * 0.000016818028927009755 <= motion.local_velocity.y ? 0x666c7921
                                                                                           : 0x686f7672);
    if (name == "CreatureChooseStance" || name == "CreatureChooseOFlyAnim") {
        const bool flying = name == "CreatureChooseOFlyAnim";
        if (flying && stats.current[4] * 0.000016818028927009755 < motion.local_velocity.y)
            return event(0x666c7921); // fly!
        if (current_side_mode == 1) {
            static constexpr std::array<std::uint32_t, 11> table{
                0x69646c32, 0x69646c32, 0x69646c32, 0x69646c32, 0x69646c32, 38, 38, 38, 37, 37, 0x69646c31};
            return event(table[random_->next() % table.size()]);
        }
        if (current_side_mode == -1) {
            static constexpr std::array<std::uint32_t, 16> table{
                40, 40, 40, 35, 35,         35,         35,         35,
                39, 39, 34, 34, 0x69646c33, 0x69646c33, 0x69646c33, 0x69646c31};
            return event(table[random_->next() & 15]);
        }
        std::uint32_t selected = flying ? 0x686f7672 : 0x7374616e; // hovr/stan.
        bool weak = false;
        if (!flying) {
            if (state_flags & 0x100000) {
                timer = std::max(timer, 240);
                return event(48);
            }
            if (state_flags & 0x40)
                return event(59);
            if (state_flags & 0x200000)
                return event(58);
            weak = stats.current[5] < (stats.base[5] >> 2) || animation_multiplier < 0.9;
            if (weak)
                selected = 0x73746675; // stfu.
        } else {
            // The original flying variant compares unsigned 16-bit health words.
            weak =
                static_cast<std::uint16_t>(stats.current[5]) < static_cast<std::uint16_t>(stats.base[5] >> 2);
        }
        if (random_->next() < 1000) {
            if (weak)
                return event(0x69646c33); // idl3.
            const auto pick = random_->next();
            selected = pick > 10000 ? 0x69646c31 : flying && pick > 5000 ? 0x69646c32 : 0x69646c34;
        }
        return event(selected);
    }
    if (extra_handler)
        return extra_handler(*this, name, ticks);
    throw Error("Unimplemented creature FSM handler: " + std::string(name));
}
void CreatureActor::notify_event(int id, MatchObject* target) {
    for (const auto& request : resources_->definition.event_spells) {
        if (!request.event || !request.spell.value)
            break;
        if (request.event != id || random_->next() > request.chance || !(request.flags & 3))
            continue;
        const int mode = request.flags & 4 ? 1 : request.flags & 8 ? 2 : 0;
        const auto* selected = request.flags & 1 ? this : target;
        if (mode && selected)
            spell_requests.push_back(
                {request.spell, mode, static_cast<std::uint16_t>(request.flags & 3), selected->reference()});
    }
}
void CreatureActor::animation_trigger(const SaxsSequence::Trigger& trigger) {
    const auto id = trigger.type;
    switch (id) {
    case 0x6162696c:
        fsm_->event(256);
        break; // abil
    case 0x6162726c:
        fsm_->event(512);
        break; // abrl
    case 0x746b6f66:
        fsm_->event(768);
        break; // tkof
    case 0x6c616e64:
        fsm_->event(1024);
        break;       // land
    case 0x6174636b: // atck
        if (clip_)
            attack_seconds = clip_->attack_seconds;
        fsm_->event(1280);
        break;
    case 0x67726162:
        fsm_->event(1536);
        break; // grab
    case 0x63617374:
        fsm_->event(2048);
        break; // cast
    case 0x736e6421: // snd!: the trigger value is the SAMP id itself (0x2040006 at this creature).
        cues.push_back({id, trigger.value, FourCC{static_cast<std::uint32_t>(trigger.value)}});
        break;
    case 0x73667821: // sfx!: 0x45c600(value, -1), a random sample from the creature's own tables.
        cues.push_back({id, trigger.value, voice_sample(FourCC{static_cast<std::uint32_t>(trigger.value)})});
        break;
    case 0x666f6f74: // foot: 0x45c600(0x58667574, -1), the creature's footstep group.
        cues.push_back({id, trigger.value, voice_sample(FourCC{1483109748u})});
        break;
    default:
        break;
    }
}
void CreatureActor::advance_animation(int ticks) {
    fsm_->run(ticks);
    if (!fsm_->state() || (fsm_->state()->flags & 0x20))
        return;
    if (!clip_)
        throw Error("Creature FSM selected no compatible animation: " + resources_->definition.id.text());
    const auto previous_clip = clip_;
    const auto previous_frame = static_cast<unsigned>(frame_);
    const float delta = static_cast<float>(static_cast<double>(rate_) * animation_multiplier * ticks);
    previous_clip->sequence.animate(frame_, delta,
                                    static_cast<unsigned>(previous_clip->animation.frames.size()),
                                    [this](const auto& trigger) { animation_trigger(trigger); });
    if (frame_ == 0) {
        if (!(fsm_->state()->flags & 0x200000))
            frame_ = static_cast<float>(clip_->animation.frames.size() - 1);
        fsm_->event(2);
    }
    if (previous_clip != clip_ || previous_frame != static_cast<unsigned>(frame_))
        ++pose_revision_;
}
bool CreatureActor::run(Match& match, int ticks) {
    if (type == MatchObject::wizard)
        for (auto& book : wizard_spells)
            book.advance(ticks);
    if (type == MatchObject::wizard)
        run_wizard(match);
    if (peasant) // 0x45a620 runs the behaviour controller's +8 first.
        peasant_evaluate(match, ticks);
    // 0x45a300 handles deferred death before cooldowns, damage, controls and animation.
    if ((state_flags & 0x40000) &&
        (((fsm_->state()->flags & 0x4000) && !(fsm_->state()->flags & 0x40000)) || fsm_->event(5) == 2))
        state_flags &= ~0x40000u;
    spells.advance(ticks);
    if (ritual_building)
        run_guard(match);
    tick_damage(match, ticks); // Vtable +240 inside the native pre-update.
    if (network_identity && (kind_flags & 5)) {
        previous_turn = controls[0];
        controls = {};
        execute_orders(match);
        auto provider = std::move(input_provider);
        input_provider = {};
        if (fsm_->state() && (fsm_->state()->flags & 1)) {
            if (provider) {
                controls = provider();
                if (match.world_journal)
                    match.world_journal->input(reference(), controls);
                const int throttle =
                    std::bit_cast<int>((static_cast<unsigned>(controls[2]) + 127u) << 7) / 254;
                controls[2] = std::clamp(throttle, 0, 128);
            } else if (direct_controls)
                controls = *direct_controls;
        }
    }
    const auto flags = fsm_->state() ? fsm_->state()->flags : 0;
    regeneration.advance(stats, ticks, mana, maximum_mana, !(flags & 0x44000),
                         (flags & 0x800) || (resources_->definition.flags & 0x2000));
    knockdown_cooldown = std::max(
        std::bit_cast<int>(static_cast<unsigned>(knockdown_cooldown) - static_cast<unsigned>(ticks)), 0);
    path.advance(ticks);
    advance_animation(ticks);
    if (MatchObject::run(match, ticks))
        return true;
    // 0x45a480: the fade request must be renewed each pass. Its limb/material host is pending.
    if (fade != fade_target) {
        const double amount = static_cast<double>(ticks) * fade_step;
        if (fade < fade_target)
            fade = static_cast<float>(
                std::min(static_cast<double>(fade) + amount, static_cast<double>(fade_target)));
        else if (fade > fade_target)
            fade = static_cast<float>(
                std::max(static_cast<double>(fade) - amount, static_cast<double>(fade_target)));
    }
    fade_target = 0;
    if (stats.changed)
        stats.advance(ticks);
    advance_bone_scales(ticks);
    animation_multiplier = 1; // 0x45a480, after the animation/common run.
    return false;
}
void CreatureActor::update(Match& match, int ticks) {
    if (!terrain)
        return;
    auto& m = motion;
    const auto old_position = m.position;
    const auto old_state = state_flags;
    const auto flags = fsm_->state() ? fsm_->state()->flags : 0;
    const auto properties = resources_->definition.flags;
    const bool controlled = (flags & 1) && !(state_flags & 0x80000);
    float yaw_target = 0, pitch_target = 0, forward = 0, vertical = 0, world_vertical = 0;
    const auto approach = [](float& value, float target, float accel, float brake) {
        motion_approach(value, target, target >= 0 ? accel : brake, target >= 0 ? brake : accel);
    };
    if (flags & 2) {
        const double maximum = stats.current[4] * 0.0001681802892700975;
        const float threshold = static_cast<float>(maximum * 0.1);
        const float ratio = static_cast<float>(m.local_velocity.y / maximum);
        if (controlled) {
            yaw_target = static_cast<float>(-static_cast<double>(controls[0]) * 0.0002727074652777777);
            if (m.local_velocity.y >= threshold)
                pitch_target = static_cast<float>(controls[1] * 0.0002045305989583333);
            else {
                const double factor = std::max(static_cast<double>(m.local_velocity.y) / threshold, 0.0);
                pitch_target = static_cast<float>(std::sin(factor) * controls[1] * 0.0002045305989583333);
                world_vertical = static_cast<float>(std::cos(factor) * controls[1] * 0.0006510416666666666);
            }
            forward = static_cast<float>(controls[2] * maximum * 0.0078125);
            if (forward < 0)
                forward = 0;
            else if (forward > maximum)
                forward = static_cast<float>(maximum);
            m.roll = static_cast<float>(
                (static_cast<double>(m.angular_velocity.x) * m.local_velocity.y * 15 / maximum +
                 static_cast<double>(m.roll) * 31) *
                0.03125);
        }
        if (terrain_surface(*terrain, m.position.x, m.position.y) >= m.position.z &&
            ((controlled && controls[1] < 0) || m.orientation[5] * m.local_velocity.y < 0))
            fsm_->event(17);
        const double limit = ratio * 0.75;
        if (m.pitch > limit) {
            m.pitch = static_cast<float>(limit);
            pitch_target = 0;
        } else if (m.pitch < -limit) {
            m.pitch = static_cast<float>(-limit);
            pitch_target = 0;
        }
        const float turn = static_cast<float>(ticks * 0.002617991666666667);
        motion_approach(m.angular_velocity.x, yaw_target, turn, turn);
        motion_approach(m.angular_velocity.y, pitch_target, turn, turn);
        approach(m.local_velocity.y, forward, static_cast<float>(ticks * 0.0075),
                 static_cast<float>(ticks * 0.01));
        const float lift = static_cast<float>(ticks * 0.05);
        motion_approach(m.velocity.z, world_vertical, lift, lift);
    } else {
        const float acceleration =
            static_cast<float>(ticks * static_cast<double>(stats.current[3]) * 0.000008409014463504877);
        float braking =
            static_cast<float>(ticks * static_cast<double>(stats.current[3]) * 0.00001681802892700975);
        if (old_state & 2) {
            if (controlled) {
                yaw_target = static_cast<float>(-static_cast<double>(controls[0]) * 0.0004122821522309711);
                forward = static_cast<float>(-static_cast<double>(controls[1]) * stats.current[3] *
                                             0.000001324254246221241);
                double slope = static_cast<double>(m.orientation[5]) * m.plane.normal.z +
                               static_cast<double>(m.orientation[4]) * m.plane.normal.y +
                               static_cast<double>(m.orientation[3]) * m.plane.normal.x;
                if (forward < 0)
                    slope = -slope;
                forward = static_cast<float>((slope * 0.5 + 1) * forward);
                if (!(properties & 0x400)) {
                    vertical = static_cast<float>(-slope * forward);
                    forward = static_cast<float>(std::sqrt(1 - slope * slope) * forward);
                }
                if (common_status & 0x20)
                    forward *= 0.25f;
            }
            if (flags & 4) {
                fsm_->event(9);
                if (m.velocity.z < -0.08175) {
                    const auto amount = static_cast<int>(static_cast<double>(m.velocity.z) * -917.43121);
                    fall_damage.push_back(amount);
                    damage(match, {0, amount, 0});
                }
            }
        } else {
            braking = static_cast<float>(braking * 0.2);
            if (flags & 4) {
                if (properties & 0x200)
                    fsm_->event(16);
            } else if (m.airborne_ticks > 15)
                fsm_->event(8);
        }
        motion_approach(m.local_velocity.x, 0, acceleration, braking);
        approach(m.local_velocity.y, forward, acceleration, braking);
        approach(m.local_velocity.z, vertical, acceleration, braking);
        const float turn = static_cast<float>(ticks * 1.570795);
        motion_approach(m.angular_velocity.x, yaw_target, turn, turn);
        if ((properties & 0xc00) && model_bounds) {
            float target_pitch = 0, target_roll = 0;
            if (state_flags & 2) {
                const auto yaw = motion_orientation(m.yaw);
                const auto& b = *model_bounds;
                const std::array<float, 4> bounds{b.minimum.x, b.maximum.x, b.minimum.y, b.maximum.y};
                std::array<float, 4> heights;
                for (unsigned i = 0; i < 4; ++i) {
                    const unsigned axis = 3 * (i >> 1);
                    const float x =
                        static_cast<float>(static_cast<double>(yaw[axis]) * bounds[i] + m.position.x);
                    const float y =
                        static_cast<float>(static_cast<double>(yaw[axis + 1]) * bounds[i] + m.position.y);
                    const double height = terrain_surface(*terrain, x, y);
                    heights[i] = height > 0 ? static_cast<float>(height) : m.position.z;
                }
                if (properties & 0x400)
                    target_pitch =
                        static_cast<float>(std::atan2(heights[3] - heights[2], bounds[3] - bounds[2]));
                if (properties & 0x800)
                    target_roll =
                        static_cast<float>(std::atan2(heights[1] - heights[0], bounds[1] - bounds[0]));
            }
            motion_approach_angle(m.pitch, target_pitch, static_cast<float>(ticks * 0.1));
            motion_approach_angle(m.roll, target_roll, static_cast<float>(ticks * 0.1));
        }
        if (fsm_->state() && (fsm_->state()->flags & 1))
            fsm_->event((m.local_velocity.y == 0 && m.local_velocity.x == 0) || !controlled ? 20 : 3);
    }
    auto hooks = motion_hooks;
    const auto on_death = hooks.death;
    hooks.death = [&](int reason) {
        death_requests.push_back(reason);
        die(match, reason);
        if (on_death)
            on_death(reason);
    };
    m.advance(*terrain, state_flags, ticks, hooks);
    if (fsm_->state() && (fsm_->state()->flags & 2) && (m.flags & 1))
        motion_damp(m.velocity, 0.02f, ticks);
    if ((state_flags & 4) && (old_state & 2)) {
        Vec3 slide;
        if (terrain_edge_slide(*terrain, old_position, m.position, slide)) {
            m.position = slide;
            const bool valid = terrain_plane(*terrain, slide.x, slide.y, m.plane) && m.plane.solid;
            // The caller stores GetPlane's boolean return over the plane distance (+556).
            m.plane.distance = valid ? 1.0f : 0.0f;
            m.airborne_ticks = 0;
            state_flags = (state_flags & ~6u) | 2;
            // +576 displacement and +560 ground height deliberately retain the pre-slide result.
        }
    }
}
} // namespace sacrifice
