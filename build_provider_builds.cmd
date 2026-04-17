@echo off
REM build_provider_builds.cmd
REM Builds the Gemini CLI release-slice binary.
REM Output: Builds\GeminiCLI\

setlocal enabledelayedexpansion

call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

set "FETCH_DEPS=ON"
set "BUILD_TESTS=OFF"
set "BUILD_DIR=Builds\GeminiCLI"

echo.
echo ==========================================
echo Building: Gemini CLI release slice
echo ==========================================

cmake -S . -B "%BUILD_DIR%" ^
  -DUAM_FETCH_DEPS=%FETCH_DEPS% ^
  -DUAM_BUILD_TESTS=%BUILD_TESTS%

if errorlevel 1 (
    echo ERROR: CMake configure failed.
    exit /b 1
)

cmake --build "%BUILD_DIR%" -j8

if errorlevel 1 (
    echo ERROR: Build failed.
    exit /b 1
)

echo.
echo ==========================================
echo Gemini CLI build complete.
echo ==========================================
echo.
echo Output directory:
echo %BUILD_DIR%
echo.
