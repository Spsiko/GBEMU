#include "ppu.h"
#include "bus.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// =====================================================================
// Constants
// =====================================================================

#define VRAM_SIZE  0x2000
#define OAM_SIZE   0x00A0

// Each visible scanline is 456 T-cycles, partitioned:
//   mode 2 (OAM scan):     T 0   - 79     (80 cycles)
//   mode 3 (pixel xfer):   T 80  - 251    (172 cycles, FIXED for Batch A)
//   mode 0 (HBlank):       T 252 - 455    (204 cycles)
// VBlank lines (LY 144-153) are entirely mode 1, 456 cycles each.
//
// TODO (Batch C): mode 3 length actually depends on sprite count and
// window activation on real hardware. Mooneye PPU timing tests fail
// with a fixed value, but no commercial DMG game and not dmg-acid2
// depend on the variation, so we keep mode 3 fixed at 172 for now.
#define DOTS_PER_LINE       456
#define MODE2_DOTS          80
#define MODE3_DOTS_FIXED    172
#define MODE3_END           (MODE2_DOTS + MODE3_DOTS_FIXED)  // 252
#define VBLANK_START_LINE   144
#define LINES_PER_FRAME     154

// LCDC bits.
#define LCDC_LCD_ENABLE        0x80
#define LCDC_WIN_TILEMAP       0x40
#define LCDC_WIN_ENABLE        0x20
#define LCDC_BG_TILEDATA       0x10
#define LCDC_BG_TILEMAP        0x08
#define LCDC_OBJ_SIZE          0x04
#define LCDC_OBJ_ENABLE        0x02
#define LCDC_BG_ENABLE         0x01

// STAT bits.
#define STAT_LYC_INT_ENABLE    0x40
#define STAT_M2_INT_ENABLE     0x20
#define STAT_M1_INT_ENABLE     0x10
#define STAT_M0_INT_ENABLE     0x08
#define STAT_LYC_EQUAL         0x04
#define STAT_MODE_MASK         0x03

// =====================================================================
// State
// =====================================================================

struct Ppu {
  Bus* bus;
  uint8_t vram[VRAM_SIZE];
  uint8_t oam[OAM_SIZE];

  // Registers.
  uint8_t lcdc;
  uint8_t stat;       // bits 0-2 are computed each tick; only 3-6 are user-writable
  uint8_t scy;
  uint8_t scx;
  uint8_t ly;         // current scanline (0-153)
  uint8_t lyc;
  uint8_t bgp;
  uint8_t obp0;
  uint8_t obp1;
  uint8_t wy;
  uint8_t wx;

  // State machine.
  unsigned dot;       // 0-455 within the current line
  uint8_t  mode;      // 0, 1, 2, or 3 (matches STAT bits 0-1)

  // Window line counter: advances by 1 on each visible scanline where
  // the window was actually drawn. Reset to 0 on VBlank entry. This
  // is NOT (LY - WY) because the window can be enabled/disabled
  // mid-frame; the counter only advances on scanlines where the
  // window was actually rendered.
  uint8_t window_line_counter;

  // STAT IRQ rising-edge detection. Real hardware ORs all four enable
  // sources into one wire; the IRQ fires only on the 0->1 transition.
  bool prev_stat_line;

  // Output. Always all zeros in Batch A.
  uint8_t framebuffer[PPU_LCD_HEIGHT * PPU_LCD_WIDTH];

  // Per-line scratch buffer of pre-palette BG/window color indices
  // (0-3). Filled by the BG/window pass and consulted by the sprite
  // pass to evaluate the OBJ-to-BG priority bit. The check is against
  // the BG COLOR INDEX, not the displayed grayscale -- if BGP maps
  // index 1 to white, a sprite with priority bit set still loses to
  // it (because the index, not the shade, is what matters).
  uint8_t bg_color_idx_line[PPU_LCD_WIDTH];
};

// =====================================================================
// Lifecycle
// =====================================================================

Ppu* ppu_create(Bus* bus) {
  Ppu* p = calloc(1, sizeof(Ppu));
  if (p == NULL) return NULL;
  p->bus = bus;
  return p;
}

void ppu_destroy(Ppu* p) {
  free(p);
}

