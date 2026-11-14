#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"

#include "mascot.h"

static const char *TAG = "ui";

#define BG       0x000000
#define BRIGHTNESS 80
#define TEXT     0xF2F4F8
#define DIM      0x8B93A3

#define ACCENT_READY      0x3A3F4B
#define ACCENT_LISTENING  0xE8453C
#define ACCENT_WORKING    0xF0A92E
#define ACCENT_SPEAKING   0x2EA3F0
#define ACCENT_ERROR      0xD64545

/* The BSP's default transfer size is the whole 480x480 frame (460 KB). That
 * overruns the SPI DMA on this board and leaves the panel wedged, so bring the
 * panel up with a small transfer and register it with the adapter ourselves. */
/* Must cover a full flush: 480 px * 48 lines * 2 bytes = 46080.
 * 480 / 48 = 10 whole bands; a partial last band produced a dark line. */
#define OUR_MAX_TRANSFER_SZ 49152

static lv_display_t *s_disp;
static lv_obj_t     *s_bar;
static lv_obj_t     *s_mascot;
static lv_obj_t     *s_headline;
static lv_obj_t     *s_detail;
static lv_obj_t     *s_hint;
static bool          s_ready;
static bool          s_bob_on;
static volatile bool s_screen_on = true;

static lv_color_t accent_for(ui_state_t state)
{
    switch (state) {
        case UI_LISTENING:    return lv_color_hex(ACCENT_LISTENING);
        case UI_TRANSCRIBING:
        case UI_THINKING:     return lv_color_hex(ACCENT_WORKING);
        case UI_SPEAKING:     return lv_color_hex(ACCENT_SPEAKING);
        case UI_ERROR:        return lv_color_hex(ACCENT_ERROR);
        default:              return lv_color_hex(ACCENT_READY);
    }
}

static const lv_image_dsc_t *mascot_for(ui_state_t state)
{
    switch (state) {
        case UI_LISTENING:    return &mascot_listening;
        case UI_TRANSCRIBING:
        case UI_THINKING:     return &mascot_thinking;
        case UI_SPEAKING:     return &mascot_speaking;
        case UI_ERROR:        return &mascot_thinking;
        default:              return &mascot_idle;
    }
}

/* Slow float, so the character feels alive. Kept as a pair so screen-off can
 * stop it: an invisible animation would keep invalidating 240x240 pixels. */
static void bob_start(void)
{
    if (!s_mascot || s_bob_on) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_mascot);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&a, 20, 12);
    lv_anim_set_duration(&a, 1600);
    lv_anim_set_playback_duration(&a, 1600);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
    s_bob_on = true;
}

