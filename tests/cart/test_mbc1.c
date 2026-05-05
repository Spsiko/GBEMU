/**
 * @file test_mbc1.c
 * @brief Standalone tests for MBC1 cart behaviour.
 *
 * Synthesises a 128 KB ROM in memory (8 banks of 16 KB), with each
 * bank tagged so that reads from 0x4000 after a bank switch can be
 * verified. Exercises:
 *
 *   1. Default state: bank 1 selected at 0x4000-0x7FFF.
 *   2. ROM bank switch via writes to 0x2000-0x3FFF.
 *   3. The 0->1 quirk: writing 0 selects bank 1.
 *   4. Bank number masked against actual ROM size.
 *   5. RAM enable gating (0xA enables; anything else disables).
 *   6. RAM read/write round-trip.
 *   7. Mode 1 with bank_hi set: bank 0x20 visible at 0x0000-0x3FFF.
 *      (Synthesised with a 1MB ROM so bank 0x20 is within range.)
 *
 * Build:
 *   gcc -std=c11 -Wall -Wextra -O0 -g -I../.. \
 *     test_mbc1.c ../../cart.c -o test_mbc1
 *
 * Run:
 *   ./test_mbc1
 */

#include "../../include/cart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Nintendo logo bytes required at 0x0104-0x0133.
static const uint8_t kNintendoLogo[48] = {
  0xCE,0xED,0x66,0x66,0xCC,0x0D,0x00,0x0B,0x03,0x73,0x00,0x83,0x00,0x0C,0x00,0x0D,
  0x00,0x08,0x11,0x1F,0x88,0x89,0x00,0x0E,0xDC,0xCC,0x6E,0xE6,0xDD,0xDD,0xD9,0x99,
  0xBB,0xBB,0x67,0x63,0x6E,0x0E,0xEC,0xCC,0xDD,0xDC,0x99,0x9F,0xBB,0xB9,0x33,0x3E,
};

// =====================================================================
// Test harness
// =====================================================================

static int g_failures = 0;

static void check_eq(const char* label, uint32_t actual, uint32_t expected) {
  if (actual == expected) {
    printf("  PASS: %s\n", label);
  } else {
    printf("  FAIL: %s (got 0x%X, expected 0x%X)\n", label, actual, expected);
    g_failures++;
  }
}

// Build a synthetic ROM image with the given size, tagged so that
// each bank's byte at offset 0 is the bank number.
//   rom[bank * 0x4000 + 0] = bank
// Header is constructed inside bank 0 (rom[0x0100..0x014F]).
//
// Caller must free() the returned buffer.
static uint8_t* synthesise_rom(size_t banks, uint8_t mbc_type_code,
                               uint8_t rom_size_code, uint8_t ram_size_code) {
  size_t sz = banks * 0x4000;
  uint8_t* rom = calloc(1, sz);
  if (rom == NULL) return NULL;

  // Tag every bank.
  for (size_t b = 0; b < banks; b++) {
    rom[b * 0x4000] = (uint8_t)b;
  }

  // Header.
  rom[0x0100] = 0x00; // NOP
  rom[0x0101] = 0xC3; // JP 0x0150
  rom[0x0102] = 0x50;
  rom[0x0103] = 0x01;
  memcpy(&rom[0x0104], kNintendoLogo, 48);
  rom[0x0147] = mbc_type_code;
  rom[0x0148] = rom_size_code;
  rom[0x0149] = ram_size_code;

  // Header checksum at 0x014D: sum = 0; for a in 0x134..0x14C: sum -= rom[a] + 1.
  uint8_t sum = 0;
  for (uint16_t a = 0x0134; a <= 0x014C; a++) {
    sum -= rom[a] + 1;
  }
  rom[0x014D] = sum;

  return rom;
}

// =====================================================================
// Test cases
// =====================================================================

