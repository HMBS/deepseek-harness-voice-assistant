/* DeepSeek Harness Voice Assistant: a purpose-built voice assistant for the
 * Waveshare ESP32-S3-Touch-AMOLED-2.16.
 *
 * One loop, no agent framework:
 *
 *   hold BOOT -> record -> relay does Deepgram STT -> DeepSeek replies
 *             -> relay does Deepgram TTS -> play through the speaker
 *
 * Everything is configured over the serial console at boot:
 *
 *   cfg set wifi_ssid <ssid>     cfg set wifi_pass <pass>
 *   cfg set relay_url http://HOST:8799/
 *   cfg set deepseek_key <key>
 *   cfg save && reboot
 *
 * Serial commands:  cfg show | cfg set <k> <v> | cfg save | reboot
 * BOOT is GPIO0 on this board (a strapping pin, so it must read high at boot).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "config.h"
#include "wifi.h"
#include "audio.h"
#include "net.h"
#include "ui.h"

static const char *TAG = "voice";

#define BTN_GPIO      GPIO_NUM_0     /* BOOT on this board */
#define SCREEN_BTN_GPIO GPIO_NUM_18  /* KEY3: user button */
#define MAX_WAV_BYTES (16000 * 2 * 20 + 44)   /* 20 s ceiling */

static voice_cfg_t     s_cfg;
static QueueHandle_t   s_trigger;
static uint8_t        *s_wav;
static char            s_last_reply[1024];
static char            s_last_heard[512];

/* ── button ───────────────────────────────────────────────────────────── */

/* Poll BOOT. A press is a falling edge followed by a release; report it once
 * the button has been held past the debounce window and released. */
static volatile bool s_button_held = false;

/* Polled by the recorder so it stops shortly after the button is released. */
static bool button_is_held(void)
{
    return s_button_held;
}

static void button_task(void *arg)
{
    gpio_set_direction(BTN_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_GPIO, GPIO_PULLUP_ONLY);

    int level = gpio_get_level(BTN_GPIO);
    ESP_LOGI(TAG, "button on GPIO%d, idle=%d", BTN_GPIO, level);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(15));

        int now = gpio_get_level(BTN_GPIO);
        if (now == level) {
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(30));      /* debounce */
        if (gpio_get_level(BTN_GPIO) != now) {
            continue;
        }
        level = now;

        if (level == 0) {
            /* Press: start listening immediately, so the first words are
             * captured. The recorder runs until the button is released. */
            s_button_held = true;
            ui_screen_set(true);
            ESP_LOGI(TAG, "press -> listening");
            xQueueSend(s_trigger, &(int){1}, 0);
        } else {
            s_button_held = false;
            ESP_LOGI(TAG, "release");
        }
    }
}

/* KEY3 turns the screen off and on again. One task per physical button keeps
 * their debounce state independent. */
static void screen_button_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << SCREEN_BTN_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    int level = gpio_get_level(SCREEN_BTN_GPIO);
    ESP_LOGI(TAG, "screen button on GPIO%d, idle=%d", SCREEN_BTN_GPIO, level);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(15));

        int now = gpio_get_level(SCREEN_BTN_GPIO);
        if (now == level) {
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(30));      /* debounce */
        if (gpio_get_level(SCREEN_BTN_GPIO) != now) {
            continue;
        }
        level = now;

        if (level == 0) {                   /* active low: pressed */
            bool was_on = ui_screen_is_on();
            ui_screen_toggle();
            ESP_LOGI(TAG, "KEY3 tapped: screen %s", was_on ? "off" : "on");
        }
    }
}

/* ── the voice loop ───────────────────────────────────────────────────── */

static int64_t s_first_audio_us;

static int ms_since(int64_t t0)
{
    return (int)((esp_timer_get_time() - t0) / 1000);
}

