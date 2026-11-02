#ifndef BOLDON_XORGTRACE_H
#define BOLDON_XORGTRACE_H
#include <stdint.h>

/* Read-only, bounded Xorg bring-up trace. No pointers to userspace retained. */
void xorgtrace_register(void);
void xorgtrace_stage(const char *stage, uint64_t pid, uint64_t detail);
uint64_t xorgtrace_syscall_enter(uint64_t pid, uint64_t num,
                                uint64_t a1, uint64_t a2);
void xorgtrace_syscall_exit(uint64_t ticket, int64_t result);
int xorgtrace_exec_target(const char *path);
#endif
