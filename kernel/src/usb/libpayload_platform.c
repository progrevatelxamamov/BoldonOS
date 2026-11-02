#include <usb/libpayload.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <kernel.h>
#include <memory.h>
#include <string.h>
#include <stdarg.h>
#include <gconsole.h>
#include <cpu/acpi.h>
#include <cpu/time.h>
#include <scheduler/scheduler.h>
#include <scheduler/spinlock.h>

#define LP_ALLOC_MAGIC 0x4C50444D41414C4CULL
struct lp_allocation { uint64_t magic, phys, pages, size; };
struct lp_controller { uint32_t pci; uint8_t type; uintptr_t bar; };
static struct lp_controller controllers[32];
static unsigned controller_count;
static volatile bool usb_ready;
static volatile uintptr_t usb_owner;
static unsigned usb_depth;
static volatile int usb_failed;
uint16_t pci_read_config16(uint32_t dev, uint16_t reg);
void pci_write_config16(uint32_t dev, uint16_t reg, uint16_t value);
static spinlock_t lp_pci_lock = SPINLOCK_INIT;

extern void lp_backend_initialize(void);
extern void lp_backend_poll(void);

uintptr_t lp_virt_to_phys(const volatile void *ptr) {
    uint64_t v = (uint64_t)ptr;
    uint64_t offset = get_bootinfo().hhdm->offset;
    if (!ptr) return 0;
    if (v >= offset && v - offset < (1ULL << 47)) return v - offset;
    return vmm_get_phys(get_kernel_pml4(), v & ~4095ULL) | (v & 4095);
}

void *lp_phys_to_virt(uintptr_t phys) {
    if (!phys) return NULL;
    return (void *)(phys + get_bootinfo().hhdm->offset);
}

void *lp_memalign(size_t align, size_t size) {
    if (align < 16) align = 16;
    if ((align & (align - 1)) || align > 0x100000 || size > 0x4000000) return NULL;
    size_t pages = (size + align + sizeof(struct lp_allocation) + 4095) / 4096;
    uint64_t phys = (uint64_t)pmm_alloc_pages_below(pages, 0x100000000ULL);
    if (!phys) return NULL;
    uintptr_t base = (uintptr_t)lp_phys_to_virt(phys);
    uintptr_t ptr = (base + sizeof(struct lp_allocation) + align - 1) & ~(align - 1);
    struct lp_allocation *meta = (void *)(ptr - sizeof(*meta));
    *meta = (struct lp_allocation){LP_ALLOC_MAGIC, phys, pages, size};
    memset((void *)ptr, 0, size);
    return (void *)ptr;
}

void *lp_malloc(size_t size) { return lp_memalign(16, size); }

void *lp_calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) return NULL;
    return lp_malloc(count * size);
}

void lp_free(void *ptr) {
    if (!ptr) return;
    struct lp_allocation *meta = (void *)((uintptr_t)ptr - sizeof(*meta));
    if (meta->magic != LP_ALLOC_MAGIC) {
        console_print_string("[LPUSB] invalid allocator pointer\n");
        return;
    }
    uint64_t phys = meta->phys, pages = meta->pages;
    meta->magic = 0;
    pmm_free_pages((void *)phys, pages);
}

void lp_udelay(unsigned us) {
    uint64_t start = hpet_read_counter();
    uint64_t ticks = hpet_ticks_for_ms(1) * us / 1000;
    if (ticks) {
        while (hpet_read_counter() - start < ticks) __asm__ volatile("pause");
    } else {
        for (uint64_t n = 0; n < (uint64_t)us * 32; n++) __asm__ volatile("pause");
    }
}

struct lp_timeval { long tv_sec, tv_usec; };
int lp_gettimeofday(struct lp_timeval *tv, void *tz) {
    (void)tz;
    if (!tv) return -1;
    uint64_t ticks = hpet_ticks_for_ms(1);
    uint64_t ms = ticks ? hpet_read_counter() / ticks : get_monotonic_ms();
    tv->tv_sec = ms / 1000;
    tv->tv_usec = (ms % 1000) * 1000;
    return 0;
}

