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
import json, os, signal, subprocess, sys, threading

HERE = os.path.dirname(os.path.abspath(__file__))
QEMU = os.environ.get("QEMU", os.path.join(HERE, "..", "..", "build", "qemu-system-arm"))

if not os.access(QEMU, os.X_OK):
    print("SKIP: qemu not built at %s" % QEMU)
    sys.exit(0)

golden = json.load(open(os.path.join(HERE, "rm-golden.json")))

#
# ⭐ COVERAGE IS AN ASSERTION, NOT A PRINT -- AND IT IS A CENSUS, NOT A TOTAL.
#
# Two rungs, both learned the hard way, both from other people's trees:
#
#   rt1180emulator: a gate that PRINTS its coverage and passes regardless is a gate you
#   have agreed not to look at.  He read "unmatched: 4371" in his own output every run
#   for a day and it changed nothing, BECAUSE IT WAS PRINTED, NOT ASSERTED.
#
#   91emulator: an asserted TOTAL is still only a control on the SIZE of the coverage,
#   NOT ON ITS SHAPE.  A TOTAL CANNOT SEE A MISSING BLOCK -- if one peripheral goes
#   blind and another grows by the same amount, the number is unchanged and the gate
#   passes.  And A PRESENCE CHECK CANNOT SEE A HALF-EMPTY BLOCK.
#
# So the manifest is a PER-INSTANCE CENSUS held OUTSIDE the artifact, and it fails in
# every direction: missing, shrunk, grown, or new.  Nothing changes silently.
#
MANIFEST = {}
for _line in open(os.path.join(HERE, "expected-coverage.txt")):
    _line = _line.split("#", 1)[0].strip()
    if _line:
        _i, _n = _line.split()
        MANIFEST[_i] = int(_n)

_have = {}
for _r in golden:
    _have[_r["inst"]] = _have.get(_r["inst"], 0) + 1

_blind   = [(i, n, _have.get(i, 0)) for i, n in MANIFEST.items() if _have.get(i, 0) < n]
_grew    = [(i, n, _have.get(i, 0)) for i, n in MANIFEST.items() if _have.get(i, 0) > n]
_newinst = sorted(set(_have) - set(MANIFEST))

if _blind or _grew or _newinst:
    if _blind:
        print("FAIL: %d peripheral(s) LOST COVERAGE -- the gate has gone partially blind."
              % len(_blind))
        print("      Every register it can no longer see is UNCHECKED, and this run would")
        print("      otherwise have said PASS.  A TOTAL CANNOT SEE A MISSING BLOCK.")
        for _i, _want, _got in sorted(_blind):
            print("        %-16s expected %4d  got %4d   <-- %s"
                  % (_i, _want, _got, "WENT BLIND" if _got == 0 else "HALF-EMPTY"))
    if _grew or _newinst:
        print("FAIL: coverage GREW and was not declared.  Good news -- but it must be")
        print("      DECLARED, not absorbed, or the next regression has nothing to fail")
        print("      against.  Update expected-coverage.txt.")
        for _i, _want, _got in sorted(_grew):
            print("        %-16s expected %4d  got %4d" % (_i, _want, _got))
        for _i in _newinst:
            print("        %-16s NEW instance, %d registers" % (_i, _have[_i]))
    sys.exit(2)

#
# ⛔ EVERY ALLOWLIST ENTRY MUST CARRY A BUCKET AND A REASON, AND THE GATE ENFORCES IT.
#
# This parser USED TO DO:   line = line.split("#", 1)[0].strip()
#
# -- it threw the comment AWAY.  The reason column existed for humans and THE TOOL
# COULD NOT SEE IT.  So nobody ever noticed that all 194 "reasons" were the string
# `model=0x00000000 RM=0xc0000000`: THE DIFF, restated.  That is not a justification
# for a deviation, it is a restatement of the deviation.
#
#     ⭐ A COLUMN THE TOOL DOES NOT READ IS A COLUMN THE TOOL CANNOT ENFORCE --
#        AND IT FILLS UP WITH WHATEVER IS EASIEST TO TYPE.
#
# USBPHY CTRL hid in here: the PHY reports itself released from soft-reset and
# ungated before any firmware released it, and the line excusing it was shaped
# exactly like the 193 acceptable ones.
#
BUCKETS = ("DECISION", "UNMODELLED", "UNTRIAGED")
allow = {}
buckets = {b: 0 for b in BUCKETS}
malformed = []
with open(os.path.join(HERE, "known-deviations.txt")) as f:
    for lineno, raw in enumerate(f, 1):
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        body, _, comment = raw.partition("#")
        parts = body.split()
        if len(parts) < 2:
            malformed.append((lineno, raw.rstrip(), "not 'INST REG'"))
            continue
        inst, reg = parts[0], parts[1]
        bucket, _, why = comment.strip().partition(":")
        bucket = bucket.strip()
        if bucket not in BUCKETS:
            malformed.append((lineno, raw.rstrip(), "no bucket (need one of %s)"
                              % "/".join(BUCKETS)))
            continue
        if not why.strip():
            malformed.append((lineno, raw.rstrip(), "bucket with NO REASON"))
            continue
        allow[(inst, reg)] = (bucket, why.strip())
        buckets[bucket] += 1

