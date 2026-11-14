#include "media_web.hpp"
#include "sacrifice/media.hpp"
#include "sacrifice/sound.hpp"
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <emscripten.h>
#include <limits>
#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include <dr_wav.h>
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include <dr_mp3.h>
#include <smacker.h>

namespace sacrifice {
namespace {
constexpr std::size_t sample_limit = 128 * 1024 * 1024;
bool signature(ByteView bytes, const char* tag) {
    return bytes.size() >= 4 && std::memcmp(bytes.data(), tag, 4) == 0;
}
AudioClip convert(AudioClip source, AudioDecodeFormat format) {
    if (!format.sample_rate || format.sample_rate > 384000 || !format.channels || format.channels > 8 ||
        !source.sample_rate || !source.channels || source.channels > 8 ||
        source.samples.size() % source.channels)
        throw Error("Invalid audio decode format");
    if (source.sample_rate == format.sample_rate && source.channels == format.channels)
        return source;
    const auto count = source.samples.size() / source.channels;
    const auto frames = static_cast<std::size_t>(
        std::ceil(static_cast<double>(count) * format.sample_rate / source.sample_rate));
    if (frames > sample_limit / format.channels)
        throw Error("Decoded audio exceeds memory limit");
    AudioClip result{format.sample_rate, format.channels, {}};
    result.samples.resize(frames * format.channels);
    const auto sample = [&](std::size_t frame, unsigned channel) {
        if (format.channels == 1) {
            float sum = 0;
            for (unsigned i = 0; i < source.channels; ++i)
                sum += source.samples[frame * source.channels + i];
            return sum / source.channels;
        }
        return source.samples[frame * source.channels + std::min(channel, source.channels - 1)];
    };
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const double position = static_cast<double>(frame) * source.sample_rate / format.sample_rate;
        const auto a = std::min(static_cast<std::size_t>(position), count - 1),
                   b = std::min(a + 1, count - 1);
        const float fraction = static_cast<float>(position - a);
        for (unsigned channel = 0; channel < format.channels; ++channel) {
            const float x = sample(a, channel), y = sample(b, channel);
            result.samples[frame * format.channels + channel] = x + (y - x) * fraction;
        }
    }
    return result;
}
struct Smacker {
    smk decoder{};
    unsigned long frames{}, width{}, height{};
    unsigned char scale{};
    double interval{};
    explicit Smacker(ByteView bytes) {
        decoder = smk_open_memory(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
        if (!decoder)
            throw Error("Cannot open Smacker media");
        smk_info_all(decoder, nullptr, &frames, &interval);
        smk_info_video(decoder, &width, &height, &scale);
        interval /= 1000000;
        if (!frames || width > 8192 || height > 8192 || !std::isfinite(interval) || interval <= 0) {
            smk_close(decoder);
            decoder = nullptr;
            throw Error("Invalid Smacker media dimensions/timing");
        }
    }
    ~Smacker() {
        if (decoder)
            smk_close(decoder);
    }
};
AudioClip smacker_audio(ByteView bytes) {
    Smacker file(bytes);
    unsigned char mask{}, channels[7]{}, bits[7]{};
    unsigned long rates[7]{};
    smk_info_audio(file.decoder, &mask, channels, bits, rates);
    unsigned track = 0;
    while (track < 7 && !(mask & (1u << track)))
        ++track;
    if (track == 7)
        throw Error("Smacker media has no audio track");
    if ((bits[track] != 8 && bits[track] != 16) || !channels[track] || channels[track] > 2 || !rates[track])
        throw Error("Unsupported Smacker audio format");
    smk_enable_all(file.decoder, static_cast<unsigned char>(1u << track));
    AudioClip result{static_cast<unsigned>(rates[track]), channels[track], {}};
    for (unsigned long frame = 0; frame < file.frames; ++frame) {
        const int status = frame ? smk_next(file.decoder) : smk_first(file.decoder);
        if (status < 0)
            throw Error("Smacker audio decode failed");
        if (status == SMK_DONE)
            break;
        const auto* data = smk_get_audio(file.decoder, track);
        const auto count = smk_get_audio_size(file.decoder, track) / (bits[track] / 8);
        if (count > sample_limit - result.samples.size())
            throw Error("Smacker audio exceeds memory limit");
        for (unsigned long i = 0; i < count; ++i)
            result.samples.push_back(
                bits[track] == 8 ? (static_cast<int>(data[i]) - 128) / 128.f
                                 : static_cast<std::int16_t>(static_cast<unsigned>(data[i * 2]) |
                                                             static_cast<unsigned>(data[i * 2 + 1]) << 8) /
                                       32768.f);
    }
    return result;
}
template <class Read> AudioClip read_pcm(unsigned rate, unsigned channels, Read read) {
    if (!rate || !channels || channels > 8)
        throw Error("Invalid decoded audio channels/rate");
    AudioClip clip{rate, channels, {}};
    std::array<float, 8192> block;
    while (const auto count = read(block.size() / channels, block.data())) {
        const auto samples = static_cast<std::size_t>(count) * channels;
        if (samples > sample_limit - clip.samples.size())
            throw Error("Decoded audio exceeds memory limit");
        clip.samples.insert(clip.samples.end(), block.begin(), block.begin() + samples);
    }
    return clip;
}
} // namespace
AudioClip decode_audio(ByteView bytes, AudioDecodeFormat format) {
    if (signature(bytes, "SMK2") || signature(bytes, "SMK4"))
        return convert(smacker_audio(bytes), format);
    if (signature(bytes, "RIFF") || signature(bytes, "RF64")) {
        drwav wav{};
        if (!drwav_init_memory(&wav, bytes.data(), bytes.size(), nullptr))
            throw Error("Cannot decode WAVE audio");
        struct Cleanup {
            drwav* wav;
            ~Cleanup() {
                drwav_uninit(wav);
            }
        } cleanup{&wav};
        return convert(read_pcm(wav.sampleRate, wav.channels,
                                [&](auto n, float* p) { return drwav_read_pcm_frames_f32(&wav, n, p); }),
                       format);
    }
    drmp3 mp3{};
    if (!drmp3_init_memory(&mp3, bytes.data(), bytes.size(), nullptr))
        throw Error("Cannot decode MP3 audio");
    struct Cleanup {
        drmp3* mp3;
        ~Cleanup() {
            drmp3_uninit(mp3);
        }
    } cleanup{&mp3};
    return convert(read_pcm(mp3.sampleRate, mp3.channels,
                            [&](auto n, float* p) { return drmp3_read_pcm_frames_f32(&mp3, n, p); }),
                   format);
}
AudioClip decode_audio(const std::filesystem::path& path) {
    return decode_audio(read_file(path));
}

// SAMP resources use the same RIFF wrapper as the Windows backend.
namespace {
void append16(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::byte>(v & 255));
    b.push_back(static_cast<std::byte>(v >> 8));
}
void append32(Bytes& b, std::uint32_t v) {
    append16(b, static_cast<std::uint16_t>(v));
    append16(b, static_cast<std::uint16_t>(v >> 16));
}
void appendtag(Bytes& b, const char* tag) {
    for (unsigned i = 0; i < 4; ++i)
        b.push_back(static_cast<std::byte>(tag[i]));
}
} // namespace
AudioClip decode_audio(const Sample& s, AudioDecodeFormat format) {
    if (s.wave_container)
        return decode_audio(s.audio, format);
    Bytes wave_format;
    append16(wave_format, s.format);
    append16(wave_format, s.channels);
    append32(wave_format, s.sample_rate);
    append32(wave_format, s.byte_rate);
    append16(wave_format, s.block_align);
    append16(wave_format, s.bits);
    if (s.format != 1 || !s.format_extra.empty()) {
        append16(wave_format, static_cast<std::uint16_t>(s.format_extra.size()));
        wave_format.insert(wave_format.end(), s.format_extra.begin(), s.format_extra.end());
    }
    Bytes wave;
    appendtag(wave, "RIFF");
    append32(wave, static_cast<std::uint32_t>(4 + 8 + wave_format.size() + (wave_format.size() & 1) + 8 +
                                              s.audio.size() + (s.audio.size() & 1)));
    appendtag(wave, "WAVE");
    appendtag(wave, "fmt ");
    append32(wave, static_cast<std::uint32_t>(wave_format.size()));
    wave.insert(wave.end(), wave_format.begin(), wave_format.end());
    if (wave_format.size() & 1)
        wave.push_back(std::byte{});
    appendtag(wave, "data");
    append32(wave, static_cast<std::uint32_t>(s.audio.size()));
    wave.insert(wave.end(), s.audio.begin(), s.audio.end());
    if (s.audio.size() & 1)
        wave.push_back(std::byte{});
    return decode_audio(wave, format);
}
struct VideoClip::Impl {
    Smacker file;
    unsigned long frame{};
    explicit Impl(ByteView bytes) : file(bytes) {
        smk_enable_all(file.decoder, SMK_VIDEO_TRACK);
    }
};
VideoClip::VideoClip(const std::filesystem::path& path) : impl_(std::make_unique<Impl>(read_file(path))) {}
VideoClip::~VideoClip() = default;
double VideoClip::frame_seconds() const {
    return impl_->file.interval;
}
bool VideoClip::next(Image& image) {
    auto& p = *impl_;
    if (p.frame >= p.file.frames)
        return false;
    const int result = p.frame++ ? smk_next(p.file.decoder) : smk_first(p.file.decoder);
    if (result < 0)
        throw Error("Smacker video decode failed");
    if (result == SMK_DONE)
        return false;
    if (!p.file.width || !p.file.height)
        throw Error("Smacker media has no video frames");
    image.width = p.file.width;
    image.height = p.file.height * (p.file.scale ? 2 : 1);
    image.rgba.resize(static_cast<std::size_t>(image.width) * image.height);
    const auto* palette = smk_get_palette(p.file.decoder);
    const auto* pixels = smk_get_video(p.file.decoder);
    for (unsigned y = 0; y < image.height; ++y)
        for (unsigned x = 0; x < image.width; ++x) {
            const auto index = pixels[(p.file.scale ? y / 2 : y) * image.width + x] * 3;
            image.rgba[static_cast<std::size_t>(y) * image.width + x] =
                palette[index] | palette[index + 1] << 8 | palette[index + 2] << 16 | 0xff000000u;
        }
    return true;
}

