#!/bin/sh

set -eu

# CMake requires an executable path; Zig exposes the archiver as a subcommand.
exec "${UAM_ZIG:-zig}" ar "$@"
