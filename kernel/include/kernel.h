#ifndef KERNEL_H
#define KERNEL_H

#include <bootloader.h>

void kstart(bootloader_t blinfo);
bootloader_t get_bootinfo(void);

#endif