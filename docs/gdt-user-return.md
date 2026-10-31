# Flat data descriptors and the hardware IRETQ fault

The old-PC photo was initially misread: the rejected frame shows **CS 0x23,
SS 0x18** (not 0x1B), with error code 0x18. CS has RPL=3 but SS has RPL=0;
IRETQ cannot return to CPL3 with that SS. The displayed *fault handler*'s
CS 0x08 and SS 0x10 are both CPL0 and belong to a different frame. The GDT
entry at 0x18 may have DPL=3, but the selector itself must still have RPL=3
(`0x1B`). The #GP error code `0x18` encodes the GDT index without selector
RPL bits, so it can be reported for either SS=0x18 or SS=0x1B. It does not
prove which full selector the CPU attempted to load.

The current source had already specified `USER_DATA | 3` for `sys_execve` and
the compiled object pushed 0x1B. We now encode the initial SS push explicitly
as `pushq $0x1B` and verify that immediate in the built object. A boot marker
`[EXEC] IRET target SS=0x1B` helps distinguish a newly built image from an old
kernel/ISO. If the marker appears but the rejected frame still has SS=0x18,
the cause is elsewhere (a different IRET path or frame corruption); record the
full new photo before changing the GDT. The follow-up photo has that boot
marker, but the rejected frame's SS still appears to be 0x18 (the blurred
photo does not resolve the last digit conclusively), with #GP(0x18) on CPU 2.
The marker only proves that the new code reached the logging call, **not** the
value present in the stack frame at IRETQ. The next hardware photo confirmed
`target SS RPL=0` and `active-frame SS=0x18 RPL=0`, with active CR3 matching
process CR3. Its CODE bytes immediately before `iretq` are the ISR epilogue
(`pop r14; pop r15; add rsp,16; iretq`), **not** the `sys_execve` initial IRET.
The scheduler can restore a user frame containing bare `USER_DATA` (0x18).
It now repairs only this exact user-CS/user-SS combination to 0x1B and updates
the saved frame. Look for `[SCHED] repaired user SS 0x18 -> 0x1B` on the next
boot. This is a targeted workaround; the source of the lost RPL bits and
successful boot on the old PC remain to be confirmed. After the workaround,
the old PC produced repeated short messages, then a kernel #GP and a fast
reboot. The repair message is now emitted only on the first occurrence. A
kernel-mode #GP (even while a user process is current) now prints a short
`[GP-HALT]` line with RIP, ERR, CS, RSP and CPU and halts with interrupts
disabled instead of trying to exit the user process or run the lengthy panic
dump. This sacrifices the full dump for a stable photo; a reboot that still
occurs before `[GP-HALT]` suggests an earlier nested/double/triple fault or
external reset, not a confirmed cause of the original SS problem.

The GDT definitions previously specified D/B=0 for both kernel and user data.
They now use the conventional flat data/stack encoding with D/B=1, L=0 and
G=1. Code descriptors retain L=1 and D/B=0. That earlier change did not
alter selector values or the IRETQ/syscall paths; the explicit initial-IRETQ
SS immediate described above is a separate follow-up. The user data descriptor
should appear as 0x00CFF2000000FFFF, or 0x00CFF3000000FFFF after access.

This is a narrow descriptor-normalization/compatibility change to try on the
reported hardware. D/B is generally ignored for data segments in 64-bit mode;
the photo does not prove that this bit caused the fault. No hardware-specific
erratum or successful boot has been established. Keep the IRET diagnostics and
save the exact ELF when testing. If the fault persists, compare the new target
frame and descriptors rather than assuming that USB caused the user return
failure.

`python3 tests/test_gdt.py` compiles the actual descriptor-construction function
and checks all four exact encodings, data/stack D/B and L bits, and user selectors.
The old definitions fail this regression; this tests encoding, not CPU IRETQ
execution. Full boot verification is still required.
