#pragma once
#include "sacrifice/gameplay_hud.hpp"
#include "sacrifice/menu_presentation.hpp"
#include "sacrifice/save_pages.hpp"
#include <span>
#include <cmath>
#include <imgui.h>

namespace sacrifice {
struct HudImage {
    ImTextureID image{};
    TextureHeader header;
};
inline void draw_hud_mesh(ImDrawList& draw, const Mesh& geometry, ImTextureID texture = {}) {
    if (geometry.vertices.empty())
        return;
    const auto white = ImGui::GetFontTexUvWhitePixel();
    if (texture)
        draw.PushTexture(texture);
    draw.PrimReserve(static_cast<int>(geometry.indices.size()), static_cast<int>(geometry.vertices.size()));
    const auto base = draw._VtxCurrentIdx;
    for (const auto index : geometry.indices)
        draw.PrimWriteIdx(static_cast<ImDrawIdx>(base + index));
    for (const auto& vertex : geometry.vertices)
        draw.PrimWriteVtx({vertex.position.x, vertex.position.y},
                          texture ? ImVec2{vertex.uv.x, vertex.uv.y} : white, vertex.color);
    if (texture)
        draw.PopTexture();
}
// Both applications use ImGui. Only texture handles and the additive pipeline callback differ.
class ImGuiHudPainter final : public HudPainter {
  public:
    ImGuiHudPainter(ImDrawList& draw, std::function<HudImage(FourCC, FourCC)> image, const NativeFont* font,
                    ImTextureID atlas, std::function<void(ImDrawList&)> additive, bool additive_text = false)
        : draw_(draw), image_(std::move(image)), font_(font), atlas_(atlas), additive_(std::move(additive)),
          additive_text_(additive_text) {}
    const NativeFont* font() const override {
        return font_;
    }
    const TextureHeader* header(FourCC type, FourCC id) override {
        const auto source = image_(type, id);
        if (!source.image)
            return nullptr;
        header_ = source.header;
        return &header_;
    }
    void fill(HudRect box, std::uint32_t colour) override {
        draw_.AddRectFilled(at(box.origin), end(box), colour);
    }
    void image(FourCC id, HudRect box, Vec2 uv0, Vec2 uv1) override {
        const auto source = image_(FourCC::from_text("TXTR"), id);
        if (source.image)
            draw_.AddImage(source.image, at(box.origin), end(box), at(uv0), at(uv1));
    }
    void icon(FourCC id, HudRect box, std::uint32_t colour = 0xffffffff, bool glow = false) override {
        const auto source = image_(FourCC::from_text("ICON"), id);
        if (source.image) {
            if (glow)
                additive_(draw_);
            draw_.AddImage(source.image, at(box.origin), end(box), {0, 0}, {1, 1}, colour);
            reset(glow);
        }
    }
    void sprite(FourCC id, unsigned frame, HudRect box, std::uint32_t colour = 0xffffffff, bool glow = false,
                float angle = 0) override {
        const auto source = image_(FourCC::from_text("TXTR"), id);
        const auto& h = source.header;
        if (!source.image || !h.frame_width || !h.frame_height)
            return;
        const unsigned columns = h.width / h.frame_width;
        if (!columns || frame >= std::max(h.frames, 1u) || frame / columns >= h.height / h.frame_height)
            return;
        const float x = static_cast<float>((frame % columns) * h.frame_width),
                    y = static_cast<float>((frame / columns) * h.frame_height);
        if (glow)
            additive_(draw_);
        const float c = std::cos(angle), s = std::sin(angle), a = box.size.x * .5f, b = box.size.y * .5f;
        const auto corner = [&](float dx, float dy) {
            return ImVec2{box.origin.x + a + dx * c - dy * s, box.origin.y + b + dx * s + dy * c};
        };
        const float u0 = (x + .5f) / h.width, v0 = (y + .5f) / h.height,
                    u1 = (x + h.frame_width - .5f) / h.width, v1 = (y + h.frame_height - .5f) / h.height;
        draw_.AddImageQuad(source.image, corner(-a, -b), corner(a, -b), corner(a, b), corner(-a, b), {u0, v0},
                           {u1, v0}, {u1, v1}, {u0, v1}, colour);
        reset(glow);
    }
    void text(HudRect box, std::string_view text, std::uint32_t colour = 0xffffffff, float scale = 1.5f,
              unsigned alignment = 0) override {
        if (!font_ || !atlas_)
            return;
        if (additive_text_)
            additive_(draw_);
        font_->layout(0, 0, box.size.x / scale, box.size.y / scale, alignment, text,
                      [&](const NativeFont::Glyph& glyph) {
                          draw_.AddImage(atlas_,
                                         {box.origin.x + glyph.x * scale, box.origin.y + glyph.y * scale},
                                         {box.origin.x + (glyph.x + glyph.width) * scale,
                                          box.origin.y + (glyph.y + glyph.height) * scale},
                                         {glyph.u0, glyph.v0}, {glyph.u1, glyph.v1}, colour);
                      });
        reset(additive_text_);
    }
    void mesh(const Mesh& geometry, bool glow = false, FourCC texture = {}) override {
        const auto source = texture.value ? image_(FourCC::from_text("TXTR"), texture) : HudImage{};
        if (texture.value && !source.image)
            return;
        if (glow)
            additive_(draw_);
        draw_hud_mesh(draw_, geometry, source.image);
        reset(glow);
    }

