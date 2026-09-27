// main_window.cpp - the custom-drawn application window.
//
// The window reproduces the original CustomTkinter layout with GDI+: rounded
// buttons and fields, a themed file list with its own scrollbar, a progress bar,
// an animated language switch and a theme cross-fade.
#include "main_window.h"

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <objbase.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../core/audio.h"
#include "../core/util.h"

using namespace Gdiplus;

namespace ac {
namespace gui {
namespace {

constexpr wchar_t kWindowClassName[] = L"AudioConverterMainWindow";
constexpr wchar_t kWindowTitle[] = L"Audio Converter";

constexpr UINT_PTR kTimerCaret = 1;
constexpr UINT_PTR kTimerLanguage = 2;
constexpr UINT_PTR kTimerTheme = 3;
constexpr UINT_PTR kTimerTooltip = 4;

constexpr UINT kMessageProgress = WM_APP + 1;
constexpr UINT kMessageFinished = WM_APP + 2;

constexpr int kClientWidth = 780;
constexpr int kClientHeight = 680;
constexpr int kMinClientWidth = 660;
constexpr int kMinClientHeight = 560;

constexpr float kCornerRadius = 8.0f;
constexpr float kEntryPadding = 10.0f;

// DWMWA_USE_IMMERSIVE_DARK_MODE (20 on current Windows, 19 on early builds).
void applyDarkTitleBar(HWND hwnd, bool dark) {
    const BOOL value = dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &value, sizeof(value))))
        DwmSetWindowAttribute(hwnd, 19, &value, sizeof(value));
}

UINT queryWindowDpi(HWND hwnd) {
    // GetDpiForWindow is available from Windows 10 1607 onwards.
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static GetDpiForWindowFn getDpiForWindow = []() -> GetDpiForWindowFn {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (!user32) return nullptr;
        return reinterpret_cast<GetDpiForWindowFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForWindow")));
    }();
    if (getDpiForWindow) {
        const UINT dpi = getDpiForWindow(hwnd);
        if (dpi >= 72) return dpi;
    }
    HDC dc = GetDC(hwnd);
    const UINT dpi = dc ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX)) : 96;
    if (dc) ReleaseDC(hwnd, dc);
    return dpi ? dpi : 96;
}

