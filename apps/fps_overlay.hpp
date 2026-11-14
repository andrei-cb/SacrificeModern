#pragma once
#include <imgui.h>
#include <algorithm>
#include <format>

// Port overlay (not native): the 'sfps' option's framerate readout in the top-left corner, shared by the
// desktop and browser builds. The figure is the frame count over each half second, so it stays readable.
namespace sacrifice {
struct FpsOverlay {
    float elapsed{}, shown{};
    int frames{};
    void draw(float seconds, bool enabled) {
        elapsed += seconds;
        ++frames;
        if (elapsed >= .5f) {
            shown = frames / elapsed;
            elapsed = 0;
            frames = 0;
        }
        if (!enabled || shown <= 0)
            return;
        const auto text = std::format("{:.0f} FPS", shown);
        auto& draw = *ImGui::GetForegroundDrawList();
        const float scale = ImGui::GetIO().DisplaySize.y / 480.f;
        const float size = ImGui::GetFontSize() * std::max(scale * .9f, 1.f);
        const ImVec2 at{8, 8};
        draw.AddText(nullptr, size, {at.x + 1, at.y + 1}, IM_COL32(0, 0, 0, 200), text.c_str());
        draw.AddText(nullptr, size, at, IM_COL32(255, 255, 160, 255), text.c_str());
    }
};
} // namespace sacrifice
