@echo off
rem One-click launcher: sync the Fysetc AltAz mount clock/site, then start Stellarium.
rem Edit the --lat/--lon values below to match YOUR observing site (east/north positive).
cd /d "%~dp0.."
python tools\start_stellarium.py --lat 39.9042 --lon 116.4074 %*
if errorlevel 1 pause
