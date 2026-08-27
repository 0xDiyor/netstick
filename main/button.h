#pragma once
#include <stdint.h>
#include <stdbool.h>

// The one side button, turned into two gestures the whole UI is built on:
//   TAP  - press and release quickly. Always means "next / advance".
//   HOLD - keep it pressed for BTN_HOLD_MS. Fires while still held, so the
//          footer can draw a filling bar and the user knows exactly when it
//          will trigger. Always means "select / open menu".
#define BTN_HOLD_MS   550

typedef enum { BTN_NONE = 0, BTN_TAP, BTN_HOLD } btn_event_t;

void        button_init(void);
btn_event_t button_poll(void);         // non-blocking, returns queued event or BTN_NONE
uint32_t    button_held_ms(void);      // 0 when not pressed, for the hold-progress bar
void        button_inject(btn_event_t e);   // from the serial console (debug driving)
