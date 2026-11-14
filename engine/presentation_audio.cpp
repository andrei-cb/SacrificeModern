#include "sacrifice/presentation_audio.hpp"
#include "sacrifice/building_actor.hpp"
#include "sacrifice/creature_actor.hpp"
#include "sacrifice/soul_actor.hpp"
#include "sacrifice/sound.hpp"
#include "sacrifice/spell_actor.hpp"
#include <format>
#include <utility>
namespace sacrifice {
void update_spell_audio(EffectAudio& effects, Match& match, SpellActor& spell, bool enabled,
                        const AudioAttempt& attempt) {
    const auto locate = [&](std::uint32_t reference) -> std::optional<Vec3> {
        const auto* object = match.resolve(reference);
        const auto* motion = object ? object->source_motion() : nullptr;
        return motion ? std::optional<Vec3>(motion->position) : std::nullopt;
    };
    for (const auto& request : spell.sound_requests)
        if (request.stop)
            effects.stop_loops(request.emitter, request.group);
        else if (enabled)
            attempt([&] {
                effects.play({request.sample,
                              request.group,
                              request.flags | EffectSound::sample_cap,
                              0,
                              2,
                              request.emitter,
                              {}},
                             locate);
            });
    spell.sound_requests.clear();
}
void update_object_audio(EffectAudio& effects, Match& match, bool enabled, const AudioAttempt& attempt) {
    const auto locate = [&](std::uint32_t reference) -> std::optional<Vec3> {
        const auto* object = reference ? match.resolve(reference) : nullptr;
        const auto* motion = object ? object->source_motion() : nullptr;
        return motion ? std::optional<Vec3>(motion->position) : std::nullopt;
    };
    // 0x44de40: bit 2 without 0x40000 is a fixed point, the caller's or else the emitter's position.
    const auto play = [&](EffectSound sound, bool given_point = false) -> std::uint64_t {
        if (sound.flags & 2 && !(sound.flags & EffectSound::follow))
            sound.flags |= EffectSound::fixed_point;
        if (!given_point)
            if (const auto at = locate(sound.emitter))
                sound.point = *at;
        std::uint64_t key = 0;
        if (enabled)
            attempt([&] { key = effects.play(sound, locate); });
        return key;
    };
    for (const auto& object : match.objects()) {
        if (auto* spell = dynamic_cast<SpellActor*>(object.get())) {
            update_spell_audio(effects, match, *spell, enabled, attempt);
        } else if (auto* actor = dynamic_cast<CreatureActor*>(object.get())) {
            const auto id = actor->reference();
            // 0x45c6b0: creature-table sounds on channel 0 at the creature, two at a time.
            for (const auto& request : actor->combat_requests)
                if (request.kind == CreatureActor::CombatRequest::Kind::sound) {
                    effects.fade(actor->voice_handle, 0, 0, true);
                    actor->voice_handle = play({request.sample, request.group, 0x1040006, 0, 2, id, {}});
                }
            std::erase_if(actor->combat_requests, [](const auto& request) {
                return request.kind == CreatureActor::CombatRequest::Kind::sound;
            });
            for (const auto& cue : actor->cues) // 0x455460: snd! direct (0x2040006), sfx!/foot via 0x45c6b0.
                play({cue.sample,
                      cue.sample,
                      cue.type == 0x736e6421u ? 0x2040006u : 0x1040006u,
                      0,
                      2,
                      id,
                      {}});
            actor->cues.clear();
            if (std::exchange(actor->incantation_requested, false) && !effects.alive(actor->voice_handle)) {
                const auto group = FourCC::from_text("Xinc");
                const auto sample = actor->voice_sample(group);
                actor->voice_handle = play({sample, group, 0x1040006, 0, 2, id, {}});
                if (actor->voice_handle)
                    log(std::format("Casting incantation: {} [{}]", actor->definition().id.text(),
                                    sample.text()));
            }
        } else if (auto* building = dynamic_cast<BuildingActor*>(object.get())) {
            for (const auto& request : building->requests)
                if (request.kind == BuildingActor::Request::Kind::sound_start && !building->sound_handle)
                    // 0x465910: the definition's SAMP directly, looping, following the building.
                    building->sound_handle = static_cast<std::uintptr_t>(play({FourCC{request.code},
                                                                               FourCC{request.code},
                                                                               0x4040007,
                                                                               0,
                                                                               0,
                                                                               building->reference(),
                                                                               {}}));
                else if (request.kind == BuildingActor::Request::Kind::sound_stop)
                    effects.fade(request.code, 0, 1, true);
        } else if (auto* soul = dynamic_cast<SoulActor*>(object.get())) {
            for (const auto& request : soul->sounds) // 0x475b70: fixed point, two per sample.
                play({request.sample, request.group, 0x2000006, 0, 2,
                      request.owner ? request.owner->reference() : 0u, request.point},
                     true);
            soul->sounds.clear();
        }
    }
}
void update_script_audio(ScriptAudioState& state, EffectAudio& effects, const Triggers& triggers,
                         bool enabled, bool speech_allowed, bool effects_allowed,
                         const AudioAttempt& attempt) {
    const bool allowed = speech_allowed;
    const auto* current = !triggers.dialogue.empty() && triggers.dialogue.front().started
                              ? &triggers.dialogue.front()
                              : nullptr;
    const auto id = current ? current->id : 0;
    if (state.line != id || !allowed) {
        if (state.speech)
            effects.fade(state.speech, 0, 0, true);
        state.speech = 0;
    }
    if (state.line != id) {
        state.line = id;
        if (current && allowed && enabled && current->sample.value)
            attempt([&] {
                state.speech = effects.play({current->sample,
                                             current->sample,
                                             EffectSound::channel_cap | EffectSound::priority,
                                             0x74616c6b,
                                             1,
                                             0,
                                             {}});
            });
    }
    const auto service = [&](const auto& requests, auto& playing, bool dialogue) {
        for (auto it = playing.begin(); it != playing.end();) {
            if (std::ranges::find(requests, it->first, &Triggers::Dialogue::id) == requests.end()) {
                if (it->second)
                    effects.fade(it->second, 0, 0, true);
                it = playing.erase(it);
            } else
                ++it;
        }
        for (const auto& request : requests)
            if (request.started && !playing.contains(request.id)) {
                auto& key = playing[request.id];
                // Consume suppressed one-shots so they cannot restart between dialogue lines.
                if (enabled && (dialogue ? (allowed && triggers.dialogue.empty()) : effects_allowed))
                    attempt([&] {
                        key =
                            effects.play({request.sample,
                                          request.sample,
                                          EffectSound::channel_cap | (dialogue ? 0u : EffectSound::priority),
                                          dialogue ? 0x74616c6bu : 64u,
                                          1,
                                          0,
                                          {}});
                    });
            }
    };
    service(triggers.voices, state.voices, true);
    service(triggers.sounds, state.sounds, false);
}
void update_interface_audio(PlayerControl& player, Match& match, const SoundSet* interface_set,
                            const SoundSet* advisor_set, float& busy, float seconds, bool suppressed,
                            const std::function<float(FourCC)>& play) {
    if (suppressed) {
        player.interface_sounds.clear();
        player.feedback = {};
        return;
    }
    for (auto group : player.interface_sounds)
        if (interface_set)
            if (const auto sample = interface_set->select(group, match.random()); sample.value)
                play(sample);
    player.interface_sounds.clear();
    busy = std::max(0.f, busy - seconds);
    if (busy > 0)
        return;
    for (unsigned slot = 0; slot < player.feedback.size(); ++slot) {
        auto [speaker, id] = player.feedback[slot];
        if (!id.value)
            continue;
        player.feedback[slot] = {};
        FourCC sample;
        if (!slot && advisor_set)
            sample = advisor_set->select(id, match.random());
        else if (auto* actor = dynamic_cast<CreatureActor*>(speaker))
            sample = actor->voice_sample(id);
        if (sample.value) {
            busy = play(sample);
            break;
        }
    }
}
void update_weather_audio(EffectAudio& effects, Match& match, std::uint64_t& rain, bool enabled,
                          const AudioAttempt& attempt) {
    for (const auto& request : match.weather_sounds)
        if (request.action == 0 && rain)
            effects.fade(std::exchange(rain, 0), 0, 0, true);
        else if (request.action == 3 && rain)
            effects.fade(rain, request.volume, 0, false);
        else if (enabled && (request.sample.value || request.group.value) && (request.action == 1 || request.action == 2))
            attempt([&] {
                // 0x44de40 passes an id the sound set doesn't resolve on as the sample id: the rain loop's
                // 'rain' group is not in SSET:spel and plays SAMP:rain (sounds.wad ambi).
                const auto sample = request.sample.value ? request.sample : request.group;
                EffectSound sound{sample, request.group, request.action == 2 ? EffectSound::loop : 0u, 0, 2, 0, {}};
                sound.volume = request.action == 2 ? 1.f : request.volume;
                const auto key = effects.play(sound);
                if (request.action == 2)
                    rain = key;
            });
    match.weather_sounds.clear();
}
bool advance_movie(std::unique_ptr<VideoClip>& clip, const std::filesystem::path& path, double& elapsed,
                   float seconds, Image& image) {
    const bool first = !clip;
    if (first)
        clip = std::make_unique<VideoClip>(path);
    elapsed += seconds;
    if (!first && elapsed < clip->frame_seconds())
        return false;
    elapsed = 0;
    if (!clip->next(image)) {
        clip = std::make_unique<VideoClip>(path);
        return clip->next(image);
    }
    return true;
}
std::array<std::filesystem::path, 2> talking_movie_paths(const std::filesystem::path& root, int god,
                                                         unsigned emotion) {
    if (god < 0 || god >= 5)
        return {};
    constexpr const char* gods[]{"pers", "james", "charnel", "stratos", "pyro"};
    const char* mood = emotion == 1 ? "angry" : emotion == 2 ? "happy" : "normal";
    return {root / "data/anims" / std::format("{}_{}.smk", gods[god], mood),
            root / "data/anims/frame128.smk"};
}
} // namespace sacrifice
