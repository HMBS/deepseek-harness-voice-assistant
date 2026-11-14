#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Runtime configuration, stored in NVS and editable over the serial console.
 *
 * Nothing here is a secret at compile time: keys are written once with
 * `cfg set deepgram <key>` and stay on the device. */

#define CFG_MAX_STR 128

typedef struct {
    char wifi_ssid[CFG_MAX_STR];
    char wifi_pass[CFG_MAX_STR];

    /* Host running stt_relay.py. Must be reachable over the LAN. */
    char relay_url[CFG_MAX_STR];

    /* Deepgram key is held by the relay, not the board; the board only needs
     * the DeepSeek key because it calls the model itself. */
    char deepseek_url[CFG_MAX_STR];
    char deepseek_key[CFG_MAX_STR];

    char deepseek_model[CFG_MAX_STR];
    char system_prompt[CFG_MAX_STR * 2];

    int  record_ms;      /* max recording length */
    int  volume;         /* speaker volume 0..100 */
    float play_gain;     /* software gain applied to playback (1.0 = none) */
} voice_cfg_t;

/* Load from NVS, falling back to compile-time defaults. */
void cfg_load(voice_cfg_t *cfg);

/* Persist the whole struct. */
bool cfg_save(const voice_cfg_t *cfg);

/* Set one string field by name ("wifi_ssid", "wifi_pass", ...). */
bool cfg_set_str(voice_cfg_t *cfg, const char *key, const char *value);

/* Set one int field by name ("record_ms", "volume"). */
bool cfg_set_int(voice_cfg_t *cfg, const char *key, int value);

/* Masked one-line summary, safe to print. */
void cfg_print(const voice_cfg_t *cfg);