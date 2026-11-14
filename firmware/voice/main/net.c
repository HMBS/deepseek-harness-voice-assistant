#include "net.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "net";

/* One request, body read back by hand.
 *
 * esp_http_client_perform() consumes the response body and only hands it to
 * an event handler, so calling it without one leaves nothing to read. Drive
 * the client manually instead: open, write, fetch headers, then read.
 */
static char *http_do(const char *url, const char *content_type,
                     const char *body, size_t body_len, int timeout_ms,
                     size_t cap, bool tls,
                     int *status_out, size_t *len_out)
{
    *status_out = 0;
    *len_out = 0;

    char *resp = calloc(1, cap + 1);
    if (!resp) {
        return NULL;
    }

    esp_http_client_config_t cfg = { 0 };
    cfg.url = url;
    cfg.timeout_ms = timeout_ms;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 8192;
    if (tls) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    cfg.disable_auto_redirect = false;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        free(resp);
        return NULL;
    }

    esp_http_client_set_method(cli, body ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    if (content_type) {
        esp_http_client_set_header(cli, "Content-Type", content_type);
    }

    if (esp_http_client_open(cli, (int)body_len) == ESP_OK) {
        if (body_len > 0) {
            int w = esp_http_client_write(cli, body, (int)body_len);
            if (w != (int)body_len) {
                ESP_LOGE(TAG, "http write %d/%u", w, (unsigned)body_len);
            }
        }

        if (esp_http_client_fetch_headers(cli) >= 0) {
            *status_out = esp_http_client_get_status_code(cli);
            size_t len = 0;
            while (len < cap) {
                int n = esp_http_client_read(cli, resp + len, (int)(cap - len));
                if (n <= 0) {
                    break;
                }
                len += (size_t)n;
            }
            resp[len] = '\0';
            *len_out = len;
        }
    } else {
        ESP_LOGE(TAG, "http open failed: %s", url);
    }

    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return resp;
}

/* ── DeepSeek ─────────────────────────────────────────────────────────── */

bool net_ask(const char *url, const char *api_key, const char *model,
             const char *system_prompt,
             const char *user_text, char *out, size_t out_size, int timeout_ms)
{
    if (!url || !url[0]) {
        ESP_LOGE(TAG, "no DeepSeek endpoint");
        return false;
    }
    if (!api_key || !api_key[0]) {
        ESP_LOGE(TAG, "no DeepSeek key");
        return false;
    }
    if (!out || out_size == 0) {
        return false;
    }
    out[0] = '\0';

    /* Build the request body. */
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return false;
    }
    cJSON_AddStringToObject(root, "model", model && model[0] ? model : "deepseek-chat");
    cJSON_AddNumberToObject(root, "max_tokens", 120);
    cJSON_AddNumberToObject(root, "temperature", 0.7);

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    if (system_prompt && system_prompt[0]) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", "system");
        cJSON_AddStringToObject(m, "content", system_prompt);
        cJSON_AddItemToArray(messages, m);
    }
    cJSON *u = cJSON_CreateObject();
    cJSON_AddStringToObject(u, "role", "user");
    cJSON_AddStringToObject(u, "content", user_text);
    cJSON_AddItemToArray(messages, u);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return false;
    }

    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", api_key);

    /* DeepSeek needs an Authorization header, so its request is built here
     * rather than through http_do(). */
    int status = 0;
    size_t len = 0;

    char *resp = NULL;
    {
        esp_http_client_config_t cfg = { 0 };
        cfg.url = url;
        cfg.timeout_ms = timeout_ms;
        cfg.buffer_size = 4096;
        cfg.buffer_size_tx = 4096;
        /* A LAN harness can be plain http; only attach the cert bundle for
         * https endpoints. */
        if (strncmp(url, "https://", 8) == 0) {
            cfg.crt_bundle_attach = esp_crt_bundle_attach;
        }

        size_t cap = 32768;
        resp = calloc(1, cap + 1);
        esp_http_client_handle_t cli = esp_http_client_init(&cfg);
        if (!resp || !cli) {
            free(resp);
            if (cli) {
                esp_http_client_cleanup(cli);
            }
            free(body);
            return false;
        }

        esp_http_client_set_method(cli, HTTP_METHOD_POST);
        esp_http_client_set_header(cli, "Content-Type", "application/json");
        esp_http_client_set_header(cli, "Authorization", auth);

        if (esp_http_client_open(cli, (int)strlen(body)) == ESP_OK) {
            int w = esp_http_client_write(cli, body, (int)strlen(body));
            if (w != (int)strlen(body)) {
                ESP_LOGE(TAG, "deepseek write %d/%u", w, (unsigned)strlen(body));
            }
            if (esp_http_client_fetch_headers(cli) >= 0) {
                status = esp_http_client_get_status_code(cli);
                while (len < cap) {
                    int n = esp_http_client_read(cli, resp + len, (int)(cap - len));
                    if (n <= 0) {
                        break;
                    }
                    len += (size_t)n;
                }
                resp[len] = '\0';
            }
        }

        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
    }

    free(body);

    if (status != 200) {
        ESP_LOGE(TAG, "deepseek http %d", status);
        free(resp);
        return false;
    }

    /* Pull choices[0].message.content. */
    bool ok = false;
    cJSON *json = cJSON_Parse(resp);
    if (json) {
        cJSON *choices = cJSON_GetObjectItem(json, "choices");
        if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
            cJSON *first = cJSON_GetArrayItem(choices, 0);
            cJSON *msg = cJSON_GetObjectItem(first, "message");
            cJSON *content = msg ? cJSON_GetObjectItem(msg, "content") : NULL;
            if (cJSON_IsString(content)) {
                strlcpy(out, content->valuestring, out_size);
                ok = true;
            }
        }
        cJSON_Delete(json);
    } else {
        ESP_LOGE(TAG, "deepseek: bad JSON (%u bytes)", (unsigned)len);
    }

    free(resp);
    return ok && out[0];
}

