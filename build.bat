@echo off
title AudioConverter - Build
setlocal

echo ================================================
echo   AudioConverter -- Build (MinGW-w64 / g++)
echo ================================================
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
if errorlevel 1 (
    echo.
    echo [ERROR] Build failed.
    echo.
    pause
    exit /b 1
)

echo Run the app:  build\AudioConverter.exe
echo Run the tests: build\AudioConverter.exe --selftest
echo.
pause
