@echo off
title LILYGO T-SIM7000G Live Serial Monitor (COM7)
color 0A
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scratch\serial_monitor.ps1"
pause
