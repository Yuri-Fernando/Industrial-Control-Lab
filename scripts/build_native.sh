#!/usr/bin/env bash
# Plain-GCC build (no PlatformIO): controller + remote I/O node into build/.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
CXXFLAGS=(-std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread -Ilib/control -Ilib/modbus -Ilib/net -Ilib/io -Ilib/stvm -Ilib/timing)
LIBS=()
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) LIBS=(-lws2_32 -lwinmm -static) ;; esac
# e.g. EXTRA_LDFLAGS="-static-libstdc++ -static-libgcc" when the runtime image has an older libstdc++
read -r -a EXTRA <<< "${EXTRA_LDFLAGS:-}"
LIBS+=("${EXTRA[@]}")
COMMON=(lib/control/control.cpp lib/modbus/modbus.cpp lib/net/net.cpp lib/io/io.cpp)
g++ "${CXXFLAGS[@]}" src/main.cpp "${COMMON[@]}" lib/stvm/stvm.cpp lib/stvm/st_controller.cpp \
  lib/timing/cycle_stats.cpp -o build/industrial_controller "${LIBS[@]}"
g++ "${CXXFLAGS[@]}" src/io_node/main.cpp "${COMMON[@]}" -o build/io_node "${LIBS[@]}"
printf '%s\n' 'Built build/industrial_controller and build/io_node'
