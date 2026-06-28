@echo off
REM Reusable MSVC build environment wrapper for the Demon's Souls port.
REM Adds standalone CMake + winget Ninja to PATH, loads the VS x64 dev env,
REM then runs whatever command was passed as arguments.
REM Usage:  cmd /c "scripts\vsenv.bat <command...>"
set "PATH=C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin;C:\Users\jonat\AppData\Local\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe;%PATH%"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [vsenv] ERROR: failed to load vcvars64.bat
  exit /b 1
)
%*
