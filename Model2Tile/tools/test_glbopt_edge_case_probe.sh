#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${ROOT_DIR}"
mkdir -p build/tools

c++ -std=c++17 -O2 -Wall -Wextra \
  -isystem ./dep/tinygltf \
  -isystem ./dep/meshoptimizer/src \
  tools/glbopt_edge_case_probe.cpp \
  glbopt.cpp \
  glbopt_io.cpp \
  glbopt_optimize.cpp \
  glbopt_rewrite.cpp \
  glbopt_deep_copy.cpp \
  tinygltf_imp.cpp \
  dep/meshoptimizer/src/allocator.cpp \
  dep/meshoptimizer/src/clusterizer.cpp \
  dep/meshoptimizer/src/indexanalyzer.cpp \
  dep/meshoptimizer/src/indexcodec.cpp \
  dep/meshoptimizer/src/indexgenerator.cpp \
  dep/meshoptimizer/src/meshletcodec.cpp \
  dep/meshoptimizer/src/overdrawoptimizer.cpp \
  dep/meshoptimizer/src/partition.cpp \
  dep/meshoptimizer/src/quantization.cpp \
  dep/meshoptimizer/src/rasterizer.cpp \
  dep/meshoptimizer/src/simplifier.cpp \
  dep/meshoptimizer/src/spatialorder.cpp \
  dep/meshoptimizer/src/stripifier.cpp \
  dep/meshoptimizer/src/vcacheoptimizer.cpp \
  dep/meshoptimizer/src/vertexcodec.cpp \
  dep/meshoptimizer/src/vertexfilter.cpp \
  dep/meshoptimizer/src/vfetchoptimizer.cpp \
  -o build/tools/glbopt_edge_case_probe

./build/tools/glbopt_edge_case_probe
