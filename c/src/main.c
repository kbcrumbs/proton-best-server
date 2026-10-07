/* Proton VPN Tools: Win32 GUI front end for the server ranker and the
 * connection check. Workers run on background threads and post results to
 * their page window; the UI thread owns all widgets and result data. */
#include "tools.h"
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

enum {
    WM_APP_STATUS = WM_APP + 1,   /* lParam: wchar_t* (free)             */
    WM_APP_RECOMMEND,             /* lParam: wchar_t* (free)             */
    WM_APP_ROWS,                  /* lParam: ServerResult* (take over)   */
    WM_APP_LOG,                   /* lParam: wchar_t* line (free)        */
    WM_APP_JSON,                  /* lParam: char* json text (take over) */
    WM_APP_ERROR,                 /* lParam: wchar_t* (free)             */
    WM_APP_DONE
};

static HINSTANCE g_inst;
static HWND g_main, g_tab, g_pages[2];
static HFONT g_mono;
static int g_dpi = 96;

static int px(int v) { return MulDiv(v, g_dpi, 96); }

/* ---- posting helpers ---------------------------------------------------- */
static void post_text(HWND h, UINT msg, const char *u8)
{
    wchar_t *w = u8_to_w(u8);
    if (!PostMessageW(h, msg, 0, (LPARAM)w)) free(w);
}

static void post_fmt(HWND h, UINT msg, const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    post_text(h, msg, tmp);
}

/* ---- anchored layout ---------------------------------------------------- */
enum { A_RIGHT = 1, A_BOTTOM = 2, A_WIDTH = 4, A_HEIGHT = 8 };
typedef struct { int id, flags; RECT r; } Anchor;
typedef struct { Anchor *a; int n; SIZE init; } Layout;

static void layout_init(HWND dlg, Layout *L, Anchor *a, int n)
{
    RECT rc;
    GetClientRect(dlg, &rc);
    L->init.cx = rc.right; L->init.cy = rc.bottom;
    L->a = a; L->n = n;
    for (int i = 0; i < n; i++) {
        GetWindowRect(GetDlgItem(dlg, a[i].id), &a[i].r);
        MapWindowPoints(NULL, dlg, (POINT *)&a[i].r, 2);
    }
}

