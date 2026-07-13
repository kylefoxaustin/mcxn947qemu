#!/usr/bin/env python3
"""Differential: read EVERY register at reset, diff against the REFERENCE MANUAL.

WHY THIS EXISTS.  59 of this model's peripherals reset with

    memset(s->regs, 0, sizeof(s->regs));

which feels like a safe, neutral default.  It is not neutral.  It is 59 CLAIMS THAT
EVERY RESET VALUE IS ZERO, AND THE GUEST BELIEVES THEM.  Two of those claims turned
out to hard-fault real firmware:

  * SCG SIRCCSR resets to 0100_0020h.  Bit 5 (SIRC_CLK_PERIPH_EN) is SET OUT OF
    RESET.  CLOCK_GetFro12MFreq() reads it and returns 0 Hz when it is clear, so
    EVERY FlexComm driver init tripped assert(sourceClock_Hz > 0U) and HARD-FAULTED.
    Nothing in the guest's clock_config.c sets that bit BECAUSE ON SILICON IT IS
    ALREADY SET.
  * SCG's read path returned "always valid" for every oscillator, so the SDK
    reported an external crystal frequency FOR AN OSCILLATOR NOBODY TURNED ON.

    ⭐ A ZERO RESET VALUE IS NOT THE ABSENCE OF A CLAIM.  IT IS A CLAIM.

THE GOLDEN IS THE REFERENCE MANUAL (rm-golden.json, extracted from the MCX N RM
rev 7 register tables and checked in), read back at the addresses CMSIS gives.  So
this gate CANNOT BE SATISFIED BY THE MODEL AGREEING WITH ITSELF -- which is exactly
how the LPI2C sub-block sat at the wrong address for months with a green test: the
test had taken its addresses FROM THE MODEL.  A test that gets its addresses from
the model is not a test, it is a MIRROR, and mutation testing cannot see it because
mutating the model moves the mirror too.  This is the oracle nobody here authored.

ABOUT THE ALLOWLIST, which is the dangerous part of this file.

    "An independent whitelist doesn't merely MISS the bug -- IT CERTIFIES IT."
                                                       -- ollama_95_neutron

So the allowlist is built to SHRINK, and it fights back:

  * a mismatch NOT in the allowlist    -> FAIL (a new lie)
  * an allowlisted entry that now MATCHES -> FAIL ("this is fixed; delete the line")
  * the remaining count is PRINTED LOUDLY, every run.  No silent caps: a gate that
    quietly tolerates 400 known-wrong registers reads as "covered" when it is not.

COVERAGE IS PARTIAL AND SAID SO OUT LOUD: only registers whose RM table row parsed
cleanly AND that CMSIS attributes to exactly one peripheral are probed.  It is a
FLOOR on the bugs, not a ceiling.
"""
import json, os, signal, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
QEMU = os.environ.get("QEMU", os.path.join(HERE, "..", "..", "build", "qemu-system-arm"))

if not os.access(QEMU, os.X_OK):
    print("SKIP: qemu not built at %s" % QEMU)
    sys.exit(0)

golden = json.load(open(os.path.join(HERE, "rm-golden.json")))

#
# ⭐ COVERAGE IS AN ASSERTION, NOT A PRINT STATEMENT.
#
# This gate used to PRINT "probed N registers" and return PASS regardless of N.  So an
# extractor that silently regressed -- one broken regex, one unparsed table layout --
# would probe a THIRD of the chip, announce it accurately, AND STILL PASS.
#
#     A GATE WHOSE COVERAGE CANNOT FAIL IT IS A GATE YOU HAVE AGREED NOT TO LOOK AT.
#                                                     -- rt1180emulator, who lived it:
# his extractor printed "unmatched: 4371" every run, for a day, while returning PASS --
# and the blind set contained the two blocks he most needed it to see (his CCM clock
# roots and the eDMA map he had just found WRONG).  He read the number and it changed
# nothing, BECAUSE IT WAS PRINTED, NOT ASSERTED.  An honest number with no threshold is
# the same organ as an allowlist that never shrinks.
#
# THE COUNT LIVES OUTSIDE THE ARTIFACT (expected-coverage.txt), because the suite and
# the artifact must not share a source -- AND THE COUNT IS PART OF THE ARTIFACT.
#
# It RATCHETS IN BOTH DIRECTIONS, exactly like the deviation allowlist:
#     coverage DOWN -> FAIL (something went blind)
#     coverage UP   -> FAIL ("you can see more now: update the line and say so")
# Neither direction may pass silently.  A number that only ever goes up by accident is
# a number nobody is reading.
#
EXPECTED = int(open(os.path.join(HERE, "expected-coverage.txt")).read().strip())

if len(golden) != EXPECTED:
    direction = "SHRANK" if len(golden) < EXPECTED else "GREW"
    print("FAIL: COVERAGE %s -- the golden holds %d registers, expected-coverage.txt "
          "says %d." % (direction, len(golden), EXPECTED))
    if len(golden) < EXPECTED:
        print("      THE GATE HAS GONE PARTIALLY BLIND.  Every register it can no longer")
        print("      see is UNCHECKED, and this run would otherwise have said PASS.")
    else:
        print("      The gate can see MORE than it was told to.  That is good news --")
        print("      but it must be DECLARED, not absorbed: update expected-coverage.txt")
        print("      so the next regression has something to fail against.")
    sys.exit(2)

allow = {}
with open(os.path.join(HERE, "known-deviations.txt")) as f:
    for line in f:
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        inst, reg, reason = (line.split(None, 2) + [""])[:3]
        allow[(inst, reg)] = reason


