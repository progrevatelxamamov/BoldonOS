#ifndef BOLDON_LP_ENV_H
#define BOLDON_LP_ENV_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <io.h>
#include <libpayload-config.h>
#include <arch/virtual.h>
#include <arch/barrier.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
#define USB_DEBUG 1
#define __packed __attribute__((packed))
#define __aligned(n) __attribute__((aligned(n)))
#define __printf(a,b) __attribute__((format(__printf__,a,b)))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define ALIGN_UP(x,a) (((x) + (a) - 1) & ~((a) - 1))
#define IS_ALIGNED(x,a) (!((uintptr_t)(x) & ((a) - 1)))
#define DIV_ROUND_UP(x,d) (((x) + (d) - 1) / (d))
#define BIT(x) (1UL << (x))
#define xhci_init lp_xhci_init
#define xhci_reset_endpoint lp_xhci_reset_endpoint
#define xhci_set_tr_dequeue lp_xhci_set_tr_dequeue
#define printf lp_printf
#define vprintf lp_vprintf
int lp_printf(const char *fmt, ...) __printf(1,2);
int lp_vprintf(const char *fmt, va_list ap);
void lp_fatal(const char *fmt, ...) __attribute__((noreturn));
#define fatal lp_fatal
#define assert(x) do { if (!(x)) lp_fatal("libpayload assertion: %s\n", #x); } while (0)
void lp_udelay(unsigned us);
#define udelay lp_udelay
#define mdelay(ms) lp_udelay((unsigned)(ms) * 1000)
#define dma_memalign lp_memalign
#define dma_malloc lp_malloc
static inline bool dma_initialized(void) { return true; }
static inline bool dma_coherent(const void *p) { (void)p; return false; }
static inline void *xzalloc(size_t n) { void *p = lp_calloc(1,n); if (!p) lp_fatal("libpayload allocation failed\n"); return p; }
static inline void *xmalloc(size_t n) { void *p = lp_malloc(n); if (!p) lp_fatal("libpayload allocation failed\n"); return p; }
typedef long time_t;
#define div_round_up(x,d) DIV_ROUND_UP(x,d)
static inline uint32_t read32(const volatile void *p) { return *(const volatile uint32_t *)p; }
struct timeval { long tv_sec, tv_usec; };
int lp_gettimeofday(struct timeval *tv, void *tz);
#define gettimeofday lp_gettimeofday
#endif
