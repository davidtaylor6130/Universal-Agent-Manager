# Isolated bootstrap probe regressions

This small target compiles the actual `src/remote/runner_bootstrap.cpp` and the same cases included by `remote_runner_tests.cpp`. It links an injected process service and a platform factory that throws on unexpected use. No OS process service, provider, helper or desktop application is linked or launched. All six provider build definitions stay enabled for the shared types.

From the repository root, use the allocated shared validation lease when required:

```sh
cmake -S tests/bootstrap_probe -B Builds/task-1091 -DCMAKE_BUILD_TYPE=Release -DUAM_PACKAGE_REMOTE_RUNNERS=OFF
cmake --build Builds/task-1091 --target uam_bootstrap_probe_tests --parallel 2
python3 scripts/run_native_tests.py Builds/task-1091/uam_bootstrap_probe_tests --jobs 1
```

The inventory covers typed failures in either probe, diagnostic privacy, Linux/Windows fallback and CPU normalization, malformed versus unsupported output, cancellation fences, injected process-service outcomes, canceled plans and unavailable working directories. The last control applies to macOS/Linux. These tests do not establish full-core, desktop/helper delivery, live SSH/provider or Windows runtime acceptance.

The missing-working-directory control creates a uniquely named directory owned by that invocation and restores its process directory on exit, so leftovers from an interrupted earlier run do not block it.
