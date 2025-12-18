#include "ble_scanner.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define SCAN_TYPE_ACTIVE 0x01
#define OWN_ADDRESS_PUBLIC 0x00

static double elapsed_seconds(const struct timeval *start, const struct timeval *end) {
    return (double)(end->tv_sec - start->tv_sec) +
           (double)(end->tv_usec - start->tv_usec) / 1000000.0;
}

static void restore_filter(int sock, const struct hci_filter *filter, socklen_t length) {
    if (filter == NULL) {
        return;
    }

    if (setsockopt(sock, SOL_HCI, HCI_FILTER, filter, length) < 0) {
        perror("setsockopt(HCI_FILTER restore)");
    }
}

int start_ble_scan(int duration_seconds) {
    int dev_id = hci_get_route(NULL);
    if (dev_id < 0) {
        perror("hci_get_route");
        return -1;
    }

    int sock = hci_open_dev(dev_id);
    if (sock < 0) {
        perror("hci_open_dev");
        return -1;
    }

    struct hci_filter original_filter;
    socklen_t original_filter_len = sizeof(original_filter);
    bool has_original_filter = false;
    if (getsockopt(sock, SOL_HCI, HCI_FILTER, &original_filter, &original_filter_len) == 0) {
        has_original_filter = true;
    }

    uint16_t scan_interval = htobs(0x0010);
    uint16_t scan_window = htobs(0x0010);

    if (hci_le_set_scan_parameters(
            sock, SCAN_TYPE_ACTIVE, scan_interval, scan_window, OWN_ADDRESS_PUBLIC, 0x00, 1000) <
        0) {
        perror("hci_le_set_scan_parameters");
        close(sock);
        return -1;
    }

    if (hci_le_set_scan_enable(sock, 0x01, 1, 1000) < 0) {
        perror("hci_le_set_scan_enable");
        close(sock);
        return -1;
    }

    struct hci_filter event_filter;
    hci_filter_clear(&event_filter);
    hci_filter_set_ptype(HCI_EVENT_PKT, &event_filter);
    hci_filter_set_event(EVT_LE_META_EVENT, &event_filter);

    if (setsockopt(sock, SOL_HCI, HCI_FILTER, &event_filter, sizeof(event_filter)) < 0) {
        perror("setsockopt(HCI_FILTER)");
        hci_le_set_scan_enable(sock, 0x00, 1, 1000);
        restore_filter(sock, has_original_filter ? &original_filter : NULL, original_filter_len);
        close(sock);
        return -1;
    }

    if (duration_seconds <= 0) {
        duration_seconds = BLE_SCAN_DEFAULT_DURATION;
    }

    printf("[*] Listening for BLE advertising packets for %d seconds...\n", duration_seconds);

    struct timeval start_time;
    gettimeofday(&start_time, NULL);

    unsigned char buffer[HCI_MAX_EVENT_SIZE];
    struct pollfd fds = {.fd = sock, .events = POLLIN, .revents = 0};

    while (1) {
        struct timeval now;
        gettimeofday(&now, NULL);

        if (elapsed_seconds(&start_time, &now) >= duration_seconds) {
            break;
        }

        int poll_result = poll(&fds, 1, 500);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }

        if (poll_result == 0) {
            continue;
        }

        ssize_t length = read(sock, buffer, sizeof(buffer));
        if (length < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("read");
            break;
        }

        if (length < (1 + HCI_EVENT_HDR_SIZE)) {
            continue;
        }

        evt_le_meta_event *meta = (evt_le_meta_event *)(buffer + (1 + HCI_EVENT_HDR_SIZE));
        if (meta->subevent != EVT_LE_ADVERTISING_REPORT) {
            continue;
        }

        uint8_t reports = meta->data[0];
        uint8_t *ptr = meta->data + 1;

        for (uint8_t i = 0; i < reports; i++) {
            le_advertising_info *info = (le_advertising_info *)ptr;

            char address[18];
            ba2str(&info->bdaddr, address);

            int8_t rssi = *(int8_t *)(ptr + sizeof(le_advertising_info) + info->length);
            printf("[ADV] %s  RSSI: %d dBm\n", address, rssi);

            ptr += sizeof(le_advertising_info) + info->length + 1;
        }
    }

    if (hci_le_set_scan_enable(sock, 0x00, 1, 1000) < 0) {
        perror("hci_le_set_scan_enable");
    }

    restore_filter(sock, has_original_filter ? &original_filter : NULL, original_filter_len);
    close(sock);

    return 0;
}
