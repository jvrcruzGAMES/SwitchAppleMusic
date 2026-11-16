#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
python_bin="${PYTHON:-.venv/bin/python}"
"$python_bin" -m unittest discover -s tests -p 'test_*.py'
"$python_bin" -m pytest backend/tests -q
test_binary="$(mktemp /tmp/switchmusic-protocol.XXXXXX)"
trap 'rm -f "$test_binary"' EXIT
c++ -std=c++17 -Icommon/include -Ithird_party/json common/src/client.cpp tests/protocol.cpp \
    -o "$test_binary" $(pkg-config --cflags --libs libcurl) -pthread
"$test_binary"
