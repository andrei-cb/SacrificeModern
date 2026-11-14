#pragma once
#include "effect_audio.hpp"
#include "player.hpp"
#include "trigger.hpp"

namespace sacrifice {
class SpellActor;
struct SoundSet;
using AudioAttempt = std::function<void(const std::function<void()>&)>;
struct ScriptAudioState {
    std::uint64_t line{}, speech{};
    std::map<std::uint64_t, std::uint64_t> voices, sounds;
};
void update_script_audio(ScriptAudioState& state, EffectAudio& effects, const Triggers& triggers,
                         bool enabled, bool speech_allowed, bool effects_allowed,
                         const AudioAttempt& attempt);
void update_spell_audio(EffectAudio& effects, Match& match, SpellActor& spell, bool enabled,
                        const AudioAttempt& attempt);
void update_object_audio(EffectAudio& effects, Match& match, bool enabled, const AudioAttempt& attempt);
void update_weather_audio(EffectAudio& effects, Match& match, std::uint64_t& rain, bool enabled,
                          const AudioAttempt& attempt);
void update_interface_audio(PlayerControl& player, Match& match, const SoundSet* interface_set,
                            const SoundSet* advisor_set, float& busy, float seconds, bool suppressed,
                            const std::function<float(FourCC)>& play);
// Decode due movie frames and restart from the first frame at EOF. Upload stays in the backend.
bool advance_movie(std::unique_ptr<VideoClip>& clip, const std::filesystem::path& path, double& elapsed,
                   float seconds, Image& image);
std::array<std::filesystem::path, 2> talking_movie_paths(const std::filesystem::path& root, int god,
                                                         unsigned emotion);
} // namespace sacrifice
