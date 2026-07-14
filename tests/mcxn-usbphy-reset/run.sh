#!/bin/bash
# USBPHY comes out of power-on reset HELD IN SOFT-RESET AND CLOCK-GATED
# (RM: CTRL reset = 0xC000_0000).  Software must clear SFTRST, then CLKGATE.
#
# This suite exists because the model used to STRIP both bits on every read, with a
# comment asserting they "self-clear".  They do not -- the RM says "Write 0 to
# CTRL[SFTRST] to release USBPHY from reset", twice, and the separate
# ENAUTOCLR_CLKGATE control is proof that CLKGATE does not auto-clear either.
# The reset-value gate CAUGHT the disagreement and it was ALLOWLISTED.  The
# allowlist did not miss the bug; it certified it.
#
# It also pins the alias semantics: SET/CLR/TOG are four VIEWS of one register.
# The write path folded them onto the base; the read path did not, so each alias
# returned its own frozen reset value forever.
set -e
cd "$(dirname "$0")/../.."
exec python3 tests/mcxn-usbphy-reset/check.py