  private:
    static ImVec2 at(Vec2 p) {
        return {p.x, p.y};
    }
    static ImVec2 end(HudRect r) {
        return {r.origin.x + r.size.x, r.origin.y + r.size.y};
    }
    void reset(bool additive) {
        if (additive)
            draw_.AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    }
    ImDrawList& draw_;
    std::function<HudImage(FourCC, FourCC)> image_;
    const NativeFont* font_;
    ImTextureID atlas_;
    std::function<void(ImDrawList&)> additive_;
    bool additive_text_{};
    TextureHeader header_;
};
struct HudFontImage {
    const NativeFont* font{};
    ImTextureID image{};
};
class ImGuiMenuPainter final : public MenuPainter {
  public:
    ImGuiMenuPainter(ImDrawList& draw, float scale, std::function<ImTextureID(FourCC)> texture,
                     std::function<HudFontImage(std::string_view)> font,
                     std::function<void(ImDrawList&)> additive, bool additive_text)
        : draw_(draw), scale_(scale), texture_(std::move(texture)), font_(std::move(font)),
          additive_(std::move(additive)), additive_text_(additive_text) {}
    void quad(FourCC texture, HudRect r, std::array<Vec2, 4> coordinates, std::uint32_t tint) override {
        const auto image = texture_(texture);
        if (!image || r.size.x <= 0 || r.size.y <= 0)
            return;
        std::array<ImVec2, 4> uv;
        for (unsigned i = 0; i < uv.size(); ++i)
            uv[i] = {coordinates[i].x, coordinates[i].y};
        // Split repeats into atlas-safe tiles because ImGui's shared sampler clamps.
        const ImVec2 dx{uv[1].x - uv[0].x, uv[1].y - uv[0].y}, dy{uv[2].x - uv[0].x, uv[2].y - uv[0].y};
        const auto cuts = [&](ImVec2 delta) {
            std::vector<float> out{0, 1};
            const auto split = [&](float start, float extent) {
                if (std::abs(extent) < .0001f)
                    return;
                for (float k = std::ceil(std::min(start, start + extent));
                     k < std::max(start, start + extent); ++k) {
                    const float t = (k - start) / extent;
                    if (t > .0001f && t < .9999f)
                        out.push_back(t);
                }
            };
            split(uv[0].x, delta.x);
            split(uv[0].y, delta.y);
            std::ranges::sort(out);
            return out;
        };
        const auto xs = cuts(dx), ys = cuts(dy);
        const auto at = [&](float x, float y) {
            return ImVec2{uv[0].x + dx.x * x + dy.x * y, uv[0].y + dx.y * x + dy.y * y};
        };
        r.origin.x *= scale_;
        r.origin.y *= scale_;
        r.size.x *= scale_;
        r.size.y *= scale_;
        for (unsigned y = 1; y < ys.size(); ++y)
            for (unsigned x = 1; x < xs.size(); ++x) {
                const float x0 = xs[x - 1], x1 = xs[x], y0 = ys[y - 1], y1 = ys[y];
                const auto mid = at((x0 + x1) * .5f, (y0 + y1) * .5f);
                const auto wrap = [&](ImVec2 p) {
                    return ImVec2{p.x - std::floor(mid.x), p.y - std::floor(mid.y)};
                };
                const float left = r.origin.x + x0 * r.size.x - .5f, top = r.origin.y + y0 * r.size.y - .5f,
                            right = r.origin.x + x1 * r.size.x - .5f,
                            bottom = r.origin.y + y1 * r.size.y - .5f;
                draw_.AddImageQuad(image, {left, top}, {right, top}, {right, bottom}, {left, bottom},
                                   wrap(at(x0, y0)), wrap(at(x1, y0)), wrap(at(x1, y1)), wrap(at(x0, y1)),
                                   tint);
            }
    }
    void fill(HudRect box, std::uint32_t colour) override {
        draw_.AddRectFilled({box.origin.x * scale_, box.origin.y * scale_},
                            {(box.origin.x + box.size.x) * scale_, (box.origin.y + box.size.y) * scale_},
                            colour);
    }
    int line_height(std::string_view name) override {
        const auto font = font_(name);
        return font.font ? font.font->line_height() : 0;
    }
    void text(std::string_view name, HudRect box, std::string_view string, std::uint32_t colour,
              unsigned alignment) override {
        const auto font = font_(name);
        if (!font.font || !font.image)
            return;
        if (additive_text_)
            additive_(draw_);
        font.font->layout(box.origin.x, box.origin.y, box.size.x, box.size.y, alignment, string,
                          [&](const NativeFont::Glyph& g) {
                              draw_.AddImage(font.image, {g.x * scale_, g.y * scale_},
                                             {(g.x + g.width) * scale_, (g.y + g.height) * scale_},
                                             {g.u0, g.v0}, {g.u1, g.v1}, colour);
                          });
        if (additive_text_)
            draw_.AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    }

