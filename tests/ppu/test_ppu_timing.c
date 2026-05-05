/**
 * @file test_ppu_timing.c
 * @brief Standalone tests for PPU Batch A: state machine, registers,
 *        and interrupt sources.
 *
 * Builds against the real bus + ppu + timer + cart + serial. No
 * rendering is exercised (Batch A has none); we just verify that the
 * state machine walks through modes 2/3/0 at the right T-cycle
 * boundaries and that LY/STAT/IF are updated correctly.
 *
 * Build:
 *   gcc -std=c11 -Wall -Wextra -O0 -g -I../.. \
 *     test_ppu_timing.c \
 *     ../../bus.c ../../timer.c ../../serial.c ../../ppu.c \
 *     ../../cart.c \
 *     -o test_ppu_timing
 */

#include "../../include/bus.h"
#include "../../include/ppu.h"
#include "../../include/cart.h"
#include "../../include/timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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

static void check_eq_u(const char* label, uint32_t actual, uint32_t expected) {
  if (actual == expected) {
    printf("  PASS: %s\n", label);
  } else {
    printf("  FAIL: %s (got 0x%X, expected 0x%X)\n", label, actual, expected);
    g_failures++;
  }
}

// Tick the bus directly. mcycles -> T-cycles internally.
static void tick_t(Bus* b, int t_cycles) {
  // sys_tick takes M-cycles. We need a way to do T-cycle granular ticks,
  // which means converting. Most of our test points fall on M-cycle
  // boundaries (multiples of 4) but the PPU's mode boundaries don't.
  // For this we rely on sys_tick(b, 1) ticking 4 T-cycles -- so we'll
  // step in M-cycle units and pick test points that align.
  //
  // For the rare test that needs finer granularity, we'd extend the
  // bus API. Batch A's test points are all multiples of 4.
  if (t_cycles % 4 != 0) {
    fprintf(stderr, "tick_t requires multiples of 4 T-cycles\n");
    exit(1);
  }
  sys_tick(b, t_cycles / 4);
}

// =====================================================================
// Helpers to read state without going through bus_read (which would
// introduce its own ticks).
// =====================================================================

static uint8_t peek_reg(Bus* b, uint16_t addr) {
  return ppu_read_reg(bus_ppu(b), addr);
}

// Returns mode bits (0-3) from STAT.
static uint8_t mode_now(Bus* b) {
  return peek_reg(b, 0xFF41) & 0x03;
}

static uint8_t ly_now(Bus* b) {
  return peek_reg(b, 0xFF44);
}

static uint8_t if_now(Bus* b) {
  return bus_peek(b, 0xFF0F) & 0x1F;
}

static void clear_if(Bus* b) {
  bus_poke(b, 0xFF0F, 0);
}

// =====================================================================
// Test cases
// =====================================================================

static void test_initial_state(Bus* b) {
  printf("[test_initial_state]\n");
  // ppu_reset puts us at LY=0, mode=2, dot=0, LCD on (LCDC=0x91).
  check_eq_u("LY = 0",    ly_now(b),    0);
  check_eq_u("mode = 2",  mode_now(b),  2);
  // STAT bit 7 always reads 1.
  check("STAT bit 7 reads 1", (peek_reg(b, 0xFF41) & 0x80) != 0);
}

