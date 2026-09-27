// main.cpp - entry point. Launches the GUI, or the command line mode when the
// first argument selects it.
#include <windows.h>
#include <gdiplus.h>
#include <objbase.h>

#include "cli.h"
#include "gui/main_window.h"

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

namespace {

// Per-monitor DPI awareness, declared in code rather than in a manifest because
// MinGW's linker cannot merge an embedded manifest with its own default one.
void enableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto setContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
        if (setContext && setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    }

    // Windows 8.1 fallback: PROCESS_PER_MONITOR_DPI_AWARE.
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        using SetProcessDpiAwarenessFn = HRESULT(WINAPI*)(int);
        auto setAwareness = reinterpret_cast<SetProcessDpiAwarenessFn>(
            reinterpret_cast<void*>(GetProcAddress(shcore, "SetProcessDpiAwareness")));
        if (setAwareness && SUCCEEDED(setAwareness(2))) {
            FreeLibrary(shcore);
            return;
        }
        FreeLibrary(shcore);
    }

    SetProcessDPIAware();
}

} // namespace

namespace {

// Startup folders for the GUI, taken from the command line.
struct GuiStartup {
    std::wstring sourceFolder;
    std::wstring outputFolder;
    int formatIndex = -1;   // -1 keeps the default (FLAC)
};

GuiStartup parseGuiStartup(int argumentCount, LPWSTR* arguments) {
    GuiStartup startup;
    for (int i = 1; i < argumentCount; ++i) {
        const std::wstring argument = arguments[i];
        const bool hasValue = (i + 1 < argumentCount);
        if ((argument == L"--source" || argument == L"-s") && hasValue) {
            startup.sourceFolder = arguments[++i];
        } else if ((argument == L"--output" || argument == L"-o") && hasValue) {
            startup.outputFolder = arguments[++i];
        } else if ((argument == L"--format" || argument == L"-f") && hasValue) {
            std::wstring format = arguments[++i];
            for (wchar_t& c : format) c = static_cast<wchar_t>(towlower(c));
            if (format == L"wav") startup.formatIndex = 1;
            else if (format == L"flac") startup.formatIndex = 0;
        }
    }
    return startup;
}

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand) {
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    bool commandLineMode = false;
    if (arguments && argumentCount > 1)
        commandLineMode = ac::isCommandLineCommand(arguments[1]);

    if (commandLineMode) {
        const int exitCode = ac::runCommandLine(argumentCount, arguments);
        LocalFree(arguments);
        return exitCode;
    }

    const GuiStartup guiStartup = parseGuiStartup(argumentCount, arguments);
    if (arguments) LocalFree(arguments);

    enableDpiAwareness();

    // The shell folder picker and Media Foundation both need COM.
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    (void)comResult;

    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr) != Gdiplus::Ok) {
        MessageBoxW(nullptr, L"Failed to initialise GDI+.", L"Audio Converter",
                    MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    int exitCode = 0;
    {
        ac::gui::MainWindow window;
        std::wstring error;
        if (window.create(instance, showCommand, &error)) {
            window.applyStartupOptions(guiStartup.sourceFolder, guiStartup.outputFolder,
                                       guiStartup.formatIndex);
            exitCode = window.runMessageLoop();
        } else {
            const std::wstring message =
                L"Failed to create the application window.\n\n" + error;
            MessageBoxW(nullptr, message.c_str(), L"Audio Converter", MB_OK | MB_ICONERROR);
            exitCode = 1;
        }
    }

    Gdiplus::GdiplusShutdown(gdiplusToken);
    CoUninitialize();
    return exitCode;
}
