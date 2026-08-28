#include "speed.h"
#include "secrets.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "speed";

bool speed_find_server(char *host, size_t host_len)
{
    if (SURVEY_SERVER[0]) { snprintf(host, host_len, "%s", SURVEY_SERVER); return true; }

    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return false;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    struct timeval tv = { .tv_sec = 0, .tv_usec = 600000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(SPEED_PORT),
                              .sin_addr.s_addr = htonl(INADDR_BROADCAST) };
    bool found = false;
    for (int attempt = 0; attempt < 3 && !found; attempt++) {
        sendto(s, "NETSTICK?", 9, 0, (struct sockaddr *)&to, sizeof(to));
        char buf[32];
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
        if (n > 0) {
            buf[n] = '\0';
            if (strncmp(buf, "NETSTICK", 8) == 0) {
                inet_ntoa_r(from.sin_addr, host, host_len);
                found = true;
            }
        }
    }
    close(s);
    ESP_LOGI(TAG, "server %s", found ? host : "not found");
    return found;
}

static int connect_server(const char *host)
{
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(SPEED_PORT) };
    inet_aton(host, &a.sin_addr);
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) { close(s); return -1; }
    return s;
}

bool speed_lan_download(const char *host, int ms, uint32_t *mbps_x10)
{
    *mbps_x10 = 0;
    int s = connect_server(host);
    if (s < 0) return false;
    char cmd[24];
    int n = snprintf(cmd, sizeof(cmd), "DL %d\n", ms);
    send(s, cmd, n, 0);

    char *buf = malloc(8192);
    if (!buf) { close(s); return false; }
    int64_t t0 = esp_timer_get_time();
    uint64_t total = 0;
    while ((esp_timer_get_time() - t0) < (int64_t)(ms + 1500) * 1000) {
        int r = recv(s, buf, 8192, 0);
        if (r <= 0) break;
        total += r;
    }
    int64_t us = esp_timer_get_time() - t0;
    if (us > (int64_t)ms * 1000) us = (int64_t)ms * 1000;   // server stops at ms; ignore tail
    free(buf);
    close(s);
    if (us > 0) *mbps_x10 = (uint32_t)((total * 8 * 10) / us);
    return total > 0;
}

bool speed_lan_upload(const char *host, int ms, uint32_t *mbps_x10)
{
    *mbps_x10 = 0;
    int s = connect_server(host);
    if (s < 0) return false;
    char cmd[24];
    int n = snprintf(cmd, sizeof(cmd), "UL %d\n", ms);
    send(s, cmd, n, 0);

    char *buf = malloc(4096);
    if (!buf) { close(s); return false; }
    for (int i = 0; i < 4096; i++) buf[i] = (char)(i * 31 + 7);
    int64_t t0 = esp_timer_get_time();
    uint64_t total = 0;
    while ((esp_timer_get_time() - t0) < (int64_t)ms * 1000) {
        int w = send(s, buf, 4096, 0);
        if (w <= 0) break;
        total += w;
    }
    int64_t us = esp_timer_get_time() - t0;
    shutdown(s, SHUT_WR);
    // The server reports what it actually received; prefer that.
    int r = recv(s, buf, 63, 0);
    if (r > 0) {
        buf[r] = '\0';
        unsigned long sbytes = 0, sms = 0;
        if (sscanf(buf, "OK %lu %lu", &sbytes, &sms) == 2 && sms > 0) {
            *mbps_x10 = (uint32_t)((uint64_t)sbytes * 8 * 10 / (sms * 1000));
            free(buf); close(s);
            return true;
        }
    }
    free(buf);
    close(s);
    if (us > 0) *mbps_x10 = (uint32_t)((total * 8 * 10) / us);
    return total > 0;
}
