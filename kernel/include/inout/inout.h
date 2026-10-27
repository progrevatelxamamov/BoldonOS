#ifndef INOUT_H
#define INOUT_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    INOUT_EVENT_KEY,
    INOUT_EVENT_MOUSE
} inout_event_type;

typedef enum {
    INOUT_KEY_DOWN,
    INOUT_KEY_UP
} inout_data_key_state;

typedef enum {
    INOUT_MOUSE_DOWN,
    INOUT_MOUSE_UP,
    INOUT_MOUSE_MOVE,
    INOUT_MOUSE_MOVE_ABSOLUTE,
} inout_data_mouse_state;

typedef enum {
    INOUT_MOUSE_LEFTBTN,
    INOUT_MOUSE_CENTERBTN,
    INOUT_MOUSE_RIGHTBTN,
} mouse_keys;

typedef struct {
    inout_data_key_state key_state;
    uint64_t keycode;
} inout_data_key_t;

typedef struct {
    inout_data_mouse_state mouse_state;
    mouse_keys             mouse_btn;
    int                    x;
    int                    y;
    int                    z;
} inout_data_mouse_t;

typedef struct {
    inout_event_type type;
    union {
        inout_data_key_t key;
        inout_data_mouse_t mouse;
    } data;
} inout_event_t;

enum {
    KEY_ESC        = 0x01,
    KEY_1          = 0x02,
    KEY_2          = 0x03,
    KEY_3          = 0x04,
    KEY_4          = 0x05,
    KEY_5          = 0x06,
    KEY_6          = 0x07,
    KEY_7          = 0x08,
    KEY_8          = 0x09,
    KEY_9          = 0x0A,
    KEY_0          = 0x0B,
    KEY_MINUS      = 0x0C,
    KEY_EQUALS     = 0x0D,
    KEY_BACKSPACE  = 0x0E,
    KEY_TAB        = 0x0F,
    KEY_Q          = 0x10,
    KEY_W          = 0x11,
    KEY_E          = 0x12,
    KEY_R          = 0x13,
    KEY_T          = 0x14,
    KEY_Y          = 0x15,
    KEY_U          = 0x16,
    KEY_I          = 0x17,
    KEY_O          = 0x18,
    KEY_P          = 0x19,
    KEY_LBRACKET   = 0x1A,
    KEY_RBRACKET   = 0x1B,
    KEY_ENTER      = 0x1C,
    KEY_LCTRL      = 0x1D,
    KEY_A          = 0x1E,
    KEY_S          = 0x1F,
    KEY_D          = 0x20,
    KEY_F          = 0x21,
    KEY_G          = 0x22,
    KEY_H          = 0x23,
    KEY_J          = 0x24,
    KEY_K          = 0x25,
    KEY_L          = 0x26,
    KEY_SEMICOLON  = 0x27,
    KEY_APOSTROPHE = 0x28,
    KEY_GRAVE      = 0x29,
    KEY_LSHIFT     = 0x2A,
    KEY_BACKSLASH  = 0x2B,
    KEY_Z          = 0x2C,
    KEY_X          = 0x2D,
    KEY_C          = 0x2E,
    KEY_V          = 0x2F,
    KEY_B          = 0x30,
    KEY_N          = 0x31,
    KEY_M          = 0x32,
    KEY_COMMA      = 0x33,
    KEY_DOT        = 0x34,
    KEY_SLASH      = 0x35,
    KEY_RSHIFT     = 0x36,
    KEY_KP_STAR    = 0x37,
    KEY_LALT       = 0x38,
    KEY_SPACE      = 0x39,
    KEY_CAPSLOCK   = 0x3A,
    KEY_F1         = 0x3B,
    KEY_F2         = 0x3C,
    KEY_F3         = 0x3D,
    KEY_F4         = 0x3E,
    KEY_F5         = 0x3F,
    KEY_F6         = 0x40,
    KEY_F7         = 0x41,
    KEY_F8         = 0x42,
    KEY_F9         = 0x43,
    KEY_F10        = 0x44,
    KEY_NUMLOCK    = 0x45,
    KEY_SCROLLLOCK = 0x46,
    KEY_KP_7       = 0x47,
    KEY_KP_8       = 0x48,
    KEY_KP_9       = 0x49,
    KEY_KP_MINUS   = 0x4A,
    KEY_KP_4       = 0x4B,
    KEY_KP_5       = 0x4C,
    KEY_KP_6       = 0x4D,
    KEY_KP_PLUS    = 0x4E,
    KEY_KP_1       = 0x4F,
    KEY_KP_2       = 0x50,
    KEY_KP_3       = 0x51,
    KEY_KP_0       = 0x52,
    KEY_KP_DOT     = 0x53,
    KEY_F11        = 0x57,
    KEY_F12        = 0x58,

