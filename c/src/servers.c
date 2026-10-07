/* Proton VPN Windows client server cache: locate, parse (protobuf wire
 * format, no schema needed), filter and group by physical entry node. */
#include "tools.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- protobuf wire reader --------------------------------------------- */
typedef struct { const unsigned char *p, *end; } Cur;
typedef struct { unsigned field, wtype; unsigned long long varint; const unsigned char *data; size_t len; } Field;

static int read_varint(Cur *c, unsigned long long *out)
{
    unsigned long long v = 0; int shift = 0;
    while (c->p < c->end && shift < 64) {
        unsigned char b = *c->p++;
        v |= (unsigned long long)(b & 0x7F) << shift;
        if (!(b & 0x80)) { *out = v; return 1; }
        shift += 7;
    }
    return 0;
}

static int next_field(Cur *c, Field *f)
{
    unsigned long long key;
    if (c->p >= c->end || !read_varint(c, &key)) return 0;
    f->field = (unsigned)(key >> 3);
    f->wtype = (unsigned)(key & 7);
    f->data = NULL; f->len = 0; f->varint = 0;
    switch (f->wtype) {
    case 0: return read_varint(c, &f->varint);
    case 1: if (c->end - c->p < 8) return 0; f->data = c->p; f->len = 8; c->p += 8; return 1;
    case 2: {
        unsigned long long n;
        if (!read_varint(c, &n) || n > (unsigned long long)(c->end - c->p)) return 0;
        f->data = c->p; f->len = (size_t)n; c->p += n; return 1;
    }
    case 5: if (c->end - c->p < 4) return 0; f->data = c->p; f->len = 4; c->p += 4; return 1;
    default: return 0;
    }
}

static void copy_str(char *dst, size_t cap, const Field *f)
{
    size_t n = f->len < cap - 1 ? f->len : cap - 1;
    memcpy(dst, f->data, n);
    dst[n] = 0;
}

/* ---- cache file -------------------------------------------------------- */
int find_cache(wchar_t *path, size_t n, FILETIME *mtime)
{
    wchar_t base[MAX_PATH], pattern[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!len || len >= MAX_PATH) return 0;
    _snwprintf(pattern, MAX_PATH, L"%s\\Proton\\Proton VPN\\Storage\\Servers.*.bin", base);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FILETIME best = { 0, 0 };
    int found = 0;
    do {
        if (!found || CompareFileTime(&fd.ftLastWriteTime, &best) > 0) {
            best = fd.ftLastWriteTime;
            _snwprintf(path, n, L"%s\\Proton\\Proton VPN\\Storage\\%s", base, fd.cFileName);
            found = 1;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (mtime) *mtime = best;
    return found;
}

static unsigned char *read_file(const wchar_t *path, size_t *len)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 256 * 1024 * 1024) { CloseHandle(h); return NULL; }
    unsigned char *buf = xmalloc((size_t)sz.QuadPart + 1);
    DWORD got = 0, total = 0;
    while (total < (DWORD)sz.QuadPart) {
        if (!ReadFile(h, buf + total, (DWORD)sz.QuadPart - total, &got, NULL) || !got) break;
        total += got;
    }
    CloseHandle(h);
    *len = total;
    return buf;
}

/* Field numbers observed in the Windows client v5.1.x cache:
 *   top-level 3 = LogicalServer
 *     2 name, 3 city, 5 entry country, 6 exit country,
 *     13 tier, 14 features bitmask, 15 load %, 16 score (float32),
 *     17 physical node (repeated): 2 entry IP, 4 domain, 6 status */
int load_servers(const wchar_t *path, Server **out, int *count)
{
    size_t len;
    unsigned char *data = read_file(path, &len);
    if (!data) return 0;
    Server *servers = NULL; int n = 0, cap = 0;
    Cur top = { data, data + len };
    Field f;
    while (next_field(&top, &f)) {
        if (f.field != 3 || f.wtype != 2) continue;
        Server s; memset(&s, 0, sizeof s);
        s.tier = -1;
        Cur sc = { f.data, f.data + f.len };
        Field g;
        while (next_field(&sc, &g)) {
            switch (g.field) {
            case 2: if (g.wtype == 2) copy_str(s.name, sizeof s.name, &g); break;
            case 3: if (g.wtype == 2) copy_str(s.city, sizeof s.city, &g); break;
            case 5: if (g.wtype == 2) copy_str(s.entry, sizeof s.entry, &g); break;
            case 6: if (g.wtype == 2) copy_str(s.exit_, sizeof s.exit_, &g); break;
            case 13: if (g.wtype == 0) s.tier = (int)g.varint; break;
            case 14: if (g.wtype == 0) s.features = (int)g.varint; break;
            case 15: if (g.wtype == 0) { s.load = (int)g.varint; s.has_load = 1; } break;
            case 16: if (g.wtype == 5) { memcpy(&s.score, g.data, 4); s.has_score = 1; } break;
            case 17:
                if (g.wtype == 2) {
                    Node nd; memset(&nd, 0, sizeof nd); nd.status = 1;
                    Cur nc = { g.data, g.data + g.len };
                    Field h;
                    while (next_field(&nc, &h)) {
                        if (h.field == 2 && h.wtype == 2) copy_str(nd.ip, sizeof nd.ip, &h);
                        else if (h.field == 4 && h.wtype == 2) copy_str(nd.domain, sizeof nd.domain, &h);
                        else if (h.field == 6 && h.wtype == 0) nd.status = (int)h.varint;
                    }
                    if (nd.ip[0]) {
                        s.nodes = xrealloc(s.nodes, (size_t)(s.nnodes + 1) * sizeof(Node));
                        s.nodes[s.nnodes++] = nd;
                    }
                }
                break;
            }
        }
        if (s.name[0]) {
            if (n == cap) { cap = cap ? cap * 2 : 256; servers = xrealloc(servers, (size_t)cap * sizeof(Server)); }
            servers[n++] = s;
        } else {
            free(s.nodes);
        }
    }
    free(data);
    *out = servers; *count = n;
    return 1;
}

