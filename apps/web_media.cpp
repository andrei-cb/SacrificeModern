#include "web_media.hpp"
#include "media_web.hpp"
#include "sacrifice/ambient_audio.hpp"
#include "sacrifice/effect_audio.hpp"
#include "sacrifice/music.hpp"
#include "sacrifice/player.hpp"
#include "sacrifice/presentation_audio.hpp"
#include "sacrifice/scene_match.hpp"
#include "sacrifice/soul_actor.hpp"
#include "sacrifice/spell_actor.hpp"
#include "sacrifice/storage.hpp"
#include "webgpu_context.hpp"
#include <emscripten.h>
#include <format>
#include <imgui.h>
#include <set>

void ImGui_ImplWGPU_SetAdditive();
namespace sacrifice {
struct WebMedia::Impl {
    const ResourceCatalog& catalog;
    DocumentStore preferences{application_data() / "web-audio.json", "sacrifice.web-audio", 1};
    SceneMatch* bound{};
    std::filesystem::path root;
    AmbientAudio ambient;
    EffectAudio effects;
    MusicDirector music;
    std::optional<SoundSet> interface_set, advisor_set;
    std::set<std::string> errors;
    ScriptAudioState script_audio;
    std::uint64_t rain{}, choice{}, choice_voice{};
    std::uint32_t last_tick{};
    float voice_busy{}, volume{.8f};
    bool music_on{true}, sound_on{true};
    GameOptions options;
    struct Movie {
        std::filesystem::path path;
        std::unique_ptr<VideoClip> clip;
        std::shared_ptr<WebGpuContext::Texture> texture;
        unsigned width{}, height{};
        double time{};
        bool failed{};
    };
    std::array<Movie, 2> movies;
    std::uint64_t movie_line{};
    unsigned movie_frames{};
    Impl(const ResourceCatalog& c, std::filesystem::path directory)
        : catalog(c), root(std::move(directory)), ambient(c), effects(c), music(root / "data/music", true) {
        for (auto [name, destination] : {std::pair{"ifac", &interface_set}, std::pair{"advi", &advisor_set}})
            if (const auto source = catalog.find(FourCC::from_text("SSET"), FourCC::from_text(name)))
                *destination = SoundSet::decode(catalog.read(*source));
        if (const auto settings = preferences.read()) {
            volume = std::clamp(settings->value("volume", .8f), 0.f, 1.f);
            sound_on = settings->value("sound", true);
            music_on = settings->value("music", true);
        }
        apply_volume();
    }
    template <class Work> void attempt(Work work) {
        try {
            work();
        } catch (const std::exception& error) {
            if (errors.emplace(error.what()).second)
                log(std::string("Web media: ") + error.what());
        }
    }
    void apply_volume() {
        ambient.volume(sound_on ? volume * options.value(FourCC::from_text("smpv")) / 100.f : 0);
        effects.volume(sound_on ? volume * options.value(FourCC::from_text("smpv")) / 100.f : 0);
        music.volume(volume * options.value(FourCC::from_text("strv")) / 100.f);
    }
    float play_2d(FourCC sample) {
        if (!sample.value)
            return 0;
        AmbientCue cue;
        cue.sample = sample;
        cue.volume = 1;
        return ambient.submit(cue);
    }
    void scripted(Triggers& triggers, bool fast) {
        update_script_audio(script_audio, effects, triggers, !fast, !fast && triggers.choices.empty(), !fast,
                            [this](const auto& work) { attempt(work); });
        const auto next_choice = triggers.choices.empty() ? 0 : triggers.choices.front().id;
        if (choice != next_choice) {
            if (choice_voice)
                effects.fade(choice_voice, 0, 0, true);
            choice = next_choice;
            choice_voice = 0;
            if (choice && !fast)
                attempt([&] {
                    const auto sample = triggers.choices.front().sample;
                    choice_voice = effects.play({sample,
                                                 sample,
                                                 EffectSound::channel_cap | EffectSound::priority,
                                                 0x74616c6b,
                                                 1,
                                                 0,
                                                 {}});
                });
        }
    }
    void interface_audio(Match& match, PlayerControl& player, float dt, bool suppressed) {
        update_interface_audio(player, match, interface_set ? &*interface_set : nullptr,
                               advisor_set && !options.value(FourCC::from_text("nadv")) ? &*advisor_set
                                                                                        : nullptr,
                               voice_busy, dt, suppressed, [this](FourCC sample) {
                                   float result{};
                                   attempt([&] { result = play_2d(sample); });
                                   return result;
                               });
    }
    void match_audio(Match& match, float dt, bool fast) {
        update_object_audio(effects, match, !fast, [this](const auto& work) { attempt(work); });
        for (const auto& object : match.objects())
            if (auto* building = dynamic_cast<BuildingActor*>(object.get()))
                building->requests.clear();
        // Remastered Look: the world's ambient clips (insects, birds) fall quiet while it rains or hails.
        const auto& weather = match.weather.effects;
        const bool storm = options.value(FourCC::from_text("rmst")) &&
                           std::max(weather[0].intensity, weather[2].intensity) > .25f;
        if (!fast && !storm)
            for (const auto& cue : match.ambient_cues)
                if (cue.sample)
                    attempt([&] { ambient.submit(cue); });
        match.ambient_cues.clear();
        update_weather_audio(effects, match, rain, !fast, [this](const auto& work) { attempt(work); });
        attempt([&] {
            effects.advance(dt, [&](std::uint32_t ref) -> std::optional<Vec3> {
                const auto* object = match.resolve(ref);
                const auto* motion = object ? object->source_motion() : nullptr;
                return motion ? std::optional(motion->position) : std::nullopt;
            });
        });
    }
    void spell_audio(Match& match, SpellActor& spell, bool fast) {
        update_spell_audio(effects, match, spell, !fast, [this](const auto& work) { attempt(work); });
    }
    float portrait(WebGpuContext& gpu, int god, unsigned emotion, std::uint64_t id, unsigned w, unsigned h,
                   float dt) {
        if (movie_line != id) {
            movies = {};
            movie_line = id;
            const auto paths = talking_movie_paths(root, god, emotion);
            for (unsigned i = 0; i < paths.size(); ++i)
                movies[i].path = paths[i];
        }
        float bottom = h * .2f;
        auto& draw = *ImGui::GetForegroundDrawList();
        for (unsigned i = 0; i < movies.size(); ++i) {
            auto& movie = movies[i];
            if (movie.path.empty() || movie.failed)
                continue;
            try {
                Image image;
                if (advance_movie(movie.clip, movie.path, movie.time, dt, image)) {
                    if (!i)
                        for (auto& pixel : image.rgba)
                            if (!(pixel & 0xffffffu))
                                pixel = 0;
                    movie.width = image.width;
                    movie.height = image.height;
                    if (movie.texture)
                        gpu.update(*movie.texture, image);
                    else
                        movie.texture = gpu.upload(image);
                    ++movie_frames;
                }
            } catch (const std::exception& error) {
                movie.failed = true;
                if (errors.emplace(error.what()).second)
                    log(std::string("Web movie: ") + error.what());
            }
            if (movie.texture) {
                const float x = w * .5f, y = h * .2f, half_w = movie.width * .5f, half_h = movie.height * .5f;
                bottom = std::max(bottom, y + half_h);
                if (i)
                    draw.AddCallback(
                        [](const ImDrawList*, const ImDrawCmd*) { ImGui_ImplWGPU_SetAdditive(); }, nullptr);
                draw.AddImage(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(movie.texture->view)),
                              {x - half_w, y - half_h}, {x + half_w, y + half_h});
                if (i)
                    draw.AddCallback(ImDrawCallback_ResetRenderState, nullptr);
            }
        }
        return bottom;
    }
};
WebMedia::WebMedia(const ResourceCatalog& catalog, std::filesystem::path root)
    : impl_(std::make_unique<Impl>(catalog, std::move(root))) {}
