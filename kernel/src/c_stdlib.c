#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <gconsole.h>
#include <stdarg.h>
#include <limits.h>
#include <stdint.h>
#include <stdbool.h>

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

char *strcpy(char *dest, const char *src) {
    char *saved = dest;
    while ((*dest++ = *src++));
    return saved;
}

char *strncpy(char *dest, const char *src, size_t n) {
    char *saved = dest;
    while (n && (*dest++ = *src++)) n--;
    while (n--) *dest++ = 0;
    return saved;
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *ptr = malloc_alloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    } else {
        console_print_string("[LIB] ERROR: calloc() failed to allocate memory!\n");
    }
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) {
        return malloc_alloc(size);
    }
    if (size == 0) {
        malloc_free(ptr);
        return NULL;
    }
    void *new_ptr = malloc_alloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, size);
        malloc_free(ptr);
    } else {
        console_print_string("[LIB] ERROR: realloc() failed to allocate memory!\n");
    }
    return new_ptr;
}

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    if (nmemb < 2 || size == 0 || !base || !compar) {
        console_print_string("[LIB] qsort() early exit: invalid arguments or small array\n");
        return;
    }

    uint8_t *bytes = (uint8_t *)base;
    for (size_t gap = nmemb / 2; gap > 0; gap /= 2) {
        for (size_t i = gap; i < nmemb; i++) {
            for (size_t j = i; j >= gap && compar(bytes + (j - gap) * size, bytes + j * size) > 0; j -= gap) {
                uint8_t *a = bytes + (j - gap) * size;
                uint8_t *b = bytes + j * size;
                for (size_t k = 0; k < size; k++) {
                    uint8_t tmp = a[k];
                    a[k] = b[k];
                    b[k] = tmp;
                }
            }
        }
    }
}

int printf(const char *format, ...) {
    if (format) {
        console_print_string(format);
    }
    return 0;
}

/* Freestanding formatter used by procfs, mountinfo and disk diagnostics.
   Keep va_arg consumption aligned with the complete conversion specification:
   skipping %4u/%10llu made a later %s treat the integer 8 as a pointer. */
typedef struct {
    char *data;
    size_t capacity;
    size_t length; /* full length, including bytes truncated from output */
} format_output_t;

static void format_char(format_output_t *out, char c) {
    if (out->capacity && out->length < out->capacity - 1)
        out->data[out->length] = c;
    if (out->length < (size_t)INT_MAX) out->length++;
}

static void format_padding(format_output_t *out, char c, int count) {
    while (count-- > 0) format_char(out, c);
}

