#ifndef KERNEL_H
#define KERNEL_H

#include <bootloader.h>

void kstart(bootloader_t blinfo);
void enable_sse(void);
void cpu_enable_features(void);
bootloader_t get_bootinfo(void);

#endif