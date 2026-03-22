#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cmake -S "$SCRIPT_DIR" -B "$SCRIPT_DIR/_build"
cmake --build "$SCRIPT_DIR/_build"

cd "$SCRIPT_DIR/_build/main" && ./main

cd "$SCRIPT_DIR"
