/**
 * @file test_mqtt_topics.c
 * @brief Tests for MQTT topic building, sanitising and matching
 *
 * The previous version of this file tested nothing. It defined its own
 * MQTT_BASE_TOPIC and its own static build_device_state_topic() and friends,
 * built topics with snprintf, and asserted on those — so it passed regardless
 * of what mqtt_topics.c did, and would have kept passing if the module had
 * been deleted outright. These tests call the real API.
 *
 * Two things are worth covering rather than assuming:
 *
 * 1. **Truncation.** Every builder is an snprintf into a caller's buffer and
 *    has to report ESP_ERR_NO_MEM rather than hand back a shortened topic. A
 *    truncated topic is not a broken string — it is a valid topic naming a
 *    different device, which is how a state update gets published under
 *    someone else's name. This project has already had one truncation bug on a
 *    path built exactly this way.
 *
 * 2. **Sanitising.** Friendly names come from users and end up in topics. The
 *    MQTT wildcards + and # and the separator / have to be neutralised, or a
 *    device named "Lamp/+" yields a topic overlapping subscriptions it should
 *    never match.
 *
 * @copyright Copyright (c) 2026
 * @license Apache License 2.0
 */

#include "../test_framework.h"
#include "mqtt/mqtt_topics.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "TEST_MQTT_TOPICS";

/** Fill byte used to prove a builder did not write past the length it was given. */
#define CANARY 0x5A

/**
 * @brief Buffer with guard bytes past the usable end
 *
 * The builder is given @c usable as its buf_len; everything from buf[usable]
 * onwards must still read as CANARY afterwards.
 */
typedef struct {
    char   buf[MQTT_TOPIC_MAX_LEN + 8];
    size_t usable;
} guarded_buf_t;

static void guarded_init(guarded_buf_t *g, size_t usable)
{
    memset(g->buf, CANARY, sizeof(g->buf));
    g->usable = usable;
}

static bool guard_intact(const guarded_buf_t *g)
{
    for (size_t i = g->usable; i < sizeof(g->buf); i++) {
        if ((unsigned char)g->buf[i] != CANARY) {
            return false;
        }
    }
    return true;
}

/* ============================================================================
 * Topic construction
 * ============================================================================ */

static void test_state_topic_shape(void)
{
    char topic[MQTT_TOPIC_MAX_LEN];
    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_device_state("Living Room", topic, sizeof(topic)));

    size_t base_len = strlen(MQTT_BASE_TOPIC);
    TEST_ASSERT_EQUAL(0, strncmp(topic, MQTT_BASE_TOPIC, base_len));
    TEST_ASSERT_EQUAL('/', topic[base_len]);
    TEST_ASSERT_EQUAL_STRING("Living Room", topic + base_len + 1);
}

static void test_four_topics_are_distinct_and_share_a_prefix(void)
{
    char state[MQTT_TOPIC_MAX_LEN];
    char set[MQTT_TOPIC_MAX_LEN];
    char get[MQTT_TOPIC_MAX_LEN];
    char avail[MQTT_TOPIC_MAX_LEN];

    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_device_state("dev", state, sizeof(state)));
    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_device_set("dev", set, sizeof(set)));
    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_device_get("dev", get, sizeof(get)));
    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_device_availability("dev", avail, sizeof(avail)));

    /* A device's four topics must not collide. State published onto the set
     * topic would make the gateway command itself. */
    TEST_ASSERT_NOT_EQUAL(0, strcmp(state, set));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(state, get));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(state, avail));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(set, get));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(set, avail));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(get, avail));

    /* All three sub-topics hang off the device topic. */
    TEST_ASSERT_EQUAL(0, strncmp(set, state, strlen(state)));
    TEST_ASSERT_EQUAL(0, strncmp(get, state, strlen(state)));
    TEST_ASSERT_EQUAL(0, strncmp(avail, state, strlen(state)));
}

