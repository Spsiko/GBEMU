/**
 * @file test_joypad.c
 * @brief Standalone tests for the joypad peripheral.
 *
 * Builds against the real bus + joypad. Each test:
 *   1. Starts from a known reset state.
 *   2. Sets buttons via joypad_set_buttons.
 *   3. Selects a group via JOYP write.
 *   4. Reads JOYP and checks bits.
 *   5. Where applicable, checks IF bit 4 (joypad interrupt).
 *
 * Build:
 *   gcc -std=c11 -Wall -Wextra -O0 -g -I../.. \
 *     test_joypad.c \
 *     ../../bus.c ../../timer.c ../../serial.c ../../ppu.c \
 *     ../../joypad.c ../../cart.c \
 *     -o test_joypad
 */

#include "../../include/bus.h"
#include "../../include/joypad.h"
#include "../../include/cart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int g_failures = 0;

static void check_eq(const char* label, uint32_t actual, uint32_t expected) {
  if (actual == expected) {
    printf("  PASS: %s\n", label);
  } else {
    printf("  FAIL: %s (got 0x%X, expected 0x%X)\n", label, actual, expected);
    g_failures++;
  }
}

static void check(const char* label, int condition) {
  if (condition) {
    printf("  PASS: %s\n", label);
  } else {
    printf("  FAIL: %s\n", label);
    g_failures++;
  }
}

// Helper: set selection bits 5:4 of JOYP via bus_poke. Bits set
// (= 1) deselect that group, bits clear (= 0) select.
//   sel_action = 1 means "select action group" (bit 5 = 0)
//   sel_dir    = 1 means "select direction group" (bit 4 = 0)
static void select_groups(Bus* b, int sel_action, int sel_dir) {
  uint8_t v = 0;
  if (!sel_action) v |= 0x20;
  if (!sel_dir)    v |= 0x10;
  bus_poke(b, 0xFF00, v);
}

// =====================================================================
// Tests
// =====================================================================

static void test_no_buttons(Bus* b) {
  printf("[test_no_buttons]\n");
  joypad_reset(bus_joypad(b));
  // Default: both groups deselected. Reading JOYP returns
  // 0xC0 | 0x30 | 0x0F = 0xFF.
  check_eq("post-reset JOYP read = 0xFF", bus_peek(b, 0xFF00), 0xFF);

  // Select action group (bit 5 = 0, bit 4 = 1: direction deselected).
  // No buttons held: low nibble 0xF.
  // JOYP = 0xC0 | 0x10 | 0x0F = 0xDF.
  select_groups(b, 1, 0);
  check_eq("select action only, no buttons -> JOYP = 0xDF",
           bus_peek(b, 0xFF00), 0xDF);

  // Select direction group (bit 5 = 1, bit 4 = 0: action deselected).
  // JOYP = 0xC0 | 0x20 | 0x0F = 0xEF.
  select_groups(b, 0, 1);
  check_eq("select direction only, no buttons -> JOYP = 0xEF",
           bus_peek(b, 0xFF00), 0xEF);
}

static void test_press_action_button(Bus* b) {
  printf("[test_press_action_button]\n");
  joypad_reset(bus_joypad(b));
  // Press A. Action group: A is at bit 0.
  joypad_set_buttons(bus_joypad(b), 1u << JOYPAD_A);
  // Select action group.
  select_groups(b, 1, 0);
  // JOYP: 0xC0 | 0x10 (dir deselected) | 0x0E (bit 0 low because A pressed)
  //     = 0xDE.
  check_eq("A pressed, action selected -> JOYP = 0xDE",
           bus_peek(b, 0xFF00), 0xDE);

  // Switch to direction group: A no longer visible -> low nibble 0xF.
  // JOYP: 0xC0 | 0x20 (action deselected) | 0x0F = 0xEF.
  select_groups(b, 0, 1);
  check_eq("A pressed but only direction selected -> JOYP = 0xEF",
           bus_peek(b, 0xFF00), 0xEF);
}

static void test_press_direction_button(Bus* b) {
  printf("[test_press_direction_button]\n");
  joypad_reset(bus_joypad(b));
  // Press Up. Direction group: Up is at bit 2.
  joypad_set_buttons(bus_joypad(b), 1u << JOYPAD_UP);
  select_groups(b, 0, 1);
  // JOYP: 0xC0 | 0x20 (action deselected) | 0x0B (bit 2 low) = 0xEB.
  check_eq("Up pressed, direction selected -> JOYP = 0xEB",
           bus_peek(b, 0xFF00), 0xEB);
}

static void test_both_groups_selected(Bus* b) {
  printf("[test_both_groups_selected]\n");
  joypad_reset(bus_joypad(b));
  // Press A (action bit 0) and Down (direction bit 3) simultaneously.
  joypad_set_buttons(bus_joypad(b), (1u << JOYPAD_A) | (1u << JOYPAD_DOWN));
  // Select BOTH groups: bits 5 and 4 both 0.
  select_groups(b, 1, 1);
  // Both groups OR together at the low nibble:
  //   action group: A pressed -> bit 0 low
  //   direction group: Down pressed -> bit 3 low
  // Combined: bits 0 and 3 low -> low nibble 0x06.
  // JOYP: 0xC0 | 0x00 | 0x06 = 0xC6.
  check_eq("A + Down, both selected -> JOYP = 0xC6",
           bus_peek(b, 0xFF00), 0xC6);
}