void ppu_reset(Ppu* p) {
  memset(p->vram, 0, VRAM_SIZE);
  memset(p->oam, 0, OAM_SIZE);
  // Approximate post-bootrom values. Most ROMs immediately overwrite
  // these but the LCDC=0x91 default (LCD on, BG on, BG/window tile
  // data at 0x8000) is what real DMG ends up with.
  p->lcdc = 0x91;
  p->stat = 0x00;
  p->scy = p->scx = 0;
  p->ly = 0;
  p->lyc = 0;
  p->bgp = 0xFC;
  p->obp0 = 0xFF;
  p->obp1 = 0xFF;
  p->wy = p->wx = 0;
  p->dot = 0;
  p->mode = 2;       // visible line starts in mode 2
  p->window_line_counter = 0;
  p->prev_stat_line = false;
  memset(p->framebuffer, 0, sizeof(p->framebuffer));
}

// =====================================================================
// VRAM / OAM access (CPU-visible, mode-gated)
// =====================================================================
//
// During mode 3 the CPU cannot read VRAM (returns 0xFF, writes drop).
// During modes 2 AND 3 the CPU cannot read OAM. Modes 0 (HBlank) and
// 1 (VBlank) allow full access.

uint8_t ppu_read_vram(const Ppu* p, uint16_t addr) {
  if (p->mode == 3) return 0xFF;
  return p->vram[addr - 0x8000];
}

void ppu_write_vram(Ppu* p, uint16_t addr, uint8_t value) {
  if (p->mode == 3) return;
  p->vram[addr - 0x8000] = value;
}

uint8_t ppu_read_oam(const Ppu* p, uint16_t addr) {
  if (p->mode == 2 || p->mode == 3) return 0xFF;
  return p->oam[addr - 0xFE00];
}

void ppu_write_oam(Ppu* p, uint16_t addr, uint8_t value) {
  if (p->mode == 2 || p->mode == 3) return;
  p->oam[addr - 0xFE00] = value;
}

void ppu_poke_vram(Ppu* p, uint16_t addr, uint8_t value) {
  p->vram[addr - 0x8000] = value;
}

void ppu_poke_oam(Ppu* p, uint16_t addr, uint8_t value) {
  p->oam[addr - 0xFE00] = value;
}

// =====================================================================
// Register access
// =====================================================================

uint8_t ppu_read_reg(const Ppu* p, uint16_t addr) {
  switch (addr) {
    case 0xFF40: return p->lcdc;
    case 0xFF41: {
      // STAT: bit 7 always reads 1, bits 6-3 are user-writable (the
      // four interrupt enables), bit 2 is LY=LYC, bits 1-0 are mode.
      uint8_t lyc_eq = (p->ly == p->lyc) ? STAT_LYC_EQUAL : 0;
      return (uint8_t)(0x80 | (p->stat & 0x78) | lyc_eq | (p->mode & STAT_MODE_MASK));
    }
    case 0xFF42: return p->scy;
    case 0xFF43: return p->scx;
    case 0xFF44: return p->ly;
    case 0xFF45: return p->lyc;
    case 0xFF47: return p->bgp;
    case 0xFF48: return p->obp0;
    case 0xFF49: return p->obp1;
    case 0xFF4A: return p->wy;
    case 0xFF4B: return p->wx;
    default:     return 0xFF;
  }
}

void ppu_write_reg(Ppu* p, uint16_t addr, uint8_t value) {
  switch (addr) {
    case 0xFF40: {
      bool was_on = (p->lcdc & LCDC_LCD_ENABLE) != 0;
      p->lcdc = value;
      bool now_on = (p->lcdc & LCDC_LCD_ENABLE) != 0;
      if (was_on && !now_on) {
        // LCD turned off: reset state to LY=0, mode 0 (HBlank), dot=0.
        // Real hardware also blanks the screen white, but we don't
        // produce frames in Batch A.
        p->ly = 0;
        p->mode = 0;
        p->dot = 0;
        p->window_line_counter = 0;
        p->prev_stat_line = false;
      } else if (!was_on && now_on) {
        // LCD turned on: start the next frame fresh in mode 2 at LY=0.
        // Real hardware actually has a few cycles of "bogus" mode 0
        // before the first proper mode 2, but the dmg-acid2 path and
        // any normal game don't depend on that.
        // TODO (Mooneye polish): model the LCD-on glitch cycles.
        p->ly = 0;
        p->mode = 2;
        p->dot = 0;
        p->window_line_counter = 0;
        p->prev_stat_line = false;
      }
      break;
    }
    case 0xFF41:
      // Only bits 6-3 are user-writable; bits 2-0 are read-only state.
      p->stat = (uint8_t)(value & 0x78);
      break;
    case 0xFF42: p->scy = value; break;
    case 0xFF43: p->scx = value; break;
    case 0xFF44:
      // LY is read-only in normal use. Some emulators reset it on
      // write (matching one interpretation); we ignore the write.
      break;
    case 0xFF45: p->lyc = value; break;
    case 0xFF47: p->bgp = value; break;
    case 0xFF48: p->obp0 = value; break;
    case 0xFF49: p->obp1 = value; break;
    case 0xFF4A: p->wy = value; break;
    case 0xFF4B: p->wx = value; break;
    default: break;
  }
}

