#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>

/* Отключаем системные заголовки libc, которых нет в ядре */
#define LWIP_NO_CTYPE_H   1
#define LWIP_NO_STDIO_H   1
#define LWIP_NO_STDLIB_H  1
#define LWIP_NO_UNISTD_H  1

/* Реализуем ctype и stdlib функции своими силами */
#define isdigit(c)  ((c) >= '0' && (c) <= '9')
#define isxdigit(c) (((c) >= '0' && (c) <= '9') || ((c) >= 'a' && (c) <= 'f') || ((c) >= 'A' && (c) <= 'F'))
#define isspace(c)  ((c) == ' ' || (c) == '\t' || (c) == '\n' || (c) == '\r')
#define tolower(c)  (((c) >= 'A' && (c) <= 'Z') ? ((c) + ('a' - 'A')) : (c))
#define toupper(c)  (((c) >= 'a' && (c) <= 'z') ? ((c) - ('a' - 'A')) : (c))

/* Реализация atoi для преобразования портов/строк в числа */
static inline int atoi(const char *s) {
    if (!s) return 0;
    int res = 0, sign = 1;
    while (isspace(*s)) s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    while (isdigit(*s)) {
        res = res * 10 + (*s - '0');
        s++;
    }
    return res * sign;
}

/* =====================================================================
 *  FALLBACK КОДОВ ОШИБОК ERRNO
 * ===================================================================== */
#ifndef EPERM
#define EPERM            1
#endif
#ifndef ENOENT
#define ENOENT           2
#endif
#ifndef EIO
#define EIO              5
#endif
#ifndef ENOMEM
#define ENOMEM          12
#endif
#ifndef EACCES
#define EACCES          13
#endif
#ifndef EFAULT
#define EFAULT          14
#endif
#ifndef EEXIST
#define EEXIST          17
#endif
#ifndef EINVAL
#define EINVAL          22
#endif
#ifndef EAGAIN
#define EAGAIN          11
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK     EAGAIN
#endif
#ifndef EINPROGRESS
#define EINPROGRESS    115
#endif
#ifndef EALREADY
#define EALREADY       114
#endif
#ifndef ENOTSOCK
#define ENOTSOCK        88
#endif
#ifndef EDESTADDRREQ
#define EDESTADDRREQ    89
#endif
#ifndef EMSGSIZE
#define EMSGSIZE        90
#endif
#ifndef EPROTOTYPE
#define EPROTOTYPE      91
#endif
#ifndef ENOPROTOOPT
#define ENOPROTOOPT     92
#endif
#ifndef EPROTONOSUPPORT
#define EPROTONOSUPPORT 93
#endif
#ifndef EADDRINUSE
#define EADDRINUSE      98
#endif
#ifndef EADDRNOTAVAIL
#define EADDRNOTAVAIL   99
#endif
#ifndef ENETDOWN
#define ENETDOWN       100
#endif
#ifndef ENETUNREACH
#define ENETUNREACH    101
#endif
#ifndef ECONNABORTED
#define ECONNABORTED   103
#endif
#ifndef ECONNRESET
#define ECONNRESET     104
#endif
#ifndef ENOBUFS
#define ENOBUFS        105
#endif
#ifndef EISCONN
#define EISCONN        106
#endif
#ifndef ENOTCONN
#define ENOTCONN       107
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT      110
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED   111
#endif
#ifndef EHOSTUNREACH
#define EHOSTUNREACH   113
#endif

/* Выравнивание байт для x86_64 */
#define BYTE_ORDER LITTLE_ENDIAN

/* Базовые типы данных для lwIP */
typedef uint8_t   u8_t;
typedef int8_t    s8_t;
typedef uint16_t  u16_t;
typedef int16_t   s16_t;
typedef uint32_t  u32_t;
typedef int32_t   s32_t;
typedef uint64_t  u64_t;
typedef int64_t   s64_t;
typedef uintptr_t mem_ptr_t;

typedef uint16_t sa_family_t;
#define sa_family_t sa_family_t

typedef uint32_t socklen_t;
#define socklen_t socklen_t

/* Упаковка структур */
#define PACK_STRUCT_FIELD(x) x
#define PACK_STRUCT_STRUCT   __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END

/* Макросы диагностики */
extern void console_print_string(const char *str);

#define LWIP_PLATFORM_DIAG(x) do { console_print_string x; } while(0)
#define LWIP_PLATFORM_ASSERT(x) do { \
    console_print_string("[LWIP ASSERT FAILED] "); \
    console_print_string(x); \
    console_print_string("\n"); \
    while(1); \
} while(0)

#define LWIP_RAND() ((u32_t)0x12345678)

#endif /* LWIP_ARCH_CC_H */