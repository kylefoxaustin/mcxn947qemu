"""
THE FIFO MUST SURVIVE A SNAPSHOT -- AND I CLAIMED IT DID WITHOUT EVER CHECKING.

When I gave the LPUART a real 8-deep RX FIFO I added it to the vmstate and wrote, in
the commit message: "buffered bytes are state, and state that is not migrated silently
vanishes across a snapshot."  That is an ASSERTION.  I never tested it.  I read the
code and believed myself -- which is the exact failure this whole tree exists to refuse.

    91emulator: "a migration test that only checks values cannot see a wire format it
    has silently broken."  And before that it cannot even see whether the values move.

So: put bytes in the FIFO, savevm, wipe the machine's idea of the world by loadvm, and
demand the bytes come back -- IN ORDER, with the count and the watermark state intact.

⭐ AND THE NEGATIVE HALF, WHICH IS THE ONE THAT MATTERS: this test must FAIL if the FIFO
  is dropped from the vmstate.  A migration test that passes on a model which migrates
  nothing is not a test; it is a receipt.
"""
import subprocess, os, signal, sys, socket, tempfile, time

BASE  = 0x400B4000          # FlexComm4 / LPUART4 -- the FRDM debug console
STAT  = BASE + 0x14
CTRL  = BASE + 0x18
DATA  = BASE + 0x1C
FIFO  = BASE + 0x28
WATER = BASE + 0x2C

CTRL_RE   = 0x00040000      # CMSIS LPUART_CTRL_RE_MASK
STAT_RDRF = 0x00200000      # CMSIS LPUART_STAT_RDRF_MASK
FIFO_RXFE = 0x8             # CMSIS LPUART_FIFO_RXFE_MASK

sock_path = tempfile.mktemp(suffix=".sock")
snap_dir  = tempfile.mkdtemp()
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(sock_path); srv.listen(1)

# qtest + a QMP socket so we can savevm/loadvm.  The snapshot needs a block device to
# live in, so give it a small qcow2 the machine does not otherwise use.
img = os.path.join(snap_dir, "snap.qcow2")
subprocess.run(["build/qemu-img", "create", "-f", "qcow2", img, "16M"],
               check=True, stdout=subprocess.DEVNULL)

qmp_path = tempfile.mktemp(suffix=".qmp")
p = subprocess.Popen(["build/qemu-system-arm", "-M", "frdm-mcxn947", "-accel", "qtest",
                      "-qtest", "stdio", "-nographic", "-monitor", "none",
                      "-chardev", "socket,id=u0,path=%s" % sock_path,
                      "-serial", "chardev:u0",
                      "-drive", "file=%s,if=none,id=snapdisk,format=qcow2" % img,
                      "-qmp", "unix:%s,server=on,wait=off" % qmp_path],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                     start_new_session=True)
conn, _ = srv.accept()

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

import json
q = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for _ in range(50):
    try:
        q.connect(qmp_path); break
    except OSError:
        time.sleep(0.1)
qf = q.makefile("rw")
qf.readline()                                     # greeting
def qmp(**kw):
    qf.write(json.dumps(kw) + "\n"); qf.flush()
    while True:
        r = json.loads(qf.readline())
        if "return" in r or "error" in r:
            return r
qmp(execute="qmp_capabilities")

fails = 0
def chk(what, got, want):
    global fails
    ok = got == want
    if not ok: fails += 1
    print("   %-56s %s  (got 0x%08x, want 0x%08x)"
          % (what, "ok" if ok else "FAIL", got, want))

wr(CTRL, CTRL_RE)
wr(FIFO, FIFO_RXFE)
wr(WATER, 5 << 16)               # RXWATER = 5, so 4 bytes must NOT raise RDRF

print("\n① FOUR BYTES SIT IN THE FIFO, BELOW THE WATERMARK")
conn.sendall(b"WXYZ")
for _ in range(60):
    if rd(WATER) >> 24 == 4: break
    time.sleep(0.02)
chk("RXCOUNT = 4", rd(WATER) >> 24, 4)
chk("RDRF clear (4 is not > 5)", rd(STAT) & STAT_RDRF, 0)

print("\n② SNAPSHOT, THEN RESTORE")
r = qmp(execute="human-monitor-command", arguments={"command-line": "savevm s0"})
if "error" in r: print("   savevm error:", r); sys.exit(1)
# Prove the machine really is reloaded: drain the FIFO first, so the bytes can ONLY
# come back from the snapshot and not from a FIFO that was never touched.
for _ in range(4):
    rd(DATA)
chk("FIFO drained before loadvm (so a pass cannot be a no-op)", rd(WATER) >> 24, 0)
r = qmp(execute="human-monitor-command", arguments={"command-line": "loadvm s0"})
if "error" in r: print("   loadvm error:", r); sys.exit(1)

print("\n③ THE BUFFERED BYTES MUST COME BACK -- IN ORDER")
chk("RXCOUNT = 4 again", rd(WATER) >> 24, 4)
chk("RDRF still clear (the watermark migrated too)", rd(STAT) & STAT_RDRF, 0)
for expect in b"WXYZ":
    chk("DATA pops %r" % chr(expect), rd(DATA) & 0xFF, expect)

os.killpg(os.getpgid(p.pid), signal.SIGKILL)
for f in (sock_path, qmp_path):
    try: os.unlink(f)
    except OSError: pass
print("\n%s" % ("*** %d FAILED ***" % fails if fails else
      "PASS: the RX FIFO survives a snapshot -- the claim is now checked, not asserted"))
sys.exit(1 if fails else 0)
