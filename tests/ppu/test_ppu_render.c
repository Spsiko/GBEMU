/**
 * @file test_ppu_render.c
 * @brief End-to-end render test: builds a tiny ROM that sets up BG
 *        tile data and a tile map, runs it through the emu, and
 *        checks that the resulting framebuffer matches expectations.
 *
 * This is the "the renderer actually produces pixels" test, distinct
 * from test_ppu_timing which only verifies the state machine. Doesn't
 * need dmg-acid2; uses a synthesized ROM with a known pattern.
 *
 * Build:
 *   gcc -std=c11 -Wall -Wextra -O0 -g -I../.. \
 *     test_ppu_render.c \
 *     ../../emu.c ../../bus.c ../../timer.c ../../serial.c \
 *     ../../ppu.c ../../cart.c ../../cpu.c ../../cpu_ops.c \
 *     -o test_ppu_render
 */

#include "../../include/emu.h"
#include "../../include/cart.h"
#include "../../include/ppu.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>

static const uint8_t kNintendoLogo[48] = {
  0xCE,0xED,0x66,0x66,0xCC,0x0D,0x00,0x0B,0x03,0x73,0x00,0x83,0x00,0x0C,0x00,0x0D,
  0x00,0x08,0x11,0x1F,0x88,0x89,0x00,0x0E,0xDC,0xCC,0x6E,0xE6,0xDD,0xDD,0xD9,0x99,
  0xBB,0xBB,0x67,0x63,0x6E,0x0E,0xEC,0xCC,0xDD,0xDC,0x99,0x9F,0xBB,0xB9,0x33,0x3E,
};

static int g_failures = 0;

static void check_eq(const char* label, uint32_t actual, uint32_t expected) {
  if (actual == expected) {
    printf("  PASS: %s\n", label);
  } else {
    printf("  FAIL: %s (got 0x%X, expected 0x%X)\n",
           label, actual, expected);
    g_failures++;
  }
}

// Helper: emit instruction bytes into the ROM at *pc, advancing pc.
static void emit(uint8_t* rom, uint16_t* pc, int n, ...) {
  va_list ap;
  va_start(ap, n);
  for (int i = 0; i < n; i++) {
    rom[(*pc)++] = (uint8_t)va_arg(ap, int);
  }
  va_end(ap);
}

// Build a 32 KB ROM that:
//   1. Disables LCD by writing 0 to 0xFF40.
//   2. Loads a tile into VRAM 0x8000 with pattern bytes lo[8], hi[8].
//   3. Fills the BG tile map at 0x9800 with tile index 0.
//   4. Sets BGP to 0xE4 (identity palette: 3,2,1,0).
//   5. Sets SCY=SCX=0, WY=WX=0, but window disabled.
//   6. Enables LCD with LCDC=0x91 (LCD+BG on, tile data 0x8000,
//      tile map 0x9800, window off).
//   7. Halts in an infinite loop (JR -2).
static void build_rom(uint8_t* rom,
                      const uint8_t lo[8], const uint8_t hi[8]) {
  // Header.
  memset(rom, 0, 0x8000);
  rom[0x0100] = 0x00;  // NOP
  rom[0x0101] = 0xC3;  // JP 0x0150
  rom[0x0102] = 0x50;
  rom[0x0103] = 0x01;
  memcpy(&rom[0x0104], kNintendoLogo, 48);
  rom[0x0147] = 0x00;  // MBC: none
  rom[0x0148] = 0x00;  // ROM size: 32 KB
  rom[0x0149] = 0x00;  // RAM size: none

  uint16_t pc = 0x0150;

  // Disable LCD: LD A, 0; LDH (0x40), A.
  emit(rom, &pc, 4, 0x3E, 0x00, 0xE0, 0x40);

  // Load tile into 0x8000. Each row: write low byte then high byte.
  // Use HL = 0x8000.
  emit(rom, &pc, 3, 0x21, 0x00, 0x80);  // LD HL, 0x8000
  for (int row = 0; row < 8; row++) {
    // LD (HL+), A with A = lo[row].
    emit(rom, &pc, 2, 0x3E, lo[row]);   // LD A, lo[row]
    emit(rom, &pc, 1, 0x22);            // LD (HL+), A
    emit(rom, &pc, 2, 0x3E, hi[row]);   // LD A, hi[row]
    emit(rom, &pc, 1, 0x22);            // LD (HL+), A
  }

  // Fill BG tile map at 0x9800 with 0 for all 32*32 = 1024 entries.
  // Loop body uses LD A,B / OR C to test BC==0, which clobbers A,
  // so we re-load A=0 each iteration before the LD (HL+),A.
  emit(rom, &pc, 3, 0x21, 0x00, 0x98);  // LD HL, 0x9800
  emit(rom, &pc, 3, 0x01, 0x00, 0x04);  // LD BC, 0x0400
  // Loop start.
  uint16_t loop_start = pc;
  emit(rom, &pc, 2, 0x3E, 0x00);        // LD A, 0   (re-load each iter)
  emit(rom, &pc, 1, 0x22);              // LD (HL+), A
  emit(rom, &pc, 1, 0x0B);              // DEC BC
  emit(rom, &pc, 1, 0x78);              // LD A, B
  emit(rom, &pc, 1, 0xB1);              // OR C
  // JR NZ, e: e is signed offset from PC after the JR instruction.
  int8_t off = (int8_t)((int)loop_start - (int)(pc + 2));
  emit(rom, &pc, 2, 0x20, (uint8_t)off);  // JR NZ, off

  // BGP = 0xE4 (identity).
  emit(rom, &pc, 4, 0x3E, 0xE4, 0xE0, 0x47);

  // SCY=0, SCX=0.
  emit(rom, &pc, 4, 0x3E, 0x00, 0xE0, 0x42);
  emit(rom, &pc, 4, 0x3E, 0x00, 0xE0, 0x43);

  // Enable LCD: LCDC = 0x91 (bit 7 = LCD on, bit 4 = tile data 0x8000,
  // bit 0 = BG enable; bits 6/5/3 = 0 means window off, BG tilemap 0x9800).
  emit(rom, &pc, 4, 0x3E, 0x91, 0xE0, 0x40);

  // Infinite loop: JR -2.
  emit(rom, &pc, 2, 0x18, 0xFE);

  // Header checksum.
  uint8_t sum = 0;
  for (uint16_t a = 0x0134; a <= 0x014C; a++) {
    sum -= rom[a] + 1;
  }
  rom[0x014D] = sum;
}