static void test_mode_transitions_visible_line(Bus* b) {
  printf("[test_mode_transitions_visible_line]\n");
  // Reset state by toggling LCD off and on.
  bus_poke(b, 0xFF40, 0x11);   // LCD off (bit 7 = 0)
  bus_poke(b, 0xFF40, 0x91);   // LCD on
  // Now at LY=0, mode=0 immediately after LCD-on; the very next tick
  // will advance dot to 1 but mode stays as it was -- our reset puts
  // mode=0 on LCD-off. Walk through carefully.
  //
  // With LCD freshly on: ly=0, dot=0, mode=0 (from the off-state).
  // First tick advances dot to 1. We don't transition to mode 2 until
  // a new line starts. To get a clean view of mode transitions within
  // a visible line we instead use the post-reset state (mode 2) and
  // start observing.
  //
  // For simplicity, just call ppu_reset semantics by recreating: skip
  // the LCD-off dance and assume we're early in a fresh frame.
  //
  // This test runs after test_initial_state, so we're at ly=0, mode=2,
  // dot=0. Tick to specific boundaries.

  // Drive with a fresh reset state: tick from dot=0.
  // dot 0 -> 79 = mode 2 (80 cycles). Tick 76 T-cycles (19 M-cycles)
  // to land at dot=76, still mode 2.
  tick_t(b, 76);
  check_eq_u("dot 76 in mode 2", mode_now(b), 2);

  // Tick 4 more -> dot 80 -> mode 3.
  tick_t(b, 4);
  check_eq_u("dot 80 in mode 3", mode_now(b), 3);

  // Mode 3 is 172 cycles, so dot 80..251 = mode 3. Tick another 168
  // (172 - 4) to land at dot=248, still mode 3.
  tick_t(b, 168);
  check_eq_u("dot 248 still mode 3", mode_now(b), 3);

  // Tick 4 -> dot 252 -> mode 0.
  tick_t(b, 4);
  check_eq_u("dot 252 in mode 0 (HBlank)", mode_now(b), 0);

  // Mode 0 lasts to dot 455. Tick 200 -> dot 452, still mode 0.
  tick_t(b, 200);
  check_eq_u("dot 452 still mode 0", mode_now(b), 0);

  // Tick 4 -> dot 0 of next line, mode=2, LY=1.
  tick_t(b, 4);
  check_eq_u("after 456 cycles: LY = 1", ly_now(b), 1);
  check_eq_u("after 456 cycles: mode = 2", mode_now(b), 2);
}

static void test_vblank_entry(Bus* b) {
  printf("[test_vblank_entry]\n");
  // Reset.
  bus_poke(b, 0xFF40, 0x11);
  bus_poke(b, 0xFF40, 0x91);
  clear_if(b);

  // Tick 144 full lines -> LY should be 144, in VBlank (mode 1),
  // and IF VBlank bit set.
  // After LCD-on the very first tick advances dot from 0 to 1 with
  // mode=0, then transitions don't happen until dot=80. To get clean
  // behavior we count 144 * 456 = 65664 T-cycles.
  // But the off-on dance doesn't perfectly reset -- our LCD-on path
  // doesn't fire any boundary handling. The state right after LCD-on
  // is dot=0, ly=0, mode=0 (carried from off). The next tick advances
  // dot to 1 in mode 0, and we never enter mode 2 on this line.
  //
  // For this test we want a clean "first line" setup. Reset clean
  // and pre-tick one full line so the mode machinery is fresh.
  // After 456 ticks LY becomes 1, mode becomes 2 (we tested that).
  // Continue ticking from there.

  tick_t(b, 456);    // LY -> 1, mode 2
  // 143 more lines puts LY = 144, which is VBlank entry.
  tick_t(b, 143 * 456);

  check_eq_u("LY = 144 at VBlank entry", ly_now(b), 144);
  check_eq_u("mode = 1 in VBlank", mode_now(b), 1);
  check("IF VBlank bit (0x01) set", (if_now(b) & 0x01) != 0);
}

static void test_vblank_duration(Bus* b) {
  printf("[test_vblank_duration]\n");
  // From the previous test we're at LY=144, dot=0 (just entered VBlank).
  // Tick 9 full lines -> LY=153, still mode 1.
  tick_t(b, 9 * 456);
  check_eq_u("LY = 153 in late VBlank", ly_now(b), 153);
  check_eq_u("mode = 1 in late VBlank", mode_now(b), 1);

  // Tick 1 more line -> LY wraps to 0, mode 2.
  tick_t(b, 456);
  check_eq_u("LY wraps to 0 after VBlank", ly_now(b), 0);
  check_eq_u("mode returns to 2", mode_now(b), 2);
}

static void test_lyc_compare(Bus* b) {
  printf("[test_lyc_compare]\n");
  // Reset.
  bus_poke(b, 0xFF40, 0x11);
  bus_poke(b, 0xFF40, 0x91);
  clear_if(b);

  // Set LYC = 5. Currently LY=0 (just after reset), so STAT bit 2 = 0.
  bus_poke(b, 0xFF45, 5);
  check("LYC=5 LY=0 -> STAT bit 2 clear", (peek_reg(b, 0xFF41) & 0x04) == 0);

  // Tick 5 full lines -> LY = 5.
  tick_t(b, 5 * 456);
  check_eq_u("LY = 5", ly_now(b), 5);
  check("LY=LYC -> STAT bit 2 set", (peek_reg(b, 0xFF41) & 0x04) != 0);
}

