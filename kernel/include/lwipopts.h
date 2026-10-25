#ifndef LWIP_HDR_LWIPOPTS_H
#define LWIP_HDR_LWIPOPTS_H

/* =====================================================================
 *  ОСНОВНЫЕ НАСТРОЙКИ СИСТЕМЫ И МНОГОПОТОЧНОСТИ
 * ===================================================================== */

/* 0 = Использовать операционную систему (многопоточный режим tcpip_thread) */
#define NO_SYS                      0

/* Защита критических секций вызовом системных блокировок */
#define SYS_LIGHTWEIGHT_PROT        1

/* Выравнивание памяти для x86_64 (8 байт) */
#define MEM_ALIGNMENT               8

/* Размер внутренней кучи lwIP (2 МБ вполне хватит для сетевых буферов) */
#define MEM_SIZE                    (2 * 1024 * 1024)

/* =====================================================================
 *  ПОДДЕРЖКА ПРОТОКОЛОВ (IPv4, TCP, UDP, ICMP, ARP, DHCP)
 * ===================================================================== */

#define LWIP_IPV4                   1
#define LWIP_IPV6                   0

#define LWIP_ARP                    1
#define ARP_TABLE_SIZE              10
#define IP_REASS_MAX_PBUFS          10

#define LWIP_ICMP                   1   /* Для работы ping */
#define LWIP_RAW                    1

#define LWIP_UDP                    1
#define UDP_TTL                     255

#define LWIP_TCP                    1
#define TCP_TTL                     255
#define TCP_WND                     (16 * 1024)
#define TCP_MSS                     1460
#define TCP_SND_BUF                 (32 * 1024)
#define TCP_SND_QUEUELEN            (8 * TCP_SND_BUF / TCP_MSS)

/* Автоматическое получение IP через DHCP */
#define LWIP_DHCP                   1
#define LWIP_AUTOIP                 0

/* DNS для преобразования доменов (google.com -> IP) */
#define LWIP_DNS                    1
#define DNS_MAX_SERVERS             2

/* =====================================================================
 *  СОКЕТЫ И NETCONN API (для интеграции с Linux syscalls)
 * ===================================================================== */

#define LWIP_NETCONN                1
#define LWIP_SOCKET                 1

/* Отключаем переопределение стандартных типов (socklen_t и т.д.), 
   так как они уже есть в твоем ядре */
#define LWIP_TIMEVAL_PRIVATE        1
#define LWIP_SOCKET_SET_ERRNO       1

/* Не переименовываем функции сокетов в lwip_socket (мы сделаем обертку сами) */
#define LWIP_COMPAT_SOCKETS         0
#define LWIP_POSIX_SOCKETS_IO_NAMES 0

/* =====================================================================
 *  БУФЕРЫ (PBUF)
 * ===================================================================== */

#define PBUF_POOL_SIZE              512
#define PBUF_POOL_BUFSIZE           1536

/* =====================================================================
 *  ПАРАМЕТРЫ ПОТОКА TCPIP THREAD (ПОТОК СТЕКА)
 * ===================================================================== */

#define TCPIP_THREAD_NAME           "lwip_tcpip"
#define TCPIP_THREAD_STACKSIZE      32768
#define TCPIP_THREAD_PRIO           1

#define TCPIP_MBOX_SIZE             256
#define DEFAULT_RAW_RECVMBOX_SIZE   64
#define DEFAULT_UDP_RECVMBOX_SIZE   64
#define DEFAULT_TCP_RECVMBOX_SIZE   128
#define DEFAULT_ACCEPTMBOX_SIZE     64

/* Статистика и отладка (по желанию можно выключить) */
#define LWIP_STATS                  1
#define LWIP_STATS_DISPLAY          0

#define MEMP_NUM_TCP_SEG            256
#define MEMP_NUM_TCP_PCB            32
#define MEMP_NUM_TCP_PCB_LISTEN     16

#endif /* LWIP_HDR_LWIPOPTS_H */