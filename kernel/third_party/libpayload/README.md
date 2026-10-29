# coreboot libpayload USB subset

Upstream: https://github.com/coreboot/coreboot/tree/ab1a93527cab535166bb9a3fef2d58fabdfeeb2f/payloads/libpayload

Pinned commit: `ab1a93527cab535166bb9a3fef2d58fabdfeeb2f`.

Included: the USB core, UHCI/OHCI/EHCI/xHCI host controllers, their root hubs,
generic hubs, USB mass storage and quirks; required USB/PCI/inttypes headers.
The upstream DWC2, HID and firmware PCI scanner/initialization sources are not
included. BoldonOS supplies its own HID and PCI integration, not a renamed
version of the former xHCI controller.

`UPSTREAM.sha256` records the original pinned source hashes. Unchanged files
match those hashes. Local changes:

- `drivers/usb/xhci.c`: cap enabled slots at the size of the USB core's device
  array minus the root-hub slot; do not use a bit mask as a numerical limit.
  Run one diagnostic NOOP instead of 32; report completion separately from the
  command-ring running bit (which can clear when the queue becomes idle).
- `drivers/usb/usbmsc.c`: use byte-sized START STOP UNIT bitfields so the
  anonymous command union remains one byte on x86-64. Compile-time assertions
  enforce CDB6/CDB10/CBW/CSW wire sizes. Export cache synchronization through
  SYNCHRONIZE CACHE(10), and chunked read/write with unsigned 32-bit LBAs.
  Rate-limit media polling to once per second; ready media use a quiet TUR,
  with spin-up/capacity discovery only on readiness recovery or command error.
- `include/usb/usbmsc.h`: per-device monotonic media-poll deadline.
- `drivers/usb/ehci.c`: descriptor cleanup stops at the qTD terminate bit
  instead of following the ignored address bits of a terminated link. The OS
  physical-address shim preserves null pointers in both directions.

The upstream files use BSD-style redistribution notices, reproduced in full
in `LICENSES.md` and retained in the sources. Include `LICENSES.md` with binary
distributions; the repository's GPL license does not replace those notices.
BoldonOS-specific environment shims are in `kernel/usb-compat`; OS bindings
are in `kernel/src/usb`. Details and limitations: `docs/libpayload-usb.md`.
