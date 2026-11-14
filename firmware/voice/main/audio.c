#include "audio.h"

#include <string.h>
#include <math.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_codec_dev.h"

static const char *TAG = "audio";

#define SAMPLE_RATE   16000
#define CHANNELS      1
#define MIC_CHANNELS  2   /* ES7210 has two mics wired; capture both, use ch0 */
#define BITS          16
#define BYTES_PER_FRAME (CHANNELS * (BITS / 8))

static esp_codec_dev_handle_t s_spk  = NULL;
static esp_codec_dev_handle_t s_mic  = NULL;
static bool s_started = false;
static int  s_volume  = 80;
static float s_gain   = 1.0f;
static bool  s_streaming;
static uint8_t s_odd_byte;
static bool  s_odd_valid;

/* Network audio arrives in bursts; without a cushion the I2S DMA runs dry
 * between them and playback stutters. Buffer 800 ms before starting. */
#define PREBUFFER_MS 800
#define PREBUFFER_BYTES ((SAMPLE_RATE * PREBUFFER_MS / 1000) * 2)
static uint8_t *s_pre;
static size_t   s_pre_len;
static bool     s_playing;

bool audio_start(const audio_cfg_t *cfg)
{
    if (s_started) {
        return true;
    }
    if (!cfg) {
        return false;
    }

    s_volume = cfg->volume;
    s_gain   = cfg->play_gain > 0.1f ? cfg->play_gain : 1.0f;

    /* I2S: the BSP's default is mono, 16-bit, 22.05 kHz; override the clock to
     * 16 kHz, which is what Deepgram wants and what the ES7210 is happy with. */
    i2s_std_config_t i2s_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT,
                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws   = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din  = BSP_I2S_DSIN,
            .invert_flags = { false, false, false },
        },
    };

    /* The BSP's codecs sit on I2C, but bsp_i2c_init() is normally called by
     * bsp_display_start(). We bring the panel up ourselves, so do it here:
     * without the bus, bsp_audio_codec_speaker_init() asserts on a NULL
     * i2c_ctrl_if and the board reboots. Safe to call more than once. */
    esp_err_t i2c_err = bsp_i2c_init();
    if (i2c_err != ESP_OK) {
        ESP_LOGE(TAG, "i2c init failed: %s", esp_err_to_name(i2c_err));
        return false;
    }

    esp_err_t err = bsp_audio_init(&i2s_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init failed: %s", esp_err_to_name(err));
        return false;
    }

    s_spk = bsp_audio_codec_speaker_init();
    if (!s_spk) {
        ESP_LOGE(TAG, "speaker init failed");
        return false;
    }

    s_mic = bsp_audio_codec_microphone_init();
    if (!s_mic) {
        ESP_LOGE(TAG, "microphone init failed");
        return false;
    }

    /* The BSP's codec config drives the PA pin (GPIO46) itself. */

    ESP_LOGI(TAG, "ready: %d Hz mono %d-bit, volume %d, gain %.2fx",
             SAMPLE_RATE, BITS, s_volume, (double)s_gain);

    s_started = true;
    return true;
}

void audio_stop(void)
{
    if (!s_started) {
        return;
    }
    /* The BSP has no deinit; drop our handles and let the codecs be. */
    s_spk = NULL;
    s_mic = NULL;
    s_started = false;
}

bool audio_ready(void)
{
    return s_started && s_spk && s_mic;
}

/* Write a little-endian 32-bit value. */
static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

