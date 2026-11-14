#include "wifi.h"

#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "wifi";

#define CONNECTED_BIT (1 << 0)
#define FAIL_BIT      (1 << 1)

static EventGroupHandle_t s_events;
static esp_netif_t       *s_netif;
static bool s_started;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected, retrying");
        if (s_events) {
            xEventGroupClearBits(s_events, CONNECTED_BIT);
        }
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "ip: " IPSTR, IP2STR(&ev->ip_info.ip));
        if (s_events) {
            xEventGroupSetBits(s_events, CONNECTED_BIT);
        }
    }
}

bool wifi_connect(const char *ssid, const char *pass, int timeout_ms)
{
    if (!ssid || !ssid[0]) {
        ESP_LOGE(TAG, "no SSID configured");
        return false;
    }
    if (strlen(ssid) >= sizeof(((wifi_config_t *)0)->sta.ssid)) {
        ESP_LOGE(TAG, "SSID too long");
        return false;
    }

    s_events = xEventGroupCreate();
    if (!s_events) {
        return false;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "netif_init: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
        return false;
    }

    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) {
        ESP_LOGE(TAG, "netif create failed");
        return false;
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_init: %s", esp_err_to_name(err));
        return false;
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL);

    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    if (pass && pass[0]) {
        strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    /* Don't let PMF or band steering get in the way on a small board. */
    cfg.sta.pmf_cfg.capable = false;
    cfg.sta.pmf_cfg.required = false;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start: %s", esp_err_to_name(err));
        return false;
    }

    s_started = true;

    /* Modem sleep adds hundreds of milliseconds of latency and throttles
     * throughput on this board; a desk gadget on mains does not need it. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI(TAG, "connecting to '%s' ...", ssid);

    EventBits_t bits = xEventGroupWaitBits(
        s_events, CONNECTED_BIT | FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if (!(bits & CONNECTED_BIT)) {
        ESP_LOGE(TAG, "did not connect within %d ms", timeout_ms);
        return false;
    }
    return true;
}

bool wifi_is_connected(void)
{
    if (!s_events) {
        return false;
    }
    return (xEventGroupGetBits(s_events) & CONNECTED_BIT) != 0;
}

void wifi_disconnect(void)
{
    if (s_started) {
        esp_wifi_disconnect();
    }
}