    KEY_EXT_BASE       = 0x80,
    KEY_RCTRL          = 0x80,
    KEY_RALT           = 0x81,
    KEY_KP_ENTER       = 0x82,
    KEY_KP_SLASH       = 0x83,
    KEY_HOME           = 0x84,
    KEY_UP             = 0x85,
    KEY_PGUP           = 0x86,
    KEY_LEFT           = 0x87,
    KEY_RIGHT          = 0x88,
    KEY_END            = 0x89,
    KEY_DOWN           = 0x8A,
    KEY_PGDN           = 0x8B,
    KEY_INSERT         = 0x8C,
    KEY_DELETE         = 0x8D,
    KEY_LGUI           = 0x8E,
    KEY_RGUI           = 0x8F,
    KEY_MENU           = 0x90,
    KEY_PRINTSCREEN    = 0x91,
    KEY_PAUSE          = 0x92,

    KEY_MAX            = 256
};

typedef struct {
    bool down[KEY_MAX];
    bool caps_lock;
    bool num_lock;
    bool scroll_lock;
    bool shift_pressed;
} keyboard_state_t;

typedef struct {
    bool leftbtn_pressed;
    bool centerbtn_pressed;
    bool rightbtn_pressed;
    int x, y, z;
} mouse_state_t;

extern volatile keyboard_state_t g_keyboard;
extern volatile mouse_state_t    g_mouse;

static const char keymap_normal[KEY_MAX] = {
    [KEY_ESC]        = 0x1B, [KEY_BACKSPACE]  = '\b', [KEY_TAB]        = '\t',
    [KEY_ENTER]      = '\n', [KEY_SPACE]      = ' ',  [KEY_GRAVE]      = '`',
    [KEY_1]          = '1',  [KEY_2]          = '2',  [KEY_3]          = '3',
    [KEY_4]          = '4',  [KEY_5]          = '5',  [KEY_6]          = '6',
    [KEY_7]          = '7',  [KEY_8]          = '8',  [KEY_9]          = '9',
    [KEY_0]          = '0',  [KEY_MINUS]      = '-',  [KEY_EQUALS]     = '=',
    [KEY_Q] = 'q', [KEY_W] = 'w', [KEY_E] = 'e', [KEY_R] = 'r', [KEY_T] = 't',
    [KEY_Y] = 'y', [KEY_U] = 'u', [KEY_I] = 'i', [KEY_O] = 'o', [KEY_P] = 'p',
    [KEY_LBRACKET]   = '[',  [KEY_RBRACKET]   = ']',  [KEY_BACKSLASH]  = '\\',
    [KEY_A] = 'a', [KEY_S] = 's', [KEY_D] = 'd', [KEY_F] = 'f', [KEY_G] = 'g',
    [KEY_H] = 'h', [KEY_J] = 'j', [KEY_K] = 'k', [KEY_L] = 'l',
    [KEY_SEMICOLON]  = ';',  [KEY_APOSTROPHE] = '\'',
    [KEY_Z] = 'z', [KEY_X] = 'x', [KEY_C] = 'c', [KEY_V] = 'v', [KEY_B] = 'b',
    [KEY_N] = 'n', [KEY_M] = 'm', [KEY_COMMA]      = ',',  [KEY_DOT]        = '.',
    [KEY_SLASH]      = '/',
    [KEY_KP_0] = '0', [KEY_KP_1] = '1', [KEY_KP_2] = '2', [KEY_KP_3] = '3',
    [KEY_KP_4] = '4', [KEY_KP_5] = '5', [KEY_KP_6] = '6', [KEY_KP_7] = '7',
    [KEY_KP_8] = '8', [KEY_KP_9] = '9', [KEY_KP_DOT] = '.', [KEY_KP_PLUS] = '+',
    [KEY_KP_MINUS] = '-', [KEY_KP_STAR] = '*', [KEY_KP_SLASH] = '/', [KEY_KP_ENTER] = '\n',
};