def probe(regs):
    """Ask, read exactly as many answers as questions, then kill it.

    There is no `quit` in the qtest protocol -- it answers "FAIL Unknown command" --
    so QEMU never exits and THE HARNESS must end it.  (For a long time a CRASHING
    QEMU was what ended these tests: without -accel qtest the vCPU free-runs from a
    zeroed vector table and dies of Lockup.  A crash and a pass must never be
    indistinguishable, and a free-running CPU is a SECOND WRITER to the address
    space under test.)
    """
    #
    # ⭐ `timeout -s KILL` -- THE GATE CALLS THE SUBJECT, SO THE SUBJECT CAN WEDGE THE
    #    GATE.
    #
    # This harness blocks in p.stdout.readline().  A QEMU that hangs -- and QEMU CAN
    # hang; a wedged device model is exactly the class of bug this gate exists to find
    # -- parks the checker FOREVER, and it looks BUSY, NOT BROKEN.
    #
    #     "NO VERDICT" AND "STILL WORKING" ARE THE SAME OBSERVATION.  A refusal is a
    #     verdict; a hang is an ABSENCE.  Fail-safe assumes the gate RETURNS.
    #                                                   -- ollama_95_neutron, measured
    #
    # So the SUBJECT runs out-of-process under a HARD KILL (SIGTERM does not free a
    # process stuck in a driver), and the answer-count assertion below turns the kill
    # into a FAILED VERDICT rather than a silent short read.
    #
    # ⚠ AND KILL THE PROCESS *GROUP*, NOT THE PROCESS.
    #
    # `timeout` is now the child; QEMU is its GRANDchild.  A p.kill() sends SIGKILL to
    # `timeout`, which dies INSTANTLY WITHOUT FORWARDING ANYTHING -- and QEMU is
    # reparented to init and RUNS FOREVER.
    #
    #     ⭐ THE FIX FOR THE WEDGE CREATED A LEAK.  I added `timeout -s KILL` to stop a
    #        hung subject from parking the gate (ollama's finding), and in doing so I
    #        put a process between me and the thing I was killing.  Five orphaned QEMUs
    #        accumulated on a SHARED BOX in five hours; 91emulator reaped two more of
    #        mine, one of which had been spinning at 99.9% CPU FOR 18 HOURS, quietly
    #        poisoning the host load of everyone else's benchmarks.
    #
    #     "An orphaned process of your own is a second tenant that the fence is
    #      structurally blind to, BECAUSE IT IS WEARING YOUR BADGE."  -- ollama_95_neutron
    #
    # start_new_session puts the whole chain in its own process group, so killpg reaches
    # BOTH the wrapper and the subject.  A cleanup that cannot reach what it created is
    # not a cleanup.
    p = subprocess.Popen(
        ["timeout", "-s", "KILL", "120",
         QEMU, "-M", "frdm-mcxn947", "-display", "none", "-accel", "qtest",
         "-qtest", "stdio", "-monitor", "none", "-serial", "none"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, text=True, start_new_session=True)
    try:
        p.stdin.write("".join("readl 0x%x\n" % r["addr"] for r in regs))
        p.stdin.flush()
        vals = []
        while len(vals) < len(regs):
            line = p.stdout.readline()
            if not line:
                break
            if line.startswith("OK 0x"):
                vals.append(int(line.split()[1], 16))
        return vals
    finally:
        try:
            os.killpg(os.getpgid(p.pid), signal.SIGKILL)   # the wrapper AND the subject
        except (ProcessLookupError, PermissionError):
            pass
        p.wait()


vals = probe(golden)
if len(vals) != len(golden):
    print("FAIL: asked %d questions, got %d answers.  A truncated conversation and a "
          "correct one differ only in the answers you never notice are missing."
          % (len(golden), len(vals)))
    sys.exit(1)

mismatched = {(r["inst"], r["reg"]): (r, v)
              for r, v in zip(golden, vals) if v != r["reset"]}

new  = [k for k in mismatched if k not in allow]
# An allowlisted register that now agrees with the RM has been FIXED.  Say so, and
# FAIL, so the line gets deleted.  This is what stops the list from becoming a
# permanent certificate for 400 wrong answers.
stale = [k for k in allow if k not in mismatched]

print("probed %d registers against the RM (golden = the reference manual)  [asserted]"
      % len(golden))
print("  matching        : %d" % (len(golden) - len(mismatched)))
print("  known deviations: %d   <-- THIS NUMBER MUST GO DOWN" % (len(mismatched) - len(new)))

rc = 0
if new:
    print("\nFAIL: %d register(s) disagree with the RM and are NOT in the allowlist." % len(new))
    print("      The guest reads a value the silicon would never produce.")
    for inst, reg in sorted(new)[:25]:
        r, v = mismatched[(inst, reg)]
        print("        %-12s %-18s @0x%08x  model=0x%08x  RM=0x%08x"
              % (inst, reg, r["addr"], v, r["reset"]))
    if len(new) > 25:
        print("        ... and %d more" % (len(new) - 25))
    rc = 1

if stale:
    print("\nFAIL: %d allowlisted register(s) now MATCH the RM -- they are FIXED." % len(stale))
    print("      Delete them from known-deviations.txt.  An allowlist that never")
    print("      shrinks stops being a to-do list and becomes a CERTIFICATE.")
    for inst, reg in sorted(stale)[:25]:
        print("        %-12s %s" % (inst, reg))
    rc = 1

if rc == 0:
    print("\nPASS: no new reset-value lies; %d known deviations, all still known."
          % len(mismatched))
sys.exit(rc)
