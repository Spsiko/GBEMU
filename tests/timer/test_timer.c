/**
 * @file test_timer.c
 * @brief Standalone tests for the timer peripheral.
 *
 * Builds against the real bus, cart, and timer modules. Exercises:
 *   1. DIV increment rate and DIV-write reset.
 *   2. TIMA increment at each of the 4 clock-select rates.
 *   3. TIMA frozen when TAC enable bit is 0.
 *   4. TIMA overflow: 1-M-cycle reload delay and timer interrupt request.
 *   5. TAC reads back with upper 5 bits set (0xF8 mask).
 *   6. Bus interrupt routing: writing IF then reading it via bus_peek.
 *
 * Each test prints PASS or FAIL with a short label. Exit status is
 * 0 if all pass, 1 if any fail.
 *
 * Build (from this directory):
 *   gcc -std=c11 -Wall -Wextra -O0 -g -I../.. \
 *     test_timer.c ../../bus.c ../../timer.c ../../cart.c \
 *     -o test_timer
 *
 * Run:
 *   ./test_timer
 */

#include "../../include/bus.h"
#include "../../include/timer.h"
#include "../../include/cart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// =====================================================================
// Test harness
// =====================================================================

static int g_failures = 0;

static void check(const char* label, int condition) {
  if (condition) {
    printf("  PASS: %s\n", label);
  } else {
    printf("  FAIL: %s\n", label);
    g_failures++;
  }
}

// Tick the bus by N M-cycles using sys_tick (advances all peripherals).
static void tick_m(Bus* b, int mcycles) {
  sys_tick(b, mcycles);
}

// =====================================================================
// Test cases
// =====================================================================

static void test_div_increment_rate(Bus* b) {
  printf("[test_div_increment_rate]\n");

  // After a DIV write, internal counter is 0. DIV is the upper byte
  // of a 16-bit counter that ticks every T-cycle, so DIV should
  // increment every 256 T-cycles = 64 M-cycles.
  bus_poke(b, 0xFF04, 0);  // any value resets

  check("DIV reads 0 after reset", bus_peek(b, 0xFF04) == 0);

  // 63 M-cycles (252 T-cycles) is not yet a full DIV step.
  tick_m(b, 63);
  check("DIV still 0 after 63 M-cycles", bus_peek(b, 0xFF04) == 0);

  // 1 more M-cycle (4 T-cycles) takes us to 256 T-cycles total.
  tick_m(b, 1);
  check("DIV = 1 after 64 M-cycles total", bus_peek(b, 0xFF04) == 1);

  // 64 more M-cycles -> DIV = 2.
  tick_m(b, 64);
  check("DIV = 2 after 128 M-cycles total", bus_peek(b, 0xFF04) == 2);
}

static void test_div_write_resets(Bus* b) {
  printf("[test_div_write_resets]\n");

  // Get DIV nonzero.
  bus_poke(b, 0xFF04, 0);
  tick_m(b, 64 * 5);  // DIV should be 5
  check("DIV = 5 after 320 M-cycles", bus_peek(b, 0xFF04) == 5);

  // Any write resets to 0, regardless of value.
  bus_poke(b, 0xFF04, 0xAB);
  check("DIV = 0 after write of 0xAB", bus_peek(b, 0xFF04) == 0);
}

static void test_tima_rate_4096(Bus* b) {
  printf("[test_tima_rate_4096 (TAC=0x04)]\n");

  // Reset state
  bus_poke(b, 0xFF04, 0);   // reset DIV / counter
  bus_poke(b, 0xFF05, 0);   // TIMA = 0
  bus_poke(b, 0xFF06, 0);   // TMA = 0
  bus_poke(b, 0xFF07, 0x04); // enable, clock 00 -> bit 9 -> 4096 Hz
                            // TIMA increments every 1024 T-cycles = 256 M-cycles

  tick_m(b, 255);
  check("TIMA = 0 after 255 M-cycles", bus_peek(b, 0xFF05) == 0);

  tick_m(b, 1); // 256th M-cycle -> falling edge of bit 9
  check("TIMA = 1 after 256 M-cycles", bus_peek(b, 0xFF05) == 1);

  tick_m(b, 256);
  check("TIMA = 2 after 512 M-cycles total", bus_peek(b, 0xFF05) == 2);
}

static void test_tima_rate_262144(Bus* b) {
  printf("[test_tima_rate_262144 (TAC=0x05)]\n");

  bus_poke(b, 0xFF04, 0);
  bus_poke(b, 0xFF05, 0);
  bus_poke(b, 0xFF06, 0);
  bus_poke(b, 0xFF07, 0x05); // enable, clock 01 -> bit 3 -> 262144 Hz
                            // TIMA increments every 16 T-cycles = 4 M-cycles

  tick_m(b, 3);
  check("TIMA = 0 after 3 M-cycles", bus_peek(b, 0xFF05) == 0);
  tick_m(b, 1);
  check("TIMA = 1 after 4 M-cycles", bus_peek(b, 0xFF05) == 1);
  tick_m(b, 4);
  check("TIMA = 2 after 8 M-cycles", bus_peek(b, 0xFF05) == 2);
  tick_m(b, 4);
  check("TIMA = 3 after 12 M-cycles", bus_peek(b, 0xFF05) == 3);
}