static const char keymap_shift[KEY_MAX] = {
    [KEY_ESC]        = 0x1B, [KEY_BACKSPACE]  = '\b', [KEY_TAB]        = '\t',
    [KEY_ENTER]      = '\n', [KEY_SPACE]      = ' ',  [KEY_GRAVE]      = '~',
    [KEY_1] = '!', [KEY_2] = '@', [KEY_3] = '#', [KEY_4] = '$', [KEY_5] = '%',
    [KEY_6] = '^', [KEY_7] = '&', [KEY_8] = '*', [KEY_9] = '(', [KEY_0] = ')',
    [KEY_MINUS]      = '_',  [KEY_EQUALS]     = '+',
    [KEY_Q] = 'Q', [KEY_W] = 'W', [KEY_E] = 'E', [KEY_R] = 'R', [KEY_T] = 'T',
    [KEY_Y] = 'Y', [KEY_U] = 'U', [KEY_I] = 'I', [KEY_O] = 'O', [KEY_P] = 'P',
    [KEY_LBRACKET]   = '{',  [KEY_RBRACKET]   = '}',  [KEY_BACKSLASH]  = '|',
    [KEY_A] = 'A', [KEY_S] = 'S', [KEY_D] = 'D', [KEY_F] = 'F', [KEY_G] = 'G',
    [KEY_H] = 'H', [KEY_J] = 'J', [KEY_K] = 'K', [KEY_L] = 'L',
    [KEY_SEMICOLON]  = ':',  [KEY_APOSTROPHE] = '"',
    [KEY_Z] = 'Z', [KEY_X] = 'X', [KEY_C] = 'C', [KEY_V] = 'V', [KEY_B] = 'B',
    [KEY_N] = 'n', [KEY_M] = 'M', [KEY_COMMA]      = '<',  [KEY_DOT]        = '>',
    [KEY_SLASH]      = '?',
    [KEY_KP_0] = '0', [KEY_KP_1] = '1', [KEY_KP_2] = '2', [KEY_KP_3] = '3',
    [KEY_KP_4] = '4', [KEY_KP_5] = '5', [KEY_KP_6] = '6', [KEY_KP_7] = '7',
    [KEY_KP_8] = '8', [KEY_KP_9] = '9', [KEY_KP_DOT] = '.', [KEY_KP_PLUS] = '+',
    [KEY_KP_MINUS] = '-', [KEY_KP_STAR] = '*', [KEY_KP_SLASH] = '/', [KEY_KP_ENTER] = '\n',
};

static const char keymap_caps[KEY_MAX] = {
    [KEY_Q] = 'Q', [KEY_W] = 'W', [KEY_E] = 'E', [KEY_R] = 'R', [KEY_T] = 'T',
    [KEY_Y] = 'Y', [KEY_U] = 'U', [KEY_I] = 'I', [KEY_O] = 'O', [KEY_P] = 'P',
    [KEY_A] = 'A', [KEY_S] = 'S', [KEY_D] = 'D', [KEY_F] = 'F', [KEY_G] = 'G',
    [KEY_H] = 'H', [KEY_J] = 'J', [KEY_K] = 'K', [KEY_L] = 'L',
    [KEY_Z] = 'Z', [KEY_X] = 'X', [KEY_C] = 'C', [KEY_V] = 'V', [KEY_B] = 'B',
    [KEY_N] = 'N', [KEY_M] = 'M',
};

void inout_init(void);
void inout_add_event(inout_event_t ev);
void inout_call_scheduler(void);
void update_keyboard_leds(void);

/* Принудительно выставить состояние клавиши (используется USB HID для
   сверки всего состояния на каждом репорте). Состояние g_keyboard.down[]
   меняется только здесь, больше никто его не трогает. */
void inout_set_key_state(uint64_t keycode, bool down);

#endif