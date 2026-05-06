/**
 * @file test_ppu_sprites.c
 * @brief Standalone tests for sprite rendering (Batch C).
 *
 * Builds against bus + ppu directly. Each test:
 *   1. Disables the LCD (LCDC = 0) so we can write VRAM/OAM freely.
 *   2. Uses ppu_poke_vram / ppu_poke_oam to load tile data and OAM.
 *   3. Sets palettes (BGP, OBP0, OBP1) via ppu_write_reg.
 *   4. Enables the LCD with sprites on.
 *   5. Ticks the bus for one full frame.
 *   6. Inspects the framebuffer.
 *
 * Build:
 *   gcc -std=c11 -Wall -Wextra -O0 -g -I../.. \
 *     test_ppu_sprites.c \
 *     ../../bus.c ../../timer.c ../../serial.c ../../ppu.c ../../cart.c \
 *     -o test_ppu_sprites
 */

#include "../../include/bus.h"
#include "../../include/ppu.h"
#include "../../include/cart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// =====================================================================
// Test harness
// =====================================================================

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

// Run the bus for one full frame's worth of T-cycles.
static void run_one_frame(Bus* b) {
  // 70224 T-cycles per frame, but sys_tick takes M-cycles.
  // 70224 / 4 = 17556 M-cycles per frame.
  sys_tick(b, 17556);
}

// =====================================================================
// Setup helpers
// =====================================================================

// Reset PPU state into a known-disabled configuration. Tests build on
// top of this: load tiles + OAM + palettes, then enable LCD.
static void setup_ppu_disabled(Bus* b) {
  Ppu* p = bus_ppu(b);
  // Disable LCD via the register write (which resets LY/mode/etc.).
  ppu_write_reg(p, 0xFF40, 0x00);
  // Wipe VRAM and OAM.
  for (int i = 0; i < 0x2000; i++) ppu_poke_vram(p, (uint16_t)(0x8000 + i), 0);
  for (int i = 0; i < 0xA0;   i++) ppu_poke_oam (p, (uint16_t)(0xFE00 + i), 0);
}

// Write 16 bytes (one 8x8 tile) starting at the given VRAM address.
// Tile data layout: 2 bytes per row, low-bitplane then high-bitplane.
// Row N's bit M (0=rightmost) defines pixel M of that row:
//   color_index = (high_bit << 1) | low_bit.
static void write_tile(Bus* b, uint16_t addr, const uint8_t* bytes) {
  Ppu* p = bus_ppu(b);
  for (int i = 0; i < 16; i++) {
    ppu_poke_vram(p, (uint16_t)(addr + i), bytes[i]);
  }
}

// A solid-color tile: every pixel is `color`. Returns 16 bytes via
// the supplied buffer.
static void fill_solid_tile(uint8_t buf[16], uint8_t color) {
  uint8_t lo = (color & 1) ? 0xFF : 0x00;
  uint8_t hi = (color & 2) ? 0xFF : 0x00;
  for (int row = 0; row < 8; row++) {
    buf[row * 2 + 0] = lo;
    buf[row * 2 + 1] = hi;
  }
}

// Write one OAM entry (4 bytes).
static void write_oam_entry(Bus* b, int index,
                            uint8_t y, uint8_t x,
                            uint8_t tile, uint8_t attr) {
  Ppu* p = bus_ppu(b);
  ppu_poke_oam(p, (uint16_t)(0xFE00 + index * 4 + 0), y);
  ppu_poke_oam(p, (uint16_t)(0xFE00 + index * 4 + 1), x);
  ppu_poke_oam(p, (uint16_t)(0xFE00 + index * 4 + 2), tile);
  ppu_poke_oam(p, (uint16_t)(0xFE00 + index * 4 + 3), attr);
}

// Get a pixel from the framebuffer at (x, y).
static uint8_t pixel_at(Bus* b, int x, int y) {
  return ppu_framebuffer(bus_ppu(b))[y * PPU_LCD_WIDTH + x];
}