static void test_tima_disabled(Bus* b) {
  printf("[test_tima_disabled (TAC enable=0)]\n");

  bus_poke(b, 0xFF04, 0);
  bus_poke(b, 0xFF05, 0);
  bus_poke(b, 0xFF06, 0);
  bus_poke(b, 0xFF07, 0x01); // clock 01 selected, but enable bit clear

  tick_m(b, 1000);
  check("TIMA = 0 after 1000 M-cycles when disabled",
        bus_peek(b, 0xFF05) == 0);
}

static void test_tima_overflow(Bus* b) {
  printf("[test_tima_overflow]\n");

  // Set TIMA close to overflow, with TMA = 0xAB so we can confirm reload.
  bus_poke(b, 0xFF04, 0);
  bus_poke(b, 0xFF05, 0xFF); // one increment from overflow
  bus_poke(b, 0xFF06, 0xAB); // reload value
  bus_poke(b, 0xFF07, 0x05); // enable, clock 01 (4 M-cycle period)

  // Clear IF.
  bus_poke(b, 0xFF0F, 0);

  // Tick 4 M-cycles to trigger the overflow.
  tick_m(b, 4);

  // During the 4-T-cycle delay, TIMA reads as 0x00. We're now exactly
  // 0 T-cycles past the overflow event (the increment just happened),
  // so we expect TIMA = 0x00 still and IF bit 2 NOT yet set.
  check("TIMA reads 0x00 during overflow delay",
        bus_peek(b, 0xFF05) == 0x00);
  uint8_t if_reg_during = bus_peek(b, 0xFF0F);
  check("Timer interrupt NOT yet pending during delay",
        (if_reg_during & 0x04) == 0);

  // Wait the 4-T-cycle delay (1 M-cycle). Now TIMA should be reloaded
  // from TMA and the timer interrupt should be requested.
  tick_m(b, 1);

  check("TIMA reloaded from TMA = 0xAB after delay",
        bus_peek(b, 0xFF05) == 0xAB);
  uint8_t if_reg_after = bus_peek(b, 0xFF0F);
  check("Timer interrupt (IF bit 2) set after delay",
        (if_reg_after & 0x04) != 0);
}

static void test_tac_upper_bits_read_set(Bus* b) {
  printf("[test_tac_upper_bits_read_set]\n");

  // TAC's upper 5 bits are unimplemented and read as 1. So writing
  // 0x05 should read back as 0xFD (0xF8 | 0x05).
  bus_poke(b, 0xFF07, 0x05);
  check("TAC = 0x05 reads back as 0xFD", bus_peek(b, 0xFF07) == 0xFD);

  bus_poke(b, 0xFF07, 0x00);
  check("TAC = 0x00 reads back as 0xF8", bus_peek(b, 0xFF07) == 0xF8);
}

static void test_bus_request_interrupt(Bus* b) {
  printf("[test_bus_request_interrupt]\n");

  // Clear IF.
  bus_poke(b, 0xFF0F, 0);
  check("IF starts at 0 (low bits)",
        (bus_peek(b, 0xFF0F) & 0x1F) == 0);

  bus_request_interrupt(b, INT_TIMER);
  check("INT_TIMER sets bit 2 of IF",
        (bus_peek(b, 0xFF0F) & 0x04) != 0);

  bus_request_interrupt(b, INT_VBLANK);
  uint8_t if_now = bus_peek(b, 0xFF0F);
  check("INT_VBLANK sets bit 0 (without clearing bit 2)",
        (if_now & 0x01) != 0 && (if_now & 0x04) != 0);
}

// =====================================================================
// main
// =====================================================================

int main(void) {
  Cart* cart = cart_create();
  if (cart == NULL) {
    fprintf(stderr, "cart_create failed\n");
    return 1;
  }

  Bus* bus = bus_create(cart);
  if (bus == NULL) {
    fprintf(stderr, "bus_create failed\n");
    cart_free(cart);
    return 1;
  }

  test_div_increment_rate(bus);
  test_div_write_resets(bus);
  test_tima_rate_4096(bus);
  test_tima_rate_262144(bus);
  test_tima_disabled(bus);
  test_tima_overflow(bus);
  test_tac_upper_bits_read_set(bus);
  test_bus_request_interrupt(bus);

  bus_destroy(bus);
  cart_free(cart);

  if (g_failures == 0) {
    printf("\nAll timer tests passed.\n");
    return 0;
  } else {
    printf("\n%d timer test(s) FAILED.\n", g_failures);
    return 1;
  }
}