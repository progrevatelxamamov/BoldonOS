#ifndef BCACHE_H
#define BCACHE_H

#include <stdint.h>
#include <stdbool.h>

void bcache_init(void);
int bcache_read(const char *dev_name, uint64_t lba, uint64_t count, void *buf);
int bcache_write(const char *dev_name, uint64_t lba, uint64_t count, void *buf);
int bcache_flush(const char *dev_name);

#endif