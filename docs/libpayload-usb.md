# USB backend: coreboot libpayload

## Active controller path

PCI class 0C/03 controllers are collected by `libpayload_pci_register`.
`pci.c` no longer invokes the previous `xhci_init`. After VFS, input devices,
partition hotplug and scheduler initialization, a dedicated kernel task starts
libpayload controllers in xHCI/EHCI/OHCI/UHCI order. An acquire/release boot gate
prevents controller enumeration before interrupts are enabled. There is no
silent fallback to the old driver.

The included upstream sources are pinned at
`ab1a93527cab535166bb9a3fef2d58fabdfeeb2f`; see
`kernel/third_party/libpayload/README.md`, `UPSTREAM.sha256` and `LICENSES.md`.
Original third-party license comments are preserved.

## Environment and DMA

Only upstream objects and `src/usb/lp` use the isolated shim-first headers.
Native OS integration uses the normal BoldonOS headers. Separate wrappers
handle encoded PCI addresses without changing the native PCI API. MMIO BARs
can be 64-bit; the original upstream xHCI PCI helper's 32-bit BAR restriction
is bypassed. Initial MMIO mapping covers 1 MiB. PCI memory/I/O and bus mastering
are enabled, and legacy PCI interrupt delivery is masked for polling.

Native preparation performs xHCI/EHCI BIOS ownership handoff with bounded
waits and SMI disabling, OHCI ownership acquisition, and scoped Intel switchable
port routing. libpayload handles UHCI legacy setup itself. This is still a
firmware-style polling driver, not a complete Linux PCI/power-management stack.

All upstream allocations use contiguous PMM pages below 4 GiB, with allocation
metadata and explicit HHDM virtual/physical translation. The shim never assumes
identity mapping. DMA rings, schedules, contexts and interrupt reports inherit
these limits. Controller control/bulk callbacks are wrapped with bounded,
aligned DMA buffers, including setup packets and MSC CBWs/CSWs originally
placed on the stack. Regular kernel I/O buffers need not be DMA-safe. Upstream
controller bounce-buffer paths remain enabled too.

HPET supplies microsecond delays and elapsed wall-independent time. The
fallback uses bounded busy delays and the kernel monotonic clock. The USB
backend is serialized by a recursive, scheduler-yielding lock owned by the
current **thread**, not the process. Interrupts are not disabled across USB
waits. Fatal upstream errors stop the USB worker and disable bus mastering;
subsequent disk operations return errors instead of waiting for its lock.

## Input and Nano-X

The upstream HID driver only supplies a boot keyboard, so BoldonOS installs
its own `usb_hid_init`: fetch report descriptors, select boot/report protocol,
request SET_IDLE(0), and create an interrupt-IN queue. The existing BoldonOS HID
parser and report translators are reused through opaque helpers in `xhci.c`;
that does **not** initialize the former controller backend. The old source is
still linked for these helpers and existing driver dependencies.

Keyboard/mouse events feed the existing inout system, evdev and `/dev/input/mice`.
The PS/2/ImPS/2/Explorer client protocol and Nano-X's four-byte mouse packets are
unchanged. Disconnect releases keys, modifiers and buttons. HID reports must fit
64 bytes. Keyboard LEDs use descriptor output fields or a boot report, with
SET_REPORT issued from polling rather than inside an input callback.
Interrupt NAK/idle is not treated as a timed failure and does not cause periodic
endpoint resets. Upstream's interrupt queue API does not expose actual packet
length to the consumer; it returns fixed-size report buffers.

## Storage and hotplug

Bulk-only MSC devices use the native block registry and existing periodic
partition/mount hotplug service. Supported logical sectors are power-of-two
512–4096 bytes. READ/WRITE(10) requests are split into at most 64 KiB, with
unsigned 32-bit LBAs; transport errors are propagated. Flush really issues
SYNCHRONIZE CACHE(10); devices rejecting it return an error rather than silently
claiming durability. Existing diskctl eject calls the normal flush path.

Detach marks the disk as removing, notifies partition removal, unregisters the
block device and invalidates the upstream pointer. A stable allocation is kept
as an offline tombstone because an already-entered block callback can retain
its context across unregister. This deliberately costs one small page-backed
context per removed disk; it prevents use-after-free on nested transport-error
detach. Reclaiming those contexts requires block-layer reference counting.
Late callbacks return ENODEV. A failed upstream transfer is never followed by
dereferencing its potentially freed usbdev.

## Explicit limitations

- This port has **not been tested on the reported physical PC**. Replacing the
  controller stack does not prove that its Address Device timeout is fixed.
- USB is polled by one worker, not interrupt-driven. A long control/MSC timeout
  serializes other USB activity, although the scheduler and non-USB tasks run.
- The upstream core selects one interface per device. Simple keyboard, mouse,
  hubs and bulk-only storage are supported paths; arbitrary composite devices,
  multiple HID interfaces, UAS, isochronous audio/video and USB Wi-Fi are not
  promised. Multi-LUN MSC remains the upstream single-LUN model.