static void bob_stop(void)
{
    if (!s_mascot || !s_bob_on) {
        return;
    }
    lv_anim_del(s_mascot, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_obj_set_y(s_mascot, 20);
    s_bob_on = false;
}

static const char *headline_for(ui_state_t state)
{
    switch (state) {
        case UI_READY:        return "Ready";
        case UI_LISTENING:    return "Listening";
        case UI_TRANSCRIBING: return "Transcribing";
        case UI_THINKING:     return "Thinking";
        case UI_SPEAKING:     return "Speaking";
        case UI_ERROR:        return "Error";
        default:              return "Starting";
    }
}

static const char *hint_for(ui_state_t state)
{
    switch (state) {
        case UI_LISTENING:    return "release to send";
        case UI_ERROR:        return "hold BOOT to retry";
        default:              return "hold BOOT to talk - tap KEY3 for screen";
    }
}

bool ui_start(void)
{
    if (s_ready) {
        return true;
    }

    bsp_display_config_t disp_cfg = { .max_transfer_sz = OUR_MAX_TRANSFER_SZ };
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;

    esp_err_t err = bsp_display_new(&disp_cfg, &panel, &io);
    if (err != ESP_OK || !panel) {
        ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "lv adapter init failed");
        return false;
    }

    esp_lv_adapter_display_config_t d_cfg = {
        .panel    = panel,
        .panel_io = io,
        .profile  = {
            .interface        = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation         = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res          = BSP_LCD_H_RES,
            .ver_res          = BSP_LCD_V_RES,
            .buffer_height    = 48,
            /* Keep the draw buffer in internal RAM: PSRAM plus a large SPI
             * transfer is what caused the DMA underflow. 48 KB fits. */
            .use_psram        = false,
            .enable_ppa_accel = false,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };

    s_disp = esp_lv_adapter_register_display(&d_cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "display register failed");
        return false;
    }

    /* The adapter's own task drives LVGL; without it the lock is never taken
     * and every draw call fails. */
    if (esp_lv_adapter_start() != ESP_OK) {
        ESP_LOGE(TAG, "lv adapter start failed");
        return false;
    }

    /* Touch is optional: the voice loop only needs the screen. */
    bsp_display_brightness_init();
    bsp_display_brightness_set(BRIGHTNESS);

    /* The adapter's lock, not the BSP's. */
    bool locked = false;
    for (int attempt = 0; attempt < 20 && !locked; attempt++) {
        locked = (esp_lv_adapter_lock(500) == ESP_OK);
        if (!locked) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if (!locked) {
        ESP_LOGE(TAG, "could not take the LVGL lock");
        return false;
    }

    lv_obj_t *root = lv_display_get_screen_active(s_disp);
    lv_obj_set_style_bg_color(root, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    s_bar = lv_obj_create(root);
    lv_obj_set_size(s_bar, 480, 8);
    lv_obj_align(s_bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(ACCENT_READY), 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bar, 0, 0);
    lv_obj_set_style_radius(s_bar, 0, 0);
    lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_mascot = lv_image_create(root);
    lv_image_set_src(s_mascot, &mascot_idle);
    lv_obj_align(s_mascot, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_clear_flag(s_mascot, LV_OBJ_FLAG_SCROLLABLE);

    bob_start();

    s_headline = lv_label_create(root);
    lv_obj_set_style_text_color(s_headline, lv_color_hex(TEXT), 0);
    lv_obj_set_style_text_font(s_headline, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_headline, "Starting");
    lv_obj_set_style_text_align(s_headline, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_headline, lv_pct(100));
    lv_obj_align(s_headline, LV_ALIGN_TOP_MID, 0, 266);

    s_detail = lv_label_create(root);
    lv_obj_set_style_text_color(s_detail, lv_color_hex(TEXT), 0);
    lv_obj_set_style_text_font(s_detail, &lv_font_montserrat_28, 0);
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_detail, 450);
    lv_obj_set_height(s_detail, 168);
    lv_obj_align(s_detail, LV_ALIGN_TOP_MID, 0, 296);
    lv_label_set_text(s_detail, "");

    s_hint = lv_label_create(root);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(DIM), 0);
    lv_label_set_text(s_hint, "hold BOOT to talk");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -16);

    esp_lv_adapter_unlock();

    s_ready = true;
    ESP_LOGI(TAG, "display ready");
    return true;
}

bool ui_ready(void)
{
    return s_ready;
}

void ui_show(ui_state_t state, const char *detail)
{
    if (!s_ready) {
        return;
    }
    if (esp_lv_adapter_lock(3000) != ESP_OK) {
        ESP_LOGW(TAG, "ui_show skipped: no lock");
        return;
    }

    lv_image_set_src(s_mascot, mascot_for(state));
    lv_obj_set_style_bg_color(s_bar, accent_for(state), 0);
    lv_obj_set_style_text_color(s_headline, accent_for(state), 0);
    lv_label_set_text(s_headline, headline_for(state));

    if (detail && detail[0]) {
        char buf[181];
        strlcpy(buf, detail, sizeof(buf));
        lv_label_set_text(s_detail, buf);
        lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_detail, "");
        lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text(s_hint, hint_for(state));

    esp_lv_adapter_unlock();
}
void ui_screen_set(bool on)
{
    if (!s_ready || on == s_screen_on) {
        return;
    }

    s_screen_on = on;

    /* Hold the LVGL lock while touching the panel: a brightness command must
     * not overlap a flush on the same QSPI bus. */
    if (esp_lv_adapter_lock(3000) == ESP_OK) {
        if (on) {
            bob_start();
        } else {
            bob_stop();
        }
        bsp_display_brightness_set(on ? BRIGHTNESS : 0);
        esp_lv_adapter_unlock();
    } else {
        bsp_display_brightness_set(on ? BRIGHTNESS : 0);
    }
}

void ui_screen_toggle(void)
{
    ui_screen_set(!s_screen_on);
}

bool ui_screen_is_on(void)
{
    return s_screen_on;
}
