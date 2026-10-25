#ifndef LINUX_INPUT_H
#define LINUX_INPUT_H

#include <stdint.h>

struct timeval {
    uint64_t tv_sec;
    uint64_t tv_usec;
};

struct input_event {
    struct timeval time;
    uint16_t type;
    uint16_t code;
    int32_t value;
};

#define EV_SYN          0x00
#define EV_KEY          0x01
#define EV_REL          0x02
#define EV_ABS          0x03
#define EV_MSC          0x04
#define EV_SW           0x05
#define EV_LED          0x11
#define EV_SND          0x12
#define EV_REP          0x14
#define EV_FF           0x15
#define EV_PWR          0x16
#define EV_FF_STATUS    0x17
#define EV_MAX          0x1f

#define REL_X           0x00
#define REL_Y           0x01
#define REL_Z           0x02
#define REL_HWHEEL      0x06
#define REL_DIAL        0x08
#define REL_WHEEL       0x08
#define REL_MISC        0x09
#define REL_MAX         0x0f

#define BTN_MOUSE       0x110
#define BTN_LEFT        0x110
#define BTN_RIGHT       0x111
#define BTN_MIDDLE      0x112
#define BTN_SIDE        0x113
#define BTN_EXTRA       0x114

#define EVIOCGVERSION   0x80044501
#define EVIOCGID        0x80084502
#define EVIOCGNAME(len) (0x80004506 | ((len) << 16))
#define EVIOCGBIT(ev,len) (0x80004520 | ((len) << 16) | ((ev) << 8))

struct input_id {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
};

#endif