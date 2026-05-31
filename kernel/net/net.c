/* Compact IPv4 stack: ARP + IP + ICMP + UDP + a single-connection blocking
 * TCP client + http_get. Sits on top of struct net_iface (e1000 or
 * ne2000). All multi-byte fields on the wire are big-endian; helpers
 * htons/ntohs/htonl/ntohl come from include/net.h.
 *
 * Design notes:
 *   - One ARP cache (8 entries), no expiry. Good for a v0.3 toy.
 *   - No IP fragmentation, no IP options.
 *   - One TCP connection at a time, blocking API (tcp_connect / tcp_send
 *     / tcp_recv / tcp_close). Tiny send/receive queue, simple ACK every
 *     in-order segment, no congestion control. Retransmit on PIT timeout.
 *   - http_get parses a URL of the form "http://A.B.C.D[:port]/path" and
 *     writes the body (after the blank line) into a destination file via
 *     fs_create / fs_write.
 */

#include "net.h"
#include "kernel.h"
#include "kio.h"
#include "string.h"
#include "fs.h"
#include "types.h"

extern u32 pit_ticks(void);

/* --- Wire-format headers -------------------------------------------- */
struct eth_hdr {
    u8  dst[6];
    u8  src[6];
    u16 type;
} __attribute__((packed));

struct arp_hdr {
    u16 htype, ptype;
    u8  hlen, plen;
    u16 op;
    u8  sha[6];
    u32 spa;
    u8  tha[6];
    u32 tpa;
} __attribute__((packed));

struct ipv4_hdr {
    u8  vihl;       /* version<<4 | ihl */
    u8  tos;
    u16 tot_len;
    u16 id;
    u16 frag_off;
    u8  ttl;
    u8  proto;
    u16 checksum;
    u32 src;
    u32 dst;
} __attribute__((packed));

struct icmp_hdr {
    u8  type;
    u8  code;
    u16 checksum;
    u16 id;
    u16 seq;
} __attribute__((packed));

struct udp_hdr {
    u16 sport;
    u16 dport;
    u16 length;
    u16 checksum;
} __attribute__((packed));

struct tcp_hdr {
    u16 sport;
    u16 dport;
    u32 seq;
    u32 ack;
    u8  data_off;   /* upper 4 bits = header words */
    u8  flags;
    u16 window;
    u16 checksum;
    u16 urg_ptr;
} __attribute__((packed));

#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

#define ETH_TYPE_IPV4 0x0800
#define ETH_TYPE_ARP_W 0x0806

#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

/* --- Internet checksum ---------------------------------------------- */
static u16 ip_checksum(const void *data, u32 len) {
    const u8 *p = data;
    u32 sum = 0;
    while (len >= 2) {
        sum += ((u32)p[0] << 8) | p[1];
        p += 2; len -= 2;
    }
    if (len) sum += (u32)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((u16)~sum);
}

/* --- ARP cache ------------------------------------------------------- */
#define ARP_N 8
static struct {
    u32        ip;
    mac_addr_t mac;
    int        valid;
} arp_cache[ARP_N];

static int arp_find(u32 ip, mac_addr_t out) {
    for (int i = 0; i < ARP_N; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(out, arp_cache[i].mac, 6);
            return 1;
        }
    }
    return 0;
}

