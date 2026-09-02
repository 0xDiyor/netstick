#include "netmgr.h"
#include "secrets.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "ping/ping_sock.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include <string.h>
#include <time.h>

static const char *TAG = "net";

#define BIT_GOT_IP BIT0

static net_status_t     s_st;
static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_events;
static esp_netif_t      *s_netif;
static int               s_retry_delay_ms = 1000;
static volatile bool     s_scan_hold = false;
static volatile int64_t  s_next_retry_us = 0;
static volatile bool     s_connecting = false;

static void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static uint32_t now_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

// ------------------------------------------------------------------ ping ---
static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    uint32_t elapsed = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));

    lock();
    s_st.rtt_last = (uint16_t)(elapsed > 9999 ? 9999 : elapsed);
    s_st.rtt[s_st.rtt_head] = s_st.rtt_last;
    s_st.rtt_head = (s_st.rtt_head + 1) % NET_RTT_HIST;
    s_st.ping_sent++;
    unlock();
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    lock();
    s_st.rtt[s_st.rtt_head] = 0;
    s_st.rtt_head = (s_st.rtt_head + 1) % NET_RTT_HIST;
    s_st.ping_sent++;
    s_st.ping_lost++;
    unlock();
}

static void ping_start(void)
{
    static esp_ping_handle_t ping = NULL;
    if (ping) return;

    ip_addr_t target;
    struct addrinfo hint = { 0 }, *res = NULL;
    if (getaddrinfo(PING_TARGET, NULL, &hint, &res) != 0 || !res) {
        ESP_LOGW(TAG, "cannot resolve ping target %s", PING_TARGET);
        return;
    }
    struct in_addr a = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    inet_addr_to_ip4addr(ip_2_ip4(&target), &a);
    target.type = IPADDR_TYPE_V4;
    freeaddrinfo(res);

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count       = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = 2000;
    cfg.timeout_ms  = 1500;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
    };
    if (esp_ping_new_session(&cfg, &cbs, &ping) == ESP_OK) esp_ping_start(ping);
}

// ---------------------------------------------------------------- events ---
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_next_retry_us = 0;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connecting = false;
        lock();
        if (s_st.connected) s_st.disconnects++;
        s_st.connected = false;
        s_st.has_ip = false;
        s_st.ip[0] = '\0';
        unlock();
        xEventGroupClearBits(s_events, BIT_GOT_IP);

        // Never block the event task - hand the retry to link_task with backoff.
        s_next_retry_us = esp_timer_get_time() + (int64_t)s_retry_delay_ms * 1000;
        s_retry_delay_ms = s_retry_delay_ms < 15000 ? s_retry_delay_ms * 2 : 15000;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        s_connecting = false;
        s_retry_delay_ms = 1000;
        lock();
        s_st.connected = true;
        s_st.connected_since = now_s();
        unlock();
        netmgr_refresh_link();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        lock();
        s_st.has_ip = true;
        snprintf(s_st.ip, sizeof(s_st.ip), IPSTR, IP2STR(&ev->ip_info.ip));
        unlock();
        xEventGroupSetBits(s_events, BIT_GOT_IP);
        ping_start();
    }
}

// Owns reconnection so the event handler never sleeps, and so a survey can
// hold off association attempts while it sweeps.
static void link_task(void *arg)
{
    while (1) {
        bool connected;
        lock(); connected = s_st.connected; unlock();

        if (connected) {
            netmgr_refresh_link();
        } else if (!s_scan_hold && !s_connecting &&
                   esp_timer_get_time() >= s_next_retry_us) {
            // Calling connect() while one is already in flight just logs
            // "sta is connecting, return error" every second.
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
        // cancel it. An established link can stay up - the sweep just goes
        // off-channel for a few seconds.
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

static void on_time_sync(struct timeval *tv)
{
    lock(); s_st.time_synced = true; unlock();
    ESP_LOGI(TAG, "time synced");
}

// ------------------------------------------------------------------ init ---
void netmgr_refresh_link(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;

    lock();
    memcpy(s_st.ssid, ap.ssid, sizeof(s_st.ssid) - 1);
    s_st.ssid[sizeof(s_st.ssid) - 1] = '\0';
    s_st.rssi    = ap.rssi;
    s_st.channel = ap.primary;
    // No band field in wifi_ap_record_t - the channel number disambiguates:
    // 2.4 GHz is 1-14, 5 GHz starts at 32.
    s_st.band    = (ap.primary >= 32) ? 5 : 2;
    const char *phy = ap.phy_11ax ? "11ax" : ap.phy_11ac ? "11ac"
                    : ap.phy_11n  ? "11n"  : ap.phy_11g  ? "11g" : "11b";
    strncpy(s_st.phy, phy, sizeof(s_st.phy) - 1);
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

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_wifi_event, NULL, NULL));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password) - 1);
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());

    // The whole point of the C5: let the radio use both bands, which also makes
    // a single esp_wifi_scan_start() sweep 2.4 GHz and 5 GHz in one pass.
    // This only takes effect once the driver is started.
    esp_err_t band_err = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if (band_err != ESP_OK) {
        ESP_LOGW(TAG, "band mode AUTO rejected (%s) - 2.4 GHz only", esp_err_to_name(band_err));
    } else {
        ESP_LOGI(TAG, "band mode: 2.4 GHz + 5 GHz");
    }

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp.sync_cb = on_time_sync;
    sntp.start = true;
    esp_netif_sntp_init(&sntp);

    setenv("TZ", TZ_STRING, 1);
    tzset();

    xTaskCreate(link_task, "link", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "wifi started, connecting to \"%s\"", WIFI_SSID);
    return ESP_OK;
}

void netmgr_get(net_status_t *out)
{
    lock();
    *out = s_st;
    unlock();
}

bool netmgr_wait_ip(uint32_t timeout_ms)
{
    EventBits_t b = xEventGroupWaitBits(s_events, BIT_GOT_IP, pdFALSE, pdTRUE,
                                        pdMS_TO_TICKS(timeout_ms));
    return (b & BIT_GOT_IP) != 0;
}