int main(void) {
  // We'll test with a tile that produces 8 distinct color indices in
  // a horizontal stripe pattern: row 0 = color 0, row 1 = color 1, ...
  //
  // For row N, color index N (0..3). Wait, we have 4 colors and 8 rows.
  // Use colors 0,1,2,3,0,1,2,3 down the rows.
  //
  // For a single row to produce color index C (0-3) for every pixel:
  //   if C bit 0 set -> low byte = 0xFF
  //   if C bit 1 set -> high byte = 0xFF
  //
  // So:  C=0 -> lo=0x00, hi=0x00
  //      C=1 -> lo=0xFF, hi=0x00
  //      C=2 -> lo=0x00, hi=0xFF
  //      C=3 -> lo=0xFF, hi=0xFF
  uint8_t row_color[8] = {0, 1, 2, 3, 0, 1, 2, 3};
  uint8_t lo[8], hi[8];
  for (int i = 0; i < 8; i++) {
    uint8_t c = row_color[i];
    lo[i] = (c & 1) ? 0xFF : 0x00;
    hi[i] = (c & 2) ? 0xFF : 0x00;
  }

  // Build ROM into a buffer, hand it to cart_load_from_buffer.
  uint8_t rom[0x8000];
  build_rom(rom, lo, hi);

  Emu* e = emu_create();
  if (e == NULL) {
    fprintf(stderr, "emu_create failed\n");
    return 1;
  }

  // We need to load via cart_load_from_buffer, not the file path
  // version. emu_load_rom uses cart_load (file). Bypass: we can't
  // get at the cart from outside. Easiest fix is to write the ROM
  // to a temp file and use emu_load_rom -- but mkstemp etc. are not
  // portable. Solution: use cart_load_from_buffer directly. This
  // means we need access to the cart, which we get via... we don't
  // have that accessor on Emu. Add one? For test only? Or just write
  // the file?
  //
  // Cheat: use tmpnam (deprecated but portable). Better: write to a
  // fixed path in the current dir.

  const char* tmp_path = "test_ppu_render_rom.gb";
  FILE* fp = fopen(tmp_path, "wb");
  if (fp == NULL) { fprintf(stderr, "fopen failed\n"); emu_destroy(e); return 1; }
  if (fwrite(rom, 1, sizeof(rom), fp) != sizeof(rom)) {
    fprintf(stderr, "fwrite failed\n");
    fclose(fp); emu_destroy(e); return 1;
  }
  fclose(fp);

  int rc = emu_load_rom(e, tmp_path);
  if (rc != 0) {
    fprintf(stderr, "load rc=%d\n", rc);
    remove(tmp_path);
    emu_destroy(e);
    return 1;
  }

  // Run for a couple of frames so the ROM has time to set everything
  // up and complete one full PPU frame.
  for (int i = 0; i < 3; i++) emu_run_frame(e);

  // Now check the framebuffer. The tile is 8 rows, each row is one
  // solid color (0,1,2,3,0,1,2,3 going down). The tile map repeats
  // it across the whole screen, so:
  //   line 0 -> color 0
  //   line 1 -> color 1
  //   line 2 -> color 2
  //   line 3 -> color 3
  //   line 4 -> color 0
  //   ...
  //
  // Through BGP=0xE4 (identity), color index N -> grayscale N.
  const uint8_t* fb = emu_framebuffer(e);

  for (int line = 0; line < 8; line++) {
    uint8_t expected = (uint8_t)(line & 3);
    char label[64];
    snprintf(label, sizeof(label),
             "line %d, x=0 should be color %d", line, expected);
    check_eq(label, fb[line * PPU_LCD_WIDTH + 0], expected);
    snprintf(label, sizeof(label),
             "line %d, x=159 should be color %d", line, expected);
    check_eq(label, fb[line * PPU_LCD_WIDTH + 159], expected);
  }

  // Spot-check a few rows further down, where the tile repeats.
  // Line 9 = tile-row 1 = color 1. Line 100 = tile-row 4 = color 0.
  check_eq("line 9, x=80 should be color 1",
           fb[9 * PPU_LCD_WIDTH + 80], 1);
  check_eq("line 100, x=80 should be color 0",
           fb[100 * PPU_LCD_WIDTH + 80], 0);
  check_eq("line 143, x=159 (last visible pixel) should be color 7%4 = 3",
           fb[143 * PPU_LCD_WIDTH + 159], (143 & 7) & 3);
  // Wait, line 143 % 8 = 7, and row_color[7] = 3. So expect 3.

  remove(tmp_path);
  emu_destroy(e);

  if (g_failures == 0) {
    printf("\nAll PPU render tests passed.\n");
    return 0;
  } else {
    printf("\n%d PPU render test(s) FAILED.\n", g_failures);
    return 1;
  }
}