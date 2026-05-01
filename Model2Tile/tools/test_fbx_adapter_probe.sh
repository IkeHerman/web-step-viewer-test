#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${ROOT_DIR}"
mkdir -p build/tools

c++ -std=c++17 -O2 -Wall -Wextra \
  tools/fbx_adapter_probe.cpp \
  adapters/fbx_to_scene_ir.cpp \
  -o build/tools/fbx_adapter_probe

./build/tools/fbx_adapter_probe
