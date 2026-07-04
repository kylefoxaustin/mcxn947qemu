#!/usr/bin/env bash
# CI anti-drift gate (fleet-common, per 91's 446062f980): the README capability
# table MUST equal what test-matrix.yaml renders.  Regenerate it and fail the
# build (red) if it changed — a block added / removed / re-classed in the YAML,
# or a hand-edited README table, is caught here instead of silently drifting.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
# verify_groups() runs inside --inject-readme: fails first on any un-grouped,
# double-grouped, or mis-tiered block.
python3 tests/gen-test-matrix.py --inject-readme
if ! git diff --quiet -- README.md; then
    echo "DRIFT: README capability table is stale vs docs/validation/test-matrix.yaml."
    echo "Fix: python3 tests/gen-test-matrix.py --inject-readme  &&  commit README.md"
    git --no-pager diff -- README.md
    exit 1
fi
echo "OK: README capability table is in sync with test-matrix.yaml"
