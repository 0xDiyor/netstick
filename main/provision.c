#include "provision.h"
#include "netmgr.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "prov";

#define AP_IP    "192.168.4.1"
#define MAX_NETS 24

static prov_status_t     s_st;
static SemaphoreHandle_t s_lock;
static esp_netif_t      *s_ap_netif;
static httpd_handle_t    s_httpd;
static TaskHandle_t      s_dns_task;
static volatile bool     s_dns_run;
static char              s_nets[MAX_NETS][33];
static int8_t            s_nets_rssi[MAX_NETS];
static bool              s_nets_open[MAX_NETS];
static int               s_n_nets;
static bool              s_ap_registered;

static void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }
static void set_state(prov_state_t st, const char *msg)
{
    lock(); s_st.state = st; if (msg) snprintf(s_st.msg, sizeof(s_st.msg), "%s", msg); unlock();
}

// ------------------------------------------------------------------ DNS ---
// Answers every A query with the AP address so the phone's connectivity probe
// lands on our page and the captive-portal sheet pops up by itself.
static void dns_task(void *arg)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    bind(s, (struct sockaddr *)&a, sizeof(a));
    struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t buf[512];
    while (s_dns_run) {
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12) continue;
        // Find the end of the question name, keep the query, append one answer.
        int q = 12;
        while (q < n && buf[q]) q += buf[q] + 1;
        q += 5;                                  // NUL + type + class
        if (q > n) continue;
        buf[2] = 0x81; buf[3] = 0x80;            // standard response, no error
        buf[6] = 0; buf[7] = 1;                  // ANCOUNT = 1
        buf[8] = buf[9] = buf[10] = buf[11] = 0; // no authority / additional
        uint8_t ans[16] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1 };
        memcpy(buf + q, ans, sizeof(ans));
        sendto(s, buf, q + sizeof(ans), 0, (struct sockaddr *)&from, fl);
    }
    close(s);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

// ----------------------------------------------------------------- HTTP ---
static const char PAGE_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>netstick wifi</title><style>body{background:#000;color:#8cffa0;font:18px/1.5 ui-monospace,Menlo,monospace;"
    "padding:1.2em;max-width:26em;margin:auto}h1{font-size:1.3em;color:#ebffeb;font-weight:normal}"
    "label{display:block;color:#46785a;margin-top:1em}input,select,button{width:100%;box-sizing:border-box;font:inherit;"
    "background:#06110a;color:#ebffeb;border:1px solid #46785a;border-radius:0;padding:.6em;margin-top:.3em}"
    "button{background:#8cffa0;color:#000;margin-top:1.5em}p{color:#46785a}</style></head><body>"
    "<h1>&gt;netstick wifi_</h1>";
static const char PAGE_TAIL[] = "</body></html>";

static void html_escape(char *out, size_t n, const char *in)
{
    size_t o = 0;
    for (; *in && o + 6 < n; in++) {
        if (*in == '<') { memcpy(out + o, "&lt;", 4); o += 4; }
        else if (*in == '>') { memcpy(out + o, "&gt;", 4); o += 4; }
        else if (*in == '&') { memcpy(out + o, "&amp;", 5); o += 5; }
        else if (*in == '"') { memcpy(out + o, "&quot;", 6); o += 6; }
        else out[o++] = *in;
    }
    out[o] = '\0';
}

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req, "<form method=post action=/save><label>network</label><select name=s "
                                  "onchange=\"document.getElementById('m').value=''\">");
    char line[160], esc[80];
    for (int i = 0; i < s_n_nets; i++) {
        html_escape(esc, sizeof(esc), s_nets[i]);
        snprintf(line, sizeof(line), "<option value=\"%s\">%s  (%d dBm%s)</option>", esc, esc, s_nets_rssi[i], s_nets_open[i] ? ", open" : "");
        httpd_resp_sendstr_chunk(req, line);
    }
    httpd_resp_sendstr_chunk(req, "<option value=\"\">other / hidden network</option></select>"
                                  "<label>or type the name</label><input id=m name=m autocomplete=off autocapitalize=none>"
                                  "<label>password</label><input name=p type=password autocomplete=off placeholder=\"empty for open networks\">"
                                  "<button>connect</button></form><p>the stick saves these and joins; watch its screen.</p>");
    httpd_resp_sendstr_chunk(req, PAGE_TAIL);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], 0 }; *o++ = (char)strtol(h, NULL, 16); s += 2;
        } else *o++ = *s;
    }
    *o = '\0';
}

