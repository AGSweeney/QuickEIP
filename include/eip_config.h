/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* QuickEIP build-time configuration.
 *
 * Every value here has a default. To change one, either pass -D on the
 * compiler command line or put the overrides in your own header and build
 * with -DEIP_CONFIG_FILE=\"my_eip_config.h\" (that file is included first).
 */
#ifndef EIP_CONFIG_H_
#define EIP_CONFIG_H_

#ifdef EIP_CONFIG_FILE
#include EIP_CONFIG_FILE
#endif

/* Largest encapsulation packet sent or received, in bytes. It sizes the
 * scanner session buffers (3 x this), the adapter server buffers and each
 * adapter TCP client buffer. 1536 holds a full symbol-list page. Explicit
 * messages are limited to about 500 bytes regardless (unconnected UCMM).
 * The minimum is 1024: the scanner stages a request of up to 480 bytes at
 * offset 544 of EipSession.scratch. */
#ifndef EIP_MAX_PACKET
#define EIP_MAX_PACKET 1536
#endif
#if EIP_MAX_PACKET < 1024
#error "EIP_MAX_PACKET must be at least 1024"
#endif

/* Adapter: maximum number of assembly instances (consumed + produced + config). */
#ifndef EIP_ADAPTER_MAX_ASSEMBLIES
#define EIP_ADAPTER_MAX_ASSEMBLIES 8
#endif

/* Adapter: bytes shared by all assembly data. Each assembly takes its size
 * from this pool. A Class 1 assembly is limited to about 500 bytes. */
#ifndef EIP_ADAPTER_DATA_POOL
#define EIP_ADAPTER_DATA_POOL 512
#endif

/* Adapter: concurrent TCP (explicit messaging) sessions. Each costs about
 * EIP_MAX_PACKET bytes of RAM. */
#ifndef EIP_ADAPTER_MAX_CLIENTS
#define EIP_ADAPTER_MAX_CLIENTS 4
#endif

/* Adapter: a TCP session silent this long may be evicted when every slot is
 * busy and a new client connects. The I/O owner's session is never evicted. */
#ifndef EIP_ADAPTER_EVICT_MS
#define EIP_ADAPTER_EVICT_MS 60000u
#endif

/* Adapter: UDP 44818 requests (ListIdentity, ListServices) are rate limited by
 * a token bucket: one token per request, one added every
 * EIP_ADAPTER_UDP_REPLY_MS, at most EIP_ADAPTER_UDP_REPLY_BURST stored. A
 * broadcast storm cannot starve I/O. */
#ifndef EIP_ADAPTER_UDP_REPLY_MS
#define EIP_ADAPTER_UDP_REPLY_MS 20u
#endif
#ifndef EIP_ADAPTER_UDP_REPLY_BURST
#define EIP_ADAPTER_UDP_REPLY_BURST 20u
#endif

/* Adapter: the longest one eip_server_service() call waits for socket
 * activity when no T->O packet is due sooner. */
#ifndef EIP_ADAPTER_IDLE_WAIT_MS
#define EIP_ADAPTER_IDLE_WAIT_MS 5u
#endif

/* Adapter task settings, in the RTOS's own units; 0 lets the port pick its
 * default. ESP-IDF uses both (stack in bytes). NetBurner uses only the
 * priority. POSIX uses neither. */
#ifndef EIP_ADAPTER_TASK_STACK
#define EIP_ADAPTER_TASK_STACK 0
#endif
#ifndef EIP_ADAPTER_TASK_PRIORITY
#define EIP_ADAPTER_TASK_PRIORITY 0
#endif

#endif /* EIP_CONFIG_H_ */