// =====================================================================
// Test cases
// =====================================================================

static void test_basic_sprite(Bus* b) {
  printf("[test_basic_sprite]\n");
  setup_ppu_disabled(b);

  // Tile 0 at 0x8000: solid color 1.
  uint8_t tile[16];
  fill_solid_tile(tile, 1);
  write_tile(b, 0x8000, tile);

  // Place sprite 0 at screen position (8, 16): OAM Y = 16+16 = 32, X = 8+8 = 16.
  // Tile index 0, attr 0 (no flips, OBP0, no priority).
  write_oam_entry(b, 0, 32, 16, 0, 0x00);

  // Identity palettes.
  Ppu* p = bus_ppu(b);
  ppu_write_reg(p, 0xFF47, 0xE4);   // BGP
  ppu_write_reg(p, 0xFF48, 0xE4);   // OBP0
  ppu_write_reg(p, 0xFF49, 0xE4);   // OBP1

  // Enable LCD with sprites on but BG disabled, so we're testing
  // sprite output in isolation. (LCDC: bit 7 LCD on, bit 4 tile data
  // 0x8000, bit 1 OBJ enable. BG enable bit 0 = 0.)
  ppu_write_reg(p, 0xFF40, 0x92);

  run_one_frame(b);

  // Sprite is at screen X=8..15, Y=16..23. Color 1 throughout.
  check_eq("pixel inside sprite (10, 18) = color 1", pixel_at(b, 10, 18), 1);
  check_eq("pixel at sprite top-left (8, 16) = color 1", pixel_at(b, 8, 16), 1);
  check_eq("pixel at sprite bottom-right (15, 23) = color 1", pixel_at(b, 15, 23), 1);

  // Just outside the sprite: BG disabled so blank (color 0).
  check_eq("pixel left of sprite (7, 18) = color 0", pixel_at(b, 7, 18), 0);
  check_eq("pixel right of sprite (16, 18) = color 0", pixel_at(b, 16, 18), 0);
  check_eq("pixel above sprite (10, 15) = color 0", pixel_at(b, 10, 15), 0);
  check_eq("pixel below sprite (10, 24) = color 0", pixel_at(b, 10, 24), 0);
}

static void test_sprite_color_zero_transparent(Bus* b) {
  printf("[test_sprite_color_zero_transparent]\n");
  setup_ppu_disabled(b);

  // Tile 0 at 0x8000: row pattern. We'll make col 0..3 = color 0
  // (transparent) and col 4..7 = color 2.
  uint8_t tile[16] = {0};
  for (int row = 0; row < 8; row++) {
    // For col 4-7 (right half), color 2: low bitplane has those bits=0,
    // high bitplane has those bits=1.
    // Bit positions in the byte: bit 7 = leftmost (col 0), bit 0 = col 7.
    // So col 4-7 = bits 3-0 of the byte.
    tile[row * 2 + 0] = 0x00;        // low plane: all 0 -> low bit clear
    tile[row * 2 + 1] = 0x0F;        // high plane: bits 3-0 set -> col 4-7
  }
  write_tile(b, 0x8000, tile);

  // BG tile 1 at 0x8010: solid color 1 (so we can see if sprite covers it).
  uint8_t bg_tile[16];
  fill_solid_tile(bg_tile, 1);
  write_tile(b, 0x8010, bg_tile);

  // BG tile map: tile 1 everywhere visible.
  Ppu* p = bus_ppu(b);
  for (int i = 0; i < 1024; i++) {
    ppu_poke_vram(p, (uint16_t)(0x9800 + i), 1);
  }

  // Sprite at (8, 16) using tile 0.
  write_oam_entry(b, 0, 32, 16, 0, 0x00);

  ppu_write_reg(p, 0xFF47, 0xE4);
  ppu_write_reg(p, 0xFF48, 0xE4);
  ppu_write_reg(p, 0xFF40, 0x93);

  run_one_frame(b);

  // Left half of sprite area: sprite color 0 = transparent -> BG color 1.
  check_eq("transparent pixel in sprite (8, 18) shows BG color 1",
           pixel_at(b, 8, 18), 1);
  check_eq("transparent pixel in sprite (11, 18) shows BG color 1",
           pixel_at(b, 11, 18), 1);
  // Right half: sprite color 2.
  check_eq("opaque pixel in sprite (12, 18) shows sprite color 2",
           pixel_at(b, 12, 18), 2);
  check_eq("opaque pixel in sprite (15, 18) shows sprite color 2",
           pixel_at(b, 15, 18), 2);
}