static void layout_apply(HWND dlg, Layout *L)
{
    RECT rc;
    GetClientRect(dlg, &rc);
    int dw = rc.right - L->init.cx, dh = rc.bottom - L->init.cy;
    HDWP h = BeginDeferWindowPos(L->n);
    for (int i = 0; i < L->n; i++) {
        RECT r = L->a[i].r;
        int f = L->a[i].flags;
        if (f & A_RIGHT)  { r.left += dw; r.right += dw; }
        if (f & A_WIDTH)  r.right += dw;
        if (f & A_BOTTOM) { r.top += dh; r.bottom += dh; }
        if (f & A_HEIGHT) r.bottom += dh;
        h = DeferWindowPos(h, GetDlgItem(dlg, L->a[i].id), NULL, r.left, r.top,
                           r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    EndDeferWindowPos(h);
}

static void set_text_u8(HWND dlg, int id, const char *u8)
{
    wchar_t *w = u8_to_w(u8);
    SetDlgItemTextW(dlg, id, w);
    free(w);
}

static void ft_to_str(const FILETIME *ft, char *out, size_t n)
{
    FILETIME lt; SYSTEMTIME st;
    FileTimeToLocalFileTime(ft, &lt);
    FileTimeToSystemTime(&lt, &st);
    _snprintf(out, n, "%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    out[n - 1] = 0;
}

/* ======================================================================== */
/* Page 1: best server                                                      */
/* ======================================================================== */
typedef struct {
    HWND page;
    char country[8];
    int include_free, include_sc, force, pings, timeout, maxload;
} ServerJob;

typedef struct { NodeRow *rows; int n, best; } ServerResult;

static ServerResult *g_res;      /* owned by the UI thread */
static Layout g_lay1;
static Anchor g_anch1[] = {
    { IDC_RUN, A_RIGHT }, { IDC_COPY, A_RIGHT },
    { IDC_LIST, A_WIDTH | A_HEIGHT },
    { IDC_RECOMMEND, A_BOTTOM | A_WIDTH }, { IDC_STATUS, A_BOTTOM | A_WIDTH },
};

typedef struct { int n, pings; HWND page; } PingCtx;

static void ping_progress(int round, void *p)
{
    PingCtx *c = p;
    post_fmt(c->page, WM_APP_STATUS, "Ping round %d of %d done (%d nodes)...", round, c->pings, c->n);
}

static DWORD WINAPI server_thread(LPVOID arg)
{
    ServerJob *j = arg;
    HWND page = j->page;
    char *warn = NULL;

    char *up = vpn_adapter_up();
    if (up && !j->force) {
        post_fmt(page, WM_APP_ERROR,
                 "VPN adapter '%s' is connected. Pings would travel through the tunnel and measure "
                 "the current exit's path, not yours.\n\nDisconnect Proton VPN and run again, "
                 "or tick Force to measure anyway.", up);
        free(up); free(j);
        PostMessageW(page, WM_APP_DONE, 0, 0);
        return 0;
    }
    if (up) {
        warn = xmalloc(256);
        _snprintf(warn, 256, "WARNING: VPN adapter '%s' is up; results reflect the tunnel path, not yours.", up);
        warn[255] = 0;
    }
    free(up);

    wchar_t path[MAX_PATH]; FILETIME ft;
    if (!find_cache(path, MAX_PATH, &ft)) {
        post_text(page, WM_APP_ERROR,
                  "No Proton VPN server cache found under %LOCALAPPDATA%\\Proton\\Proton VPN\\Storage.\n\n"
                  "Open the Proton VPN app once so it downloads the server list, then retry.");
        free(warn); free(j);
        PostMessageW(page, WM_APP_DONE, 0, 0);
        return 0;
    }
    Server *servers; int ns;
    if (!load_servers(path, &servers, &ns)) {
        post_text(page, WM_APP_ERROR, "Could not read the server cache file.");
        free(warn); free(j);
        PostMessageW(page, WM_APP_DONE, 0, 0);
        return 0;
    }
    ServerFilter f = { j->country, j->include_free, j->include_sc };
    NodeRow *rows; int n;
    group_servers(servers, ns, &f, &rows, &n);
    free_servers(servers, ns);
    if (!n) {
        post_fmt(page, WM_APP_ERROR,
                 "No %s servers matched. Try Include Free or Include Secure Core, or check the country code.",
                 j->country);
        free_rows(rows, n); free(warn); free(j);
        PostMessageW(page, WM_APP_DONE, 0, 0);
        return 0;
    }

    char when[32];
    ft_to_str(&ft, when, sizeof when);
    post_fmt(page, WM_APP_RECOMMEND, "Testing %d unique %s entry nodes with %d pings each...", n, j->country, j->pings);
    post_fmt(page, WM_APP_STATUS, "Server list cached %s (load values are from that refresh)", when);

    {
        const char **hosts = xmalloc((size_t)n * sizeof *hosts);
        PingStat *stats = xmalloc((size_t)n * sizeof *stats);
        for (int i = 0; i < n; i++) hosts[i] = rows[i].ip;
        PingCtx ctx = { n, j->pings, page };
        ping_hosts(hosts, n, j->pings, j->timeout, 100, stats, ping_progress, &ctx);
        for (int i = 0; i < n; i++) rows[i].ping = stats[i];
        free(stats); free(hosts);
    }
    sort_rows(rows, n);

    int best = -1;
    for (int i = 0; i < n && best < 0; i++) {
        NodeRow *r = &rows[i];
        if (r->ping.has && r->ping.loss == 0 && (!r->has_load || r->load <= j->maxload)) best = i;
    }
    ServerResult *res = xmalloc(sizeof *res);
    res->rows = rows; res->n = n; res->best = best;
    if (!PostMessageW(page, WM_APP_ROWS, 0, (LPARAM)res)) { free_rows(rows, n); free(res); }

    if (best >= 0) {
        NodeRow *b = &rows[best];
        Buf msg = { 0 };
        buf_printf(&msg, "Recommended: %s (%s, %s) - %d ms avg", b->names[0], b->city, b->ip, b->ping.avg);
        if (b->has_load) buf_printf(&msg, ", %d%% load", b->load);
        if (b->nnames > 1) {
            buf_puts(&msg, ".  Same physical node: ");
            for (int i = 0; i < b->nnames; i++) buf_printf(&msg, "%s%s", i ? ", " : "", b->names[i]);
        }
        post_text(page, WM_APP_RECOMMEND, msg.s);
        buf_free(&msg);
    } else {
        post_text(page, WM_APP_RECOMMEND, "No node answered all pings under the load limit; raise Max load or use more pings.");
    }
    if (warn) post_text(page, WM_APP_STATUS, warn);
    else post_fmt(page, WM_APP_STATUS, "Done. Server list cached %s. Double-click a row to copy its server name.", when);
    free(warn); free(j);
    PostMessageW(page, WM_APP_DONE, 0, 0);
    return 0;
}

static void lv_set(HWND lv, int row, int col, const char *u8)
{
    wchar_t *w = u8_to_w(u8);
    if (col == 0) {
        LVITEMW it; memset(&it, 0, sizeof it);
        it.mask = LVIF_TEXT; it.iItem = row; it.pszText = w;
        ListView_InsertItem(lv, &it);
    } else {
        ListView_SetItemText(lv, row, col, w);
    }
    free(w);
}

static void fill_list(HWND lv, ServerResult *res)
{
    char tmp[512], flags[128];
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    for (int i = 0; i < res->n; i++) {
        NodeRow *r = &res->rows[i];
        if (r->ping.has) _snprintf(tmp, sizeof tmp, "%d ms", r->ping.avg); else strcpy(tmp, "--");
        lv_set(lv, i, 0, tmp);
        if (r->ping.has) _snprintf(tmp, sizeof tmp, "%d", r->ping.min); else strcpy(tmp, "--");
        lv_set(lv, i, 1, tmp);
        if (r->ping.has) _snprintf(tmp, sizeof tmp, "%d", r->ping.max); else strcpy(tmp, "--");
        lv_set(lv, i, 2, tmp);
        _snprintf(tmp, sizeof tmp, "%d%%", r->ping.loss);
        lv_set(lv, i, 3, tmp);
        if (r->has_load) _snprintf(tmp, sizeof tmp, "%d%%", r->load); else strcpy(tmp, "--");
        lv_set(lv, i, 4, tmp);
        lv_set(lv, i, 5, tier_name(r->tier));
        lv_set(lv, i, 6, r->city);
        lv_set(lv, i, 7, r->ip);
        Buf names = { 0 };
        for (int k = 0; k < r->nnames && k < 4; k++) buf_printf(&names, "%s%s", k ? ", " : "", r->names[k]);
        if (r->nnames > 4) buf_printf(&names, " (+%d)", r->nnames - 4);
        lv_set(lv, i, 8, names.s ? names.s : "");
        buf_free(&names);
        flags_text(r, flags, sizeof flags);
        lv_set(lv, i, 9, flags);
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    if (res->best >= 0) {
        ListView_SetItemState(lv, res->best, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(lv, res->best, FALSE);
    }
}

static void copy_row(HWND page, int row)
{
    if (!g_res || row < 0 || row >= g_res->n || !g_res->rows[row].nnames) return;
    const char *name = g_res->rows[row].names[0];
    char msg[256];
    if (copy_to_clipboard(name))
        _snprintf(msg, sizeof msg, "%s is on your clipboard. Open Proton VPN, click the search box, press Ctrl+V, then Connect.", name);
    else
        _snprintf(msg, sizeof msg, "Could not open the clipboard.");
    msg[sizeof msg - 1] = 0;
    set_text_u8(page, IDC_STATUS, msg);
}

static void populate_countries(HWND page)
{
    HWND cb = GetDlgItem(page, IDC_COUNTRY);
    wchar_t path[MAX_PATH]; FILETIME ft;
    if (!find_cache(path, MAX_PATH, &ft)) {
        SetDlgItemTextW(page, IDC_STATUS,
            L"No Proton VPN server cache found. Open the Proton VPN app once so it downloads the server list.");
        return;
    }
    Server *s; int n;
    if (!load_servers(path, &s, &n)) return;
    for (int i = 0; i < n; i++) {
        if (!s[i].exit_[0]) continue;
        wchar_t *w = u8_to_w(s[i].exit_);
        if (SendMessageW(cb, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)w) == CB_ERR)
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)w);
        free(w);
    }
    free_servers(s, n);
    char when[32], msg[200];
    ft_to_str(&ft, when, sizeof when);
    _snprintf(msg, sizeof msg, "Server list cached %s, %d logical servers. Disconnect the VPN, then click Run.", when, n);
    set_text_u8(page, IDC_STATUS, msg);
}

static void start_server_job(HWND page)
{
    ServerJob *j = xmalloc(sizeof *j);
    memset(j, 0, sizeof *j);
    j->page = page;
    wchar_t wc[16];
    GetDlgItemTextW(page, IDC_COUNTRY, wc, 16);
    char *c = w_to_u8(wc);
    strncpy(j->country, c, sizeof j->country - 1);
    free(c);
    for (char *p = j->country; *p; p++) *p = (char)toupper((unsigned char)*p);
    if (strlen(j->country) != 2) {
        MessageBoxW(page, L"Enter a two-letter country code, for example JP.", L"Proton VPN Tools", MB_ICONWARNING);
        return;
    }
    j->include_free = IsDlgButtonChecked(page, IDC_FREE) == BST_CHECKED;
    j->include_sc = IsDlgButtonChecked(page, IDC_SECURECORE) == BST_CHECKED;
    j->force = IsDlgButtonChecked(page, IDC_FORCE) == BST_CHECKED;
    j->pings = (int)GetDlgItemInt(page, IDC_PINGS, NULL, FALSE);
    j->timeout = (int)GetDlgItemInt(page, IDC_TIMEOUT, NULL, FALSE);
    j->maxload = (int)GetDlgItemInt(page, IDC_MAXLOAD, NULL, FALSE);
    if (j->pings < 1) j->pings = 1;
    if (j->pings > 100) j->pings = 100;
    if (j->timeout < 100) j->timeout = 100;
    if (j->timeout > 10000) j->timeout = 10000;

    EnableWindow(GetDlgItem(page, IDC_RUN), FALSE);
    EnableWindow(GetDlgItem(page, IDC_COPY), FALSE);
    SetDlgItemTextW(page, IDC_RECOMMEND, L"");
    SetDlgItemTextW(page, IDC_STATUS, L"Checking VPN state and reading the server cache...");
    HANDLE th = CreateThread(NULL, 0, server_thread, j, 0, NULL);
    if (th) CloseHandle(th);
    else { free(j); EnableWindow(GetDlgItem(page, IDC_RUN), TRUE); }
}

static INT_PTR CALLBACK servers_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_INITDIALOG: {
        HWND lv = GetDlgItem(h, IDC_LIST);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);
        static const struct { const wchar_t *t; int w, fmt; } cols[] = {
            { L"avg", 52, LVCFMT_LEFT }, { L"min", 40, LVCFMT_RIGHT }, { L"max", 40, LVCFMT_RIGHT },
            { L"loss", 42, LVCFMT_RIGHT }, { L"load", 42, LVCFMT_RIGHT }, { L"tier", 44, LVCFMT_LEFT },
            { L"city", 90, LVCFMT_LEFT }, { L"entry IP", 110, LVCFMT_LEFT }, { L"servers", 230, LVCFMT_LEFT },
            { L"features", 120, LVCFMT_LEFT },
        };
        for (int i = 0; i < (int)(sizeof cols / sizeof cols[0]); i++) {
            LVCOLUMNW c; memset(&c, 0, sizeof c);
            c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            c.pszText = (wchar_t *)cols[i].t; c.cx = px(cols[i].w); c.fmt = cols[i].fmt;
            ListView_InsertColumn(lv, i, &c);
        }
        SetDlgItemInt(h, IDC_PINGS, 5, FALSE);
        SetDlgItemInt(h, IDC_TIMEOUT, 1500, FALSE);
        SetDlgItemInt(h, IDC_MAXLOAD, 80, FALSE);
        SetDlgItemTextW(h, IDC_COUNTRY, L"JP");
        populate_countries(h);
        layout_init(h, &g_lay1, g_anch1, sizeof g_anch1 / sizeof g_anch1[0]);
        return TRUE;
    }
    case WM_SIZE:
        layout_apply(h, &g_lay1);
        /* Let the last column soak up whatever width is left. */
        ListView_SetColumnWidth(GetDlgItem(h, IDC_LIST), 9, LVSCW_AUTOSIZE_USEHEADER);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(w) == IDC_RUN) { start_server_job(h); return TRUE; }
        if (LOWORD(w) == IDC_COPY && g_res) { copy_row(h, g_res->best); return TRUE; }
        return FALSE;
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)l;
        if (nm->idFrom == IDC_LIST && nm->code == NM_DBLCLK) {
            copy_row(h, ((NMITEMACTIVATE *)l)->iItem);
            return TRUE;
        }
        return FALSE;
    }
    case WM_APP_STATUS:
        SetDlgItemTextW(h, IDC_STATUS, (wchar_t *)l); free((void *)l); return TRUE;
    case WM_APP_RECOMMEND:
        SetDlgItemTextW(h, IDC_RECOMMEND, (wchar_t *)l); free((void *)l); return TRUE;
    case WM_APP_ERROR:
        SetDlgItemTextW(h, IDC_STATUS, L"Stopped.");
        MessageBoxW(h, (wchar_t *)l, L"Proton VPN Tools", MB_ICONWARNING);
        free((void *)l);
        return TRUE;
    case WM_APP_ROWS: {
        ServerResult *res = (ServerResult *)l;
        if (g_res) { free_rows(g_res->rows, g_res->n); free(g_res); }
        g_res = res;
        fill_list(GetDlgItem(h, IDC_LIST), res);
        EnableWindow(GetDlgItem(h, IDC_COPY), res->best >= 0);
        return TRUE;
    }
    case WM_APP_DONE:
        EnableWindow(GetDlgItem(h, IDC_RUN), TRUE);
        return TRUE;
    }
    return FALSE;
}

