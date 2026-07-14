"""
PORTSC1[PSPD] -- "what speed did I enumerate at?"

    00 = Full Speed   01 = Low Speed   10 = High Speed   11 = Undefined

The model returned ZERO for the whole register.  On PSPD, zero is not "no answer" --
IT IS FULL SPEED.  So a driver asking a HIGH-SPEED controller what speed it was running
at got back a confident "Full Speed", and would have sized its endpoints for 64-byte
packets on a controller that does 512.

    ⭐ A ZERO IS NOT THE ABSENCE OF A CLAIM.  ON AN ENCODED FIELD IT IS WHICHEVER CLAIM
      HAPPENS TO BE ENCODED AS ZERO.

And the RM's reset value is MORE HONEST than our zero: it says UNDEFINED -- the speed is
not known yet -- where we said Full Speed, which is an answer.

PSPD is a STATUS field, so it does not sit at its reset value: undefined until the guest
starts the controller (USBCMD[RS]), then the speed we actually operate at.
"""
import subprocess, os, signal, sys

USBC   = 0x4010B000
SBUSCFG    = USBC + 0x090
USBCMD     = USBC + 0x140
BURSTSIZE  = USBC + 0x160
CONFIGFLAG = USBC + 0x180
PORTSC1    = USBC + 0x184
USBMODE    = USBC + 0x1A8

PSPD_SHIFT = 26
PSPD_FULL, PSPD_LOW, PSPD_HIGH, PSPD_UNDEF = 0, 1, 2, 3
PE   = 1 << 2
RS   = 1 << 0
ITC  = 0xFF0000

p = subprocess.Popen(["build/qemu-system-arm", "-M", "frdm-mcxn947", "-accel", "qtest",
                      "-qtest", "stdio", "-nographic", "-monitor", "none", "-serial", "none"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                     start_new_session=True)
def cmd(c):
    p.stdin.write(c + "\n"); p.stdin.flush()
    while True:
        l = p.stdout.readline()
        if l == "":
            print("\n*** QEMU EXITED while answering %r -- the model ABORTED." % c)
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

print("\n① AT RESET THE SPEED IS *UNDEFINED*, NOT 'FULL SPEED'")
print("   (a zero here is not silence -- it is the answer 'Full Speed')")
speed = (rd(PORTSC1) >> PSPD_SHIFT) & 3
chk("PORTSC1[PSPD] = Undefined (0b11)", speed, PSPD_UNDEF)
chk("...and it is NOT Full Speed (0b00)", 1 if speed != PSPD_FULL else 0, 1)
chk("PORTSC1[PE] set: in device mode the port is always enabled", rd(PORTSC1) & PE, PE)

print("\n② ONCE THE GUEST STARTS THE CONTROLLER, IT REPORTS THE SPEED IT ACTUALLY RUNS AT")
wr(USBCMD, rd(USBCMD) | RS)
speed = (rd(PORTSC1) >> PSPD_SHIFT) & 3
chk("PORTSC1[PSPD] = High Speed (0b10)", speed, PSPD_HIGH)

print("\n③ THE CONFIG REGISTERS THE VENDOR DRIVER READ-MODIFY-WRITES")
print("   (our zeros were being laundered into the guest's own state)")
chk("USBCMD[ITC] = 8 (interrupt threshold, not 'immediately')",
    (rd(USBCMD) & ITC) >> 16, 8)
chk("BURSTSIZE = 0x0808 (not a zero-length burst)", rd(BURSTSIZE), 0x0808)
chk("CONFIGFLAG[CF] = 1 (port routed to this controller)", rd(CONFIGFLAG) & 1, 1)
chk("SBUSCFG = 2", rd(SBUSCFG), 2)
chk("USBMODE = 0x5000", rd(USBMODE), 0x5000)

print("\n④ AND A DRIVER'S READ-MODIFY-WRITE MUST NOT DESTROY WHAT IT DID NOT TOUCH")
v = rd(USBCMD)                 # the guest reads...
wr(USBCMD, v | RS)             # ...ORs in its one bit, and writes back
chk("ITC survives the guest's RMW", (rd(USBCMD) & ITC) >> 16, 8)

os.killpg(os.getpgid(p.pid), signal.SIGKILL)
print("\n%s" % ("*** %d FAILED ***" % fails if fails else
      "PASS: a high-speed controller no longer tells the guest it is full-speed"))
sys.exit(1 if fails else 0)
