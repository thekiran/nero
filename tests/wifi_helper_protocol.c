#include "nero/wifi_platform.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/platform/macos/wifi_helper_protocol.h"

static int parse(const char *s, nero_wifi_ap *out, int *count, nero_wifi_platform_capabilities *caps)
{
    char error[256] = {0};
    return helper_parse(s, strlen(s) + 1, out, 2, count, caps, error, sizeof(error));
}
int main(void)
{
    nero_wifi_ap out[2];
    nero_wifi_platform_capabilities caps;
    int count = 99;
    const char *valid = "{\"version\":1,\"status\":\"authorized\",\"aps\":[{\"bssid\":\"02:11:22:33:44:55\",\"ssid_hex\":\"411b0a42\",\"channel\":6,\"band\":1,\"rssi\":-48,\"security\":4}]}";
    assert(!parse(valid, out, &count, &caps));
    assert(count == 1 && !strcmp(out[0].ssid, "A??B") && out[0].has_bssid);
    assert(out[0].frequency_mhz == 2437 && out[0].beacon_frames == 0 && out[0].client_count == 0);
    const char *redacted = "{\"version\":1,\"status\":\"authorized\",\"aps\":[{\"bssid\":null,\"ssid_hex\":null,\"channel\":0,\"band\":0,\"rssi\":0,\"security\":0}]}";
    assert(!parse(redacted, out, &count, &caps));
    assert(count == 1 && caps.identifiers_redacted && !out[0].has_bssid && !out[0].hidden);
    const char *states[] = {"denied", "restricted", "disabled", "not_determined", "no_interface", "powered_off", "scan_failed", "bogus"};
    for (size_t i = 0; i < sizeof(states)/sizeof(states[0]); ++i) {
        char input[160];
        snprintf(input, sizeof(input), "{\"version\":1,\"status\":\"%s\",\"aps\":[]}", states[i]);
        assert(parse(input, out, &count, &caps) == -1 && count == 0);
    }
    const char *bad[] = {"", "{}", "{", "{\"version\":2,\"status\":\"authorized\",\"aps\":[]}",
        "{\"version\":1,\"status\":\"authorized\",\"aps\":[]}trailing",
        "{\"version\":1,\"status\":\"authorized\",\"aps\":[{}]}",
        "{\"version\":1,\"version\":1,\"status\":\"authorized\",\"aps\":[]}"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i)
        assert(parse(bad[i], out, &count, &caps) == -1 && count == 0);
    char modified[1024];
    strcpy(modified, valid);
    char *rssi = strstr(modified, "-48"); memcpy(rssi, "999", 3);
    assert(parse(modified, out, &count, &caps) == -1 && count == 0);
    strcpy(modified, valid);
    char *ssid = strstr(modified, "411b"); ssid[0] = 'z';
    assert(parse(modified, out, &count, &caps) == -1 && count == 0);
    assert(!parse("{\"version\":1,\"status\":\"authorized\",\"aps\":[]}", out, &count, &caps) && count == 0);
    /* Validate every row before publishing any part of a response. */
    cJSON *root = cJSON_Parse(valid);
    cJSON *rows = cJSON_GetObjectItemCaseSensitive(root, "aps");
    cJSON *row = cJSON_GetArrayItem(rows, 0);
    cJSON_AddItemToArray(rows, cJSON_CreateObject());
    char *encoded = cJSON_PrintUnformatted(root);
    memset(out, 0x5a, sizeof(out));
    nero_wifi_ap before[2];
    memcpy(before, out, sizeof(out));
    assert(parse(encoded, out, &count, &caps) == -1 && count == 0);
    assert(memcmp(before, out, sizeof(out)) == 0);
    free(encoded);
    cJSON_DeleteItemFromArray(rows, 1);
    for (int i = 1; i < 257; ++i) cJSON_AddItemToArray(rows, cJSON_Duplicate(row, 1));
    encoded = cJSON_PrintUnformatted(root);
    assert(parse(encoded, out, &count, &caps) == -1 && count == 0);
    free(encoded);
    cJSON_Delete(root);

    root = cJSON_Parse(valid);
    row = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "aps"), 0);
    cJSON_ReplaceItemInObjectCaseSensitive(row, "bssid", cJSON_CreateString("bad"));
    encoded = cJSON_PrintUnformatted(root);
    assert(parse(encoded, out, &count, &caps) == -1 && count == 0);
    free(encoded);
    cJSON_ReplaceItemInObjectCaseSensitive(row, "bssid", cJSON_CreateString("02:11:22:33:44:55"));
    cJSON_ReplaceItemInObjectCaseSensitive(row, "ssid_hex", cJSON_CreateString(""));
    encoded = cJSON_PrintUnformatted(root);
    assert(!parse(encoded, out, &count, &caps) && out[0].hidden && !out[0].redacted);
    free(encoded);
    cJSON_Delete(root);

    /* Neighbor-cache response parser. */
    {
        nero_wifi_neighbor n[2];
        char error[256] = {0};
        int nc = 99;
        const char *ok = "{\"version\":1,\"status\":\"ok\",\"neighbors\":"
                         "[{\"ip\":\"192.168.1.1\",\"mac\":\"50:88:11:a4:e9:1f\"}]}";
        assert(!helper_parse_neighbors(ok, strlen(ok) + 1, n, 2, &nc, error, sizeof(error)));
        assert(nc == 1 && !strcmp(n[0].ip, "192.168.1.1"));
        assert(n[0].mac[0] == 0x50 && n[0].mac[5] == 0x1f);
        /* Empty list is valid and yields zero rows. */
        const char *empty = "{\"version\":1,\"status\":\"ok\",\"neighbors\":[]}";
        assert(!helper_parse_neighbors(empty, strlen(empty) + 1, n, 2, &nc, error, sizeof(error)));
        assert(nc == 0);
        /* Rejections: wrong status, bad MAC, extra key, wrong version, trailing. */
        const char *bad_n[] = {
            "{\"version\":1,\"status\":\"fail\",\"neighbors\":[]}",
            "{\"version\":1,\"status\":\"ok\",\"neighbors\":[{\"ip\":\"10.0.0.1\",\"mac\":\"zz:88:11:a4:e9:1f\"}]}",
            "{\"version\":1,\"status\":\"ok\",\"neighbors\":[{\"ip\":\"10.0.0.1\",\"mac\":\"50:88:11:a4:e9:1f\",\"x\":1}]}",
            "{\"version\":2,\"status\":\"ok\",\"neighbors\":[]}",
            "{\"version\":1,\"status\":\"ok\",\"neighbors\":[]}trailing",
            "{\"version\":1,\"status\":\"ok\"}"};
        for (size_t i = 0; i < sizeof(bad_n) / sizeof(bad_n[0]); ++i)
            assert(helper_parse_neighbors(bad_n[i], strlen(bad_n[i]) + 1, n, 2, &nc, error,
                                          sizeof(error)) == -1 && nc == 0);
        /* Capacity is honored: three rows into a buffer of two. */
        const char *three = "{\"version\":1,\"status\":\"ok\",\"neighbors\":["
            "{\"ip\":\"10.0.0.1\",\"mac\":\"02:00:00:00:00:01\"},"
            "{\"ip\":\"10.0.0.2\",\"mac\":\"02:00:00:00:00:02\"},"
            "{\"ip\":\"10.0.0.3\",\"mac\":\"02:00:00:00:00:03\"}]}";
        assert(!helper_parse_neighbors(three, strlen(three) + 1, n, 2, &nc, error, sizeof(error)));
        assert(nc == 2);
    }
    puts("Wi-Fi helper protocol tests passed (offline)");
    return 0;
}
