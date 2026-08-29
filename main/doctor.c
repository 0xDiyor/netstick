// Network doctor: a fixed sequence of checks against the current link, each
// graded PASS / WARN / FAIL with a one-line summary for the list and up to
// three lines of detail. Runs in its own task; the UI polls doctor_get().
#include "doctor.h"
#include "netmgr.h"
#include "netprobe.h"
#include "storage.h"
#include "secrets.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "doctor";

static doctor_report_t   s_rep;
static SemaphoreHandle_t s_lock;
static volatile bool     s_busy;

static const char *NAMES[DOC_N] = { "LINK", "DHCP", "GW", "DNS", "WAN", "PORTL",
                                    "TLS", "IPV6", "NAT", "TIME", "SPEED" };

static void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static void begin(int i, const char *activity)
{
    lock();
    s_rep.current = i;
    s_rep.checks[i].state = DOC_RUNNING;
    snprintf(s_rep.activity, sizeof(s_rep.activity), "%s", activity);
    unlock();
}

static void activity(const char *a)
{
    lock(); snprintf(s_rep.activity, sizeof(s_rep.activity), "%s", a); unlock();
}

static void finish(int i, doc_state_t st, const char *summary,
                   const char *d0, const char *d1, const char *d2)
{
    lock();
    doc_check_t *c = &s_rep.checks[i];
    c->state = st;
    snprintf(c->summary, sizeof(c->summary), "%s", summary ? summary : "");
    snprintf(c->detail[0], sizeof(c->detail[0]), "%s", d0 ? d0 : "");
    snprintf(c->detail[1], sizeof(c->detail[1]), "%s", d1 ? d1 : "");
    snprintf(c->detail[2], sizeof(c->detail[2]), "%s", d2 ? d2 : "");
    unlock();
    ESP_LOGI(TAG, "%-6s %s %s | %s | %s | %s", NAMES[i],
             st == DOC_PASS ? "PASS" : st == DOC_WARN ? "WARN" : st == DOC_FAIL ? "FAIL" : "SKIP",
             c->summary, c->detail[0], c->detail[1], c->detail[2]);
}

static void skip_from(int i, const char *why)
{
    for (; i < DOC_N; i++) finish(i, DOC_SKIP, why, "skipped:", why, NULL);
}

static void write_report(void)
{
    // Always to the console (useful when the stick is plugged into a laptop),
    // and to the SD card when there is one.
    time_t now = time(NULL);
    struct tm tm; localtime_r(&now, &tm);
    char stamp[32]; strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);

    FILE *f = NULL;
    if (storage_mounted()) {
        strftime(s_rep.report_path, sizeof(s_rep.report_path), "/sd/doctor/%Y%m%d-%H%M%S.txt", &tm);
        f = fopen(s_rep.report_path, "w");
        if (!f) s_rep.report_path[0] = '\0';
    }
    printf("\n==== netstick doctor %s ====\n", stamp);
    if (f) fprintf(f, "netstick doctor report  %s\n\n", stamp);
    for (int i = 0; i < DOC_N; i++) {
        doc_check_t *c = &s_rep.checks[i];
        const char *tag = c->state == DOC_PASS ? "PASS" : c->state == DOC_WARN ? "WARN"
                        : c->state == DOC_FAIL ? "FAIL" : "SKIP";
        printf("%-4s %-6s %s\n", tag, c->name, c->summary);
        if (f) fprintf(f, "%-4s %-6s %s\n", tag, c->name, c->summary);
        for (int d = 0; d < 3; d++) {
            if (!c->detail[d][0]) continue;
            printf("            %s\n", c->detail[d]);
            if (f) fprintf(f, "            %s\n", c->detail[d]);
        }
    }
    printf("==== %d pass %d warn %d fail in %lu ms ====\n\n",
           s_rep.n_pass, s_rep.n_warn, s_rep.n_fail, (unsigned long)s_rep.elapsed_ms);
    if (f) { fprintf(f, "\n%d pass, %d warn, %d fail\n", s_rep.n_pass, s_rep.n_warn, s_rep.n_fail); fclose(f); }
}

static void fmt_bssid(char *out, size_t n, const uint8_t *b)
{
    snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
}

