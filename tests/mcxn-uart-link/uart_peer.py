#!/usr/bin/env python3
# UART b2b peer: connect to the MCX LPUART2 socket, send a GO byte (handshake),
# then echo every payload byte back.
import socket, sys, time
port = int(sys.argv[1]) if len(sys.argv) > 1 else 14990
for _ in range(50):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=5); break
    except OSError:
        time.sleep(0.1)
else:
    print("peer: could not connect"); sys.exit(1)
s.settimeout(8)
time.sleep(0.2)                 # let the guest arm its RX
s.sendall(b"G")                # GO: releases the firmware's transmit
n = 0
try:
    while n < 32:
        d = s.recv(64)
        if not d:
            break
        s.sendall(d)           # echo the payload
        n += len(d)
except Exception as e:
    print("peer:", e)
print("peer: echoed %d bytes" % n)
