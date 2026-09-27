// cli.h - headless command line mode.
#pragma once

namespace ac {

// Returns true when the first argument selects command line mode.
bool isCommandLineCommand(const wchar_t* argument);

// Runs the command line mode. Returns the process exit code.
int runCommandLine(int argumentCount, wchar_t** arguments);

} // namespace ac
