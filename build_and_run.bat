@echo off
setlocal enabledelayedexpansion

title VortexOS Build ^& Run

:: ============================================================
:: Настройки
:: ============================================================
set MSYS2=C:\msys64\usr\bin
set MAKE=%MSYS2%\make.exe
set BASH=%MSYS2%\bash.exe
set QEMU=D:\qemu\qemu-system-x86_64.exe
set PROJECT_DIR=D:\VOS

:: Цвета через ANSI (требует Windows 10+)
:: Просто используем echo с метками

echo.
echo ============================================================
echo  VortexOS ^- Build ^& Run Script
echo ============================================================
echo.

cd /d "%PROJECT_DIR%"
if errorlevel 1 (
    echo [ERROR] Cannot cd to %PROJECT_DIR%
    pause
    exit /b 1
)

:: ============================================================
:: 1. Clean
:: ============================================================
echo [1/5] Cleaning build artifacts...
%MAKE% clean >nul 2>&1
if exist build\disk.img del build\disk.img >nul 2>&1
echo       Done.

:: ============================================================
:: 2. Build kernel
:: ============================================================
echo [2/5] Building kernel...
%MAKE% 2>&1
if errorlevel 1 (
    echo.
    echo [ERROR] Kernel build FAILED! See errors above.
    pause
    exit /b 1
)
echo       Done.

:: ============================================================
:: 3. Build userspace
:: ============================================================
echo [3/5] Building userspace...
%MAKE% userspace 2>&1
if errorlevel 1 (
    echo.
    echo [ERROR] Userspace build FAILED! See errors above.
    pause
    exit /b 1
)
echo       Done.

:: ============================================================
:: 4. Create disk image with apps
:: ============================================================
echo [4/5] Creating disk image...
%MAKE% disk-with-apps 2>&1
if errorlevel 1 (
    echo.
    echo [ERROR] Disk creation FAILED! See errors above.
    pause
    exit /b 1
)
echo       Done.

:: ============================================================
:: 5. Run in QEMU
:: ============================================================
echo [5/5] Launching QEMU...
echo.
echo ============================================================
echo  Starting VortexOS in QEMU
echo  Close QEMU window to return to this script
echo ============================================================
echo.

%MAKE% run 2>&1

echo.
echo ============================================================
echo  QEMU exited.
echo ============================================================
pause
