/* Network adapters, HTTP via WinHTTP, clipboard. */
#include "tools.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "user32.lib")

/* ---- adapters ---------------------------------------------------------- */
static void wcopy_u8(char *dst, size_t n, const wchar_t *src)
{
    char *u = w_to_u8(src ? src : L"");
    strncpy(dst, u, n - 1);
    dst[n - 1] = 0;
    free(u);
}

int list_adapters(Adapter **out, int *n)
{
    ULONG size = 16 * 1024;
    IP_ADAPTER_ADDRESSES *aa = xmalloc(size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_UNICAST | GAA_FLAG_SKIP_FRIENDLY_NAME * 0;
    ULONG rc = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, aa, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        aa = xrealloc(aa, size);
        rc = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, aa, &size);
    }
    *out = NULL; *n = 0;
    if (rc != NO_ERROR) { free(aa); return 0; }

    Adapter *list = NULL; int count = 0;
    for (IP_ADAPTER_ADDRESSES *a = aa; a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        list = xrealloc(list, (size_t)(count + 1) * sizeof(Adapter));
        Adapter *d = &list[count++];
        memset(d, 0, sizeof *d);
        wcopy_u8(d->name, sizeof d->name, a->FriendlyName);
        wcopy_u8(d->desc, sizeof d->desc, a->Description);
        d->up = a->OperStatus == IfOperStatusUp;
        d->is_vpn = str_icontains(d->name, "Proton") || str_icontains(d->desc, "WireGuard") ||
                    str_icontains(d->desc, "TAP") || str_icontains(d->desc, "Wintun") ||
                    str_icontains(d->desc, "OpenVPN");
        for (IP_ADAPTER_DNS_SERVER_ADDRESS *s = a->FirstDnsServerAddress; s; s = s->Next) {
            if (s->Address.lpSockaddr->sa_family != AF_INET) continue;
            char ip[INET_ADDRSTRLEN];
            struct sockaddr_in *sin = (struct sockaddr_in *)s->Address.lpSockaddr;
            if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip)) continue;
            if (d->dns[0]) strncat(d->dns, ", ", sizeof d->dns - strlen(d->dns) - 1);
            strncat(d->dns, ip, sizeof d->dns - strlen(d->dns) - 1);
        }
    }
    free(aa);
    *out = list; *n = count;
    return 1;
}

char *vpn_adapter_up(void)
{
    Adapter *list; int n;
    char *found = NULL;
    if (!list_adapters(&list, &n)) return NULL;
    for (int i = 0; i < n && !found; i++) {
        Adapter *d = &list[i];
        if (!d->up) continue;
        if (str_icontains(d->name, "Proton") || str_icontains(d->desc, "WireGuard") || str_icontains(d->desc, "TAP-Proton"))
            found = xstrdup(d->name);
    }
    free(list);
    return found;
}

/* ---- HTTP -------------------------------------------------------------- */
typedef struct {
    HINTERNET session, conn, req;
    int status;
} Http;

static void http_close(Http *h)
{
    if (h->req) WinHttpCloseHandle(h->req);
    if (h->conn) WinHttpCloseHandle(h->conn);
    if (h->session) WinHttpCloseHandle(h->session);
    memset(h, 0, sizeof *h);
}

