#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Audio capture and playback through the board's ES7210 microphones and
 * ES8311 speaker. Everything runs at 16 kHz mono signed 16-bit, which is what
 * Deepgram expects and what the codec's I2S is configured for. */

typedef struct {
    int   sample_rate;   /* 16000 */
    int   channels;      /* 1 */
    int   bits;          /* 16 */
    int   record_ms;     /* cap on recording length */
    int   volume;        /* 0..100, applied to the speaker */
    float play_gain;     /* software gain on playback, 1.0 = none */
} audio_cfg_t;

/* Bring up I2S and both codecs. Call once, after Wi-Fi is up. */
bool audio_start(const audio_cfg_t *cfg);

/* Release the codecs. Safe to call when not started. */
void audio_stop(void);

bool audio_ready(void);

/* Record from the microphones into `out`.
 *
 * `held` is polled between chunks: while it returns true recording continues;
 * once it returns false for `release_tail_ms`, recording stops. This makes the
 * button itself the end-of-speech marker, which is far more reliable than
 * guessing from signal level. `max_ms` is a hard cap.
 *
 * Writes a 44-byte WAV header followed by the samples, so the buffer can be
 * uploaded as audio/wav without further work. Returns the number of bytes
 * written, or 0 on failure.
 */
typedef bool (*audio_held_fn)(void);

size_t audio_record_held(uint8_t *out, size_t out_cap, int max_ms,
                         int release_tail_ms, audio_held_fn held);

/* Streaming playback: begin opens the speaker, write applies the configured
 * gain to raw 16 kHz mono s16le, end closes it. Odd-sized chunks are handled. */
bool audio_stream_begin(void);
bool audio_stream_write(const uint8_t *pcm, size_t bytes);
void audio_stream_end(void);

/* Play a WAV file's PCM payload from memory. The buffer may be a bare PCM
 * payload (no header) or a full WAV; the header is skipped when present.
 * Blocks until playback finishes. */
bool audio_play_pcm(const uint8_t *pcm, size_t bytes);

/* Play a short beep, to confirm a press was noticed. */
void audio_beep(void);