/* Capture-session manager: owns a tracker, a bounded worker thread and the
 * live capture lifecycle. Stopping is cooperative and joins the worker before
 * reporting completion, so the interface is not left half-configured by NERO. */

#include "nero/wifi.h"
#include "nero/wifi_capture.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct nero_wifi_session {
    nero_wifi_tracker *tracker;
    pthread_mutex_t mutex;
    pthread_t thread;
    atomic_int running;
    atomic_int stop_requested;
    atomic_int stop_flag;
    atomic_int joinable;
    atomic_uint_fast64_t packets;
    atomic_uint_fast64_t bytes;
    atomic_uint_fast64_t non_wifi;
    nero_wifi_stats base;
    char interface[NERO_WIFI_IFNAME_MAX];
    char last_error[256];
};

typedef struct {
    nero_wifi_session *session;
    nero_wifi_capture *capture;
} session_worker;

static int session_packet_callback(void *context, const nero_wifi_capture_packet *packet)
{
    nero_wifi_session *session = context;
    char parse_error[128] = {0};
    atomic_fetch_add_explicit(&session->packets, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&session->bytes, packet->caplen, memory_order_relaxed);
    if (packet->linktype != NERO_WIFI_LINKTYPE_IEEE802_11 &&
        packet->linktype != NERO_WIFI_LINKTYPE_RADIOTAP) {
        atomic_fetch_add_explicit(&session->non_wifi, 1, memory_order_relaxed);
        return 0;
    }
    nero_wifi_tracker_ingest_l2(session->tracker, packet->data, packet->caplen, packet->linktype,
                                packet->timestamp_us, parse_error, sizeof(parse_error));
    return 0;
}

static void *session_worker_main(void *argument)
{
    session_worker *worker = argument;
    nero_wifi_session *session = worker->session;
    nero_wifi_capture *capture = worker->capture;
    nero_wifi_capture_summary summary;
    char error[256] = {0};

    memset(&summary, 0, sizeof(summary));
    int result = nero_wifi_capture_run(capture, session_packet_callback, session,
                                       &session->stop_flag, 200, &summary, error, sizeof(error));

    pthread_mutex_lock(&session->mutex);
    session->base.dropped += summary.dropped;
    session->base.monitor_set = summary.monitor_set;
    session->base.linktype = summary.linktype;
    session->base.ended_us = nero_wifi_now_us();
    snprintf(session->base.backend, sizeof(session->base.backend), "%s", summary.backend);
    if (result < 0) {
        snprintf(session->last_error, sizeof(session->last_error), "%s",
                 error[0] != '\0' ? error : "capture failed");
    }
    pthread_mutex_unlock(&session->mutex);

    nero_wifi_capture_close(capture);
    free(worker);
    atomic_store_explicit(&session->running, 0, memory_order_release);
    return NULL;
}

nero_wifi_session *nero_wifi_session_create(void)
{
    nero_wifi_session *session = calloc(1, sizeof(*session));
    if (session == NULL)
        return NULL;
    session->tracker = nero_wifi_tracker_create();
    if (session->tracker == NULL) {
        free(session);
        return NULL;
    }
    if (pthread_mutex_init(&session->mutex, NULL) != 0) {
        nero_wifi_tracker_destroy(session->tracker);
        free(session);
        return NULL;
    }
    atomic_init(&session->running, 0);
    atomic_init(&session->stop_requested, 0);
    atomic_init(&session->stop_flag, 0);
    atomic_init(&session->joinable, 0);
    atomic_init(&session->packets, 0);
    atomic_init(&session->bytes, 0);
    atomic_init(&session->non_wifi, 0);
    return session;
}