int lp_vprintf(const char *fmt, va_list ap) {
    char out[1024];
    size_t used = 0;
    while (*fmt && used < sizeof(out) - 1) {
        if (*fmt != '%') { out[used++] = *fmt++; continue; }
        fmt++;
        bool zero = false;
        bool alternate = false;
        if (*fmt == '#') { alternate = true; fmt++; }
        if (*fmt == '0') { zero = true; fmt++; }
        unsigned width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (width > 64) width = 64;
        int length = 0;
        if (*fmt == 'z') { length = 1; fmt++; }
        while (*fmt == 'l') { length++; fmt++; }
        char c = *fmt;
        if (!c) break;
        fmt++;
        if (c == '%') { out[used++] = '%'; continue; }
        if (c == 's') {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            while (*str && used < sizeof(out) - 1) out[used++] = *str++;
            continue;
        }
        if (c == 'c') { out[used++] = va_arg(ap, int); continue; }
        uint64_t value;
        bool neg = false;
        if (c == 'd' || c == 'i') {
            int64_t v = length >= 2 ? va_arg(ap, long long) : length ? va_arg(ap, long) : va_arg(ap, int);
            neg = v < 0;
            value = neg ? 0ULL - (uint64_t)v : (uint64_t)v;
        } else if (c == 'p') {
            value = (uintptr_t)va_arg(ap, void *);
        } else if (c == 'u' || c == 'x' || c == 'X' || c == 'o') {
            value = length >= 2 ? va_arg(ap, unsigned long long) : length ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
        } else {
            out[used++] = '?';
            continue;
        }
        unsigned base = c == 'x' || c == 'X' || c == 'p' ? 16 : c == 'o' ? 8 : 10;
        const char *digits = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
        char number[65];
        unsigned n = 0;
        if ((alternate || c == 'p') && base == 16 && used < sizeof(out) - 3) {
            out[used++] = '0'; out[used++] = 'x';
        }
        do { number[n++] = digits[value % base]; value /= base; } while (value);
        if (neg && used < sizeof(out) - 1) out[used++] = '-';
        while (width > n + (unsigned)neg && used < sizeof(out) - 1) { out[used++] = zero ? '0' : ' '; width--; }
        while (n && used < sizeof(out) - 1) out[used++] = number[--n];
    }
    out[used] = 0;
    console_print_string(out);
    return (int)used;
}

int lp_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = lp_vprintf(fmt, ap);
    va_end(ap);
    return n;
}

__attribute__((noreturn)) void lp_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    lp_vprintf(fmt, ap);
    va_end(ap);
    console_print_string("[LPUSB] fatal backend error; USB worker stopped\n");
    for (unsigned i = 0; i < controller_count; i++) {
        uint32_t dev = controllers[i].pci;
        pci_write_config16(dev, 4, pci_read_config16(dev, 4) & ~4u);
    }
    __atomic_store_n(&usb_failed, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&usb_owner, 0, __ATOMIC_RELEASE);
    for (;;) { task_sleep(get_current_process()); schedule_yield(); }
}