/* ======================================================================== */
/* Page 2: connection check                                                 */
/* ======================================================================== */
typedef struct { HWND page; int pings, streams, nospeed; wchar_t compare[MAX_PATH]; } CheckJob;

static char *g_json;      /* last run, owned by the UI thread */
static Layout g_lay2;
static Anchor g_anch2[] = {
    { IDC_CRUN, A_RIGHT }, { IDC_SAVEJSON, A_RIGHT }, { IDC_BROWSE, A_RIGHT },
    { IDC_COMPARE, A_WIDTH }, { IDC_OUTPUT, A_WIDTH | A_HEIGHT }, { IDC_CSTATUS, A_BOTTOM | A_WIDTH },
};

static const char *PING_HOSTS[] = { "1.1.1.1", "8.8.8.8", "www.google.com", "www.youtube.com", "steamcommunity.com" };
#define NPING (int)(sizeof PING_HOSTS / sizeof PING_HOSTS[0])

static const struct { const char *cc, *loc; } VULTR[] = {
    { "JP", "hnd-jp" }, { "KR", "icn-kr" }, { "SG", "sgp" }, { "IN", "bom-in" }, { "AU", "syd-au" },
    { "US", "lax-ca-us" }, { "CA", "yto-ca" }, { "MX", "mex-mx" }, { "BR", "sao-br" },
    { "GB", "lon-gb" }, { "DE", "fra-de" }, { "FR", "cdg-fr" }, { "NL", "ams-nl" }, { "PL", "waw-pl" },
    { "ES", "mad-es" }, { "SE", "sto-se" }, { "ZA", "jnb-za" }, { "IL", "tlv-il" },
};
#define DEFAULT_LOC "lax-ca-us"
#define UPLOAD_URL L"https://speed.cloudflare.com/__up"
#define UPLOAD_BYTES 25000000

