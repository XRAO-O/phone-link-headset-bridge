@echo off
rem Builds bridge\bin\headset_bridge.exe and headset_bridge_gui.exe with MSYS2 MinGW64 (see README).
setlocal
if "%MSYS2_ROOT%"=="" set MSYS2_ROOT=C:\msys64
set PATH=%MSYS2_ROOT%\mingw64\bin;%PATH%
cd /d "%~dp0"
if not exist btstack\src\btstack.h git submodule update --init || goto :error
cmake -S bridge -B bridge\build -G Ninja || goto :error
cmake --build bridge\build || goto :error
echo.
echo Built into %~dp0bridge\bin
exit /b 0
:error
echo Build failed.
exit /b 1