static void test_default_state(void) {
  printf("[test_default_state]\n");
  // 128 KB MBC1 ROM, no RAM.
  // mbc=0x01 (MBC1), rom_size=0x02 (128 KB = 8 banks), ram_size=0x00.
  uint8_t* rom = synthesise_rom(8, 0x01, 0x02, 0x00);
  Cart* c = cart_create();
  int rc = cart_load_from_buffer(c, rom, 8 * 0x4000);
  check_eq("cart_load_from_buffer rc", (uint32_t)rc, 0);

  // Bank 0 is always at 0x0000.
  check_eq("read 0x0000 -> bank 0", cart_read(c, 0x0000), 0x00);
  // Default high bank is 1.
  check_eq("read 0x4000 -> bank 1", cart_read(c, 0x4000), 0x01);

  cart_free(c);
  free(rom);
}

static void test_rom_bank_switch(void) {
  printf("[test_rom_bank_switch]\n");
  uint8_t* rom = synthesise_rom(8, 0x01, 0x02, 0x00);
  Cart* c = cart_create();
  cart_load_from_buffer(c, rom, 8 * 0x4000);

  // Switch to bank 3, read.
  cart_write(c, 0x2000, 0x03);
  check_eq("after write 0x2000 = 3 -> read 0x4000 = bank 3",
           cart_read(c, 0x4000), 0x03);

  // Switch to bank 5.
  cart_write(c, 0x2000, 0x05);
  check_eq("after write 0x2000 = 5 -> read 0x4000 = bank 5",
           cart_read(c, 0x4000), 0x05);

  // Bank 0 still visible at 0x0000.
  check_eq("read 0x0000 still bank 0 in mode 0", cart_read(c, 0x0000), 0x00);

  cart_free(c);
  free(rom);
}

static void test_zero_to_one_quirk(void) {
  printf("[test_zero_to_one_quirk]\n");
  uint8_t* rom = synthesise_rom(8, 0x01, 0x02, 0x00);
  Cart* c = cart_create();
  cart_load_from_buffer(c, rom, 8 * 0x4000);

  // Writing 0 should select bank 1, not bank 0.
  cart_write(c, 0x2000, 0x00);
  check_eq("write 0x2000 = 0 -> reads bank 1 at 0x4000",
           cart_read(c, 0x4000), 0x01);

  cart_free(c);
  free(rom);
}

static void test_bank_mask(void) {
  printf("[test_bank_mask]\n");
  // Only 4 banks (64 KB). rom_size_code=0x01 (-> 64 KB).
  uint8_t* rom = synthesise_rom(4, 0x01, 0x01, 0x00);
  Cart* c = cart_create();
  cart_load_from_buffer(c, rom, 4 * 0x4000);

  // mask should be 0x03. Writing 5 should mask down to 1.
  cart_write(c, 0x2000, 0x05);
  check_eq("write 5 to 4-bank ROM masks to bank 1",
           cart_read(c, 0x4000), 0x01);
  cart_write(c, 0x2000, 0x07);
  check_eq("write 7 to 4-bank ROM masks to bank 3",
           cart_read(c, 0x4000), 0x03);

  cart_free(c);
  free(rom);
}

static void test_ram_enable_gating(void) {
  printf("[test_ram_enable_gating]\n");
  // MBC1 + RAM (0x02 = MBC1+RAM, but MBC1+RAM+BATTERY = 0x03 also fine).
  // 64 KB ROM, 8 KB RAM.
  uint8_t* rom = synthesise_rom(4, 0x02, 0x01, 0x02);
  Cart* c = cart_create();
  cart_load_from_buffer(c, rom, 4 * 0x4000);

  // RAM is disabled by default. Writes ignored, reads return 0xFF.
  cart_write(c, 0xA000, 0x42);
  check_eq("RAM disabled: write ignored, read returns 0xFF",
           cart_read(c, 0xA000), 0xFF);

  // Enable RAM with 0x0A. Writes must succeed afterward.
  cart_write(c, 0x0000, 0x0A);
  cart_write(c, 0xA000, 0x42);
  check_eq("RAM enabled (0x0A): write 0x42 -> read 0x42",
           cart_read(c, 0xA000), 0x42);

  // Other low-nibble values (e.g. 0x0B, 0x00) must NOT enable.
  cart_write(c, 0x0000, 0x0B);
  cart_write(c, 0xA000, 0x99);
  check_eq("0x0B does NOT enable RAM (read returns 0xFF)",
           cart_read(c, 0xA000), 0xFF);

  // Re-enable, write, then disable: data should be retained but inaccessible.
  cart_write(c, 0x0000, 0x0A);
  cart_write(c, 0xA000, 0x77);
  cart_write(c, 0x0000, 0x00);  // disable
  check_eq("disable after write: read returns 0xFF",
           cart_read(c, 0xA000), 0xFF);
  cart_write(c, 0x0000, 0x0A);  // re-enable
  check_eq("re-enable: data persisted (read 0x77)",
           cart_read(c, 0xA000), 0x77);

  cart_free(c);
  free(rom);
}

