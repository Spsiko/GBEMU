/**
 * @file joypad.c
 *
 * Implementation strategy:
 *
 * We keep three pieces of state:
 *   buttons_held: bitmask of currently-held buttons (host-facing).
 *   select:       which group(s) the CPU has selected via JOYP[5:4].
 *   prev_joyp:    the last JOYP read value, for transition detection.
 *
 * On each call to joypad_set_buttons, and on each write to JOYP that
 * changes the select bits, we recompute the "what would JOYP read as"
 * value and check whether any of bits 3-0 newly went from high to
 * low (i.e., a new button became pressed in the active selection).
 * If so, we raise the joypad interrupt.
 *
 * Note: real hardware fires on the falling edge of any of bits 3-0
 * of the JOYP wire output, which on real hardware is sensitive to
 * which group is selected. A button press while neither group is
 * selected (both 5 and 4 are 1) does NOT cause a falling edge of
 * JOYP[3:0] because both groups read all-1. Our diff approach
 * naturally captures this: if prev_joyp[3:0] was 0xF and current
 * is 0xF, no edge.
 */

#include "joypad.h"
#include "bus.h"
#include <stdlib.h>
#include <stdbool.h>

struct Joypad {
  Bus*    bus;
  uint8_t buttons_held;   // bitmask, JoypadButton positions
  uint8_t select;         // bits 5:4 of last JOYP write (1 = group deselected)
  uint8_t prev_joyp_low;  // bits 3:0 of the last JOYP read value
};

Joypad* joypad_create(Bus* bus) {
  Joypad* j = calloc(1, sizeof(Joypad));
  if (j == NULL) return NULL;
  j->bus = bus;
  return j;
}

void joypad_destroy(Joypad* j) {
  free(j);
}

void joypad_reset(Joypad* j) {
  j->buttons_held  = 0;
  // Power-on default: both groups deselected (CPU has not written yet).
  j->select        = 0x30;
  // No buttons pressed in either group: low nibble reads 0xF.
  j->prev_joyp_low = 0x0F;
}

// Compute the low 4 bits of the JOYP read value given the current
// button state and selection. Bits read 0 if pressed, 1 if released.
// If both groups are selected (both 5 and 4 are 0), both groups OR
// together at the wire level -- effectively, a bit reads low if it
// would read low in either group.
static uint8_t compute_joyp_low(const Joypad* j) {
  bool action_selected = (j->select & 0x20) == 0;  // bit 5 = 0 -> selected
  bool dir_selected    = (j->select & 0x10) == 0;  // bit 4 = 0 -> selected

  // Per-group bitmaps of "is this position pressed":
  //   bit 0: Right / A
  //   bit 1: Left  / B
  //   bit 2: Up    / Select
  //   bit 3: Down  / Start
  uint8_t dir_bits = 0;
  if (j->buttons_held & (1u << JOYPAD_RIGHT)) dir_bits |= 0x01;
  if (j->buttons_held & (1u << JOYPAD_LEFT))  dir_bits |= 0x02;
  if (j->buttons_held & (1u << JOYPAD_UP))    dir_bits |= 0x04;
  if (j->buttons_held & (1u << JOYPAD_DOWN))  dir_bits |= 0x08;

  uint8_t act_bits = 0;
  if (j->buttons_held & (1u << JOYPAD_A))      act_bits |= 0x01;
  if (j->buttons_held & (1u << JOYPAD_B))      act_bits |= 0x02;
  if (j->buttons_held & (1u << JOYPAD_SELECT)) act_bits |= 0x04;
  if (j->buttons_held & (1u << JOYPAD_START))  act_bits |= 0x08;

  uint8_t pressed = 0;
  if (dir_selected)    pressed |= dir_bits;
  if (action_selected) pressed |= act_bits;
  // Convert "pressed" to "JOYP bits": pressed bits are LOW.
  return (uint8_t)(0x0F & ~pressed);
}

// Recompute the current low nibble; if any bit went from 1 to 0
// (high to low), raise the joypad interrupt. Update prev_joyp_low
// either way.
static void update_and_check_irq(Joypad* j) {
  uint8_t now = compute_joyp_low(j);
  uint8_t falling = (uint8_t)(j->prev_joyp_low & ~now);  // was 1, now 0
  if (falling != 0) {
    bus_request_interrupt(j->bus, INT_JOYPAD);
  }
  j->prev_joyp_low = now;
}

void joypad_set_buttons(Joypad* j, uint8_t buttons) {
  if (j->buttons_held == buttons) return;  // idempotent
  j->buttons_held = buttons;
  update_and_check_irq(j);
}

uint8_t joypad_read(const Joypad* j, uint16_t addr) {
  if (addr != 0xFF00) return 0xFF;
  // Bits 7-6 always 1; bits 5-4 reflect current selection; bits 3-0
  // are the live computed state.
  return (uint8_t)(0xC0 | j->select | compute_joyp_low(j));
}

void joypad_write(Joypad* j, uint16_t addr, uint8_t value) {
  if (addr != 0xFF00) return;
  // Only bits 5 and 4 of JOYP are writable.
  uint8_t new_select = (uint8_t)(value & 0x30);
  if (new_select == j->select) return;
  j->select = new_select;
  // Selection change can change which group's bits show up in 3-0,
  // which can produce a falling edge -> joypad interrupt.
  update_and_check_irq(j);
}