int nero_wifi_session_start(nero_wifi_session *session, const char *interface_name,
                            int request_monitor_mode, char *error, size_t error_len)
{
    if (session == NULL) {
        if (error != NULL && error_len > 0)
            snprintf(error, error_len, "invalid session");
        return -1;
    }
    if (atomic_load_explicit(&session->running, memory_order_acquire)) {
        if (error != NULL && error_len > 0)
            snprintf(error, error_len, "capture already running");
        return -1;
    }
    /* Join a worker that ended on its own before reusing the session. */
    nero_wifi_session_stop(session);
    if (interface_name == NULL || interface_name[0] == '\0') {
        if (error != NULL && error_len > 0)
            snprintf(error, error_len, "no Wi-Fi interface");
        return -1;
    }

    char capture_error[256] = {0};
    nero_wifi_capture *capture = nero_wifi_capture_open(interface_name, request_monitor_mode,
                                                        capture_error, sizeof(capture_error));
    if (capture == NULL) {
        if (error != NULL && error_len > 0) {
            snprintf(error, error_len, "%s",
                     capture_error[0] != '\0' ? capture_error : "capture open failed");
        }
        return -1;
    }

    session_worker *worker = calloc(1, sizeof(*worker));
    if (worker == NULL) {
        nero_wifi_capture_close(capture);
        if (error != NULL && error_len > 0)
            snprintf(error, error_len, "out of memory");
        return -1;
    }
    worker->session = session;
    worker->capture = capture;

    nero_wifi_tracker_reset(session->tracker);

    pthread_mutex_lock(&session->mutex);
    memset(&session->base, 0, sizeof(session->base));
    session->base.source_is_file = 0;
    session->base.monitor_requested = request_monitor_mode;
    session->base.monitor_set = request_monitor_mode;
    snprintf(session->base.backend, sizeof(session->base.backend), "%s", "libpcap/Npcap");
    snprintf(session->base.source, sizeof(session->base.source), "live: %s", interface_name);
    session->base.started_us = nero_wifi_now_us();
    session->last_error[0] = '\0';
    snprintf(session->interface, sizeof(session->interface), "%s", interface_name);
    snprintf(session->base.interface, sizeof(session->base.interface), "%s", interface_name);
    pthread_mutex_unlock(&session->mutex);

    atomic_store_explicit(&session->packets, 0, memory_order_relaxed);
    atomic_store_explicit(&session->bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&session->non_wifi, 0, memory_order_relaxed);
    atomic_store_explicit(&session->stop_flag, 0, memory_order_relaxed);
    atomic_store_explicit(&session->stop_requested, 0, memory_order_relaxed);
    atomic_store_explicit(&session->running, 1, memory_order_release);

    if (pthread_create(&session->thread, NULL, session_worker_main, worker) != 0) {
        atomic_store_explicit(&session->running, 0, memory_order_release);
        nero_wifi_capture_close(capture);
        free(worker);
        nero_wifi_session_reset(session);
        if (error != NULL && error_len > 0)
            snprintf(error, error_len, "could not start worker");
        return -1;
    }
    atomic_store_explicit(&session->joinable, 1, memory_order_release);
    if (error != NULL && error_len > 0)
        error[0] = '\0';
    return 0;
}

int nero_wifi_session_stop(nero_wifi_session *session)
{
    if (session == NULL)
        return -1;
    atomic_store_explicit(&session->stop_requested, 1, memory_order_release);
    atomic_store_explicit(&session->stop_flag, 1, memory_order_release);

    int joinable = atomic_load_explicit(&session->joinable, memory_order_acquire);
    if (joinable) {
        if (pthread_join(session->thread, NULL) != 0)
            return -1;
        atomic_store_explicit(&session->joinable, 0, memory_order_release);
    }
    return 0;
}

void nero_wifi_session_destroy(nero_wifi_session *session)
{
    if (session == NULL)
        return;
    nero_wifi_session_stop(session);
    nero_wifi_tracker_destroy(session->tracker);
    pthread_mutex_destroy(&session->mutex);
    free(session);
}

int nero_wifi_session_running(const nero_wifi_session *session)
{
    if (session == NULL)
        return 0;
    return atomic_load_explicit(&session->running, memory_order_acquire) ? 1 : 0;
}

int nero_wifi_session_analyze(nero_wifi_session *session, const char *path, char *error,
                              size_t error_len)
{
    if (session == NULL || path == NULL) {
        if (error != NULL && error_len > 0)
            snprintf(error, error_len, "invalid argument");
        return -1;
    }
    if (nero_wifi_session_running(session)) {
        if (error != NULL && error_len > 0)
            snprintf(error, error_len, "stop capture first");
        return -1;
    }
    nero_wifi_tracker *incoming = nero_wifi_tracker_create();
    if (incoming == NULL) {
        if (error != NULL && error_len > 0)
            snprintf(error, error_len, "out of memory");
        return -1;
    }
    nero_wifi_stats stats;
    if (nero_wifi_analyze_pcap(incoming, path, &stats, error, error_len) != 0) {
        nero_wifi_tracker_destroy(incoming);
        return -1;
    }
    nero_wifi_tracker *previous = session->tracker;
    session->tracker = incoming;
    nero_wifi_tracker_destroy(previous);
    pthread_mutex_lock(&session->mutex);
    session->base = stats;
    session->base.source_is_file = 1;
    snprintf(session->interface, sizeof(session->interface), "file");
    session->last_error[0] = '\0';
    pthread_mutex_unlock(&session->mutex);
    atomic_store_explicit(&session->packets, stats.packets, memory_order_relaxed);
    atomic_store_explicit(&session->bytes, stats.bytes, memory_order_relaxed);
    atomic_store_explicit(&session->non_wifi, stats.non_wifi, memory_order_relaxed);
    return 0;
}