namespace {
struct Mixer {
    struct Voice {
        std::shared_ptr<const AudioClip> clip;
        std::uint64_t id{}, bus{};
        double position{}, played{};
        float volume{}, gain{}, pan{};
        bool positional{}, loop{}, done{};
    };
    SDL_AudioDeviceID device{};
    unsigned rate{};
    std::vector<Voice> voices;
    std::uint64_t next_id{1}, next_bus{1}, frames{};
    float peak{};
    Mixer() {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
            throw Error(SDL_GetError());
        SDL_AudioSpec desired{}, obtained{};
        desired.freq = 48000;
        desired.format = AUDIO_F32SYS;
        desired.channels = 2;
        desired.samples = 1024;
        desired.userdata = this;
        desired.callback = [](void* userdata, Uint8* data, int length) {
            static_cast<Mixer*>(userdata)->mix(reinterpret_cast<float*>(data),
                                               static_cast<unsigned>(length) / 8);
        };
        device = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
        if (!device) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            throw Error(SDL_GetError());
        }
        rate = obtained.freq;
        SDL_PauseAudioDevice(device, 0);
    }
    ~Mixer() {
        SDL_CloseAudioDevice(device);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    void mix(float* output, unsigned count) {
        std::fill(output, output + count * 2, 0.f);
        for (auto& voice : voices) {
            if (voice.done)
                continue;
            const auto& clip = *voice.clip;
            const auto end = clip.samples.size() / clip.channels;
            const double step = static_cast<double>(clip.sample_rate) / rate;
            const float left = voice.positional ? std::min(std::sqrt(1 - voice.pan), 1.f) : 1.f,
                        right = voice.positional ? std::min(std::sqrt(1 + voice.pan), 1.f) : 1.f;
            for (unsigned i = 0; i < count; ++i) {
                if (voice.position >= end) {
                    if (voice.loop)
                        voice.position = std::fmod(voice.position, static_cast<double>(end));
                    else {
                        voice.done = true;
                        break;
                    }
                }
                const auto a = static_cast<std::size_t>(voice.position), b = a + 1 < end  ? a + 1
                                                                             : voice.loop ? 0
                                                                                          : a;
                const float fraction = static_cast<float>(voice.position - a);
                for (unsigned channel = 0; channel < 2; ++channel) {
                    const auto c = std::min(channel, clip.channels - 1);
                    const float x = clip.samples[a * clip.channels + c],
                                y = clip.samples[b * clip.channels + c];
                    output[i * 2 + channel] +=
                        (x + (y - x) * fraction) * voice.gain * (channel ? right : left);
                }
                voice.position += step;
                voice.played += step;
            }
        }
        for (unsigned i = 0; i < count * 2; ++i) {
            peak = std::max(peak, std::abs(output[i]));
            output[i] = std::clamp(output[i], -1.f, 1.f);
        }
        frames += count;
    }
};
std::weak_ptr<Mixer> device;
struct AudioLock {
    SDL_AudioDeviceID id;
    explicit AudioLock(Mixer& mixer) : id(mixer.device) {
        SDL_LockAudioDevice(id);
    }
    ~AudioLock() {
        SDL_UnlockAudioDevice(id);
    }
};
void validate(const AudioClip& clip, float gain) {
    if (!clip.channels || clip.channels > 2 || !clip.sample_rate || clip.sample_rate > 384000 ||
        clip.samples.size() % clip.channels || !std::isfinite(gain) || gain < 0 || gain > 1)
        throw Error("Invalid audio voice format/buffer/gain");
}
} // namespace
struct AudioOutput::Impl {
    std::shared_ptr<Mixer> mixer;
    std::uint64_t bus{};
    float master{1};
    Impl() {
        mixer = device.lock();
        if (!mixer) {
            mixer = std::make_shared<Mixer>();
            device = mixer;
        }
        bus = mixer->next_bus++;
    }
};
AudioOutput::AudioOutput() : impl_(std::make_unique<Impl>()) {}
AudioOutput::~AudioOutput() {
    stop();
}
void AudioOutput::play(AudioClip clip) {
    stop();
    queue(std::make_shared<const AudioClip>(std::move(clip)));
}
void AudioOutput::stop() {
    AudioLock lock(*impl_->mixer);
    std::erase_if(impl_->mixer->voices, [&](const auto& voice) { return voice.bus == impl_->bus; });
}
void AudioOutput::update() {
    AudioLock lock(*impl_->mixer);
    std::erase_if(impl_->mixer->voices,
                  [&](const auto& voice) { return voice.bus == impl_->bus && voice.done; });
}
std::uint64_t AudioOutput::queue(std::shared_ptr<const AudioClip> clip, float volume) {
    if (!clip || clip->samples.empty())
        return 0;
    validate(*clip, volume);
    update();
    auto& p = *impl_;
    AudioLock lock(*p.mixer);
    if (std::ranges::count_if(p.mixer->voices,
                              [&](const auto& v) { return v.bus == p.bus && !v.positional; }) >= 16)
        return 0;
    const auto id = p.mixer->next_id++;
    p.mixer->voices.push_back({std::move(clip), id, p.bus, 0, 0, volume, sound_2d_gain(volume, p.master)});
    return id;
}
std::uint64_t AudioOutput::start(std::shared_ptr<const AudioClip> clip, float gain, float pan, bool loop) {
    if (!clip || clip->samples.empty())
        return 0;
    validate(*clip, gain);
    if (!std::isfinite(pan))
        throw Error("Invalid audio panning");
    update();
    auto& p = *impl_;
    AudioLock lock(*p.mixer);
    const auto id = p.mixer->next_id++;
    p.mixer->voices.push_back(
        {std::move(clip), id, p.bus, 0, 0, gain, gain, std::clamp(pan, -1.f, 1.f), true, loop});
    return id;
}
void AudioOutput::master_volume(float volume) {
    if (!std::isfinite(volume) || volume < 0 || volume > 1)
        throw Error("Audio master volume outside 0..1");
    auto& p = *impl_;
    AudioLock lock(*p.mixer);
    p.master = volume;
    for (auto& voice : p.mixer->voices)
        if (voice.bus == p.bus && !voice.positional)
            voice.gain = sound_2d_gain(voice.volume, volume);
}
void AudioOutput::adjust(std::uint64_t id, float gain, float pan) {
    if (!std::isfinite(gain) || !std::isfinite(pan))
        return;
    AudioLock lock(*impl_->mixer);
    for (auto& voice : impl_->mixer->voices)
        if (voice.bus == impl_->bus && voice.id == id) {
            voice.volume = voice.gain = std::clamp(gain, 0.f, 1.f);
            voice.pan = std::clamp(pan, -1.f, 1.f);
            return;
        }
}
void AudioOutput::halt(std::uint64_t id) {
    AudioLock lock(*impl_->mixer);
    std::erase_if(impl_->mixer->voices, [&](const auto& v) { return v.bus == impl_->bus && v.id == id; });
}
bool AudioOutput::active(std::uint64_t id) const {
    AudioLock lock(*impl_->mixer);
    return std::ranges::any_of(impl_->mixer->voices,
                               [&](const auto& v) { return v.bus == impl_->bus && v.id == id && !v.done; });
}
std::vector<AudioOutput::VoiceState> AudioOutput::voices() const {
    AudioLock lock(*impl_->mixer);
    std::vector<VoiceState> result;
    for (const auto& v : impl_->mixer->voices)
        if (v.bus == impl_->bus)
            result.push_back({v.id, static_cast<std::uint64_t>(v.played), v.done ? 0u : 1u, v.gain});
    return result;
}
WebAudioStats web_audio_stats() {
    const auto mixer = device.lock();
    if (!mixer)
        return {};
    AudioLock lock(*mixer);
    // clang-format off
    const bool running = EM_ASM_INT({
        const context = Module['SDL2'] && Module['SDL2'].audioContext;
        return context && context.state === 'running';
    });
    // clang-format on
    return {
        mixer->frames,
        static_cast<unsigned>(std::ranges::count_if(mixer->voices, [](const auto& v) { return !v.done; })),
        mixer->peak, running};
}
void resume_web_audio() {
    // clang-format off
    EM_ASM({
        const context = Module['SDL2'] && Module['SDL2'].audioContext;
        if (context) context.resume();
    });
    // clang-format on
}
} // namespace sacrifice
