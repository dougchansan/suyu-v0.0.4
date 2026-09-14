#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${1:?usage: test-real-emitter.sh NEW_OUTPUT_DIRECTORY}
if [[ -e "$OUT" ]]; then
  echo 'Output must not already exist; preserve prior evidence.' >&2; exit 2
fi
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
: "${CXX:=c++}"
"$CXX" -std=c++20 -O1 -I"$ROOT/src" "$HERE/tests/export_synthetic.cpp" -o "$OUT/export_synthetic"
SUYU_AOT_TRANSLATE_ALL=1 "$OUT/export_synthetic" "$OUT/exefs"
python3 - "$HERE/tests" "$OUT/exefs" <<'PYCODE'
import sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
from make_fixture import registration
(Path(sys.argv[2])/'recomp_registration.c').write_text(registration())
PYCODE
cmake -S "$HERE" -B "$OUT/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSWITCH_AOT_EXEFS="$OUT/exefs"
cmake --build "$OUT/build" --parallel 2
ctest --test-dir "$OUT/build" --output-on-failure
