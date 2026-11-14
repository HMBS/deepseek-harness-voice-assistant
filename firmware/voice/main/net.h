#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Network calls: DeepSeek for replies, and the host relay for speech.
 *
 * The board uploads recorded audio to a small relay on the host
 * (stt_relay.py) rather than to Deepgram directly, because the relay holds the
 * Deepgram key and can amplify the quiet microphone signal before sending it.
 * The reply path goes straight to api.deepseek.com. */

/* Ask DeepSeek with one user message. Writes a NUL-terminated reply into
 * `out`. Returns true on success. */
bool net_ask(const char *url, const char *api_key, const char *model,
             const char *system_prompt,
             const char *user_text, char *out, size_t out_size, int timeout_ms);

/* Fetch the host's local time from the relay. On success `out` holds a
 * human-readable date and time, ready to put in a prompt. */
bool net_time(const char *relay_url, char *out, size_t out_size, int timeout_ms);

/* Send a WAV to the relay for transcription. On success `text` holds the
 * transcript. Returns true when speech was recognised. */
bool net_transcribe(const char *relay_url, const uint8_t *wav, size_t wav_bytes,
                    char *text, size_t text_size,
                    char *now_text, size_t now_size, int timeout_ms);

/* Ask the relay to synthesise `text`; `out` receives malloc'd PCM (caller
 * frees). Returns true on success. */
bool net_speak(const char *relay_url, const char *text, uint8_t **out,
               size_t *out_len, int timeout_ms);

/* Called for each PCM chunk as it arrives. Return false to abort. */
typedef bool (*net_pcm_sink_t)(const uint8_t *pcm, size_t bytes, void *ctx);

/* Stream synthesised PCM to `sink`, so playback can start on the first chunk
 * instead of after the whole clip has downloaded. */
bool net_speak_stream(const char *relay_url, const char *text,
                      net_pcm_sink_t sink, void *ctx, int timeout_ms);