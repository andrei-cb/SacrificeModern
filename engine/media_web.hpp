#pragma once
#include <cstdint>
namespace sacrifice {
struct WebAudioStats {
    std::uint64_t frames{};
    unsigned voices{};
    float peak{};
    bool running{};
};
WebAudioStats web_audio_stats();
void resume_web_audio();
} // namespace sacrifice