// =====================================================================
// Rendering (Batch B: BG + window)
// =====================================================================
//
// At mode 3 entry the entire visible scanline is rendered to the
// framebuffer. The fetcher walks left to right in pixel-x, computing
// which tile each pixel belongs to and caching the tile-row data so
// consecutive pixels in the same tile share one fetch.
//
// LCDC controls:
//   bit 0 (BG_ENABLE):   if 0 on DMG, BG is drawn as white. CGB
//                        differs (transparent BG); we are DMG.
//                        TODO (CGB): change to "transparent" semantics
//                        if CGB support is ever added.
//   bit 4 (BG_TILEDATA): 0 = signed tile index relative to 0x9000;
//                        1 = unsigned tile index relative to 0x8000.
//   bit 3 (BG_TILEMAP):  0 = 0x9800 base; 1 = 0x9C00 base.
//   bit 5 (WIN_ENABLE):  enables the window layer.
//   bit 6 (WIN_TILEMAP): 0 = 0x9800 base; 1 = 0x9C00 base.
//
// Tile data layout:
//   16 bytes per tile, 2 bytes per row of 8 pixels.
//   For a row, byte 0 is the low bit of each of the 8 pixels (left to
//   right, MSB = leftmost pixel) and byte 1 is the high bit.
//   The 2-bit color index is then mapped through BGP to produce the
//   final 0-3 grayscale value.

// Read a byte of VRAM directly (no mode gating; the renderer is the
// PPU itself, not the CPU).
static uint8_t vram(const Ppu* p, uint16_t addr) {
  return p->vram[addr - 0x8000];
}

// Apply a palette: 4 entries packed as 2 bits each in the palette byte.
static uint8_t apply_palette(uint8_t pal, uint8_t color_idx) {
  return (uint8_t)((pal >> (color_idx * 2)) & 0x03);
}

// Fetch the tile-data address for the given tile index, taking the
// LCDC BG_TILEDATA bit into account.
//   bit 4 = 1: tile_idx is unsigned, base 0x8000. Range 0..255.
//   bit 4 = 0: tile_idx is signed, base 0x9000. Range -128..127.
//              Effective address: 0x9000 + (int8_t)tile_idx * 16.
static uint16_t tile_data_addr(uint8_t lcdc, uint8_t tile_idx) {
  if (lcdc & LCDC_BG_TILEDATA) {
    return (uint16_t)(0x8000 + tile_idx * 16);
  }
  // Signed: cast to int8_t, multiply, add to 0x9000.
  int8_t signed_idx = (int8_t)tile_idx;
  return (uint16_t)(0x9000 + signed_idx * 16);
}

