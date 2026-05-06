/**
 * @file ppu.h
 * @brief Game Boy PPU (Picture Processing Unit) -- DMG variant.
 *
 * The PPU drives the LCD: it owns VRAM (0x8000-0x9FFF) and OAM
 * (0xFE00-0xFE9F), runs a state machine that walks through modes
 * 2/3/0 for each visible scanline and mode 1 during VBlank, and
 * fires interrupts at well-defined points.
 *
 * This is Batch A: state machine, registers, interrupts. No actual
 * pixel rendering yet -- the framebuffer is allocated and zeroed
 * but never written to. Batches B and C add BG/window and sprite
 * rendering respectively.
 *
 * Memory map ownership:
 *   0x8000 - 0x9FFF  VRAM (8 KiB)
 *   0xFE00 - 0xFE9F  OAM  (160 bytes)
 *   0xFF40           LCDC
 *   0xFF41           STAT
 *   0xFF42           SCY
 *   0xFF43           SCX
 *   0xFF44           LY
 *   0xFF45           LYC
 *   0xFF47           BGP
 *   0xFF48           OBP0
 *   0xFF49           OBP1
 *   0xFF4A           WY
 *   0xFF4B           WX
 *
 * (DMA at 0xFF46 is a separate module.)
 */
#ifndef PPU_H
#define PPU_H

#include <stdint.h>
#include <stddef.h>

typedef struct Bus Bus;
typedef struct Ppu Ppu;

#define PPU_LCD_WIDTH   160
#define PPU_LCD_HEIGHT  144

Ppu* ppu_create(Bus* bus);
void ppu_destroy(Ppu* p);
void ppu_reset(Ppu* p);

/**
 * @brief Advance the PPU by one T-cycle.
 *
 * Called from sys_tick. Updates the mode/scanline state machine,
 * computes LY=LYC, and raises STAT and VBlank interrupts on the
 * cycles real hardware does.
 */
void ppu_tick_1t(Ppu* p);

/**
 * @brief VRAM and OAM access.
 *
 * Real hardware blocks CPU access to VRAM during mode 3 and to OAM
 * during modes 2/3 (reads return 0xFF, writes drop). Batch A
 * implements this gating so timing-sensitive ROMs see the right
 * thing; Batch B and beyond rely on it not being broken.
 */
uint8_t ppu_read_vram(const Ppu* p, uint16_t addr);
void    ppu_write_vram(Ppu* p, uint16_t addr, uint8_t value);
uint8_t ppu_read_oam(const Ppu* p, uint16_t addr);
void    ppu_write_oam(Ppu* p, uint16_t addr, uint8_t value);

/** @brief Read/write the I/O registers in 0xFF40 - 0xFF4B (skipping 0xFF46). */
uint8_t ppu_read_reg(const Ppu* p, uint16_t addr);
void    ppu_write_reg(Ppu* p, uint16_t addr, uint8_t value);

/**
 * @brief Test-only OAM / VRAM access that bypasses mode-3 gating.
 *
 * Real CPU code (going through bus_read/bus_write) is mode-gated.
 * Tests that want to set up known VRAM or OAM contents from the
 * outside use these to skip the gate. Production code should not.
 */
void ppu_poke_vram(Ppu* p, uint16_t addr, uint8_t value);
void ppu_poke_oam(Ppu* p, uint16_t addr, uint8_t value);

/**
 * @brief Pointer to the current framebuffer.
 *
 * 144 rows of 160 bytes each, row-major. Each byte is a 0-3 grayscale
 * index (0 = lightest, 3 = darkest, matching the natural BGP/OBP
 * mapping). The host maps these to actual colors as it sees fit.
 *
 * In Batch A this is always all zeros.
 */
const uint8_t* ppu_framebuffer(const Ppu* p);

#endif