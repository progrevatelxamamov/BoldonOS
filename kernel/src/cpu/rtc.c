#include <cpu/rtc.h>
#include <io.h>

static inline bool rtc_update_in_progress(void) {
    outb(0x70, 0x0A);
    return (inb(0x71) & 0x80) != 0;
}

static inline uint8_t rtc_read_reg(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

void rtc_get_time(rtc_time_t *time) {
    uint8_t reg_b;
    uint8_t century = 0;

    while (rtc_update_in_progress());

    time->sec = rtc_read_reg(0x00);
    time->min = rtc_read_reg(0x02);
    time->hour = rtc_read_reg(0x04);
    time->day = rtc_read_reg(0x07);
    time->month = rtc_read_reg(0x08);
    time->year = rtc_read_reg(0x09);

    reg_b = rtc_read_reg(0x0B);

    if (!(reg_b & 0x04)) {
        time->sec = (time->sec & 0x0F) + ((time->sec / 16) * 10);
        time->min = (time->min & 0x0F) + ((time->min / 16) * 10);
        time->hour = ((time->hour & 0x0F) + (((time->hour & 0x70) / 16) * 10)) | (time->hour & 0x80);
        time->day = (time->day & 0x0F) + ((time->day / 16) * 10);
        time->month = (time->month & 0x0F) + ((time->month / 16) * 10);
        time->year = (time->year & 0x0F) + ((time->year / 16) * 10);
    }

    if (!(reg_b & 0x02) && (time->hour & 0x80)) {
        time->hour = ((time->hour & 0x7F) + 12) % 24;
    }

    if (time->year < 70) {
        time->year += 2000;
    } else {
        time->year += 1900;
    }
}