static void test_null_arguments_rejected(void)
{
    char topic[MQTT_TOPIC_MAX_LEN];

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mqtt_topic_device_state(NULL, topic, sizeof(topic)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mqtt_topic_device_state("d", NULL, sizeof(topic)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mqtt_topic_device_state("d", topic, 0));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mqtt_topic_device_set(NULL, topic, sizeof(topic)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mqtt_topic_device_get(NULL, topic, sizeof(topic)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      mqtt_topic_device_availability(NULL, topic, sizeof(topic)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mqtt_topic_sanitize_name(NULL, topic, sizeof(topic)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      mqtt_topic_extract_friendly_name(NULL, topic, sizeof(topic)));
}

/* The point of the suite: a builder that cannot fit the topic must say so
 * rather than return a shorter, valid-looking topic for a different device. */
static void test_small_buffer_is_an_error_not_a_truncation(void)
{
    guarded_buf_t g;
    const size_t tiny = 8;      /* Far shorter than base + '/' + name */
    const char *long_name = "A Device With A Long Name";

    guarded_init(&g, tiny);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, mqtt_topic_device_state(long_name, g.buf, tiny));
    TEST_ASSERT_TRUE(guard_intact(&g));

    guarded_init(&g, tiny);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, mqtt_topic_device_set(long_name, g.buf, tiny));
    TEST_ASSERT_TRUE(guard_intact(&g));

    guarded_init(&g, tiny);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, mqtt_topic_device_get(long_name, g.buf, tiny));
    TEST_ASSERT_TRUE(guard_intact(&g));

    guarded_init(&g, tiny);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, mqtt_topic_device_availability(long_name, g.buf, tiny));
    TEST_ASSERT_TRUE(guard_intact(&g));
}

/* Exactly-fitting buffer succeeds; one byte less must fail. */
static void test_exact_fit_boundary(void)
{
    const char *name = "dev";
    size_t needed = strlen(MQTT_BASE_TOPIC) + 1 + strlen(name) + 1;

    char buf[MQTT_TOPIC_MAX_LEN];
    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_device_state(name, buf, needed));
    TEST_ASSERT_EQUAL(needed - 1, strlen(buf));

    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, mqtt_topic_device_state(name, buf, needed - 1));
}

/* ============================================================================
 * Sanitising
 * ============================================================================ */

static void test_sanitize_neutralises_wildcards_and_separator(void)
{
    char out[64];

    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_sanitize_name("Lamp/Kitchen", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Lamp_Kitchen", out);

    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_sanitize_name("all+", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("all_", out);

    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_sanitize_name("all#", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("all_", out);

    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_sanitize_name("Living Room", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Living_Room", out);

    /* Whatever goes in, no wildcard or separator may survive. */
    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_sanitize_name("a/b+c#d$e\\f*g?h", out, sizeof(out)));
    TEST_ASSERT_NULL(strpbrk(out, "/+#$\\*?"));
}

static void test_sanitize_keeps_safe_characters(void)
{
    char out[64];
    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_sanitize_name("Sensor-1_v2.0", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Sensor-1_v2.0", out);
}

static void test_sanitize_errors_rather_than_truncating(void)
{
    guarded_buf_t g;
    guarded_init(&g, 8);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM,
                      mqtt_topic_sanitize_name("a name far longer than eight", g.buf, 8));
    TEST_ASSERT_TRUE(guard_intact(&g));
}

static void test_sanitize_empty_name(void)
{
    char out[8];
    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_sanitize_name("", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

/* ============================================================================
 * Extraction
 * ============================================================================ */

static void test_extract_friendly_name(void)
{
    char name[64];
    char topic[MQTT_TOPIC_MAX_LEN];

    snprintf(topic, sizeof(topic), "%s/Kitchen Lamp", MQTT_BASE_TOPIC);
    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_extract_friendly_name(topic, name, sizeof(name)));
    TEST_ASSERT_EQUAL_STRING("Kitchen Lamp", name);

    /* A trailing segment must not become part of the name. */
    snprintf(topic, sizeof(topic), "%s/Kitchen Lamp/set", MQTT_BASE_TOPIC);
    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_extract_friendly_name(topic, name, sizeof(name)));
    TEST_ASSERT_EQUAL_STRING("Kitchen Lamp", name);
}

static void test_extract_rejects_foreign_base(void)
{
    char name[64];
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      mqtt_topic_extract_friendly_name("someoneelse/dev", name, sizeof(name)));
}

static void test_extract_errors_rather_than_truncating(void)
{
    guarded_buf_t g;
    char topic[MQTT_TOPIC_MAX_LEN];
    snprintf(topic, sizeof(topic), "%s/A Very Long Device Name Indeed", MQTT_BASE_TOPIC);

    guarded_init(&g, 8);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, mqtt_topic_extract_friendly_name(topic, g.buf, 8));
    TEST_ASSERT_TRUE(guard_intact(&g));
}

/* A sanitised name must come back out of its own state topic unchanged. */
static void test_sanitized_name_round_trips(void)
{
    char sane[64];
    char topic[MQTT_TOPIC_MAX_LEN];
    char back[64];

    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_sanitize_name("Buero/Lampe 1", sane, sizeof(sane)));
    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_device_state(sane, topic, sizeof(topic)));
    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_extract_friendly_name(topic, back, sizeof(back)));
    TEST_ASSERT_EQUAL_STRING(sane, back);
}