// Render one scanline of background + window into the framebuffer.
// Called from ppu_tick_1t at mode 3 entry.
static void render_scanline(Ppu* p, uint8_t ly) {
  uint8_t* row = &p->framebuffer[ly * PPU_LCD_WIDTH];

  bool bg_enabled  = (p->lcdc & LCDC_BG_ENABLE)  != 0;
  bool win_enabled = (p->lcdc & LCDC_WIN_ENABLE) != 0;

  // Window can draw on this line at all only if WY <= LY and the
  // window's leftmost pixel (WX-7) is within the visible range.
  // WX = 0..6 puts the window slightly off the left; we still draw it.
  // WX > 166 means the window is fully off-screen to the right.
  // TODO: WX = 0..6 has subtle hardware quirks (CGB-only mostly); we
  // treat it as "window starts at x=0 with offset (7 - WX) inside the
  // window's tile column 0."
  bool win_active = win_enabled && (p->wy <= ly) && (p->wx <= 166);

  // Tile-row cache: avoids fetching the same tile-row repeatedly when
  // 8 consecutive pixels share it. We track which tile-row we have
  // cached as (map_addr, row_within_tile, base_for_data) so a cache
  // hit only happens within the same tile.
  uint16_t cached_tile_addr = 0xFFFF;  // sentinel: nothing cached yet
  uint8_t  cached_lo = 0;
  uint8_t  cached_hi = 0;
  bool     window_drawn_this_line = false;

  for (int x = 0; x < PPU_LCD_WIDTH; x++) {
    uint8_t color_idx;

    // 1. Decide whether this pixel comes from the window.
    bool pixel_in_window = win_active && (x + 7 >= (int)p->wx);

    if (pixel_in_window) {
      // Window pixel.
      window_drawn_this_line = true;

      // Source coordinates within the 256x256 window plane.
      // The window's internal Y is window_line_counter, NOT (LY - WY).
      uint16_t win_x = (uint16_t)(x - ((int)p->wx - 7));
      uint16_t win_y = p->window_line_counter;

      // Tile map lookup.
      uint16_t map_base = (p->lcdc & LCDC_WIN_TILEMAP) ? 0x9C00 : 0x9800;
      uint16_t map_x = win_x / 8;
      uint16_t map_y = win_y / 8;
      uint16_t map_addr = (uint16_t)(map_base + map_y * 32 + map_x);

      // Tile-row address.
      uint8_t tile_idx = vram(p, map_addr);
      uint8_t row_in_tile = (uint8_t)(win_y & 7);
      uint16_t tile_addr = (uint16_t)(tile_data_addr(p->lcdc, tile_idx) + row_in_tile * 2);

      if (tile_addr != cached_tile_addr) {
        cached_tile_addr = tile_addr;
        cached_lo = vram(p, tile_addr);
        cached_hi = vram(p, (uint16_t)(tile_addr + 1));
      }

      uint8_t bit = (uint8_t)(7 - (win_x & 7));
      uint8_t lo_bit = (cached_lo >> bit) & 1;
      uint8_t hi_bit = (cached_hi >> bit) & 1;
      color_idx = (uint8_t)((hi_bit << 1) | lo_bit);

    } else if (bg_enabled) {
      // BG pixel.
      uint16_t bg_x = (uint16_t)((p->scx + x) & 0xFF);
      uint16_t bg_y = (uint16_t)((p->scy + ly) & 0xFF);

      uint16_t map_base = (p->lcdc & LCDC_BG_TILEMAP) ? 0x9C00 : 0x9800;
      uint16_t map_x = bg_x / 8;
      uint16_t map_y = bg_y / 8;
      uint16_t map_addr = (uint16_t)(map_base + map_y * 32 + map_x);

      uint8_t tile_idx = vram(p, map_addr);
      uint8_t row_in_tile = (uint8_t)(bg_y & 7);
      uint16_t tile_addr = (uint16_t)(tile_data_addr(p->lcdc, tile_idx) + row_in_tile * 2);

      if (tile_addr != cached_tile_addr) {
        cached_tile_addr = tile_addr;
        cached_lo = vram(p, tile_addr);
        cached_hi = vram(p, (uint16_t)(tile_addr + 1));
      }

      uint8_t bit = (uint8_t)(7 - (bg_x & 7));
      uint8_t lo_bit = (cached_lo >> bit) & 1;
      uint8_t hi_bit = (cached_hi >> bit) & 1;
      color_idx = (uint8_t)((hi_bit << 1) | lo_bit);

    } else {
      // BG disabled on DMG: draw white (color index 0 through palette).
      color_idx = 0;
    }

    // Record the pre-palette BG color index for the sprite pass.
    p->bg_color_idx_line[x] = color_idx;
    row[x] = apply_palette(p->bgp, color_idx);
  }

  // Advance the window's internal Y if the window contributed any
  // pixels on this line. The mid-frame quirk this handles: a game
  // that disables the window for a few lines, then re-enables it,
  // continues from where it left off rather than restarting at WY.
  if (window_drawn_this_line) {
    p->window_line_counter++;
  }
}

