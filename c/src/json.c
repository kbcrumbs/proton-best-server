/* Minimal JSON reader and string escaper. Enough for the ipify/ipinfo
 * responses and our own saved result files. */
#include "tools.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct { const char *p; int depth; } P;

static JVal *parse_value(P *ps);

static void skip_ws(P *ps) { while (*ps->p && isspace((unsigned char)*ps->p)) ps->p++; }

static JVal *new_val(JType t)
{
    JVal *v = xmalloc(sizeof *v);
    memset(v, 0, sizeof *v);
    v->type = t;
    return v;
}

static void utf8_put(Buf *b, unsigned cp)
{
    char tmp[4]; int n = 0;
    if (cp < 0x80) tmp[n++] = (char)cp;
    else if (cp < 0x800) { tmp[n++] = (char)(0xC0 | (cp >> 6)); tmp[n++] = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { tmp[n++] = (char)(0xE0 | (cp >> 12)); tmp[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[n++] = (char)(0x80 | (cp & 0x3F)); }
    else { tmp[n++] = (char)(0xF0 | (cp >> 18)); tmp[n++] = (char)(0x80 | ((cp >> 12) & 0x3F)); tmp[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[n++] = (char)(0x80 | (cp & 0x3F)); }
    buf_append(b, tmp, (size_t)n);
}

static int hex4(const char *s, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i]; v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static char *parse_string(P *ps)
{
    if (*ps->p != '"') return NULL;
    ps->p++;
    Buf b = { 0 };
    buf_append(&b, "", 0);
    while (*ps->p && *ps->p != '"') {
        if (*ps->p == '\\') {
            ps->p++;
            char c = *ps->p++;
            switch (c) {
            case '"': buf_append(&b, "\"", 1); break;
            case '\\': buf_append(&b, "\\", 1); break;
            case '/': buf_append(&b, "/", 1); break;
            case 'b': buf_append(&b, "\b", 1); break;
            case 'f': buf_append(&b, "\f", 1); break;
            case 'n': buf_append(&b, "\n", 1); break;
            case 'r': buf_append(&b, "\r", 1); break;
            case 't': buf_append(&b, "\t", 1); break;
            case 'u': {
                unsigned cp;
                if (!hex4(ps->p, &cp)) { buf_free(&b); return NULL; }
                ps->p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                    unsigned lo;
                    if (hex4(ps->p + 2, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        ps->p += 6;
                    }
                }
                utf8_put(&b, cp);
                break;
            }
            default: buf_free(&b); return NULL;
            }
        } else {
            buf_append(&b, ps->p, 1);
            ps->p++;
        }
    }
    if (*ps->p != '"') { buf_free(&b); return NULL; }
    ps->p++;
    return b.s;
}

static void push(JVal *v, const char *key, JVal *item)
{
    v->items = xrealloc(v->items, (size_t)(v->n + 1) * sizeof *v->items);
    if (v->type == J_OBJ) {
        v->keys = xrealloc(v->keys, (size_t)(v->n + 1) * sizeof *v->keys);
        v->keys[v->n] = (char *)key;
    }
    v->items[v->n++] = item;
}

static JVal *parse_value(P *ps)
{
    skip_ws(ps);
    if (ps->depth > 64) return NULL;
    const char *p = ps->p;
    if (*p == '{') {
        ps->p++; ps->depth++;
        JVal *v = new_val(J_OBJ);
        skip_ws(ps);
        if (*ps->p == '}') { ps->p++; ps->depth--; return v; }
        for (;;) {
            skip_ws(ps);
            char *key = parse_string(ps);
            if (!key) { json_free(v); return NULL; }
            skip_ws(ps);
            if (*ps->p != ':') { free(key); json_free(v); return NULL; }
            ps->p++;
            JVal *item = parse_value(ps);
            if (!item) { free(key); json_free(v); return NULL; }
            push(v, key, item);
            skip_ws(ps);
            if (*ps->p == ',') { ps->p++; continue; }
            if (*ps->p == '}') { ps->p++; ps->depth--; return v; }
            json_free(v); return NULL;
        }
    }
    if (*p == '[') {
        ps->p++; ps->depth++;
        JVal *v = new_val(J_ARR);
        skip_ws(ps);
        if (*ps->p == ']') { ps->p++; ps->depth--; return v; }
        for (;;) {
            JVal *item = parse_value(ps);
            if (!item) { json_free(v); return NULL; }
            push(v, NULL, item);
            skip_ws(ps);
            if (*ps->p == ',') { ps->p++; continue; }
            if (*ps->p == ']') { ps->p++; ps->depth--; return v; }
            json_free(v); return NULL;
        }
    }
    if (*p == '"') {
        char *s = parse_string(ps);
        if (!s) return NULL;
        JVal *v = new_val(J_STR);
        v->str = s;
        return v;
    }
    if (strncmp(p, "true", 4) == 0) { ps->p += 4; JVal *v = new_val(J_BOOL); v->num = 1; return v; }
    if (strncmp(p, "false", 5) == 0) { ps->p += 5; return new_val(J_BOOL); }
    if (strncmp(p, "null", 4) == 0) { ps->p += 4; return new_val(J_NULL); }
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        char *end;
        double d = strtod(p, &end);
        if (end == p) return NULL;
        ps->p = end;
        JVal *v = new_val(J_NUM);
        v->num = d;
        return v;
    }
    return NULL;
}

JVal *json_parse(const char *text)
{
    P ps = { text, 0 };
    JVal *v = parse_value(&ps);
    if (!v) return NULL;
    skip_ws(&ps);
    if (*ps.p) { json_free(v); return NULL; }
    return v;
}

void json_free(JVal *v)
{
    if (!v) return;
    for (int i = 0; i < v->n; i++) {
        json_free(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->str);
    free(v);
}

JVal *json_get(JVal *obj, const char *key)
{
    if (!obj || obj->type != J_OBJ) return NULL;
    for (int i = 0; i < obj->n; i++)
        if (strcmp(obj->keys[i], key) == 0) return obj->items[i];
    return NULL;
}

const char *json_str(JVal *obj, const char *key, const char *dflt)
{
    JVal *v = json_get(obj, key);
    return v && v->type == J_STR ? v->str : dflt;
}

int json_has_num(JVal *obj, const char *key, double *out)
{
    JVal *v = json_get(obj, key);
    if (!v || v->type != J_NUM) return 0;
    *out = v->num;
    return 1;
}

void json_escape(Buf *b, const char *s)
{
    buf_append(b, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"': buf_puts(b, "\\\""); break;
        case '\\': buf_puts(b, "\\\\"); break;
        case '\n': buf_puts(b, "\\n"); break;
        case '\r': buf_puts(b, "\\r"); break;
        case '\t': buf_puts(b, "\\t"); break;
        default:
            if (c < 0x20) buf_printf(b, "\\u%04x", c);
            else buf_append(b, (const char *)&c, 1);
        }
    }
    buf_append(b, "\"", 1);
}