/* Build a 44-byte canonical WAV header for mono 16-bit PCM. */
static void wav_header(uint8_t *h, uint32_t data_bytes)
{
    memcpy(h + 0,  "RIFF", 4);
    put_u32le(h + 4,  36 + data_bytes);
    memcpy(h + 8,  "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put_u32le(h + 16, 16);                              /* fmt chunk size */
    put_u16le(h + 20, 1);                               /* PCM */
    put_u16le(h + 22, CHANNELS);
    put_u32le(h + 24, SAMPLE_RATE);
    put_u32le(h + 28, SAMPLE_RATE * BYTES_PER_FRAME);   /* byte rate */
    put_u16le(h + 32, BYTES_PER_FRAME);                 /* block align */
    put_u16le(h + 34, BITS);
    memcpy(h + 36, "data", 4);
    put_u32le(h + 40, data_bytes);
}

size_t audio_record_held(uint8_t *out, size_t out_cap, int max_ms,
                         int release_tail_ms, audio_held_fn held)
{
    if (!s_started || !s_mic || !out || out_cap < 64) {
        ESP_LOGE(TAG, "record: not ready");
        return 0;
    }

    if (max_ms <= 0 || max_ms > 60000) {
        max_ms = 15000;
    }
    if (release_tail_ms < 0 || release_tail_ms > 2000) {
        release_tail_ms = 400;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = SAMPLE_RATE,
        .channel         = CHANNELS,
        .channel_mask    = 0,          /* 0 = all channels */
        .bits_per_sample = BITS,
        .mclk_multiple   = 0,          /* 0 = sample_rate * 256 */
    };

    esp_err_t err = esp_codec_dev_open(s_mic, &fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mic open failed: %s", esp_err_to_name(err));
        return 0;
    }

    /* Let the I2S DMA fill: reads immediately after open return zeros. */
    vTaskDelay(pdMS_TO_TICKS(120));

    const int    chunk_ms    = 40;
    const size_t chunk_bytes = (size_t)(SAMPLE_RATE * chunk_ms / 1000) * BYTES_PER_FRAME;
    const size_t max_bytes   = (size_t)(SAMPLE_RATE * max_ms / 1000) * BYTES_PER_FRAME;
    const size_t tail_bytes  =
        (size_t)(SAMPLE_RATE * release_tail_ms / 1000) * BYTES_PER_FRAME;

    uint8_t *pcm     = out + 44;
    size_t   pcm_cap = out_cap - 44;
    if (pcm_cap > max_bytes) {
        pcm_cap = max_bytes;
    }

    size_t total = 0;
    size_t after_release = 0;

    ESP_LOGI(TAG, "recording (max %d ms) ...", max_ms);

    while (total < pcm_cap) {
        size_t want = chunk_bytes;
        if (total + want > pcm_cap) {
            want = pcm_cap - total;
        }

        /* Returns 0 (ESP_CODEC_DEV_OK) on success and fills `want` bytes. */
        err = esp_codec_dev_read(s_mic, pcm + total, (int)want);
        if (err != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "mic read failed after %u bytes: %s",
                     (unsigned)total, esp_err_to_name(err));
            break;
        }
        total += want;

        /* The button marks the end of speech: stop a moment after release. */
        if (held && !held()) {
            after_release += want;
            if (after_release >= tail_bytes) {
                break;
            }
        } else {
            after_release = 0;
        }
    }

    esp_codec_dev_close(s_mic);

    if (total < (size_t)(SAMPLE_RATE / 10) * BYTES_PER_FRAME) {
        ESP_LOGW(TAG, "recording too short (%u bytes)", (unsigned)total);
        return 0;
    }

    wav_header(out, (uint32_t)total);
    ESP_LOGI(TAG, "captured %u bytes of PCM", (unsigned)total);
    return total + 44;
}

/* Skip a WAV header if one is present; return a pointer to the PCM payload. */
static const uint8_t *skip_wav(const uint8_t *data, size_t bytes, size_t *out_len)
{
    if (bytes >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WAVE", 4)) {
        /* Walk chunks to find "data", rather than assuming a 44-byte header. */
        size_t pos = 12;
        while (pos + 8 <= bytes) {
            const uint8_t *id = data + pos;
            uint32_t sz = (uint32_t)data[pos + 4] |
                          ((uint32_t)data[pos + 5] << 8) |
                          ((uint32_t)data[pos + 6] << 16) |
                          ((uint32_t)data[pos + 7] << 24);
            if (!memcmp(id, "data", 4)) {
                size_t payload = pos + 8;
                if (payload > bytes) {
                    return NULL;
                }
                size_t n = bytes - payload;
                if (sz && sz < n) {
                    n = sz;
                }
                *out_len = n;
                return data + payload;
            }
            pos += 8 + sz + (sz & 1);   /* chunks are word-aligned */
        }
        return NULL;
    }
    *out_len = bytes;
    return data;
}

/* Write an even number of bytes, applying gain with clipping. */
static bool write_scaled(const uint8_t *src, size_t bytes)
{
    int16_t chunk[2048];   /* 128 ms per write */

    while (bytes > 0) {
        size_t take = bytes < sizeof(chunk) ? bytes : sizeof(chunk);
        memcpy(chunk, src, take);

        if (s_gain != 1.0f) {
            size_t n = take / 2;
            for (size_t i = 0; i < n; i++) {
                int32_t v = (int32_t)((float)chunk[i] * s_gain);

                /* Soft knee: quiet parts get the full gain, peaks are
                 * compressed instead of hard-clipped, which sounds louder
                 * without crackling. */
                if (v > 26000) {
                    v = 26000 + (v - 26000) / 4;
                } else if (v < -26000) {
                    v = -26000 + (v + 26000) / 4;
                }

                if (v > 32767) {
                    v = 32767;
                } else if (v < -32768) {
                    v = -32768;
                }
                chunk[i] = (int16_t)v;
            }
        }

        /* Like read(), write() returns a status code, not a byte count. */
        if (esp_codec_dev_write(s_spk, chunk, (int)take) != ESP_CODEC_DEV_OK) {
            return false;
        }
        src += take;
        bytes -= take;
    }
    return true;
}

