@echo off
title AudioConverter - Build EXE

echo ================================================
echo   AudioConverter -- Build Standalone EXE
echo ================================================
echo.

:: -----------------------------------------------------------------
:: Check Python
:: -----------------------------------------------------------------
echo [1/2] Checking Python...
echo.

where python >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Python not found. Please install Python 3.10+
    echo         https://www.python.org/downloads/
    echo.
    pause
    exit /b 1
)

python --version 2>&1
echo.

:: -----------------------------------------------------------------
:: Install dependencies
:: -----------------------------------------------------------------
echo [2/2] Installing dependencies and building...
echo.

python -m pip install -r requirements.txt --upgrade --disable-pip-version-check -i https://pypi.tuna.tsinghua.edu.cn/simple
if errorlevel 1 (
    echo   Tsinghua mirror failed, trying PyPI...
    python -m pip install -r requirements.txt --upgrade --disable-pip-version-check
    if errorlevel 1 (
        echo   [ERROR] Failed to install dependencies.
        echo.
        pause
        exit /b 1
    )
)
echo.

:: -----------------------------------------------------------------
:: Build
:: -----------------------------------------------------------------
echo Building EXE...
echo.

python -m PyInstaller ^
    --noconfirm ^
    --onefile ^
    --windowed ^
    --name "AudioConverter" ^
    --clean ^
    --strip ^
    --optimize 2 ^
    --add-data "ncm_decryptor.py;." ^
    --collect-data customtkinter ^
    --hidden-import miniaudio ^
    --hidden-import soundfile ^
    --hidden-import numpy ^
    --hidden-import Crypto ^
    --hidden-import Crypto.Cipher ^
    --hidden-import Crypto.Cipher.AES ^
    --hidden-import mutagen ^
    audio_converter.py

if errorlevel 1 (
    echo.
    echo [ERROR] Build failed.
    echo.
    pause
    exit /b 1
)

:: -----------------------------------------------------------------
:: Done
:: -----------------------------------------------------------------
echo.
if exist "dist\AudioConverter.exe" (
    echo ================================================
    echo   BUILD COMPLETE
    echo   Output: dist\AudioConverter.exe
    echo ================================================
) else (
    echo ================================================
    echo   BUILD COMPLETE
    echo   Check dist\ folder for the EXE.
    echo ================================================
)
echo.
pause