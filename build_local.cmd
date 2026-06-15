@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
npm --prefix UI-V2 ci
if errorlevel 1 exit /b 1
cmake -S . -B Builds\windows-local -DUAM_BUILD_TESTS=OFF
if errorlevel 1 exit /b 1
cmake --build Builds\windows-local --config Release
