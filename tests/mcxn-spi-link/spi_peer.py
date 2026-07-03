#!/usr/bin/env python3
# SPI b2b peer over the spi-link chardev: drain/verify the MCX MOSI (0x5A stream)
# and send a framed pattern [0xA5, P0..P31] as MISO, resent across the window.
import socket, sys, time
port = int(sys.argv[1]) if len(sys.argv) > 1 else 14980
for _ in range(50):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=5); break
    except OSError:
        time.sleep(0.1)
else:
    print("peer: could not connect"); sys.exit(1)
s.settimeout(0.2)
frame = bytes([0xA5] + [((i * 3 + 5) & 0x7F) for i in range(32)])
mosi_total = 0; mosi_bad = 0
deadline = time.time() + 6.0
last_send = 0.0
while time.time() < deadline:
    now = time.time()
    if now - last_send > 0.15:
        try: s.sendall(frame)          # (re)send the MISO pattern
        except OSError: break
        last_send = now
    try:
        d = s.recv(4096)               # drain the MCX's MOSI stream
        if d:
            mosi_total += len(d)
            mosi_bad += sum(1 for b in d if b != 0x5A)
    except socket.timeout:
        pass
    except OSError:
        break
print("peer: MOSI %d bytes, %d not-0x5A" % (mosi_total, mosi_bad))