#define LOG(...) post_fmt(page, WM_APP_LOG, __VA_ARGS__)
#define SECTION(...) section(page, __VA_ARGS__)

static void section(HWND page, const char *fmt, ...)
{
    char title[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(title, sizeof title, fmt, ap);
    va_end(ap);
    post_text(page, WM_APP_LOG, "");
    post_fmt(page, WM_APP_LOG, "=== %s ===", title);
}

typedef struct { int ok; char ip[64], city[64], region[64], country[8], org[128]; } ExitInfo;

static void jcopy(char *dst, size_t n, JVal *o, const char *key)
{
    strncpy(dst, json_str(o, key, ""), n - 1);
    dst[n - 1] = 0;
}

static void public_ip(int family, ExitInfo *e)
{
    memset(e, 0, sizeof *e);
    char *body; size_t len; int status;
    const wchar_t *url = family == 4 ? L"https://api.ipify.org/?format=json" : L"https://api6.ipify.org/?format=json";
    if (!http_get(url, &body, &len, &status, 10)) { free(body); return; }
    JVal *v = json_parse(body);
    free(body);
    if (!v) return;
    jcopy(e->ip, sizeof e->ip, v, "ip");
    json_free(v);
    if (!e->ip[0]) return;
    e->ok = 1;
    wchar_t url2[160], *wip = u8_to_w(e->ip);
    _snwprintf(url2, 160, L"https://ipinfo.io/%s/json", wip);
    free(wip);
    if (http_get(url2, &body, &len, &status, 10) && (v = json_parse(body)) != NULL) {
        jcopy(e->city, sizeof e->city, v, "city");
        jcopy(e->region, sizeof e->region, v, "region");
        jcopy(e->country, sizeof e->country, v, "country");
        jcopy(e->org, sizeof e->org, v, "org");
        json_free(v);
    }
    free(body);
}

static void json_exit(Buf *b, const ExitInfo *e)
{
    if (!e->ok) { buf_puts(b, "null"); return; }
    buf_puts(b, "{\"ip\": "); json_escape(b, e->ip);
    buf_puts(b, ", \"city\": "); json_escape(b, e->city);
    buf_puts(b, ", \"region\": "); json_escape(b, e->region);
    buf_puts(b, ", \"country\": "); json_escape(b, e->country);
    buf_puts(b, ", \"org\": "); json_escape(b, e->org);
    buf_puts(b, "}");
}

typedef struct { int pings; PingStat st[NPING]; HWND page; } CPing;
static void cping_progress(int round, void *p)
{
    CPing *c = p;
    post_fmt(c->page, WM_APP_STATUS, "Ping round %d of %d done...", round, c->pings);
}

typedef struct { const wchar_t *url; volatile LONGLONG total; } DlCtx;
static void dl_one(int i, void *p)
{
    DlCtx *c = p; long long n; double s; int code;
    (void)i;
    http_download(c->url, &n, &s, &code);
    InterlockedExchangeAdd64(&c->total, n);
}

static double mbps(long long bytes, double secs) { return secs > 0 ? (double)bytes * 8 / 1e6 / secs : 0; }
static int iround(double d) { return (int)(d + 0.5); }

static void compare_row(HWND page, const char *label, int has_a, double a, int has_b, double b, const char *unit)
{
    if (!has_a || !has_b) return;
    LOG("  %-22s %7.0f %s %7.0f %s %+8.0f", label, a, unit, b, unit, b - a);
}

static DWORD WINAPI check_thread(LPVOID arg)
{
    CheckJob *j = arg;
    HWND page = j->page;
    Buf out = { 0 };
    SYSTEMTIME st;
    GetLocalTime(&st);
    buf_printf(&out, "{\n  \"time\": \"%04d-%02d-%02d %02d:%02d:%02d\",\n",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    /* 1. adapters */
    SECTION("VPN adapters");
    post_text(page, WM_APP_STATUS, "Listing adapters...");
    Adapter *ad; int nad;
    list_adapters(&ad, &nad);
    int nvpn = 0, nup = 0;
    buf_puts(&out, "  \"vpn_up\": [");
    for (int i = 0; i < nad; i++) {
        if (!ad[i].is_vpn) continue;
        nvpn++;
        LOG("  %-24s %-12s %s", ad[i].name, ad[i].up ? "Up" : "Down", ad[i].desc);
        if (ad[i].up) { if (nup++) buf_puts(&out, ", "); json_escape(&out, ad[i].name); }
    }
    buf_puts(&out, "],\n");
    if (!nvpn) LOG("  none found");
    {
        Buf t = { 0 };
        for (int i = 0; i < nad; i++) if (ad[i].is_vpn && ad[i].up) buf_printf(&t, "%s%s", t.len ? ", " : "", ad[i].name);
        LOG("  tunnel: %s", nup ? t.s : "NOT connected");
        buf_free(&t);
    }

    /* 2. public exit */
    SECTION("Public exit");
    post_text(page, WM_APP_STATUS, "Looking up public IPv4 and IPv6 exit...");
    ExitInfo v4, v6;
    public_ip(4, &v4);
    public_ip(6, &v6);
    buf_puts(&out, "  \"ipv4\": "); json_exit(&out, &v4); buf_puts(&out, ",\n");
    buf_puts(&out, "  \"ipv6\": "); json_exit(&out, &v6); buf_puts(&out, ",\n");
    const ExitInfo *ex[2] = { &v4, &v6 };
    for (int i = 0; i < 2; i++) {
        if (ex[i]->ok) LOG("  %s  %-40s %s, %s  (%s)", i ? "IPv6" : "IPv4", ex[i]->ip,
                           ex[i]->city[0] ? ex[i]->city : "?", ex[i]->country[0] ? ex[i]->country : "?",
                           ex[i]->org[0] ? ex[i]->org : "?");
        else LOG("  %s  no route", i ? "IPv6" : "IPv4");
    }
    if (nup && v4.ok && v6.ok && v6.country[0] && strcmp(v6.country, v4.country) != 0)
        LOG("  WARNING: IPv6 exits in a different country from IPv4. Likely an IPv6 leak.");
    else if (nup && v4.ok && v6.ok && v6.org[0] && strcmp(v6.org, v4.org) != 0)
        LOG("  note: IPv6 and IPv4 exit through different networks (%s vs %s). "
            "Check that both belong to your VPN provider.", v6.org, v4.org);

    /* 3. DNS */
    SECTION("DNS servers");
    buf_puts(&out, "  \"dns\": [");
    int ndns = 0, vpn_dns = 0;
    for (int i = 0; i < nad; i++) {
        if (!ad[i].up || !ad[i].dns[0]) continue;
        LOG("  %-24s %s", ad[i].name, ad[i].dns);
        if (ad[i].is_vpn) vpn_dns = 1;
        if (ndns++) buf_puts(&out, ", ");
        buf_puts(&out, "{\"iface\": "); json_escape(&out, ad[i].name); buf_puts(&out, ", \"servers\": [");
        char *copy = xstrdup(ad[i].dns), *tok = strtok(copy, ", ");
        for (int k = 0; tok; tok = strtok(NULL, ", "), k++) { if (k) buf_puts(&out, ", "); json_escape(&out, tok); }
        free(copy);
        buf_puts(&out, "]}");
    }
    buf_puts(&out, "],\n");
    if (nup && !vpn_dns) LOG("  WARNING: no DNS server bound to the VPN adapter. Lookups may bypass the tunnel.");
    free(ad);

    /* 4. latency */
    SECTION("Latency (%d pings each)", j->pings);
    post_fmt(page, WM_APP_STATUS, "Pinging %d hosts, %d times each...", NPING, j->pings);
    CPing cp; cp.pings = j->pings; cp.page = page;
    ping_hosts(PING_HOSTS, NPING, j->pings, 2000, 100, cp.st, cping_progress, &cp);
    buf_puts(&out, "  \"ping\": {");
    for (int i = 0; i < NPING; i++) {
        PingStat *p = &cp.st[i];
        if (p->has) LOG("  %-22s avg %4d ms   min %4d   max %4d   loss %d%%", PING_HOSTS[i], p->avg, p->min, p->max, p->loss);
        else LOG("  %-22s unreachable (%d%% loss)", PING_HOSTS[i], p->loss);
        buf_printf(&out, "%s\"%s\": {\"host\": \"%s\", \"loss\": %d, ", i ? ", " : "", PING_HOSTS[i], PING_HOSTS[i], p->loss);
        if (p->has) buf_printf(&out, "\"min\": %d, \"avg\": %d, \"max\": %d}", p->min, p->avg, p->max);
        else buf_puts(&out, "\"min\": null, \"avg\": null, \"max\": null}");
    }
    buf_puts(&out, "}");

    /* 5. throughput */
    int has_dl = 0, has_ul = 0, dl_single = 0, dl_multi = 0, ul = 0;
    char url8[256] = "";
    if (!j->nospeed) {
        const char *loc = DEFAULT_LOC;
        for (int i = 0; i < (int)(sizeof VULTR / sizeof VULTR[0]); i++)
            if (v4.ok && strcmp(VULTR[i].cc, v4.country) == 0) loc = VULTR[i].loc;
        _snprintf(url8, sizeof url8, "https://%s-ping.vultr.com/vultr.com.100MB.bin", loc);
        wchar_t *url = u8_to_w(url8);
        SECTION("Download (%s-ping.vultr.com)", loc);
        post_text(page, WM_APP_STATUS, "Downloading, single stream...");
        long long n; double s; int code;
        if (http_download(url, &n, &s, &code)) {
            dl_single = iround(mbps(n, s));
            LOG("  single stream   %6.0f Mbps  (%.1f s)", mbps(n, s), s);
        } else {
            LOG("  single stream   failed (HTTP %d)", code);
        }
        post_fmt(page, WM_APP_STATUS, "Downloading, %d streams...", j->streams);
        DlCtx dc = { url, 0 };
        double t0 = now_seconds();
        run_parallel(j->streams, j->streams, dl_one, &dc);
        s = now_seconds() - t0;
        if (dc.total) {
            dl_multi = iround(mbps(dc.total, s));
            LOG("  %d streams       %6.0f Mbps  (%.1f s)", j->streams, mbps(dc.total, s), s);
        } else {
            LOG("  %d streams       failed", j->streams);
        }
        free(url);
        has_dl = 1;

        SECTION("Upload (speed.cloudflare.com)");
        post_text(page, WM_APP_STATUS, "Uploading 25 MB...");
        if (http_upload(UPLOAD_URL, UPLOAD_BYTES, &s, &code)) {
            ul = iround(mbps(UPLOAD_BYTES, s));
            LOG("  %d MB            %6.0f Mbps  (%.1f s)", UPLOAD_BYTES / 1000000, mbps(UPLOAD_BYTES, s), s);
        } else {
            LOG("  failed (HTTP %d)", code);
        }
        has_ul = 1;
    }
    if (has_dl) {
        buf_printf(&out, ",\n  \"download_mbps\": {\"single\": %d, \"multi\": %d, \"url\": ", dl_single, dl_multi);
        json_escape(&out, url8);
        buf_puts(&out, "}");
    }
    if (has_ul) buf_printf(&out, ",\n  \"upload_mbps\": %d", ul);
    buf_puts(&out, "\n}\n");

    /* 6. compare */
    if (j->compare[0]) {
        char *text = read_text_file(j->compare);
        JVal *old = text ? json_parse(text) : NULL;
        char *name8 = w_to_u8(j->compare);
        if (!old) {
            LOG("");
            LOG("could not read %s", name8);
        } else {
            SECTION("Compared with %s (%s)", name8, json_str(old, "time", "?"));
            LOG("  %-22s %10s %10s %8s", "", "before", "now", "delta");
            JVal *oping = json_get(old, "ping");
            for (int i = 0; i < NPING; i++) {
                double a; char label[64];
                int ha = json_has_num(json_get(oping, PING_HOSTS[i]), "avg", &a);
                _snprintf(label, sizeof label, "ping %s", PING_HOSTS[i]);
                compare_row(page, label, ha, a, cp.st[i].has, cp.st[i].avg, "ms");
            }
            JVal *od = json_get(old, "download_mbps");
            double a;
            compare_row(page, "download single", json_has_num(od, "single", &a), a, has_dl, dl_single, "Mb");
            compare_row(page, "download multi", json_has_num(od, "multi", &a), a, has_dl, dl_multi, "Mb");
            compare_row(page, "upload", json_has_num(old, "upload_mbps", &a), a, has_ul, ul, "Mb");
            JVal *oe = json_get(old, "ipv4");
            const char *oip = json_str(oe, "ip", "");
            if (strcmp(oip, v4.ok ? v4.ip : "") != 0)
                LOG("  exit changed: %s (%s) -> %s (%s)", oip[0] ? oip : "none", json_str(oe, "city", "?"),
                    v4.ok ? v4.ip : "none", v4.ok ? v4.city : "?");
            json_free(old);
        }
        free(name8);
        free(text);
    }

    if (!PostMessageW(page, WM_APP_JSON, 0, (LPARAM)out.s)) buf_free(&out);
    post_text(page, WM_APP_STATUS, "Done.");
    free(j);
    PostMessageW(page, WM_APP_DONE, 0, 0);
    return 0;
}

static void edit_append(HWND e, const wchar_t *s)
{
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)s);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
}