static void arp_insert(u32 ip, const u8 mac[6]) {
    for (int i = 0; i < ARP_N; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    }
    for (int i = 0; i < ARP_N; i++) {
        if (!arp_cache[i].valid) {
            arp_cache[i].valid = 1;
            arp_cache[i].ip = ip;
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    }
    /* Overwrite slot 0. */
    arp_cache[0].valid = 1;
    arp_cache[0].ip = ip;
    memcpy(arp_cache[0].mac, mac, 6);
}

/* --- Low-level send -------------------------------------------------- */
static void eth_send(const u8 dst[6], u16 ethertype,
                     const void *payload, u32 len) {
    struct net_iface *n = net_iface();
    if (!n || !n->present) return;
    static u8 frame[ETH_FRAME_MAX];
    struct eth_hdr *h = (struct eth_hdr *)frame;
    memcpy(h->dst, dst, 6);
    memcpy(h->src, n->mac, 6);
    h->type = htons(ethertype);
    if (len > sizeof frame - 14) len = sizeof frame - 14;
    memcpy(frame + 14, payload, len);
    n->tx(frame, 14 + len);
}

void arp_request(ip4_addr_t target_ip) {
    struct net_iface *n = net_iface();
    if (!n) return;
    struct arp_hdr a;
    a.htype = htons(1);
    a.ptype = htons(ETH_TYPE_IPV4);
    a.hlen = 6;
    a.plen = 4;
    a.op = htons(1);
    memcpy(a.sha, n->mac, 6);
    a.spa = n->ip;
    memset(a.tha, 0, 6);
    a.tpa = target_ip;
    static const u8 bcast[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
    eth_send(bcast, ETH_TYPE_ARP_W, &a, sizeof a);
}

int arp_lookup(ip4_addr_t ip, mac_addr_t out) {
    return arp_find(ip, out);
}

void arp_dump(void) {
    kputs("ARP cache:\n");
    for (int i = 0; i < ARP_N; i++) {
        if (!arp_cache[i].valid) continue;
        u32 v = ntohl(arp_cache[i].ip);
        const u8 *m = arp_cache[i].mac;
        kprintf("  %u.%u.%u.%u  %02x:%02x:%02x:%02x:%02x:%02x\n",
                (v>>24)&0xFF, (v>>16)&0xFF, (v>>8)&0xFF, v&0xFF,
                m[0], m[1], m[2], m[3], m[4], m[5]);
    }
}

/* --- ARP / IP input -------------------------------------------------- */
static void arp_input(const u8 *pkt, u32 len) {
    if (len < sizeof(struct arp_hdr)) return;
    const struct arp_hdr *a = (const struct arp_hdr *)pkt;
    if (ntohs(a->ptype) != ETH_TYPE_IPV4) return;
    arp_insert(a->spa, a->sha);
    struct net_iface *n = net_iface();
    if (!n) return;
    /* Reply to ARP request for our IP. */
    if (ntohs(a->op) == 1 && a->tpa == n->ip) {
        struct arp_hdr r;
        r.htype = htons(1);
        r.ptype = htons(ETH_TYPE_IPV4);
        r.hlen = 6; r.plen = 4;
        r.op = htons(2);
        memcpy(r.sha, n->mac, 6);
        r.spa = n->ip;
        memcpy(r.tha, a->sha, 6);
        r.tpa = a->spa;
        eth_send(a->sha, ETH_TYPE_ARP_W, &r, sizeof r);
    }
}

/* Build + send an IPv4 packet. payload is the L4 payload; proto = 1/6/17.
 * Picks a destination MAC via ARP (gateway if not on local subnet). */
static int ip_send(ip4_addr_t dst, u8 proto, const void *payload, u32 len) {
    struct net_iface *n = net_iface();
    if (!n) return -1;
    static u8 buf[ETH_FRAME_MAX - 14];
    if (len + sizeof(struct ipv4_hdr) > sizeof buf) return -1;

    struct ipv4_hdr *h = (struct ipv4_hdr *)buf;
    h->vihl = 0x45;
    h->tos = 0;
    h->tot_len = htons((u16)(sizeof(struct ipv4_hdr) + len));
    static u16 ip_id;
    h->id = htons(++ip_id);
    h->frag_off = 0;
    h->ttl = 64;
    h->proto = proto;
    h->checksum = 0;
    h->src = n->ip;
    h->dst = dst;
    h->checksum = ip_checksum(h, sizeof *h);
    memcpy(buf + sizeof *h, payload, len);

    /* Pick next-hop MAC. */
    u32 next_hop = ((dst ^ n->ip) & n->netmask) ? n->gateway : dst;
    mac_addr_t mac;
    int tries = 5;
    while (!arp_find(next_hop, mac) && tries-- > 0) {
        arp_request(next_hop);
        u32 t0 = pit_ticks();
        while (pit_ticks() - t0 < 50) net_poll();
    }
    if (!arp_find(next_hop, mac)) return -1;

    eth_send(mac, ETH_TYPE_IPV4, buf, sizeof *h + len);
    return (int)len;
}

/* --- ICMP ping ------------------------------------------------------- */
static volatile int icmp_reply_received;
static volatile u32 icmp_reply_id, icmp_reply_seq;

static void icmp_input(u32 src, const u8 *pkt, u32 len) {
    if (len < sizeof(struct icmp_hdr)) return;
    const struct icmp_hdr *ic = (const struct icmp_hdr *)pkt;
    if (ic->type == 8) {
        /* echo request -> respond */
        static u8 reply[1500];
        u32 paylen = len;
        if (paylen > sizeof reply) paylen = sizeof reply;
        memcpy(reply, pkt, paylen);
        struct icmp_hdr *r = (struct icmp_hdr *)reply;
        r->type = 0;
        r->checksum = 0;
        r->checksum = ip_checksum(r, paylen);
        ip_send(src, IPPROTO_ICMP, reply, paylen);
    } else if (ic->type == 0) {
        /* echo reply */
        icmp_reply_id  = ntohs(ic->id);
        icmp_reply_seq = ntohs(ic->seq);
        icmp_reply_received = 1;
    }
}

int icmp_ping(ip4_addr_t dst, u32 timeout_ticks, u32 *rtt_ticks) {
    static u8 pkt[16];
    struct icmp_hdr *ic = (struct icmp_hdr *)pkt;
    static u16 ping_seq;
    ping_seq++;
    ic->type = 8; ic->code = 0; ic->checksum = 0;
    ic->id = htons(0x1234); ic->seq = htons(ping_seq);
    memset(pkt + 8, 0, 8);
    ic->checksum = ip_checksum(pkt, sizeof pkt);

    icmp_reply_received = 0;
    u32 t0 = pit_ticks();
    if (ip_send(dst, IPPROTO_ICMP, pkt, sizeof pkt) < 0) return -1;
    while (pit_ticks() - t0 < timeout_ticks) {
        net_poll();
        if (icmp_reply_received) {
            if (rtt_ticks) *rtt_ticks = pit_ticks() - t0;
            return 0;
        }
    }
    return -1;
}

/* --- UDP ------------------------------------------------------------- */
static volatile u16 udp_listen_port;
static volatile u32 udp_recv_src;
static volatile u16 udp_recv_sport;
static u8 udp_recv_buf[1500];
static volatile u32 udp_recv_len;
static volatile int udp_recv_ready;

int udp_send(ip4_addr_t dst, u16 dport, u16 sport,
             const void *buf, u32 len) {
    static u8 pkt[1500];
    if (len + sizeof(struct udp_hdr) > sizeof pkt) return -1;
    struct udp_hdr *h = (struct udp_hdr *)pkt;
    h->sport = htons(sport);
    h->dport = htons(dport);
    h->length = htons((u16)(sizeof *h + len));
    h->checksum = 0;
    memcpy(pkt + sizeof *h, buf, len);
    return ip_send(dst, IPPROTO_UDP, pkt, sizeof *h + len);
}

int udp_recv(u16 sport, void *buf, u32 max, ip4_addr_t *src, u16 *src_port,
             u32 timeout_ticks) {
    udp_listen_port = sport;
    udp_recv_ready = 0;
    u32 t0 = pit_ticks();
    while (pit_ticks() - t0 < timeout_ticks) {
        net_poll();
        if (udp_recv_ready) {
            u32 n = udp_recv_len;
            if (n > max) n = max;
            memcpy(buf, (void *)udp_recv_buf, n);
            if (src) *src = udp_recv_src;
            if (src_port) *src_port = udp_recv_sport;
            udp_recv_ready = 0;
            udp_listen_port = 0;
            return (int)n;
        }
    }
    udp_listen_port = 0;
    return -1;
}

static void udp_input(u32 src, const u8 *pkt, u32 len) {
    if (len < sizeof(struct udp_hdr)) return;
    const struct udp_hdr *h = (const struct udp_hdr *)pkt;
    u16 dport = ntohs(h->dport);
    if (udp_listen_port != dport || udp_recv_ready) return;
    u32 plen = (u32)ntohs(h->length);
    if (plen < sizeof *h || plen > len) return;
    u32 payload = plen - sizeof *h;
    if (payload > sizeof udp_recv_buf) payload = sizeof udp_recv_buf;
    memcpy((void *)udp_recv_buf, pkt + sizeof *h, payload);
    udp_recv_len = payload;
    udp_recv_src = src;
    udp_recv_sport = ntohs(h->sport);
    udp_recv_ready = 1;
}

/* --- TCP (single blocking client connection) ------------------------- */
enum tcp_state {
    TCP_S_CLOSED = 0, TCP_S_SYN_SENT, TCP_S_ESTABLISHED,
    TCP_S_FIN_WAIT, TCP_S_CLOSE_WAIT, TCP_S_CLOSED_BY_PEER,
};

struct tcp_conn {
    enum tcp_state state;
    u32 src_ip, dst_ip;
    u16 src_port, dst_port;
    u32 snd_nxt, snd_una;
    u32 rcv_nxt;
    /* Receive ring for in-order delivered bytes. */
    u8  rbuf[4096];
    u32 rhead, rtail;
};

static struct tcp_conn tcp;

static u16 tcp_checksum(u32 src, u32 dst, const void *hdr, u32 len) {
    /* Pseudo-header + segment. src/dst are already in network byte order
     * (typed u32 but stored that way), so a memcpy preserves the right
     * layout. */
    static u8 buf[1600];
    if (len + 12 > sizeof buf) return 0;
    memcpy(buf, &src, 4);
    memcpy(buf + 4, &dst, 4);
    buf[8] = 0; buf[9] = IPPROTO_TCP;
    buf[10] = (len >> 8) & 0xFF; buf[11] = len & 0xFF;
    memcpy(buf + 12, hdr, len);
    return ip_checksum(buf, 12 + len);
}

static void tcp_emit(u8 flags, const void *data, u32 dlen) {
    static u8 pkt[1500];
    if (dlen + sizeof(struct tcp_hdr) > sizeof pkt) return;
    struct tcp_hdr *h = (struct tcp_hdr *)pkt;
    h->sport = htons(tcp.src_port);
    h->dport = htons(tcp.dst_port);
    h->seq = htonl(tcp.snd_nxt);
    h->ack = htonl(tcp.rcv_nxt);
    h->data_off = (sizeof *h / 4) << 4;
    h->flags = flags;
    h->window = htons(4096);
    h->checksum = 0;
    h->urg_ptr = 0;
    if (dlen) memcpy(pkt + sizeof *h, data, dlen);
    h->checksum = tcp_checksum(tcp.src_ip, tcp.dst_ip, pkt, sizeof *h + dlen);
    ip_send(tcp.dst_ip, IPPROTO_TCP, pkt, sizeof *h + dlen);
}

static void tcp_input(u32 src, const u8 *pkt, u32 len) {
    if (len < sizeof(struct tcp_hdr)) return;
    const struct tcp_hdr *h = (const struct tcp_hdr *)pkt;
    if (tcp.state == TCP_S_CLOSED) return;
    if (ntohs(h->dport) != tcp.src_port) return;
    if (src != tcp.dst_ip) return;
    if (ntohs(h->sport) != tcp.dst_port) return;

    u32 seg_seq = ntohl(h->seq);
    u32 seg_ack = ntohl(h->ack);
    u8  flags = h->flags;
    u32 hlen = (h->data_off >> 4) * 4u;
    if (hlen > len) return;
    u32 dlen = len - hlen;
    const u8 *data = pkt + hlen;

    if (flags & TCP_RST) { tcp.state = TCP_S_CLOSED; return; }

    if (tcp.state == TCP_S_SYN_SENT) {
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            tcp.rcv_nxt = seg_seq + 1;
            tcp.snd_una = seg_ack;
            tcp.snd_nxt = seg_ack;
            tcp.state = TCP_S_ESTABLISHED;
            tcp_emit(TCP_ACK, NULL, 0);
        }
        return;
    }

    if (tcp.state >= TCP_S_ESTABLISHED) {
        /* Accept in-order data only (no reordering). */
        if (dlen && seg_seq == tcp.rcv_nxt) {
            for (u32 i = 0; i < dlen; i++) {
                u32 next = (tcp.rhead + 1) % sizeof tcp.rbuf;
                if (next == tcp.rtail) break;
                tcp.rbuf[tcp.rhead] = data[i];
                tcp.rhead = next;
            }
            tcp.rcv_nxt += dlen;
            tcp_emit(TCP_ACK, NULL, 0);
        }
        if (flags & TCP_ACK) tcp.snd_una = seg_ack;
        if (flags & TCP_FIN) {
            tcp.rcv_nxt++;
            tcp_emit(TCP_ACK, NULL, 0);
            tcp.state = TCP_S_CLOSED_BY_PEER;
        }
    }
}

int tcp_connect(ip4_addr_t dst, u16 dport) {
    struct net_iface *n = net_iface();
    if (!n || !n->present) return -1;
    static u16 ephemeral = 49152;
    if (++ephemeral < 49152) ephemeral = 49152;
    memset(&tcp, 0, sizeof tcp);
    tcp.src_ip = n->ip;
    tcp.dst_ip = dst;
    tcp.src_port = ephemeral;
    tcp.dst_port = dport;
    tcp.snd_nxt = pit_ticks() * 1009u + 0xC0DE;
    tcp.rcv_nxt = 0;
    tcp.state = TCP_S_SYN_SENT;
    tcp_emit(TCP_SYN, NULL, 0);
    tcp.snd_nxt++;

    u32 t0 = pit_ticks();
    while (pit_ticks() - t0 < 500) {
        net_poll();
        if (tcp.state == TCP_S_ESTABLISHED) return 0;
        if (tcp.state == TCP_S_CLOSED) return -1;
    }
    tcp.state = TCP_S_CLOSED;
    return -1;
}

int tcp_send(const void *buf, u32 len) {
    if (tcp.state != TCP_S_ESTABLISHED) return -1;
    const u8 *p = buf;
    u32 sent = 0;
    while (sent < len) {
        u32 n = len - sent;
        if (n > 1400) n = 1400;
        tcp_emit(TCP_ACK | TCP_PSH, p + sent, n);
        tcp.snd_nxt += n;
        sent += n;
        /* Tiny wait so we don't fill the line. */
        u32 t = pit_ticks();
        while (pit_ticks() - t < 2) net_poll();
    }
    return (int)sent;
}

int tcp_recv(void *buf, u32 max, u32 timeout_ticks) {
    u8 *p = buf;
    u32 got = 0;
    u32 t0 = pit_ticks();
    while (got < max) {
        net_poll();
        while (tcp.rtail != tcp.rhead && got < max) {
            p[got++] = tcp.rbuf[tcp.rtail];
            tcp.rtail = (tcp.rtail + 1) % sizeof tcp.rbuf;
        }
        if (got) return (int)got;
        if (tcp.state == TCP_S_CLOSED_BY_PEER || tcp.state == TCP_S_CLOSED) {
            return 0;
        }
        if (pit_ticks() - t0 > timeout_ticks) return -1;
    }
    return (int)got;
}

void tcp_close(void) {
    if (tcp.state == TCP_S_ESTABLISHED || tcp.state == TCP_S_CLOSED_BY_PEER) {
        tcp_emit(TCP_FIN | TCP_ACK, NULL, 0);
        tcp.snd_nxt++;
        u32 t0 = pit_ticks();
        while (pit_ticks() - t0 < 100) net_poll();
    }
    tcp.state = TCP_S_CLOSED;
}

/* --- IP dispatch ----------------------------------------------------- */
static void ip_input(const u8 *pkt, u32 len) {
    if (len < sizeof(struct ipv4_hdr)) return;
    const struct ipv4_hdr *h = (const struct ipv4_hdr *)pkt;
    if ((h->vihl >> 4) != 4) return;
    u32 hlen = (h->vihl & 0x0F) * 4u;
    if (hlen < sizeof *h || hlen > len) return;
    u32 plen = (u32)ntohs(h->tot_len);
    if (plen > len) plen = len;
    u32 payload = plen - hlen;
    const u8 *data = pkt + hlen;
    struct net_iface *n = net_iface();
    if (!n) return;
    if (h->dst != n->ip) {
        /* Accept broadcast / any for ICMP echo, drop otherwise. */
        if (h->dst != 0xFFFFFFFFu && h->proto != IPPROTO_ICMP) return;
    }
    switch (h->proto) {
        case IPPROTO_ICMP: icmp_input(h->src, data, payload); break;
        case IPPROTO_UDP:  udp_input (h->src, data, payload); break;
        case IPPROTO_TCP:  tcp_input (h->src, data, payload); break;
        default: break;
    }
}

/* --- Poll loop ------------------------------------------------------- */
void net_poll(void) {
    struct net_iface *n = net_iface();
    if (!n || !n->present || !n->poll) return;
    static u8 frame[ETH_FRAME_MAX];
    for (int i = 0; i < 8; i++) {
        int len = n->poll(frame, sizeof frame);
        if (len <= 0) return;
        if (len < (int)sizeof(struct eth_hdr)) continue;
        struct eth_hdr *h = (struct eth_hdr *)frame;
        u16 t = ntohs(h->type);
        const u8 *payload = frame + 14;
        u32 plen = (u32)(len - 14);
        switch (t) {
            case ETH_TYPE_ARP_W: arp_input(payload, plen); break;
            case ETH_TYPE_IPV4:  ip_input(payload, plen); break;
            default: break;
        }
    }
}

void net_init(void) {
    /* Drivers self-register before we run; nothing to do beyond
     * announcing presence. */
    struct net_iface *n = net_iface();
    if (n && n->present) {
        u32 v = ntohl(n->ip);
        kprintf("net: MAC %02x:%02x:%02x:%02x:%02x:%02x  IP %u.%u.%u.%u\n",
                n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5],
                (v>>24)&0xFF, (v>>16)&0xFF, (v>>8)&0xFF, v&0xFF);
    }
}