static void handle_trigger(void)
{
    /* Drop presses that queued up while a previous answer was being prepared:
     * they would otherwise be played back one after another, which looks like
     * a huge delay. */
    int stale = 0;
    while (xQueueReceive(s_trigger, &stale, 0) == pdTRUE) {
        ESP_LOGI(TAG, "dropping a queued press");
    }

    int64_t t_start = esp_timer_get_time();
    int64_t t_rec = 0, t_stt = 0, t_ask = 0, t_tts = 0, t_play = 0;

    ESP_LOGI(TAG, "trigger: recording");
    ui_show(UI_LISTENING, NULL);

    int recorded = (int)audio_record_held(s_wav, MAX_WAV_BYTES,
                                         s_cfg.record_ms, 400,
                                         button_is_held);
    if (recorded <= 0) {
        ESP_LOGW(TAG, "nothing recorded (ret=%d)", recorded);
        ui_show(UI_ERROR, "did not catch that");
        vTaskDelay(pdMS_TO_TICKS(1500));
        ui_show(UI_READY, NULL);
        return;
    }
    t_rec = esp_timer_get_time();
    ESP_LOGI(TAG, "recorded %d bytes", recorded);

    ui_show(UI_TRANSCRIBING, NULL);
    ESP_LOGI(TAG, "posting to relay %s", s_cfg.relay_url);

    char heard[512] = { 0 };
    char when[80]   = { 0 };
    if (!net_transcribe(s_cfg.relay_url, s_wav, (size_t)recorded,
                        heard, sizeof(heard), when, sizeof(when), 40000)) {
        ESP_LOGW(TAG, "transcription failed");
        ui_show(UI_ERROR, "speech not recognised");
        vTaskDelay(pdMS_TO_TICKS(2000));
        ui_show(UI_READY, NULL);
        return;
    }
    if (!heard[0]) {
        ESP_LOGW(TAG, "transcript empty");
        ui_show(UI_ERROR, "did not catch that");
        vTaskDelay(pdMS_TO_TICKS(1500));
        ui_show(UI_READY, NULL);
        return;
    }

    t_stt = esp_timer_get_time();
    ESP_LOGI(TAG, "heard: %s", heard);
    strlcpy(s_last_heard, heard, sizeof(s_last_heard));

    ui_show(UI_THINKING, heard);

    /* The relay returned the host's clock with the transcript, so "what time
     * is it?" works without another request in the critical path. */
    char sys_prompt[sizeof(s_cfg.system_prompt) + 128];
    if (when[0]) {
        snprintf(sys_prompt, sizeof(sys_prompt),
                 "%s The current local date and time is %s. Use it when asked.",
                 s_cfg.system_prompt, when);
    } else {
        strlcpy(sys_prompt, s_cfg.system_prompt, sizeof(sys_prompt));
    }

    char reply[1024] = { 0 };
    if (!net_ask(s_cfg.deepseek_url, s_cfg.deepseek_key, s_cfg.deepseek_model,
                 sys_prompt,
                 heard, reply, sizeof(reply), 60000)) {
        ESP_LOGW(TAG, "deepseek failed");
        ui_show(UI_ERROR, "no reply from DeepSeek");
        vTaskDelay(pdMS_TO_TICKS(2000));
        ui_show(UI_READY, NULL);
        return;
    }

    t_ask = esp_timer_get_time();
    ESP_LOGI(TAG, "reply: %s", reply);
    strlcpy(s_last_reply, reply, sizeof(s_last_reply));

    ui_show(UI_SPEAKING, reply);

    /* Download the whole clip before playing. Streaming from Deepgram arrives
     * slower than real time on this link, which drains the I2S buffer and
     * makes the voice stutter; buffering guarantees smooth speech. */
    uint8_t *pcm = NULL;
    size_t   pcm_len = 0;
    s_first_audio_us = 0;
    t_tts = esp_timer_get_time();

    bool got = net_speak(s_cfg.relay_url, reply, &pcm, &pcm_len, 20000) && pcm;
    if (!got) {
        ESP_LOGW(TAG, "tts failed, retrying once");
        vTaskDelay(pdMS_TO_TICKS(400));
        got = net_speak(s_cfg.relay_url, reply, &pcm, &pcm_len, 20000) && pcm;
    }

    if (got) {
        s_first_audio_us = esp_timer_get_time();
        if (!audio_play_pcm(pcm, pcm_len)) {
            ESP_LOGW(TAG, "playback failed");
            ui_show(UI_ERROR, reply);
        }
        t_play = esp_timer_get_time();
        free(pcm);
    } else {
        /* Keep the text on screen but make the failure obvious. */
        ESP_LOGW(TAG, "speech synthesis failed, showing text only");
        ui_show(UI_ERROR, reply);
    }

    int rec_ms   = t_rec ? (int)((t_rec - t_start) / 1000) : 0;
    int stt_ms   = t_stt ? (int)((t_stt - t_rec) / 1000) : 0;
    int ask_ms   = t_ask ? (int)((t_ask - t_stt) / 1000) : 0;
    int audio_ms = s_first_audio_us
                       ? (int)((s_first_audio_us - t_ask) / 1000) : 0;
    int play_ms  = (t_play && s_first_audio_us)
                       ? (int)((t_play - s_first_audio_us) / 1000) : 0;
    ESP_LOGI(TAG, "timings: record %d ms, stt %d ms, deepseek %d ms, "
                  "audio ready %d ms, playback %d ms, total %d ms",
             rec_ms, stt_ms, ask_ms, audio_ms, play_ms, ms_since(t_start));

    vTaskDelay(pdMS_TO_TICKS(600));
    ui_show(UI_READY, NULL);
}

