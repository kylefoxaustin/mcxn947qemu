"""
LPUART -- the console UART -- ADVERTISED an 8-deep RX FIFO and implemented a ONE-BYTE
holding register.

The advertisement was CORRECT: RM FIFO reset = 0x00C0_0022, RXFIFOSIZE = 010b, and the
RM's own table reads "010b - 8".  So the model told the truth about the CHIP and a lie
about ITSELF.  A capability register is a CONTRACT: we promised eight and delivered one.

It was invisible because RM defines STAT[RDRF] as "the number of datawords in the
receive buffer is GREATER THAN [RXWATER]" -- and RXWATER resets to 0, so with no
watermark set, depth-1 and depth-8 are indistinguishable.  NOTHING IN 70 SUITES EVER
SET A WATERMARK.  A driver that did (enable RXFE, RXWATER=3, wait for RDRF, read 4
bytes) was woken on the FIRST byte and read three stale ones.
"""
import subprocess, os, signal, sys, socket, tempfile, time

BASE     = 0x400B4000        # FlexComm4 / LPUART4 -- the FRDM debug console
STAT     = BASE + 0x14
CTRL     = BASE + 0x18
DATA     = BASE + 0x1C
FIFO     = BASE + 0x28
WATER    = BASE + 0x2C

# ⚠ EVERY BIT BELOW IS THE CMSIS MASK, NOT A NUMBER I TYPED FROM THE BIT INDEX.
#   The first draft of this test said CTRL_RE = 1<<19.  That is CTRL_**TE** -- CMSIS
#   says LPUART_CTRL_RE_MASK = 0x40000 (bit 18).  So it enabled the TRANSMITTER, no
#   byte was ever delivered, and the test indicted a MODEL THAT WAS CORRECT.
#   ⭐ A TEST WITH A WRONG CONSTANT DOES NOT MERELY MISS A BUG -- IT MANUFACTURES ONE,
#     and the fix you then apply is damage.  Take the constants from the header, always.
CTRL_RE  = 0x00040000        # CMSIS LPUART_CTRL_RE_MASK
STAT_RDRF= 0x00200000        # CMSIS LPUART_STAT_RDRF_MASK
STAT_OR  = 0x00080000        # CMSIS LPUART_STAT_OR_MASK
FIFO_RXFE   = 1 << 3
FIFO_RXFLUSH= 1 << 14
FIFO_RXEMPT = 1 << 22

sock_path = tempfile.mktemp(suffix=".sock")
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(sock_path); srv.listen(1)

p = subprocess.Popen(["build/qemu-system-arm","-M","frdm-mcxn947","-accel","qtest",
                      "-qtest","stdio","-nographic","-monitor","none",
                      "-chardev","socket,id=u0,path=%s" % sock_path,
                      "-serial","chardev:u0"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                     start_new_session=True)
conn, _ = srv.accept()

def cmd(c):
    p.stdin.write(c+"\n"); p.stdin.flush()
    while True:
        l = p.stdout.readline()
        if l == "":
            print("\n*** QEMU EXITED while answering %r -- the model ABORTED." % c)
            sys.exit(1)
        if l.startswith("OK"):
            return l.split()[1] if len(l.split()) > 1 else ""
rd = lambda a: int(cmd("readl 0x%x" % a), 16)
wr = lambda a, v: cmd("writel 0x%x 0x%x" % (a, v))

def feed(bs):
    conn.sendall(bs)
    # let QEMU's chardev layer deliver; qtest clock is not driving the socket
    for _ in range(50):
        if rd(WATER) >> 24: break
        time.sleep(0.02)
    time.sleep(0.05)

fails = 0
def chk(what, got, want):
    global fails
    ok = got == want
    if not ok: fails += 1
    print("   %-58s %s  (got 0x%08x, want 0x%08x)"
          % (what, "ok" if ok else "FAIL", got, want))

wr(CTRL, CTRL_RE)                       # enable the receiver

print("\n① THE CAPABILITY: the chip says it has an 8-deep RX FIFO (RM: 010b = 8)")
chk("FIFO[RXFIFOSIZE] = 010b", rd(FIFO) & 0x7, 2)
chk("FIFO[RXEMPT] set when empty", rd(FIFO) & FIFO_RXEMPT, FIFO_RXEMPT)

print("\n② …AND NOW IT DELIVERS ONE. ENABLE THE FIFO, SET A WATERMARK OF 3.")
wr(FIFO, FIFO_RXFE)
wr(WATER, 3 << 16)                      # RXWATER = 3
feed(b"ABC")
chk("WATER[RXCOUNT] = 3 (three bytes BUFFERED, not dropped)", rd(WATER) >> 24, 3)
chk("RDRF CLEAR: 3 is not > 3", rd(STAT) & STAT_RDRF, 0)
print("      (the old 1-byte model raised RDRF here, on the very first byte)")

print("\n③ THE FOURTH BYTE CROSSES THE WATERMARK")
feed(b"D")
chk("WATER[RXCOUNT] = 4", rd(WATER) >> 24, 4)
chk("RDRF SET: 4 > 3", rd(STAT) & STAT_RDRF, STAT_RDRF)

print("\n④ AND ALL FOUR BYTES ARE REALLY THERE, IN ORDER")
print("   (the old model had ONE byte; a driver reading 4 got 1 real + 3 stale)")
for expect in b"ABCD":
    chk("DATA pops %r" % chr(expect), rd(DATA) & 0xFF, expect)
chk("FIFO now empty", rd(FIFO) & FIFO_RXEMPT, FIFO_RXEMPT)

print("\n⑤ NINE BYTES INTO AN EIGHT-DEEP FIFO: THE FIFO SATURATES AND NOTHING IS LOST")
print("   ⚠ A SOCKET-BACKED UART CANNOT OVERRUN, AND THAT IS THE CHARDEV'S DOING, NOT")
print("     THE MODEL'S: can_receive() reports our free space, so QEMU BACKPRESSURES")
print("     the sender and HOLDS the byte.  Real silicon has no such flow control -- a")
print("     byte on the wire arrives whether or not there is room, and STAT[OR] fires.")
print("     So the overrun path exists and is correct, but a socket cannot reach it.")
print("     ⭐ THE HONEST ASSERTION IS THEREFORE THE STRONGER ONE: NO BYTE IS LOST.")
conn.sendall(b"123456789")
time.sleep(0.2)
chk("RXCOUNT saturates at the advertised depth (8)", rd(WATER) >> 24, 8)

got = bytearray()
for _ in range(9):
    for _ in range(60):
        if rd(WATER) >> 24:
            break
        time.sleep(0.02)
    if not (rd(WATER) >> 24):
        break
    got.append(rd(DATA) & 0xFF)
chk("all NINE bytes arrive, in order, none dropped",
    1 if bytes(got) == b"123456789" else 0, 1)
print("      (received %r)" % bytes(got))

print("\n⑥ RXFLUSH ACTUALLY EMPTIES IT")
conn.sendall(b"XY")
time.sleep(0.2)
chk("two bytes buffered", rd(WATER) >> 24, 2)
wr(FIFO, FIFO_RXFE | FIFO_RXFLUSH)
chk("RXCOUNT = 0 after RXFLUSH", rd(WATER) >> 24, 0)

os.killpg(os.getpgid(p.pid), signal.SIGKILL)
os.unlink(sock_path)
print("\n%s" % ("*** %d FAILED ***" % fails if fails else
      "PASS: the LPUART delivers the 8-deep FIFO it advertises"))
sys.exit(1 if fails else 0)