/* --- IP helpers ----------------------------------------------------- */
ip4_addr_t ip4_parse(const char *s) {
    u32 v = 0;
    for (int i = 0; i < 4; i++) {
        u32 n = 0;
        while (*s >= '0' && *s <= '9') { n = n * 10 + (u32)(*s - '0'); s++; }
        if (n > 255) return 0;
        v = (v << 8) | n;
        if (i < 3) {
            if (*s != '.') return 0;
            s++;
        }
    }
    return htonl(v);
}

void ip4_format(ip4_addr_t a, char *out) {
    u32 v = ntohl(a);
    ksnprintf(out, 16, "%u.%u.%u.%u",
              (v>>24)&0xFF, (v>>16)&0xFF, (v>>8)&0xFF, v&0xFF);
}

/* --- DNS ------------------------------------------------------------ */
/* Minimal A-record resolver. Builds a single query, sends it to the
 * configured DNS server (default 8.8.8.8 -- override via net_iface.dns),
 * waits for the response, returns the first A record. Supports up to 3
 * CNAME hops. No EDNS, no caching, no IPv6.
 *
 * Wire-format (RFC 1035, simplified):
 *   header: 12 bytes (id, flags=0x0100 for RD, qdcount=1, an/ns/ar=0)
 *   qname:  for "example.com" -> 07 'example' 03 'com' 00
 *   qtype:  2 bytes (1 = A)
 *   qclass: 2 bytes (1 = IN) */
