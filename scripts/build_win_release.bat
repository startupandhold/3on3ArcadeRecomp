@echo off
rem Configure (if needed) and build the optimized Windows Release exe
rem (-O3, ThinLTO, x86-64-v3; no debug runtime or Tracy).
setlocal
cd /d "%~dp0.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
if not exist out\build\win-amd64-release\build.ninja (
  cmake --preset win-amd64-release -DCMAKE_PREFIX_PATH=%CD%\sdk\win-amd64\win-amd64 || exit /b 1
)
cmake --build out\build\win-amd64-release
