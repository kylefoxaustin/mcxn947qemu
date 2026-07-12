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

# ⚠ THE GATE USED TO GUARD ONLY README.md — AND docs/validation/test-result-matrix.md
# IS *ALSO* GENERATED.  So when the `flag-at-operator` class was DEPRECATED in the
# generator (it had been defined as "op acked + truth via QMP (no silent-wrong)" --
# a LICENCE TO LIE TO THE GUEST that had already authorised four real bugs), the
# fix landed in the generator and the OLD DEFINITION WENT ON BEING SHIPPED in the
# generated document, because nothing regenerated it and nothing checked it.
#
# A rule fixed at the SOURCE and stale at the POINT OF USE is still a live rule.
# Guard every generated artifact, not just the one you happened to think of.
python3 tests/gen-test-matrix.py --no-run >/dev/null
if ! git diff --quiet -- docs/validation/test-result-matrix.md; then
    echo "DRIFT: docs/validation/test-result-matrix.md is stale vs test-matrix.yaml."
    echo "Fix: python3 tests/gen-test-matrix.py --no-run  &&  commit the .md"
    git --no-pager diff -- docs/validation/test-result-matrix.md
    exit 1
fi
echo "OK: README capability table is in sync with test-matrix.yaml"
