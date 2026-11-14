#pragma once
#include <cstdint>
#include <string_view>
#ifdef _WIN32
#include <Windows.h>
#include <cstring>
#elif defined(__EMSCRIPTEN__)
#include <emscripten.h>
#else
#include <unistd.h>
#endif

namespace sacrifice {
inline bool ascii_iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
        if (lower(a[i]) != lower(b[i]))
            return false;
    }
    return true;
}
inline std::uint32_t process_id() {
#ifdef _WIN32
    return GetCurrentProcessId();
#elif defined(__EMSCRIPTEN__)
    // Emscripten's getpid() returns a fixed value, which collides in parallel Node checks.
    return static_cast<std::uint32_t>(
        EM_ASM_INT({ return typeof process !== 'undefined' && process.versions?.node ? process.pid : 1; }));
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}
} // namespace sacrifice
