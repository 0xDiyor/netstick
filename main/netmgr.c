#include "netmgr.h"
#include "secrets.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include <string.h>
#include <time.h>

static const char *TAG = "net";

#define BIT_GOT_IP BIT0

static net_status_t       s_st;
static SemaphoreHandle_t  s_lock;
static EventGroupHandle_t s_events;
static esp_netif_t       *s_netif;
static int                s_retry_delay_ms = 1000;
static volatile bool      s_scan_hold = false;
static volatile int64_t   s_next_retry_us = 0;
static volatile bool      s_connecting = false;
static volatile int64_t   s_assoc_at_us = 0;
static bool               s_pinned = false;
static bool               s_sntp_up = false;
static char               s_ssid[33], s_pass[65];

static void creds_load(void)
{
    s_ssid[0] = s_pass[0] = '\0';
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(s_ssid); nvs_get_str(h, "ssid", s_ssid, &n);
        n = sizeof(s_pass);         nvs_get_str(h, "pass", s_pass, &n);
        nvs_close(h);
    }
    if (!s_ssid[0] && strcmp(WIFI_SSID, "your-network") != 0 && WIFI_SSID[0]) {
        snprintf(s_ssid, sizeof(s_ssid), "%s", WIFI_SSID);
        snprintf(s_pass, sizeof(s_pass), "%s", WIFI_PASS);
    }
}

static void creds_save(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    if (s_ssid[0]) { nvs_set_str(h, "ssid", s_ssid); nvs_set_str(h, "pass", s_pass); }
    else nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}

bool        netmgr_has_creds(void) { return s_ssid[0] != '\0'; }
const char *netmgr_cfg_ssid(void)  { return s_ssid; }

static void on_time_sync(struct timeval *tv);
static void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }
static uint32_t now_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

const char *netmgr_reason_str(uint8_t r)
{
    switch (r) {
        case WIFI_REASON_AUTH_EXPIRE:          return "auth expire";
        case WIFI_REASON_AUTH_LEAVE:           return "auth leave";
        case WIFI_REASON_ASSOC_EXPIRE:         return "assoc expire";
        case WIFI_REASON_ASSOC_TOOMANY:        return "AP full";
        case WIFI_REASON_NOT_AUTHED:           return "not authed";
        case WIFI_REASON_NOT_ASSOCED:          return "not assoc";
        case WIFI_REASON_ASSOC_LEAVE:          return "we left";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "bad password?";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:    return "handshake t/o";
        case WIFI_REASON_BEACON_TIMEOUT:       return "beacon lost";
        case WIFI_REASON_NO_AP_FOUND:          return "no AP found";
        case WIFI_REASON_AUTH_FAIL:            return "auth failed";
        case WIFI_REASON_ASSOC_FAIL:           return "assoc failed";
        case WIFI_REASON_CONNECTION_FAIL:      return "connect fail";
        case WIFI_REASON_ROAMING:              return "roaming";
        default:                               return "reason ?";
    }
}

static void read_ip_info(void)
{
    esp_netif_ip_info_t ip;
    esp_netif_dns_info_t d;
    if (esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        snprintf(s_st.ip,   sizeof(s_st.ip),   IPSTR, IP2STR(&ip.ip));
        snprintf(s_st.mask, sizeof(s_st.mask), IPSTR, IP2STR(&ip.netmask));
        snprintf(s_st.gw,   sizeof(s_st.gw),   IPSTR, IP2STR(&ip.gw));
    }
    for (int i = 0; i < 2; i++) {
        s_st.dns[i][0] = '\0';
        if (esp_netif_get_dns_info(s_netif, i == 0 ? ESP_NETIF_DNS_MAIN : ESP_NETIF_DNS_BACKUP, &d) == ESP_OK
            && d.ip.type == ESP_IPADDR_TYPE_V4 && d.ip.u_addr.ip4.addr != 0) {
            snprintf(s_st.dns[i], sizeof(s_st.dns[i]), IPSTR, IP2STR(&d.ip.u_addr.ip4));
        }
    }
}

