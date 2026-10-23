#!/bin/sh

set -eu

# Keep archive indexing in the same target-aware toolchain as compilation.
exec "${UAM_ZIG:-zig}" ranlib "$@"
