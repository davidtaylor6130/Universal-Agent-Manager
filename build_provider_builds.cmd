@echo off
REM build_provider_builds.cmd
REM Builds the Gemini-only Windows binary.
REM Output: Builds\GeminiCLI\

setlocal enabledelayedexpansion

call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

set "BUILD_TESTS=OFF"
set "BUILD_DIR=Builds\GeminiCLI"

echo.
echo ==========================================
echo Building: Gemini-only Windows app
echo ==========================================

npm --prefix UI-V2 ci
if errorlevel 1 (
    echo ERROR: UI-V2 dependency install failed.
    exit /b 1
)

cmake -S . -B "%BUILD_DIR%" ^
  -DUAM_BUILD_TESTS=%BUILD_TESTS% ^
  -DUAM_FETCHCONTENT_BASE_DIR=Builds\_deps ^
  -DUAM_ENABLE_RUNTIME_GEMINI_CLI=ON ^
  -DUAM_ENABLE_RUNTIME_CODEX_CLI=OFF ^
  -DUAM_ENABLE_RUNTIME_CLAUDE_CLI=OFF

if errorlevel 1 (
    echo ERROR: CMake configure failed.
    exit /b 1
)

cmake --build "%BUILD_DIR%" --config Release -j8

if errorlevel 1 (
    echo ERROR: Build failed.
    exit /b 1
)

echo.
echo ==========================================
echo Gemini-only Windows build complete.
echo ==========================================
echo.
echo Output directory:
echo %BUILD_DIR%
echo.
