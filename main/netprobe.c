#include "netprobe.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "ping/ping_sock.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/ip.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "probe";

// ------------------------------------------------------------------ ping ---
typedef struct {
    ping_result_t *r;
    uint32_t sum;
    SemaphoreHandle_t done;
} ping_ctx_t;

static void on_ok(esp_ping_handle_t h, void *arg)
{
    ping_ctx_t *c = arg;
    uint32_t t = 0;
    esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &t, sizeof(t));
    if (t > 9999) t = 9999;
    c->r->recv++;
    c->sum += t;
    if (c->r->recv == 1 || t < c->r->min_ms) c->r->min_ms = t;
    if (t > c->r->max_ms) c->r->max_ms = t;
}
static void on_to(esp_ping_handle_t h, void *arg) { (void)h; (void)arg; }
static void on_end(esp_ping_handle_t h, void *arg)
{
    ping_ctx_t *c = arg;
    uint32_t tx = 0;
    esp_ping_get_profile(h, ESP_PING_PROF_REQUEST, &tx, sizeof(tx));
    c->r->sent = tx;
    xSemaphoreGive(c->done);
}

static bool resolve_any(const char *host, ip_addr_t *out, char *str, size_t len)
{
    struct addrinfo hint = { .ai_family = AF_UNSPEC }, *res = NULL;
    if (getaddrinfo(host, NULL, &hint, &res) != 0 || !res) return false;
    bool ok = false;
    if (res->ai_family == AF_INET) {
        struct in_addr a = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        inet_addr_to_ip4addr(ip_2_ip4(out), &a);
        out->type = IPADDR_TYPE_V4;
        ok = true;
    } else if (res->ai_family == AF_INET6) {
        struct in6_addr a = ((struct sockaddr_in6 *)res->ai_addr)->sin6_addr;
        inet6_addr_to_ip6addr(ip_2_ip6(out), &a);
        out->type = IPADDR_TYPE_V6;
        ok = true;
    }
    if (ok && str) ipaddr_ntoa_r(out, str, len);
    freeaddrinfo(res);
    return ok;
}

bool probe_ping(const char *host, int count, int interval_ms, int timeout_ms, ping_result_t *out)
{
    memset(out, 0, sizeof(*out));
    ip_addr_t target;
    if (!resolve_any(host, &target, out->ip, sizeof(out->ip))) {
        out->loss_pct = 100;
        return false;
    }
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count       = count;
    cfg.interval_ms = interval_ms;
    cfg.timeout_ms  = timeout_ms;
    cfg.task_stack_size = 3072;

    ping_ctx_t ctx = { .r = out, .done = xSemaphoreCreateBinary() };
    esp_ping_callbacks_t cbs = { .cb_args = &ctx, .on_ping_success = on_ok,
                                 .on_ping_timeout = on_to, .on_ping_end = on_end };
    esp_ping_handle_t h;
    if (esp_ping_new_session(&cfg, &cbs, &h) != ESP_OK) {
        vSemaphoreDelete(ctx.done);
        out->loss_pct = 100;
        return false;
    }
    esp_ping_start(h);
    xSemaphoreTake(ctx.done, pdMS_TO_TICKS(count * (interval_ms + timeout_ms) + 2000));
    esp_ping_delete_session(h);
    vSemaphoreDelete(ctx.done);

    if (out->sent == 0) out->sent = count;
    out->avg_ms  = out->recv ? ctx.sum / out->recv : 0;
    out->loss_pct = out->sent ? (100 * (out->sent - out->recv)) / out->sent : 100;
    return out->recv > 0;
}

// ------------------------------------------------------------------- dns ---
int probe_resolve(const char *host, char *ip, size_t ip_len)
{
    int64_t t0 = esp_timer_get_time();
    struct addrinfo hint = { .ai_family = AF_INET }, *res = NULL;
    int rc = getaddrinfo(host, NULL, &hint, &res);
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (rc != 0 || !res) return -1;
    if (ip) inet_ntoa_r(((struct sockaddr_in *)res->ai_addr)->sin_addr, ip, ip_len);
    freeaddrinfo(res);
    return ms;
}

// ------------------------------------------------------------------ http ---
static esp_err_t http_evt(esp_http_client_event_t *evt)
{
    http_result_t *r = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(evt->header_key, "Location") == 0) {
        snprintf(r->location, sizeof(r->location), "%s", evt->header_value);
    } else if (evt->event_id == HTTP_EVENT_ON_DATA) {
        size_t have = strlen(r->body);
        size_t room = sizeof(r->body) - 1 - have;
        size_t n = evt->data_len < room ? evt->data_len : room;
        memcpy(r->body + have, evt->data, n);
        r->body[have + n] = '\0';
    }
    return ESP_OK;
}