static u16 dns_id;

static int dns_encode_name(const char *name, u8 *out, int max) {
    int o = 0;
    const char *s = name;
    while (*s) {
        int len = 0;
        while (s[len] && s[len] != '.') len++;
        if (len > 63 || o + len + 1 >= max) return -1;
        out[o++] = (u8)len;
        for (int i = 0; i < len; i++) out[o++] = (u8)s[i];
        s += len;
        if (*s == '.') s++;
    }
    if (o >= max) return -1;
    out[o++] = 0;
    return o;
}

/* Skip a (possibly compressed) DNS name; returns new offset, or -1. */
static int dns_skip_name(const u8 *p, int plen, int off) {
    int hops = 0;
    while (off < plen) {
        u8 b = p[off];
        if (b == 0) return off + 1;
        if ((b & 0xC0) == 0xC0) {
            if (off + 2 > plen) return -1;
            return off + 2;
        }
        if (b > 63) return -1;
        off += 1 + b;
        if (++hops > 64) return -1;
    }
    return -1;
}

ip4_addr_t dns_resolve(const char *name) {
    /* Already a dotted-quad? */
    ip4_addr_t lit = ip4_parse(name);
    if (lit) return lit;
    struct net_iface *n = net_iface();
    if (!n || !n->present) return 0;
    /* Pick a DNS server: iface->dns if set, else 8.8.8.8. */
    extern ip4_addr_t dns_server;
    ip4_addr_t server = dns_server;
    if (!server) server = htonl(0x08080808u);

    static u8 pkt[512];
    u16 qid = ++dns_id;
    pkt[0] = (qid >> 8) & 0xFF; pkt[1] = qid & 0xFF;
    pkt[2] = 0x01; pkt[3] = 0x00;       /* RD=1 */
    pkt[4] = 0x00; pkt[5] = 0x01;       /* qdcount=1 */
    pkt[6] = 0; pkt[7] = 0;             /* an/ns/ar = 0 */
    pkt[8] = 0; pkt[9] = 0;
    pkt[10] = 0; pkt[11] = 0;
    int qn = dns_encode_name(name, pkt + 12, (int)sizeof pkt - 12 - 4);
    if (qn < 0) return 0;
    int qo = 12 + qn;
    pkt[qo++] = 0; pkt[qo++] = 1;       /* QTYPE = A */
    pkt[qo++] = 0; pkt[qo++] = 1;       /* QCLASS = IN */

    /* Pick an ephemeral source port. */
    u16 sport = 35000 + (u16)(pit_ticks() & 0x0FFF);
    /* Retry up to 3 times with 250 PIT-tick (~2.5 s) timeout each. */
    for (int retry = 0; retry < 3; retry++) {
        udp_send(server, 53, sport, pkt, qo);
        static u8 reply[512];
        ip4_addr_t src;
        u16 sp;
        int rlen = udp_recv(sport, reply, sizeof reply, &src, &sp, 250);
        if (rlen < 12) continue;
        if (reply[0] != pkt[0] || reply[1] != pkt[1]) continue;
        int ancount = (reply[6] << 8) | reply[7];
        if (ancount == 0) continue;
        /* Skip question section. */
        int off = dns_skip_name(reply, rlen, 12);
        if (off < 0) continue;
        off += 4;                       /* QTYPE + QCLASS */
        /* Walk answers, return the first A record. */
        for (int a = 0; a < ancount && off + 10 < rlen; a++) {
            off = dns_skip_name(reply, rlen, off);
            if (off < 0 || off + 10 > rlen) break;
            u16 atype = ((u16)reply[off] << 8) | reply[off + 1];
            u16 rdlen = ((u16)reply[off + 8] << 8) | reply[off + 9];
            off += 10;
            if (atype == 1 && rdlen == 4 && off + 4 <= rlen) {
                ip4_addr_t out;
                memcpy(&out, reply + off, 4);
                return out;
            }
            off += rdlen;
        }
    }
    return 0;
}

