@echo off
rem Builds Wii Party from your own disc. See README.md (or kit\READ ME.txt).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0kit\build-kit.ps1"
pause