bool audio_stream_begin(void)
{
    if (!s_started || !s_spk) {
        return false;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = SAMPLE_RATE,
        .channel         = CHANNELS,
        .channel_mask    = 0,
        .bits_per_sample = BITS,
        .mclk_multiple   = 0,
    };

    esp_err_t err = esp_codec_dev_open(s_spk, &fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "speaker open failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_codec_dev_set_out_vol(s_spk, s_volume);
    s_streaming = true;
    s_odd_valid = false;
    s_pre_len = 0;
    s_playing = false;

    if (!s_pre) {
        /* Keep this out of internal RAM; PSRAM has plenty. */
        s_pre = heap_caps_malloc(PREBUFFER_BYTES, MALLOC_CAP_SPIRAM);
    }
    return true;
}

bool audio_stream_write(const uint8_t *pcm, size_t bytes)
{
    if (!s_streaming) {
        return false;
    }
    if (!pcm || bytes == 0) {
        return true;
    }

    const uint8_t *p = pcm;
    size_t n = bytes;

    /* Hold playback until the cushion is full, so bursts of network audio do
     * not turn into gaps. If PSRAM allocation failed, play straight through. */
    if (!s_playing && s_pre) {
        size_t room = PREBUFFER_BYTES - s_pre_len;
        size_t take = n < room ? n : room;
        memcpy(s_pre + s_pre_len, p, take);
        s_pre_len += take;
        p += take;
        n -= take;

        if (s_pre_len < PREBUFFER_BYTES) {
            return true;
        }

        size_t even = s_pre_len & ~(size_t)1;
        if (!write_scaled(s_pre, even)) {
            return false;
        }
        if (even != s_pre_len) {
            s_odd_byte = s_pre[s_pre_len - 1];
            s_odd_valid = true;
        }
        s_pre_len = 0;
        s_playing = true;
    }

    /* Network chunks are not guaranteed to contain whole samples. Stitch the
     * leftover byte onto the next chunk. */
    if (s_odd_valid) {
        uint8_t pair[2] = { s_odd_byte, p[0] };
        if (!write_scaled(pair, sizeof(pair))) {
            return false;
        }
        s_odd_valid = false;
        p++;
        n--;
    }

    if (n & 1) {
        s_odd_byte = p[n - 1];
        s_odd_valid = true;
        n--;
    }

    return write_scaled(p, n);
}

void audio_stream_end(void)
{
    if (!s_streaming) {
        return;
    }

    /* A clip shorter than the prebuffer never started playing: flush it now. */
    if (!s_playing && s_pre && s_pre_len > 0) {
        size_t even = s_pre_len & ~(size_t)1;
        write_scaled(s_pre, even);
    }

    s_streaming = false;
    s_odd_valid = false;
    s_pre_len = 0;
    s_playing = false;
    esp_codec_dev_close(s_spk);
}

bool audio_play_pcm(const uint8_t *pcm, size_t bytes)
{
    if (!s_started || !s_spk || !pcm || bytes == 0) {
        return false;
    }

    size_t len = bytes;
    const uint8_t *data = skip_wav(pcm, bytes, &len);
    if (!data || len == 0) {
        ESP_LOGW(TAG, "no PCM payload to play");
        return false;
    }
    len &= ~(size_t)1;                  /* whole samples only */

    if (!audio_stream_begin()) {
        return false;
    }
    bool ok = audio_stream_write(data, len);
    audio_stream_end();

    if (!ok) {
        ESP_LOGE(TAG, "playback failed");
        return false;
    }

    ESP_LOGI(TAG, "played %u bytes at gain %.2fx", (unsigned)len, (double)s_gain);
    return true;
}

void audio_beep(void)
{
    /* A short 880 Hz tone, generated rather than shipped as a file. */
    const int ms = 90;
    size_t n = (size_t)(SAMPLE_RATE * ms / 1000);
    static int16_t tone[SAMPLE_RATE / 10];
    if (n > sizeof(tone) / sizeof(tone[0])) {
        n = sizeof(tone) / sizeof(tone[0]);
    }
    for (size_t i = 0; i < n; i++) {
        float t = (float)i / (float)SAMPLE_RATE;
        /* Fade the last fifth to avoid a click. */
        float env = 1.0f;
        size_t fade = n / 5;
        if (fade && i > n - fade) {
            env = (float)(n - i) / (float)fade;
        }
        tone[i] = (int16_t)(sinf(2.0f * 3.14159265f * 880.0f * t) * 12000.0f * env);
    }
    audio_play_pcm((const uint8_t *)tone, n * sizeof(int16_t));
}