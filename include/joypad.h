/**
 * @file joypad.h
 * @brief Game Boy joypad input.
 *
 * The joypad is exposed to the CPU through one register (JOYP at
 * 0xFF00). The host (main.c, an SDL window, a test) tells the joypad
 * which buttons are currently held; the joypad reports button state
 * to the CPU through JOYP and fires a joypad interrupt on the
 * transitions the CPU asked to see.
 *
 * JOYP register layout:
 *   bit 7-6: always 1 (unused)
 *   bit 5:   0 to select action buttons (A/B/Select/Start)
 *   bit 4:   0 to select direction buttons (Right/Left/Up/Down)
 *   bit 3-0: read-only state of the selected group
 *            (0 = pressed, 1 = released; counterintuitive but real)
 *
 * The CPU writes bits 5/4 to choose which group it's polling, then
 * reads back to see button states. If the CPU clears both bits 5
 * and 4, both groups are reported simultaneously (an OR over the two
 * groups -- buttons in either group reading low).
 *
 * The joypad interrupt fires on any high-to-low transition of
 * bits 3-0 of the JOYP read value. In effect: any newly pressed
 * button in the currently selected group(s) raises the interrupt.
 */
#ifndef JOYPAD_H
#define JOYPAD_H

#include <stdint.h>

typedef struct Bus Bus;
typedef struct Joypad Joypad;

/**
 * @brief Button bit positions in the bitmask used by joypad_set_buttons.
 *
 * The integer values do NOT match JOYP's bit layout; this is a host-
 * facing API and uses a single byte with all 8 buttons distinct.
 */
typedef enum {
  JOYPAD_RIGHT  = 0,
  JOYPAD_LEFT   = 1,
  JOYPAD_UP     = 2,
  JOYPAD_DOWN   = 3,
  JOYPAD_A      = 4,
  JOYPAD_B      = 5,
  JOYPAD_SELECT = 6,
  JOYPAD_START  = 7,
} JoypadButton;

Joypad* joypad_create(Bus* bus);
void    joypad_destroy(Joypad* j);
void    joypad_reset(Joypad* j);

/**
 * @brief Set the current state of all 8 buttons in one call.
 *
 * @param buttons Bitmask: bit N (per JoypadButton enum) is 1 if held,
 *                0 if released. Idempotent: passing the same value
 *                twice is a no-op.
 *
 * Called by the host once per frame (or per input event) with the
 * current keyboard state. The joypad internally diffs against the
 * previous state to decide whether to raise the joypad interrupt.
 */
void joypad_set_buttons(Joypad* j, uint8_t buttons);

uint8_t joypad_read(const Joypad* j, uint16_t addr);
void    joypad_write(Joypad* j, uint16_t addr, uint8_t value);

#endif