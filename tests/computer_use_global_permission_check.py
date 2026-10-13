#!/usr/bin/env python3
"""Exercise the production permission-request branch with counted native OS stubs."""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
helper_source = (ROOT / "src/computer_use/computer_use_mcp_server.cpp").read_text()
start = helper_source.index('else if (settings_action == "request")')
branch = helper_source[helper_source.index("{", start):helper_source.index('else if (settings_action == "open")', start)]
handler_source = (ROOT / "src/cef/chat_config_handlers.cpp").read_text()
start = handler_source.index("void UamQueryHandler::HandleRequestComputerUsePermission(")
handler = handler_source[start:handler_source.index("void UamQueryHandler::HandleSetChatMemoryEnabled", start)]
assert handler.index("!m_app.settings.computer_use_globally_enabled") < handler.index("RunAsyncCefQuery")
assert 'cb->Failure(409, "Computer Use is disabled globally.")' in handler

harness = r'''
#include <nlohmann/json.hpp>
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
struct AppSettings { bool computer_use_globally_enabled = true; };
bool policy_available = true;
bool policy_enabled = true;
int native_requests = 0;
bool LoadLivePolicy(AppSettings& settings)
{
    settings.computer_use_globally_enabled = policy_enabled;
    return policy_available;
}
std::string StringArgument(const std::vector<std::string>& arguments, const std::string&)
{
    return arguments.front();
}
bool RequestCapturePermission(std::string*) { ++native_requests; return true; }
bool RequestActionPermission(std::string*) { ++native_requests; return true; }
int Request(const std::vector<std::string>& arguments)
{
    nlohmann::json result;
    std::string error;
    REQUEST_BRANCH
    return 0;
}
int main()
{
    for (const std::string& permission : {std::string("screenRecording"), std::string("accessibility")})
    {
        policy_available = true; policy_enabled = false; native_requests = 0;
        assert(Request({permission}) == 1 && native_requests == 0);
        policy_available = false; policy_enabled = true;
        assert(Request({permission}) == 1 && native_requests == 0);
        policy_available = true;
        assert(Request({permission}) == 0 && native_requests == 1);
    }
}
'''.replace("REQUEST_BRANCH", branch)
compiler = shutil.which("clang++") or shutil.which("c++")
json_include = next((ROOT / "Builds").glob("*/_deps/nlohmann_json-src/include"), None)
if compiler is None or json_include is None:
    raise SystemExit("C++ compiler or nlohmann include directory unavailable")
with tempfile.TemporaryDirectory(prefix="uam-cu-global-permission-") as directory:
    source = Path(directory) / "check.cpp"
    binary = Path(directory) / "check"
    source.write_text(harness)
    subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(json_include), str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Global permission request gates passed")