static void start_check_job(HWND page)
{
    CheckJob *j = xmalloc(sizeof *j);
    memset(j, 0, sizeof *j);
    j->page = page;
    j->pings = (int)GetDlgItemInt(page, IDC_CPINGS, NULL, FALSE);
    j->streams = (int)GetDlgItemInt(page, IDC_STREAMS, NULL, FALSE);
    j->nospeed = IsDlgButtonChecked(page, IDC_NOSPEED) == BST_CHECKED;
    GetDlgItemTextW(page, IDC_COMPARE, j->compare, MAX_PATH);
    if (j->pings < 1) j->pings = 1;
    if (j->pings > 100) j->pings = 100;
    if (j->streams < 1) j->streams = 1;
    if (j->streams > 16) j->streams = 16;

    EnableWindow(GetDlgItem(page, IDC_CRUN), FALSE);
    EnableWindow(GetDlgItem(page, IDC_SAVEJSON), FALSE);
    SetDlgItemTextW(page, IDC_OUTPUT, L"");
    free(g_json); g_json = NULL;
    HANDLE th = CreateThread(NULL, 0, check_thread, j, 0, NULL);
    if (th) CloseHandle(th);
    else { free(j); EnableWindow(GetDlgItem(page, IDC_CRUN), TRUE); }
}