/* ============================================================================
 * Wildcard matching
 * ============================================================================ */

static void test_matches_exact(void)
{
    TEST_ASSERT_TRUE(mqtt_topic_matches("a/b/c", "a/b/c"));
    TEST_ASSERT_FALSE(mqtt_topic_matches("a/b/c", "a/b/d"));
    TEST_ASSERT_FALSE(mqtt_topic_matches("a/b", "a/b/c"));
    TEST_ASSERT_FALSE(mqtt_topic_matches("a/b/c", "a/b"));
}

static void test_matches_single_level_wildcard(void)
{
    TEST_ASSERT_TRUE(mqtt_topic_matches("a/anything/c", "a/+/c"));
    TEST_ASSERT_TRUE(mqtt_topic_matches("a/b/c", "a/+/c"));
    /* + covers exactly one level and must not cross a separator. */
    TEST_ASSERT_FALSE(mqtt_topic_matches("a/b/extra/c", "a/+/c"));
}

static void test_matches_multi_level_wildcard(void)
{
    TEST_ASSERT_TRUE(mqtt_topic_matches("a/b/c/d", "a/#"));
    TEST_ASSERT_TRUE(mqtt_topic_matches("a/b", "a/#"));
    TEST_ASSERT_TRUE(mqtt_topic_matches("anything/at/all", "#"));
    TEST_ASSERT_FALSE(mqtt_topic_matches("b/c", "a/#"));
}

static void test_matches_null_is_false_not_a_crash(void)
{
    TEST_ASSERT_FALSE(mqtt_topic_matches(NULL, "a/#"));
    TEST_ASSERT_FALSE(mqtt_topic_matches("a/b", NULL));
    TEST_ASSERT_FALSE(mqtt_topic_matches(NULL, NULL));
}

/* The bridge subscribes with wildcards. A device's set topic has to match the
 * pattern actually subscribed to, or its commands never arrive. */
static void test_set_topic_matches_bridge_subscription(void)
{
    char topic[MQTT_TOPIC_MAX_LEN];
    char pattern[MQTT_TOPIC_MAX_LEN];

    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_device_set("Kitchen_Lamp", topic, sizeof(topic)));

    snprintf(pattern, sizeof(pattern), "%s/+/set", MQTT_BASE_TOPIC);
    TEST_ASSERT_TRUE(mqtt_topic_matches(topic, pattern));

    snprintf(pattern, sizeof(pattern), "%s/#", MQTT_BASE_TOPIC);
    TEST_ASSERT_TRUE(mqtt_topic_matches(topic, pattern));
}

/* ============================================================================
 * Suite
 * ============================================================================ */


/* ============================================================================
 * Topic builders must not emit a name that breaks the topic
 * ============================================================================ */

/**
 * A wildcard in a friendly name used to reach the wire.
 *
 * Friendly names come from the manufacturer and model strings a device reports
 * during its interview, so a device on the air picks them. '#' and '+' make a
 * published topic malformed; '/' quietly adds a level, and
 * mqtt_topic_extract_friendly_name() then reads back only the first one.
 */
static void test_builders_strip_wildcards_and_separators(void)
{
    char topic[MQTT_TOPIC_MAX_LEN];

    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_device_state("Bad#Name", topic, sizeof(topic)));
    TEST_ASSERT_EQUAL_STRING("zigbee2mqtt/Bad_Name", topic);

    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_device_set("Plus+Name", topic, sizeof(topic)));
    TEST_ASSERT_EQUAL_STRING("zigbee2mqtt/Plus_Name/set", topic);

    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_device_get("Kitchen/Lamp", topic, sizeof(topic)));
    TEST_ASSERT_EQUAL_STRING("zigbee2mqtt/Kitchen_Lamp/get", topic);

    TEST_ASSERT_EQUAL(ESP_OK,
                      mqtt_topic_device_availability("Ctrl\tName", topic, sizeof(topic)));
    TEST_ASSERT_EQUAL_STRING("zigbee2mqtt/Ctrl_Name/availability", topic);
}