static bool form_get(const char *body, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    const char *p = body;
    while (p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            const char *e = strchr(p + kl + 1, '&');
            size_t len = e ? (size_t)(e - p - kl - 1) : strlen(p + kl + 1);
            if (len >= n) len = n - 1;
            memcpy(out, p + kl + 1, len); out[len] = '\0';
            url_decode(out);
            return true;
        }
        p = strchr(p, '&'); if (p) p++;
    }
    out[0] = '\0';
    return false;
}

static void join_task(void *arg)
{
    char ssid[33]; lock(); strcpy(ssid, s_st.chosen); unlock();
    // Tear the AP down first: the phone's page has already been delivered and
    // the C5 cannot keep an AP on one band while joining on the other.
    s_dns_run = false;
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    vTaskDelay(pdMS_TO_TICKS(400));
    set_state(PROV_JOINING, "joining...");
    netmgr_set_creds(ssid, (const char *)arg);
    free(arg);
    if (netmgr_wait_ip(20000)) {
        net_status_t n; netmgr_get(&n);
        char m[27]; snprintf(m, sizeof(m), "ip %s", n.ip);
        set_state(PROV_JOINED, m);
    } else {
        net_status_t n; netmgr_get(&n);
        char m[27]; snprintf(m, sizeof(m), "%s", n.last_reason ? n.reason_str : "no address");
        set_state(PROV_FAILED, m);
    }
    vTaskDelete(NULL);
}

static esp_err_t save_post(httpd_req_t *req)
{
    char body[300] = { 0 };
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) break;
        got += r;
    }
    char sel[33], manual[33], pass[65], ssid[33];
    form_get(body, "s", sel, sizeof(sel));
    form_get(body, "m", manual, sizeof(manual));
    form_get(body, "p", pass, sizeof(pass));
    snprintf(ssid, sizeof(ssid), "%s", manual[0] ? manual : sel);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (!ssid[0]) {
        httpd_resp_sendstr_chunk(req, PAGE_HEAD);
        httpd_resp_sendstr_chunk(req, "<p>pick a network or type its name.</p><p><a href=/ style=\"color:#8cffa0\">&lt; back</a></p>");
        httpd_resp_sendstr_chunk(req, PAGE_TAIL);
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_OK;
    }
    lock(); snprintf(s_st.chosen, sizeof(s_st.chosen), "%s", ssid); unlock();
    char esc[80]; html_escape(esc, sizeof(esc), ssid);
    char line[200]; snprintf(line, sizeof(line), "<p style=\"color:#ebffeb\">saved. joining <b>%s</b> now.</p><p>this hotspot goes away; watch the stick's screen for the result.</p>", esc);
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req, line);
    httpd_resp_sendstr_chunk(req, PAGE_TAIL);
    httpd_resp_sendstr_chunk(req, NULL);

    char *p = strdup(pass);
    xTaskCreate(join_task, "join", 4096, p, 5, NULL);
    return ESP_OK;
}

// Every other URL (the OS connectivity probes included) redirects to the page,
// which is what makes the captive-portal sheet appear.
static esp_err_t redirect_any(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" AP_IP "/");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}
static esp_err_t probe_get(httpd_req_t *req) { return redirect_any(req, 0); }

// ------------------------------------------------------------------- AP ---
static void on_ap_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED)         { lock(); s_st.clients++; unlock(); }
    else if (id == WIFI_EVENT_AP_STADISCONNECTED) { lock(); if (s_st.clients) s_st.clients--; unlock(); }
}