void nero_wifi_session_reset(nero_wifi_session *session)
{
    if (session == NULL)
        return;
    nero_wifi_session_stop(session);
    nero_wifi_tracker_reset(session->tracker);
    pthread_mutex_lock(&session->mutex);
    memset(&session->base, 0, sizeof(session->base));
    session->interface[0] = '\0';
    session->last_error[0] = '\0';
    pthread_mutex_unlock(&session->mutex);
    atomic_store_explicit(&session->packets, 0, memory_order_relaxed);
    atomic_store_explicit(&session->bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&session->non_wifi, 0, memory_order_relaxed);
}

void nero_wifi_session_snapshot_stats(const nero_wifi_session *session, nero_wifi_stats *out)
{
    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));
    if (session == NULL)
        return;
    nero_wifi_session *mutable_session = (nero_wifi_session *)session;
    nero_wifi_stats tracker_stats;
    nero_wifi_tracker_get_stats(session->tracker, &tracker_stats);

    pthread_mutex_lock(&mutable_session->mutex);
    *out = session->base;
    pthread_mutex_unlock(&mutable_session->mutex);

    out->packets = atomic_load_explicit(&mutable_session->packets, memory_order_relaxed);
    out->bytes = atomic_load_explicit(&mutable_session->bytes, memory_order_relaxed);
    out->non_wifi = atomic_load_explicit(&mutable_session->non_wifi, memory_order_relaxed);
    out->frames = tracker_stats.frames;
    out->management = tracker_stats.management;
    out->control = tracker_stats.control;
    out->data = tracker_stats.data;
    out->malformed = tracker_stats.malformed;
    out->duplicates = tracker_stats.duplicates;
    out->evicted = tracker_stats.evicted;
    out->dropped += tracker_stats.dropped;
}

int nero_wifi_session_get_aps(const nero_wifi_session *session, nero_wifi_ap *out, int capacity,
                              int channel_filter)
{
    if (session == NULL)
        return 0;
    return nero_wifi_tracker_get_aps(session->tracker, out, capacity, channel_filter);
}

int nero_wifi_session_get_clients(const nero_wifi_session *session, nero_wifi_client *out,
                                  int capacity, int channel_filter)
{
    if (session == NULL)
        return 0;
    return nero_wifi_tracker_get_clients(session->tracker, out, capacity, channel_filter);
}

int nero_wifi_session_find_ap(const nero_wifi_session *session,
                              const uint8_t bssid[NERO_WIFI_MAC_LEN], nero_wifi_ap *out)
{
    if (session == NULL)
        return -1;
    return nero_wifi_tracker_find_ap(session->tracker, bssid, out);
}

int nero_wifi_session_find_client(const nero_wifi_session *session,
                                  const uint8_t mac[NERO_WIFI_MAC_LEN], nero_wifi_client *out)
{
    if (session == NULL)
        return -1;
    return nero_wifi_tracker_find_client(session->tracker, mac, out);
}

static size_t session_copy_text(const nero_wifi_session *session, const char *source, char *out,
                                size_t out_len)
{
    if (out == NULL || out_len == 0)
        return 0;
    out[0] = '\0';
    if (session == NULL)
        return 0;
    nero_wifi_session *mutable_session = (nero_wifi_session *)session;
    pthread_mutex_lock(&mutable_session->mutex);
    snprintf(out, out_len, "%s", source);
    size_t length = strlen(out);
    pthread_mutex_unlock(&mutable_session->mutex);
    return length;
}

size_t nero_wifi_session_interface(const nero_wifi_session *session, char *out, size_t out_len)
{
    return session_copy_text(session, session != NULL ? session->interface : "", out, out_len);
}

size_t nero_wifi_session_last_error(const nero_wifi_session *session, char *out, size_t out_len)
{
    return session_copy_text(session, session != NULL ? session->last_error : "", out, out_len);
}
