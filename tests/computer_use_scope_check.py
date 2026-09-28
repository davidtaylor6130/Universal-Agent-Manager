#!/usr/bin/env python3
"""Compile and exercise the production computer-use application-scope checks."""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/computer_use/computer_use_mcp_server.cpp"
JSON_INCLUDE = next((ROOT / "Builds").glob("*/_deps/nlohmann_json-src/include"), None)


def extract(source: str, start_marker: str, end_marker: str) -> str:
    start = source.index(start_marker)
    end = source.index(end_marker, start)
    return source[start:end].rstrip()


HARNESS = r'''#include <nlohmann/json.hpp>

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace uam::computer_use {
struct ApplicationIdentity { std::string id; std::string title; };
}

namespace test_state {
std::string task;
std::string control;
}

namespace uam::io {
bool TryReadTextFile(const std::filesystem::path& path, std::string& output, std::size_t)
{
    const std::string name = path.filename().string();
    if (name == "task.json") output = test_state::task;
    else if (name == "control.json") output = test_state::control;
    else return false;
    return !output.empty();
}
}

namespace uam::strings {
std::string Trim(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\n\r");
    return std::string(value.substr(first, last - first + 1));
}

std::string ToLowerAscii(std::string_view value)
{
    std::string result(value);
    for (char& character : result)
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    return result;
}
}

namespace uam::computer_use {
std::optional<ApplicationIdentity> ResolveApplication(std::string_view query, std::string* = nullptr)
{
    if (query == "Safari") return ApplicationIdentity{"com.apple.Safari", "Safari"};
    if (query == "Firefox") return ApplicationIdentity{"org.mozilla.firefox", "Firefox"};
    return std::nullopt;
}

std::string JsonString(const nlohmann::json& value, std::string_view field, std::string fallback = {})
{
    const auto found = value.find(field);
    return found != value.end() && found->is_string() ? found->get<std::string>() : std::move(fallback);
}

APP_NAMED_METHOD

class Harness {
public:
    std::optional<std::string> CheckWindow(std::string task_id, std::uint64_t id, std::uint64_t pid) const
    {
        struct Window { std::uint64_t id; std::uint64_t process_id; } window{id, pid};
        const Window* requested = &window;
WINDOW_GUARD
        return std::nullopt;
    }

    std::optional<std::string> Check(const ApplicationIdentity& application, const std::string& query = {}) const
    {
        return ValidateApplicationScope(application, query);
    }

private:
    static constexpr std::size_t kMaxControlBytes = 4096;
    std::filesystem::path m_sessionDirectory = "/scope-check";
    std::filesystem::path m_controlFile = m_sessionDirectory / "control.json";

SCOPE_METHOD
};

bool Check(bool condition, const char* message)
{
    if (!condition) std::cerr << message << "\n";
    return condition;
}
}

int main()
{
    using uam::computer_use::ApplicationIdentity;
    using uam::computer_use::Harness;
    bool ok = true;
    const ApplicationIdentity firefox{"org.mozilla.firefox", "Firefox"};
    const ApplicationIdentity uam{"com.universal-agent-manager", "Universal Agent Manager"};
    const ApplicationIdentity safari{"com.apple.Safari", "Safari"};

    test_state::task.clear();
    test_state::control.clear();
    ok &= uam::computer_use::Check(Harness{}.Check(firefox).has_value(), "missing task must deny");

    test_state::task = "not json";
    ok &= uam::computer_use::Check(Harness{}.Check(firefox).has_value(), "malformed task must deny");

    test_state::task = R"({"id":"task-1","prompt":"Open Firefox"})";
    test_state::control.clear();
    ok &= uam::computer_use::Check(Harness{}.Check(firefox) == std::nullopt, "named Firefox must permit");
    ok &= uam::computer_use::Check(Harness{}.Check(uam).has_value(), "unnamed UAM app must deny");

    test_state::control = R"({"applicationId":"org.mozilla.firefox","applicationTitle":"Firefox","taskId":"task-1"})";
    ok &= uam::computer_use::Check(Harness{}.Check(safari).has_value(), "same-task Safari must deny");

    test_state::task = R"({"id":"task-2","prompt":"Open Safari"})";
    ok &= uam::computer_use::Check(Harness{}.Check(safari) == std::nullopt, "new task naming Safari must permit");

    test_state::task = R"({"id":"task-1","prompt":"Continue the work"})";
    ok &= uam::computer_use::Check(Harness{}.Check(firefox) == std::nullopt, "same-task Firefox continuation must permit");
    test_state::control = R"({"applicationId":"org.mozilla.firefox","taskId":"task-1","targetId":"42","targetProcessId":"7"})";
    ok &= uam::computer_use::Check(!Harness{}.CheckWindow("task-1", 42, 7), "same window must permit");
    ok &= uam::computer_use::Check(Harness{}.CheckWindow("task-1", 43, 7).has_value(), "same-task window switch must deny");
    ok &= uam::computer_use::Check(Harness{}.CheckWindow("task-1", 42, 8).has_value(), "changed owner must deny");
    ok &= uam::computer_use::Check(!Harness{}.CheckWindow("task-2", 43, 7), "new task can request another window");
    return ok ? 0 : 1;
}
'''


def main() -> int:
    compiler = shutil.which("clang++") or shutil.which("c++")
    if compiler is None or JSON_INCLUDE is None:
        print("compiler or nlohmann/json.hpp unavailable", file=sys.stderr)
        return 2
    source = SOURCE.read_text()
    scope_method = extract(
        source,
        "\t\t\tstd::optional<std::string> ReadTask(nlohmann::json& task) const",
        "\t\t\tstd::optional<Target> ResolveRequestedTarget",
    )
    app_named_method = extract(
        source,
        "\tbool ApplicationNamedInTask(std::string_view prompt, std::string_view application)",
        "\n\tstd::optional<std::string> ConvertPointerCoordinates",
    )
    window_guard = extract(source, "\t\t\t\tstd::string previous_control_text;", "\t\t\t\tconst ApplicationIdentity application = ApplicationIdentityForTarget(requested->kind")
    harness = HARNESS.replace("SCOPE_METHOD", scope_method).replace("APP_NAMED_METHOD", app_named_method).replace("WINDOW_GUARD", window_guard)
    with tempfile.TemporaryDirectory(prefix="uam-scope-") as directory:
        source_path = pathlib.Path(directory) / "check.cpp"
        binary_path = pathlib.Path(directory) / "check"
        source_path.write_text(harness)
        result = subprocess.run(
            [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(JSON_INCLUDE), str(source_path), "-o", str(binary_path)],
            capture_output=True,
            text=True,
        )
        if result.returncode:
            print(result.stderr, file=sys.stderr, end="")
            return result.returncode
        result = subprocess.run([str(binary_path)], capture_output=True, text=True)
        print(result.stdout, end="")
        print(result.stderr, file=sys.stderr, end="")
        return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
