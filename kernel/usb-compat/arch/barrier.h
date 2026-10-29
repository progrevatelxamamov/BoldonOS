#define mb() __asm__ volatile("mfence" ::: "memory")
#define rmb() __asm__ volatile("lfence" ::: "memory")
#define wmb() __asm__ volatile("sfence" ::: "memory")