ip4_addr_t dns_server;
void net_set_dns(ip4_addr_t s) { dns_server = s; }

/* --- http_get ------------------------------------------------------- */
/* Last-fetch status, exposed via http_last_status() so callers (e.g.
 * the Web widget) can render meaningful errors instead of a silent
 * empty body. Status is the HTTP response code (200, 404, 301, ...);
 * 0 means we never got a status line; -1 means transport failure. */
static int g_http_status;
static char g_http_location[256];
int http_last_status(void) { return g_http_status; }
const char *http_last_location(void) { return g_http_location; }

/* Internal: one HTTP request, no redirect-following. Parses the
 * status line and the Location header into g_http_status /
 * g_http_location, streams body bytes to the named file. */
static int http_get_once(const char *url, const char *out_path) {
    g_http_status = 0;
    g_http_location[0] = '\0';
    if (strncmp(url, "http://", 7) != 0) {
        kputs("http_get: only http:// supported\n");
        return -1;
    }
    const char *p = url + 7;
    char host[64]; int hi = 0;
    while (*p && *p != ':' && *p != '/' && hi < (int)sizeof host - 1)
        host[hi++] = *p++;
    host[hi] = '\0';

    u16 port = 80;
    if (*p == ':') {
        p++;
        port = 0;
        while (*p >= '0' && *p <= '9') { port = (u16)(port * 10 + (*p - '0')); p++; }
    }
    const char *path = (*p == '/') ? p : "/";
    ip4_addr_t dst = dns_resolve(host);
    if (!dst) { kprintf("http_get: cannot resolve %s\n", host); return -1; }

    kprintf("http_get: GET http://%s:%u%s\n", host, (u32)port, path);

    if (tcp_connect(dst, port) < 0) {
        kputs("http_get: TCP connect failed\n"); return -1;
    }

    char req[256];
    int reqlen = ksnprintf(req, sizeof req,
                           "GET %s HTTP/1.0\r\nHost: %s\r\n"
                           "User-Agent: Zenbite/0.3\r\n"
                           "Connection: close\r\n\r\n",
                           path, host);
    tcp_send(req, (u32)reqlen);

    fs_unlink(out_path);
    if (fs_create(out_path) < 0) {
        kputs("http_get: cannot create output file\n");
        tcp_close(); return -1;
    }
    int fh = fs_open(out_path);
    if (fh < 0) { tcp_close(); return -1; }

    /* Header parser: accumulate into a small line buffer so we can
     * extract the status line and Location:. State machine: every
     * \r\n closes a "line"; the special blank line (CRLF CRLF)
     * marks the body start. */
    char line[256]; int ll = 0;
    int header_done = 0;
    int got_status = 0;
    static u8 chunk[1024];
    u32 total = 0;
    for (;;) {
        int n = tcp_recv(chunk, sizeof chunk, 500);
        if (n <= 0) break;
        int i = 0;
        while (!header_done && i < n) {
            u8 c = chunk[i++];
            if (c == '\r') continue;
            if (c != '\n') {
                if (ll < (int)sizeof line - 1) line[ll++] = (char)c;
                continue;
            }
            /* End of one header line. */
            line[ll] = '\0';
            if (ll == 0) { header_done = 1; break; }
            if (!got_status &&
                line[0] == 'H' && line[1] == 'T' && line[2] == 'T' &&
                line[3] == 'P' && line[4] == '/') {
                /* "HTTP/1.0 200 OK" -- find first space. */
                int j = 0; while (line[j] && line[j] != ' ') j++;
                while (line[j] == ' ') j++;
                int code = 0;
                while (line[j] >= '0' && line[j] <= '9') {
                    code = code * 10 + (line[j] - '0'); j++;
                }
                g_http_status = code;
                got_status = 1;
            } else if ((line[0] == 'L' || line[0] == 'l') &&
                       (line[1] == 'o' || line[1] == 'O') &&
                       strncmp(line + 2, "cation:", 7) == 0) {
                int j = 9;
                while (line[j] == ' ' || line[j] == '\t') j++;
                int o = 0;
                while (line[j] && o < (int)sizeof g_http_location - 1)
                    g_http_location[o++] = line[j++];
                g_http_location[o] = '\0';
            }
            ll = 0;
        }
        if (header_done && i < n) {
            int w = fs_write(fh, chunk + i, (size_t)(n - i));
            if (w > 0) total += (u32)w;
        }
    }
    fs_close(fh);
    tcp_close();
    return (int)total;
}

