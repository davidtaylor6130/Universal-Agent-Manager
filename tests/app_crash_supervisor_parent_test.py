"""Exercise supervisor death against a real child with a bound process identity."""
import ctypes
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

binary, method = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix="uam-parent-death-") as directory:
    environment = {**os.environ, "UAM_SUPERVISOR_TEST_ROOT": directory}
    parent = subprocess.Popen([binary, "hold"], env=environment)
    child_pid = None
    child_handle = None
    try:
        pid_file = Path(directory) / "child.pid"
        deadline = time.monotonic() + 10
        while (not pid_file.exists() or not pid_file.read_text().strip()) and time.monotonic() < deadline:
            if parent.poll() is not None:
                raise AssertionError("Supervisor exited before child startup")
            time.sleep(0.02)
        child_pid = int(pid_file.read_text())
        if os.name == "nt":
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
            kernel.OpenProcess.restype = ctypes.c_void_p
            kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
            kernel.CloseHandle.argtypes = [ctypes.c_void_p]
            child_handle = kernel.OpenProcess(0x00100000, False, child_pid)
            assert child_handle, "Failed to bind the child process handle"
        else:
            os.kill(child_pid, 0)
        getattr(parent, method)()
        parent.wait(timeout=5)
        if os.name == "nt":
            assert kernel.WaitForSingleObject(child_handle, 5000) == 0, "GUI child survived supervisor termination"
        else:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                try:
                    os.kill(child_pid, 0)
                except ProcessLookupError:
                    child_pid = None
                    break
                time.sleep(0.02)
            else:
                raise AssertionError("GUI child survived supervisor termination")
        print(f"PASS {method}: supervisor and GUI child exited")
    finally:
        if parent.poll() is None:
            parent.kill()
            parent.wait(timeout=5)
        if child_handle:
            # A bound handle cannot accidentally target a reused PID.
            if kernel.WaitForSingleObject(child_handle, 0) != 0:
                cleanup = kernel.OpenProcess(0x0001, False, child_pid)
                if cleanup:
                    kernel.TerminateProcess.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
                    kernel.TerminateProcess(cleanup, 1)
                    kernel.CloseHandle(cleanup)
            kernel.CloseHandle(child_handle)
        elif child_pid:
            try:
                os.kill(child_pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