/* ── Relay: transcribe ────────────────────────────────────────────────── */

/* Base64-encode `in` into `out` (NUL-terminated). `out` must hold at least
 * ((in_len + 2) / 3) * 4 + 1 bytes. */
static void b64_encode(const uint8_t *in, size_t in_len, char *out)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, o = 0;
    while (i + 2 < in_len) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = tbl[(v >> 6) & 63];
        out[o++] = tbl[v & 63];
        i += 3;
    }
    if (i < in_len) {
        uint32_t v = (uint32_t)in[i] << 16;
        bool two = (i + 1) < in_len;
        if (two) {
            v |= (uint32_t)in[i + 1] << 8;
        }
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = two ? tbl[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
}

/* Extract a top-level string field from a flat JSON object. */
static bool json_str_field(const char *json_text, const char *field,
                           char *out, size_t out_size)
{
    cJSON *root = cJSON_Parse(json_text);
    if (!root) {
        return false;
    }
    cJSON *item = cJSON_GetObjectItem(root, field);
    bool ok = cJSON_IsString(item) && item->valuestring;
    if (ok) {
        strlcpy(out, item->valuestring, out_size);
    }
    cJSON_Delete(root);
    return ok;
}

bool net_time(const char *relay_url, char *out, size_t out_size, int timeout_ms)
{
    if (!relay_url || !relay_url[0] || !out || !out_size) {
        return false;
    }
    out[0] = '\0';

    char url[192];
    snprintf(url, sizeof(url), "%stime", relay_url);

    int status = 0;
    size_t len = 0;
    char *resp = http_do(url, NULL, NULL, 0, timeout_ms, 2048, false,
                         &status, &len);
    if (!resp) {
        return false;
    }

    bool ok = false;
    if (status == 200 && len > 0) {
        ok = json_str_field(resp, "text", out, out_size);
    }
    free(resp);
    return ok;
}

bool net_transcribe(const char *relay_url, const uint8_t *wav, size_t wav_bytes,
                    char *text, size_t text_size,
                    char *now_text, size_t now_size, int timeout_ms)
{
    if (!relay_url || !relay_url[0] || !wav || wav_bytes == 0 || !text || !text_size) {
        return false;
    }
    text[0] = '\0';
    if (now_text && now_size) {
        now_text[0] = '\0';
    }

    size_t b64_len = ((wav_bytes + 2) / 3) * 4 + 1;
    char *b64 = malloc(b64_len);
    if (!b64) {
        return false;
    }
    b64_encode(wav, wav_bytes, b64);

    size_t body_len = b64_len + 128;
    char *body = malloc(body_len);
    if (!body) {
        free(b64);
        return false;
    }
    snprintf(body, body_len,
             "{\"audio_b64\":\"%s\",\"sample_rate\":16000}", b64);
    free(b64);

    /* The base URL points at the relay root; append the endpoint. */
    char url[192];
    snprintf(url, sizeof(url), "%stranscribe", relay_url);

    int status = 0;
    size_t len = 0;
    char *resp = http_do(url, "application/json", body, strlen(body),
                         timeout_ms, 8192, false, &status, &len);
    free(body);
    if (!resp) {
        return false;
    }

    bool ok = false;
    if (status == 200) {
        ok = json_str_field(resp, "transcript", text, text_size);
        if (!ok) {
            ESP_LOGW(TAG, "no transcript in relay reply");
        }
        if (now_text && now_size) {
            json_str_field(resp, "now", now_text, now_size);
        }
    } else {
        ESP_LOGE(TAG, "relay transcribe http %d", status);
    }

    free(resp);
    return ok;
}

/* ── Relay: text to speech ────────────────────────────────────────────── */

bool net_speak(const char *relay_url, const char *text_in, uint8_t **out,
               size_t *out_len, int timeout_ms)
{
    if (!relay_url || !relay_url[0] || !text_in || !out || !out_len) {
        return false;
    }
    *out = NULL;
    *out_len = 0;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return false;
    }
    cJSON_AddStringToObject(root, "text", text_in);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return false;
    }

    char url[192];
    snprintf(url, sizeof(url), "%sspeak", relay_url);

    int status = 0;
    size_t len = 0;
    /* 512 KB covers a short spoken reply, base64-encoded. */
    char *resp = http_do(url, "application/json", body, strlen(body),
                         timeout_ms, 1 << 19, false, &status, &len);
    free(body);

    if (!resp) {
        return false;
    }
    if (status != 200 || len == 0) {
        ESP_LOGE(TAG, "relay tts http %d (%u bytes)", status, (unsigned)len);
        free(resp);
        return false;
    }

    /* The relay replies with base64, to keep the JSON ASCII-only. */
    cJSON *json = cJSON_Parse(resp);
    if (!json) {
        free(resp);
        return false;
    }
    cJSON *field = cJSON_GetObjectItem(json, "pcm_b64");
    if (!cJSON_IsString(field) || !field->valuestring) {
        cJSON_Delete(json);
        free(resp);
        return false;
    }

    const char *b64 = field->valuestring;
    size_t b64_len = strlen(b64);
    uint8_t *pcm = malloc(b64_len / 4 * 3 + 4);
    if (!pcm) {
        cJSON_Delete(json);
        free(resp);
        return false;
    }

    /* Decode base64 by hand: the device has no base64 helper. */
    static const int8_t tbl[256] = { ['A'] = 0, ['B'] = 1, ['C'] = 2, ['D'] = 3,
        ['E'] = 4, ['F'] = 5, ['G'] = 6, ['H'] = 7, ['I'] = 8, ['J'] = 9,
        ['K'] = 10, ['L'] = 11, ['M'] = 12, ['N'] = 13, ['O'] = 14, ['P'] = 15,
        ['Q'] = 16, ['R'] = 17, ['S'] = 18, ['T'] = 19, ['U'] = 20, ['V'] = 21,
        ['W'] = 22, ['X'] = 23, ['Y'] = 24, ['Z'] = 25,
        ['a'] = 26, ['b'] = 27, ['c'] = 28, ['d'] = 29, ['e'] = 30, ['f'] = 31,
        ['g'] = 32, ['h'] = 33, ['i'] = 34, ['j'] = 35, ['k'] = 36, ['l'] = 37,
        ['m'] = 38, ['n'] = 39, ['o'] = 40, ['p'] = 41, ['q'] = 42, ['r'] = 43,
        ['s'] = 44, ['t'] = 45, ['u'] = 46, ['v'] = 47, ['w'] = 48, ['x'] = 49,
        ['y'] = 50, ['z'] = 51,
        ['0'] = 52, ['1'] = 53, ['2'] = 54, ['3'] = 55, ['4'] = 56, ['5'] = 57,
        ['6'] = 58, ['7'] = 59, ['8'] = 60, ['9'] = 61, ['+'] = 62, ['/'] = 63 };

    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < b64_len; i++) {
        char c = b64[i];
        if (c == '=') {
            break;
        }
        int8_t v = tbl[(unsigned char)c];
        if (v < 0) {
            continue;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            pcm[o++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }

    cJSON_Delete(json);
    free(resp);

    if (o == 0) {
        free(pcm);
        return false;
    }

    *out = pcm;
    *out_len = o;
    return true;
}

bool net_speak_stream(const char *relay_url, const char *text_in,
                      net_pcm_sink_t sink, void *ctx, int timeout_ms)
{
    if (!relay_url || !relay_url[0] || !text_in || !sink) {
        return false;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return false;
    }
    cJSON_AddStringToObject(root, "text", text_in);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return false;
    }

    char url[192];
    snprintf(url, sizeof(url), "%sspeak_stream", relay_url);

    esp_http_client_config_t cfg = { 0 };
    cfg.url = url;
    cfg.timeout_ms = timeout_ms;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 4096;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        free(body);
        return false;
    }

    esp_http_client_set_method(cli, HTTP_METHOD_POST);
    esp_http_client_set_header(cli, "Content-Type", "application/json");

    bool ok = false;
    int body_len = (int)strlen(body);

    if (esp_http_client_open(cli, body_len) == ESP_OK) {
        int w = esp_http_client_write(cli, body, body_len);
        if (w == body_len && esp_http_client_fetch_headers(cli) >= 0 &&
            esp_http_client_get_status_code(cli) == 200) {
            uint8_t buf[4096];
            int n;
            ok = true;
            while ((n = esp_http_client_read(cli, (char *)buf, sizeof(buf))) > 0) {
                if (!sink(buf, (size_t)n, ctx)) {
                    ok = false;
                    break;
                }
            }
            if (n < 0) {
                ok = false;
            }
        } else {
            ESP_LOGE(TAG, "speak_stream http %d", esp_http_client_get_status_code(cli));
        }
    }

    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    free(body);
    return ok;
}
