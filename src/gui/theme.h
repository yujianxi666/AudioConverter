// theme.h - colours, fonts and GDI+ drawing helpers for the custom-drawn UI.
#pragma once

#include <windows.h>
#include <gdiplus.h>

#include <string>
#include <vector>

namespace ac {
namespace gui {

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    bool contains(float px, float py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
    float right() const { return x + w; }
    float bottom() const { return y + h; }
};

// Colours follow the original CustomTkinter "green" appearance theme.
struct Theme {
    Gdiplus::Color background;
    Gdiplus::Color surface;
    Gdiplus::Color surfaceAlt;
    Gdiplus::Color border;
    Gdiplus::Color text;
    Gdiplus::Color textMuted;
    Gdiplus::Color accent;
    Gdiplus::Color accentHover;
    Gdiplus::Color accentText;
    Gdiplus::Color listBackground;
    Gdiplus::Color listHeaderBackground;
    Gdiplus::Color listHeaderText;
    Gdiplus::Color listRowText;
    Gdiplus::Color track;
    Gdiplus::Color fill;
    Gdiplus::Color scrollThumb;
    Gdiplus::Color tooltipBackground;
    Gdiplus::Color tooltipText;
};

const Theme& lightTheme();
const Theme& darkTheme();

// Fonts are rebuilt when the language (and therefore the family) changes.
class FontSet {
public:
    ~FontSet();
    void rebuild(bool chinese, float scale);

    Gdiplus::Font* title = nullptr;
    Gdiplus::Font* label = nullptr;
    Gdiplus::Font* entry = nullptr;
    Gdiplus::Font* button = nullptr;
    Gdiplus::Font* listHeader = nullptr;
    Gdiplus::Font* listRow = nullptr;
    Gdiplus::Font* small = nullptr;
    Gdiplus::Font* startButton = nullptr;

private:
    void clear();
    std::vector<Gdiplus::Font*> owned_;
    bool chinese_ = true;
    float scale_ = 0.0f;
};

// ------------------------------------------------------------------ helpers
void fillRoundRect(Gdiplus::Graphics& graphics, const Rect& rect, float radius,
                   const Gdiplus::Color& color);
void strokeRoundRect(Gdiplus::Graphics& graphics, const Rect& rect, float radius,
                     const Gdiplus::Color& color, float thickness);
void fillRect(Gdiplus::Graphics& graphics, const Rect& rect, const Gdiplus::Color& color);

enum class Align { Near, Center, Far };

void drawText(Gdiplus::Graphics& graphics, const std::wstring& text, Gdiplus::Font* font,
              const Rect& rect, const Gdiplus::Color& color, Align align, bool ellipsis);
void drawTextAt(Gdiplus::Graphics& graphics, const std::wstring& text, Gdiplus::Font* font,
                float x, float y, const Gdiplus::Color& color);

float measureText(Gdiplus::Graphics& graphics, const std::wstring& text, Gdiplus::Font* font);
float textHeight(Gdiplus::Graphics& graphics, Gdiplus::Font* font);

// Vertical centring helper used by every widget.
float centeredTextY(Gdiplus::Graphics& graphics, Gdiplus::Font* font, const Rect& rect);

} // namespace gui
} // namespace ac