static void test_x_flip(Bus* b) {
  printf("[test_x_flip]\n");
  setup_ppu_disabled(b);

  // Tile 0: leftmost 4 cols = color 1, rightmost 4 cols = color 0.
  uint8_t tile[16] = {0};
  for (int row = 0; row < 8; row++) {
    tile[row * 2 + 0] = 0xF0;   // low plane: bits 7-4 set (cols 0-3)
    tile[row * 2 + 1] = 0x00;
  }
  write_tile(b, 0x8000, tile);

  // Two sprites: one normal, one X-flipped, at different positions.
  write_oam_entry(b, 0, 32, 16, 0, 0x00);          // normal at X-screen 8
  write_oam_entry(b, 1, 32, 32, 0, 0x20);          // X-flipped at X-screen 24

  Ppu* p = bus_ppu(b);
  ppu_write_reg(p, 0xFF47, 0xE4);
  ppu_write_reg(p, 0xFF48, 0xE4);
  ppu_write_reg(p, 0xFF40, 0x92);

  run_one_frame(b);

  // Normal sprite at X 8..15: cols 0-3 (X 8-11) = color 1, cols 4-7 (X 12-15) = transparent.
  check_eq("normal sprite, leftmost pixel (8, 18) = color 1", pixel_at(b, 8, 18), 1);
  check_eq("normal sprite, col 3 pixel (11, 18) = color 1", pixel_at(b, 11, 18), 1);
  check_eq("normal sprite, col 4 pixel (12, 18) = color 0 (transparent -> BG 0)",
           pixel_at(b, 12, 18), 0);

  // X-flipped sprite at X 24..31: tile cols flipped, so original cols 0-3
  // (color 1) end up at cols 4-7 (X 28-31).
  check_eq("X-flipped sprite, leftmost pixel (24, 18) = color 0 (was transparent)",
           pixel_at(b, 24, 18), 0);
  check_eq("X-flipped sprite, rightmost pixel (31, 18) = color 1",
           pixel_at(b, 31, 18), 1);
  check_eq("X-flipped sprite, col 4 of screen (28, 18) = color 1",
           pixel_at(b, 28, 18), 1);
}

static void test_y_flip(Bus* b) {
  printf("[test_y_flip]\n");
  setup_ppu_disabled(b);

  // Tile 0: row 0 = color 1, rows 1-7 = color 0.
  uint8_t tile[16] = {0};
  tile[0] = 0xFF;  // row 0 low plane all 1
  tile[1] = 0x00;  // row 0 high plane all 0
  // rows 1-7 already 0 from memset.
  write_tile(b, 0x8000, tile);

  // Two sprites: one normal, one Y-flipped.
  write_oam_entry(b, 0, 32, 16, 0, 0x00);          // normal: top row = color 1
  write_oam_entry(b, 1, 32, 32, 0, 0x40);          // Y-flipped: bottom row = color 1

  Ppu* p = bus_ppu(b);
  ppu_write_reg(p, 0xFF47, 0xE4);
  ppu_write_reg(p, 0xFF48, 0xE4);
  ppu_write_reg(p, 0xFF40, 0x92);

  run_one_frame(b);

  // Normal: row 0 visible at screen Y=16, color 1; row 7 at Y=23, color 0.
  check_eq("normal sprite, top row (10, 16) = color 1", pixel_at(b, 10, 16), 1);
  check_eq("normal sprite, bottom row (10, 23) = color 0", pixel_at(b, 10, 23), 0);

  // Y-flipped: original row 7 (color 0) at top (Y=16); original row 0 (color 1) at bottom (Y=23).
  check_eq("Y-flipped sprite, top row (26, 16) = color 0", pixel_at(b, 26, 16), 0);
  check_eq("Y-flipped sprite, bottom row (26, 23) = color 1", pixel_at(b, 26, 23), 1);
}

