@echo off
REM Reusable MSVC build environment wrapper for the Demon's Souls port.
REM Adds LLVM, standalone CMake and Ninja (NINJA_DIR, if set; else Ninja must be
REM on PATH) to PATH, loads the VS x64 dev env (VCVARS overrides where
REM vcvars64.bat is), then runs whatever command was passed as arguments.
REM Usage:  cmd /c "scripts\vsenv.bat <command...>"
set "PATH=C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin;%NINJA_DIR%;%PATH%"
if not defined VCVARS set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
  echo [vsenv] ERROR: failed to load vcvars64.bat
  exit /b 1
)
%*
