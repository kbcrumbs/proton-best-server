#include "tools.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { MessageBoxW(NULL, L"Out of memory", L"Proton VPN Tools", MB_ICONERROR); ExitProcess(1); }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) { MessageBoxW(NULL, L"Out of memory", L"Proton VPN Tools", MB_ICONERROR); ExitProcess(1); }
    return p;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    return memcpy(xmalloc(n), s, n);
}

wchar_t *u8_to_w(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = xmalloc((n ? n : 1) * sizeof(wchar_t));
    if (n) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n); else w[0] = 0;
    return w;
}

char *w_to_u8(const wchar_t *s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *u = xmalloc(n ? n : 1);
    if (n) WideCharToMultiByte(CP_UTF8, 0, s, -1, u, n, NULL, NULL); else u[0] = 0;
    return u;
}

int str_icontains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n) return 1;
    for (; *hay; hay++)
        if (_strnicmp(hay, needle, n) == 0) return 1;
    return 0;
}

double now_seconds(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

void buf_append(Buf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->len + n + 1) cap *= 2;
        b->s = xrealloc(b->s, cap);
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

void buf_puts(Buf *b, const char *s) { buf_append(b, s, strlen(s)); }

void buf_printf(Buf *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) { buf_append(b, tmp, (size_t)n); return; }
    char *big = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    buf_append(b, big, (size_t)n);
    free(big);
}

void buf_free(Buf *b) { free(b->s); b->s = NULL; b->len = b->cap = 0; }

/* ---- thread pool ------------------------------------------------------- */
typedef struct { LONG next; int n; void (*fn)(int, void *); void *ctx; } Pool;

static DWORD WINAPI pool_worker(LPVOID arg)
{
    Pool *p = arg;
    for (;;) {
        LONG i = InterlockedIncrement(&p->next) - 1;
        if (i >= p->n) break;
        p->fn((int)i, p->ctx);
    }
    return 0;
}

void run_parallel(int n, int workers, void (*fn)(int, void *), void *ctx)
{
    Pool p = { 0, n, fn, ctx };
    HANDLE th[64];
    if (workers > n) workers = n;
    if (workers > 64) workers = 64;
    if (workers < 1) workers = 1;
    int started = 0;
    for (int i = 0; i < workers; i++) {
        th[started] = CreateThread(NULL, 0, pool_worker, &p, 0, NULL);
        if (th[started]) started++;
    }
    if (!started) { pool_worker(&p); return; }
    WaitForMultipleObjects(started, th, TRUE, INFINITE);
    for (int i = 0; i < started; i++) CloseHandle(th[i]);
}

/* ---- files ------------------------------------------------------------- */
char *read_text_file(const wchar_t *path)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 16 * 1024 * 1024) { CloseHandle(h); return NULL; }
    char *buf = xmalloc((size_t)sz.QuadPart + 1);
    DWORD got = 0, total = 0;
    while (total < (DWORD)sz.QuadPart) {
        if (!ReadFile(h, buf + total, (DWORD)sz.QuadPart - total, &got, NULL) || !got) break;
        total += got;
    }
    CloseHandle(h);
    buf[total] = 0;
    if (total >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
        memmove(buf, buf + 3, total - 2);
    return buf;
}

int write_text_file(const wchar_t *path, const char *text)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD len = (DWORD)strlen(text), wrote = 0;
    int ok = WriteFile(h, text, len, &wrote, NULL) && wrote == len;
    CloseHandle(h);
    return ok;
}
