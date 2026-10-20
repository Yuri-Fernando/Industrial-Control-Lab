#!/usr/bin/env bash
# Linux/WSL shortcut; extra flags pass through, e.g. ./scripts/run_local.sh --io remote --engine st
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 scripts/lab.py run "$@"