static void read_ip6(void)
{
    esp_ip6_addr_t a[8];
    int n = esp_netif_get_all_ip6(s_netif, a);
    s_st.ip6_global[0] = '\0';
    s_st.ip6_ll[0] = '\0';
    for (int i = 0; i < n; i++) {
        esp_ip6_addr_type_t t = esp_netif_ip6_get_addr_type(&a[i]);
        char buf[46];
        snprintf(buf, sizeof(buf), IPV6STR, IPV62STR(a[i]));
        if (t == ESP_IP6_ADDR_IS_GLOBAL && !s_st.ip6_global[0]) strcpy(s_st.ip6_global, buf);
        else if (t == ESP_IP6_ADDR_IS_LINK_LOCAL && !s_st.ip6_ll[0]) strcpy(s_st.ip6_ll, buf);
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_next_retry_us = 0;
        lock(); s_st.started = true; unlock();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        s_connecting = false;
        lock();
        if (s_st.connected) s_st.disconnects++;
        s_st.connected = false;
        s_st.has_ip = false;
        s_st.ip[0] = s_st.gw[0] = s_st.ip6_global[0] = s_st.ip6_ll[0] = '\0';
        s_st.last_reason = ev ? ev->reason : 0;
        snprintf(s_st.reason_str, sizeof(s_st.reason_str), "%s", netmgr_reason_str(s_st.last_reason));
        unlock();
        xEventGroupClearBits(s_events, BIT_GOT_IP);
        ESP_LOGW(TAG, "disconnected: %d (%s)", s_st.last_reason, s_st.reason_str);
        // Never block the event task - hand the retry to link_task with backoff.
        s_next_retry_us = esp_timer_get_time() + (int64_t)s_retry_delay_ms * 1000;
        s_retry_delay_ms = s_retry_delay_ms < 15000 ? s_retry_delay_ms * 2 : 15000;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        s_connecting = false;
        s_retry_delay_ms = 1000;
        s_assoc_at_us = esp_timer_get_time();
        lock();
        s_st.connected = true;
        s_st.connected_since = now_s();
        unlock();
        esp_netif_create_ip6_linklocal(s_netif);   // kicks off SLAAC
        netmgr_refresh_link();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        lock();
        s_st.has_ip = true;
        s_st.dhcp_ms = (uint32_t)((esp_timer_get_time() - s_assoc_at_us) / 1000);
        read_ip_info();
        unlock();
        xEventGroupSetBits(s_events, BIT_GOT_IP);
        ESP_LOGI(TAG, "ip %s gw %s dns %s (dhcp %lu ms)", s_st.ip, s_st.gw, s_st.dns[0],
                 (unsigned long)s_st.dhcp_ms);
        // (Re)start SNTP now that there is a route; left running from boot it
        // would sit in its retry backoff for minutes after a late join.
        if (s_sntp_up) esp_netif_sntp_deinit();
        esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        sntp.sync_cb = on_time_sync;
        sntp.start = true;
        s_sntp_up = esp_netif_sntp_init(&sntp) == ESP_OK;
    } else if (base == IP_EVENT && id == IP_EVENT_GOT_IP6) {
        lock(); read_ip6(); unlock();
        ESP_LOGI(TAG, "ipv6 global=%s ll=%s", s_st.ip6_global, s_st.ip6_ll);
    }
}