static void test_sprite_priority_smaller_x_wins(Bus* b) {
  printf("[test_sprite_priority_smaller_x_wins]\n");
  setup_ppu_disabled(b);

  // Tile 0: solid color 1. Tile 1: solid color 2.
  uint8_t t1[16], t2[16];
  fill_solid_tile(t1, 1);
  fill_solid_tile(t2, 2);
  write_tile(b, 0x8000, t1);
  write_tile(b, 0x8010, t2);

  // Two sprites overlapping at X=12:
  //   Sprite A: OAM idx 0, X=14 (screen X=6..13), tile 0 (color 1)
  //   Sprite B: OAM idx 1, X=10 (screen X=2..9 ... wait that doesn't overlap)
  // Let me redo: sprites overlap if their X ranges share pixels.
  //   Sprite A at OAM-X=20 -> screen X 12..19, color 1.
  //   Sprite B at OAM-X=16 -> screen X 8..15, color 2.
  // They overlap at screen X=12..15. B has smaller X, so B (color 2) wins
  // at X=12..15.
  write_oam_entry(b, 0, 32, 20, 0, 0x00);   // A: tile 0 = color 1
  write_oam_entry(b, 1, 32, 16, 1, 0x00);   // B: tile 1 = color 2

  Ppu* p = bus_ppu(b);
  ppu_write_reg(p, 0xFF47, 0xE4);
  ppu_write_reg(p, 0xFF48, 0xE4);
  ppu_write_reg(p, 0xFF40, 0x92);

  run_one_frame(b);

  // X=8..11 only B is there: color 2.
  check_eq("only B at (10, 18) = color 2 (B alone)", pixel_at(b, 10, 18), 2);
  // X=12..15 both: B wins -> color 2.
  check_eq("overlap (13, 18) = color 2 (smaller X wins)", pixel_at(b, 13, 18), 2);
  // X=16..19 only A: color 1.
  check_eq("only A at (17, 18) = color 1 (A alone)", pixel_at(b, 17, 18), 1);
}

static void test_sprite_priority_oam_index_tiebreak(Bus* b) {
  printf("[test_sprite_priority_oam_index_tiebreak]\n");
  setup_ppu_disabled(b);

  uint8_t t1[16], t2[16];
  fill_solid_tile(t1, 1);
  fill_solid_tile(t2, 2);
  write_tile(b, 0x8000, t1);
  write_tile(b, 0x8010, t2);

  // Two sprites at the same X. OAM index 0 wins.
  // OAM idx 0 -> tile 0 (color 1).
  // OAM idx 1 -> tile 1 (color 2). Same X.
  write_oam_entry(b, 0, 32, 16, 0, 0x00);
  write_oam_entry(b, 1, 32, 16, 1, 0x00);

  Ppu* p = bus_ppu(b);
  ppu_write_reg(p, 0xFF47, 0xE4);
  ppu_write_reg(p, 0xFF48, 0xE4);
  ppu_write_reg(p, 0xFF40, 0x92);

  run_one_frame(b);

  check_eq("same X tiebreak (12, 18) = color 1 (OAM idx 0 wins)",
           pixel_at(b, 12, 18), 1);
}

