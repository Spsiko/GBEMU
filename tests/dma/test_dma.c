/**
 * @file test_dma.c
 * @brief Standalone tests for OAM DMA.
 *
 * Builds against bus + dma + ppu + cart. Each test:
 *   1. Sets up source data via bus_poke (which goes through dispatch
 *      and writes WRAM/HRAM/etc.).
 *   2. Triggers DMA by writing 0xFF46.
 *   3. Ticks the bus the expected number of M-cycles.
 *   4. Verifies OAM contents (using ppu_read_oam directly, since by
 *      this point DMA is done and the PPU is in mode 1 anyway).
 *
 * Build:
 *   gcc -std=c11 -Wall -Wextra -O0 -g -I../.. \
 *     test_dma.c \
 *     ../../bus.c ../../timer.c ../../serial.c ../../ppu.c \
 *     ../../joypad.c ../../dma.c ../../cart.c \
 *     -o test_dma
 */

#include "../../include/bus.h"
#include "../../include/dma.h"
#include "../../include/ppu.h"
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

// =====================================================================
// Tests
// =====================================================================

static void test_basic_copy_from_wram(Bus* b) {
  printf("[test_basic_copy_from_wram]\n");
  // Place known data in WRAM at 0xC000-0xC09F.
  for (int i = 0; i < 160; i++) {
    bus_poke(b, (uint16_t)(0xC000 + i), (uint8_t)(i + 0x10));
  }

  // Wipe OAM.
  Ppu* p = bus_ppu(b);
  for (int i = 0; i < 160; i++) {
    ppu_poke_oam(p, (uint16_t)(0xFE00 + i), 0);
  }

  // Trigger DMA: source = 0xC0 << 8 = 0xC000.
  bus_poke(b, 0xFF46, 0xC0);

  // DMA active immediately; need 160 M-cycles to complete.
  check("DMA active right after trigger", dma_active(bus_dma(b)));

  // Tick 159 M-cycles: still active.
  sys_tick(b, 159);
  check("DMA still active at 159 M-cycles", dma_active(bus_dma(b)));

  // One more M-cycle: done.
  sys_tick(b, 1);
  check("DMA inactive at 160 M-cycles", !dma_active(bus_dma(b)));

  // Disable LCD so OAM reads bypass PPU mode gating.
  ppu_write_reg(p, 0xFF40, 0x00);

  // Verify OAM contents.
  for (int i = 0; i < 160; i++) {
    uint8_t got = ppu_read_oam(p, (uint16_t)(0xFE00 + i));
    if (got != (uint8_t)(i + 0x10)) {
      printf("  FAIL: OAM[%d] = 0x%02X, expected 0x%02X\n",
             i, got, i + 0x10);
      g_failures++;
      return;
    }
  }
  printf("  PASS: all 160 OAM bytes match source\n");
}

static void test_dma_register_readback(Bus* b) {
  printf("[test_dma_register_readback]\n");
  // After completion, reading 0xFF46 should return the last value written.
  bus_poke(b, 0xFF46, 0xC0);
  sys_tick(b, 160);  // complete the transfer
  check_eq("0xFF46 reads back last write value 0xC0",
           bus_peek(b, 0xFF46), 0xC0);
}

static void test_cpu_access_blocked_during_dma(Bus* b) {
  printf("[test_cpu_access_blocked_during_dma]\n");
  // Set known values in WRAM and HRAM.
  bus_poke(b, 0xC100, 0xAA);  // WRAM
  bus_poke(b, 0xFF85, 0x55);  // HRAM (0xFF80-0xFFFE)
  bus_poke(b, 0xFFFF, 0x99);  // IE

  // Trigger DMA.
  bus_poke(b, 0xFF46, 0xC0);
  check("DMA active", dma_active(bus_dma(b)));

  // bus_read of WRAM during DMA should return 0xFF.
  // Note: bus_read also ticks 1 M-cycle. We've already ticked 0
  // M-cycles since the DMA write, so bus_read here is the FIRST
  // M-cycle after trigger; DMA is still active.
  check_eq("WRAM read during DMA returns 0xFF",
           bus_read(b, 0xC100), 0xFF);

  // bus_read of HRAM during DMA should still work.
  // bus_read ticks another M-cycle. We've ticked 2 M-cycles total now.
  check_eq("HRAM read during DMA returns real value",
           bus_read(b, 0xFF85), 0x55);

  // IE read during DMA should work.
  check_eq("IE read during DMA returns real value",
           bus_read(b, 0xFFFF), 0x99);

  // Finish out the transfer (157 more M-cycles).
  sys_tick(b, 157);
  check("DMA complete", !dma_active(bus_dma(b)));

  // Now WRAM read should work.
  check_eq("WRAM read after DMA returns real value",
           bus_peek(b, 0xC100), 0xAA);
}

