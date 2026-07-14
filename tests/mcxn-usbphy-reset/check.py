import subprocess, os, signal, sys
p=subprocess.Popen(["build/qemu-system-arm","-M","frdm-mcxn947","-accel","qtest","-qtest","stdio",
    "-nographic","-monitor","none","-serial","none"],stdin=subprocess.PIPE,stdout=subprocess.PIPE,
    text=True,start_new_session=True)
def cmd(c):
    p.stdin.write(c+"\n"); p.stdin.flush()
    while True:
        l=p.stdout.readline()
        if l.startswith("OK"): return l.split()[1] if len(l.split())>1 else ""
rd=lambda a:int(cmd("readl 0x%x"%a),16); wr=lambda a,v:cmd("writel 0x%x 0x%x"%(a,v))
CTRL=0x4010A030; SFTRST=1<<31; CLKGATE=1<<30
fails=0
def chk(what, got, want):
    global fails
    ok = got==want
    if not ok: fails+=1
    print("   %-52s %s  (got 0x%08x, want 0x%08x)" % (what, "ok" if ok else "FAIL", got, want))

print("\n① THE PHY POWERS UP HELD IN RESET AND GATED (RM: CTRL reset = 0xC000_0000)")
chk("CTRL at reset = SFTRST|CLKGATE", rd(CTRL), 0xC0000000)
chk("...and the guest can SEE it is in reset", rd(CTRL)&SFTRST, SFTRST)

print("\n② THE ALIASES ARE VIEWS OF THE SAME REGISTER, NOT FOUR REGISTERS")
for off,nm in ((4,"CTRL_SET"),(8,"CTRL_CLR"),(0xC,"CTRL_TOG")):
    chk("%s reads the SAME value as CTRL" % nm, rd(CTRL+off), rd(CTRL))

print("\n③ SFTRST DOES NOT SELF-CLEAR -- SOFTWARE MUST WRITE 0 (RM 62.x, twice)")
wr(CTRL+8, SFTRST)                       # CLR alias: release from soft reset
chk("after CLR[SFTRST], reset is released", rd(CTRL)&SFTRST, 0)
chk("...but CLKGATE is STILL SET (not touched)", rd(CTRL)&CLKGATE, CLKGATE)
wr(CTRL+8, CLKGATE)                      # then ungate the UTMI clocks
chk("after CLR[CLKGATE], clocks ungated", rd(CTRL)&CLKGATE, 0)
chk("the alias now tracks the LIVE value, not the reset one", rd(CTRL+4), rd(CTRL))

print("\n④ AND THE FROZEN-ALIAS BUG IS GONE (it used to read 0xC0000000 forever)")
chk("CTRL_SET after full release", rd(CTRL+4), 0x00000000)
os.killpg(os.getpgid(p.pid), signal.SIGKILL)
print("\n%s" % ("*** %d FAILED ***"%fails if fails else "PASS: the PHY tells the guest the truth about its own reset state"))
sys.exit(1 if fails else 0)
