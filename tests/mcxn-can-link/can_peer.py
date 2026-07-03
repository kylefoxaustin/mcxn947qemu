#!/usr/bin/env python3
# CAN b2b peer: connect to the can-host-chardev socket, verify the MCX frame
# (std ID 0x321, payload DE AD BE EF CA FE BA BE) and reply (std ID 0x322,
# payload 11 22 33 44 55 66 77 88).  Wire = raw qemu_can_frame (72 bytes:
# <IBB2x64s> = can_id, can_dlc, flags, pad, data[64]).
import socket, struct, sys, time
port = int(sys.argv[1]) if len(sys.argv) > 1 else 14970
FRAME = 72
def pack(can_id, data8):
    return struct.pack("<IBB2x64s", can_id, len(data8), 0, bytes(data8).ljust(64, b"\0"))
reply = pack(0x322, [0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88])
mcx_expect = bytes([0xDE,0xAD,0xBE,0xEF,0xCA,0xFE,0xBA,0xBE])
for _ in range(50):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=5); break
    except OSError:
        time.sleep(0.1)
else:
    print("peer: could not connect"); sys.exit(1)
s.settimeout(0.15)
buf = b""; rx_ok = 0; rx_bad = 0; last = 0.0
deadline = time.time() + 6.0
while time.time() < deadline:
    now = time.time()
    if now - last > 0.1:
        try: s.sendall(reply)
        except OSError: break
        last = now
    try:
        d = s.recv(4096)
        if not d: break
        buf += d
        while len(buf) >= FRAME:
            fr, buf = buf[:FRAME], buf[FRAME:]
            can_id, dlc, flags = struct.unpack("<IBB", fr[:6])
            data = fr[8:8+8]
            if (can_id & 0x7FF) == 0x321 and data == mcx_expect:
                rx_ok += 1
            elif (can_id & 0x7FF) == 0x321:
                rx_bad += 1
    except socket.timeout:
        pass
    except OSError:
        break
print("peer: MCX frames ok=%d bad=%d" % (rx_ok, rx_bad))