static void test_writes_blocked_during_dma(Bus* b) {
  printf("[test_writes_blocked_during_dma]\n");
  // Set known initial values.
  bus_poke(b, 0xC200, 0x11);
  bus_poke(b, 0xFF85, 0x22);

  // Trigger DMA.
  bus_poke(b, 0xFF46, 0xC0);

  // Try to write WRAM during DMA: should be dropped.
  bus_write(b, 0xC200, 0xFF);
  // HRAM write should succeed.
  bus_write(b, 0xFF85, 0xEE);

  // Finish.
  sys_tick(b, 158);
  check("DMA complete", !dma_active(bus_dma(b)));

  check_eq("WRAM write during DMA was dropped (still 0x11)",
           bus_peek(b, 0xC200), 0x11);
  check_eq("HRAM write during DMA succeeded (now 0xEE)",
           bus_peek(b, 0xFF85), 0xEE);
}

static void test_dma_register_writable_during_dma(Bus* b) {
  printf("[test_dma_register_writable_during_dma]\n");
  // Set up two distinct source regions.
  for (int i = 0; i < 160; i++) {
    bus_poke(b, (uint16_t)(0xC000 + i), 0xAA);   // page 0xC0
    bus_poke(b, (uint16_t)(0xC100 + i), 0xBB);   // page 0xC1
  }

  // Wipe OAM.
  Ppu* p = bus_ppu(b);
  for (int i = 0; i < 160; i++) {
    ppu_poke_oam(p, (uint16_t)(0xFE00 + i), 0);
  }

  // Start DMA from 0xC0.
  bus_poke(b, 0xFF46, 0xC0);
  // After 80 M-cycles, restart DMA from 0xC1 (mid-transfer).
  sys_tick(b, 80);
  bus_write(b, 0xFF46, 0xC1);  // bus_write -- must succeed despite DMA
  // Finish: 160 more M-cycles for the new transfer.
  sys_tick(b, 160);
  check("DMA complete", !dma_active(bus_dma(b)));

  ppu_write_reg(p, 0xFF40, 0x00);  // disable LCD for OAM read access

  // After the restart, all 160 OAM bytes should be 0xBB.
  for (int i = 0; i < 160; i++) {
    uint8_t got = ppu_read_oam(p, (uint16_t)(0xFE00 + i));
    if (got != 0xBB) {
      printf("  FAIL: OAM[%d] = 0x%02X, expected 0xBB (post-restart)\n",
             i, got);
      g_failures++;
      return;
    }
  }
  printf("  PASS: post-restart OAM is all 0xBB (second source)\n");
}

static void test_source_in_rom(Bus* b) {
  printf("[test_source_in_rom]\n");
  // We can't easily put data in ROM (cart is empty in this test
  // setup), but we CAN put data in HRAM and DMA from there.
  // Source 0xFF doesn't actually work on real hardware (HRAM is only
  // 127 bytes), but pages like 0xC0..0xDF (WRAM) all do.
  // We've already covered 0xC0; let's try 0xD0 (still WRAM, since
  // WRAM is 0xC000-0xDFFF).
  for (int i = 0; i < 160; i++) {
    bus_poke(b, (uint16_t)(0xD000 + i), (uint8_t)(0x80 + (i & 0x7F)));
  }
  Ppu* p = bus_ppu(b);
  for (int i = 0; i < 160; i++) {
    ppu_poke_oam(p, (uint16_t)(0xFE00 + i), 0);
  }

  bus_poke(b, 0xFF46, 0xD0);
  sys_tick(b, 160);

  ppu_write_reg(p, 0xFF40, 0x00);  // disable LCD so OAM reads bypass gating

  for (int i = 0; i < 160; i++) {
    uint8_t got = ppu_read_oam(p, (uint16_t)(0xFE00 + i));
    if (got != (uint8_t)(0x80 + (i & 0x7F))) {
      printf("  FAIL: OAM[%d] = 0x%02X, expected 0x%02X\n",
             i, got, 0x80 + (i & 0x7F));
      g_failures++;
      return;
    }
  }
  printf("  PASS: DMA from page 0xD0 (later WRAM) succeeded\n");
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

  test_basic_copy_from_wram(bus);
  test_dma_register_readback(bus);
  test_cpu_access_blocked_during_dma(bus);
  test_writes_blocked_during_dma(bus);
  test_dma_register_writable_during_dma(bus);
  test_source_in_rom(bus);

  bus_destroy(bus);
  cart_free(cart);

  if (g_failures == 0) {
    printf("\nAll DMA tests passed.\n");
    return 0;
  } else {
    printf("\n%d DMA test(s) FAILED.\n", g_failures);
    return 1;
  }
}