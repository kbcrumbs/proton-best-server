/* ICMP echo via iphlpapi. Works without administrator rights.
 *
 * All hosts are pinged round-robin from the calling thread with one echo in
 * flight at a time. Measured on this machine through a WireGuard tunnel:
 * several threads calling IcmpSendEcho concurrently (own handle each, shared
 * handle, async IcmpSendEcho2, even serialized with a mutex) lost 20-50% of
 * replies, while one thread cycling over the same hosts lost none. So the
 * "parallel" design of the Python version is replaced by interleaving, which
 * costs about (hosts x count x (gap + rtt)) seconds in total. */
#include "tools.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <string.h>
#include <stdlib.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

static int resolve_v4(const char *host, IPAddr *out)
{
    struct in_addr a;
    if (inet_pton(AF_INET, host, &a) == 1) { *out = a.S_un.S_addr; return 1; }
    static LONG wsa_ready;
    if (!wsa_ready) {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        InterlockedExchange(&wsa_ready, 1);
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_RAW;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return 0;
    *out = ((struct sockaddr_in *)res->ai_addr)->sin_addr.S_un.S_addr;
    freeaddrinfo(res);
    return 1;
}

void ping_hosts(const char **hosts, int n, int count, int timeout_ms, int gap_ms,
                PingStat *out, void (*progress)(int, void *), void *ctx)
{
    IPAddr *addr = xmalloc((size_t)n * sizeof *addr);
    int *resolved = xmalloc((size_t)n * sizeof *resolved);
    long *sum = xmalloc((size_t)n * sizeof *sum);
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof out[i]);
        out[i].loss = 100;
        sum[i] = 0;
        resolved[i] = resolve_v4(hosts[i], &addr[i]);
    }

    HANDLE h = IcmpCreateFile();
    char payload[32];
    memset(payload, 'a', sizeof payload);
    DWORD replysz = sizeof(ICMP_ECHO_REPLY) + sizeof payload + 8;
    void *reply = xmalloc(replysz);

    int first = 1;
    for (int round = 0; round < count; round++) {
        for (int i = 0; i < n; i++) {
            if (!resolved[i]) continue;
            if (!first) Sleep((DWORD)gap_ms);
            first = 0;
            PingStat *o = &out[i];
            o->sent++;
            if (h == INVALID_HANDLE_VALUE) continue;
            DWORD got = IcmpSendEcho(h, addr[i], payload, sizeof payload, NULL, reply, replysz, (DWORD)timeout_ms);
            if (got == 0) continue;
            ICMP_ECHO_REPLY *r = reply;
            if (r->Status != IP_SUCCESS) continue;
            int rtt = (int)r->RoundTripTime;
            if (!o->has) { o->min = o->max = rtt; o->has = 1; }
            if (rtt < o->min) o->min = rtt;
            if (rtt > o->max) o->max = rtt;
            sum[i] += rtt;
            o->recv++;
        }
        if (progress) progress(round + 1, ctx);
    }
    if (h != INVALID_HANDLE_VALUE) IcmpCloseHandle(h);

    for (int i = 0; i < n; i++) {
        PingStat *o = &out[i];
        if (o->recv) o->avg = (int)((sum[i] + o->recv / 2) / o->recv);
        o->loss = o->sent ? (int)((100 * (o->sent - o->recv) + o->sent / 2) / o->sent) : 100;
    }
    free(reply); free(sum); free(resolved); free(addr);
}