// =====================================================================
// Sprite rendering (Batch C)
// =====================================================================
//
// OAM holds 40 sprites, 4 bytes each:
//   byte 0: Y position + 16 (so on-screen rows are Y in 16..159)
//   byte 1: X position + 8  (so on-screen columns are X in 8..167)
//   byte 2: tile index (low bit forced to 0 in 8x16 mode)
//   byte 3: attributes
//     bit 7: BG-over-OBJ priority (1 = sprite hidden behind BG colors 1-3)
//     bit 6: Y flip
//     bit 5: X flip
//     bit 4: palette select (0 = OBP0, 1 = OBP1)
//     bits 3-0: CGB only, ignored on DMG
//
// Per-line rendering rules:
//   - At most 10 sprites per line. The first 10 in OAM-index order
//     whose Y range covers this scanline are drawn; the rest are
//     skipped. The scan ignores X -- a sprite with X=0 still consumes
//     a slot.
//   - Sprite color index 0 is transparent (palette is irrelevant for it).
//   - Among sprites that overlap on a pixel:
//       * smaller X wins
//       * tiebreaker: smaller OAM index wins
//   - BG-over-OBJ bit (attribute bit 7): when set, the sprite is
//     hidden wherever the BG color INDEX is 1, 2, or 3. The check is
//     against the pre-palette index, not the displayed shade.

#define MAX_SPRITES_PER_LINE 10

typedef struct {
  uint8_t y;        // raw OAM byte 0 (screen Y + 16)
  uint8_t x;        // raw OAM byte 1 (screen X + 8)
  uint8_t tile;
  uint8_t attr;
  uint8_t oam_idx;  // 0..39, used for tiebreak ordering
} SpriteEntry;

// Scan OAM for sprites whose Y range covers `ly`. Returns the count
// (0..10). Fills `out` with up to 10 entries in OAM order.
static int oam_scan(const Ppu* p, uint8_t ly, SpriteEntry* out) {
  int sprite_height = (p->lcdc & LCDC_OBJ_SIZE) ? 16 : 8;
  int count = 0;
  for (int i = 0; i < 40 && count < MAX_SPRITES_PER_LINE; i++) {
    uint8_t y = p->oam[i * 4 + 0];
    int top = (int)y - 16;
    if ((int)ly >= top && (int)ly < top + sprite_height) {
      out[count].y       = y;
      out[count].x       = p->oam[i * 4 + 1];
      out[count].tile    = p->oam[i * 4 + 2];
      out[count].attr    = p->oam[i * 4 + 3];
      out[count].oam_idx = (uint8_t)i;
      count++;
    }
  }
  return count;
}

// Render the sprite layer on top of the BG/window already in the
// framebuffer for the given scanline. Uses bg_color_idx_line to
// evaluate the OBJ-to-BG priority bit.
static void render_sprites_on_line(Ppu* p, uint8_t ly) {
  if ((p->lcdc & LCDC_OBJ_ENABLE) == 0) return;

  int sprite_height = (p->lcdc & LCDC_OBJ_SIZE) ? 16 : 8;
  uint8_t* row = &p->framebuffer[ly * PPU_LCD_WIDTH];

  SpriteEntry sprites[MAX_SPRITES_PER_LINE];
  int n = oam_scan(p, ly, sprites);

  // For each pixel, find the highest-priority sprite that covers it
  // and is non-transparent there. Priority order: smaller X wins;
  // smaller OAM index breaks ties. Rather than sort, we iterate all
  // candidate sprites per pixel -- with at most 10 sprites this is
  // 10*160 = 1600 iterations, trivial.
  for (int x = 0; x < PPU_LCD_WIDTH; x++) {
    int best = -1;       // index into sprites[]
    uint8_t best_color = 0;

    for (int i = 0; i < n; i++) {
      const SpriteEntry* s = &sprites[i];
      // Sprite covers screen-X (s->x - 8) .. (s->x - 1).
      int sprite_x = (int)s->x - 8;
      if (x < sprite_x || x >= sprite_x + 8) continue;

      // Pixel column inside the sprite, accounting for X flip.
      int col = x - sprite_x;
      if (s->attr & 0x20) col = 7 - col;

      // Pixel row inside the sprite, accounting for Y flip and
      // 8x16 mode.
      int row_in_sprite = (int)ly - ((int)s->y - 16);
      if (s->attr & 0x40) row_in_sprite = (sprite_height - 1) - row_in_sprite;

      // Tile index. In 8x16 mode the low bit of the tile is masked
      // off; the second tile (N | 1) is automatically used for rows
      // 8-15.
      uint8_t tile = s->tile;
      if (sprite_height == 16) {
        tile &= 0xFE;
        if (row_in_sprite >= 8) {
          tile |= 1;
          row_in_sprite -= 8;
        }
      }

      // Sprite tile data is always at 0x8000 base (unsigned), unlike BG.
      uint16_t tile_addr = (uint16_t)(0x8000 + tile * 16 + row_in_sprite * 2);
      uint8_t lo_byte = vram(p, tile_addr);
      uint8_t hi_byte = vram(p, (uint16_t)(tile_addr + 1));
      uint8_t bit = (uint8_t)(7 - col);
      uint8_t color_idx = (uint8_t)(((hi_byte >> bit) & 1) << 1
                                  | ((lo_byte >> bit) & 1));

      // Color 0 is transparent for sprites.
      if (color_idx == 0) continue;

      // Priority among candidates: smaller X wins; smaller OAM idx
      // breaks ties.
      if (best == -1) {
        best = i;
        best_color = color_idx;
      } else {
        const SpriteEntry* b = &sprites[best];
        if (s->x < b->x || (s->x == b->x && s->oam_idx < b->oam_idx)) {
          best = i;
          best_color = color_idx;
        }
      }
    }

    if (best == -1) continue;

    // OBJ-to-BG priority check.
    const SpriteEntry* s = &sprites[best];
    bool bg_over_obj = (s->attr & 0x80) != 0;
    if (bg_over_obj && p->bg_color_idx_line[x] != 0) continue;

    // Apply the sprite's palette and write the pixel.
    uint8_t pal = (s->attr & 0x10) ? p->obp1 : p->obp0;
    row[x] = apply_palette(pal, best_color);
  }
}



