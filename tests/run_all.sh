#!/bin/sh
# Single-command CI runner (Section 9.8). From the repo root:
#   sh tests/run_all.sh
# Phase 1: unit suites. Phase 2: property tests (5 fixed seeds; both live in
# lsmkv_tests). Phase 3: chaos drills A-E. On failure, keep as artifacts:
# data/manifest.json, wal-verify output, stats --json, and the failing seed
# (printed by lsmkv_tests).
set -u
TESTS=./build/Debug/lsmkv_tests.exe
LSMKV=./build/Debug/lsmkv.exe
[ -x "$TESTS" ] || TESTS=./build/lsmkv_tests
[ -x "$LSMKV" ] || LSMKV=./build/lsmkv

echo "== phase 0: build =="
cmake --build build || exit 1

echo "== phase 1+2: unit + property (fixed seeds 1..5) =="
"$TESTS" || { echo "unit/property failed"; exit 1; }

echo "== phase 3: chaos drills A-E =="
sh tests/drills.sh "$LSMKV" || { echo "drills failed";
  echo "-- artifacts --"; "$LSMKV" wal-verify; "$LSMKV" stats --json 2>/dev/null;
  cat data/manifest.json 2>/dev/null; exit 1; }

echo "== all phases passed =="