uint32_t pci_read_config32(uint32_t dev, uint16_t reg) {
    uint64_t flags;
    spin_lock_irqsave(&lp_pci_lock, &flags);
    uint32_t value = pci_read_config((dev >> 16) & 255, (dev >> 11) & 31, (dev >> 8) & 7, reg);
    spin_unlock_irqrestore(&lp_pci_lock, flags);
    return value;
}
uint16_t pci_read_config16(uint32_t dev, uint16_t reg) { return pci_read_config32(dev, reg) >> ((reg & 2) * 8); }
uint8_t pci_read_config8(uint32_t dev, uint16_t reg) { return pci_read_config32(dev, reg) >> ((reg & 3) * 8); }
void pci_write_config32(uint32_t dev, uint16_t reg, uint32_t value) {
    uint64_t flags;
    spin_lock_irqsave(&lp_pci_lock, &flags);
    pci_write_config((dev >> 16) & 255, (dev >> 11) & 31, (dev >> 8) & 7, reg, value);
    spin_unlock_irqrestore(&lp_pci_lock, flags);
}
void pci_write_config16(uint32_t dev, uint16_t reg, uint16_t value) {
    uint64_t flags;
    spin_lock_irqsave(&lp_pci_lock, &flags);
    uint8_t b = dev >> 16, s = (dev >> 11) & 31, f = (dev >> 8) & 7;
    uint32_t old = pci_read_config(b, s, f, reg);
    uint32_t shift = (reg & 2) * 8;
    if ((reg & ~3) == 4) old = 0;
    pci_write_config(b, s, f, reg, (old & ~(0xFFFFu << shift)) | ((uint32_t)value << shift));
    spin_unlock_irqrestore(&lp_pci_lock, flags);
}
void pci_write_config8(uint32_t dev, uint16_t reg, uint8_t value) {
    uint64_t flags;
    spin_lock_irqsave(&lp_pci_lock, &flags);
    uint8_t b = dev >> 16, s = (dev >> 11) & 31, f = (dev >> 8) & 7;
    uint32_t old = pci_read_config(b, s, f, reg), shift = (reg & 3) * 8;
    pci_write_config(b, s, f, reg, (old & ~(255u << shift)) | ((uint32_t)value << shift));
    spin_unlock_irqrestore(&lp_pci_lock, flags);
}

void libpayload_pci_register(pci_device_t *dev) {
    if (controller_count == 32) return;
    if (dev->prog_if != 0 && dev->prog_if != 0x10 && dev->prog_if != 0x20 && dev->prog_if != 0x30) return;
    uintptr_t bar = dev->bars[0] & ~15ULL;
    if ((dev->bars[0] & 6) == 4) bar |= (uint64_t)dev->bars[1] << 32;
    if (dev->prog_if != 0 && (!bar || (dev->bars[0] & 1))) return;
    controllers[controller_count++] = (struct lp_controller){
        0x80000000u | (dev->bus << 16) | (dev->slot << 11) | (dev->func << 8), dev->prog_if, bar
    };
}

int lp_platform_controller(unsigned index, uint32_t *pci, uint8_t *type, uintptr_t *bar) {
    if (index >= controller_count) return 0;
    *pci = controllers[index].pci; *type = controllers[index].type; *bar = controllers[index].bar;
    return 1;
}

