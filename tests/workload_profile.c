/* Deterministic offline tests for hardware probing hooks, the workload
 * recommendation rule and the strict run-time count parser.
 * Real machine hardware is never asserted; fixtures override the probe. */
#include "nero/hardware.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int condition, const char *label)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", label);
        failures++;
    }
}

static nero_hardware_info fixture(long cores, unsigned long long total)
{
    nero_hardware_info info;
    memset(&info, 0, sizeof(info));
    info.os = "Fixture";
    info.architecture = "test";
    info.cpu_logical = cores;
    info.cpu_physical = -1;
    info.ram_total_bytes = total;
    info.ram_available_bytes = 0;
    return info;
}

static void test_low_resource(void)
{
    nero_hardware_info info = fixture(2, 2ULL * 1024 * 1024 * 1024);
    char reason[256];
    nero_workload_profile profile =
        nero_workload_recommend(&info, reason, sizeof(reason));
    expect(profile == NERO_WORKLOAD_PROFILE_LOW, "low-resource -> LOW");
    expect(reason[0] != '\0', "low resource explains the recommendation");
}

static void test_low_on_small_memory(void)
{
    nero_hardware_info info = fixture(16, 3ULL * 1024 * 1024 * 1024);
    char reason[256];
    expect(nero_workload_recommend(&info, reason, sizeof(reason)) ==
               NERO_WORKLOAD_PROFILE_LOW,
           "many cores but 3 GiB -> LOW");
}

static void test_balanced(void)
{
    nero_hardware_info info = fixture(8, 16ULL * 1024 * 1024 * 1024);
    char reason[256];
    expect(nero_workload_recommend(&info, reason, sizeof(reason)) ==
               NERO_WORKLOAD_PROFILE_BALANCED,
           "8 cores / 16 GiB -> BALANCED");
    nero_hardware_info modest = fixture(4, 8ULL * 1024 * 1024 * 1024);
    expect(nero_workload_recommend(&modest, reason, sizeof(reason)) ==
               NERO_WORKLOAD_PROFILE_BALANCED,
           "4 cores / 8 GiB -> BALANCED");
}

static void test_failed_probe(void)
{
    nero_hardware_info info = fixture(-1, 0);
    char reason[256];
    expect(nero_workload_recommend(&info, reason, sizeof(reason)) ==
               NERO_WORKLOAD_PROFILE_NONE,
           "unavailable CPU/RAM -> no recommendation");
    expect(reason[0] != '\0', "unavailable case is explained");
    expect(strstr(reason, "unavailable") != NULL,
           "unavailable wording present");
}

static void test_extreme_counts(void)
{
    /* CPU count overflow and negative sentinels must not crash or guess. */
    nero_hardware_info info = fixture(LONG_MAX, 16ULL * 1024 * 1024 * 1024);
    char reason[256];
    expect(nero_workload_recommend(&info, reason, sizeof(reason)) ==
               NERO_WORKLOAD_PROFILE_BALANCED,
           "LONG_MAX cores handled without overflow");
    nero_hardware_info negative = fixture(-3, 16ULL * 1024 * 1024 * 1024);
    expect(nero_workload_recommend(&negative, reason, sizeof(reason)) ==
               NERO_WORKLOAD_PROFILE_NONE,
           "negative CPU count -> no recommendation");
    expect(nero_workload_recommend(NULL, reason, sizeof(reason)) ==
               NERO_WORKLOAD_PROFILE_NONE,
           "null info -> no recommendation");
}

static void test_probe_fixture_override(void)
{
    nero_hardware_info info = fixture(12, 32ULL * 1024 * 1024 * 1024);
    nero_hardware_test_set(&info);
    nero_hardware_info probed;
    expect(nero_hardware_probe(&probed) == 1, "fixture probe reports success");
    expect(probed.cpu_logical == 12, "fixture CPU count used");
    expect(probed.ram_total_bytes == 32ULL * 1024 * 1024 * 1024,
           "fixture RAM used");
    expect(probed.os != NULL && probed.architecture != NULL,
           "platform identity stays real");
    nero_hardware_test_clear();
    /* Clearing falls back to the real machine probe: values are plausible or
     * explicitly unavailable, never fabricated. */
    expect(nero_hardware_probe(&probed) == 0 || probed.cpu_logical >= 0,
           "real probe is plausible or unavailable");
}

static void test_suggested_workers(void)
{
    char reason[256];
    nero_hardware_info low = fixture(2, 2ULL * 1024 * 1024 * 1024);
    expect(nero_workload_suggested_workers(&low, reason, sizeof(reason)) ==
               NERO_WORKLOAD_LOW_WORKERS,
           "low-resource -> low worker suggestion");
    nero_hardware_info balanced = fixture(8, 16ULL * 1024 * 1024 * 1024);
    expect(nero_workload_suggested_workers(&balanced, reason, sizeof(reason)) ==
               NERO_WORKLOAD_BALANCED_WORKERS,
           "8 cores / 16 GiB -> balanced worker suggestion");
    nero_hardware_info failed = fixture(-1, 0);
    expect(nero_workload_suggested_workers(&failed, reason, sizeof(reason)) == 0,
           "unavailable hardware -> no worker suggestion");
}

static void test_count_parse(void)
{
    long out = 0;
    const long max = 65535;
    /* Valid bounds, including values well above the old 512 display cap. */
    expect(nero_count_parse("1", 1, max, &out) == 0 && out == 1,
           "parse lower bound 1");
    expect(nero_count_parse("1000", 1, max, &out) == 0 && out == 1000,
           "parse value above 512");
    expect(nero_count_parse("65535", 1, max, &out) == 0 && out == 65535,
           "parse upper bound 65535");
    /* Rejections: empty, signed, non-numeric, junk, overflow, range. */
    expect(nero_count_parse("", 1, max, &out) == -1, "reject empty");
    expect(nero_count_parse(" ", 1, max, &out) == -1, "reject blank");
    expect(nero_count_parse("abc", 1, max, &out) == -1, "reject letters");
    expect(nero_count_parse("-1", 1, max, &out) == -1, "reject negative");
    expect(nero_count_parse("+5", 1, max, &out) == -1, "reject signed");
    expect(nero_count_parse("0", 1, max, &out) == -1, "reject zero");
    expect(nero_count_parse("65536", 1, max, &out) == -1, "reject above max");
    expect(nero_count_parse("5x", 1, max, &out) == -1, "reject trailing junk");
    expect(nero_count_parse("5 ", 1, max, &out) == -1,
           "reject trailing space");
    expect(nero_count_parse(" 5", 1, max, &out) == -1,
           "reject leading space");
    expect(nero_count_parse("999999999999999999999", 1, max, &out) == -1,
           "reject long overflow");
    expect(nero_count_parse("4294967296", 1, max, &out) == -1,
           "reject 2^32");
    expect(nero_count_parse(NULL, 1, max, &out) == -1, "reject NULL text");
    expect(nero_count_parse("5", 1, max, NULL) == -1, "reject NULL out");
}

int main(void)
{
    test_low_resource();
    test_low_on_small_memory();
    test_balanced();
    test_failed_probe();
    test_extreme_counts();
    test_probe_fixture_override();
    test_suggested_workers();
    test_count_parse();

    if (failures != 0) {
        fprintf(stderr, "%d workload-profile test(s) failed\n", failures);
        return 1;
    }
    puts("workload-profile: all tests passed");
    return 0;
}