static void test_obj_to_bg_priority(Bus* b) {
  printf("[test_obj_to_bg_priority]\n");
  setup_ppu_disabled(b);

  // Sprite tile: solid color 2.
  uint8_t st[16];
  fill_solid_tile(st, 2);
  write_tile(b, 0x8000, st);

  // BG tiles:
  //   Tile 1: solid color 0 (transparent BG, sprite should always show).
  //   Tile 2: solid color 1 (non-zero BG, sprite hidden if priority bit set).
  uint8_t bg0[16], bg1[16];
  fill_solid_tile(bg0, 0);
  fill_solid_tile(bg1, 1);
  write_tile(b, 0x8010, bg0);
  write_tile(b, 0x8020, bg1);

  // BG tile map: half tile 1 (color 0), half tile 2 (color 1).
  // Use tile index 1 for the left half (cols 0-19 of the screen / cols 0-9 of map),
  // tile 2 for the right half.
  Ppu* p = bus_ppu(b);
  for (int my = 0; my < 32; my++) {
    for (int mx = 0; mx < 32; mx++) {
      uint8_t idx = (mx < 10) ? 1 : 2;
      ppu_poke_vram(p, (uint16_t)(0x9800 + my * 32 + mx), idx);
    }
  }

  // Sprite with priority bit set, spans cols 6..13 (so straddles the BG boundary at x=80).
  // Wait, BG tile 1 at map cols 0-9 covers screen X 0-79, BG tile 2 at 80-159.
  // Place sprite at OAM-X = 84 -> screen X 76-83. So 76-79 over BG color 0, 80-83 over BG color 1.
  write_oam_entry(b, 0, 32, 84, 0, 0x80);  // priority bit set

  ppu_write_reg(p, 0xFF47, 0xE4);
  ppu_write_reg(p, 0xFF48, 0xE4);
  ppu_write_reg(p, 0xFF40, 0x93);

  run_one_frame(b);

  // X 76-79: BG was color 0, sprite shows -> color 2.
  check_eq("priority sprite over BG=0 (78, 18) = sprite color 2",
           pixel_at(b, 78, 18), 2);
  // X 80-83: BG was color 1, priority bit set, sprite hidden -> BG color 1.
  check_eq("priority sprite over BG=1 (81, 18) = BG color 1 (sprite hidden)",
           pixel_at(b, 81, 18), 1);
}

static void test_8x16_mode(Bus* b) {
  printf("[test_8x16_mode]\n");
  setup_ppu_disabled(b);

  // Tile 0: color 1. Tile 1: color 2. In 8x16 mode the sprite uses
  // tile 0 for the top half and tile 1 for the bottom half.
  uint8_t t0[16], t1[16];
  fill_solid_tile(t0, 1);
  fill_solid_tile(t1, 2);
  write_tile(b, 0x8000, t0);
  write_tile(b, 0x8010, t1);

  // Sprite at (8, 16) using tile 0 (which becomes 0,1 in 8x16 mode).
  write_oam_entry(b, 0, 32, 16, 0, 0x00);

  Ppu* p = bus_ppu(b);
  ppu_write_reg(p, 0xFF47, 0xE4);
  ppu_write_reg(p, 0xFF48, 0xE4);
  // LCDC: bit 7 LCD on, bit 4 tile data 0x8000, bit 2 OBJ_SIZE 8x16,
  // bit 1 OBJ enable. BG disabled to isolate sprite output.
  ppu_write_reg(p, 0xFF40, 0x96);

  run_one_frame(b);

  // Top 8 lines (Y 16-23) use tile 0 = color 1.
  check_eq("8x16 top half (10, 16) = color 1", pixel_at(b, 10, 16), 1);
  check_eq("8x16 top half (10, 23) = color 1", pixel_at(b, 10, 23), 1);
  // Bottom 8 lines (Y 24-31) use tile 1 = color 2.
  check_eq("8x16 bottom half (10, 24) = color 2", pixel_at(b, 10, 24), 2);
  check_eq("8x16 bottom half (10, 31) = color 2", pixel_at(b, 10, 31), 2);
}