static void test_mode1_high_register_lower_bank(void) {
  printf("[test_mode1_high_register_lower_bank]\n");
  // 1 MB ROM (64 banks). Need bank 0x20 (the first bank reachable
  // only via mode 1) to be tagged so we can verify it.
  // rom_size_code=0x05 -> 1 MB.
  uint8_t* rom = synthesise_rom(64, 0x01, 0x05, 0x00);
  Cart* c = cart_create();
  cart_load_from_buffer(c, rom, 64 * 0x4000);

  // Switch high bank to 1, set bank_lo = 0 (-> 1 with quirk),
  // mode 0 first to confirm 0x0000 is still bank 0.
  cart_write(c, 0x4000, 0x01);  // bank_hi = 1
  check_eq("mode 0 with bank_hi=1: 0x0000 still bank 0",
           cart_read(c, 0x0000), 0x00);

  // Now enable mode 1. 0x0000-0x3FFF should reflect (bank_hi << 5) = bank 0x20.
  cart_write(c, 0x6000, 0x01);  // mode = 1
  check_eq("mode 1 with bank_hi=1: 0x0000 -> bank 0x20",
           cart_read(c, 0x0000), 0x20);

  // Upper bank is (bank_hi << 5) | bank_lo. With bank_lo defaulting to
  // 1, upper bank should be 0x21.
  check_eq("mode 1 with bank_hi=1, bank_lo=1: 0x4000 -> bank 0x21",
           cart_read(c, 0x4000), 0x21);

  // Set bank_lo to 5: upper bank = 0x25.
  cart_write(c, 0x2000, 0x05);
  check_eq("mode 1 bank_hi=1 bank_lo=5: 0x4000 -> bank 0x25",
           cart_read(c, 0x4000), 0x25);

  cart_free(c);
  free(rom);
}

static void test_high_bank_combination_in_mode_zero(void) {
  printf("[test_high_bank_combination_in_mode_zero]\n");
  // 2 MB ROM (128 banks, mask 0x7F). Even in mode 0 the upper bank
  // uses bank_hi. We need at least bank 0x45 to exist, so >= 70 banks
  // -- 128 is the next standard size.
  // rom_size_code=0x06 -> 2 MB.
  uint8_t* rom = synthesise_rom(128, 0x01, 0x06, 0x00);
  Cart* c = cart_create();
  cart_load_from_buffer(c, rom, 128 * 0x4000);

  // Set bank_hi = 2, bank_lo = 5 -> upper bank = 0x45.
  cart_write(c, 0x4000, 0x02);
  cart_write(c, 0x2000, 0x05);
  check_eq("mode 0 bank_hi=2 bank_lo=5: 0x4000 -> bank 0x45",
           cart_read(c, 0x4000), 0x45);

  // bank_lo = 0 with quirk -> bank 0x41.
  cart_write(c, 0x2000, 0x00);
  check_eq("mode 0 bank_hi=2 bank_lo=0 (quirk): 0x4000 -> bank 0x41",
           cart_read(c, 0x4000), 0x41);

  // Lower bank still 0 in mode 0 regardless of bank_hi.
  check_eq("mode 0 bank_hi=2: 0x0000 still bank 0",
           cart_read(c, 0x0000), 0x00);

  cart_free(c);
  free(rom);
}

// =====================================================================
// main
// =====================================================================

int main(void) {
  test_default_state();
  test_rom_bank_switch();
  test_zero_to_one_quirk();
  test_bank_mask();
  test_ram_enable_gating();
  test_mode1_high_register_lower_bank();
  test_high_bank_combination_in_mode_zero();

  if (g_failures == 0) {
    printf("\nAll MBC1 tests passed.\n");
    return 0;
  } else {
    printf("\n%d MBC1 test(s) FAILED.\n", g_failures);
    return 1;
  }
}