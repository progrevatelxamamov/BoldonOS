#include <cpu/time.h>
#include <cpu/rtc.h>
#include <scheduler/spinlock.h>

static int64_t ntp_offset = 0;
static spinlock_t time_lock = SPINLOCK_INIT;

static uint64_t unix_uptime;
static uint64_t unix_uptime_offset;

static const uint16_t days_per_month[12] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
};

void init_uptime_time(void);

static bool is_leap_year(uint16_t year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static uint64_t datetime_to_unix(rtc_time_t *t) {
    uint32_t years = t->year - 1970;
    uint32_t leaps = (years + 2) / 4;
    uint32_t days = (years * 365) + leaps + days_per_month[t->month - 1] + (t->day - 1);
    
    if (t->month > 2 && is_leap_year(t->year)) {
        days++;
    }
    
    return (days * 86400) + (t->hour * 3600) + (t->min * 60) + t->sec;
}

void time_init(void) {
    uint64_t flags;
    spin_lock_irqsave(&time_lock, &flags);
    ntp_offset = 0;
    spin_unlock_irqrestore(&time_lock, flags);

    init_uptime_time();
}

void time_set_offset(int64_t offset) {
    uint64_t flags;
    spin_lock_irqsave(&time_lock, &flags);
    ntp_offset = offset;
    spin_unlock_irqrestore(&time_lock, flags);
}

uint64_t get_unix_time(void) {
    rtc_time_t t;
    rtc_get_time(&t);
    
    uint64_t flags;
    spin_lock_irqsave(&time_lock, &flags);
    uint64_t current_offset = ntp_offset;
    spin_unlock_irqrestore(&time_lock, flags);
    
    return datetime_to_unix(&t) + current_offset;
}

void init_uptime_time(void) {
    uint64_t flags;
    spin_lock_irqsave(&time_lock, &flags);
    rtc_time_t t;
    rtc_get_time(&t);

    uint64_t current_offset = ntp_offset;
    unix_uptime_offset = datetime_to_unix(&t) + current_offset;
    spin_unlock_irqrestore(&time_lock, flags);
}

uint64_t get_uptime_time(void) { return unix_uptime_offset - get_unix_time(); }