"""
SPC -- the System Power Controller.  This suite does not check that the model
AGREES WITH THE RM (the reset-value gate already does that).  It checks the
CONSEQUENCE: that a guest doing what the vendor driver does cannot be made to
switch off its own power rails by reading a lie.

The bug it pins: SPC was memset(0), so CNTRL read 0 -- all three regulators OFF --
when silicon says 0x7 (CORELDO_EN|SYSLDO_EN|DCDC_EN; the chip is RUNNING, of course
they are on).  fsl_spc.c read-modify-writes that register:

    base->CNTRL |= SPC_CNTRL_CORELDO_EN_MASK;

so a guest enabling ONE regulator read our zero, OR'd in its bit, wrote back --
and silently switched off the SYS LDO and the DC-DC converter it never touched.
On silicon that is a brownout.  The model never read the value back, so nothing
in the model could ever notice.  (93emulator's RMW-laundering class.)
"""
import subprocess, os, signal, sys

SPC          = 0x40045000
CNTRL        = SPC + 0x014
VD_CORE_CFG  = SPC + 0x134
ACTIVE_VDELAY= SPC + 0x124

CORELDO_EN = 1 << 0      # CMSIS: SPC_CNTRL_CORELDO_EN_MASK
SYSLDO_EN  = 1 << 1      # CMSIS: SPC_CNTRL_SYSLDO_EN_MASK
DCDC_EN    = 1 << 2      # CMSIS: SPC_CNTRL_DCDC_EN_MASK
LVDRE      = 1 << 0      # CMSIS: SPC_VD_CORE_CFG_LVDRE_MASK  (brownout RESET enable)
LVDIE      = 1 << 1      # CMSIS: SPC_VD_CORE_CFG_LVDIE_MASK  (brownout INTERRUPT enable)

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
    print("   %-56s %s  (got 0x%08x, want 0x%08x)"
          % (what, "ok" if ok else "FAIL", got, want))

print("\n① THE CHIP IS RUNNING, SO ITS REGULATORS ARE ON (RM: CNTRL reset = 0x7)")
chk("CNTRL = CORELDO_EN | SYSLDO_EN | DCDC_EN", rd(CNTRL), 0x7)

print("\n② THE VENDOR DRIVER'S READ-MODIFY-WRITE MUST NOT DISABLE THE OTHER TWO RAILS")
print("   (fsl_spc.c: base->CNTRL |= SPC_CNTRL_DCDC_EN_MASK)")
v = rd(CNTRL)            # <-- the guest reads.  With the bug this was 0.
wr(CNTRL, v | DCDC_EN)   # <-- ...ORs in its one bit, and writes back.
after = rd(CNTRL)
chk("CORELDO still enabled after the guest's RMW", after & CORELDO_EN, CORELDO_EN)
chk("SYSLDO  still enabled after the guest's RMW", after & SYSLDO_EN,  SYSLDO_EN)
chk("DCDC    enabled, as the guest asked",         after & DCDC_EN,    DCDC_EN)

print("\n③ THE BROWNOUT RESET IS ARMED AT POWER-ON, AND ARMING THE *INTERRUPT*")
print("   MUST NOT SILENTLY DISARM THE *RESET* (RM: VD_CORE_CFG reset = LVDRE)")
chk("VD_CORE_CFG[LVDRE] set at reset", rd(VD_CORE_CFG) & LVDRE, LVDRE)
v = rd(VD_CORE_CFG)
wr(VD_CORE_CFG, v | LVDIE)          # guest arms the low-voltage INTERRUPT
chk("LVDRE survives the guest arming LVDIE", rd(VD_CORE_CFG) & LVDRE, LVDRE)

print("\n④ THE CORE RAIL DOES NOT SETTLE INSTANTLY (RM: ACTIVE_VDELAY = 200)")
chk("ACTIVE_VDELAY = 0xC8", rd(ACTIVE_VDELAY), 0xC8)

os.killpg(os.getpgid(p.pid), signal.SIGKILL)
print("\n%s" % ("*** %d FAILED ***" % fails if fails else
      "PASS: a guest cannot switch off its own power rails by reading this model"))
sys.exit(1 if fails else 0)