// Compute the STAT-IRQ wire: true if any enabled interrupt source is
// currently asserting. The IRQ fires on the rising edge of this wire.
static bool compute_stat_line(const Ppu* p) {
  if (p->stat & STAT_LYC_INT_ENABLE && p->ly == p->lyc) return true;
  // Mode interrupts: fire while we're in the matching mode.
  // Note: the mode-1 enable is also raised on mode-2-during-VBlank
  // on real hardware (a quirk), but no DMG game depends on it.
  // TODO (Mooneye polish): model the mode-2-on-VBlank quirk.
  if ((p->stat & STAT_M0_INT_ENABLE) && p->mode == 0) return true;
  if ((p->stat & STAT_M1_INT_ENABLE) && p->mode == 1) return true;
  if ((p->stat & STAT_M2_INT_ENABLE) && p->mode == 2) return true;
  return false;
}

// Update prev_stat_line and fire a STAT IRQ on the rising edge.
static void check_stat_irq(Ppu* p) {
  bool now = compute_stat_line(p);
  if (now && !p->prev_stat_line) {
    bus_request_interrupt(p->bus, INT_STAT);
  }
  p->prev_stat_line = now;
}

void ppu_tick_1t(Ppu* p) {
  if ((p->lcdc & LCDC_LCD_ENABLE) == 0) {
    // LCD off: state machine frozen. STAT line low.
    p->prev_stat_line = false;
    return;
  }

  // Advance the dot counter. Mode transitions happen when crossing
  // specific boundaries; at the start of a new line we may also
  // transition to a new mode and increment LY.
  p->dot++;

  if (p->dot >= DOTS_PER_LINE) {
    // End of line. Move to next line.
    p->dot = 0;
    p->ly++;
    if (p->ly >= LINES_PER_FRAME) {
      p->ly = 0;
    }

    // Decide the mode at the start of the new line.
    if (p->ly < VBLANK_START_LINE) {
      // Visible line: starts in mode 2.
      p->mode = 2;
    } else if (p->ly == VBLANK_START_LINE) {
      // First VBlank line: enter mode 1, fire VBlank IRQ. Reset the
      // window's internal Y counter so the next frame's window draws
      // from its own row 0.
      p->mode = 1;
      p->window_line_counter = 0;
      bus_request_interrupt(p->bus, INT_VBLANK);
    }
    // else: still in VBlank; mode stays 1.
  } else {
    // Mid-line. Check for intra-line mode transitions on visible scanlines.
    if (p->ly < VBLANK_START_LINE) {
      if (p->dot == MODE2_DOTS) {
        // Transition mode 2 -> mode 3.
        p->mode = 3;
        // Render the scanline at mode 3 entry. Real hardware fetches
        // tile data progressively across mode 3; we treat it as a
        // single batch render. Since CPU access to VRAM is gated
        // off during mode 3, the visible result is the same.
        render_scanline(p, p->ly);
        render_sprites_on_line(p, p->ly);
      } else if (p->dot == MODE3_END) {
        // Transition mode 3 -> mode 0 (HBlank).
        p->mode = 0;
      }
    }
    // VBlank lines have no intra-line transitions.
  }

  check_stat_irq(p);
}

const uint8_t* ppu_framebuffer(const Ppu* p) {
  return p->framebuffer;
}