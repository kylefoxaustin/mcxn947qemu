# mcxn-usb-link — i.MX93 ↔ MCX USB link (M4)

The MCX is the USB **device**; the i.MX93 is the USB **host** running stock
Linux. They connect over a usbredir socket — **zero model coupling** on the
i.MX side (stock `-device usb-redir`).

## Roles (locked with 93emulator + holobench)

- **MCX = usbredir server / listener** (this repo).
- **i.MX93 = client** (`-device usb-redir`, `server=off`, `reconnect-ms=2000`).
- Gadget: **vendor stub first** (operator's call) — prove Linux enumerates the
  device; CDC-ACM (`/dev/ttyACM0` + real bytes) is the follow-on.

## Bring-up

MCX side (this repo):

    tests/mcxn-usb-link/serve.sh hs      # or 'fs' for the full-speed controller

i.MX93 side (stock QEMU, that repo):

    -chardev socket,id=ur0,path=/tmp/holo-usb-imx93-mcx.sock,server=off,reconnect-ms=2000 \
    -device usb-redir,chardev=ur0

Then in the i.MX93 Linux guest, the kernel should enumerate the device
(`dmesg | grep -i usb`, `lsusb` → VID:PID 1FC9:0094).

## Device-end readiness (verified standalone)

Both controllers are data-path-proven against a self-contained usbredir host
(`../mcxn-usb/usbredir_host.py`, controller-agnostic):

- USBFS0 (full-speed, KHCI):  `tests/mcxn-usb`     — enum + bulk echo.
- USBHS1 (high-speed, ChipIdea): `tests/mcxn-usb-hs` — enum + bulk echo.

The host mimics the real-Linux sequence (9-byte + full-length config reads,
GET_STATUS). Remaining real-kernel quirks (string descriptors, device_qualifier)
surface only against 93's actual Linux host — that's the live pairing step.