static int file_dialog(HWND owner, wchar_t *path, int save)
{
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"JSON files (*.json)\0*.json\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"json";
    ofn.Flags = save ? OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST : OFN_FILEMUSTEXIST;
    return save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
}

static INT_PTR CALLBACK check_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_INITDIALOG: {
        HWND e = GetDlgItem(h, IDC_OUTPUT);
        SendMessageW(e, WM_SETFONT, (WPARAM)g_mono, TRUE);
        SendMessageW(e, EM_SETLIMITTEXT, 0, 0);
        SetDlgItemInt(h, IDC_CPINGS, 10, FALSE);
        SetDlgItemInt(h, IDC_STREAMS, 4, FALSE);
        SetDlgItemTextW(h, IDC_CSTATUS, L"Click Run. Works connected or disconnected; save a disconnected run and compare against it later.");
        layout_init(h, &g_lay2, g_anch2, sizeof g_anch2 / sizeof g_anch2[0]);
        return TRUE;
    }
    case WM_SIZE:
        layout_apply(h, &g_lay2);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_CRUN: start_check_job(h); return TRUE;
        case IDC_BROWSE: {
            wchar_t path[MAX_PATH] = L"";
            if (file_dialog(h, path, 0)) SetDlgItemTextW(h, IDC_COMPARE, path);
            return TRUE;
        }
        case IDC_SAVEJSON: {
            wchar_t path[MAX_PATH] = L"vpn-check.json";
            if (g_json && file_dialog(h, path, 1)) {
                if (write_text_file(path, g_json)) {
                    wchar_t msg[MAX_PATH + 32];
                    _snwprintf(msg, MAX_PATH + 32, L"Saved %s", path);
                    SetDlgItemTextW(h, IDC_CSTATUS, msg);
                } else {
                    MessageBoxW(h, L"Could not write the file.", L"Proton VPN Tools", MB_ICONWARNING);
                }
            }
            return TRUE;
        }
        }
        return FALSE;
    case WM_APP_LOG:
        edit_append(GetDlgItem(h, IDC_OUTPUT), (wchar_t *)l); free((void *)l); return TRUE;
    case WM_APP_STATUS:
        SetDlgItemTextW(h, IDC_CSTATUS, (wchar_t *)l); free((void *)l); return TRUE;
    case WM_APP_JSON:
        free(g_json); g_json = (char *)l;
        EnableWindow(GetDlgItem(h, IDC_SAVEJSON), TRUE);
        return TRUE;
    case WM_APP_DONE:
        EnableWindow(GetDlgItem(h, IDC_CRUN), TRUE);
        return TRUE;
    }
    return FALSE;
}

