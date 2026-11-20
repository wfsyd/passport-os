#include "apps/ai_usage/ai_usage_core.h"

#include <stdio.h>
#include <string.h>

// The body is a small, flat schema written by tools/ai_usage_helper. Each
// field is looked up by its quoted key inside the "claude" or "codex" object.
// Nested objects are rejected, so a scan never recurses and never allocates.

typedef struct {
    const char *begin;
    const char *end;
} span_t;

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    return p;
}

// Returns the value start for "key": inside [begin, end), or NULL.
static const char *find_value(const char *begin, const char *end, const char *key)
{
    const size_t klen = strlen(key);
    for (const char *p = begin; p + klen + 2 <= end; ++p) {
        if (p[0] != '"' || p[klen + 1] != '"' || memcmp(p + 1, key, klen) != 0) continue;
        const char *colon = skip_ws(p + klen + 2, end);
        if (colon >= end || *colon != ':') continue;
        return skip_ws(colon + 1, end);
    }
    return NULL;
}

// Value must be an object without nested objects or arrays.
static bool object_value(const char *v, const char *end, span_t *out)
{
    if (!v || v >= end || *v != '{') return false;
    for (const char *p = v + 1; p < end; ++p) {
        if (*p == '{' || *p == '[') return false;
        if (*p == '}') {
            out->begin = v + 1;
            out->end = p;
            return true;
        }
    }
    return false;
}

static bool int_value(const char *v, const char *end, int32_t max, int32_t *out)
{
    if (!v || v >= end || *v < '0' || *v > '9') return false;
    int64_t n = 0;
    int digits = 0;
    const char *p = v;
    for (; p < end && *p >= '0' && *p <= '9'; ++p) {
        n = n * 10 + (*p - '0');
        if (++digits > 10 || n > max) return false;
    }
    // The number must end at a separator, not at a letter or a decimal point.
    if (p < end && *p != ',' && *p != '}' && *p != ' ' && *p != '\t' &&
        *p != '\r' && *p != '\n') return false;
    *out = (int32_t)n;
    return true;
}