static int format_vprintf(char *dest, size_t cap, const char *fmt, va_list ap) {
    if (!fmt || (cap && !dest)) return -1;
    format_output_t out = {dest, cap, 0};
    while (*fmt) {
        if (*fmt != '%') { format_char(&out, *fmt++); continue; }
        fmt++;
        if (*fmt == '%') { format_char(&out, *fmt++); continue; }
        if (!*fmt) { format_char(&out, '%'); break; }

        bool left = false, zero = false, alt = false, plus = false, space = false;
        for (;;) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') zero = true;
            else if (*fmt == '#') alt = true;
            else if (*fmt == '+') plus = true;
            else if (*fmt == ' ') space = true;
            else break;
            fmt++;
        }
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) { left = true; width = width == INT_MIN ? INT_MAX : -width; }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                if (width < 65536) width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }
        if (width > 65536) width = 65536;
        int precision = -1;
        if (*fmt == '.') {
            precision = 0;
            fmt++;
            if (*fmt == '*') { precision = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') {
                if (precision < 65536) precision = precision * 10 + (*fmt - '0');
                fmt++;
            }
            if (precision > 65536) precision = 65536;
        }
        int length = 0; /* 0=int, 1=long, 2=long long, 3=size_t */
        if (*fmt == 'h') { fmt++; if (*fmt == 'h') fmt++; }
        else if (*fmt == 'l') { length = 1; fmt++; if (*fmt == 'l') { length = 2; fmt++; } }
        else if (*fmt == 'z' || *fmt == 't') { length = 3; fmt++; }
        char spec = *fmt;
        if (!spec) break;
        fmt++;
        if (spec == 's') {
            const char *text = va_arg(ap, const char *);
            if (!text) text = "(null)";
            size_t n = 0;
            while (text[n] && (precision < 0 || n < (size_t)precision)) n++;
            if (!left && width > (int)n) format_padding(&out, ' ', width - (int)n);
            for (size_t i = 0; i < n; i++) format_char(&out, text[i]);
            if (left && width > (int)n) format_padding(&out, ' ', width - (int)n);
            continue;
        }
        if (spec == 'c') {
            int c = va_arg(ap, int);
            if (!left) format_padding(&out, ' ', width - 1);
            format_char(&out, (char)c);
            if (left) format_padding(&out, ' ', width - 1);
            continue;
        }
        if (spec != 'd' && spec != 'i' && spec != 'u' && spec != 'x' &&
            spec != 'X' && spec != 'o' && spec != 'p') {
            format_char(&out, '%'); format_char(&out, spec);
            continue;
        }
        bool signed_num = spec == 'd' || spec == 'i';
        uint64_t value;
        char sign = 0;
        if (signed_num) {
            int64_t signed_value = length == 2 ? va_arg(ap, long long) :
                                   length == 1 ? va_arg(ap, long) :
                                   length == 3 ? va_arg(ap, ptrdiff_t) : va_arg(ap, int);
            if (signed_value < 0) { sign = '-'; value = 0ULL - (uint64_t)signed_value; }
            else { sign = plus ? '+' : space ? ' ' : 0; value = (uint64_t)signed_value; }
        } else if (spec == 'p') {
            value = (uintptr_t)va_arg(ap, void *);
            alt = true;
        } else {
            value = length == 2 ? va_arg(ap, unsigned long long) :
                    length == 1 ? va_arg(ap, unsigned long) :
                    length == 3 ? va_arg(ap, size_t) : va_arg(ap, unsigned int);
        }
        unsigned base = spec == 'o' ? 8 : spec == 'x' || spec == 'X' || spec == 'p' ? 16 : 10;
        const char *digits = spec == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
        char reverse[32];
        int n = 0;
        uint64_t original = value;
        do { reverse[n++] = digits[value % base]; value /= base; } while (value);
        if (precision == 0 && original == 0) n = 0;
        char prefix = (alt && base == 16 && (original || spec == 'p')) ? (spec == 'X' ? 'X' : 'x') : 0;
        int leading = precision > n ? precision - n : 0;
        int occupied = n + leading + (sign != 0) + (prefix ? 2 : 0);
        int pad = width > occupied ? width - occupied : 0;
        if (!left && (!zero || precision >= 0)) format_padding(&out, ' ', pad);
        if (sign) format_char(&out, sign);
        if (prefix) { format_char(&out, '0'); format_char(&out, prefix); }
        if (!left && zero && precision < 0) format_padding(&out, '0', pad);
        format_padding(&out, '0', leading);
        while (n) format_char(&out, reverse[--n]);
        if (left) format_padding(&out, ' ', pad);
    }
    if (cap) dest[out.length < cap ? out.length : cap - 1] = 0;
    return (int)out.length;
}

int snprintf(char *str, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int ret = format_vprintf(str, size, format, args);
    va_end(args);
    return ret;
}

int sprintf(char *str, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int ret = format_vprintf(str, (size_t)-1, format, args);
    va_end(args);
    return ret;
}

int fflush(FILE *stream) {
    (void)stream;
    return 0;
}

char *strcat(char *dest, const char *src) {
    char *ptr = dest;

    while (*dest != '\0') {
        dest++;
    }

    while (*src != '\0') {
        *dest = *src;
        dest++;
        src++;
    }

    *dest = '\0';

    return ptr;
}

const char* strchr(const char* str, int c) {
    while (*str != '\0') {
        if (*str == (char)c) {
            return str;
        }
        str++;
    }
    if (c == '\0') {
        return str;
    }
    return NULL;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) {
        return (char *)haystack;
    }

    while (*haystack) {
        const char *h = haystack;
        const char *n = needle;

        while (*h && *n && (*h == *n)) {
            h++;
            n++;
        }

        if (!*n) {
            return (char *)haystack;
        }

        haystack++;
    }

    return NULL;
}