static void test_lyc_interrupt(Bus* b) {
  printf("[test_lyc_interrupt]\n");
  // Reset.
  bus_poke(b, 0xFF40, 0x11);
  bus_poke(b, 0xFF40, 0x91);
  clear_if(b);

  // Enable LYC interrupt source (STAT bit 6) and set LYC=10.
  bus_poke(b, 0xFF41, 0x40);
  bus_poke(b, 0xFF45, 10);

  // Confirm STAT IRQ not yet pending.
  check("STAT IRQ not yet pending", (if_now(b) & 0x02) == 0);

  // Tick to LY=10. Note: pre-tick one full line so we don't pick up
  // spurious interrupts from the LCD-on glitch at LY=0 dot=0.
  tick_t(b, 10 * 456);

  check_eq_u("LY = 10", ly_now(b), 10);
  check("STAT IRQ (IF bit 1) raised on LY=LYC", (if_now(b) & 0x02) != 0);
}

static void test_mode2_interrupt(Bus* b) {
  printf("[test_mode2_interrupt]\n");
  // Reset.
  bus_poke(b, 0xFF40, 0x11);
  bus_poke(b, 0xFF40, 0x91);
  clear_if(b);

  // Enable mode-2 interrupt source.
  bus_poke(b, 0xFF41, 0x20);

  // Pre-tick one full line so we're at LY=1, dot=0, mode=2 -- the
  // mode-2 entry should have just fired the STAT IRQ.
  tick_t(b, 456);
  check_eq_u("LY = 1 mode = 2", mode_now(b), 2);
  check("STAT IRQ fired on mode 2 entry", (if_now(b) & 0x02) != 0);

  // Clear IF, tick to next line. Mode 2 entry should fire again.
  clear_if(b);
  tick_t(b, 456);
  check("STAT IRQ fires again on next mode 2 entry",
        (if_now(b) & 0x02) != 0);
}

static void test_mode0_interrupt(Bus* b) {
  printf("[test_mode0_interrupt]\n");
  // Reset.
  bus_poke(b, 0xFF40, 0x11);
  bus_poke(b, 0xFF40, 0x91);
  clear_if(b);

  // Pre-tick one full line so we're cleanly at LY=1 dot=0 mode=2.
  tick_t(b, 456);

  // Enable mode-0 interrupt source. Don't enable the others -- we
  // don't want spurious fires from mode 2 we're already in.
  bus_poke(b, 0xFF41, 0x08);
  clear_if(b);

  // Tick from mode 2 (80 cycles) into mode 3 (172 cycles), then to
  // mode 0. Total: 80 + 172 = 252 cycles.
  tick_t(b, 252);
  check_eq_u("now in mode 0", mode_now(b), 0);
  check("STAT IRQ fired on mode 0 entry", (if_now(b) & 0x02) != 0);
}

static void test_mode1_interrupt(Bus* b) {
  printf("[test_mode1_interrupt]\n");
  // Reset.
  bus_poke(b, 0xFF40, 0x11);
  bus_poke(b, 0xFF40, 0x91);
  clear_if(b);

  // Enable mode-1 interrupt source.
  bus_poke(b, 0xFF41, 0x10);

  // Tick to LY=144 (VBlank entry). Pre-tick one line as before.
  tick_t(b, 456);
  clear_if(b);
  tick_t(b, 143 * 456);

  check_eq_u("mode = 1 (VBlank)", mode_now(b), 1);
  // Both VBlank IRQ (bit 0) and STAT IRQ (bit 1) should fire.
  check("VBlank IRQ raised", (if_now(b) & 0x01) != 0);
  check("STAT IRQ raised on mode 1 entry", (if_now(b) & 0x02) != 0);
}