static void scan_networks(void)
{
    netmgr_scan_hold(true);
    wifi_scan_config_t cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = { .min = 60, .max = 120 },
        .scan_time.passive = 150,
        .channel_bitmap = { .ghz_2_channels = 0x7FFE, .ghz_5_channels = 0x1FFFFFFE },
    };
    s_n_nets = 0;
    if (esp_wifi_scan_start(&cfg, true) == ESP_OK) {
        uint16_t n = 0; esp_wifi_scan_get_ap_num(&n);
        if (n > 64) n = 64;
        wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(*recs));
        if (recs) {
            esp_wifi_scan_get_ap_records(&n, recs);       // strongest first
            for (int i = 0; i < n && s_n_nets < MAX_NETS; i++) {
                if (!recs[i].ssid[0]) continue;
                bool dup = false;                          // one entry per SSID across bands
                for (int j = 0; j < s_n_nets; j++) if (!strcmp(s_nets[j], (char *)recs[i].ssid)) { dup = true; break; }
                if (dup) continue;
                snprintf(s_nets[s_n_nets], 33, "%s", recs[i].ssid);
                s_nets_rssi[s_n_nets] = recs[i].rssi;
                s_nets_open[s_n_nets] = recs[i].authmode == WIFI_AUTH_OPEN;
                s_n_nets++;
            }
            free(recs);
        }
    }
    esp_wifi_clear_ap_list();
    netmgr_scan_hold(false);
    lock(); s_st.n_scanned = s_n_nets; unlock();
}

static void start_task(void *arg)
{
    set_state(PROV_SCANNING, "scanning networks");
    scan_networks();

    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_registered) {
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_ap_event, NULL, NULL);
        s_ap_registered = true;
    }
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    wifi_config_t ap = { 0 };
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "netstick-%02x%02x", mac[4], mac[5]);
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    lock(); snprintf(s_st.ap_ssid, sizeof(s_st.ap_ssid), "%s", (char *)ap.ap.ssid); strcpy(s_st.ap_ip, AP_IP); s_st.clients = 0; unlock();

    esp_wifi_disconnect();
    netmgr_scan_hold(true);            // no station attempts while the AP is up
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    s_dns_run = true;
    xTaskCreate(dns_task, "dns", 3072, NULL, 4, &s_dns_task);

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 12;
    hc.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &hc) == ESP_OK) {
        httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
        httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post };
        httpd_register_uri_handler(s_httpd, &root);
        httpd_register_uri_handler(s_httpd, &save);
        static const char *probes[] = { "/generate_204", "/gen_204", "/hotspot-detect.html", "/ncsi.txt",
                                        "/connecttest.txt", "/redirect", "/canonical.html", "/success.txt", "/library/test/success.html" };
        for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
            httpd_uri_t u = { .uri = probes[i], .method = HTTP_GET, .handler = probe_get };
            httpd_register_uri_handler(s_httpd, &u);
        }
        httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, redirect_any);
    }
    set_state(PROV_WAITING, "waiting for phone");
    ESP_LOGI(TAG, "AP %s up, %d networks listed", s_st.ap_ssid, s_n_nets);
    vTaskDelete(NULL);
}

void prov_start(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    prov_status_t st; prov_get(&st);
    if (st.state != PROV_OFF && st.state != PROV_JOINED && st.state != PROV_FAILED) return;
    lock(); memset(&s_st, 0, sizeof(s_st)); s_st.state = PROV_SCANNING; unlock();
    xTaskCreate(start_task, "prov", 6144, NULL, 5, NULL);
}

void prov_stop(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    s_dns_run = false;
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    esp_wifi_set_mode(WIFI_MODE_STA);
    netmgr_scan_hold(false);
    set_state(PROV_OFF, "");
    ESP_LOGI(TAG, "AP down");
}

void prov_get(prov_status_t *out)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    lock(); *out = s_st; unlock();
}