static void doctor_task(void *arg)
{
    int64_t t0 = esp_timer_get_time();
    char s[32], d0[40], d1[40], d2[40];
    net_status_t n;

    // ---- LINK -------------------------------------------------------------
    begin(DOC_LINK, "checking link");
    for (int i = 0; i < 20; i++) {          // give a booting link a moment
        netmgr_get(&n);
        if (n.connected) break;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    netmgr_refresh_link();
    netmgr_get(&n);
    if (!n.connected) {
        if (!n.has_creds) {
            finish(DOC_LINK, DOC_FAIL, "no network", "no wifi network saved", "home > wifi > setup", "via phone");
        } else {
            snprintf(s, sizeof(s), "%s", n.last_reason ? n.reason_str : "not joined");
            snprintf(d0, sizeof(d0), "ssid: %.19s", netmgr_cfg_ssid());
            snprintf(d1, sizeof(d1), "last: %s", n.last_reason ? n.reason_str : "still trying");
            finish(DOC_LINK, DOC_FAIL, s, d0, d1, "check ssid/password");
        }
        skip_from(DOC_DHCP, "no link");
        goto done;
    }
    {
        char b[20]; fmt_bssid(b, sizeof(b), n.bssid);
        snprintf(s, sizeof(s), "%ddBm %uG ch%u", n.rssi, n.band, n.channel);
        snprintf(d0, sizeof(d0), "%.26s", n.ssid);
        snprintf(d1, sizeof(d1), "%s %s %uMHz", b + 9, n.phy, n.bw_mhz);
        snprintf(d2, sizeof(d2), "rssi %d  %s", n.rssi, n.ax ? "AP is 802.11ax" : "AP not 11ax");
        doc_state_t st = n.rssi >= -70 ? DOC_PASS : n.rssi >= -80 ? DOC_WARN : DOC_FAIL;
        finish(DOC_LINK, st, s, d0, d1, d2);
    }

    // ---- DHCP -------------------------------------------------------------
    begin(DOC_DHCP, "waiting for ip");
    if (!netmgr_wait_ip(12000)) {
        finish(DOC_DHCP, DOC_FAIL, "no address", "no DHCP offer in 12s", "AP up but router silent?", "try another network");
        skip_from(DOC_GW, "no ip");
        goto done;
    }
    netmgr_get(&n);
    snprintf(s, sizeof(s), "%s", n.ip);
    snprintf(d0, sizeof(d0), "ip %s", n.ip);
    snprintf(d1, sizeof(d1), "gw %s", n.gw);
    snprintf(d2, sizeof(d2), "dns %s", n.dns[0][0] ? n.dns[0] : "none!");
    if (!n.dns[0][0]) finish(DOC_DHCP, DOC_WARN, "no dns given", d0, d1, d2);
    else if (n.dhcp_ms > 5000) { snprintf(s, sizeof(s), "slow %lu.%lus", (unsigned long)n.dhcp_ms / 1000, (unsigned long)(n.dhcp_ms % 1000) / 100); finish(DOC_DHCP, DOC_WARN, s, d0, d1, d2); }
    else finish(DOC_DHCP, DOC_PASS, s, d0, d1, d2);

    // ---- GATEWAY ----------------------------------------------------------
    begin(DOC_GW, "ping gateway");
    {
        ping_result_t p;
        probe_ping(n.gw, 5, 250, 1000, &p);
        snprintf(d0, sizeof(d0), "%s", n.gw);
        snprintf(d1, sizeof(d1), "%u/%u replies, loss %u%%", p.recv, p.sent, p.loss_pct);
        snprintf(d2, sizeof(d2), "min %u avg %u max %u ms", p.min_ms, p.avg_ms, p.max_ms);
        if (p.recv == 0) finish(DOC_GW, DOC_WARN, "no reply", d0, "no icmp reply from router", "(some routers block it)");
        else {
            snprintf(s, sizeof(s), "%ums %u%% loss", p.avg_ms, p.loss_pct);
            finish(DOC_GW, (p.loss_pct > 20 || p.avg_ms > 100) ? DOC_WARN : DOC_PASS, s, d0, d1, d2);
        }
    }

    // ---- DNS --------------------------------------------------------------
    begin(DOC_DNS, "resolve names");
    {
        const char *names[3] = { "google.com", "cloudflare.com", "example.com" };
        int ok = 0, worst = 0, total = 0;
        char ip[16] = "";
        for (int i = 0; i < 3; i++) {
            char act[27]; snprintf(act, sizeof(act), "dns %.10s", names[i]); activity(act);
            int ms = probe_resolve(names[i], ip, sizeof(ip));
            if (ms >= 0) { ok++; total += ms; if (ms > worst) worst = ms; }
        }
        // A resolver that answers for a name that cannot exist is hijacking
        // NXDOMAIN (ISP "search assist" pages, some captive portals).
        char bogus[48]; snprintf(bogus, sizeof(bogus), "nx%08lx.example.com", (unsigned long)esp_random());
        activity("dns hijack?");
        char bip[16] = "";
        bool hijack = probe_resolve(bogus, bip, sizeof(bip)) >= 0;

        snprintf(d0, sizeof(d0), "resolver %s", n.dns[0]);
        snprintf(d1, sizeof(d1), "%d/3 ok, avg %dms", ok, ok ? total / ok : 0);
        if (ok == 0) finish(DOC_DNS, DOC_FAIL, "no answers", d0, "google/cloudflare/example", "all failed to resolve");
        else if (hijack) { snprintf(d2, sizeof(d2), "bogus name -> %s", bip); finish(DOC_DNS, DOC_WARN, "nxdomain hijack", d0, d1, d2); }
        else if (ok < 3) { snprintf(s, sizeof(s), "%d/3 ok", ok); finish(DOC_DNS, DOC_WARN, s, d0, d1, "some names failed"); }
        else if (worst > 500) { snprintf(s, sizeof(s), "slow %dms", worst); finish(DOC_DNS, DOC_WARN, s, d0, d1, "slowest lookup >500ms"); }
        else { snprintf(s, sizeof(s), "ok %dms", total / 3); finish(DOC_DNS, DOC_PASS, s, d0, d1, "nxdomain honest"); }
    }

    // ---- WAN (raw internet reachability) ---------------------------------
    begin(DOC_WAN, "ping 1.1.1.1");
    {
        ping_result_t a, b;
        probe_ping("1.1.1.1", 4, 250, 1500, &a);
        activity("ping 8.8.8.8");
        probe_ping("8.8.8.8", 4, 250, 1500, &b);
        snprintf(d0, sizeof(d0), "1.1.1.1 %u/%u avg %ums", a.recv, a.sent, a.avg_ms);
        snprintf(d1, sizeof(d1), "8.8.8.8 %u/%u avg %ums", b.recv, b.sent, b.avg_ms);
        ping_result_t *best = (a.recv >= b.recv) ? &a : &b;
        if (a.recv == 0 && b.recv == 0) finish(DOC_WAN, DOC_FAIL, "unreachable", d0, d1, "no icmp to the internet");
        else {
            snprintf(s, sizeof(s), "%ums %u%% loss", best->avg_ms, best->loss_pct);
            snprintf(d2, sizeof(d2), "jitter max %u ms", best->max_ms > best->min_ms ? best->max_ms - best->min_ms : 0);
            doc_state_t st = (best->loss_pct > 25 || best->avg_ms > 250) ? DOC_WARN : DOC_PASS;
            finish(DOC_WAN, st, s, d0, d1, d2);
        }
    }

    // ---- CAPTIVE PORTAL ---------------------------------------------------
    begin(DOC_PORTAL, "http probe");
    {
        http_result_t h;
        probe_http("http://connectivitycheck.gstatic.com/generate_204", 6000, &h);
        if (h.status == 204) {
            snprintf(s, sizeof(s), "none %dms", h.ms);
            finish(DOC_PORTAL, DOC_PASS, s, "generate_204 -> 204", "no captive portal", "plain http is clean");
        } else if (h.status >= 300 && h.status < 400) {
            const char *loc = h.location;
            if (strncmp(loc, "http://", 7) == 0) loc += 7; else if (strncmp(loc, "https://", 8) == 0) loc += 8;
            snprintf(d1, sizeof(d1), "-> %.24s", loc);
            snprintf(d0, sizeof(d0), "redirected (%d)", h.status);
            finish(DOC_PORTAL, DOC_FAIL, "captive portal", d0, d1, "open a browser to log in");
        } else if (h.status == 200) {
            finish(DOC_PORTAL, DOC_FAIL, "content swap", "expected 204, got 200", "portal is faking pages", "open a browser to log in");
        } else if (h.status > 0) {
            snprintf(s, sizeof(s), "http %d", h.status);
            finish(DOC_PORTAL, DOC_WARN, s, "unexpected status", "probe: gstatic.com", NULL);
        } else {
            snprintf(d0, sizeof(d0), "%.26s", h.err);
            finish(DOC_PORTAL, DOC_FAIL, "http blocked", d0, "port 80 not reachable", "dns ok but no http");
        }
    }

    // ---- TLS --------------------------------------------------------------
    begin(DOC_TLS, "tls handshake");
    {
        http_result_t h;
        probe_http("https://www.google.com/generate_204", 8000, &h);
        netmgr_get(&n);
        if (h.status == 204 || h.status == 200) {
            snprintf(s, sizeof(s), "ok %dms", h.ms);
            finish(DOC_TLS, DOC_PASS, s, "cert chain verified", "no interception seen", "https works");
        } else if (h.status == -2) {
            snprintf(d0, sizeof(d0), "%.26s", h.err);
            finish(DOC_TLS, DOC_FAIL, n.time_synced ? "cert failed" : "fail (clock?)", d0,
                   n.time_synced ? "possible tls interception" : "clock not set, cert dates", "fail before sntp sync");
        } else if (h.status > 0) {
            snprintf(s, sizeof(s), "http %d", h.status);
            finish(DOC_TLS, DOC_WARN, s, "handshake ok, odd status", NULL, NULL);
        } else {
            snprintf(d0, sizeof(d0), "%.26s", h.err);
            finish(DOC_TLS, DOC_FAIL, "no https", d0, "port 443 blocked?", NULL);
        }
    }

    // ---- IPv6 -------------------------------------------------------------
    begin(DOC_IPV6, "ipv6 check");
    {
        netmgr_refresh_link();
        netmgr_get(&n);
        if (!n.ip6_global[0]) {
            finish(DOC_IPV6, DOC_WARN, "no global addr", "no router advert / slaac", "ipv4 only network", "(very common, not fatal)");
        } else {
            ping_result_t p;
            activity("ping6 1.1.1.1");
            probe_ping("2606:4700:4700::1111", 3, 300, 1500, &p);
            snprintf(d0, sizeof(d0), "%.26s", n.ip6_global);
            snprintf(d1, sizeof(d1), "%.26s", n.ip6_global + (strlen(n.ip6_global) > 26 ? 26 : 0));
            if (p.recv) { snprintf(s, sizeof(s), "ok %ums", p.avg_ms); snprintf(d2, sizeof(d2), "ping6 2606:4700::1111 ok"); finish(DOC_IPV6, DOC_PASS, s, d0, d1, d2); }
            else finish(DOC_IPV6, DOC_WARN, "addr, no route", d0, d1, "global addr but no ping6");
        }
    }

    // ---- NAT / public address / hops -------------------------------------
    begin(DOC_NAT, "public ip?");
    {
        http_result_t h;
        // ip-api answers in its own canonical order whatever the fields list says:
        // countryCode, isp, query.
        probe_http("http://ip-api.com/line/?fields=countryCode,isp,query", 6000, &h);
        char pub[16] = "", isp[40] = "", cc[4] = "";
        if (h.status == 200) {
            // Response is one field per line: query, isp, countryCode.
            char *p = h.body, *l1 = strchr(p, '\n');
            if (l1) { *l1 = '\0'; snprintf(cc, sizeof(cc), "%.2s", p); p = l1 + 1;
                char *l2 = strchr(p, '\n');
                if (l2) { *l2 = '\0'; snprintf(isp, sizeof(isp), "%s", p); p = l2 + 1;
                    char *l3 = strchr(p, '\n'); if (l3) *l3 = '\0';
                    snprintf(pub, sizeof(pub), "%.15s", p); } }
        }
        activity("traceroute");
        trace_hop_t hops[6];
        int nh = probe_traceroute("1.1.1.1", 6, 900, hops);
        int first_public = -1, private_hops = 0;
        for (int i = 0; i < nh; i++) {
            if (hops[i].timeout) continue;
            if (probe_ip_is_private(hops[i].ip) || probe_ip_is_cgnat(hops[i].ip)) private_hops = i + 1;
            else if (first_public < 0) first_public = i + 1;
        }
        bool cgnat = probe_ip_is_cgnat(pub) || (nh > 1 && !hops[1].timeout && probe_ip_is_cgnat(hops[1].ip));
        bool double_nat = private_hops >= 2;

        snprintf(d0, sizeof(d0), "public %s", pub[0] ? pub : "unknown");
        if (isp[0]) snprintf(d1, sizeof(d1), "%.2s %.23s", cc, isp); else snprintf(d1, sizeof(d1), "%d hops traced", nh);
        if (nh >= 2 && !hops[1].timeout) snprintf(d2, sizeof(d2), "hop2 %s", hops[1].ip);
        else snprintf(d2, sizeof(d2), "hop2 no reply");

        if (!pub[0] && nh == 0) finish(DOC_NAT, DOC_WARN, "unknown", d0, "ip-api unreachable", "traceroute empty");
        else if (cgnat) finish(DOC_NAT, DOC_WARN, "cgnat", d0, d1, "carrier-grade nat (100.64/10)");
        else if (double_nat) { snprintf(d2, sizeof(d2), "hop2 %s private", hops[1].ip); finish(DOC_NAT, DOC_WARN, "private hop2", d0, d1, "double nat or isp-internal"); }
        else { snprintf(s, sizeof(s), "single %s", first_public > 0 ? "" : "nat"); if (first_public > 0) snprintf(s, sizeof(s), "ok, %d hop%s", first_public - 1 > 0 ? first_public - 1 : 1, first_public - 1 == 1 ? "" : "s"); finish(DOC_NAT, DOC_PASS, s, d0, d1, d2); }
    }

    // ---- TIME -------------------------------------------------------------
    begin(DOC_TIME, "sntp check");
    {
        netmgr_get(&n);
        time_t now = time(NULL);
        struct tm tm; localtime_r(&now, &tm);
        char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
        snprintf(d0, sizeof(d0), "%s", ts);
        if (n.time_synced) finish(DOC_TIME, DOC_PASS, "synced", d0, "pool.ntp.org answered", "udp/123 open");
        else finish(DOC_TIME, DOC_WARN, "not synced", d0, "no sntp reply yet", "udp/123 blocked?");
    }

    // ---- SPEED ------------------------------------------------------------
    begin(DOC_SPEED, "download 4s");
    {
        uint32_t mbps = 0; char err[32];
        uint32_t bytes = probe_http_download(SPEED_URL, 4000, &mbps, err, sizeof(err));
        const char *u = SPEED_URL;
        if (strncmp(u, "http://", 7) == 0) u += 7; else if (strncmp(u, "https://", 8) == 0) u += 8;
        snprintf(d0, sizeof(d0), "%.26s", u);
        if (bytes == 0) { snprintf(d1, sizeof(d1), "%.26s", err); finish(DOC_SPEED, DOC_WARN, "no download", d0, d1, "speed url unreachable"); }
        else {
            snprintf(s, sizeof(s), "%lu.%lu Mbps", (unsigned long)mbps / 10, (unsigned long)mbps % 10);
            snprintf(d1, sizeof(d1), "%lu KB in 4s", (unsigned long)bytes / 1024);
            snprintf(d2, sizeof(d2), "down %lu.%lu Mbit/s", (unsigned long)mbps / 10, (unsigned long)mbps % 10);
            finish(DOC_SPEED, mbps < 200 ? DOC_WARN : DOC_PASS, s, d0, d1, d2);   // mbps is x10
        }
    }

done:
    lock();
    s_rep.n_pass = s_rep.n_warn = s_rep.n_fail = 0;
    for (int i = 0; i < DOC_N; i++) {
        if (s_rep.checks[i].state == DOC_PASS) s_rep.n_pass++;
        else if (s_rep.checks[i].state == DOC_WARN) s_rep.n_warn++;
        else if (s_rep.checks[i].state == DOC_FAIL) s_rep.n_fail++;
    }
    s_rep.elapsed_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    s_rep.current = -1;
    s_rep.running = false;
    s_rep.done = true;
    s_rep.activity[0] = '\0';
    write_report();
    unlock();
    s_busy = false;
    vTaskDelete(NULL);
}

void doctor_start(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (s_busy) return;
    s_busy = true;
    lock();
    memset(&s_rep, 0, sizeof(s_rep));
    for (int i = 0; i < DOC_N; i++) snprintf(s_rep.checks[i].name, sizeof(s_rep.checks[i].name), "%s", NAMES[i]);
    s_rep.running = true;
    s_rep.current = -1;
    unlock();
    xTaskCreatePinnedToCore(doctor_task, "doctor", 8192, NULL, 5, NULL, 0);
}

void doctor_get(doctor_report_t *out)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    lock(); *out = s_rep; unlock();
}

bool doctor_busy(void) { return s_busy; }