  private:
    ImDrawList& draw_;
    float scale_;
    std::function<ImTextureID(FourCC)> texture_;
    std::function<HudFontImage(std::string_view)> font_;
    std::function<void(ImDrawList&)> additive_;
    bool additive_text_;
};
inline bool menu_checkbox(ImGuiMenuPainter& paint, HudRect r, float scale, const char* key,
                          std::string_view label, bool enabled, bool checked, std::uint32_t flags) {
    r.size.y = 16;
    ImGui::SetCursorScreenPos({r.origin.x * scale, r.origin.y * scale});
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(key, {r.size.x * scale, r.size.y * scale});
    const bool hovered = ImGui::IsItemHovered();
    ImGui::EndDisabled();
    paint_menu_checkbox(paint, r, label, hovered, enabled, checked, flags);
    return clicked && enabled;
}
inline bool menu_options_slider(ImGuiMenuPainter& paint, const MenuForm& form, HudRect bounds, float scale,
                                FourCC setting, GameOptions& options,
                                const std::function<std::string(FourCC)>& text) {
    const auto* entry = GameOptions::find(setting);
    if (!entry)
        return false;
    bool changed = false;
    const auto content = form.content(bounds);
    for (const auto& widget : form.widgets) {
        const auto r = form.bounds(widget, content);
        if (widget.kind == 10) {
            paint.text(widget_font_name(widget.flags, "verdana"),
                       {r.origin, {r.size.x, std::max(r.size.y, 16.f)}},
                       widget.id == FourCC::from_text("name") ? text(setting) : options.display(setting),
                       0xffffffff, widget_alignment(widget.flags));
        } else if (widget.kind == 4) {
            const auto mouse = ImGui::GetIO().MousePos;
            ImGui::SetCursorScreenPos({r.origin.x * scale, r.origin.y * scale});
            ImGui::InvisibleButton("slid", {r.size.x * scale, r.size.y * scale});
            const bool held = ImGui::IsItemActive(), left = held && mouse.x < (r.origin.x + 16) * scale,
                       right = held && mouse.x >= (r.origin.x + r.size.x - 16) * scale;
            const auto previous = options.value(setting);
            auto value = previous;
            const int range = std::max(entry->maximum - entry->minimum, 1);
            if (ImGui::IsItemActivated() && (left || right))
                value += left ? -1 : 1;
            else if (held && !left && !right) {
                const float position =
                    std::clamp((mouse.x / scale - r.origin.x - 24) / (r.size.x - 48), 0.f, 1.f);
                value = entry->minimum + static_cast<int>(std::lround(position * range));
            }
            options.set(setting, value);
            changed |= previous != options.value(setting);
            paint_menu_slider(paint, r, float(options.value(setting) - entry->minimum) / range, left, right);
        }
    }
    return changed;
}
// A kind-5 list as rows of the widget's font over a dark box, in logical page coordinates; returns 1 when
// a row is clicked, 2 on a double-click. The native list host's frame and scroll bar are not recovered.
inline int menu_text_list(ImGuiMenuPainter& paint, float scale, HudRect r, const MenuForm::Widget& widget,
                          const char* key, std::span<const std::string> rows, int& chosen, int& scroll,
                          bool interactive = true) {
    paint.fill(r, IM_COL32(0, 0, 0, 120));
    const auto face = widget_font_name(widget.flags, "verdana");
    const int line = paint.line_height(face);
    if (!line)
        return 0;
    int result = 0;
    const float row_height = static_cast<float>(line + 2);
    const int visible = std::max(1, static_cast<int>((r.size.y - 4) / row_height));
    const ImVec2 low{r.origin.x * scale, r.origin.y * scale},
        high{(r.origin.x + r.size.x) * scale, (r.origin.y + r.size.y) * scale};
    if (interactive) {
        ImGui::SetCursorScreenPos(low);
        ImGui::InvisibleButton(key, {high.x - low.x, high.y - low.y});
        if (ImGui::IsItemHovered())
            scroll -= static_cast<int>(ImGui::GetIO().MouseWheel);
        if (ImGui::IsItemClicked()) {
            const auto hit =
                scroll + static_cast<int>((ImGui::GetIO().MousePos.y / scale - r.origin.y - 2) / row_height);
            if (hit >= 0 && hit < static_cast<int>(rows.size())) {
                chosen = hit;
                result = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ? 2 : 1;
            }
        }
    } else if (ImGui::IsMouseHoveringRect(low, high))
        scroll -= static_cast<int>(ImGui::GetIO().MouseWheel); // Read-only lists (cinf, desc) still scroll.
    scroll = std::clamp(scroll, 0, std::max(0, static_cast<int>(rows.size()) - visible));
    for (int i = 0; i < visible && scroll + i < static_cast<int>(rows.size()); ++i) {
        const int index = scroll + i;
        const float top = r.origin.y + 2 + i * row_height;
        if (interactive && index == chosen)
            paint.fill({{r.origin.x + 2, top}, {r.size.x - 4, row_height}}, IM_COL32(70, 70, 150, 170));
        paint.text(face, {{r.origin.x + 4, top + 2}, {r.size.x - 8, row_height}},
                   rows[static_cast<std::size_t>(index)],
                   !interactive || index == chosen ? IM_COL32_WHITE : IM_COL32(0xc3, 0xc3, 0xff, 255), 0);
    }
    return result;
}
// The save pages' own widgets (FORM sgam / lgam / sprg / lprg); false for any other widget.
// `thumbnail` is the host's upload of `pages.thumbnail`; `focus` puts the keyboard in the name box.
inline bool save_page_widget(ImGuiMenuPainter& paint, ImDrawList& draw, float scale, SavePages& pages,
                             const MenuForm& form, const MenuForm::Widget& widget, HudRect r,
                             ImTextureID thumbnail, bool focus, FourCC& action) {
    const auto command = widget.id.text();
    if (widget.kind == 5 && command == "list") { // 0x4a5830 'list': '2' selects, '4' (double-click) loads.
        int chosen = pages.row;
        if (const auto hit = menu_text_list(paint, scale, r, widget, "saves", pages.names, chosen, pages.scroll)) {
            pages.select(chosen);
            if (hit == 2 && form.id == FourCC::from_text("lgam"))
                action = FourCC::from_text("lgam");
        }
        return true;
    }
    if (widget.kind == 3 && command == "thmb") { // 'thmb': the stored 128x128 picture over 170x128.
        paint.fill(r, IM_COL32(0, 0, 0, 160));
        if (thumbnail)
            draw.AddImage(thumbnail, {r.origin.x * scale, r.origin.y * scale},
                          {(r.origin.x + r.size.x) * scale, (r.origin.y + r.size.y) * scale});
        return true;
    }
    if (widget.kind == 6 && command == "name") { // sgam's edit box; Enter is the form's accept, 'name'.
        paint.fill({r.origin, {r.size.x, 16}}, IM_COL32(0, 0, 0, 160));
        ImGui::SetCursorScreenPos({r.origin.x * scale, r.origin.y * scale});
        ImGui::SetNextItemWidth(r.size.x * scale);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
        std::array<char, WorldSave::name_limit + 1> buffer{};
        std::ranges::copy(pages.name.substr(0, WorldSave::name_limit), buffer.begin());
        if (focus)
            ImGui::SetKeyboardFocusHere();
        if (ImGui::InputText("##save_name", buffer.data(), buffer.size()))
            pages.rename(buffer.data());
        ImGui::PopStyleColor();
        return true;
    }
    if (widget.kind == 1 && command == "name") { // lgam: the selected name (+72), not a command.
        paint.text(widget_font_name(widget.flags, "verdana"), {r.origin, {r.size.x, 16}}, pages.name,
                   IM_COL32_WHITE, widget_alignment(widget.flags));
        return true;
    }
    if (widget.kind == 5 && command == "desc") {
        int none = -1;
        menu_text_list(paint, scale, r, widget, "desc", pages.description(), none, pages.description_scroll, false);
        return true;
    }
    if (widget.kind == 13) { // 'prog': the port writes or reads in one step, so the bar is full.
        paint.fill(r, IM_COL32(0, 0, 0, 160));
        paint.fill({{r.origin.x + 2, r.origin.y + 2}, {r.size.x - 4, r.size.y - 4}}, IM_COL32(150, 120, 60, 255));
        return true;
    }
    return false;
}
// FORM:igam over black: the picture, the map description and the 0x4b5b80 progress bar (TXTR:FMsb repeated
// every 32 pixels across the filled width), in logical page coordinates. The per-player nam#/prg# rows are
// network-only (0x4a96d0).
inline void paint_loading_screen(ImGuiMenuPainter& paint, ImDrawList& draw, float scale, const MenuForm& form,
                                 HudRect bounds, ImTextureID picture, std::string_view description,
                                 float progress) {
    const auto content = form.content(bounds);
    for (const auto& widget : form.widgets) {
        const auto r = form.bounds(widget, content);
        const auto id = widget.id.text();
        if (id == "img1" && picture)
            draw.AddImage(picture, {r.origin.x * scale, r.origin.y * scale},
                          {(r.origin.x + 384) * scale, (r.origin.y + 384) * scale});
        else if (id == "intr" && !description.empty())
            paint.text(widget_font_name(widget.flags, "verdana"), r, description, IM_COL32_WHITE,
                       widget_alignment(widget.flags));
        else if (id == "prog") {
            const float filled = std::clamp(progress, 0.f, 1.f) * r.size.x;
            for (float x = 0; x < filled; x += 32) {
                const float span = std::min(32.f, filled - x);
                paint.quad(FourCC::from_text("FMsb"), {{r.origin.x + x, r.origin.y}, {span, r.size.y}},
                           {Vec2{0, 0}, Vec2{span / 32, 0}, Vec2{0, 1}, Vec2{span / 32, 1}}, IM_COL32_WHITE);
            }
        }
    }
}
} // namespace sacrifice
