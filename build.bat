@echo off
title Build Switchy

REM Find vcvars64.bat - check VS 2026, 2022 in both Program Files locations, all editions
set "VCVARS="
for %%V in (18 2022) do (
  for %%D in ("C:\Program Files\Microsoft Visual Studio\%%V" "C:\Program Files (x86)\Microsoft Visual Studio\%%V") do (
    for %%A in (Community Professional Enterprise BuildTools) do (
      if exist "%%~D\%%A\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS=%%~D\%%A\VC\Auxiliary\Build\vcvars64.bat"
      )
    )
  )
)

if not defined VCVARS (
  echo [ERROR] Visual Studio 2022 or 2026 not found
  echo Install from: https://visualstudio.microsoft.com/downloads/
  echo In the installer check: "Desktop development with C++"
  pause
  exit /b 1
)

echo Setting up environment...
call "%VCVARS%" >nul 2>&1
cd /d "%~dp0"

echo Building Switchy (Release x64)...
MSBuild.exe Switchy.sln /p:Configuration=Release /p:Platform=x64 /v:minimal
if errorlevel 1 (
  echo.
  echo [ERROR] Build failed
  pause
  exit /b 1
)

echo.
echo ===== Build successful =====
echo Output: %~dp0bin\Switchy.exe
echo.
pause