/* Public http_get: follow 301/302/303/307/308 redirects up to 5
 * times. Useful because many old-web sites redirect their root to a
 * canonical path -- and Google redirects http -> https almost
 * immediately, so this lets the caller see "301" + the destination
 * instead of an empty body. */
int http_get(const char *url, const char *out_path) {
    char cur[512];
    int  cl = 0;
    while (cl < (int)sizeof cur - 1 && url[cl]) { cur[cl] = url[cl]; cl++; }
    cur[cl] = '\0';
    for (int hop = 0; hop < 5; hop++) {
        int body = http_get_once(cur, out_path);
        int st = g_http_status;
        if (st == 301 || st == 302 || st == 303 ||
            st == 307 || st == 308) {
            if (!g_http_location[0]) return body;
            /* Absolute URL?  Use as-is.  Otherwise glue onto current
             * scheme/host (we only do http://, so absolute is the
             * common case from real servers). */
            if (strncmp(g_http_location, "http://", 7) == 0) {
                int i = 0;
                while (g_http_location[i] && i < (int)sizeof cur - 1) {
                    cur[i] = g_http_location[i]; i++;
                }
                cur[i] = '\0';
                continue;
            }
            /* HTTPS or other-scheme redirect: we can't follow. Leave
             * body as the redirect page (typically a tiny "Moved"
             * notice) so the caller can show the target URL. */
            return body;
        }
        return body;
    }
    return -1;
}
