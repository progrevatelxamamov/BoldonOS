#include <cpu/time.h>
#include <cpu/rtc.h>
#include <scheduler/spinlock.h>

extern uint64_t g_ticks_ms;

static int64_t ntp_offset = 0;
static spinlock_t time_lock = SPINLOCK_INIT;
static uint64_t wall_base_sec = 0;
static uint64_t wall_base_ms = 0;
static bool wall_ready = false;

static const uint16_t days_per_month[12] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
};

static bool is_leap_year(uint16_t year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static uint64_t datetime_to_unix(rtc_time_t *t) {
    if (!t || t->month < 1 || t->month > 12 || t->day < 1 || t->day > 31 ||
        t->hour > 23 || t->min > 59 || t->sec > 59 || t->year < 1970) {
        return 0;
    }

    uint32_t years = t->year - 1970;
    uint32_t leaps = (years + 2) / 4;
    uint32_t days = (years * 365) + leaps + days_per_month[t->month - 1] + (t->day - 1);

    if (t->month > 2 && is_leap_year(t->year)) {
        days++;
    }

    return (days * 86400ULL) + (t->hour * 3600ULL) + (t->min * 60ULL) + t->sec;
}

static void capture_wall_base(void) {
    rtc_time_t t;
    rtc_get_time(&t);

    uint64_t base_sec = datetime_to_unix(&t);
    uint64_t base_ms = get_monotonic_ms();

    uint64_t flags;
    spin_lock_irqsave(&time_lock, &flags);
    wall_base_sec = base_sec;
    wall_base_ms = base_ms;
    wall_ready = base_sec != 0;
    spin_unlock_irqrestore(&time_lock, flags);
}

void init_uptime_time(void) {
    capture_wall_base();
}

void time_init(void) {
    uint64_t flags;
    spin_lock_irqsave(&time_lock, &flags);
    ntp_offset = 0;
    spin_unlock_irqrestore(&time_lock, flags);

    capture_wall_base();
}

void time_set_offset(int64_t offset) {
    uint64_t flags;
    spin_lock_irqsave(&time_lock, &flags);
    ntp_offset = offset;
    spin_unlock_irqrestore(&time_lock, flags);
}

uint64_t get_monotonic_ms(void) {
    return g_ticks_ms;
}

uint64_t get_unix_time_ms(void) {
    uint64_t base_sec;
    uint64_t base_ms;
    int64_t offset;
    bool ready;

    uint64_t flags;
    spin_lock_irqsave(&time_lock, &flags);
    base_sec = wall_base_sec;
    base_ms = wall_base_ms;
    offset = ntp_offset;
    ready = wall_ready;
    spin_unlock_irqrestore(&time_lock, flags);

    if (!ready) {
        rtc_time_t t;
        rtc_get_time(&t);
        return datetime_to_unix(&t) * 1000ULL;
    }

    uint64_t now_ms = get_monotonic_ms();
    uint64_t elapsed_ms = now_ms >= base_ms ? now_ms - base_ms : 0;
    int64_t unix_ms = (int64_t)(base_sec * 1000ULL) + (int64_t)elapsed_ms + offset * 1000LL;

    return unix_ms < 0 ? 0 : (uint64_t)unix_ms;
}

uint64_t get_unix_time(void) {
    return get_unix_time_ms() / 1000ULL;
}

uint64_t get_uptime_time(void) {
    return get_monotonic_ms() / 1000ULL;
}
