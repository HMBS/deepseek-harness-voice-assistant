#pragma once

#include <stdbool.h>

/* The screen shows one of a handful of states. Keeping this to a small enum
 * means the UI code stays readable and the states map directly to the voice
 * loop's progress. */

typedef enum {
    UI_BOOT = 0,
    UI_READY,
    UI_LISTENING,
    UI_TRANSCRIBING,
    UI_THINKING,
    UI_SPEAKING,
    UI_ERROR,
} ui_state_t;

/* Bring up the AMOLED and show the first frame. */
bool ui_start(void);

/* Show a state. `detail` may be NULL; when set it is drawn under the headline
 * and wrapped to the screen width. */
void ui_show(ui_state_t state, const char *detail);

/* True once the display is up. */
bool ui_ready(void);

/* Screen on/off. Off sets the AMOLED brightness to zero (black pixels) and
 * stops the mascot animation; the voice loop keeps running. */
void ui_screen_set(bool on);
void ui_screen_toggle(void);
bool ui_screen_is_on(void);