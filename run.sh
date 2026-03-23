#!/usr/bin/env bash
set -e
cmake -S . -B _build
cmake --build _build
cd _build && ./main
