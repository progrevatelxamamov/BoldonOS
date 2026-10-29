# Flat data descriptors and the hardware IRETQ fault

The latest hardware diagnostic reports a rejected user return with CS 0x23,
SS 0x1B, canonical user RIP/RSP and error code 0x18. The selected descriptors
are 0x00AFFB000000FFFF for code and 0x008FF3000000FFFF for data/stack. The
accessed bits have already been set by the processor.

The GDT definitions previously specified D/B=0 for both kernel and user data.
They now use the conventional flat data/stack encoding with D/B=1, L=0 and
G=1. Code descriptors retain L=1 and D/B=0. Selectors, privilege levels, limits,
bases and the IRETQ/syscall paths are unchanged. The user data descriptor
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
