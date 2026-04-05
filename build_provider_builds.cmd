@echo off
REM build_provider_builds.cmd
REM Builds separate UAM binaries, each with only one provider family enabled.
REM Output: Builds\ProviderFlags\<provider_name>\

setlocal enabledelayedexpansion

call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

set "FETCH_DEPS=ON"
set "FETCH_LLAMA=OFF"
set "BUILD_TESTS=OFF"
set "BUILD_BASE=Builds\ProviderFlags"

goto :build_all

:do_build
REM %1=name %2=gemini-structured %3=gemini-cli %4=codex-cli %5=claude-cli %6=opencode-cli %7=opencode-local %8=ollama-engine
echo.
echo ==========================================
echo Building: %~1
echo ==========================================

cmake -S . -B "%BUILD_BASE%\%~1" ^
  -DUAM_FETCH_DEPS=%FETCH_DEPS% ^
  -DUAM_FETCH_LLAMA_CPP=%FETCH_LLAMA% ^
  -DUAM_BUILD_TESTS=%BUILD_TESTS% ^
  -DUAM_ENABLE_RUNTIME_GEMINI_STRUCTURED=%~2 ^
  -DUAM_ENABLE_RUNTIME_GEMINI_CLI=%~3 ^
  -DUAM_ENABLE_RUNTIME_CODEX_CLI=%~4 ^
  -DUAM_ENABLE_RUNTIME_CLAUDE_CLI=%~5 ^
  -DUAM_ENABLE_RUNTIME_OPENCODE_CLI=%~6 ^
  -DUAM_ENABLE_RUNTIME_OPENCODE_LOCAL=%~7 ^
  -DUAM_ENABLE_RUNTIME_OLLAMA_ENGINE=%~8

if errorlevel 1 (
    echo ERROR: CMake configure failed for %~1
    exit /b 1
)

cmake --build "%BUILD_BASE%\%~1" -j8

if errorlevel 1 (
    echo ERROR: Build failed for %~1
    exit /b 1
)

echo Done: %~1
goto :eof

:build_all
call :do_build Gemini       ON  ON  OFF OFF OFF OFF OFF
call :do_build Claude       OFF OFF OFF ON  OFF OFF OFF
call :do_build Codex        OFF OFF ON  OFF OFF OFF OFF
call :do_build OpenCode     OFF OFF OFF OFF ON  ON  ON
call :do_build Local        OFF OFF OFF OFF OFF OFF ON

echo.
echo ==========================================
echo All provider builds complete.
echo ==========================================
echo.
echo Output directories:
dir /b "%BUILD_BASE%" 2>nul
echo.