void lp_platform_prepare(uint32_t dev, uint8_t type, uintptr_t bar) {
    uint16_t command = pci_read_config16(dev, 4);
    pci_write_config16(dev, 4, command | 4 | 0x400 | (type ? 2 : 1));
    if (type) {
        for (uintptr_t off = 0; off < 0x100000; off += 4096)
            vmm_map_page(get_kernel_pml4(), (bar & ~4095ULL) + off,
                         (uintptr_t)lp_phys_to_virt(bar & ~4095ULL) + off,
                         PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT);
    }
    if (type == 0x30) {
        uint32_t id = pci_read_config32(dev, 0);
        bool routing = false;
        if ((id & 65535) == 0x8086) {
            switch (id >> 16) {
            case 0x1E31: case 0x8C31: case 0x8CB1: case 0x8D31:
            case 0x9C31: case 0x9CB1: routing = true; break;
            }
        }
        if (routing && pci_read_config32(dev, 0x2C) != 0x90A8104D) {
            uint32_t usb2 = pci_read_config32(dev, 0xD4), usb3 = pci_read_config32(dev, 0xDC);
            if (usb2 != UINT32_MAX) pci_write_config32(dev, 0xD0, usb2);
            if (usb3 != UINT32_MAX) pci_write_config32(dev, 0xD8, usb3);
        }
        volatile uint32_t *cap = lp_phys_to_virt(bar);
        uint32_t off = (cap[4] >> 16) * 4;
        for (unsigned n = 0; off && off < 0xFFFF0 && n < 256; n++) {
            volatile uint32_t *ext = lp_phys_to_virt(bar + off);
            uint32_t value = ext[0];
            if ((value & 255) == 1) {
                ext[0] = value | (1u << 24);
                for (unsigned ms = 0; ms < 1000 && (ext[0] & (1u << 16)); ms++) lp_udelay(1000);
                ext[0] = (ext[0] | (1u << 24)) & ~(1u << 16);
                ext[1] = (ext[1] & ((7u << 1) | (255u << 5) | (7u << 17))) | (7u << 29);
            }
            off += ((value >> 8) & 255) * 4;
            if (!((value >> 8) & 255)) break;
        }
    } else if (type == 0x10) {
        volatile uint32_t *op = lp_phys_to_virt(bar);
        if (op[1] & (1u << 8)) {
            op[2] = 1u << 3;
            for (unsigned ms = 0; ms < 500 && (op[1] & (1u << 8)); ms++) lp_udelay(1000);
            op[1] &= ~(1u << 8);
        }
        op[5] = UINT32_MAX;
        if (op[0] & (1u << 8)) op[0x100 / 4] = 0;
    } else if (type == 0x20) {
        volatile uint32_t *cap = lp_phys_to_virt(bar);
        uint32_t off = (cap[2] >> 8) & 255;
        for (unsigned n = 0; off >= 0x40 && n < 48; n++) {
            uint32_t value = pci_read_config32(dev, off);
            if ((value & 255) == 1) {
                pci_write_config32(dev, off, value | (1u << 24));
                for (unsigned ms = 0; ms < 1000 && (pci_read_config32(dev, off) & (1u << 16)); ms++) lp_udelay(1000);
                pci_write_config32(dev, off, (pci_read_config32(dev, off) | (1u << 24)) & ~(1u << 16));
                pci_write_config32(dev, off + 4, 0xE0000000);
                break;
            }
            off = (value >> 8) & 255;
        }
    }
}

int lp_usb_failed(void) { return __atomic_load_n(&usb_failed, __ATOMIC_ACQUIRE); }

void lp_usb_lock(void) {
    uintptr_t owner = (uintptr_t)get_current_thread();
    if (!owner) lp_fatal("USB I/O without scheduler thread\n");
    if (__atomic_load_n(&usb_owner, __ATOMIC_ACQUIRE) == owner && owner) { usb_depth++; return; }
    for (;;) {
        uintptr_t expected = 0;
        if (__atomic_compare_exchange_n(&usb_owner, &expected, owner, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
        schedule_yield();
    }
    usb_depth = 1;
}
void lp_usb_unlock(void) {
    if (--usb_depth == 0) __atomic_store_n(&usb_owner, 0, __ATOMIC_RELEASE);
}

static void libpayload_task(void) {
    while (!__atomic_load_n(&usb_ready, __ATOMIC_ACQUIRE)) schedule_yield();
    lp_usb_lock();
    lp_backend_initialize();
    lp_usb_unlock();
    for (;;) {
        lp_usb_lock();
        if (!lp_usb_failed()) lp_backend_poll();
        lp_usb_unlock();
        schedule_yield();
    }
}
void libpayload_usb_start(void) { create_task_kernel((void *)libpayload_task, "libpayload-usb"); }
void libpayload_usb_ready(void) { __atomic_store_n(&usb_ready, true, __ATOMIC_RELEASE); }

static uint8_t lp_leds;
void libpayload_usb_set_leds(uint8_t leds) { __atomic_store_n(&lp_leds, leds, __ATOMIC_RELEASE); }
uint8_t lp_platform_leds(void) { return __atomic_load_n(&lp_leds, __ATOMIC_ACQUIRE); }
