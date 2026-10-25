#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <gconsole.h>
#include <stdarg.h>

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

static void itoa(int n, char *s, int base) {
    static char digits[] = "0123456789abcdef";
    char buf[32];
    int i = 0;
    unsigned int num = (unsigned int)n;
    if (n < 0 && base == 10) {
        *s++ = '-';
        num = -n;
    }
    do {
        buf[i++] = digits[num % base];
        num /= base;
    } while (num > 0);
    while (i > 0) *s++ = buf[--i];
    *s = '\0';
}

int snprintf(char *str, size_t size, const char *format, ...) {
    if (!str || !format || size == 0) return 0;
    va_list args;
    va_start(args, format);
    size_t out_idx = 0;
    size_t max_write = size - 1;

    for (size_t i = 0; format[i] && out_idx < max_write; i++) {
        if (format[i] == '%') {
            i++;
            if (format[i] == 's') {
                const char *s = va_arg(args, const char *);
                while (s && *s && out_idx < max_write) str[out_idx++] = *s++;
            } else if (format[i] == 'd') {
                char buf[32];
                itoa(va_arg(args, int), buf, 10);
                for (int j = 0; buf[j] && out_idx < max_write; j++) str[out_idx++] = buf[j];
            } else if (format[i] == 'x' || format[i] == 'X') {
                char buf[32];
                itoa(va_arg(args, int), buf, 16);
                for (int j = 0; buf[j] && out_idx < max_write; j++) str[out_idx++] = buf[j];
            } else {
                str[out_idx++] = format[i];
            }
        } else {
            str[out_idx++] = format[i];
        }
    }
    str[out_idx] = '\0';
    va_end(args);
    return (int)out_idx;
}

int sprintf(char *str, const char *format, ...) {
    if (!str || !format) return 0;
    
    va_list args;
    va_start(args, format);
    
    int ret = snprintf(str, 65536, format, va_arg(args, void*)); 
    
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