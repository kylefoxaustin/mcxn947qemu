#!/bin/bash
set -e
cd "$(dirname "$0")/../.."
exec python3 tests/mcxn-spc-power/check.py
