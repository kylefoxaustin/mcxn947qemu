#!/usr/bin/env python3
# Minimal usbredir *host* (importer) for the MCXN947 USB device-mode test.
#
# Speaks just enough of the usbredir wire protocol to stand in for a real USB
# host (the role i.MX93's stock `-device usb-redir` will play): hello handshake,
# wait for the device to connect, then run a standard enumeration over EP0 and
# verify the descriptors the MCX firmware returns.  Advertises NO caps, so the
# peer uses 32-bit ids and the legacy device_connect header — keeps parsing
# trivial.  Exit 0 iff enumeration succeeds and the device descriptor matches.
import socket, struct, sys, time

# usb_redir_type
HELLO, DEVICE_CONNECT, RESET = 0, 1, 3
INTERFACE_INFO, EP_INFO = 4, 5
CONTROL_PACKET, BULK_PACKET = 100, 101
SPEED_FULL = 1

EXPECT_DEV = bytes([18, 1, 0x00, 0x02, 0, 0, 0, 64,
                    0xC9, 0x1F, 0x94, 0x00, 0x00, 0x01, 0, 0, 0, 1])
EXPECT_CFG = bytes([9, 2, 32, 0, 1, 1, 0, 0x80, 50,        # configuration
                    9, 4, 0, 0, 2, 0xFF, 0, 0, 0,          # interface, 2 ep
                    7, 5, 0x01, 2, 64, 0, 0,               # EP1 OUT bulk
                    7, 5, 0x81, 2, 64, 0, 0])              # EP1 IN  bulk


def connect(host, port):
    # host containing "/" => unix socket path; else TCP host:port.
    for _ in range(50):                       # wait for qemu to listen
        try:
            if "/" in host:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.settimeout(5)
                s.connect(host)
            else:
                s = socket.create_connection((host, port), timeout=5)
            return s
        except OSError:
            time.sleep(0.1)
    return None


