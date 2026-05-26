#ifndef CANBOOT_LWIPOPTS_H
#define CANBOOT_LWIPOPTS_H

/*
 * lwIP configuration for canboot. NO_SYS=1 mode: single-threaded raw API,
 * driven from kmain's main loop via sys_check_timeouts() + hal_net_pump().
 * Memory comes from picolibc's malloc (MEM_LIBC_MALLOC=1).
 */

#define NO_SYS                          1
#define LWIP_TIMERS                     1
#define SYS_LIGHTWEIGHT_PROT            0

/* Use picolibc malloc instead of lwIP's pools. Simpler footprint. */
#define MEM_LIBC_MALLOC                 1
#define MEMP_MEM_MALLOC                 1
#define MEM_USE_POOLS                   0
#define MEMP_USE_CUSTOM_POOLS           0
#define MEM_ALIGNMENT                   8
#define MEM_SIZE                        (64 * 1024)

/* Protocols */
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ICMP                       1
#define LWIP_IGMP                       0
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_RAW                        1
#define LWIP_DHCP                       1

/* DNS resolver over UDP. dns.c is already in the lwIP core source list;
 * lwip_init() calls dns_init(), and DHCP fills in the offered DNS server
 * (SLIRP hands out 10.0.2.3). Retries ride sys_check_timeouts(), which
 * the net pump loops already call. */
#define LWIP_DNS                        1
#define DNS_TABLE_SIZE                  4
#define DNS_MAX_SERVERS                 2
#define DNS_MAX_NAME_LENGTH             256
#define DNS_DOES_NAME_CHECK             1
#define MEMP_NUM_UDP_PCB                8

/* lwIP needs a randomness source for DNS query IDs + UDP source ports. */
unsigned int canboot_lwip_rand(void);
#define LWIP_RAND()                     (canboot_lwip_rand())

/* lwIP's threaded/socket APIs need an OS - disable in NO_SYS. */
#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0

/* Hostname/SNMP/PPP off. */
#define LWIP_NETIF_HOSTNAME             0
#define LWIP_SNMP                       0
#define PPP_SUPPORT                     0

/* Disable lwIP debug to keep serial log tight in CI. */
#define LWIP_DEBUG                      0
#define LWIP_STATS                      0
#define LWIP_STATS_DISPLAY              0

/* Buffer / window sizing. Modest but enough for HTTP GET + UDP echo. */
#define PBUF_POOL_SIZE                  16
#define PBUF_POOL_BUFSIZE               1536
#define TCP_MSS                         1460
#define TCP_WND                         (4 * TCP_MSS)
#define TCP_SND_BUF                     (4 * TCP_MSS)
#define TCP_SND_QUEUELEN                (4 * (TCP_SND_BUF / TCP_MSS))

/* DHCP shouldn't block - just start and check status. */
#define LWIP_DHCP_DOES_ACD_CHECK        0
#define LWIP_DHCP_CHECK_LINK_UP         0
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1

/* Etharp queue length. */
#define ARP_QUEUEING                    1

/* Don't use checksum offload - virtio-net doesn't validate without
 * negotiating VIRTIO_NET_F_GUEST_CSUM, which we skip. */
#define CHECKSUM_GEN_IP                 1
#define CHECKSUM_GEN_TCP                1
#define CHECKSUM_GEN_UDP                1
#define CHECKSUM_GEN_ICMP               1
#define CHECKSUM_CHECK_IP               1
#define CHECKSUM_CHECK_TCP              1
#define CHECKSUM_CHECK_UDP              1
#define CHECKSUM_CHECK_ICMP             1

#endif /* CANBOOT_LWIPOPTS_H */