bool probe_http(const char *url, int timeout_ms, http_result_t *out)
{
    memset(out, 0, sizeof(*out));
    bool tls = strncmp(url, "https://", 8) == 0;
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = timeout_ms,
        .event_handler = http_evt,
        .user_data = out,
        .disable_auto_redirect = true,
        .crt_bundle_attach = tls ? esp_crt_bundle_attach : NULL,
        .user_agent = "netstick/1 (ESP32-C5)",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { out->status = -1; strcpy(out->err, "init"); return false; }
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(c);
    out->ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (err == ESP_OK) {
        out->status = esp_http_client_get_status_code(c);
    } else {
        out->status = tls ? -2 : -1;
        snprintf(out->err, sizeof(out->err), "%s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(c);
    return err == ESP_OK;
}

uint32_t probe_http_download(const char *url, int max_ms, uint32_t *mbps_x10, char *err, size_t err_len)
{
    *mbps_x10 = 0;
    if (err) err[0] = '\0';
    bool tls = strncmp(url, "https://", 8) == 0;
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 5000,
        .crt_bundle_attach = tls ? esp_crt_bundle_attach : NULL,
        .buffer_size = 4096,
        .user_agent = "netstick/1 (ESP32-C5)",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { if (err) snprintf(err, err_len, "init"); return 0; }
    esp_err_t e = esp_http_client_open(c, 0);
    if (e != ESP_OK) {
        if (err) snprintf(err, err_len, "%s", esp_err_to_name(e));
        esp_http_client_cleanup(c);
        return 0;
    }
    int64_t hdr = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (hdr < 0 || status >= 400) {
        if (err) snprintf(err, err_len, "http %d", status);
        esp_http_client_close(c); esp_http_client_cleanup(c);
        return 0;
    }
    char *buf = malloc(4096);
    if (!buf) { esp_http_client_close(c); esp_http_client_cleanup(c); return 0; }
    int64_t t0 = esp_timer_get_time();
    uint32_t total = 0;
    while ((esp_timer_get_time() - t0) < (int64_t)max_ms * 1000) {
        int n = esp_http_client_read(c, buf, 4096);
        if (n <= 0) break;
        total += n;
    }
    int64_t us = esp_timer_get_time() - t0;
    free(buf);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (us > 0) *mbps_x10 = (uint32_t)(((uint64_t)total * 8 * 10) / us);
    return total;
}

// ------------------------------------------------------------ traceroute ---
// esp_ping only reports echo replies, so TTL-expired hops need our own raw
// socket. lwIP hands raw IPv4 sockets the full IP header on receive.
int probe_traceroute(const char *dst_ip, int max_hops, int timeout_ms, trace_hop_t *hops)
{
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) { ESP_LOGW(TAG, "raw socket failed"); return 0; }
    struct sockaddr_in dst = { .sin_family = AF_INET };
    inet_aton(dst_ip, &dst.sin_addr);

    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint16_t id = (uint16_t)(esp_timer_get_time() & 0xFFFF);
    int n = 0;
    for (int ttl = 1; ttl <= max_hops; ttl++) {
        trace_hop_t *h = &hops[n];
        memset(h, 0, sizeof(*h));
        h->ttl = ttl;
        setsockopt(sock, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));

        uint8_t pkt[sizeof(struct icmp_echo_hdr) + 16];
        struct icmp_echo_hdr *echo = (struct icmp_echo_hdr *)pkt;
        memset(pkt, 0, sizeof(pkt));
        ICMPH_TYPE_SET(echo, ICMP_ECHO);
        echo->id = htons(id);
        echo->seqno = htons(ttl);
        echo->chksum = 0;
        echo->chksum = inet_chksum(pkt, sizeof(pkt));

        int64_t t0 = esp_timer_get_time();
        if (sendto(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
            h->timeout = true; n++; continue;
        }
        bool got = false, reached = false;
        while (!got) {
            uint8_t rx[128];
            struct sockaddr_in from; socklen_t fl = sizeof(from);
            int64_t left_us = (int64_t)timeout_ms * 1000 - (esp_timer_get_time() - t0);
            if (left_us <= 0) break;
            struct timeval lt = { .tv_sec = left_us / 1000000, .tv_usec = left_us % 1000000 };
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &lt, sizeof(lt));
            int len = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &fl);
            if (len <= 0) break;
            const struct ip_hdr *ip = (const struct ip_hdr *)rx;
            int ihl = IPH_HL_BYTES(ip);
            if (len < ihl + (int)sizeof(struct icmp_echo_hdr)) continue;
            const struct icmp_echo_hdr *ic = (const struct icmp_echo_hdr *)(rx + ihl);
            if (ic->type == ICMP_ER && ic->id == htons(id)) {
                got = true; reached = true;
            } else if (ic->type == ICMP_TE) {
                // Inner header: original IP + first 8 bytes of our echo.
                const uint8_t *inner = rx + ihl + 8;
                if (len >= ihl + 8 + 20 + 8) {
                    const struct ip_hdr *iip = (const struct ip_hdr *)inner;
                    const struct icmp_echo_hdr *iic = (const struct icmp_echo_hdr *)(inner + IPH_HL_BYTES(iip));
                    if (iic->id == htons(id)) got = true;
                }
            }
            if (got) {
                h->ms = (uint16_t)((esp_timer_get_time() - t0) / 1000);
                inet_ntoa_r(from.sin_addr, h->ip, sizeof(h->ip));
            }
        }
        if (!got) h->timeout = true;
        n++;
        if (reached) break;
    }
    close(sock);
    return n;
}

bool probe_ip_is_private(const char *ip)
{
    unsigned a = 0, b = 0;
    if (sscanf(ip, "%u.%u.", &a, &b) != 2) return false;
    return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
}

bool probe_ip_is_cgnat(const char *ip)
{
    unsigned a = 0, b = 0;
    if (sscanf(ip, "%u.%u.", &a, &b) != 2) return false;
    return a == 100 && b >= 64 && b <= 127;
}
