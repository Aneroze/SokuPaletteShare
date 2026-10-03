@echo off
REM Build and run the tests (the parts of PaletteShare that don't need the game).
setlocal
call "%~dp0..\build.bat" PaletteShareTests || exit /b 1
"%~dp0..\build\PaletteShareTests.exe"