/* Open a request. Returns 1 with h->req ready to send, 0 on failure. */
static int http_open(Http *h, const wchar_t *url, const wchar_t *verb, int timeout_s)
{
    memset(h, 0, sizeof *h);
    URL_COMPONENTS uc;
    wchar_t host[256];
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    /* NULL buffers with non-zero lengths make CrackUrl return pointers into
     * `url`, so lpszUrlPath runs to the end of the string, query included. */
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return 0;
    if (uc.dwHostNameLength >= 256) return 0;
    wmemcpy(host, uc.lpszHostName, uc.dwHostNameLength);
    host[uc.dwHostNameLength] = 0;
    const wchar_t *path = uc.lpszUrlPath && *uc.lpszUrlPath ? uc.lpszUrlPath : L"/";

    h->session = WinHttpOpen(L"vpn-check/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h->session) return 0;
    int ms = timeout_s * 1000;
    WinHttpSetTimeouts(h->session, ms, ms, ms, ms);
    DWORD proto = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000 /* TLS1_3 */;
    WinHttpSetOption(h->session, WINHTTP_OPTION_SECURE_PROTOCOLS, &proto, sizeof proto);
    DWORD decomp = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(h->session, WINHTTP_OPTION_DECOMPRESSION, &decomp, sizeof decomp);

    h->conn = WinHttpConnect(h->session, host, uc.nPort, 0);
    if (!h->conn) { http_close(h); return 0; }
    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    h->req = WinHttpOpenRequest(h->conn, verb, path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!h->req) { http_close(h); return 0; }
    return 1;
}

static int http_status(Http *h)
{
    DWORD code = 0, sz = sizeof code;
    if (!WinHttpQueryHeaders(h->req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX))
        return 0;
    return (int)code;
}

int http_get(const wchar_t *url, char **body, size_t *len, int *status, int timeout_s)
{
    Http h;
    *body = NULL; *len = 0; *status = 0;
    if (!http_open(&h, url, L"GET", timeout_s)) return 0;
    int ok = 0;
    if (WinHttpSendRequest(h.req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) &&
        WinHttpReceiveResponse(h.req, NULL)) {
        *status = http_status(&h);
        Buf b = { 0 };
        char chunk[8192];
        DWORD got;
        while (WinHttpReadData(h.req, chunk, sizeof chunk, &got) && got) {
            buf_append(&b, chunk, got);
            if (b.len > 4 * 1024 * 1024) break;
        }
        if (!b.s) buf_append(&b, "", 0);
        *body = b.s; *len = b.len;
        ok = *status >= 200 && *status < 300;
    }
    http_close(&h);
    return ok;
}

int http_download(const wchar_t *url, long long *bytes, double *seconds, int *status)
{
    Http h;
    *bytes = 0; *status = 0;
    double t0 = now_seconds();
    if (!http_open(&h, url, L"GET", 120)) { *seconds = now_seconds() - t0; return 0; }
    int ok = 0;
    if (WinHttpSendRequest(h.req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) &&
        WinHttpReceiveResponse(h.req, NULL)) {
        *status = http_status(&h);
        if (*status >= 200 && *status < 300) {
            char *chunk = xmalloc(1 << 20);
            DWORD got;
            while (WinHttpReadData(h.req, chunk, 1 << 20, &got) && got) *bytes += got;
            free(chunk);
            ok = *bytes > 0;
        }
    }
    *seconds = now_seconds() - t0;
    http_close(&h);
    return ok;
}

int http_upload(const wchar_t *url, size_t nbytes, double *seconds, int *status)
{
    Http h;
    *status = 0;
    const size_t CHUNK = 1 << 20;
    unsigned char *chunk = xmalloc(CHUNK);
    unsigned x = (unsigned)GetTickCount() | 1;       /* xorshift fill, incompressible enough */
    for (size_t i = 0; i < CHUNK; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; chunk[i] = (unsigned char)x; }

    double t0 = now_seconds();
    if (!http_open(&h, url, L"POST", 120)) { free(chunk); *seconds = now_seconds() - t0; return 0; }
    int ok = 0;
    if (WinHttpSendRequest(h.req, L"Content-Type: application/octet-stream", (DWORD)-1, NULL, 0, (DWORD)nbytes, 0)) {
        size_t left = nbytes;
        int fail = 0;
        while (left && !fail) {
            DWORD want = (DWORD)(left < CHUNK ? left : CHUNK), wrote = 0;
            if (!WinHttpWriteData(h.req, chunk, want, &wrote) || !wrote) fail = 1;
            else left -= wrote;
        }
        if (!fail && WinHttpReceiveResponse(h.req, NULL)) {
            *status = http_status(&h);
            char sink[4096]; DWORD got;
            while (WinHttpReadData(h.req, sink, sizeof sink, &got) && got) {}
            ok = *status >= 200 && *status < 300;
        }
    }
    *seconds = now_seconds() - t0;
    http_close(&h);
    free(chunk);
    return ok;
}

/* ---- clipboard --------------------------------------------------------- */
int copy_to_clipboard(const char *text)
{
    wchar_t *w = u8_to_w(text);
    size_t bytes = (wcslen(w) + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    int ok = 0;
    if (g) {
        void *p = GlobalLock(g);
        memcpy(p, w, bytes);
        GlobalUnlock(g);
        if (OpenClipboard(NULL)) {
            EmptyClipboard();
            ok = SetClipboardData(CF_UNICODETEXT, g) != NULL;
            CloseClipboard();
        }
        if (!ok) GlobalFree(g);
    }
    free(w);
    return ok;
}
