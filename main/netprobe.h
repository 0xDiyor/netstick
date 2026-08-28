#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Synchronous network probes shared by the doctor and the survey.

typedef struct {
    uint16_t sent, recv;
    uint16_t min_ms, avg_ms, max_ms;
    uint8_t  loss_pct;
    char     ip[46];
} ping_result_t;

// Pings a hostname or literal (v4 or v6). interval/timeout in ms.
bool probe_ping(const char *host, int count, int interval_ms, int timeout_ms, ping_result_t *out);

// Times a getaddrinfo(). Returns ms, or -1 on failure. ip gets the A record.
int probe_resolve(const char *host, char *ip, size_t ip_len);

typedef struct {
    int      status;              // HTTP status, or -1 on transport failure, -2 TLS failure
    char     location[96];        // Location header if any
    char     body[160];           // first bytes of the body, NUL terminated
    int      ms;
    char     err[40];
} http_result_t;

// Plain fetch without following redirects. https:// URLs use the cert bundle.
bool probe_http(const char *url, int timeout_ms, http_result_t *out);

// Download the URL for up to max_ms and report throughput. Returns bytes.
uint32_t probe_http_download(const char *url, int max_ms, uint32_t *mbps_x10, char *err, size_t err_len);

// Raw-socket traceroute of the first max_hops hops (one probe each).
typedef struct { uint8_t ttl; char ip[16]; uint16_t ms; bool timeout; } trace_hop_t;
int probe_traceroute(const char *dst_ip, int max_hops, int timeout_ms, trace_hop_t *hops);

bool probe_ip_is_private(const char *ip);     // RFC1918
bool probe_ip_is_cgnat(const char *ip);       // 100.64/10
