@echo off
title Phone Link headset bridge
cd /d "%~dp0bridge\bin"
headset_bridge.exe %*
pause
