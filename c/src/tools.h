/* Shared declarations for the Proton VPN tools. All strings are UTF-8 unless
 * the type is wchar_t. */
#ifndef TOOLS_H
#define TOOLS_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>

/* ---- util.c ------------------------------------------------------------ */
void   *xmalloc(size_t n);
void   *xrealloc(void *p, size_t n);
char   *xstrdup(const char *s);
wchar_t *u8_to_w(const char *s);          /* malloc'd */
char   *w_to_u8(const wchar_t *s);        /* malloc'd */
int     str_icontains(const char *hay, const char *needle);
double  now_seconds(void);
char   *read_text_file(const wchar_t *path);   /* malloc'd, NULL on error */
int     write_text_file(const wchar_t *path, const char *text);

/* Growable text buffer. */
typedef struct { char *s; size_t len, cap; } Buf;
void buf_append(Buf *b, const char *s, size_t n);
void buf_puts(Buf *b, const char *s);
void buf_printf(Buf *b, const char *fmt, ...);
void buf_free(Buf *b);

/* Run fn(i, ctx) for i in [0, n) on up to `workers` threads. */
void run_parallel(int n, int workers, void (*fn)(int, void *), void *ctx);

/* ---- ping.c ------------------------------------------------------------ */
typedef struct {
    int sent, recv, loss;    /* loss in percent */
    int has;                 /* 1 if at least one reply */
    int min, max, avg;       /* ms, valid when has */
} PingStat;
/* Ping n hosts round-robin, `count` echoes each, one in flight at a time
 * (concurrent echoes from one process lose replies; see ping.c). progress,
 * if given, is called after each completed round with the round number. */
void ping_hosts(const char **hosts, int n, int count, int timeout_ms, int gap_ms,
                PingStat *out, void (*progress)(int, void *), void *ctx);

/* ---- servers.c --------------------------------------------------------- */
#define F_SECURE_CORE 1
#define F_TOR         2
#define F_P2P         4
#define F_STREAMING   8
#define F_IPV6       16

typedef struct { char ip[48]; char domain[96]; int status; } Node;
typedef struct {
    char name[40], city[64], entry[8], exit_[8];
    int tier, features, load, has_load, has_score;
    float score;
    Node *nodes; int nnodes;
} Server;

/* Returns 1 and fills path/mtime if a cache file exists. */
int  find_cache(wchar_t *path, size_t n, FILETIME *mtime);
int  load_servers(const wchar_t *path, Server **out, int *count);
void free_servers(Server *s, int count);

/* One physical entry node after grouping. */
typedef struct {
    char ip[48], domain[96], city[64], entry[8];
    int tier, features;
    char **names; int nnames;
    int load_sum, load_n, load, has_load;
    float score; int has_score;
    PingStat ping;
} NodeRow;

typedef struct {
    const char *country;
    int include_free, include_secure_core;
} ServerFilter;

int  group_servers(Server *s, int count, const ServerFilter *f, NodeRow **out, int *n);
void free_rows(NodeRow *rows, int n);
void sort_rows(NodeRow *rows, int n);            /* by avg, unreachable last */
void sort_names(char **names, int n);            /* by length then text */
const char *tier_name(int tier);
void flags_text(const NodeRow *r, char *out, size_t n);

/* ---- net.c ------------------------------------------------------------- */
typedef struct {
    char name[128], desc[256], dns[256];  /* dns: comma-separated IPv4 list */
    int up, is_vpn;
} Adapter;
int  list_adapters(Adapter **out, int *n);
/* Name of an active Proton/WireGuard/TAP adapter, or NULL. Caller frees. */
char *vpn_adapter_up(void);

/* HTTP via WinHTTP. All return 1 on success (2xx). status receives the HTTP
 * code, or 0 if the request never completed. */
int http_get(const wchar_t *url, char **body, size_t *len, int *status, int timeout_s);
int http_download(const wchar_t *url, long long *bytes, double *seconds, int *status);
int http_upload(const wchar_t *url, size_t nbytes, double *seconds, int *status);

int copy_to_clipboard(const char *text);

/* ---- json.c ------------------------------------------------------------ */
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;
typedef struct JVal {
    JType type;
    double num;                /* J_NUM, J_BOOL */
    char *str;                 /* J_STR */
    struct JVal **items; char **keys; int n;   /* J_ARR / J_OBJ */
} JVal;
JVal *json_parse(const char *text);
void  json_free(JVal *v);
JVal *json_get(JVal *obj, const char *key);     /* NULL if absent */
const char *json_str(JVal *obj, const char *key, const char *dflt);
int   json_has_num(JVal *obj, const char *key, double *out);
void  json_escape(Buf *b, const char *s);       /* writes quoted string */

#endif
