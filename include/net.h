#ifndef ZENBITE_NET_H
#define ZENBITE_NET_H

#include "types.h"

#define ETH_ADDR_LEN 6
#define ETH_FRAME_MAX 1518
#define ETH_PAYLOAD_MAX (ETH_FRAME_MAX - 14)

typedef u8  mac_addr_t[ETH_ADDR_LEN];
typedef u32 ip4_addr_t;        /* network byte order */

#define ETH_TYPE_IP   0x0800
#define ETH_TYPE_ARP  0x0806

/* Big-endian helpers (kernel is little-endian, network is big-endian). */
static inline u16 htons(u16 x) { return (u16)((x >> 8) | (x << 8)); }
static inline u16 ntohs(u16 x) { return htons(x); }
static inline u32 htonl(u32 x) {
    return ((x >> 24) & 0xFF) | (((x >> 16) & 0xFF) << 8) |
           (((x >> 8) & 0xFF) << 16) | ((x & 0xFF) << 24);
}
static inline u32 ntohl(u32 x) { return htonl(x); }

/* Driver-side -------------------------------------------------------- */
struct net_iface {
    int        present;
    mac_addr_t mac;
    ip4_addr_t ip;        /* host order via the API; passed as net order
                              to the stack */
    ip4_addr_t netmask;
    ip4_addr_t gateway;
    /* Driver callbacks. */
    int        (*tx)(const void *frame, u32 len);
    int        (*poll)(void *frame, u32 max);   /* returns length or 0 */
};

void                ne2000_init(void);
void                e1000_init(void);
struct net_iface   *net_iface(void);

/* Stack entry points -------------------------------------------------- */
void net_init(void);
void net_poll(void);                 /* call from the shell idle loop */

/* IP utilities -------------------------------------------------------- */
ip4_addr_t ip4_parse(const char *s);     /* "10.0.2.15" -> packed BE */
void       ip4_format(ip4_addr_t a, char *out);   /* "10.0.2.15\0", >=16 B */

/* ARP ---------------------------------------------------------------- */
int  arp_lookup(ip4_addr_t ip, mac_addr_t out);
void arp_request(ip4_addr_t ip);
void arp_dump(void);

/* ICMP / ping -------------------------------------------------------- */
int  icmp_ping(ip4_addr_t dst, u32 timeout_ticks, u32 *rtt_ticks);

/* UDP --------------------------------------------------------------- */
int  udp_send  (ip4_addr_t dst, u16 dport, u16 sport, const void *buf, u32 len);
int  udp_recv  (u16 sport, void *buf, u32 max, ip4_addr_t *src, u16 *src_port,
                u32 timeout_ticks);

/* TCP (single blocking client connection) --------------------------- */
int  tcp_connect(ip4_addr_t dst, u16 dport);
int  tcp_send   (const void *buf, u32 len);
int  tcp_recv   (void *buf, u32 max, u32 timeout_ticks);
void tcp_close  (void);

/* DNS ---------------------------------------------------------------- */
/* Resolve a name (or pass-through a dotted-quad) to a network-order
 * IPv4 address. Returns 0 on failure. */
ip4_addr_t dns_resolve(const char *name);
void       net_set_dns(ip4_addr_t server);

/* HTTP --------------------------------------------------------------- */
int  http_get(const char *url, const char *out_path);
int  http_last_status(void);              /* HTTP status of last fetch */
const char *http_last_location(void);     /* Location: of last fetch (3xx) */

#endif