static void test_vram_gating(Bus* b) {
  printf("[test_vram_gating]\n");
  // Reset.
  bus_poke(b, 0xFF40, 0x11);
  bus_poke(b, 0xFF40, 0x91);

  // Pre-tick to LY=1 mode 2 dot 0 cleanly.
  tick_t(b, 456);

  // mode 2: VRAM should be readable. Write a sentinel via bus_poke
  // (which uses dispatch_write directly without ticking, but the PPU
  // still gates via mode -- we're in mode 2 so writes go through).
  bus_poke(b, 0x8000, 0xAB);
  check_eq_u("mode 2: VRAM read returns 0xAB",
             bus_peek(b, 0x8000), 0xAB);

  // Tick into mode 3 (80 cycles to dot 80).
  tick_t(b, 80);
  check_eq_u("now in mode 3", mode_now(b), 3);

  // mode 3: VRAM read should return 0xFF; writes drop.
  bus_poke(b, 0x8000, 0xCD);  // attempt write
  check_eq_u("mode 3: VRAM read returns 0xFF",
             bus_peek(b, 0x8000), 0xFF);

  // Tick into mode 0.
  tick_t(b, 172);
  check_eq_u("now in mode 0", mode_now(b), 0);
  // Now reads should succeed and reveal that the write during mode 3
  // was dropped (still 0xAB).
  check_eq_u("mode 0: VRAM read succeeds, mode-3 write was dropped",
             bus_peek(b, 0x8000), 0xAB);
}

static void test_oam_gating(Bus* b) {
  printf("[test_oam_gating]\n");
  // Reset.
  bus_poke(b, 0xFF40, 0x11);
  bus_poke(b, 0xFF40, 0x91);

  // Pre-tick one line so we're at LY=1 mode=2 dot=0.
  tick_t(b, 456);

  // mode 2: OAM blocked. Read should return 0xFF; writes drop.
  bus_poke(b, 0xFE00, 0x42);
  check_eq_u("mode 2: OAM read returns 0xFF",
             bus_peek(b, 0xFE00), 0xFF);

  // Tick to mode 0 (80 + 172 = 252 cycles).
  tick_t(b, 252);
  check_eq_u("now in mode 0", mode_now(b), 0);

  // mode 0: OAM accessible. Confirm previous write was dropped.
  // (OAM was zeroed at reset, and the mode-2 write didn't go through.)
  check_eq_u("mode 0: OAM is 0 (mode 2 write was dropped)",
             bus_peek(b, 0xFE00), 0x00);

  // Now write in mode 0 and read back.
  bus_poke(b, 0xFE00, 0x55);
  check_eq_u("mode 0: OAM r/w works",
             bus_peek(b, 0xFE00), 0x55);
}

static void test_lcd_off_freezes(Bus* b) {
  printf("[test_lcd_off_freezes]\n");
  // Pre-tick somewhere in the middle of the frame.
  bus_poke(b, 0xFF40, 0x11);   // off
  bus_poke(b, 0xFF40, 0x91);   // on
  tick_t(b, 50 * 456);         // LY should be 50
  check_eq_u("LY = 50 with LCD on", ly_now(b), 50);

  // Turn LCD off. State should reset to LY=0.
  bus_poke(b, 0xFF40, 0x11);
  check_eq_u("LCD off resets LY to 0", ly_now(b), 0);

  // Tick a bunch with LCD off -- LY should not advance.
  tick_t(b, 1000);
  check_eq_u("LY frozen at 0 with LCD off", ly_now(b), 0);
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

  // Each test resets state itself (via LCD-off/LCD-on dance), so the
  // call order is independent.
  test_initial_state(bus);
  test_mode_transitions_visible_line(bus);
  test_vblank_entry(bus);
  test_vblank_duration(bus);
  test_lyc_compare(bus);
  test_lyc_interrupt(bus);
  test_mode2_interrupt(bus);
  test_mode0_interrupt(bus);
  test_mode1_interrupt(bus);
  test_vram_gating(bus);
  test_oam_gating(bus);
  test_lcd_off_freezes(bus);

  bus_destroy(bus);
  cart_free(cart);

  if (g_failures == 0) {
    printf("\nAll PPU timing tests passed.\n");
    return 0;
  } else {
    printf("\n%d PPU timing test(s) FAILED.\n", g_failures);
    return 1;
  }
}