// Owns reconnection so the event handler never sleeps, and so a sweep can hold
// off association attempts while it runs.
static void link_task(void *arg)
{
    while (1) {
        bool connected;
        lock(); connected = s_st.connected; unlock();
        if (connected) {
            netmgr_refresh_link();
        } else if (s_ssid[0] && !s_scan_hold && !s_connecting && esp_timer_get_time() >= s_next_retry_us) {
            if (esp_wifi_connect() == ESP_OK) s_connecting = true;
            s_next_retry_us = esp_timer_get_time() + (int64_t)s_retry_delay_ms * 1000;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void netmgr_scan_hold(bool hold)
{
    s_scan_hold = hold;
    if (hold) {
        bool connected;
        lock(); connected = s_st.connected; unlock();
        // An in-flight association makes the driver reject scans outright, so
        // cancel it. An established link can stay up.
        if (!connected) {
            esp_wifi_disconnect();
            s_connecting = false;
            vTaskDelay(pdMS_TO_TICKS(150));
        }
    } else {
        s_retry_delay_ms = 1000;
        s_next_retry_us = 0;
    }
}

static void set_sta_config(const uint8_t *bssid)
{
    if (!s_ssid[0]) return;            // nothing configured: leave the driver alone
    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, s_pass, sizeof(wc.sta.password) - 1);
    // Open networks must not ask for WPA; otherwise require at least WPA2.
    wc.sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    if (bssid) { wc.sta.bssid_set = 1; memcpy(wc.sta.bssid, bssid, 6); }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
}

bool netmgr_connect_bssid(const uint8_t bssid[6], uint32_t timeout_ms)
{
    s_scan_hold = true;                // keep link_task's hands off
    esp_wifi_disconnect();
    s_connecting = false;
    vTaskDelay(pdMS_TO_TICKS(200));
    xEventGroupClearBits(s_events, BIT_GOT_IP);
    set_sta_config(bssid);
    s_pinned = true;
    ESP_LOGI(TAG, "pin to %02x:%02x:%02x:%02x:%02x:%02x", bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) { ESP_LOGW(TAG, "connect: %s", esp_err_to_name(err)); }
    bool ok = netmgr_wait_ip(timeout_ms);
    s_scan_hold = false;
    s_retry_delay_ms = 1000;
    s_next_retry_us = esp_timer_get_time() + 1000000;
    return ok;
}

void netmgr_connect_auto(bool reconnect)
{
    set_sta_config(NULL);
    s_pinned = false;
    if (reconnect) {
        esp_wifi_disconnect();
        s_connecting = false;
        s_retry_delay_ms = 1000;
        s_next_retry_us = 0;
    }
}

bool netmgr_pinned(void) { return s_pinned; }

static void on_time_sync(struct timeval *tv)
{
    lock(); s_st.time_synced = true; unlock();
    ESP_LOGI(TAG, "time synced");
}

void netmgr_refresh_link(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;
    wifi_phy_mode_t mode = WIFI_PHY_MODE_11B;
    esp_wifi_sta_get_negotiated_phymode(&mode);

    lock();
    memcpy(s_st.ssid, ap.ssid, sizeof(s_st.ssid) - 1);
    s_st.ssid[sizeof(s_st.ssid) - 1] = '\0';
    memcpy(s_st.bssid, ap.bssid, 6);
    s_st.rssi    = ap.rssi;
    s_st.channel = ap.primary;
    s_st.band    = (ap.primary >= 32) ? 5 : 2;   // no band field; 5 GHz starts at 32
    s_st.ax      = ap.phy_11ax;
    switch (ap.bandwidth) {
        case WIFI_BW_HT40: s_st.bw_mhz = 40; break;
        case WIFI_BW80:    s_st.bw_mhz = 80; break;
        case WIFI_BW160: case WIFI_BW80_BW80: s_st.bw_mhz = 160; break;
        default:           s_st.bw_mhz = 20; break;
    }
    const char *phy;
    switch (mode) {
        case WIFI_PHY_MODE_LR:    phy = "LR";   break;
        case WIFI_PHY_MODE_11B:   phy = "11b";  break;
        case WIFI_PHY_MODE_11G:   phy = "11g";  break;
        case WIFI_PHY_MODE_11A:   phy = "11a";  break;
        case WIFI_PHY_MODE_HT20:  phy = "HT20"; break;
        case WIFI_PHY_MODE_HT40:  phy = "HT40"; break;
        case WIFI_PHY_MODE_VHT20: phy = "VHT20"; break;
        case WIFI_PHY_MODE_HE20:  phy = "HE20"; break;
        default:                  phy = "?";    break;
    }
    strncpy(s_st.phy, phy, sizeof(s_st.phy) - 1);
    if (s_st.has_ip) read_ip6();
    unlock();
}

esp_err_t netmgr_init(void)
{
    s_lock   = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_GOT_IP6, on_wifi_event, NULL, NULL));

    creds_load();
    lock(); s_st.has_creds = s_ssid[0] != 0; unlock();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    set_sta_config(NULL);
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Let the radio use both bands; also makes one scan sweep 2.4 + 5 GHz.
    esp_err_t band_err = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if (band_err != ESP_OK) ESP_LOGW(TAG, "band mode AUTO rejected (%s)", esp_err_to_name(band_err));


    setenv("TZ", TZ_STRING, 1);
    tzset();

    xTaskCreate(link_task, "link", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "wifi started, %s%s", s_ssid[0] ? "joining " : "no network saved", s_ssid);
    return ESP_OK;
}

void netmgr_set_creds(const char *ssid, const char *pass)
{
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", pass ? pass : "");
    creds_save();
    lock(); s_st.has_creds = true; s_st.last_reason = 0; s_st.reason_str[0] = 0; s_st.disconnects = 0; unlock();
    ESP_LOGI(TAG, "credentials saved for \"%s\"", s_ssid);
    s_pinned = false;
    esp_wifi_disconnect();
    s_connecting = false;
    esp_wifi_set_mode(WIFI_MODE_STA);        // provisioning may have left AP+STA on
    set_sta_config(NULL);
    s_scan_hold = false;                     // provisioning held the reconnect loop
    s_retry_delay_ms = 1000;
    s_next_retry_us = 0;
}

void netmgr_forget(void)
{
    s_ssid[0] = s_pass[0] = '\0';
    creds_save();
    lock(); s_st.has_creds = false; unlock();
    esp_wifi_disconnect();
    s_connecting = false;
    set_sta_config(NULL);
}

void netmgr_get(net_status_t *out) { lock(); *out = s_st; unlock(); }
esp_netif_t *netmgr_netif(void) { return s_netif; }

bool netmgr_wait_ip(uint32_t timeout_ms)
{
    EventBits_t b = xEventGroupWaitBits(s_events, BIT_GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (b & BIT_GOT_IP) != 0;
}
