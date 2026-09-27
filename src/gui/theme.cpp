// theme.cpp - colours, fonts and GDI+ drawing helpers.
#include "theme.h"

#include <algorithm>
#include <cstdint>

using namespace Gdiplus;

namespace ac {
namespace gui {
namespace {

Color rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return Color(a, r, g, b);
}

// Builds a rounded-rectangle path. GDI+ has no portable rounded-rect helper in
// every SDK, and GraphicsPath is not copyable, so the caller owns the object.
void buildRoundRect(GraphicsPath& path, const Rect& rect, float radius) {
    const float maxRadius = std::min(rect.w, rect.h) * 0.5f;
    const float r = std::max(0.0f, std::min(radius, maxRadius));
    if (r <= 0.01f) {
        path.AddRectangle(RectF(rect.x, rect.y, rect.w, rect.h));
        return;
    }
    const float d = r * 2.0f;
    path.AddArc(rect.x, rect.y, d, d, 180.0f, 90.0f);
    path.AddArc(rect.right() - d, rect.y, d, d, 270.0f, 90.0f);
    path.AddArc(rect.right() - d, rect.bottom() - d, d, d, 0.0f, 90.0f);
    path.AddArc(rect.x, rect.bottom() - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

} // namespace

const Theme& lightTheme() {
    static const Theme theme = [] {
        Theme t;
        t.background           = rgb(0xF2, 0xF2, 0xF2);
        t.surface              = rgb(0xFF, 0xFF, 0xFF);
        t.surfaceAlt           = rgb(0xF3, 0xF3, 0xF3);
        t.border               = rgb(0xD4, 0xD4, 0xD4);
        t.text                 = rgb(0x1A, 0x1A, 0x1A);
        t.textMuted            = rgb(0x7F, 0x7F, 0x7F);
        t.accent               = rgb(0x2C, 0xC9, 0x85);
        t.accentHover          = rgb(0x0C, 0x95, 0x5A);
        t.accentText           = rgb(0xFF, 0xFF, 0xFF);
        t.listBackground       = rgb(0xFF, 0xFF, 0xFF);
        t.listHeaderBackground = rgb(0xF3, 0xF3, 0xF3);
        t.listHeaderText       = rgb(0x33, 0x33, 0x33);
        t.listRowText          = rgb(0x1A, 0x1A, 0x1A);
        t.track                = rgb(0xE0, 0xE0, 0xE0);
        t.fill                 = rgb(0x2C, 0xC9, 0x85);
        t.scrollThumb          = rgb(0xC4, 0xC4, 0xC4);
        t.tooltipBackground    = rgb(0x33, 0x33, 0x33);
        t.tooltipText          = rgb(0xFF, 0xFF, 0xFF);
        return t;
    }();
    return theme;
}

const Theme& darkTheme() {
    static const Theme theme = [] {
        Theme t;
        t.background           = rgb(0x1A, 0x1A, 0x1A);
        t.surface              = rgb(0x2B, 0x2B, 0x2B);
        t.surfaceAlt           = rgb(0x3C, 0x3C, 0x3C);
        t.border               = rgb(0x3C, 0x3C, 0x3C);
        t.text                 = rgb(0xE0, 0xE0, 0xE0);
        t.textMuted            = rgb(0x8A, 0x8A, 0x8A);
        t.accent               = rgb(0x2F, 0xA5, 0x72);
        t.accentHover          = rgb(0x10, 0x6A, 0x43);
        t.accentText           = rgb(0xFF, 0xFF, 0xFF);
        t.listBackground       = rgb(0x2B, 0x2B, 0x2B);
        t.listHeaderBackground = rgb(0x3C, 0x3C, 0x3C);
        t.listHeaderText       = rgb(0xE0, 0xE0, 0xE0);
        t.listRowText          = rgb(0xE0, 0xE0, 0xE0);
        t.track                = rgb(0x2B, 0x2B, 0x2B);
        t.fill                 = rgb(0x2F, 0xA5, 0x72);
        t.scrollThumb          = rgb(0x55, 0x55, 0x55);
        t.tooltipBackground    = rgb(0x4A, 0x4A, 0x4A);
        t.tooltipText          = rgb(0xF0, 0xF0, 0xF0);
        return t;
    }();
    return theme;
}

// ------------------------------------------------------------------- FontSet
FontSet::~FontSet() { clear(); }

void FontSet::clear() {
    for (Font* font : owned_) delete font;
    owned_.clear();
    title = label = entry = button = listHeader = listRow = small = startButton = nullptr;
}

void FontSet::rebuild(bool chinese, float scale) {
    clear();
    chinese_ = chinese;
    scale_ = scale;

    // Microsoft YaHei UI covers Latin and CJK, so the Chinese UI uses it for
    // every string; Segoe UI is used for English.
    const wchar_t* family = chinese ? L"Microsoft YaHei UI" : L"Segoe UI";

    auto make = [&](float logicalSize, INT style) -> Font* {
        Font* font = new Font(family, logicalSize * scale, style, UnitPixel);
        if (font->GetLastStatus() != Ok) {
            delete font;
            font = new Font(L"Segoe UI", logicalSize * scale, style, UnitPixel);
            if (font->GetLastStatus() != Ok) {
                delete font;
                font = new Font(FontFamily::GenericSansSerif(), logicalSize * scale, style,
                                UnitPixel);
            }
        }
        owned_.push_back(font);
        return font;
    };

    title       = make(27.0f, FontStyleBold);
    label       = make(19.0f, FontStyleRegular);
    entry       = make(17.0f, FontStyleRegular);
    button      = make(17.0f, FontStyleRegular);
    listHeader  = make(15.0f, FontStyleBold);
    listRow     = make(15.0f, FontStyleRegular);
    small       = make(15.0f, FontStyleRegular);
    startButton = make(20.0f, FontStyleBold);
}

// ------------------------------------------------------------------ helpers
void fillRoundRect(Graphics& graphics, const Rect& rect, float radius, const Color& color) {
    if (rect.w <= 0.0f || rect.h <= 0.0f) return;
    GraphicsPath path;
    buildRoundRect(path, rect, radius);
    SolidBrush brush(color);
    graphics.FillPath(&brush, &path);
}

void strokeRoundRect(Graphics& graphics, const Rect& rect, float radius, const Color& color,
                     float thickness) {
    if (rect.w <= 0.0f || rect.h <= 0.0f) return;
    GraphicsPath path;
    buildRoundRect(path, rect, radius);
    Pen pen(color, thickness);
    graphics.DrawPath(&pen, &path);
}

void fillRect(Graphics& graphics, const Rect& rect, const Color& color) {
    if (rect.w <= 0.0f || rect.h <= 0.0f) return;
    SolidBrush brush(color);
    graphics.FillRectangle(&brush, RectF(rect.x, rect.y, rect.w, rect.h));
}

float measureText(Graphics& graphics, const std::wstring& text, Font* font) {
    if (text.empty() || font == nullptr) return 0.0f;
    RectF layout(0.0f, 0.0f, 100000.0f, 1000.0f);
    RectF bounds;
    graphics.MeasureString(text.c_str(), static_cast<INT>(text.size()), font, layout, &bounds);
    return bounds.Width;
}

float textHeight(Graphics& graphics, Font* font) {
    if (font == nullptr) return 0.0f;
    return font->GetHeight(&graphics);
}

float centeredTextY(Graphics& graphics, Font* font, const Rect& rect) {
    if (font == nullptr) return rect.y;
    const float height = font->GetHeight(&graphics);
    return rect.y + (rect.h - height) * 0.5f;
}

void drawText(Graphics& graphics, const std::wstring& text, Font* font, const Rect& rect,
              const Color& color, Align align, bool ellipsis) {
    if (font == nullptr || text.empty()) return;
    StringFormat format;
    format.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
    format.SetTrimming(ellipsis ? StringTrimmingEllipsisCharacter : StringTrimmingNone);
    switch (align) {
        case Align::Center: format.SetAlignment(StringAlignmentCenter); break;
        case Align::Far:    format.SetAlignment(StringAlignmentFar); break;
        default:            format.SetAlignment(StringAlignmentNear); break;
    }
    format.SetLineAlignment(StringAlignmentCenter);

    SolidBrush brush(color);
    RectF layout(rect.x, rect.y, rect.w, rect.h);
    graphics.DrawString(text.c_str(), static_cast<INT>(text.size()), font, layout, &format,
                        &brush);
}

void drawTextAt(Graphics& graphics, const std::wstring& text, Font* font, float x, float y,
                const Color& color) {
    if (font == nullptr || text.empty()) return;
    SolidBrush brush(color);
    StringFormat format;
    format.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
    graphics.DrawString(text.c_str(), static_cast<INT>(text.size()), font, PointF(x, y),
                        &format, &brush);
}

} // namespace gui
} // namespace ac
