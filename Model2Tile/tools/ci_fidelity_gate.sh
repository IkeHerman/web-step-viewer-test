#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo_root/Model2Tile"

make -j4
python3 tools/run_fidelity_suite.py
