@echo off
echo ===== Rebuilding VOS =====
make clean
if %errorlevel% neq 0 exit /b %errorlevel%

make userspace
if %errorlevel% neq 0 exit /b %errorlevel%

make disk-with-apps
if %errorlevel% neq 0 exit /b %errorlevel%

make iso
if %errorlevel% neq 0 exit /b %errorlevel%

echo ===== Build complete =====
echo.
echo To run: make run
echo Or use: build_and_run.bat
