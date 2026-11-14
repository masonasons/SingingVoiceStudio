@echo off
rem Build everything from scratch with scripts\build_all.ps1.
rem   build.bat [-NoZip]
setlocal
cd /d "%~dp0"
set "MSYS=C:\msys64"
rem English messages from MSYS2 tools.
set LANG=C
set LC_ALL=C
if not exist "%MSYS%\usr\bin\bash.exe" (
    echo MSYS2 not found in %MSYS%.
    exit /b 1
)

rem Remove earlier build output.
for %%d in ("build" "build-tests" "dist" "voices\dectalk" "third_party\dectalk\src\dapi\build") do (
    if exist %%d rmdir /s /q %%d
    if exist %%d (
        echo Could not remove %%~d.
        exit /b 1
    )
)

rem Install missing MSYS2 packages; Python 3 is for Eloquence's build.
set "LOG=%TEMP%\svs-pacman.log"
"%MSYS%\usr\bin\bash.exe" -lc "pacman -S --needed --noconfirm mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-python git" >"%LOG%" 2>&1
if errorlevel 1 (
    type "%LOG%"
    echo Package install failed.
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_all.ps1 -Msys "%MSYS%" %*
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)
echo Done: dist\SingingVoiceStudio
