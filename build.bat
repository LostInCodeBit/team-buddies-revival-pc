@echo off
rem Builds the Team Buddies native port.
rem   build.bat            recompile the game from the disc image and build everything
rem   build.bat runtime    rebuild only the runtime (reuses the recompiled game code)
rem   build.bat sdl        test build: the SDL3 platform layer and SDL GPU (Vulkan) renderer that Linux runs,
rem                        as build\TeamBuddiesSDL.exe (not shipped; needs the recompiled game code)
rem The disc image is looked up next to this folder ("..\Team Buddies.cue") unless DISC is set.
setlocal enabledelayedexpansion
set ROOT=%~dp0
if "%DISC%"=="" set "DISC=%ROOT%..\Team Buddies.cue"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
if exist "%VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" (echo Visual Studio 2022 Build Tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
where cl >nul 2>nul || (echo cl.exe not found & exit /b 1)

if not exist "%ROOT%build" mkdir "%ROOT%build"
if not exist "%ROOT%build\obj" mkdir "%ROOT%build\obj"
if not exist "%ROOT%build\gen" mkdir "%ROOT%build\gen"
if not exist "%ROOT%generated" mkdir "%ROOT%generated"

set CFLAGS=/nologo /std:c++17 /EHsc /O2 /Z7 /bigobj /utf-8 /W3 /wd4100 /wd4244 /wd4267 /wd4996 /DNDEBUG /I"%ROOT%runtime" /I"%ROOT%generated"

if /i "%1"=="runtime" goto runtime
if /i "%1"=="launcher" goto launcher
if /i "%1"=="sdl" goto sdl

echo === building recompiler
cl %CFLAGS% "%ROOT%recomp\recomp.cpp" /Fo"%ROOT%build\obj\\" /Fe"%ROOT%build\recomp.exe" || exit /b 1

echo === recompiling game code from "%DISC%"
del /q "%ROOT%generated\*.cpp" 2>nul
set SEEDS=
if exist "%ROOT%extra_seeds.txt" set SEEDS=!SEEDS! "%ROOT%extra_seeds.txt"
if exist "%ROOT%build\misses.txt" set SEEDS=!SEEDS! "%ROOT%build\misses.txt"
"%ROOT%build\recomp.exe" "%DISC%" "%ROOT%generated" !SEEDS! || exit /b 1

echo === compiling recompiled code (this takes a while)
del /q "%ROOT%build\gen\*.obj" 2>nul
cl %CFLAGS% /MP /c "%ROOT%generated\*.cpp" /Fo"%ROOT%build\gen\\" || exit /b 1

:runtime
echo === compiling runtime
cl %CFLAGS% /I"%ROOT%third_party\SDL3\include" /MP /c "%ROOT%runtime\*.cpp" /Fo"%ROOT%build\obj\\" || exit /b 1
del /q "%ROOT%build\obj\recomp.obj" 2>nul

rc /nologo /fo "%ROOT%build\game.res" "%ROOT%runtime\game.rc" || exit /b 1

echo === linking
rem /PDBALTPATH: the exe names only its .pdb file, not the folder it was built in
link /nologo /DEBUG /PDBALTPATH:%%_PDB%% /OUT:"%ROOT%build\TeamBuddies.exe" /STACK:33554432 /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /MANIFEST:EMBED "%ROOT%build\obj\*.obj" "%ROOT%build\gen\*.obj" "%ROOT%build\game.res" "%ROOT%third_party\SDL3\lib\x64\SDL3.lib" || exit /b 1
rem only when newer: the launcher or the game may have the old one open, and copying over it fails
xcopy /d /y /q "%ROOT%third_party\SDL3\lib\x64\SDL3.dll" "%ROOT%build\" >nul || exit /b 1
echo === done: %ROOT%build\TeamBuddies.exe

:launcher
echo === building launcher
if not exist "%ROOT%build\launcher" mkdir "%ROOT%build\launcher"
cl %CFLAGS% /I"%ROOT%third_party\SDL3\include" /MP /c "%ROOT%launcher\*.cpp" "%ROOT%runtime\settings.cpp" /Fo"%ROOT%build\launcher\\" || exit /b 1
rc /nologo /fo "%ROOT%build\launcher\launcher.res" "%ROOT%launcher\launcher.rc" || exit /b 1
link /nologo /DEBUG /PDBALTPATH:%%_PDB%% /OUT:"%ROOT%build\TeamBuddiesLauncher.exe" /SUBSYSTEM:WINDOWS /MANIFEST:EMBED "%ROOT%build\launcher\*.obj" "%ROOT%build\launcher\launcher.res" "%ROOT%third_party\SDL3\lib\x64\SDL3.lib" user32.lib gdi32.lib || exit /b 1
xcopy /d /y /q "%ROOT%third_party\SDL3\lib\x64\SDL3.dll" "%ROOT%build\" >nul || exit /b 1
echo === done: %ROOT%build\TeamBuddiesLauncher.exe
exit /b 0

:sdl
echo === compiling runtime (SDL3 platform layer, SDL GPU renderer)
if not exist "%ROOT%build\sdl" mkdir "%ROOT%build\sdl"
del /q "%ROOT%build\sdl\*.obj" 2>nul
set SDLSRC=
for %%f in ("%ROOT%runtime\*.cpp") do (
    set "n=%%~nxf"
    if /i not "!n!"=="platform.cpp" if /i not "!n!"=="gpu_d3d11.cpp" if /i not "!n!"=="netplay_ui.cpp" if /i not "!n!"=="icon_atlas.cpp" set SDLSRC=!SDLSRC! "%%f"
)
for %%f in ("%ROOT%runtime\sdl\*.cpp") do set SDLSRC=!SDLSRC! "%%f"
cl %CFLAGS% /I"%ROOT%third_party\SDL3\include" /MP /c !SDLSRC! /Fo"%ROOT%build\sdl\\" || exit /b 1
rc /nologo /fo "%ROOT%build\game.res" "%ROOT%runtime\game.rc" || exit /b 1
link /nologo /DEBUG /PDBALTPATH:%%_PDB%% /OUT:"%ROOT%build\TeamBuddiesSDL.exe" /STACK:33554432 /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /MANIFEST:EMBED "%ROOT%build\sdl\*.obj" "%ROOT%build\gen\*.obj" "%ROOT%build\game.res" "%ROOT%third_party\SDL3\lib\x64\SDL3.lib" user32.lib || exit /b 1
xcopy /d /y /q "%ROOT%third_party\SDL3\lib\x64\SDL3.dll" "%ROOT%build\" >nul || exit /b 1
echo === done: %ROOT%build\TeamBuddiesSDL.exe
