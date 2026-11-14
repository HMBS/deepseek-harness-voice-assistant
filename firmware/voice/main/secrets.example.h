#pragma once

/* Copy this file to secrets.h and fill in your values.
 *
 *     cp secrets.example.h secrets.h
 *
 * secrets.h is gitignored. Values left empty fall back to whatever is already
 * in NVS, so the console `cfg` commands can still be used if you get them
 * working on your board.
 *
 * After editing: ./idf.sh build && ./idf.sh -p /dev/ttyACM0 flash monitor
 */

/* Wi-Fi: must be a 2.4 GHz network. */
#define CFG_WIFI_SSID     "your-ssid"
#define CFG_WIFI_PASSWORD "your-password"

/* Host running relay/stt_relay.py, reachable from the board's Wi-Fi network.
 * Use the computer's LAN IP, not 127.0.0.1. */
#define CFG_RELAY_URL     "http://192.168.1.10:8799/"

/* DeepSeek API key, and the model to use. */
#define CFG_DEEPSEEK_KEY  "sk-your-key"
#define CFG_DEEPSEEK_MODEL "deepseek-chat"

/* Chat endpoint. Either the public API or the relay's OpenAI-compatible shim
 * that runs a local dsh harness. */
#define CFG_DEEPSEEK_URL  "https://api.deepseek.com/chat/completions"
/* #define CFG_DEEPSEEK_URL "http://192.168.1.10:8799/v1/chat/completions" */

/* Optional overrides; the defaults in config.c are sensible. */
#define CFG_RECORD_MS     15000
#define CFG_VOLUME        80
#define CFG_PLAY_GAIN     3.0f  /* software gain on TTS playback */