/**
 * Spaces stay.
 *
 * They are legal in a topic, and gateway entities already publish under
 * "Free Heap" and "WiFi Signal". Rewriting those would break every existing
 * automation for a cosmetic gain.
 */
static void test_builders_keep_spaces(void)
{
    char topic[MQTT_TOPIC_MAX_LEN];
    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_device_state("Free Heap", topic, sizeof(topic)));
    TEST_ASSERT_EQUAL_STRING("zigbee2mqtt/Free Heap", topic);
}

/** An ordinary name passes through untouched. */
static void test_builders_leave_ordinary_names_alone(void)
{
    char topic[MQTT_TOPIC_MAX_LEN];
    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_device_state("lumi.vibration.aq1",
                                                      topic, sizeof(topic)));
    TEST_ASSERT_EQUAL_STRING("zigbee2mqtt/lumi.vibration.aq1", topic);
}

/**
 * The name read back off a topic matches the stripped form, not the original.
 *
 * This is why callers resolving a device have to strip their candidates too —
 * an exact compare against the registry would miss every device whose name
 * needed stripping.
 */
static void test_stripped_name_is_what_comes_back(void)
{
    char topic[MQTT_TOPIC_MAX_LEN];
    char name[MQTT_TOPIC_MAX_LEN];
    char stripped[MQTT_TOPIC_MAX_LEN];

    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_device_set("Kitchen/Lamp", topic, sizeof(topic)));
    TEST_ASSERT_EQUAL(ESP_OK, mqtt_topic_extract_friendly_name(topic, name, sizeof(name)));

    mqtt_topic_strip_breakers("Kitchen/Lamp", stripped, sizeof(stripped));
    TEST_ASSERT_EQUAL_STRING(stripped, name);
    TEST_ASSERT_EQUAL_STRING("Kitchen_Lamp", name);
}

/** Stripping never overruns its buffer and always terminates. */
static void test_strip_truncates_safely(void)
{
    char big[300];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    big[0] = '#';

    char out[16];
    mqtt_topic_strip_breakers(big, out, sizeof(out));
    TEST_ASSERT_EQUAL(sizeof(out) - 1, strlen(out));
    TEST_ASSERT_EQUAL('_', out[0]);
}

static const test_case_t mqtt_topics_tests[] = {
    {"state_topic_shape",        test_state_topic_shape},
    {"four_topics_distinct",     test_four_topics_are_distinct_and_share_a_prefix},
    {"null_arguments_rejected",  test_null_arguments_rejected},
    {"small_buffer_errors",      test_small_buffer_is_an_error_not_a_truncation},
    {"exact_fit_boundary",       test_exact_fit_boundary},
    {"sanitize_wildcards",       test_sanitize_neutralises_wildcards_and_separator},
    {"sanitize_keeps_safe",      test_sanitize_keeps_safe_characters},
    {"sanitize_errors_small",    test_sanitize_errors_rather_than_truncating},
    {"sanitize_empty",           test_sanitize_empty_name},
    {"extract_friendly_name",    test_extract_friendly_name},
    {"extract_foreign_base",     test_extract_rejects_foreign_base},
    {"extract_errors_small",     test_extract_errors_rather_than_truncating},
    {"sanitized_round_trip",     test_sanitized_name_round_trips},
    {"matches_exact",            test_matches_exact},
    {"matches_plus",             test_matches_single_level_wildcard},
    {"matches_hash",             test_matches_multi_level_wildcard},
    {"matches_null",             test_matches_null_is_false_not_a_crash},
    {"set_topic_matches_sub",    test_set_topic_matches_bridge_subscription},
    {"builders_strip_wildcards",  test_builders_strip_wildcards_and_separators},
    {"builders_keep_spaces",      test_builders_keep_spaces},
    {"builders_pass_ordinary",    test_builders_leave_ordinary_names_alone},
    {"stripped_name_round_trip",  test_stripped_name_is_what_comes_back},
    {"strip_truncates_safely",    test_strip_truncates_safely},
};

test_stats_t run_mqtt_topics_tests(void)
{
    ESP_LOGI(TAG, "Running MQTT Topics Tests");
    return test_run_suite(mqtt_topics_tests,
                          sizeof(mqtt_topics_tests) / sizeof(mqtt_topics_tests[0]));
}