/* ======================================================================== */
/* Main window                                                              */
/* ======================================================================== */
static void main_layout(HWND h)
{
    RECT rc;
    GetClientRect(h, &rc);
    int m = px(4);
    MoveWindow(g_tab, m, m, rc.right - 2 * m, rc.bottom - 2 * m, TRUE);
    RECT r = { m, m, rc.right - m, rc.bottom - m };
    TabCtrl_AdjustRect(g_tab, FALSE, &r);
    for (int i = 0; i < 2; i++)
        if (g_pages[i]) MoveWindow(g_pages[i], r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
}

static INT_PTR CALLBACK main_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)w;
    switch (m) {
    case WM_INITDIALOG: {
        g_main = h;
        g_tab = GetDlgItem(h, IDC_TAB);
        TCITEMW ti; memset(&ti, 0, sizeof ti);
        ti.mask = TCIF_TEXT;
        ti.pszText = L"Best server";      TabCtrl_InsertItem(g_tab, 0, &ti);
        ti.pszText = L"Connection check"; TabCtrl_InsertItem(g_tab, 1, &ti);
        g_pages[0] = CreateDialogParamW(g_inst, MAKEINTRESOURCEW(IDD_PAGE_SERVERS), h, servers_proc, 0);
        g_pages[1] = CreateDialogParamW(g_inst, MAKEINTRESOURCEW(IDD_PAGE_CHECK), h, check_proc, 0);
        ShowWindow(g_pages[1], SW_HIDE);
        /* The template is sized for 96 DPI; at high DPI it can be taller
         * than the screen. Shrink to fit the work area and re-center. */
        RECT wa, wr;
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0) && GetWindowRect(h, &wr)) {
            int ww = wr.right - wr.left, hh = wr.bottom - wr.top;
            int maxw = (wa.right - wa.left) * 9 / 10, maxh = (wa.bottom - wa.top) * 9 / 10;
            if (ww > maxw) ww = maxw;
            if (hh > maxh) hh = maxh;
            SetWindowPos(h, NULL, wa.left + (wa.right - wa.left - ww) / 2, wa.top + (wa.bottom - wa.top - hh) / 2,
                         ww, hh, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        main_layout(h);
        return TRUE;
    }
    case WM_SIZE:
        if (g_tab) main_layout(h);
        return TRUE;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)l)->ptMinTrackSize.x = px(700);
        ((MINMAXINFO *)l)->ptMinTrackSize.y = px(420);
        return TRUE;
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)l;
        if (nm->idFrom == IDC_TAB && nm->code == TCN_SELCHANGE) {
            int sel = TabCtrl_GetCurSel(g_tab);
            for (int i = 0; i < 2; i++) ShowWindow(g_pages[i], i == sel ? SW_SHOW : SW_HIDE);
            return TRUE;
        }
        return FALSE;
    }
    case WM_CLOSE:
        EndDialog(h, 0);
        return TRUE;
    }
    return FALSE;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    (void)prev; (void)cmd; (void)show;
    g_inst = inst;
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    HDC dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    g_mono = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_MAIN), NULL, main_proc, 0);
    return 0;
}