std::wstring pickFolder(HWND owner, const std::wstring& title) {
    IFileOpenDialog* dialog = nullptr;
    const HRESULT created = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                             CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                             reinterpret_cast<void**>(&dialog));
    if (FAILED(created) || !dialog) return std::wstring();

    std::wstring result;
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    dialog->SetTitle(title.c_str());
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                result.assign(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
MainWindow::~MainWindow() {
    cancel_.store(true);
    if (worker_.joinable()) worker_.join();
}

bool MainWindow::create(HINSTANCE instance, int showCommand, std::wstring* error) {
    instance_ = instance;

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = &MainWindow::windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClassName;
    windowClass.hIcon = static_cast<HICON>(LoadImageW(instance, L"APPICON", IMAGE_ICON,
                                                     0, 0, LR_DEFAULTSIZE));
    windowClass.hIconSm = windowClass.hIcon;
    if (!RegisterClassExW(&windowClass)) {
        const DWORD code = GetLastError();
        if (code != ERROR_CLASS_ALREADY_EXISTS) {
            if (error)
                *error = L"RegisterClassExW failed (error " + std::to_wstring(code) + L")";
            return false;
        }
    }

    hwnd_ = CreateWindowExW(WS_EX_LAYERED, kWindowClassName, kWindowTitle,
                            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            kClientWidth, kClientHeight, nullptr, nullptr, instance, this);
    if (!hwnd_) {
        const DWORD code = GetLastError();
        if (error) *error = L"CreateWindowExW failed (error " + std::to_wstring(code) + L")";
        return false;
    }

    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    updateScale();

    // Size the window so the client area matches the design size exactly.
    RECT windowRect = {};
    RECT clientRect = {};
    GetWindowRect(hwnd_, &windowRect);
    GetClientRect(hwnd_, &clientRect);
    const int frameWidth = (windowRect.right - windowRect.left) -
                           (clientRect.right - clientRect.left);
    const int frameHeight = (windowRect.bottom - windowRect.top) -
                            (clientRect.bottom - clientRect.top);
    const int wantedWidth = static_cast<int>(kClientWidth * scale_) + frameWidth;
    const int wantedHeight = static_cast<int>(kClientHeight * scale_) + frameHeight;

    RECT workArea = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int x = workArea.left + ((workArea.right - workArea.left) - wantedWidth) / 2;
    const int y = workArea.top + ((workArea.bottom - workArea.top) - wantedHeight) / 2;

    SetWindowPos(hwnd_, nullptr, x, y, wantedWidth, wantedHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    applyThemeToWindow();

    ShowWindow(hwnd_, showCommand);
    UpdateWindow(hwnd_);
    return true;
}

void MainWindow::applyStartupOptions(const std::wstring& sourceFolder,
                                     const std::wstring& outputFolder,
                                     int formatIndex) {
    if (formatIndex == 0 || formatIndex == 1) outputFormatIndex_ = formatIndex;

    if (!outputFolder.empty()) {
        outputDirectory_ = outputFolder;
        outputEntry_.text = outputFolder;
        outputEntry_.caret = static_cast<int>(outputFolder.size());
        outputEntry_.clearSelection();
    }

    if (!sourceFolder.empty()) {
        const DWORD attributes = GetFileAttributesW(sourceFolder.c_str());
        const bool isDirectory = attributes != INVALID_FILE_ATTRIBUTES &&
                                 (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDirectory) {
            sourceDirectory_ = sourceFolder;
            sourceEntry_.text = sourceFolder;
            sourceEntry_.caret = static_cast<int>(sourceFolder.size());
            sourceEntry_.clearSelection();
            scanFiles();
        }
    }
    invalidate();
}

int MainWindow::runMessageLoop() {
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK MainWindow::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = reinterpret_cast<MainWindow*>(createStruct->lpCreateParams);
        // Publish the handle before WM_CREATE/WM_SIZE arrive: CreateWindowEx has
        // not returned yet, so the member would still be null otherwise.
        if (self) self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
    return self->handleMessage(message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
void MainWindow::updateScale() {
    dpi_ = queryWindowDpi(hwnd_);
    scale_ = static_cast<float>(dpi_) / 96.0f;
    rebuildFonts();
}

void MainWindow::rebuildFonts() {
    fonts_.rebuild(language_ == Language::Chinese, scale_);
}

void MainWindow::invalidate() {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::computeLayout() {
    RECT client = {};
    GetClientRect(hwnd_, &client);
    const float width = static_cast<float>(client.right);
    const float height = static_cast<float>(client.bottom);
    auto s = [this](float value) { return value * scale_; };

    HDC dc = GetDC(hwnd_);
    Graphics graphics(dc);
    graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

    Layout& L = layout_;
    L.rowHeight = s(28.0f);
    L.headerHeight = s(28.0f);

    const float padX = s(28.0f);
    const float padY = s(20.0f);
    const float rowHeight = s(34.0f);
    const float buttonWidth = s(90.0f);

    // Title row ------------------------------------------------------------
    const float titleRowHeight = s(36.0f);
    float y = padY;
    const float smallButton = s(40.0f);
    L.languageButton = { width - padX - smallButton, y, smallButton, titleRowHeight };
    L.themeButton = { L.languageButton.x - s(8.0f) - smallButton, y, smallButton,
                      titleRowHeight };
    const float titleWidth = std::max(0.0f, L.themeButton.x - s(12.0f) - padX);
    L.title = { padX, y, titleWidth, titleRowHeight };
    y += titleRowHeight + s(12.0f);

    // Source row -----------------------------------------------------------
    const std::wstring sourceLabelText = text(Key::SourceFolder);
    const float sourceLabelWidth = measureText(graphics, sourceLabelText, fonts_.label);
    L.sourceLabel = { padX, y, sourceLabelWidth, rowHeight };
    L.sourceBrowse = { width - padX - buttonWidth, y, buttonWidth, rowHeight };
    const float sourceEntryX = L.sourceLabel.right() + s(10.0f);
    L.sourceEntry = { sourceEntryX, y, std::max(s(40.0f), L.sourceBrowse.x - s(8.0f) - sourceEntryX),
                      rowHeight };
    y += rowHeight + s(10.0f);
    const float listTop = y;

    // Bottom section, laid out upwards from the start button ---------------
    const float startHeight = s(44.0f);
    L.startButton = { padX, height - padY - startHeight, width - padX * 2.0f, startHeight };

    L.progressLabel = { padX, L.startButton.y - s(10.0f) - s(16.0f), width - padX * 2.0f,
                        s(16.0f) };
    L.progressTrack = { padX, L.progressLabel.y - s(3.0f) - s(6.0f), width - padX * 2.0f,
                        s(6.0f) };

    const float formatRowY = L.progressTrack.y - s(10.0f) - rowHeight;
    const std::wstring formatLabelText = text(Key::OutputFormat);
    const float formatLabelWidth = measureText(graphics, formatLabelText, fonts_.label);
    L.formatLabel = { padX, formatRowY, formatLabelWidth, rowHeight };
    L.formatCombo = { L.formatLabel.right() + s(10.0f), formatRowY, s(120.0f), rowHeight };
    L.comboDropdown = { L.formatCombo.x, L.formatCombo.bottom(), L.formatCombo.w,
                        s(30.0f) * 2.0f };

    const float outputRowY = formatRowY - s(10.0f) - rowHeight;
    const std::wstring outputLabelText = text(Key::OutputFolder);
    const float outputLabelWidth = measureText(graphics, outputLabelText, fonts_.label);
    L.outputLabel = { padX, outputRowY, outputLabelWidth, rowHeight };
    L.outputBrowse = { width - padX - buttonWidth, outputRowY, buttonWidth, rowHeight };
    const float outputEntryX = L.outputLabel.right() + s(10.0f);
    L.outputEntry = { outputEntryX, outputRowY,
                      std::max(s(40.0f), L.outputBrowse.x - s(8.0f) - outputEntryX), rowHeight };

    // List -----------------------------------------------------------------
    const float listBottom = outputRowY - s(18.0f);
    L.listFrame = { padX, listTop, width - padX * 2.0f, std::max(s(60.0f), listBottom - listTop) };
    L.countLabel = { L.listFrame.x, L.listFrame.bottom() - s(16.0f), L.listFrame.w, s(16.0f) };
    const float listAreaHeight = std::max(s(30.0f), L.listFrame.h - s(20.0f));
    L.listArea = { L.listFrame.x, L.listFrame.y, std::max(s(80.0f), L.listFrame.w - s(14.0f)),
                   listAreaHeight };
    L.listHeader = { L.listArea.x, L.listArea.y, L.listArea.w, L.headerHeight };
    L.listViewport = { L.listArea.x, L.listArea.y + L.headerHeight, L.listArea.w,
                       std::max(s(10.0f), L.listArea.h - L.headerHeight) };
    L.listScrollbar = { L.listFrame.right() - s(10.0f), L.listFrame.y, s(10.0f),
                        listAreaHeight };

    ReleaseDC(hwnd_, dc);
    updateScrollThumb();
}

float MainWindow::listContentHeight() const {
    return static_cast<float>(items_.size()) * layout_.rowHeight;
}

float MainWindow::maxListScroll() const {
    return std::max(0.0f, listContentHeight() - layout_.listViewport.h);
}

void MainWindow::updateScrollThumb() {
    const Rect& track = layout_.listScrollbar;
    const Rect& viewport = layout_.listViewport;
    const float content = listContentHeight();
    if (content <= 0.0f || content <= viewport.h) {
        scrollThumb_ = { track.x, track.y, track.w, 0.0f };
        return;
    }
    const float ratio = viewport.h / content;
    const float thumbHeight = std::max(24.0f * scale_, track.h * ratio);
    const float maxScroll = maxListScroll();
    const float position = (maxScroll > 0.0f) ? (listScroll_ / maxScroll) : 0.0f;
    const float travel = track.h - thumbHeight;
    scrollThumb_ = { track.x, track.y + travel * position, track.w, thumbHeight };
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------
void MainWindow::paint(HDC target) {
    RECT client = {};
    GetClientRect(hwnd_, &client);
    if (client.right <= 0 || client.bottom <= 0) return;

    HDC memoryDC = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ previous = SelectObject(memoryDC, bitmap);

    {
        Graphics graphics(memoryDC);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        graphics.SetPixelOffsetMode(PixelOffsetModeHalf);

        const Theme& theme = darkMode_ ? darkTheme() : lightTheme();
        fillRect(graphics, { 0.0f, 0.0f, static_cast<float>(client.right),
                             static_cast<float>(client.bottom) }, theme.background);

        drawTitle(graphics, theme);
        drawFolderRow(graphics, theme, Id::SourceEntry, layout_.sourceLabel,
                      text(Key::SourceFolder), sourceEntry_, layout_.sourceEntry,
                      layout_.sourceBrowse, focus_ == Id::SourceEntry);
        drawList(graphics, theme);
        drawFolderRow(graphics, theme, Id::OutputEntry, layout_.outputLabel,
                      text(Key::OutputFolder), outputEntry_, layout_.outputEntry,
                      layout_.outputBrowse, focus_ == Id::OutputEntry);
        drawFormatRow(graphics, theme);
        drawProgress(graphics, theme);
        drawStartButton(graphics, theme);
        drawTooltip(graphics, theme);
    }

    BitBlt(target, 0, 0, client.right, client.bottom, memoryDC, 0, 0, SRCCOPY);
    SelectObject(memoryDC, previous);
    DeleteObject(bitmap);
    DeleteDC(memoryDC);
}

void MainWindow::drawTitle(Graphics& graphics, const Theme& theme) {
    drawText(graphics, text(Key::Title), fonts_.title, layout_.title, theme.text, Align::Near,
             false);

    // Language toggle.
    const Rect& language = layout_.languageButton;
    const bool languageHover = isHovered(Id::LanguageButton);
    fillRoundRect(graphics, language, kCornerRadius * scale_,
                  languageHover ? theme.surfaceAlt : theme.surface);
    strokeRoundRect(graphics, language, kCornerRadius * scale_, theme.border, 1.0f * scale_);
    drawText(graphics, text(Key::LanguageSwitch), fonts_.button, language, theme.text,
             Align::Center, false);

    // Theme toggle.
    const Rect& themeButton = layout_.themeButton;
    const bool themeHover = isHovered(Id::ThemeButton);
    fillRoundRect(graphics, themeButton, kCornerRadius * scale_,
                  themeHover ? theme.surfaceAlt : theme.surface);
    strokeRoundRect(graphics, themeButton, kCornerRadius * scale_, theme.border, 1.0f * scale_);
    drawText(graphics, darkMode_ ? L"\u2600" : L"\u263E", fonts_.button, themeButton,
             theme.text, Align::Center, false);
}

void MainWindow::drawFolderRow(Graphics& graphics, const Theme& theme, Id labelId,
                               const Rect& labelRect, const std::wstring& labelText,
                               TextEntry& entry, const Rect& entryRect,
                               const Rect& browseRect, bool focused) {
    (void)labelId;
    drawText(graphics, labelText, fonts_.label, labelRect, theme.text, Align::Near, false);

    const bool enabled = !running_;
    const bool hover = isHovered(labelId == Id::SourceEntry ? Id::SourceBrowse : Id::OutputBrowse);
    fillRoundRect(graphics, entryRect, kCornerRadius * scale_, theme.surface);
    strokeRoundRect(graphics, entryRect, kCornerRadius * scale_,
                    focused ? theme.accent : theme.border, (focused ? 1.6f : 1.0f) * scale_);
    drawEntryContents(graphics, theme, entryRect, entry, focused,
                      labelId == Id::SourceEntry ? text(Key::SourcePlaceholder)
                                                 : text(Key::OutputPlaceholder),
                      !entry.text.empty());

    const bool browseHover = hover && enabled;
    fillRoundRect(graphics, browseRect, kCornerRadius * scale_,
                  browseHover ? theme.accentHover : theme.accent);
    drawText(graphics, text(Key::Browse), fonts_.button, browseRect, theme.accentText,
             Align::Center, false);
}

void MainWindow::drawEntryContents(Graphics& graphics, const Theme& theme, const Rect& rect,
                                   const TextEntry& entry, bool focused,
                                   const std::wstring& placeholder, bool hasText) {
    const float padding = kEntryPadding * scale_;
    const Rect inner = { rect.x + padding, rect.y, rect.w - padding * 2.0f, rect.h };
    graphics.SetClip(RectF(inner.x, inner.y, inner.w, inner.h));

    const float textY = centeredTextY(graphics, fonts_.entry, rect);
    if (!hasText) {
        drawTextAt(graphics, placeholder, fonts_.entry, inner.x, textY, theme.textMuted);
    } else {
        if (focused && entry.hasSelection()) {
            const float begin = measureText(
                graphics, entry.text.substr(0, static_cast<size_t>(entry.selectionBegin())),
                fonts_.entry);
            const float end = measureText(
                graphics, entry.text.substr(0, static_cast<size_t>(entry.selectionEnd())),
                fonts_.entry);
            Rect selection = { inner.x - entry.scroll + begin, rect.y + 4.0f * scale_,
                               std::max(1.0f, end - begin), rect.h - 8.0f * scale_ };
            fillRect(graphics, selection, Color(80, 0x2C, 0xC9, 0x85));
        }
        drawTextAt(graphics, entry.text, fonts_.entry, inner.x - entry.scroll, textY,
                   theme.text);
    }
    graphics.ResetClip();

    if (focused && caretVisible_) {
        const float caretOffset = measureText(
            graphics, entry.text.substr(0, static_cast<size_t>(entry.caret)), fonts_.entry);
        Rect caret = { inner.x - entry.scroll + caretOffset, rect.y + 6.0f * scale_,
                       std::max(1.0f, scale_), rect.h - 12.0f * scale_ };
        fillRect(graphics, caret, theme.text);
    }
}

void MainWindow::drawList(Graphics& graphics, const Theme& theme) {
    const Rect& viewport = layout_.listViewport;
    const Rect& header = layout_.listHeader;

    fillRect(graphics, layout_.listArea, theme.listBackground);
    fillRect(graphics, header, theme.listHeaderBackground);

    // Column geometry: name takes the remaining space, format and size are fixed.
    const float sizeWidth = 100.0f * scale_;
    const float formatWidth = 80.0f * scale_;
    const float nameWidth = std::max(40.0f * scale_, layout_.listArea.w - sizeWidth - formatWidth);
    const float textPadding = 6.0f * scale_;
    const Rect nameColumn = { layout_.listArea.x + textPadding, header.y,
                              nameWidth - textPadding * 2.0f, header.h };
    const Rect formatColumn = { layout_.listArea.x + nameWidth + textPadding, header.y,
                                formatWidth - textPadding * 2.0f, header.h };
    const Rect sizeColumn = { layout_.listArea.x + nameWidth + formatWidth + textPadding,
                              header.y, sizeWidth - textPadding * 2.0f, header.h };

    drawText(graphics, text(Key::FileName), fonts_.listHeader, nameColumn,
             theme.listHeaderText, Align::Near, true);
    drawText(graphics, text(Key::Format), fonts_.listHeader, formatColumn,
             theme.listHeaderText, Align::Near, true);
    drawText(graphics, text(Key::Size), fonts_.listHeader, sizeColumn, theme.listHeaderText,
             Align::Near, true);

    graphics.SetClip(RectF(viewport.x, viewport.y, viewport.w, viewport.h));
    const float rowHeight = layout_.rowHeight;
    const size_t firstRow = (rowHeight > 0.0f)
                                ? static_cast<size_t>(std::max(0.0f, listScroll_ / rowHeight))
                                : 0;
    const size_t visibleRows = static_cast<size_t>(viewport.h / std::max(1.0f, rowHeight)) + 2;

    for (size_t index = firstRow;
         index < items_.size() && index < firstRow + visibleRows; ++index) {
        const float rowY = viewport.y + static_cast<float>(index) * rowHeight - listScroll_;
        if (rowY + rowHeight < viewport.y || rowY > viewport.bottom()) continue;

        const ConversionItem& item = items_[index];
        Rect nameRect = { nameColumn.x, rowY, nameColumn.w, rowHeight };
        Rect formatRect = { formatColumn.x, rowY, formatColumn.w, rowHeight };
        Rect sizeRect = { sizeColumn.x, rowY, sizeColumn.w, rowHeight };
        drawText(graphics, item.relativeName, fonts_.listRow, nameRect, theme.listRowText,
                 Align::Near, true);
        drawText(graphics, item.formatLabel, fonts_.listRow, formatRect, theme.listRowText,
                 Align::Near, true);
        drawText(graphics, formatSize(item.sizeBytes), fonts_.listRow, sizeRect,
                 theme.listRowText, Align::Near, true);
    }
    graphics.ResetClip();

    // Scrollbar.
    if (scrollThumb_.h > 0.0f) {
        Rect thumb = scrollThumb_;
        thumb.x += 1.0f * scale_;
        thumb.w -= 2.0f * scale_;
        fillRoundRect(graphics, thumb, thumb.w * 0.5f, theme.scrollThumb);
    }

    drawText(graphics, formatFilesCount(language_, items_.size()), fonts_.small,
             layout_.countLabel, theme.textMuted, Align::Far, false);
}

void MainWindow::drawFormatRow(Graphics& graphics, const Theme& theme) {
    drawText(graphics, text(Key::OutputFormat), fonts_.label, layout_.formatLabel, theme.text,
             Align::Near, false);

    const Rect& combo = layout_.formatCombo;
    const bool hover = isHovered(Id::FormatCombo);
    fillRoundRect(graphics, combo, kCornerRadius * scale_,
                  hover ? theme.surfaceAlt : theme.surface);
    strokeRoundRect(graphics, combo, kCornerRadius * scale_,
                    comboOpen_ ? theme.accent : theme.border,
                    (comboOpen_ ? 1.6f : 1.0f) * scale_);

    const wchar_t* value = (outputFormatIndex_ == 1) ? L"WAV" : L"FLAC";
    Rect valueRect = { combo.x + 12.0f * scale_, combo.y, combo.w - 34.0f * scale_, combo.h };
    drawText(graphics, value, fonts_.entry, valueRect, theme.text, Align::Near, false);

    // Chevron.
    const float centerX = combo.right() - 17.0f * scale_;
    const float centerY = combo.y + combo.h * 0.5f;
    const float halfWidth = 5.0f * scale_;
    const float halfHeight = 2.5f * scale_;
    GraphicsPath chevron;
    chevron.AddLine(centerX - halfWidth, centerY - halfHeight, centerX, centerY + halfHeight);
    chevron.AddLine(centerX, centerY + halfHeight, centerX + halfWidth, centerY - halfHeight);
    Pen pen(theme.textMuted, 1.6f * scale_);
    graphics.DrawPath(&pen, &chevron);

    if (comboOpen_) {
        const Rect& dropdown = layout_.comboDropdown;
        fillRoundRect(graphics, dropdown, kCornerRadius * scale_, theme.surface);
        strokeRoundRect(graphics, dropdown, kCornerRadius * scale_, theme.border, 1.0f * scale_);

        const wchar_t* values[2] = { L"FLAC", L"WAV" };
        const float itemHeight = dropdown.h * 0.5f;
        for (int i = 0; i < 2; ++i) {
            Rect item = { dropdown.x + 2.0f * scale_, dropdown.y + itemHeight * i,
                          dropdown.w - 4.0f * scale_, itemHeight };
            const Id itemId = (i == 0) ? Id::ComboItem0 : Id::ComboItem1;
            if (isHovered(itemId)) {
                fillRoundRect(graphics, item, kCornerRadius * scale_, theme.surfaceAlt);
            }
            Rect textRect = { item.x + 10.0f * scale_, item.y,
                              item.w - 20.0f * scale_, item.h };
            drawText(graphics, values[i], fonts_.entry, textRect,
                     (i == outputFormatIndex_) ? theme.accent : theme.text, Align::Near, false);
        }
    }
}

void MainWindow::drawProgress(Graphics& graphics, const Theme& theme) {
    const Rect& track = layout_.progressTrack;
    fillRoundRect(graphics, track, track.h * 0.5f, theme.track);

    const float clamped = std::max(0.0f, std::min(1.0f, progress_));
    if (clamped > 0.0f) {
        Rect fill = { track.x, track.y, track.w * clamped, track.h };
        fillRoundRect(graphics, fill, track.h * 0.5f, theme.fill);
    }
    if (!statusText_.empty()) {
        drawText(graphics, statusText_, fonts_.small, layout_.progressLabel, theme.textMuted,
                 Align::Near, true);
    }
}

void MainWindow::drawStartButton(Graphics& graphics, const Theme& theme) {
    const Rect& button = layout_.startButton;
    Color fillColor = theme.accent;
    if (running_) {
        fillColor = darkMode_ ? Color(255, 0x1F, 0x6B, 0x4B) : Color(255, 0x9A, 0xDD, 0xBD);
    } else if (isHovered(Id::StartButton)) {
        fillColor = theme.accentHover;
    }
    fillRoundRect(graphics, button, kCornerRadius * scale_, fillColor);
    drawText(graphics, text(running_ ? Key::Converting : Key::Start), fonts_.startButton,
             button, theme.accentText, Align::Center, false);
}

void MainWindow::drawTooltip(Graphics& graphics, const Theme& theme) {
    if (!tooltipVisible_) return;
    const std::wstring message = tr(language_, Key::ThemeTip);

    const float padding = 10.0f * scale_;
    const float width = measureText(graphics, message, fonts_.small) + padding * 2.0f;
    const float height = 26.0f * scale_;
    Rect tooltip = { layout_.themeButton.right() - width,
                     layout_.themeButton.bottom() + 6.0f * scale_, width, height };
    if (tooltip.x < 4.0f * scale_) tooltip.x = 4.0f * scale_;

    fillRoundRect(graphics, tooltip, 6.0f * scale_, theme.tooltipBackground);
    drawText(graphics, message, fonts_.small, tooltip, theme.tooltipText, Align::Center, false);
}

// ---------------------------------------------------------------------------
// Interaction helpers
// ---------------------------------------------------------------------------
std::wstring MainWindow::text(Key key) const {
    const std::wstring full = tr(language_, key);
    if (languageAnimationFrame_ < 0 || full.empty()) return full;
    const float progress = static_cast<float>(languageAnimationFrame_ + 1) / 10.0f;
    size_t count = static_cast<size_t>(static_cast<float>(full.size()) * progress + 0.5f);
    if (count < 1) count = 1;
    if (count > full.size()) count = full.size();
    return full.substr(0, count);
}

TextEntry* MainWindow::focusedEntry() {
    if (focus_ == Id::SourceEntry) return &sourceEntry_;
    if (focus_ == Id::OutputEntry) return &outputEntry_;
    return nullptr;
}

void MainWindow::resetCaretBlink() {
    caretVisible_ = true;
    if (hwnd_) {
        KillTimer(hwnd_, kTimerCaret);
        SetTimer(hwnd_, kTimerCaret, 530, nullptr);
    }
}

void MainWindow::setFocusTarget(Id id) {
    focus_ = (id == Id::SourceEntry || id == Id::OutputEntry) ? id : Id::None;
    if (focus_ == Id::None) {
        if (hwnd_) KillTimer(hwnd_, kTimerCaret);
    } else {
        resetCaretBlink();
    }
    invalidate();
}

void MainWindow::ensureCaretVisible(const Rect& rect, TextEntry& entry) {
    HDC dc = GetDC(hwnd_);
    Graphics graphics(dc);
    const float padding = kEntryPadding * scale_;
    const float available = std::max(1.0f, rect.w - padding * 2.0f);
    const float caretOffset = measureText(
        graphics, entry.text.substr(0, static_cast<size_t>(entry.caret)), fonts_.entry);
    if (caretOffset - entry.scroll > available) entry.scroll = caretOffset - available;
    if (caretOffset - entry.scroll < 0.0f) entry.scroll = caretOffset;
    const float total = measureText(graphics, entry.text, fonts_.entry);
    const float maximum = std::max(0.0f, total - available);
    if (entry.scroll > maximum) entry.scroll = maximum;
    if (entry.scroll < 0.0f) entry.scroll = 0.0f;
    ReleaseDC(hwnd_, dc);
}

int MainWindow::characterIndexAtX(const Rect& rect, const TextEntry& entry, float x) const {
    HDC dc = GetDC(hwnd_);
    Graphics graphics(dc);
    const float padding = kEntryPadding * scale_;
    const float local = x - (rect.x + padding) + entry.scroll;

    int result = 0;
    float previous = 0.0f;
    for (size_t i = 1; i <= entry.text.size(); ++i) {
        const float current = measureText(graphics, entry.text.substr(0, i), fonts_.entry);
        if (local < (previous + current) * 0.5f) { result = static_cast<int>(i) - 1; break; }
        previous = current;
        result = static_cast<int>(i);
    }
    if (local <= 0.0f) result = 0;
    ReleaseDC(hwnd_, dc);
    return result;
}

Id MainWindow::hitTest(float x, float y) const {
    if (comboOpen_ && layout_.comboDropdown.contains(x, y)) {
        const float itemHeight = layout_.comboDropdown.h * 0.5f;
        return (y < layout_.comboDropdown.y + itemHeight) ? Id::ComboItem0 : Id::ComboItem1;
    }
    if (layout_.themeButton.contains(x, y)) return Id::ThemeButton;
    if (layout_.languageButton.contains(x, y)) return Id::LanguageButton;
    if (layout_.sourceEntry.contains(x, y)) return Id::SourceEntry;
    if (layout_.sourceBrowse.contains(x, y)) return Id::SourceBrowse;
    if (layout_.outputEntry.contains(x, y)) return Id::OutputEntry;
    if (layout_.outputBrowse.contains(x, y)) return Id::OutputBrowse;
    if (layout_.formatCombo.contains(x, y)) return Id::FormatCombo;
    if (layout_.startButton.contains(x, y)) return Id::StartButton;
    if (scrollThumb_.h > 0.0f && scrollThumb_.contains(x, y)) return Id::ListScrollThumb;
    if (layout_.listScrollbar.contains(x, y)) return Id::ListScrollTrack;
    return Id::None;
}

void MainWindow::onMouseMove(float x, float y) {
    const Id hit = hitTest(x, y);

    if (draggingThumb_) {
        const Rect& track = layout_.listScrollbar;
        const float travel = std::max(1.0f, track.h - scrollThumb_.h);
        float position = (y - thumbGrabOffset_ - track.y) / travel;
        position = std::max(0.0f, std::min(1.0f, position));
        listScroll_ = position * maxListScroll();
        updateScrollThumb();
        invalidate();
        return;
    }

    if (hit != hover_) {
        hover_ = hit;
        if (hit != Id::ThemeButton) {
            tooltipVisible_ = false;
            KillTimer(hwnd_, kTimerTooltip);
        } else {
            SetTimer(hwnd_, kTimerTooltip, 450, nullptr);
        }
        invalidate();
    }

    if (!trackingLeave_) {
        TRACKMOUSEEVENT track = {};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd_;
        if (TrackMouseEvent(&track)) trackingLeave_ = true;
    }
}

void MainWindow::onLeftButtonDown(float x, float y) {
    const Id hit = hitTest(x, y);

    if (comboOpen_) {
        if (hit == Id::ComboItem0 || hit == Id::ComboItem1) {
            outputFormatIndex_ = (hit == Id::ComboItem0) ? 0 : 1;
        }
        comboOpen_ = false;
        if (hit != Id::FormatCombo) invalidate();
        if (hit == Id::ComboItem0 || hit == Id::ComboItem1) { invalidate(); return; }
        if (hit != Id::FormatCombo) { invalidate(); return; }
        invalidate();
        return;
    }

    switch (hit) {
        case Id::SourceEntry:
        case Id::OutputEntry: {
            TextEntry& entry = (hit == Id::SourceEntry) ? sourceEntry_ : outputEntry_;
            const Rect& rect = (hit == Id::SourceEntry) ? layout_.sourceEntry
                                                        : layout_.outputEntry;
            const int index = characterIndexAtX(rect, entry, x);
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (!shift || focus_ != hit) entry.anchor = index;
            entry.caret = index;
            setFocusTarget(hit);
            invalidate();
            break;
        }
        case Id::ListScrollThumb:
            draggingThumb_ = true;
            thumbGrabOffset_ = y - scrollThumb_.y;
            SetCapture(hwnd_);
            break;
        case Id::ListScrollTrack: {
            const float step = layout_.listViewport.h;
            listScroll_ = (y < scrollThumb_.y) ? std::max(0.0f, listScroll_ - step)
                                               : std::min(maxListScroll(), listScroll_ + step);
            updateScrollThumb();
            invalidate();
            break;
        }
        default:
            pressed_ = hit;
            if (hit != Id::None) SetCapture(hwnd_);
            break;
    }
}

void MainWindow::onLeftButtonUp(float x, float y) {
    if (draggingThumb_) {
        draggingThumb_ = false;
        ReleaseCapture();
        return;
    }
    const Id hit = hitTest(x, y);
    const Id pressed = pressed_;
    pressed_ = Id::None;
    if (GetCapture() == hwnd_) ReleaseCapture();

    if (hit != pressed) {
        invalidate();
        return;
    }

    switch (pressed) {
        case Id::ThemeButton:    toggleTheme(); break;
        case Id::LanguageButton: toggleLanguage(); break;
        case Id::SourceBrowse:   browseForSource(); break;
        case Id::OutputBrowse:   browseForOutput(); break;
        case Id::FormatCombo:    comboOpen_ = true; invalidate(); break;
        case Id::StartButton:    startConversion(); break;
        default: break;
    }
}

void MainWindow::onMouseWheel(int delta) {
    if (items_.empty()) return;
    const float step = layout_.rowHeight * 3.0f;
    listScroll_ -= (static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA)) * step;
    listScroll_ = std::max(0.0f, std::min(maxListScroll(), listScroll_));
    updateScrollThumb();
    invalidate();
}

void MainWindow::onChar(wchar_t character) {
    TextEntry* entry = focusedEntry();
    if (!entry) return;
    if (character < 32 || character == 127) return;

    if (entry->hasSelection()) {
        entry->text.erase(static_cast<size_t>(entry->selectionBegin()),
                          static_cast<size_t>(entry->selectionEnd() - entry->selectionBegin()));
        entry->caret = entry->selectionBegin();
        entry->clearSelection();
    }
    entry->text.insert(static_cast<size_t>(entry->caret), 1, character);
    ++entry->caret;
    entry->clearSelection();

    const Rect& rect = (focus_ == Id::SourceEntry) ? layout_.sourceEntry : layout_.outputEntry;
    ensureCaretVisible(rect, *entry);
    resetCaretBlink();
    invalidate();
}

void MainWindow::onKeyDown(WPARAM key) {
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

    if (key == VK_ESCAPE && comboOpen_) {
        comboOpen_ = false;
        invalidate();
        return;
    }

    TextEntry* entry = focusedEntry();
    if (!entry) return;
    const Rect& rect = (focus_ == Id::SourceEntry) ? layout_.sourceEntry : layout_.outputEntry;

    auto deleteSelection = [&]() -> bool {
        if (!entry->hasSelection()) return false;
        entry->text.erase(static_cast<size_t>(entry->selectionBegin()),
                          static_cast<size_t>(entry->selectionEnd() - entry->selectionBegin()));
        entry->caret = entry->selectionBegin();
        entry->clearSelection();
        return true;
    };

    if (control) {
        switch (key) {
            case 'A':
                entry->anchor = 0;
                entry->caret = static_cast<int>(entry->text.size());
                invalidate();
                return;
            case 'C':
                if (entry->hasSelection() && OpenClipboard(hwnd_)) {
                    EmptyClipboard();
                    const std::wstring selected = entry->text.substr(
                        static_cast<size_t>(entry->selectionBegin()),
                        static_cast<size_t>(entry->selectionEnd() - entry->selectionBegin()));
                    const size_t bytes = (selected.size() + 1) * sizeof(wchar_t);
                    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
                        if (void* target = GlobalLock(memory)) {
                            memcpy(target, selected.c_str(), bytes);
                            GlobalUnlock(memory);
                            SetClipboardData(CF_UNICODETEXT, memory);
                        }
                    }
                    CloseClipboard();
                }
                return;
            case 'X':
                if (entry->hasSelection() && OpenClipboard(hwnd_)) {
                    EmptyClipboard();
                    const std::wstring selected = entry->text.substr(
                        static_cast<size_t>(entry->selectionBegin()),
                        static_cast<size_t>(entry->selectionEnd() - entry->selectionBegin()));
                    const size_t bytes = (selected.size() + 1) * sizeof(wchar_t);
                    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
                        if (void* target = GlobalLock(memory)) {
                            memcpy(target, selected.c_str(), bytes);
                            GlobalUnlock(memory);
                            SetClipboardData(CF_UNICODETEXT, memory);
                        }
                    }
                    CloseClipboard();
                    deleteSelection();
                    ensureCaretVisible(rect, *entry);
                    resetCaretBlink();
                    invalidate();
                }
                return;
            case 'V':
                if (OpenClipboard(hwnd_)) {
                    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
                        if (const wchar_t* source = static_cast<const wchar_t*>(
                                GlobalLock(data))) {
                            std::wstring pasted(source);
                            GlobalUnlock(data);
                            while (!pasted.empty() &&
                                   (pasted.back() == L'\r' || pasted.back() == L'\n'))
                                pasted.pop_back();
                            deleteSelection();
                            entry->text.insert(static_cast<size_t>(entry->caret), pasted);
                            entry->caret += static_cast<int>(pasted.size());
                            entry->clearSelection();
                            ensureCaretVisible(rect, *entry);
                            resetCaretBlink();
                            invalidate();
                        }
                    }
                    CloseClipboard();
                }
                return;
            default:
                break;
        }
        return;
    }

    switch (key) {
        case VK_LEFT:
            if (entry->caret > 0) --entry->caret;
            if (!shift) entry->clearSelection();
            break;
        case VK_RIGHT:
            if (entry->caret < static_cast<int>(entry->text.size())) ++entry->caret;
            if (!shift) entry->clearSelection();
            break;
        case VK_HOME:
            entry->caret = 0;
            if (!shift) entry->clearSelection();
            break;
        case VK_END:
            entry->caret = static_cast<int>(entry->text.size());
            if (!shift) entry->clearSelection();
            break;
        case VK_BACK:
            if (!deleteSelection() && entry->caret > 0) {
                entry->text.erase(static_cast<size_t>(entry->caret - 1), 1);
                --entry->caret;
                entry->clearSelection();
            }
            break;
        case VK_DELETE:
            if (!deleteSelection() && entry->caret < static_cast<int>(entry->text.size())) {
                entry->text.erase(static_cast<size_t>(entry->caret), 1);
                entry->clearSelection();
            }
            break;
        default:
            return;
    }
    ensureCaretVisible(rect, *entry);
    resetCaretBlink();
    invalidate();
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
void MainWindow::browseForSource() {
    const std::wstring folder = pickFolder(hwnd_, tr(language_, Key::SourceFolder));
    if (folder.empty()) return;
    sourceDirectory_ = folder;
    sourceEntry_.text = folder;
    sourceEntry_.caret = static_cast<int>(folder.size());
    sourceEntry_.clearSelection();
    sourceEntry_.scroll = 0.0f;
    ensureCaretVisible(layout_.sourceEntry, sourceEntry_);
    scanFiles();
}

void MainWindow::browseForOutput() {
    const std::wstring folder = pickFolder(hwnd_, tr(language_, Key::OutputFolder));
    if (folder.empty()) return;
    outputDirectory_ = folder;
    outputEntry_.text = folder;
    outputEntry_.caret = static_cast<int>(folder.size());
    outputEntry_.clearSelection();
    outputEntry_.scroll = 0.0f;
    ensureCaretVisible(layout_.outputEntry, outputEntry_);
    invalidate();
}

void MainWindow::scanFiles() {
    items_.clear();
    listScroll_ = 0.0f;
    if (!sourceDirectory_.empty()) {
        try {
            std::vector<std::wstring> files;
            collectFiles(sourceDirectory_, supportedExtensions(), files);
            items_.reserve(files.size());
            for (const std::wstring& path : files) {
                ConversionItem item;
                item.sourcePath = path;
                item.relativeName = normalizeSlashes(pathRelative(path, sourceDirectory_));
                item.formatLabel = formatLabelForExtension(pathExtension(path));
                item.sizeBytes = fileSize(path);
                items_.push_back(std::move(item));
            }
            std::sort(items_.begin(), items_.end(),
                      [](const ConversionItem& a, const ConversionItem& b) {
                          return compareNoCase(a.relativeName, b.relativeName) < 0;
                      });
        } catch (const std::exception& error) {
            MessageBoxW(hwnd_, formatScanError(language_, toWide(error.what())).c_str(),
                        tr(language_, Key::Info), MB_OK | MB_ICONINFORMATION);
        }
    }
    updateScrollThumb();
    invalidate();
}

void MainWindow::startConversion() {
    if (items_.empty()) {
        MessageBoxW(hwnd_, tr(language_, Key::NoFiles), tr(language_, Key::Info),
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (outputDirectory_.empty()) {
        MessageBoxW(hwnd_, tr(language_, Key::NoOutput), tr(language_, Key::Info),
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    running_ = true;
    cancel_.store(false);
    progress_ = 0.0f;
    statusText_ = tr(language_, Key::Preparing);
    invalidate();

    ConversionOptions options;
    options.items = items_;
    options.outputDirectory = outputDirectory_;
    options.outputFormat = (outputFormatIndex_ == 1) ? "wav" : "flac";
    options.workerCount = 0;

    if (worker_.joinable()) worker_.join();
    const HWND window = hwnd_;
    worker_ = std::thread([this, options, window]() {
        ConversionSummary summary = runConversion(
            options, &cancel_, [window](size_t completed, size_t total) {
                PostMessageW(window, kMessageProgress,
                             static_cast<WPARAM>(completed), static_cast<LPARAM>(total));
            });
        {
            std::lock_guard<std::mutex> lock(summaryMutex_);
            pendingSummary_ = std::move(summary);
            hasPendingSummary_ = true;
        }
        PostMessageW(window, kMessageFinished, 0, 0);
    });
}

void MainWindow::toggleTheme() {
    if (themeAnimating_) return;
    themeAnimating_ = true;
    themeFadingOut_ = true;
    themeStep_ = 4;
    SetTimer(hwnd_, kTimerTheme, 20, nullptr);
}

void MainWindow::toggleLanguage() {
    language_ = (language_ == Language::Chinese) ? Language::English : Language::Chinese;
    languageAnimationFrame_ = 0;
    rebuildFonts();
    SetTimer(hwnd_, kTimerLanguage, 22, nullptr);
    invalidate();
}

void MainWindow::applyThemeToWindow() {
    applyDarkTitleBar(hwnd_, darkMode_);
}

// ---------------------------------------------------------------------------
// Message handling
// ---------------------------------------------------------------------------
LRESULT MainWindow::handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            updateScale();
            computeLayout();
            SetTimer(hwnd_, kTimerTooltip, 450, nullptr);
            KillTimer(hwnd_, kTimerTooltip);
            return 0;

        case WM_SIZE:
            computeLayout();
            invalidate();
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT paintStruct = {};
            HDC dc = BeginPaint(hwnd_, &paintStruct);
            paint(dc);
            EndPaint(hwnd_, &paintStruct);
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            RECT windowRect = {};
            RECT clientRect = {};
            GetWindowRect(hwnd_, &windowRect);
            GetClientRect(hwnd_, &clientRect);
            const int frameWidth = (windowRect.right - windowRect.left) -
                                   (clientRect.right - clientRect.left);
            const int frameHeight = (windowRect.bottom - windowRect.top) -
                                    (clientRect.bottom - clientRect.top);
            info->ptMinTrackSize.x = static_cast<LONG>(kMinClientWidth * scale_) + frameWidth;
            info->ptMinTrackSize.y = static_cast<LONG>(kMinClientHeight * scale_) + frameHeight;
            return 0;
        }

        case WM_MOUSEMOVE:
            onMouseMove(static_cast<float>(GET_X_LPARAM(lParam)),
                        static_cast<float>(GET_Y_LPARAM(lParam)));
            return 0;

        case WM_MOUSELEAVE:
            trackingLeave_ = false;
            if (hover_ != Id::None) {
                hover_ = Id::None;
                invalidate();
            }
            tooltipVisible_ = false;
            KillTimer(hwnd_, kTimerTooltip);
            return 0;

        case WM_LBUTTONDOWN:
            SetFocus(hwnd_);
            onLeftButtonDown(static_cast<float>(GET_X_LPARAM(lParam)),
                             static_cast<float>(GET_Y_LPARAM(lParam)));
            return 0;

        case WM_LBUTTONUP:
            onLeftButtonUp(static_cast<float>(GET_X_LPARAM(lParam)),
                           static_cast<float>(GET_Y_LPARAM(lParam)));
            return 0;

        case WM_MOUSEWHEEL:
            onMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;

        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT) {
                POINT cursor = {};
                GetCursorPos(&cursor);
                ScreenToClient(hwnd_, &cursor);
                const Id hit = hitTest(static_cast<float>(cursor.x),
                                       static_cast<float>(cursor.y));
                if (hit == Id::SourceEntry || hit == Id::OutputEntry) {
                    SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                    return TRUE;
                }
            }
            break;
        }

        case WM_KEYDOWN:
            onKeyDown(wParam);
            return 0;

        case WM_CHAR:
            onChar(static_cast<wchar_t>(wParam));
            return 0;

        case WM_TIMER:
            switch (wParam) {
                case kTimerCaret:
                    caretVisible_ = !caretVisible_;
                    if (focus_ == Id::SourceEntry) {
                        RECT rect = { static_cast<LONG>(layout_.sourceEntry.x),
                                      static_cast<LONG>(layout_.sourceEntry.y),
                                      static_cast<LONG>(layout_.sourceEntry.right()),
                                      static_cast<LONG>(layout_.sourceEntry.bottom()) };
                        InvalidateRect(hwnd_, &rect, FALSE);
                    } else if (focus_ == Id::OutputEntry) {
                        RECT rect = { static_cast<LONG>(layout_.outputEntry.x),
                                      static_cast<LONG>(layout_.outputEntry.y),
                                      static_cast<LONG>(layout_.outputEntry.right()),
                                      static_cast<LONG>(layout_.outputEntry.bottom()) };
                        InvalidateRect(hwnd_, &rect, FALSE);
                    }
                    return 0;

                case kTimerLanguage:
                    ++languageAnimationFrame_;
                    if (languageAnimationFrame_ >= 10) {
                        languageAnimationFrame_ = -1;
                        KillTimer(hwnd_, kTimerLanguage);
                        computeLayout();
                    }
                    invalidate();
                    return 0;

                case kTimerTheme:
                    if (themeFadingOut_) {
                        --themeStep_;
                        if (themeStep_ <= 0) {
                            themeStep_ = 0;
                            darkMode_ = !darkMode_;
                            applyThemeToWindow();
                            themeFadingOut_ = false;
                        }
                    } else {
                        ++themeStep_;
                        if (themeStep_ >= 4) {
                            themeStep_ = 4;
                            themeAnimating_ = false;
                            KillTimer(hwnd_, kTimerTheme);
                        }
                    }
                    SetLayeredWindowAttributes(
                        hwnd_, 0,
                        static_cast<BYTE>(255 * std::min(1.0f, 0.3f + themeStep_ * 0.175f)),
                        LWA_ALPHA);
                    invalidate();
                    return 0;

                case kTimerTooltip:
                    KillTimer(hwnd_, kTimerTooltip);
                    if (hover_ == Id::ThemeButton && !tooltipVisible_) {
                        tooltipVisible_ = true;
                        invalidate();
                    }
                    return 0;

                default:
                    break;
            }
            break;

        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            updateScale();
            computeLayout();
            invalidate();
            return 0;
        }

        case WM_CLOSE:
            cancel_.store(true);
            if (worker_.joinable()) worker_.join();
            DestroyWindow(hwnd_);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case kMessageProgress: {
            const size_t completed = static_cast<size_t>(wParam);
            const size_t total = static_cast<size_t>(lParam);
            if (total > 0) progress_ = static_cast<float>(completed) / static_cast<float>(total);
            statusText_ = formatProgress(language_, completed, total);
            invalidate();
            return 0;
        }

        case kMessageFinished: {
            if (worker_.joinable()) worker_.join();
            ConversionSummary summary;
            {
                std::lock_guard<std::mutex> lock(summaryMutex_);
                summary = std::move(pendingSummary_);
                hasPendingSummary_ = false;
            }
            running_ = false;
            progress_ = 1.0f;
            statusText_ = tr(language_, Key::Ready);

            std::wstring detail;
            const size_t shown = std::min<size_t>(summary.failures.size(), 15);
            for (size_t i = 0; i < shown; ++i) {
                if (i > 0) detail += L"\n";
                const FailureInfo& failure = summary.failures[i];
                const std::wstring reason =
                    formatError(language_, static_cast<int>(failure.kind), failure.detail);
                detail += failure.name;
                if (!reason.empty()) detail += L"(" + reason + L")";
            }

            invalidate();
            const std::wstring message =
                formatDone(language_, summary.success, summary.failures.size(), detail);
            MessageBoxW(hwnd_, message.c_str(), tr(language_, Key::DoneTitle),
                        MB_OK | MB_ICONINFORMATION);
            invalidate();
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

} // namespace gui
} // namespace ac