static void voice_task(void *arg)
{
    int held = 0;
    for (;;) {
        if (xQueueReceive(s_trigger, &held, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "trigger");
            handle_trigger();
        }
    }
}

/* ── console ──────────────────────────────────────────────────────────── */

static int cmd_cfg(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: cfg show | cfg set <key> <value> | cfg save\n");
        return 1;
    }
    if (!strcmp(argv[1], "show")) {
        cfg_print(&s_cfg);
        return 0;
    }
    if (!strcmp(argv[1], "save")) {
        if (!cfg_save(&s_cfg)) {
            printf("save failed\n");
            return 1;
        }
        printf("saved\n");
        return 0;
    }
    if (!strcmp(argv[1], "set")) {
        if (argc < 4) {
            printf("usage: cfg set <key> <value>\n");
            return 1;
        }
        /* Numeric keys parse as integers; the rest are strings. */
        if (!strcmp(argv[2], "record_ms") || !strcmp(argv[2], "volume")) {
            int v = atoi(argv[3]);
            if (!cfg_set_int(&s_cfg, argv[2], v)) {
                printf("invalid value\n");
                return 1;
            }
        } else {
            if (!cfg_set_str(&s_cfg, argv[2], argv[3])) {
                printf("invalid value\n");
                return 1;
            }
        }
        printf("set (use `cfg save` to persist)\n");
        return 0;
    }
    printf("unknown cfg command '%s'\n", argv[1]);
    return 1;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("wifi %s\n", wifi_is_connected() ? "connected" : "down");
    printf("audio %s\n", audio_ready() ? "ready" : "not started");
    printf("display %s\n", ui_ready() ? "ready" : "down");
    printf("last heard: %s\n", s_last_heard[0] ? s_last_heard : "-");
    printf("last reply: %s\n", s_last_reply[0] ? s_last_reply : "-");
    printf("free heap : %u bytes, largest block %u\n",
           (unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_restart();
    return 0;
}

static void console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();

    repl_cfg.prompt = "voice> ";
    repl_cfg.max_cmdline_length = 200;

    /* This board's USB-C is the S3's own USB Serial/JTAG, and the BSP ships
     * the console there. Using the UART console would give no prompt at all. */
    esp_console_dev_usb_serial_jtag_config_t usb_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usb_cfg, &repl_cfg, &repl));

    const esp_console_cmd_t cmds[] = {
        { .command = "cfg",    .help = "cfg show|set|save", .func = cmd_cfg },
        { .command = "status", .help = "show runtime state", .func = cmd_status },
        { .command = "reboot", .help = "restart the board",  .func = cmd_reboot },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}

/* ── entry ────────────────────────────────────────────────────────────── */

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    cfg_load(&s_cfg);

    s_trigger = xQueueCreate(4, sizeof(int));
    s_wav = heap_caps_malloc(MAX_WAV_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_wav) {
        s_wav = malloc(MAX_WAV_BYTES);
    }
    if (!s_wav) {
        ESP_LOGE(TAG, "no room for the audio buffer");
        return;
    }

    console_start();

    ui_start();
    ui_show(UI_BOOT, "starting");

    ESP_LOGI(TAG, "voice gadget starting");
    cfg_print(&s_cfg);

    if (!wifi_connect(s_cfg.wifi_ssid, s_cfg.wifi_pass, 20000)) {
        ui_show(UI_ERROR, "Wi-Fi not connected");
        ESP_LOGE(TAG, "set credentials with `cfg set wifi_ssid ...` then `cfg save` and `reboot`");
        /* Park here so the console stays usable for configuration.
         * Returning from app_main deletes the task and takes the REPL with it. */
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    audio_cfg_t acfg = {
        .sample_rate = 16000,
        .channels    = 1,
        .bits        = 16,
        .record_ms   = s_cfg.record_ms,
        .volume      = s_cfg.volume,
        .play_gain   = s_cfg.play_gain,
    };
    if (!audio_start(&acfg)) {
        ui_show(UI_ERROR, "audio init failed");
        ESP_LOGE(TAG, "audio init failed");
    }

    xTaskCreate(button_task, "button", 4096, NULL, 5, NULL);
    xTaskCreate(screen_button_task, "sbtn", 3072, NULL, 4, NULL);
    xTaskCreate(voice_task,  "voice", 16384, NULL, 5, NULL);

    ui_show(UI_READY, NULL);
    ESP_LOGI(TAG, "ready: hold BOOT to talk");

    /* Park the main task. Returning from app_main deletes the task, which on
     * this IDF version also tears down the console REPL. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}