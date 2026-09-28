#!/usr/bin/env python3
"""Focused check that launcher cleanup cannot leave a hung Caddy process behind."""
import importlib.util
from pathlib import Path
import subprocess


module_path = Path(__file__).parents[1] / 'start-desktop.py'
spec = importlib.util.spec_from_file_location('uam_start_desktop', module_path)
module = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(module)


class HungProxy:
    def __init__(self):
        self.terminated = False
        self.killed = False
        self.waits = 0

    def terminate(self):
        self.terminated = True

    def kill(self):
        self.killed = True

    def wait(self, timeout=None):
        self.waits += 1
        if timeout is not None:
            raise subprocess.TimeoutExpired('caddy', timeout)


proxy = HungProxy()
module.stop_proxy(proxy)
assert proxy.terminated
assert proxy.killed
assert proxy.waits == 2
print('Caddy cleanup fallback passed')
