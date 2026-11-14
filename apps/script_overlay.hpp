#pragma once
#include "hud_painter.hpp"
#include "sacrifice/trigger.hpp"
#include <format>

namespace sacrifice {
// Both renderers supply fonts and movie textures; campaign layout belongs here.
inline void paint_script_overlay(ImDrawList& draw, MenuPainter& paint, const Triggers& triggers,
                                 unsigned width, unsigned height,
                                 const std::function<HudFontImage(std::string_view)>& fonts,
                                 const std::function<float(const Triggers::Dialogue&)>& portrait,
                                 std::string_view timer_label) {
    const float w = static_cast<float>(width), h = static_cast<float>(height), bar = h * .1f;
    const auto book = fonts("bookman");
    if (!triggers.cinematic()) {
        float y = 52;
        const auto row = [&](const std::string& label) {
            if (!book.font)
                return;
            const float line_height = static_cast<float>(book.font->line_height());
            const float text_width = std::min(w - 36, static_cast<float>(book.font->width(label)));
            const HudRect box{{w - text_width - 18, y}, {text_width, line_height}};
            paint.text("bookman", {{box.origin.x + 1, y + 1}, box.size}, label, IM_COL32_BLACK, 2);
            paint.text("bookman", box, label, IM_COL32_WHITE, 2);
            y += line_height + 4;
        };
        for (const auto& value : triggers.display_values()) {
            auto label = value.name;
            const auto number = std::to_string(value.value);
            for (std::size_t at = 0; (at = label.find("<n>", at)) != std::string::npos; at += number.size())
                label.replace(at, 3, number);
            row(label);
        }
        if (triggers.timer_visible) {
            const auto seconds = std::max(triggers.game_timer, 0) / 60;
            row(std::format("{} {:02}:{:02}", timer_label, seconds / 60, seconds % 60));
        }
    } else {
        draw.AddRectFilled({0, 0}, {w, bar}, IM_COL32_BLACK);
        draw.AddRectFilled({0, h - bar}, {w, h}, IM_COL32_BLACK);
        const char* hint = "Space / Enter / click: next line     Hold Tab: fast-forward";
        const auto size = ImGui::CalcTextSize(hint);
        draw.AddText({(w - size.x) * .5f, h - size.y - 8}, IM_COL32(180, 180, 180, 255), hint);
    }
    if (book.font && !triggers.dialogue.empty()) {
        const auto& line = triggers.dialogue.front();
        if (line.started && !line.caption.empty()) {
            const float top = portrait(line);
            const float text_width = std::min(w - 32, std::max(w / 3, 320.0f));
            const float left = (w - text_width) * .5f;
            unsigned rows = 0;
            std::string_view rest = line.caption;
            while (!rest.empty()) {
                const auto wrapped = book.font->line(rest, static_cast<int>(text_width));
                if (wrapped.skip <= 0)
                    break;
                ++rows;
                rest.remove_prefix(std::min(rest.size(), static_cast<std::size_t>(wrapped.skip)));
            }
            const auto heading = fonts("elgar");
            const auto speaker = triggers.speaker_name(line);
            const float line_height = static_cast<float>(book.font->line_height());
            const float heading_height =
                speaker.empty() ? 0 : (heading.font ? heading.font->line_height() : line_height) + 8.f;
            const float box_height = rows * line_height + heading_height + 8;
            draw.AddRectFilled({left - 12, top - 6}, {left + text_width + 12, top + box_height},
                               IM_COL32(0, 0, 0, 185));
            if (!speaker.empty())
                paint.text(heading.font ? "elgar" : "bookman", {{left, top}, {text_width, heading_height}},
                           speaker, IM_COL32(0, 255, 255, 255), 1);
            const HudRect box{{left, top + heading_height}, {text_width, box_height - heading_height}};
            paint.text("bookman", {{box.origin.x + 1, box.origin.y + 1}, box.size}, line.caption,
                       IM_COL32_BLACK, 0);
            paint.text("bookman", box, line.caption, IM_COL32_WHITE, 0);
        }
    }
    if (triggers.fade.alpha > 0) {
        const auto rgb = triggers.fade.rgb;
        draw.AddRectFilled({0, 0}, {w, h},
                           IM_COL32(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255,
                                    static_cast<int>(std::clamp(triggers.fade.alpha, 0.f, 1.f) * 255)));
    }
}
} // namespace sacrifice
