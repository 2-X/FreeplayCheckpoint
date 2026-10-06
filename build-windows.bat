@echo off
rem Build CheckpointPlugin.dll on Windows with Visual Studio 2022 Build Tools (no IDE needed)
rem and install it into BakkesMod (the post-build step runs bakkesmod-patch.exe, which copies the
rem dll to %APPDATA%\bakkesmod\bakkesmod\plugins).  Close Rocket League first - the dll is in use
rem while the game runs.
rem
rem Needs: "Desktop development with C++" workload of VS 2022 Build Tools (MSVC v143 + Windows SDK),
rem and BakkesMod installed (the SDK comes with it; the registry key HKCU\Software\BakkesMod\AppPath
rem points the build at it).  The project file says toolset v142; v143 is what Build Tools 2022 ships,
rem so it is overridden here.
setlocal
set "VSDEV=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VSDEV%" set "VSDEV=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VSDEV%" (
    echo Visual Studio 2022 Build Tools not found. Install them with:
    echo   winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
    exit /b 1
)
call "%VSDEV%" >nul || exit /b 1
cd /d "%~dp0"
msbuild CheckpointPlugin.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /m /nologo /v:minimal
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
rem The pre-build step bumps VERSION_BUILD in version.h on every build; keep the tree clean.
git checkout -q version.h 2>nul
echo.
echo Built plugins\CheckpointPlugin.dll and installed it into BakkesMod.
echo Make sure cfg\plugins.cfg contains:  plugin load checkpointplugin
