@echo off
REM Build PaletteShare.dll (32-bit, Release) with MSVC + CMake + Ninja.
REM Needs Visual Studio with "Desktop development with C++" (x86 tools + Windows SDK).
REM First clone? Run: git submodule update --init
setlocal

where cl >nul 2>nul && goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [!] Visual Studio not found. Open an "x86 Native Tools Command Prompt for VS" and run this again.
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL ( echo [!] VS C++ tools not found. & exit /b 1 )
call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1

:build
set ROOT=%~dp0
if not exist "%ROOT%thirdparty\SokuLib\CMakeLists.txt" (
  echo [!] SokuLib submodule missing. Run: git submodule update --init
  exit /b 1
)
cmake -S "%ROOT%." -B "%ROOT%build" -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
REM Optional argument: another target (e.g. PaletteShareTests).
set TARGET=%~1
if "%TARGET%"=="" set TARGET=PaletteShare
cmake --build "%ROOT%build" --target %TARGET% || exit /b 1
if "%TARGET%"=="PaletteShare" echo built: %ROOT%build\PaletteShare.dll