WebMedia::~WebMedia() {
    bind(nullptr);
}
void WebMedia::bind(SceneMatch* scene) {
    auto& p = *impl_;
    if (p.bound) {
        p.bound->match().stop_looping_sounds = {};
        p.bound->match().flush_spell_sounds = {};
    }
    reset();
    p.bound = scene;
    if (scene) {
        scene->match().stop_looping_sounds = [&p](std::uint32_t emitter) { p.effects.stop_loops(emitter); };
        scene->match().flush_spell_sounds = [&p](SpellActor& spell) {
            p.spell_audio(p.bound->match(), spell, false);
        };
    }
}
void WebMedia::reset() {
    auto& p = *impl_;
    p.ambient.clear_cache();
    p.effects.clear_cache();
    p.music.reset();
    p.script_audio = {};
    p.movies = {};
    p.rain = p.choice = p.choice_voice = p.movie_line = 0;
    p.last_tick = 0;
    p.voice_busy = 0;
}
void WebMedia::update(SceneMatch* scene, PlayerControl* player, const Camera& camera, float dt, bool fast) {
    auto& p = *impl_;
    p.attempt([&] { p.ambient.advance(); });
    if (scene) {
        auto& match = scene->match();
        auto& triggers = scene->triggers();
        p.effects.listener({camera.position.x, camera.position.z, camera.position.y},
                           {std::cos(camera.yaw), -std::sin(camera.yaw), 0});
        p.scripted(triggers, fast);
        if (player)
            p.interface_audio(match, *player, dt, fast || triggers.cinematic() || !triggers.dialogue.empty());
        p.match_audio(match, dt, fast);
        for (const auto& request : match.music_requests)
            p.music.request(request.category, request.weight);
        match.music_requests.clear();
        const auto tick = match.clock().match;
        const int ticks = static_cast<int>(tick - p.last_tick);
        p.last_tick = tick;
        p.music.enable(p.music_on && !p.options.value(FourCC::from_text("dmus")) && tick >= 10);
        p.attempt([&] { p.music.update(ticks, dt, match.tileset().value, false); });
    } else {
        p.music.enable(p.music_on && !p.options.value(FourCC::from_text("dmus")));
        p.attempt([&] { p.music.update(static_cast<int>(dt * 60), dt, 0, true); });
    }
    const auto stats = web_audio_stats();
    EM_ASM(
        {
            const stats = Module['audioStats'] || (Module['audioStats'] = {});
            stats.frames = $0;
            stats.voices = $1;
            stats.peak = $2;
            stats.running = !!$3;
            stats.movieFrames = $4;
            stats.errors = $5;
        },
        static_cast<double>(stats.frames), stats.voices, stats.peak, stats.running, p.movie_frames,
        p.errors.size());
}
void WebMedia::options(const GameOptions& options) {
    impl_->options = options;
    impl_->apply_volume();
}
void WebMedia::controls() {
    auto& p = *impl_;
    ImGui::SeparatorText("Audio");
    bool changed = ImGui::Checkbox("Sound", &p.sound_on);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Music", &p.music_on);
    changed |= ImGui::SliderFloat("Volume", &p.volume, 0, 1, "%.2f", ImGuiSliderFlags_None);
    if (changed) {
        p.apply_volume();
        p.preferences.write({{"volume", p.volume}, {"sound", p.sound_on}, {"music", p.music_on}});
    }
    if (ImGui::Button("Resume browser audio"))
        resume_web_audio();
    const auto stats = web_audio_stats();
    ImGui::Text("%u voices | %s | %u decoded movie frames", stats.voices,
                stats.running ? "playing" : "waiting for a click", p.movie_frames);
    if (!p.errors.empty())
        ImGui::TextWrapped("%s", p.errors.begin()->c_str());
}
float WebMedia::portrait(WebGpuContext& gpu, int god, unsigned emotion, std::uint64_t line, unsigned w,
                         unsigned h, float dt) {
    return impl_->portrait(gpu, god, emotion, line, w, h, dt);
}
} // namespace sacrifice