void free_servers(Server *s, int count)
{
    for (int i = 0; i < count; i++) free(s[i].nodes);
    free(s);
}

/* ---- grouping ---------------------------------------------------------- */
static NodeRow *find_row(NodeRow *rows, int n, const char *ip)
{
    for (int i = 0; i < n; i++) if (strcmp(rows[i].ip, ip) == 0) return &rows[i];
    return NULL;
}

static void add_name(NodeRow *r, const char *name)
{
    for (int i = 0; i < r->nnames; i++) if (strcmp(r->names[i], name) == 0) return;
    r->names = xrealloc(r->names, (size_t)(r->nnames + 1) * sizeof(char *));
    r->names[r->nnames++] = xstrdup(name);
}

int group_servers(Server *s, int count, const ServerFilter *f, NodeRow **out, int *nout)
{
    NodeRow *rows = NULL; int n = 0, cap = 0;
    for (int i = 0; i < count; i++) {
        Server *sv = &s[i];
        if (_stricmp(sv->exit_, f->country) != 0) continue;
        if ((sv->features & F_SECURE_CORE) && !f->include_secure_core) continue;
        if (sv->tier == 0 && !f->include_free) continue;
        for (int k = 0; k < sv->nnodes; k++) {
            Node *nd = &sv->nodes[k];
            if (nd->status != 1) continue;
            NodeRow *r = find_row(rows, n, nd->ip);
            if (!r) {
                if (n == cap) { cap = cap ? cap * 2 : 32; rows = xrealloc(rows, (size_t)cap * sizeof(NodeRow)); }
                r = &rows[n++];
                memset(r, 0, sizeof *r);
                strcpy(r->ip, nd->ip);
                strcpy(r->domain, nd->domain);
                strcpy(r->city, sv->city);
                strcpy(r->entry, sv->entry);
                r->tier = sv->tier;
                r->features = sv->features;
                r->ping.loss = 100;
            }
            add_name(r, sv->name);
            if (sv->has_load) { r->load_sum += sv->load; r->load_n++; }
            if (sv->has_score && (!r->has_score || sv->score < r->score)) { r->score = sv->score; r->has_score = 1; }
        }
    }
    for (int i = 0; i < n; i++) {
        if (rows[i].load_n) { rows[i].load = (rows[i].load_sum + rows[i].load_n / 2) / rows[i].load_n; rows[i].has_load = 1; }
        sort_names(rows[i].names, rows[i].nnames);
    }
    *out = rows; *nout = n;
    return n > 0;
}

void free_rows(NodeRow *rows, int n)
{
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < rows[i].nnames; k++) free(rows[i].names[k]);
        free(rows[i].names);
    }
    free(rows);
}

static int cmp_name(const void *a, const void *b)
{
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    size_t lx = strlen(x), ly = strlen(y);
    if (lx != ly) return lx < ly ? -1 : 1;
    return strcmp(x, y);
}

void sort_names(char **names, int n) { if (n > 1) qsort(names, (size_t)n, sizeof *names, cmp_name); }

static int cmp_row(const void *a, const void *b)
{
    const NodeRow *x = a, *y = b;
    if (x->ping.has != y->ping.has) return x->ping.has ? -1 : 1;
    if (x->ping.has && x->ping.avg != y->ping.avg) return x->ping.avg < y->ping.avg ? -1 : 1;
    if (x->ping.loss != y->ping.loss) return x->ping.loss < y->ping.loss ? -1 : 1;
    return strcmp(x->ip, y->ip);
}

void sort_rows(NodeRow *rows, int n) { if (n > 1) qsort(rows, (size_t)n, sizeof *rows, cmp_row); }

const char *tier_name(int tier)
{
    switch (tier) { case 0: return "Free"; case 1: return "Basic"; case 2: return "Plus"; default: return "?"; }
}

static void cat(char *out, size_t n, const char *s)
{
    size_t len = strlen(out);
    if (len + 1 >= n) return;
    strncpy(out + len, s, n - len - 1);
    out[n - 1] = 0;
}

void flags_text(const NodeRow *r, char *out, size_t n)
{
    out[0] = 0;
    if (r->features & F_SECURE_CORE) { cat(out, n, "SC via "); cat(out, n, r->entry); }
    if (r->features & F_P2P)       { if (out[0]) cat(out, n, ", "); cat(out, n, "P2P"); }
    if (r->features & F_STREAMING) { if (out[0]) cat(out, n, ", "); cat(out, n, "Stream"); }
    if (r->features & F_TOR)       { if (out[0]) cat(out, n, ", "); cat(out, n, "Tor"); }
}
