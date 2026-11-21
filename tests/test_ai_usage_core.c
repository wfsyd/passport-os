// Host tests for the AI usage parser: bounded input, strict fields, no heap.
#include "apps/ai_usage/ai_usage_core.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool parse_str(const char *s, ai_usage_t *out)
{
    return ai_usage_parse(s, strlen(s), out);
}

static void check_full_example(void)
{
    const char *body =
        "{\"v\":1,\"t\":1795134600,\"claude\":{\"tok_today\":885,\"tok_5h\":435,"
        "\"reset\":\"11:00\",\"last\":\"09:00\"},\"codex\":{\"p5h\":18,\"pwk\":42,"
        "\"reset\":\"13:00\",\"reset_wk\":\"Mon 09:30\",\"tok_today\":4500}}";
    ai_usage_t u;
    assert(parse_str(body, &u));
    assert(u.has_claude && u.has_codex);
    assert(u.claude.tok_today == 885 && u.claude.tok_5h == 435);
    assert(strcmp(u.claude.reset, "11:00") == 0 && strcmp(u.claude.last, "09:00") == 0);
    assert(u.codex.p5h == 18 && u.codex.pwk == 42 && u.codex.tok_today == 4500);
    assert(strcmp(u.codex.reset, "13:00") == 0);
    assert(strcmp(u.codex.reset_wk, "Mon 09:30") == 0);
}

static void check_cache_free_fixture_matches_helper(void)
{
    // Same numbers the helper's unit test expects after cache reads are excluded.
    const char *body = "{\"v\":1,\"t\":1,\"claude\":{\"tok_today\":885,\"tok_5h\":435}}";
    ai_usage_t u;
    assert(parse_str(body, &u));
    assert(u.has_claude && !u.has_codex);
    assert(u.claude.tok_5h == 435);
    assert(u.codex.p5h == AI_USAGE_UNSET);
    assert(u.claude.reset[0] == '\0');
}

static void check_inactive_and_empty(void)
{
    ai_usage_t u;
    assert(parse_str("{\"v\":1,\"t\":5}", &u));
    assert(!u.has_claude && !u.has_codex);
    assert(parse_str("{\"v\":1,\"t\":5,\"claude\":{\"tok_today\":0}}", &u));
    assert(u.claude.tok_today == 0 && u.claude.tok_5h == AI_USAGE_UNSET);
    assert(parse_str("{\"v\":1,\"codex\":{\"tok_today\":7,\"reset_wk\":\"\"}}", &u));
    assert(u.codex.reset_wk[0] == '\0' && u.codex.tok_today == 7);
}

static void check_rejects_bad_bodies(void)
{
    ai_usage_t u;
    assert(!parse_str("", &u));
    assert(!parse_str("{\"t\":1}", &u));                       // no version
    assert(!parse_str("{\"v\":2,\"t\":1}", &u));               // unknown version
    assert(!parse_str("{\"v\":1,\"claude\":{\"tok_5h\":-4}}", &u));
    assert(!parse_str("{\"v\":1,\"claude\":{\"tok_5h\":\"12\"}}", &u));
    assert(!parse_str("{\"v\":1,\"claude\":{\"tok_5h\":12x}}", &u));
    assert(!parse_str("{\"v\":1,\"claude\":{\"tok_5h\":99999999999}}", &u));
    assert(!parse_str("{\"v\":1,\"codex\":{\"p5h\":101}}", &u));
    assert(!parse_str("{\"v\":1,\"codex\":{\"pwk\":-1}}", &u));
    assert(!parse_str("{\"v\":1,\"claude\":{\"reset\":\"25:00\"}}", &u));
    assert(!parse_str("{\"v\":1,\"claude\":{\"reset\":\"9:00\"}}", &u));
    assert(!parse_str("{\"v\":1,\"codex\":{\"reset_wk\":\"Monday 09:30\"}}", &u));
    assert(!parse_str("{\"v\":1,\"claude\":{\"reset\":\"11:00\\\"\"}}", &u));
    assert(!parse_str("{\"v\":1,\"claude\":{\"x\":{\"tok_5h\":1}}}", &u)); // nested object
    assert(!parse_str("{\"v\":1,\"claude\":[1,2]}", &u));
    assert(!parse_str("{\"v\":1,\"claude\":{\"last\":\"09:00\"", &u));    // truncated
}

