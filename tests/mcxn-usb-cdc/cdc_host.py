#!/usr/bin/env python3
# Minimal usbredir host for the MCXN947 CDC-ACM gadget test: enumerate the CDC
# descriptors (2 interfaces: Communications/ACM 0x02 + CDC-Data 0x0A) and echo
# bulk data on the data endpoints (EP1).  Stands in for the i.MX93 kernel's
# cdc_acm bind; the real /dev/ttyACM bind is validated in the live pairing.
import socket, struct, sys, time

HELLO, DEVICE_CONNECT, INTERFACE_INFO, EP_INFO = 0, 1, 4, 5
CONTROL_PACKET, BULK_PACKET = 100, 101


def main(host, port):
    for _ in range(50):
        try:
            s = socket.create_connection((host, port), timeout=5); break
        except OSError:
            time.sleep(0.1)
    else:
        print("HOST: could not connect"); return 1
    s.settimeout(8)
    rb = b""

    def rx(n):
        nonlocal rb
        while len(rb) < n:
            c = s.recv(65536)
            if not c:
                raise EOFError("closed")
            rb += c
        o, rb = rb[:n], rb[n:]; return o

    def rxpkt():
        t, l, i = struct.unpack("<III", rx(12)); return t, (rx(l) if l else b"")

    def txpkt(t, i, body):
        s.sendall(struct.pack("<III", t, len(body), i) + body)

    nid = [1]

    def control(ep, req, rt, val, idx, length):
        h = struct.pack("<BBBBHHH", ep, req, rt, 0, val, idx, length)
        mid = nid[0]; nid[0] += 1
        txpkt(CONTROL_PACKET, mid, h)
        while True:
            t, b = rxpkt()
            if t == CONTROL_PACKET:
                return b[3], b[10:]

    def bulk(ep, length, data=b""):
        h = struct.pack("<BBHI", ep, 0, length & 0xFFFF, 0)
        mid = nid[0]; nid[0] += 1
        txpkt(BULK_PACKET, mid, h + data)
        while True:
            t, b = rxpkt()
            if t == BULK_PACKET:
                return b[1], b[8:]

    # 1) hello.
    t, _ = rxpkt()
    if t != HELLO:
        print("HOST: no device hello"); return 1
    txpkt(HELLO, 0, b"cdc-host".ljust(64, b"\x00"))
    print("HOST: hello exchanged")

    # 2) wait device_connect (+ interface_info/ep_info the importer requires).
    saw_ii = False
    for _ in range(200):
        t, b = rxpkt()
        if t == INTERFACE_INFO:
            saw_ii = True
        elif t == DEVICE_CONNECT:
            print("HOST: device connected speed=%d ii=%s" % (b[0], saw_ii)); break
    else:
        print("HOST: no device_connect"); return 1

    # 3) device descriptor: expect bDeviceClass = 0x02 (Communications).
    st, dev = control(0x80, 6, 0x80, 0x0100, 0, 18)
    print("HOST: GET_DESC device status=%d class=0x%02x" % (st, dev[4] if len(dev) > 4 else -1))
    if st != 0 or len(dev) != 18 or dev[4] != 0x02:
        print("HOST: not a CDC device:", dev.hex()); return 1

    # 4) SET_ADDRESS, then config (9-byte header then full).
    control(0x00, 5, 0x00, 5, 0, 0)
    st, hdr = control(0x80, 6, 0x80, 0x0200, 0, 9)
    total = hdr[2] | (hdr[3] << 8) if len(hdr) >= 4 else 0
    st, cfg = control(0x80, 6, 0x80, 0x0200, 0, 255)
    print("HOST: GET_DESC config status=%d len=%d wTotalLength=%d nIfaces=%d"
          % (st, len(cfg), total, cfg[4] if len(cfg) > 4 else -1))
    # Verify CDC shape: 2 interfaces, comm(0x02) + data(0x0A), a bulk EP1.
    if st != 0 or len(cfg) != 67 or cfg[4] != 2:
        print("HOST: bad config:", cfg.hex()); return 1
    classes = [cfg[i + 5] for i in range(len(cfg) - 8)
               if cfg[i] == 9 and cfg[i + 1] == 4]
    if 0x02 not in classes or 0x0A not in classes:
        print("HOST: missing CDC interface classes:", classes); return 1
    print("HOST: CDC interfaces present: comm(0x02) + data(0x0A)")

    # 5) SET_CONFIGURATION + a CDC control request (GET_LINE_CODING).
    st, _ = control(0x00, 9, 0x00, 1, 0, 0)
    if st != 0:
        print("HOST: SET_CONFIG failed"); return 1
    st, lc = control(0xA1, 0x21, 0xA1, 0, 0, 7)      # CDC GET_LINE_CODING
    print("HOST: GET_LINE_CODING status=%d len=%d (%s)" % (st, len(lc), lc.hex()))
    if st != 0 or len(lc) != 7:
        print("HOST: GET_LINE_CODING bad"); return 1
    print("HOST: CDC ENUMERATION OK")

    # 6) bulk data echo on the CDC data endpoints (EP1 OUT -> EP1 IN).
    for n in (1, 64, 512):
        payload = bytes((i * 5 + 1) & 0xFF for i in range(n))
        st, _ = bulk(0x01, n, payload)
        if st != 0:
            print("HOST: bulk OUT %d fail" % n); return 1
        st, echo = bulk(0x81, 512)
        if st != 0 or echo != payload:
            print("HOST: bulk echo MISMATCH at %d: %s" % (n, echo[:16].hex())); return 1
        print("HOST: CDC DATA echo %d bytes OK" % n)

    print("HOST: CDC DATA OK")
    return 0


if __name__ == "__main__":
    h = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    p = int(sys.argv[2]) if len(sys.argv) > 2 else 14791
    try:
        sys.exit(main(h, p))
    except Exception as e:
        print("HOST: error:", e); sys.exit(1)
