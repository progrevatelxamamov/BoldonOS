#ifndef SYS_TIME_H
#define SYS_TIME_H

#include <stdint.h>

void time_init(void);
void time_set_offset(int64_t offset);
uint64_t get_unix_time(void);
uint64_t get_unix_time_ms(void);
uint64_t get_monotonic_ms(void);
uint64_t get_uptime_time(void);

#endif
