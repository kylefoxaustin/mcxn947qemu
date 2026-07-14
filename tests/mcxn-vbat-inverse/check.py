"""
VBAT -- the always-on domain (16 kHz FRO, 32 kHz crystal, retention LDO).

RM 36.x: "The VBAT registers are implemented as separate A and B registers.  When
configuring an A register, you must write the inverse value to the corresponding B
register."  It is a HARDWARE GUARD: the one power domain that survives reset is
protected against a spurious single write by requiring a complement pair.

The model used to ignore it entirely -- it took a bare LDOCTLA=7 and brought the LDO
up.  SILICON DOES NOT.  A model more permissive than the hardware does not fail safe:
firmware that skipped the B write worked here and would have died on the bench.

It also had LDOCTLA at 0x200 -- which is FROCTLA, the FRO's register -- so the LDO's
ready bit was gated on a different peripheral's enable.
"""
import subprocess, os, signal, sys

VBAT     = 0x40059000
STATUSA  = VBAT + 0x010
OSCCTLA  = VBAT + 0x100
OSCCTLB  = VBAT + 0x104
FROCTLA  = VBAT + 0x200      # <-- what the old model called LDOCTLA
FROCTLB  = VBAT + 0x204
LDOCTLA  = VBAT + 0x300      # <-- the REAL LDO control register
LDOCTLB  = VBAT + 0x304
LDOLCKA  = VBAT + 0x318
LDOLCKB  = VBAT + 0x31C

LDO_RDY  = 1 << 4
OSC_RDY  = 1 << 5
LDO_EN   = 1 << 1
OSC_EN   = 1 << 0

p = subprocess.Popen(["build/qemu-system-arm","-M","frdm-mcxn947","-accel","qtest",
                      "-qtest","stdio","-nographic","-monitor","none","-serial","none"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                     start_new_session=True)
def cmd(c):
    """
    ⚠ IF QEMU DIES, SAY SO -- DO NOT BLOCK ON A DEAD PIPE.
    This used to be `while True: readline()`, which meant a model that ABORTED (say,
    on a failed reset-time assertion) left the harness waiting forever for an answer
    that was never coming.  The suite then HUNG and was killed by a timeout.

        ⭐ A HANG IS NOT A CAUGHT BUG.  "The subject crashed" and "the subject is
           still thinking" are THE SAME OBSERVATION to a blocking read -- and a
           killed run is INCONCLUSIVE, never a FAILURE.  (ollama's wedge, in my own
           test harness: the gate calls the subject, so the subject can wedge the gate.)

    An EOF on stdout means QEMU is gone.  That is a hard FAIL with a reason, now.
    """
    p.stdin.write(c+"\n"); p.stdin.flush()
    while True:
        l = p.stdout.readline()
        if l == "":                      # EOF: QEMU died (abort, assert, segv)
            try:
                rc = p.wait(timeout=5)
            except Exception:
                rc = p.poll()
            print("\n*** QEMU EXITED (rc=%s) while answering %r -- the model ABORTED."
                  % (rc, c))
            print("    Not a hang: a dead subject is a FAILED test, and it says so.")
            sys.exit(1)
        if l.startswith("OK"):
            return l.split()[1] if len(l.split()) > 1 else ""
rd = lambda a: int(cmd("readl 0x%x" % a), 16)
wr = lambda a, v: cmd("writel 0x%x 0x%x" % (a, v))

fails = 0
def chk(what, got, want):
    global fails
    ok = got == want
    if not ok: fails += 1
    print("   %-58s %s  (got 0x%08x, want 0x%08x)"
          % (what, "ok" if ok else "FAIL", got, want))

print("\n① THE FRO16K IS ALREADY RUNNING AT POWER-ON (RM: FROCTLA reset = 1)")
chk("FROCTLA = 1", rd(FROCTLA), 1)
chk("FROCTLB = 0 (its inverse)", rd(FROCTLB), 0)

print("\n② A BARE A-WRITE IS NOT A CONFIGURATION. SILICON REFUSES IT.")
print("   (the old model took it, and brought the LDO up that never came up on the board)")
wr(LDOCTLA, 0x7)                       # RM step 2 -- and STOP. No B write.
chk("LDOCTLA took the value", rd(LDOCTLA), 0x7)
chk("...but LDO_RDY does NOT assert", rd(STATUSA) & LDO_RDY, 0)

print("\n③ THE FULL RM SEQUENCE BRINGS IT UP (LDOCTLA=7h, then LDOCTLB[INVERSE]=0h)")
wr(LDOCTLB, 0x0)                       # RM step 3: the inverse
chk("LDO_RDY asserts once the pair is complete", rd(STATUSA) & LDO_RDY, LDO_RDY)

print("\n④ AND THE LDO IS NOT THE FRO -- LDOCTLA IS AT 0x300, NOT 0x200")
print("   (the old model gated LDO_RDY on bit 1 of FROCTLA, a different peripheral)")
chk("FROCTLA untouched by the LDO sequence", rd(FROCTLA), 1)

print("\n⑤ OSC: same handshake, independently")
chk("OSC_RDY clear at reset", rd(STATUSA) & OSC_RDY, 0)
wr(OSCCTLA, OSC_EN)
chk("...still clear after the A-write alone", rd(STATUSA) & OSC_RDY, 0)
wr(OSCCTLB, (~OSC_EN) & 0xFFFFF)
chk("...asserts once the inverse lands", rd(STATUSA) & OSC_RDY, OSC_RDY)

print("\n⑥ ONCE LOCKED, SILICON REFUSES THE WRITE. WE USED TO TAKE IT.")
wr(LDOLCKA, 1); wr(LDOLCKB, 0)          # RM: lock the LDO (LCKA=1, LCKB=~1=0)
wr(LDOCTLA, 0x0)                        # a guest tries to disable the retention LDO
chk("LDOCTLA unchanged: the locked write was REFUSED", rd(LDOCTLA), 0x7)
chk("...and the LDO is still up", rd(STATUSA) & LDO_RDY, LDO_RDY)

os.killpg(os.getpgid(p.pid), signal.SIGKILL)
print("\n%s" % ("*** %d FAILED ***" % fails if fails else
      "PASS: VBAT demands the inverse-pair handshake, and honours its lock"))
sys.exit(1 if fails else 0)