static void check_size_limit(void)
{
    char big[AI_USAGE_BODY_MAX + 64];
    memset(big, ' ', sizeof(big));
    memcpy(big, "{\"v\":1}", 7);
    ai_usage_t u;
    assert(!ai_usage_parse(big, sizeof(big), &u));
    assert(!ai_usage_parse(big, AI_USAGE_BODY_MAX + 1, &u));
    assert(ai_usage_parse(big, AI_USAGE_BODY_MAX, &u));
    assert(!ai_usage_parse(NULL, 0, &u));
    assert(!ai_usage_parse("{\"v\":1}", 0, &u));
}

static void check_urls(void)
{
    assert(ai_usage_url_valid(""));
    assert(ai_usage_url_valid("http://192.168.1.20:8787/usage.json?t=xxx"));
    assert(!ai_usage_url_valid("https://192.168.1.20:8787/usage.json"));
    assert(!ai_usage_url_valid("http://"));
    assert(!ai_usage_url_valid("192.168.1.20:8787"));
    assert(!ai_usage_url_valid("http://a b"));
    assert(!ai_usage_url_valid("http://a\"b"));
    char url[AI_USAGE_URL_MAX + 1];
    memset(url, 'a', sizeof(url));
    memcpy(url, "http://", 7);
    url[AI_USAGE_URL_MAX - 1] = '\0';   // 127 characters: allowed
    assert(ai_usage_url_valid(url));
    url[AI_USAGE_URL_MAX - 1] = 'a';
    url[AI_USAGE_URL_MAX] = '\0';        // 128 characters: too long
    assert(!ai_usage_url_valid(url));
    assert(!ai_usage_url_valid(NULL));
}

static void check_text(void)
{
    char s[24];
    ai_usage_format_tokens(0, s, sizeof(s)); assert(strcmp(s, "0") == 0);
    ai_usage_format_tokens(885, s, sizeof(s)); assert(strcmp(s, "885") == 0);
    ai_usage_format_tokens(4500, s, sizeof(s)); assert(strcmp(s, "4.5k") == 0);
    ai_usage_format_tokens(999999, s, sizeof(s)); assert(strcmp(s, "999.9k") == 0);
    ai_usage_format_tokens(1234567, s, sizeof(s)); assert(strcmp(s, "1.2M") == 0);
    ai_usage_format_tokens(-1, s, sizeof(s)); assert(strcmp(s, "--") == 0);
    ai_usage_format_age(0, s, sizeof(s)); assert(strcmp(s, "just now") == 0);
    ai_usage_format_age(59, s, sizeof(s)); assert(strcmp(s, "just now") == 0);
    ai_usage_format_age(60, s, sizeof(s)); assert(strcmp(s, "1 min ago") == 0);
    ai_usage_format_age(3599, s, sizeof(s)); assert(strcmp(s, "59 min ago") == 0);
    ai_usage_format_age(7300, s, sizeof(s)); assert(strcmp(s, "2 h ago") == 0);
    char tiny[4];
    ai_usage_format_tokens(4500, tiny, sizeof(tiny));
    assert(strlen(tiny) < sizeof(tiny));
}

int main(void)
{
    check_full_example();
    check_cache_free_fixture_matches_helper();
    check_inactive_and_empty();
    check_rejects_bad_bodies();
    check_size_limit();
    check_urls();
    check_text();
    puts("AI usage core tests: PASS");
    return 0;
}
