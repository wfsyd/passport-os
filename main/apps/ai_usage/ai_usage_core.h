// AI usage helper: bounded parser and text helpers with no ESP-IDF dependency.
// Host tests: tests/test_ai_usage_core.c.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The PC helper answers with about 200 bytes; anything larger is rejected.
#define AI_USAGE_BODY_MAX 1024
#define AI_USAGE_URL_MAX 128        // bytes including the terminating zero
#define AI_USAGE_TIME_MAX 8         // "HH:MM" plus terminating zero
#define AI_USAGE_WEEK_MAX 16        // "Mon 09:30" plus terminating zero

#define AI_USAGE_UNSET (-1)         // value absent from the response

typedef struct {
    int32_t tok_today;
    int32_t tok_5h;
    char reset[AI_USAGE_TIME_MAX];  // empty when no 5 h block is active
    char last[AI_USAGE_TIME_MAX];
} ai_usage_claude_t;

typedef struct {
    int32_t p5h;                    // percent used, 0..100
    int32_t pwk;                    // percent used, 0..100
    int32_t tok_today;
    char reset[AI_USAGE_TIME_MAX];
    char reset_wk[AI_USAGE_WEEK_MAX];
} ai_usage_codex_t;

typedef struct {
    bool has_claude;
    bool has_codex;
    ai_usage_claude_t claude;
    ai_usage_codex_t codex;
} ai_usage_t;

// Parses one usage.json body (not necessarily zero-terminated). Requires
// "v":1. Missing fields become AI_USAGE_UNSET or empty strings. Values that
// are out of range or malformed make the whole body invalid.
bool ai_usage_parse(const char *body, size_t len, ai_usage_t *out);

// Empty string is valid (feature off). Otherwise it must begin with
// "http://", be shorter than AI_USAGE_URL_MAX and contain only printable
// ASCII without spaces.
bool ai_usage_url_valid(const char *url);

// "885", "4.5k", "1.2M". Writes at most cap bytes including the zero.
void ai_usage_format_tokens(int32_t tokens, char *out, size_t cap);

// "just now", "5 min ago", "2 h ago". Seconds since the last good update.
void ai_usage_format_age(uint32_t seconds, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
