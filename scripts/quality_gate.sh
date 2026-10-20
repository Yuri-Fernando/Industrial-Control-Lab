#!/usr/bin/env bash
# All gates: plain GCC build, PlatformIO Unity tests (when pio is installed) and real-TCP integration tests.
set -euo pipefail
cd "$(dirname "$0")/.."
./scripts/build_native.sh
exec python3 scripts/lab.py test