static void test_press_fires_interrupt(Bus* b) {
  printf("[test_press_fires_interrupt]\n");
  joypad_reset(bus_joypad(b));
  // Action group must be selected for an action-button press to
  // produce a falling edge in JOYP[3:0].
  select_groups(b, 1, 0);
  bus_poke(b, 0xFF0F, 0); // clear IF

  joypad_set_buttons(bus_joypad(b), 1u << JOYPAD_A);
  // INT_JOYPAD = 4, so bit 4 of IF should be set.
  check("press A with action group selected fires joypad IRQ",
        (bus_peek(b, 0xFF0F) & 0x10) != 0);
}

static void test_hold_does_not_refire(Bus* b) {
  printf("[test_hold_does_not_refire]\n");
  joypad_reset(bus_joypad(b));
  select_groups(b, 1, 0);

  // Press A.
  joypad_set_buttons(bus_joypad(b), 1u << JOYPAD_A);
  // Clear IF -- the press above already fired. We're checking that
  // a subsequent identical set_buttons doesn't fire again.
  bus_poke(b, 0xFF0F, 0);

  // Hold (same value).
  joypad_set_buttons(bus_joypad(b), 1u << JOYPAD_A);
  check("hold (same set_buttons value) does not refire IRQ",
        (bus_peek(b, 0xFF0F) & 0x10) == 0);
}

static void test_release_does_not_fire(Bus* b) {
  printf("[test_release_does_not_fire]\n");
  joypad_reset(bus_joypad(b));
  select_groups(b, 1, 0);
  joypad_set_buttons(bus_joypad(b), 1u << JOYPAD_A);
  bus_poke(b, 0xFF0F, 0);

  // Release: 0 buttons held. JOYP[3:0] goes from 0xE -> 0xF, a RISING
  // edge (not falling). Should NOT fire the joypad IRQ.
  joypad_set_buttons(bus_joypad(b), 0);
  check("release does not fire IRQ",
        (bus_peek(b, 0xFF0F) & 0x10) == 0);
}

static void test_press_in_unselected_group_does_not_fire(Bus* b) {
  printf("[test_press_in_unselected_group_does_not_fire]\n");
  joypad_reset(bus_joypad(b));
  // Select direction group only.
  select_groups(b, 0, 1);
  bus_poke(b, 0xFF0F, 0);

  // Press A (action group). Direction group's bits in JOYP[3:0] don't
  // change (no direction button is pressed), so no falling edge.
  joypad_set_buttons(bus_joypad(b), 1u << JOYPAD_A);
  check("press A while only direction selected: no IRQ",
        (bus_peek(b, 0xFF0F) & 0x10) == 0);
}

static void test_select_change_can_fire(Bus* b) {
  printf("[test_select_change_can_fire]\n");
  joypad_reset(bus_joypad(b));
  // Press A while action group is NOT selected (so no IRQ yet).
  select_groups(b, 0, 1);     // direction selected
  joypad_set_buttons(bus_joypad(b), 1u << JOYPAD_A);
  bus_poke(b, 0xFF0F, 0);

  // Now switch selection to the action group. JOYP[3:0] now exposes
  // A's pressed state, so bit 0 transitions 1 -> 0: falling edge,
  // joypad IRQ fires. This is real hardware behavior.
  select_groups(b, 1, 0);
  check("selection change exposing pressed button fires IRQ",
        (bus_peek(b, 0xFF0F) & 0x10) != 0);
}

static void test_write_only_affects_select_bits(Bus* b) {
  printf("[test_write_only_affects_select_bits]\n");
  joypad_reset(bus_joypad(b));
  // Try writing a value with all bits set.
  bus_poke(b, 0xFF00, 0xFF);
  // Bits 7-6 always read 1. Bits 5-4 should reflect what we wrote
  // (both 1 -> both deselected). Bits 3-0 should be 0xF (no buttons).
  check_eq("write 0xFF: read returns 0xFF (groups deselected, no buttons)",
           bus_peek(b, 0xFF00), 0xFF);

  // Write 0x00. Both groups selected, no buttons held -> JOYP = 0xCF.
  bus_poke(b, 0xFF00, 0x00);
  check_eq("write 0x00 (both selected, no buttons) -> JOYP = 0xCF",
           bus_peek(b, 0xFF00), 0xCF);

  // Write 0x0F (try to set bits 3-0): should be ignored.
  bus_poke(b, 0xFF00, 0x0F);
  check_eq("write 0x0F: low bits ignored, JOYP = 0xCF",
           bus_peek(b, 0xFF00), 0xCF);
}

// =====================================================================
// main
// =====================================================================

int main(void) {
  Cart* cart = cart_create();
  Bus* bus = bus_create(cart);
  if (cart == NULL || bus == NULL) {
    fprintf(stderr, "setup failed\n");
    return 1;
  }

  test_no_buttons(bus);
  test_press_action_button(bus);
  test_press_direction_button(bus);
  test_both_groups_selected(bus);
  test_press_fires_interrupt(bus);
  test_hold_does_not_refire(bus);
  test_release_does_not_fire(bus);
  test_press_in_unselected_group_does_not_fire(bus);
  test_select_change_can_fire(bus);
  test_write_only_affects_select_bits(bus);

  bus_destroy(bus);
  cart_free(cart);

  if (g_failures == 0) {
    printf("\nAll joypad tests passed.\n");
    return 0;
  } else {
    printf("\n%d joypad test(s) FAILED.\n", g_failures);
    return 1;
  }
}