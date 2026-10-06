@echo off
rem Install the built plugins\CheckpointPlugin.dll into BakkesMod's REAL plugins folder.
rem Double-click this in Explorer (or run it from a normal terminal) with Rocket League
rem closed.  Do not run it from inside the Claude desktop app: processes started there see
rem a redirected AppData (%LOCALAPPDATA%\Packages\Claude_*\LocalCache\Roaming), so the dll
rem would land in a shadow folder a normally launched game never loads.
setlocal
set "SRC=%~dp0plugins\CheckpointPlugin.dll"
set "DST=%APPDATA%\bakkesmod\bakkesmod\plugins"
if not exist "%SRC%" (
    echo %SRC% not found - build first with build-windows.bat.
    pause
    exit /b 1
)
tasklist /FI "IMAGENAME eq RocketLeague.exe" | find /I "RocketLeague.exe" >nul && (
    echo Rocket League is running - close it first, then run this again.
    pause
    exit /b 1
)
rem Redirected-AppData check: a probe file written to %APPDATA% must not show up in a package cache.
set "PROBE=.cpt_install_probe_%RANDOM%%RANDOM%"
> "%APPDATA%\%PROBE%" echo probe
for /d %%P in ("%LOCALAPPDATA%\Packages\*") do if exist "%%~P\LocalCache\Roaming\%PROBE%" (
    del "%APPDATA%\%PROBE%" >nul 2>&1
    echo This is running inside a redirected AppData view ^(%%~nxP^) - the dll would go to a
    echo shadow folder.  Double-click Install_Plugin.bat in Explorer instead.
    pause
    exit /b 1
)
del "%APPDATA%\%PROBE%" >nul 2>&1
if not exist "%DST%" mkdir "%DST%"
if exist "%DST%\CheckpointPlugin.dll" copy /Y "%DST%\CheckpointPlugin.dll" "%DST%\CheckpointPlugin.dll.prev" >nul
copy /Y "%SRC%" "%DST%\CheckpointPlugin.dll" >nul || (
    echo Copy failed - is Rocket League or BakkesMod still holding the dll?
    pause
    exit /b 1
)
findstr /I /C:"plugin load checkpointplugin" "%APPDATA%\bakkesmod\bakkesmod\cfg\plugins.cfg" >nul 2>&1 || (
    echo plugin load checkpointplugin>> "%APPDATA%\bakkesmod\bakkesmod\cfg\plugins.cfg"
    echo Added "plugin load checkpointplugin" to cfg\plugins.cfg.
)
for %%F in ("%DST%\CheckpointPlugin.dll") do echo Installed CheckpointPlugin.dll (%%~zF bytes, %%~tF) into %DST%
echo Previous dll kept as CheckpointPlugin.dll.prev.  Launch the game to load it.
pause
