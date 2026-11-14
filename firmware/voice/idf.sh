#!/usr/bin/env bash
# Run idf.py for this project, using whichever ESP-IDF is available.
#
#   ./idf.sh build
#   ./idf.sh -p /dev/ttyACM0 flash monitor
#
# 1. If `idf.py` is already on PATH (normal `export.sh` workflow), use it.
# 2. Otherwise use a local checkout at $IDF_PATH or firmware/esp-idf.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

if command -v idf.py >/dev/null 2>&1; then
    cd "$HERE"
    exec idf.py "$@"
fi

IDF_DIR="${IDF_PATH:-$ROOT/firmware/esp-idf}"
if [ ! -f "$IDF_DIR/export.sh" ]; then
    echo "idf.py not found. Install ESP-IDF v6.0.1 and run its export.sh," >&2
    echo "or place a checkout at $ROOT/firmware/esp-idf." >&2
    exit 1
fi

# Development setup: a standalone Python 3.12 env, because some systems ship a
# Python too new for ESP-IDF's tree_sitter dependency.
if [ -z "${ESP_IDF_PYTHON_ENV_PATH:-}" ] && [ -d "$HOME/.espressif/python_env/idf6.0_py3.12_env" ]; then
    export ESP_IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf6.0_py3.12_env"
    export IDF_PYTHON_ENV_PATH="$ESP_IDF_PYTHON_ENV_PATH"
fi

# shellcheck disable=SC1091
source "$IDF_DIR/export.sh" >/dev/null
cd "$HERE"
exec idf.py "$@"
