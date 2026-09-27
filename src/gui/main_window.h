// main_window.h - the custom-drawn application window.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../core/converter.h"
#include "../core/i18n.h"
#include "theme.h"

namespace ac {
namespace gui {

// Every clickable region of the window.
enum class Id {
    None,
    ThemeButton,
    LanguageButton,
    SourceEntry,
    SourceBrowse,
    OutputEntry,
    OutputBrowse,
    FormatCombo,
    ComboItem0,
    ComboItem1,
    StartButton,
    ListScrollThumb,
    ListScrollTrack
};

// A single-line editable text field with a caret and a selection.
struct TextEntry {
    std::wstring text;
    int caret = 0;
    int anchor = 0;
    float scroll = 0.0f;

    bool hasSelection() const { return caret != anchor; }
    int selectionBegin() const { return caret < anchor ? caret : anchor; }
    int selectionEnd() const { return caret < anchor ? anchor : caret; }
    void clearSelection() { anchor = caret; }
};

struct Layout {
    Rect title;
    Rect languageButton;
    Rect themeButton;
    Rect sourceLabel;
    Rect sourceEntry;
    Rect sourceBrowse;
    Rect listFrame;
    Rect listArea;
    Rect listHeader;
    Rect listViewport;
    Rect listScrollbar;
    Rect countLabel;
    Rect outputLabel;
    Rect outputEntry;
    Rect outputBrowse;
    Rect formatLabel;
    Rect formatCombo;
    Rect comboDropdown;
    Rect progressTrack;
    Rect progressLabel;
    Rect startButton;
    float rowHeight = 28.0f;
    float headerHeight = 28.0f;
};

class MainWindow {
public:
    MainWindow() = default;
    ~MainWindow();

    // Registers the window class and creates the window. Returns false and fills
    // `error` (when provided) on failure.
    bool create(HINSTANCE instance, int showCommand, std::wstring* error = nullptr);

    // Pre-fills the folder fields from the command line (used by shortcuts and
    // "Send to" entries). Scans the source folder when it is valid.
    void applyStartupOptions(const std::wstring& sourceFolder,
                             const std::wstring& outputFolder,
                             int formatIndex);
    int runMessageLoop();

private:
    // ---- window plumbing ----
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    // ---- layout and painting ----
    void updateScale();
    void computeLayout();
    void paint(HDC target);
    void invalidate();
    void drawTitle(Gdiplus::Graphics& graphics, const Theme& theme);
    void drawFolderRow(Gdiplus::Graphics& graphics, const Theme& theme, Id labelId,
                       const Rect& labelRect, const std::wstring& labelText,
                       TextEntry& entry, const Rect& entryRect, const Rect& browseRect,
                       bool focused);
    void drawList(Gdiplus::Graphics& graphics, const Theme& theme);
    void drawFormatRow(Gdiplus::Graphics& graphics, const Theme& theme);
    void drawProgress(Gdiplus::Graphics& graphics, const Theme& theme);
    void drawStartButton(Gdiplus::Graphics& graphics, const Theme& theme);
    void drawTooltip(Gdiplus::Graphics& graphics, const Theme& theme);
    void drawEntryContents(Gdiplus::Graphics& graphics, const Theme& theme,
                           const Rect& rect, const TextEntry& entry, bool focused,
                           const std::wstring& placeholder, bool hasText);

    // ---- interaction ----
    Id hitTest(float x, float y) const;
    void onMouseMove(float x, float y);
    void onLeftButtonDown(float x, float y);
    void onLeftButtonUp(float x, float y);
    void onMouseWheel(int delta);
    void onKeyDown(WPARAM key);
    void onChar(wchar_t character);
    void setFocusTarget(Id id);
    TextEntry* focusedEntry();
    void resetCaretBlink();
    void ensureCaretVisible(const Rect& rect, TextEntry& entry);
    int characterIndexAtX(const Rect& rect, const TextEntry& entry, float x) const;

    // ---- actions ----
    void browseForSource();
    void browseForOutput();
    void scanFiles();
    void startConversion();
    void toggleTheme();
    void toggleLanguage();
    void applyThemeToWindow();
    void rebuildFonts();

    // ---- helpers ----
    std::wstring text(Key key) const;
    bool isHovered(Id id) const { return hover_ == id; }
    bool isPressed(Id id) const { return pressed_ == id; }
    void updateScrollThumb();
    float listContentHeight() const;
    float maxListScroll() const;
    int listColumnCount() const { return 3; }

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    float scale_ = 1.0f;

    Language language_ = Language::Chinese;
    bool darkMode_ = false;

    std::wstring sourceDirectory_;
    std::wstring outputDirectory_;
    int outputFormatIndex_ = 0;   // 0 = FLAC, 1 = WAV
    std::vector<ConversionItem> items_;

    TextEntry sourceEntry_;
    TextEntry outputEntry_;
    Id focus_ = Id::None;

    float listScroll_ = 0.0f;
    bool draggingThumb_ = false;
    float thumbGrabOffset_ = 0.0f;
    Rect scrollThumb_;

    bool comboOpen_ = false;

    float progress_ = 0.0f;
    std::wstring statusText_;

    std::thread worker_;
    std::atomic<bool> cancel_{ false };
    bool running_ = false;
    std::mutex summaryMutex_;
    ConversionSummary pendingSummary_;
    bool hasPendingSummary_ = false;

    Id hover_ = Id::None;
    Id pressed_ = Id::None;
    bool trackingLeave_ = false;

    int languageAnimationFrame_ = -1;
    bool themeAnimating_ = false;
    int themeStep_ = 0;
    bool themeFadingOut_ = false;

    bool tooltipVisible_ = false;
    bool caretVisible_ = true;

    Layout layout_;
    FontSet fonts_;
};

} // namespace gui
} // namespace ac