- Capacity is the upstream READ CAPACITY(10) 32-bit block count, conservatively
  saturated below 2^32 blocks. READ CAPACITY(16)/READ(16) are not implemented.
- Existing `usb_net.c` and RTL8192EU drivers directly use old xHCI rings and
  endpoint structures. They are **not attached to libpayload devices**. Generic
  devices produce an explicit unclaimed-device diagnostic. Keeping the old C
  file for linkage does not provide a second active controller. Network/Wi-Fi
  transport migration is separate work, not functional support in this port.
- Other upstream firmware-driver limits and host-controller-specific hardware
  bugs remain possible. There is no universal hardware compatibility guarantee.

## Verification

Run `sh kernel/get-deps`, then `python3 tests/test_libpayload.py` and the existing
`test_hid_idle.py`, `test_mouse_protocols.py`, `test_xhci_compat.py` suites.
The new suite links the actual adapter, HID parser and upstream MSC objects into
host harnesses. It checks boot keyboard/mouse reports, indefinite idle, LED
updates, release on disconnect, geometry/range checks, nested transport detach,
late callbacks, real BOT chunking/CBW/CSW/cache-sync, unsigned high LBAs, and the
native PMM allocator's bounds/alignment/HHDM/zeroing/overflow behavior. It also
checks the PCI backend switch and boot gate.

All 186 C objects compile and aggregate with `ld -r` without duplicate symbols.
Full kernel/image build and VM boot still need NASM (and image/QEMU tools), which
were unavailable in the integration environment; a successful host harness is
not a hardware boot test.

On hardware, collect logs starting with `[BOOT] PCI scan`, `[LPUSB] initializing
PCI`, `controller ready`, HID/disk registration and the first upstream failure.
Test both controllers, hubs, long keyboard/mouse idle, unplug/replug, Nano-X,
and a disposable storage device including reads/writes/flush/eject. Do not use
important data for first write testing.

## Follow-up: repeated MSC capacity messages

Media polling now has a per-device one-second deadline, independent of HID
polling. A ready disk is checked with TEST UNIT READY only: it is not spun up
and its capacity is not rediscovered on every USB worker iteration. Empty
readers stay quiet. Readiness recovery or a failed TUR triggers the existing
initialization/capacity path; removal/reinsertion still notify block hotplug.
The regression harness checks repeated calls without time advancing, the exact
one-second deadline, quiet ready/empty states, media reinsertion, and immediate
return when transport failure detaches the USB device.

The debug xHCI startup test now sends one NOOP rather than 32. Completion code
1 is success; CRCR.CRR can be zero after the command queue has drained. The log
therefore prints the completion code and running bit separately, without calling
an idle command ring an initialization failure.

## Follow-up: EHCI cleanup panic with a nonzero HHDM

The physical-to-virtual shim must preserve zero as NULL, just as its inverse
preserves NULL as physical zero. Returning the HHDM base for physical zero
made an EHCI descriptor-chain end look like another allocation. Freeing that
false pointer inspected allocation metadata at HHDM minus 32 bytes; with HHDM
`0xFFFF800000000000`, that is the noncanonical `0xFFFF7FFFFFFFFFE0`, consistent
with a General Protection Fault rather than a page fault.

The shim now returns NULL for physical zero. EHCI cleanup also obeys the qTD
terminate bit even if ignored address bits are nonzero. Regression tests run
the actual upstream cleanup function against the native page-backed allocator:
empty chains, one/multiple descriptors and terminated nonzero links. All
allocated pages must be freed exactly once. The null-translation assertion
failed before the shim fix; the terminated-nonzero-link case still failed with
only that fix and passes with the EHCI terminate-bit fix too. This reproduces
the software defects but is not a hardware boot confirmation.

## Follow-up: repeated disabled-port reset loop

A post-reset port-enable timeout now aborts attachment before any device/EP0
requests. The generic hub previously logged the timeout and continued down the
speed/enumeration path. Repeated reset-related change notifications could keep
retrying this path without a per-port limit.

Each timed-out port now has a monotonic retry deadline: 1, 2, 4, 8, 16, then
30 seconds between failed attempts. It logs the disabled-port failure once per
failure episode instead of on each reset. The hub acknowledges status changes
while waiting; scheduled retries run even without a fresh change event. A failed
port does not abort the scan of other ports, and HID polling is not paused for
the backoff interval. Observed disconnection clears the retry state; successful
attachment clears it as well. A reconnect not observed as disconnected may wait
for the current deadline (up to 30 seconds).

The host regression invokes the real generic hub code with a fake clock and
port callbacks. It checks no EP0/device attachment on a disabled port, no repeated
reset before the deadline despite 10,000 change notifications, retries without
hub-change events, capped backoff, one warning, other-port enumeration and
reconnection. This addresses the retry/logging defect; it does not establish
why a specific physical port fails to enable or diagnose a separate panic.