static void test_obp1_palette(Bus* b) {
  printf("[test_obp1_palette]\n");
  setup_ppu_disabled(b);

  // Tile 0: solid color 1.
  uint8_t tile[16];
  fill_solid_tile(tile, 1);
  write_tile(b, 0x8000, tile);

  // Sprite using OBP1 (attr bit 4).
  write_oam_entry(b, 0, 32, 16, 0, 0x10);

  Ppu* p = bus_ppu(b);
  ppu_write_reg(p, 0xFF47, 0xE4);   // BGP identity
  ppu_write_reg(p, 0xFF48, 0xE4);   // OBP0 identity
  ppu_write_reg(p, 0xFF49, 0x4E);   // OBP1: index 1 -> color 3 (low bits 11)
                                    // 0b01001110: idx0=2, idx1=3, idx2=0, idx3=1
                                    // We just need idx 1 -> 3.
  ppu_write_reg(p, 0xFF40, 0x92);

  run_one_frame(b);

  check_eq("sprite using OBP1: tile color 1 -> palette mapped to 3",
           pixel_at(b, 10, 18), 3);
}

static void test_x_clipping(Bus* b) {
  printf("[test_x_clipping]\n");
  setup_ppu_disabled(b);

  // Sprite tile: solid color 1.
  uint8_t tile[16];
  fill_solid_tile(tile, 1);
  write_tile(b, 0x8000, tile);

  // Sprite with OAM-X = 1: screen X = -7..0 (only the rightmost pixel visible at X=0).
  write_oam_entry(b, 0, 32, 1, 0, 0x00);

  Ppu* p = bus_ppu(b);
  ppu_write_reg(p, 0xFF47, 0xE4);
  ppu_write_reg(p, 0xFF48, 0xE4);
  ppu_write_reg(p, 0xFF40, 0x92);

  run_one_frame(b);

  check_eq("partially off-screen left: rightmost pixel at (0, 18) = color 1",
           pixel_at(b, 0, 18), 1);
  // X=1 should be BG (color 0).
  check_eq("just past sprite right edge: (1, 18) = color 0",
           pixel_at(b, 1, 18), 0);
}

static void test_ten_sprite_limit(Bus* b) {
  printf("[test_ten_sprite_limit]\n");
  setup_ppu_disabled(b);

  // Tile 0: solid color 1.
  uint8_t tile[16];
  fill_solid_tile(tile, 1);
  write_tile(b, 0x8000, tile);

  // Place 11 sprites all on the same line (Y=32). Each at a distinct
  // X so they don't overlap. Sprites 0-9 should draw, sprite 10 must not.
  for (int i = 0; i < 11; i++) {
    uint8_t x_pos = (uint8_t)(8 + i * 8);  // X=8, 16, 24, ..., 88
    write_oam_entry(b, i, 32, x_pos, 0, 0x00);
  }

  Ppu* p = bus_ppu(b);
  ppu_write_reg(p, 0xFF47, 0xE4);
  ppu_write_reg(p, 0xFF48, 0xE4);
  ppu_write_reg(p, 0xFF40, 0x92);

  run_one_frame(b);

  // Sprite 0 at screen X=0..7, sprite 1 at 8..15, ..., sprite 9 at 72..79.
  // Sprite 10 (the 11th) would be at 80..87 but is dropped.
  check_eq("sprite 0 visible (4, 18) = color 1", pixel_at(b, 4, 18), 1);
  check_eq("sprite 9 visible (75, 18) = color 1", pixel_at(b, 75, 18), 1);
  check_eq("sprite 10 (11th) NOT drawn (84, 18) = color 0",
           pixel_at(b, 84, 18), 0);
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

  test_basic_sprite(bus);
  test_sprite_color_zero_transparent(bus);
  test_x_flip(bus);
  test_y_flip(bus);
  test_sprite_priority_smaller_x_wins(bus);
  test_sprite_priority_oam_index_tiebreak(bus);
  test_obj_to_bg_priority(bus);
  test_8x16_mode(bus);
  test_obp1_palette(bus);
  test_x_clipping(bus);
  test_ten_sprite_limit(bus);

  bus_destroy(bus);
  cart_free(cart);

  if (g_failures == 0) {
    printf("\nAll sprite tests passed.\n");
    return 0;
  } else {
    printf("\n%d sprite test(s) FAILED.\n", g_failures);
    return 1;
  }
}