// Copies a printable ASCII string without escapes. Returns false if it does
// not fit in cap bytes including the terminating zero.
static bool string_value(const char *v, const char *end, char *out, size_t cap)
{
    if (!v || v >= end || *v != '"') return false;
    const char *p = v + 1;
    size_t n = 0;
    for (; p < end && *p != '"'; ++p) {
        if (*p < 0x20 || *p > 0x7e || *p == '\\') return false;
        if (++n >= cap) return false;
    }
    if (p >= end) return false;
    memcpy(out, v + 1, n);
    out[n] = '\0';
    return true;
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

// "HH:MM" in 24 hour time.
static bool clock_text(const char *s)
{
    return strlen(s) == 5 && is_digit(s[0]) && is_digit(s[1]) && s[2] == ':' &&
           is_digit(s[3]) && is_digit(s[4]) &&
           (s[0] - '0') * 10 + (s[1] - '0') < 24 && (s[3] - '0') * 10 + (s[4] - '0') < 60;
}

// Optional string time: empty or HH:MM.
static bool optional_clock(const span_t *s, const char *key, char *out, size_t cap)
{
    out[0] = '\0';
    const char *v = find_value(s->begin, s->end, key);
    if (!v) return true;
    return string_value(v, s->end, out, cap) && (out[0] == '\0' || clock_text(out));
}

// Optional integer: absent stays AI_USAGE_UNSET, malformed rejects the body.
static bool optional_int(const span_t *s, const char *key, int32_t max, int32_t *out)
{
    const char *v = find_value(s->begin, s->end, key);
    if (!v) return true;
    return int_value(v, s->end, max, out);
}

static bool parse_claude(const span_t *s, ai_usage_claude_t *out)
{
    out->tok_today = AI_USAGE_UNSET;
    out->tok_5h = AI_USAGE_UNSET;
    return optional_int(s, "tok_today", INT32_MAX, &out->tok_today) &&
           optional_int(s, "tok_5h", INT32_MAX, &out->tok_5h) &&
           optional_clock(s, "reset", out->reset, sizeof(out->reset)) &&
           optional_clock(s, "last", out->last, sizeof(out->last));
}

// "Mon 09:30": three letters, a space and a clock time.
static bool week_text(const char *s)
{
    if (strlen(s) != 9) return false;
    for (int i = 0; i < 3; ++i) {
        const char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return false;
    }
    return s[3] == ' ' && clock_text(s + 4);
}

static bool parse_codex(const span_t *s, ai_usage_codex_t *out)
{
    out->p5h = AI_USAGE_UNSET;
    out->pwk = AI_USAGE_UNSET;
    out->tok_today = AI_USAGE_UNSET;
    if (!optional_int(s, "p5h", 100, &out->p5h) ||
        !optional_int(s, "pwk", 100, &out->pwk) ||
        !optional_int(s, "tok_today", INT32_MAX, &out->tok_today) ||
        !optional_clock(s, "reset", out->reset, sizeof(out->reset))) return false;
    const char *v = find_value(s->begin, s->end, "reset_wk");
    if (!v) return true;
    return string_value(v, s->end, out->reset_wk, sizeof(out->reset_wk)) &&
           (out->reset_wk[0] == '\0' || week_text(out->reset_wk));
}

static bool section(const char *body, const char *end, const char *key, span_t *out,
                    bool *present)
{
    const char *v = find_value(body, end, key);
    *present = v != NULL;
    if (!v) return true;
    return object_value(v, end, out);
}

bool ai_usage_parse(const char *body, size_t len, ai_usage_t *out)
{
    if (!body || !out || len == 0 || len > AI_USAGE_BODY_MAX) return false;
    const char *end = body + len;
    memset(out, 0, sizeof(*out));
    out->claude.tok_today = AI_USAGE_UNSET;
    out->claude.tok_5h = AI_USAGE_UNSET;
    out->codex.p5h = AI_USAGE_UNSET;
    out->codex.pwk = AI_USAGE_UNSET;
    out->codex.tok_today = AI_USAGE_UNSET;
    const char *v = find_value(body, end, "v");
    int32_t version = 0;
    if (!v || !int_value(v, end, 9, &version) || version != 1) return false;

    span_t span;
    if (!section(body, end, "claude", &span, &out->has_claude)) return false;
    if (out->has_claude && !parse_claude(&span, &out->claude)) return false;
    if (!section(body, end, "codex", &span, &out->has_codex)) return false;
    if (out->has_codex && !parse_codex(&span, &out->codex)) return false;
    return true;
}

bool ai_usage_url_valid(const char *url)
{
    if (!url) return false;
    if (url[0] == '\0') return true;
    static const char prefix[] = "http://";
    const size_t plen = sizeof(prefix) - 1;
    size_t n = 0;
    for (; url[n] != '\0'; ++n) {
        if (n >= AI_USAGE_URL_MAX - 1) return false;
        if (url[n] <= 0x20 || url[n] > 0x7e || url[n] == '"' || url[n] == '\\') return false;
    }
    return n > plen && memcmp(url, prefix, plen) == 0;
}

void ai_usage_format_tokens(int32_t tokens, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (tokens < 0) {
        snprintf(out, cap, "--");
    } else if (tokens < 1000) {
        snprintf(out, cap, "%ld", (long)tokens);
    } else if (tokens < 1000000) {
        snprintf(out, cap, "%ld.%ldk", (long)(tokens / 1000), (long)((tokens % 1000) / 100));
    } else {
        snprintf(out, cap, "%ld.%ldM", (long)(tokens / 1000000), (long)((tokens % 1000000) / 100000));
    }
}

void ai_usage_format_age(uint32_t seconds, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (seconds < 60) {
        snprintf(out, cap, "just now");
    } else if (seconds < 3600) {
        snprintf(out, cap, "%lu min ago", (unsigned long)(seconds / 60));
    } else {
        snprintf(out, cap, "%lu h ago", (unsigned long)(seconds / 3600));
    }
}