def main(host, port):
    sock = connect(host, port)
    if sock is None:
        print("HOST: could not connect"); return 1
    sock.settimeout(8)
    rb = b""

    def recv_exact(n):
        nonlocal rb
        while len(rb) < n:
            chunk = sock.recv(65536)
            if not chunk:
                raise EOFError("peer closed")
            rb += chunk
        out, rb = rb[:n], rb[n:]
        return out

    def recv_packet():
        t, length, pid = struct.unpack("<III", recv_exact(12))
        return t, pid, (recv_exact(length) if length else b"")

    def send_packet(t, pid, body):
        sock.sendall(struct.pack("<III", t, len(body), pid) + body)

    nxid = [1]

    def control(endpoint, request, requesttype, value, index, length):
        hdr = struct.pack("<BBBBHHH", endpoint, request, requesttype,
                          0, value, index, length)
        myid = nxid[0]; nxid[0] += 1
        send_packet(CONTROL_PACKET, myid, hdr)
        # Read packets until our control response arrives.
        while True:
            t, pid, body = recv_packet()
            if t == CONTROL_PACKET:
                status = body[3]
                return status, body[10:]

    def bulk(endpoint, length, data=b""):
        # usb_redir_bulk_packet_header (no 32bit-length cap negotiated): 8 bytes
        # = endpoint, status, length, stream_id.
        hdr = struct.pack("<BBHI", endpoint, 0, length & 0xFFFF, 0)
        myid = nxid[0]; nxid[0] += 1
        send_packet(BULK_PACKET, myid, hdr + data)
        while True:
            t, pid, body = recv_packet()
            if t == BULK_PACKET:
                status = body[1]
                return status, body[8:]

    # 1) hello handshake.
    t, _, _ = recv_packet()
    if t != HELLO:
        print("HOST: expected device hello, got type", t); return 1
    ver = b"usbredir-host-test".ljust(64, b"\x00")
    send_packet(HELLO, 0, ver)                # body = version[64], no caps
    print("HOST: hello exchanged")

    # 2) wait for device_connect (firmware enabled the controller).  A real
    #    usb-redir importer (QEMU redirect.c) requires interface_info + ep_info
    #    before device_connect; enforce that contract here too.
    saw_ii = saw_ei = False
    deadline = time.time() + 8
    while time.time() < deadline:
        t, _, body = recv_packet()
        if t == INTERFACE_INFO:
            saw_ii = True
        elif t == EP_INFO:
            saw_ei = True
        elif t == DEVICE_CONNECT:
            print("HOST: device connected, speed=%d (interface_info=%s ep_info=%s)"
                  % (body[0], saw_ii, saw_ei))
            break
    else:
        print("HOST: no device_connect"); return 1
    if not (saw_ii and saw_ei):
        print("HOST: missing interface_info/ep_info before device_connect "
              "(real importer would reject)"); return 1

    # Bus reset (usb_redir_reset) — a real host resets the bus before enumerating;
    # drives the gadget's USBRST re-init so a reused/persistent server
    # re-enumerates cleanly (exercises re-enumerability).
    send_packet(RESET, nxid[0], b""); nxid[0] += 1
    time.sleep(0.05)

    # 3) GET_DESCRIPTOR(device).
    st, dev = control(0x80, 6, 0x80, 0x0100, 0, 18)
    print("HOST: GET_DESC device status=%d len=%d" % (st, len(dev)))
    if st != 0 or dev != EXPECT_DEV:
        print("HOST: device descriptor MISMATCH:", dev.hex()); return 1

    # 4) SET_ADDRESS(5).
    st, _ = control(0x00, 5, 0x00, 5, 0, 0)
    print("HOST: SET_ADDRESS status=%d" % st)
    if st != 0:
        return 1

    # 5) GET_DESCRIPTOR(config) the way Linux does: a 9-byte header read to learn
    #    wTotalLength, then a full-length read (wLength=255).  A vendor stub that
    #    only answers an exact-size read aborts real-kernel enumeration (-EPROTO).
    st, hdr = control(0x80, 6, 0x80, 0x0200, 0, 9)
    print("HOST: GET_DESC config(9) status=%d len=%d" % (st, len(hdr)))
    if st != 0 or hdr != EXPECT_CFG[:9]:
        print("HOST: config header MISMATCH:", hdr.hex()); return 1
    st, cfg = control(0x80, 6, 0x80, 0x0200, 0, 255)
    print("HOST: GET_DESC config(255) status=%d len=%d" % (st, len(cfg)))
    # Accept either FS (64) or HS (512) bulk wMaxPacketSize — the shared host
    # serves both the FS (KHCI) and HS (ChipIdea) gadgets.  Compare everything
    # except the two wMaxPacketSize fields (offsets 22-23, 29-30).
    def cfg_ok(c):
        if st != 0 or len(c) != 32:
            return False
        skip = {22, 23, 29, 30}
        if any(c[i] != EXPECT_CFG[i] for i in range(32) if i not in skip):
            return False
        for o in (22, 29):                     # EP1 OUT / IN wMaxPacketSize
            if (c[o] | (c[o + 1] << 8)) not in (64, 512):
                return False
        return True
    if not cfg_ok(cfg):
        print("HOST: config descriptor MISMATCH:", cfg.hex()); return 1

    # 6) SET_CONFIGURATION(1).
    st, _ = control(0x00, 9, 0x00, 1, 0, 0)
    print("HOST: SET_CONFIGURATION status=%d" % st)
    if st != 0:
        return 1

    # 6b) GET_STATUS(device) — Linux issues this; a stub must return 2 bytes.
    st, status = control(0x80, 0, 0x80, 0, 0, 2)
    print("HOST: GET_STATUS status=%d len=%d" % (st, len(status)))
    if st != 0 or len(status) != 2:
        print("HOST: GET_STATUS bad:", status.hex()); return 1
    print("HOST: ENUMERATION OK")

    # 7) M2 — bulk data both directions at the endpoint's REAL max packet size:
    #    write to EP1 OUT, read the echo on EP1 IN, verify byte-for-byte.  The
    #    payload sizes are derived from EP1 wMaxPacketSize (cfg offset 22-23) so
    #    the FS gadget (64) and the HS gadget (512) each get a full-packet test.
    ep_mps = cfg[22] | (cfg[23] << 8)
    for n in sorted({1, 64, ep_mps}):
        payload = bytes((i * 7 + 3) & 0xFF for i in range(n))
        st, _ = bulk(0x01, n, payload)
        if st != 0:
            print("HOST: BULK OUT %d bytes status=%d" % (n, st)); return 1
        st, echo = bulk(0x81, ep_mps)
        if st != 0 or echo != payload:
            print("HOST: bulk echo MISMATCH at %d bytes: got %d (%s)"
                  % (n, len(echo), echo[:16].hex())); return 1
        print("HOST: BULK ECHO %d bytes OK" % n)

    print("HOST: BULK ECHO OK (max packet %d)" % ep_mps)
    return 0


if __name__ == "__main__":
    h = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    p = int(sys.argv[2]) if len(sys.argv) > 2 else 14761
    try:
        sys.exit(main(h, p))
    except Exception as e:
        print("HOST: error:", e); sys.exit(1)