if malformed:
    print("FAIL: %d allowlist entr%s no bucket+reason." %
          (len(malformed), "y carries" if len(malformed) == 1 else "ies carry"))
    print("      An entry without a reason does not excuse a deviation -- it HIDES one.")
    print("      Adding a line is a DECISION.  Decisions are typed by people, with a")
    print("      reason, or they are not decisions.")
    for lineno, raw, why in malformed[:15]:
        print("        line %-4d %-52s  <- %s" % (lineno, raw[:52], why))
    sys.exit(2)


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
    #
    # ⭐ ASK AND LISTEN CONCURRENTLY.  DO NOT WRITE EVERY QUESTION AND *THEN* READ.
    #
    # This used to be one blocking write of the whole question stream, followed by the
    # reads.  At 4073 registers that write is 69,241 BYTES against a 65,536-BYTE PIPE
    # BUFFER -- 1.06x OVER.  It completes today ONLY because QEMU happens to drain stdin
    # while we are still writing.
    #
    #     ⭐ WE WERE NOT SAFE.  WE WERE CORRECT BY LUCK -- and the thing that spends the
    #        luck is THE NEXT COVERAGE FIX.  Every register we teach the extractor to see
    #        pushes the question stream further past the boundary.  FIX THE BLINDNESS,
    #        BREAK THE HARNESS: the repair and the trigger are the same commit.
    #
    # 91emulator found this (it deadlocked their harness at 9282 registers) and
    # rt1180emulator measured it in mine before it fired.  And the failure mode is
    # vicious: 91 spent twenty minutes convinced a DEVICE MODEL was aborting on a read.
    #
    #     ⭐ A HARNESS DEADLOCK IS INDISTINGUISHABLE FROM A GUEST BUG -- AND IT ARRIVES
    #        DISGUISED AS YOUR OWN SUCCESS (the reward for widening coverage).
    #
    # The writer runs in its own thread, so a full pipe blocks the WRITER, never the
    # READER.  A short read is still turned into a VERDICT by the answer-count assertion
    # below -- it cannot become a silent truncation.
    #
    #
    # ⭐ READ EACH REGISTER AT ITS TRUE WIDTH.
    #
    # This used to issue `readl` for everything -- and the extractor, to keep that honest,
    # threw away every register that was not 32 bits.  340 rows: 250 sixteen-bit and 90
    # eight-bit, INCLUDING THE ENTIRE eDMA TCD BLOCK (TCD_CSR, TCD_CITER, TCD_BITER,
    # TCD_SOFF, TCD_DOFF, TCD_ATTR) -- the heart of the DMA engine, and the block with
    # more silent-wrong-answer bugs than any other in this tree.  THE GATE HAD NEVER
    # LOOKED AT IT.
    #
    # (rt1180emulator did this on his tree and the 32-bit blindness was hiding the MOTOR
    # DRIVE: eFlexPWM's DEAD-TIME counters, 0x07FF on silicon, ZERO in his model.  Zero
    # dead time is a direct short across the DC bus through both transistors of an
    # inverter leg.  Every PWM test green.)
    #
    _CMD = {8: "readb", 16: "readw", 32: "readl"}

    def _ask():
        try:
            for r in regs:
                p.stdin.write("%s 0x%x\n"
                              % (_CMD[r.get("width", 32)], r["addr"]))
            p.stdin.flush()
            p.stdin.close()
        except (BrokenPipeError, ValueError):
            pass          # the subject died; the answer count will say so

    try:
        writer = threading.Thread(target=_ask, daemon=True)
        writer.start()

        vals = []
        while len(vals) < len(regs):
            line = p.stdout.readline()
            if not line:
                break
            if line.startswith("OK 0x"):
                vals.append(int(line.split()[1], 16))
        writer.join(timeout=5)
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
print("      DECISION  : %4d  deliberate, defended, with a stated reason" % buckets["DECISION"])
print("      UNMODELLED: %4d  no model for the block -- a GAP, not a lie" % buckets["UNMODELLED"])
print("      UNTRIAGED : %4d  ⚠ IN BLOCKS WE MODEL -- NOT YET LOOKED AT."
      % buckets["UNTRIAGED"])
print("                        Every one could be the next USBPHY CTRL, which sat here")
print("                        telling guests the USB PHY was out of reset when it was not.")
print("                        This is not a status. It is an admission, and it must shrink.")

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
