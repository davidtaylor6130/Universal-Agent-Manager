#!/bin/bash
# build_select_provider.sh
# Interactive script to select which provider(s) to enable, then build.
# Output: Builds/ProviderFlags/<config_name>/

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

FETCH_DEPS="ON"
FETCH_LLAMA="OFF"
BUILD_TESTS="OFF"

LLAMA_SRC=""
if [ -d "Builds/_deps/llama_cpp_uam-src" ]; then
    LLAMA_SRC="$(pwd)/Builds/_deps/llama_cpp_uam-src"
fi

echo "============================================"
echo "  UAM Provider Build Selector"
echo "============================================"
echo ""
echo "Select which providers to enable (y/n):"
echo ""

read -rp "  Gemini (Structured)  [y]: " v; GEMINI_S="${v:-y}"
read -rp "  Gemini (CLI)         [y]: " v; GEMINI_C="${v:-y}"
read -rp "  Claude Code          [y]: " v; CLAUDE="${v:-y}"
read -rp "  Codex CLI            [y]: " v; CODEX="${v:-y}"
read -rp "  OpenCode CLI         [y]: " v; OPENCODE_C="${v:-y}"
read -rp "  OpenCode Local       [y]: " v; OPENCODE_L="${v:-y}"
read -rp "  Ollama Engine        [y]: " v; OLLAMA="${v:-y}"

echo ""

# Convert y/n to ON/OFF
to_bool() { [[ "${1,,}" == "y"* ]] && echo "ON" || echo "OFF"; }

G_S=$(to_bool "$GEMINI_S")
G_C=$(to_bool "$GEMINI_C")
CL=$(to_bool "$CLAUDE")
CX=$(to_bool "$CODEX")
OC=$(to_bool "$OPENCODE_C")
OL=$(to_bool "$OPENCODE_L")
OE=$(to_bool "$OLLAMA")

# Dependency enforcement: opencode-local requires opencode-cli + ollama-engine
if [ "$OL" = "ON" ]; then
    if [ "$OC" != "ON" ]; then
        echo "  OpenCode Local requires OpenCode CLI — enabling it."
        OC="ON"
    fi
    if [ "$OE" != "ON" ]; then
        echo "  OpenCode Local requires Ollama Engine — enabling it."
        OE="ON"
    fi
fi

# Validate at least one provider
count=0
for v in "$G_S" "$G_C" "$CL" "$CX" "$OC" "$OL" "$OE"; do
    [ "$v" = "ON" ] && ((count++)) || true
done

if [ "$count" -eq 0 ]; then
    echo "  ERROR: At least one provider must be enabled."
    exit 1
fi

# Build config name from enabled providers
name_parts=()
[ "$G_S" = "ON" ] && name_parts+=("GeminiS")
[ "$G_C" = "ON" ] && name_parts+=("GeminiC")
[ "$CL" = "ON" ] && name_parts+=("Claude")
[ "$CX" = "ON" ] && name_parts+=("Codex")
[ "$OC" = "ON" ] && name_parts+=("OpenCodeCLI")
[ "$OL" = "ON" ] && name_parts+=("OpenCodeLocal")
[ "$OE" = "ON" ] && name_parts+=("OllamaEngine")

config_name=$(IFS="+"; echo "${name_parts[*]}")
out_dir="Builds/ProviderFlags/${config_name}"

echo ""
echo "============================================"
echo "  Configuration: ${config_name}"
echo "  Output:        ${out_dir}"
echo "============================================"
echo ""
echo "  Gemini (Structured): ${G_S}"
echo "  Gemini (CLI):        ${G_C}"
echo "  Claude Code:         ${CL}"
echo "  Codex CLI:           ${CX}"
echo "  OpenCode CLI:        ${OC}"
echo "  OpenCode Local:      ${OL}"
echo "  Ollama Engine:       ${OE}"
echo ""

read -rp "  Proceed with build? [y]: " confirm
confirm="${confirm:-y}"
if [[ "${confirm,,}" != "y"* ]]; then
    echo "  Cancelled."
    exit 0
fi

echo ""
echo "  Configuring..."
echo ""

cmake_args=(
    -S .
    -B "${out_dir}"
    -DUAM_FETCH_DEPS="${FETCH_DEPS}"
    -DUAM_FETCH_LLAMA_CPP="${FETCH_LLAMA}"
    -DUAM_BUILD_TESTS="${BUILD_TESTS}"
    -DUAM_ENABLE_RUNTIME_GEMINI_STRUCTURED="${G_S}"
    -DUAM_ENABLE_RUNTIME_GEMINI_CLI="${G_C}"
    -DUAM_ENABLE_RUNTIME_CODEX_CLI="${CX}"
    -DUAM_ENABLE_RUNTIME_CLAUDE_CLI="${CL}"
    -DUAM_ENABLE_RUNTIME_OPENCODE_CLI="${OC}"
    -DUAM_ENABLE_RUNTIME_OPENCODE_LOCAL="${OL}"
    -DUAM_ENABLE_RUNTIME_OLLAMA_ENGINE="${OE}"
)

if [ -n "${LLAMA_SRC}" ]; then
    cmake_args+=(-DUAM_LLAMA_CPP_SOURCE_DIR="${LLAMA_SRC}")
fi

cmake "${cmake_args[@]}"

echo ""
echo "  Building..."
echo ""

cmake --build "${out_dir}" -j8

echo ""
echo "============================================"
echo "  Build complete: ${config_name}"
echo "  Bundle: ${out_dir}/universal_agent_manager.app"
echo "  Binary: ${out_dir}/universal_agent_manager.app/Contents/MacOS/universal_agent_manager"
echo "  Launch: open ${out_dir}/universal_agent_manager.app"
echo "============================================"
echo ""
