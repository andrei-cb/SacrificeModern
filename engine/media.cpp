#include "sacrifice/media.hpp"
#include "sacrifice/sound.hpp"
#include <Windows.h>
#include <wrl/client.h>
#include <xaudio2.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <list>

namespace sacrifice {
namespace {
void ffcheck(int result, std::string_view operation) {
    if (result < 0) {
        char text[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(result, text, sizeof(text));
        throw Error(std::string(operation) + ": " + text);
    }
}
struct Input {
    Bytes data;
    std::int64_t position{};
    AVFormatContext* format{};
    AVIOContext* io{};
    ~Input() {
        if (format)
            avformat_close_input(&format);
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
    }
    static int read(void* opaque, std::uint8_t* target, int count) {
        auto& self = *static_cast<Input*>(opaque);
        auto remaining = static_cast<std::int64_t>(self.data.size()) - self.position;
        if (!remaining)
            return AVERROR_EOF;
        auto n = static_cast<int>(std::min<std::int64_t>(remaining, count));
        std::memcpy(target, self.data.data() + self.position, n);
        self.position += n;
        return n;
    }
    static std::int64_t seek(void* opaque, std::int64_t offset, int whence) {
        auto& self = *static_cast<Input*>(opaque);
        if (whence == AVSEEK_SIZE)
            return self.data.size();
        whence &= ~AVSEEK_FORCE;
        std::int64_t base{};
        if (whence == SEEK_CUR)
            base = self.position;
        else if (whence == SEEK_END)
            base = self.data.size();
        else if (whence != SEEK_SET)
            return AVERROR(EINVAL);
        if (offset < -base || offset > static_cast<std::int64_t>(self.data.size()) - base)
            return AVERROR(EINVAL);
        self.position = base + offset;
        return self.position;
    }
    void open(Bytes bytes) {
        data = std::move(bytes);
        auto* buffer = static_cast<unsigned char*>(av_malloc(32768));
        if (!buffer)
            throw Error("Media buffer allocation failed");
        io = avio_alloc_context(buffer, 32768, 0, this, read, nullptr, seek);
        if (!io) {
            av_free(buffer);
            throw Error("Media I/O allocation failed");
        }
        format = avformat_alloc_context();
        if (!format)
            throw Error("Media format allocation failed");
        format->pb = io;
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
        AVDictionary* options{};
        av_dict_set(&options, "format_whitelist", "smk,mp3,wav", 0);
        auto result = avformat_open_input(&format, nullptr, nullptr, &options);
        av_dict_free(&options);
        ffcheck(result, "Open local media");
        ffcheck(avformat_find_stream_info(format, nullptr), "Read media streams");
    }
};
struct Decoder {
    AVCodecContext* context{};
    AVPacket* packet{av_packet_alloc()};
    AVFrame* frame{av_frame_alloc()};
    int stream{-1};
    bool draining{};
    ~Decoder() {
        avcodec_free_context(&context);
        av_packet_free(&packet);
        av_frame_free(&frame);
    }
    void open(Input& input, AVMediaType type) {
        if (!packet || !frame)
            throw Error("Media frame allocation failed");
        const AVCodec* codec{};
        stream = av_find_best_stream(input.format, type, -1, -1, &codec, 0);
        ffcheck(stream, "Find media stream");
        context = avcodec_alloc_context3(codec);
        if (!context)
            throw Error("Codec allocation failed");
        ffcheck(avcodec_parameters_to_context(context, input.format->streams[stream]->codecpar),
                "Read codec parameters");
        ffcheck(avcodec_open2(context, codec, nullptr), "Open codec");
    }
    bool next(Input& input) {
        while (true) {
            auto result = avcodec_receive_frame(context, frame);
            if (result == 0)
                return true;
            if (result == AVERROR_EOF)
                return false;
            if (result != AVERROR(EAGAIN))
                ffcheck(result, "Decode frame");
            if (draining)
                return false;
            while (true) {
                result = av_read_frame(input.format, packet);
                if (result < 0 || packet->stream_index == stream)
                    break;
                av_packet_unref(packet);
            }
            if (result < 0) {
                if (result != AVERROR_EOF)
                    ffcheck(result, "Read media packet");
                draining = true;
                ffcheck(avcodec_send_packet(context, nullptr), "Drain media");
            } else {
                auto status = avcodec_send_packet(context, packet);
                av_packet_unref(packet);
                ffcheck(status, "Send media packet");
            }
        }
    }
};
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
AudioClip decode_audio(ByteView bytes, AudioDecodeFormat output_format) {
    if (!output_format.channels || output_format.channels > 8 || !output_format.sample_rate ||
        output_format.sample_rate > 384000)
        throw Error("Invalid decoded audio format");
    Input input;
    input.open(Bytes(bytes.begin(), bytes.end()));
    Decoder decoder;
    decoder.open(input, AVMEDIA_TYPE_AUDIO);
    AudioClip clip;
    clip.sample_rate = output_format.sample_rate;
    clip.channels = output_format.channels;
    AVChannelLayout layout{};
    av_channel_layout_default(&layout, output_format.channels);
    SwrContext* raw{};
    const auto allocated = swr_alloc_set_opts2(&raw, &layout, AV_SAMPLE_FMT_FLT, output_format.sample_rate,
                                               &decoder.context->ch_layout, decoder.context->sample_fmt,
                                               decoder.context->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&layout);
    if (allocated < 0)
        swr_free(&raw);
    ffcheck(allocated, "Create audio resampler");
    struct Resampler {
        SwrContext* value;
        ~Resampler() {
            swr_free(&value);
        }
    } resampler{raw};
    ffcheck(swr_init(raw), "Initialize audio resampler");
    auto convert = [&](const std::uint8_t** data, int samples) {
        auto count = swr_get_out_samples(raw, samples);
        ffcheck(count, "Audio output size");
        std::vector<float> output(static_cast<std::size_t>(count) * clip.channels);
        auto* target = reinterpret_cast<std::uint8_t*>(output.data());
        auto produced = swr_convert(raw, &target, count, data, samples);
        ffcheck(produced, "Resample audio");
        output.resize(static_cast<std::size_t>(produced) * clip.channels);
        if (clip.samples.size() + output.size() > 128ull * 1024 * 1024)
            throw Error("Decoded audio exceeds memory limit");
        clip.samples.insert(clip.samples.end(), output.begin(), output.end());
        return produced;
    };
    while (decoder.next(input))
        convert(const_cast<const std::uint8_t**>(decoder.frame->extended_data), decoder.frame->nb_samples);
    while (convert(nullptr, 0) > 0) {
    }
    return clip;
}
AudioClip decode_audio(const std::filesystem::path& path) {
    auto bytes = read_file(path);
    return decode_audio(bytes);
}
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
    Input input;
    Decoder decoder;
    SwsContext* scaler{};
    double interval{1.0 / 15};
    ~Impl() {
        sws_freeContext(scaler);
    }
};
VideoClip::VideoClip(const std::filesystem::path& path) : impl_(std::make_unique<Impl>()) {
    auto& p = *impl_;
    p.input.open(read_file(path));
    p.decoder.open(p.input, AVMEDIA_TYPE_VIDEO);
    auto rate = av_guess_frame_rate(p.input.format, p.input.format->streams[p.decoder.stream], nullptr);
    if (rate.num > 0 && rate.den > 0)
        p.interval = av_q2d(av_inv_q(rate));
}
VideoClip::~VideoClip() = default;
double VideoClip::frame_seconds() const {
    return impl_->interval;
}
bool VideoClip::next(Image& image) {
    auto& p = *impl_;
    if (!p.decoder.next(p.input))
        return false;
    auto* frame = p.decoder.frame;
    if (frame->width <= 0 || frame->height <= 0 || frame->width > 8192 || frame->height > 8192)
        throw Error("Invalid video frame dimensions");
    image.width = frame->width;
    image.height = frame->height;
    image.rgba.resize(static_cast<std::size_t>(image.width) * image.height);
    p.scaler = sws_getCachedContext(p.scaler, frame->width, frame->height,
                                    static_cast<AVPixelFormat>(frame->format), frame->width, frame->height,
                                    AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!p.scaler)
        throw Error("Video pixel conversion failed");
    std::uint8_t* targets[4]{reinterpret_cast<std::uint8_t*>(image.rgba.data()), nullptr, nullptr, nullptr};
    int strides[4]{frame->width * 4, 0, 0, 0};
    sws_scale(p.scaler, frame->data, frame->linesize, 0, frame->height, targets, strides);
    return true;
}
struct AudioOutput::Impl {
    Microsoft::WRL::ComPtr<IXAudio2> engine;
    IXAudio2MasteringVoice* master{};
    struct Voice {
        IXAudio2SourceVoice* source{};
        std::shared_ptr<const AudioClip> clip;
        std::uint64_t id{};
        float volume{}, gain{};
        bool positional{};
        ~Voice() {
            if (source)
                source->DestroyVoice();
        }
    };
    std::list<std::unique_ptr<Voice>> active;
    std::uint64_t next_id{1};
    float master_volume{1};
    ~Impl() {
        active.clear(); // Release source voices while their buffers, master and engine remain alive.
        if (master)
            master->DestroyVoice();
    }
};
namespace {
void audio_check(HRESULT result, std::string_view operation) {
    if (FAILED(result))
        throw Error(std::string(operation) + " (HRESULT " + std::to_string(result) + ")");
}
} // namespace
AudioOutput::AudioOutput() : impl_(std::make_unique<Impl>()) {
    auto& p = *impl_;
    audio_check(XAudio2Create(&p.engine, 0, XAUDIO2_DEFAULT_PROCESSOR), "Initialize XAudio2");
    audio_check(p.engine->CreateMasteringVoice(&p.master), "Create audio mastering voice");
}
AudioOutput::~AudioOutput() = default;
void AudioOutput::stop() {
    impl_->active.clear();
}
void AudioOutput::play(AudioClip clip) {
    stop(); // Inspector preview remains a single replaceable sound; match voices use queue().
    queue(std::make_shared<const AudioClip>(std::move(clip)));
}
void AudioOutput::update() {
    auto& active = impl_->active;
    for (auto it = active.begin(); it != active.end();) {
        XAUDIO2_VOICE_STATE state{};
        (*it)->source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (!state.BuffersQueued)
            it = active.erase(it);
        else
            ++it;
    }
}
std::uint64_t AudioOutput::queue(std::shared_ptr<const AudioClip> clip, float volume) {
    if (!clip || clip->samples.empty())
        return 0;
    if (!clip->channels || clip->channels > 2 || clip->samples.size() % clip->channels ||
        clip->sample_rate < XAUDIO2_MIN_SAMPLE_RATE || clip->sample_rate > XAUDIO2_MAX_SAMPLE_RATE ||
        clip->samples.size() > XAUDIO2_MAX_BUFFER_BYTES / sizeof(float) || !std::isfinite(volume) ||
        volume < 0 || volume > 1)
        throw Error("Invalid audio voice format/buffer/volume");
    update();
    auto& p = *impl_;
    if (std::ranges::count_if(p.active, [](const auto& voice) { return !voice->positional; }) >= 16)
        return 0; // Ambient flags protect existing ambient voices from native replacement.
    auto voice = std::make_unique<Impl::Voice>();
    voice->clip = std::move(clip);
    voice->id = p.next_id++;
    voice->volume = volume;
    voice->gain = sound_2d_gain(volume, p.master_volume);
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = static_cast<WORD>(voice->clip->channels);
    format.nSamplesPerSec = voice->clip->sample_rate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = format.nChannels * 4;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    audio_check(p.engine->CreateSourceVoice(&voice->source, &format), "Create audio source voice");
    audio_check(voice->source->SetVolume(voice->gain), "Set audio source gain");
    XAUDIO2_BUFFER buffer{};
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    buffer.AudioBytes = static_cast<UINT32>(voice->clip->samples.size() * sizeof(float));
    buffer.pAudioData = reinterpret_cast<const BYTE*>(voice->clip->samples.data());
    audio_check(voice->source->SubmitSourceBuffer(&buffer), "Submit audio samples");
    audio_check(voice->source->Start(), "Start audio source voice");
    const auto id = voice->id;
    p.active.push_back(std::move(voice));
    return id;
}
void AudioOutput::master_volume(float volume) {
    if (!std::isfinite(volume) || volume < 0 || volume > 1)
        throw Error("Audio master volume outside 0..1");
    auto& p = *impl_;
    p.master_volume = volume;
    for (auto& voice : p.active) {
        if (voice->positional)
            continue;
        voice->gain = sound_2d_gain(voice->volume, volume);
        audio_check(voice->source->SetVolume(voice->gain), "Update audio source gain");
    }
}
std::vector<AudioOutput::VoiceState> AudioOutput::voices() const {
    std::vector<VoiceState> result;
    for (const auto& voice : impl_->active) {
        XAUDIO2_VOICE_STATE state{};
        voice->source->GetState(&state);
        float gain{};
        voice->source->GetVolume(&gain);
        result.push_back({voice->id, state.SamplesPlayed, state.BuffersQueued, gain});
    }
    return result;
}
namespace {
void apply_pan(IXAudio2MasteringVoice* master, IXAudio2SourceVoice* source, unsigned channels, float pan) {
    XAUDIO2_VOICE_DETAILS details{};
    master->GetVoiceDetails(&details);
    if (details.InputChannels < 2)
        return;
    const float clamped = std::clamp(pan, -1.0f, 1.0f);
    const float left = std::sqrt(.5f * (1 - clamped)) * 1.41421356f,
                right = std::sqrt(.5f * (1 + clamped)) * 1.41421356f;
    std::vector<float> matrix(static_cast<std::size_t>(channels) * details.InputChannels);
    if (channels == 1) {
        matrix[0] = std::min(left, 1.0f);
        matrix[1] = std::min(right, 1.0f);
    } else {
        matrix[0] = std::min(left, 1.0f);
        matrix[static_cast<std::size_t>(details.InputChannels) + 1] = std::min(right, 1.0f);
    }
    audio_check(source->SetOutputMatrix(master, channels, details.InputChannels, matrix.data()),
                "Set audio source panning");
}
} // namespace
std::uint64_t AudioOutput::start(std::shared_ptr<const AudioClip> clip, float gain, float pan, bool loop) {
    if (!clip || clip->samples.empty())
        return 0;
    if (!clip->channels || clip->channels > 2 || clip->samples.size() % clip->channels ||
        clip->sample_rate < XAUDIO2_MIN_SAMPLE_RATE || clip->sample_rate > XAUDIO2_MAX_SAMPLE_RATE ||
        clip->samples.size() > XAUDIO2_MAX_BUFFER_BYTES / sizeof(float) || !std::isfinite(gain) || gain < 0 ||
        gain > 1)
        throw Error("Invalid positional audio voice format/buffer/gain");
    update();
    auto& p = *impl_;
    auto voice = std::make_unique<Impl::Voice>();
    voice->clip = std::move(clip);
    voice->id = p.next_id++;
    voice->volume = voice->gain = gain;
    voice->positional = true;
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = static_cast<WORD>(voice->clip->channels);
    format.nSamplesPerSec = voice->clip->sample_rate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = format.nChannels * 4;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    audio_check(p.engine->CreateSourceVoice(&voice->source, &format), "Create positional audio voice");
    audio_check(voice->source->SetVolume(gain), "Set positional audio gain");
    apply_pan(p.master, voice->source, voice->clip->channels, pan);
    XAUDIO2_BUFFER buffer{};
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    buffer.AudioBytes = static_cast<UINT32>(voice->clip->samples.size() * sizeof(float));
    buffer.pAudioData = reinterpret_cast<const BYTE*>(voice->clip->samples.data());
    buffer.LoopCount = loop ? XAUDIO2_LOOP_INFINITE : 0;
    audio_check(voice->source->SubmitSourceBuffer(&buffer), "Submit positional audio samples");
    audio_check(voice->source->Start(), "Start positional audio voice");
    const auto id = voice->id;
    p.active.push_back(std::move(voice));
    return id;
}
void AudioOutput::adjust(std::uint64_t id, float gain, float pan) {
    auto& p = *impl_;
    for (auto& voice : p.active)
        if (voice->id == id) {
            voice->volume = voice->gain = std::clamp(gain, 0.0f, 1.0f);
            audio_check(voice->source->SetVolume(voice->gain), "Update positional audio gain");
            apply_pan(p.master, voice->source, voice->clip->channels, pan);
            return;
        }
}
void AudioOutput::halt(std::uint64_t id) {
    std::erase_if(impl_->active, [&](const auto& voice) { return voice->id == id; });
}
bool AudioOutput::active(std::uint64_t id) const {
    for (const auto& voice : impl_->active)
        if (voice->id == id) {
            XAUDIO2_VOICE_STATE state{};
            voice->source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            return state.BuffersQueued != 0;
        }
    return false;
}
} // namespace sacrifice
