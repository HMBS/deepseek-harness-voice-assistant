#include "config.h"
#include "secrets.h"

#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "cfg";

#define NS_NAME   "voice"
#define NS_KEY    "blob"

static void set_defaults(voice_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    /* Compile-time defaults from secrets.h. NVS values, when present, win. */
    strlcpy(cfg->wifi_ssid, CFG_WIFI_SSID, sizeof(cfg->wifi_ssid));
    strlcpy(cfg->wifi_pass, CFG_WIFI_PASSWORD, sizeof(cfg->wifi_pass));

    strlcpy(cfg->relay_url, CFG_RELAY_URL, sizeof(cfg->relay_url));

    strlcpy(cfg->deepseek_url, CFG_DEEPSEEK_URL, sizeof(cfg->deepseek_url));
    strlcpy(cfg->deepseek_key, CFG_DEEPSEEK_KEY, sizeof(cfg->deepseek_key));
    strlcpy(cfg->deepseek_model, CFG_DEEPSEEK_MODEL, sizeof(cfg->deepseek_model));

    /* Spoken replies should be short: this is a desk gadget, not an essayist. */
    strlcpy(cfg->system_prompt,
            "You are a voice assistant on a small handheld device. "
            "Reply in one or two short sentences, suitable for being read aloud.",
            sizeof(cfg->system_prompt));

    cfg->record_ms = CFG_RECORD_MS;
    cfg->volume    = CFG_VOLUME;
    cfg->play_gain = CFG_PLAY_GAIN;
}

void cfg_load(voice_cfg_t *cfg)
{
    set_defaults(cfg);

    nvs_handle_t h;
    if (nvs_open(NS_NAME, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "no saved config, using defaults");
        return;
    }

    size_t need = sizeof(*cfg);
    if (nvs_get_blob(h, NS_KEY, cfg, &need) != ESP_OK || need != sizeof(*cfg)) {
        ESP_LOGW(TAG, "saved config missing or stale, using defaults");
        set_defaults(cfg);
    }
    nvs_close(h);

    /* Guard against a truncated or corrupt record. */
    cfg->wifi_ssid[sizeof(cfg->wifi_ssid) - 1] = '\0';
    cfg->wifi_pass[sizeof(cfg->wifi_pass) - 1] = '\0';
    cfg->relay_url[sizeof(cfg->relay_url) - 1] = '\0';
    cfg->deepseek_key[sizeof(cfg->deepseek_key) - 1] = '\0';
    cfg->deepseek_model[sizeof(cfg->deepseek_model) - 1] = '\0';
    if (cfg->record_ms < 2000)  cfg->record_ms = 2000;
    if (cfg->record_ms > 60000) cfg->record_ms = 60000;
    if (cfg->volume < 0)        cfg->volume = 0;
    if (cfg->volume > 100)      cfg->volume = 100;
    if (cfg->play_gain < 1.0f)  cfg->play_gain = 1.0f;
    if (cfg->play_gain > 4.0f)  cfg->play_gain = 4.0f;
}

bool cfg_save(const voice_cfg_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NS_NAME, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_blob(h, NS_KEY, cfg, sizeof(*cfg));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

/* Copy `value` into a fixed-size char array, refusing anything too long. */
static bool copy_field(char *dst, size_t dst_size, const char *value)
{
    size_t n = strlen(value);
    if (n + 1 > dst_size) {
        ESP_LOGE(TAG, "value too long (%u > %u)", (unsigned)n,
                 (unsigned)(dst_size - 1));
        return false;
    }
    memcpy(dst, value, n + 1);
    return true;
}

bool cfg_set_str(voice_cfg_t *cfg, const char *key, const char *value)
{
    if      (!strcmp(key, "wifi_ssid"))     return copy_field(cfg->wifi_ssid,     sizeof(cfg->wifi_ssid),     value);
    if      (!strcmp(key, "wifi_pass"))     return copy_field(cfg->wifi_pass,     sizeof(cfg->wifi_pass),     value);
    if      (!strcmp(key, "relay_url"))     return copy_field(cfg->relay_url,     sizeof(cfg->relay_url),     value);
    if      (!strcmp(key, "deepseek_url"))  return copy_field(cfg->deepseek_url,  sizeof(cfg->deepseek_url),  value);
    if      (!strcmp(key, "deepseek_key"))  return copy_field(cfg->deepseek_key,  sizeof(cfg->deepseek_key),  value);
    if      (!strcmp(key, "deepseek_model"))return copy_field(cfg->deepseek_model,sizeof(cfg->deepseek_model),value);
    if      (!strcmp(key, "system_prompt")) return copy_field(cfg->system_prompt, sizeof(cfg->system_prompt), value);

    ESP_LOGW(TAG, "unknown config key '%s'", key);
    return false;
}

bool cfg_set_int(voice_cfg_t *cfg, const char *key, int value)
{
    if (!strcmp(key, "record_ms")) {
        if (value < 2000 || value > 60000) return false;
        cfg->record_ms = value;
        return true;
    }
    if (!strcmp(key, "volume")) {
        if (value < 0 || value > 100) return false;
        cfg->volume = value;
        return true;
    }
    ESP_LOGW(TAG, "unknown config key '%s'", key);
    return false;
}

/* Show a secret as its length only, never its value. */
static void print_masked(const char *label, const char *value)
{
    size_t n = strlen(value);
    if (n == 0)      printf("  %-16s <empty>\n", label);
    else             printf("  %-16s <set, %u chars>\n", label, (unsigned)n);
}

void cfg_print(const voice_cfg_t *cfg)
{
    printf("config:\n");
    printf("  %-16s %s\n", "wifi_ssid", cfg->wifi_ssid[0] ? cfg->wifi_ssid : "<empty>");
    print_masked("wifi_pass", cfg->wifi_pass);
    printf("  %-16s %s\n", "relay_url", cfg->relay_url);
    printf("  %-16s %s\n", "deepseek_url", cfg->deepseek_url);
    print_masked("deepseek_key", cfg->deepseek_key);
    printf("  %-16s %s\n", "deepseek_model", cfg->deepseek_model);
    printf("  %-16s %d ms\n", "record_ms", cfg->record_ms);
    printf("  %-16s %d\n", "volume", cfg->volume);
    printf("  %-16s %.2f\n", "play_gain", (double)cfg->play_gain);
}