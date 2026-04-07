#!/bin/bash
# build_provider_builds.sh
# Builds separate UAM binaries, each with only one provider family enabled.
# Output: Builds/ProviderFlags/<provider_name>/

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

BUILD_BASE="Builds/ProviderFlags"
FETCH_DEPS="ON"
FETCH_LLAMA="OFF"
BUILD_TESTS="OFF"

# Resolve local dep paths from existing Builds/_deps if available
LLAMA_SRC=""
if [ -d "Builds/_deps/llama_cpp_uam-src" ]; then
    LLAMA_SRC="$(pwd)/Builds/_deps/llama_cpp_uam-src"
fi

build_config() {
    local name="$1"
    local gemini_structured="$2"
    local gemini_cli="$3"
    local codex_cli="$4"
    local claude_cli="$5"
    local opencode_cli="$6"
    local opencode_local="$7"
    local ollama_engine="$8"

    local out_dir="${BUILD_BASE}/${name}"

    echo ""
    echo "=========================================="
    echo "Building: ${name}"
    echo "  Output: ${out_dir}"
    echo "=========================================="

    local cmake_args=(
        -S .
        -B "${out_dir}"
        -DUAM_FETCH_DEPS="${FETCH_DEPS}"
        -DUAM_FETCH_LLAMA_CPP="${FETCH_LLAMA}"
        -DUAM_BUILD_TESTS="${BUILD_TESTS}"
        -DUAM_ENABLE_RUNTIME_GEMINI_STRUCTURED="${gemini_structured}"
        -DUAM_ENABLE_RUNTIME_GEMINI_CLI="${gemini_cli}"
        -DUAM_ENABLE_RUNTIME_CODEX_CLI="${codex_cli}"
        -DUAM_ENABLE_RUNTIME_CLAUDE_CLI="${claude_cli}"
        -DUAM_ENABLE_RUNTIME_OPENCODE_CLI="${opencode_cli}"
        -DUAM_ENABLE_RUNTIME_OPENCODE_LOCAL="${opencode_local}"
        -DUAM_ENABLE_RUNTIME_OLLAMA_ENGINE="${ollama_engine}"
    )

    if [ -n "${LLAMA_SRC}" ]; then
        cmake_args+=(-DUAM_LLAMA_CPP_SOURCE_DIR="${LLAMA_SRC}")
    fi

    cmake "${cmake_args[@]}"
    cmake --build "${out_dir}" -j8

    echo "  Done: ${name}"
}

# Gemini (both structured + CLI)
build_config "Gemini" ON ON OFF OFF OFF OFF OFF

# Claude Code
build_config "Claude" OFF OFF OFF ON OFF OFF OFF

# Codex
build_config "Codex" OFF OFF ON OFF OFF OFF OFF

# OpenCode (CLI + Local, requires ollama-engine)
build_config "OpenCode" OFF OFF OFF OFF ON ON ON

# Local (ollama-engine only, no CLI providers)
build_config "Local" OFF OFF OFF OFF OFF OFF ON

echo ""
echo "=========================================="
echo "All provider builds complete."
echo "=========================================="
echo ""
echo "Output directories:"
ls -d "${BUILD_BASE}"/*/ 2>/dev/null || echo "  (none)"
echo ""
