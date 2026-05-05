/**
 * @file dma.h
 * @brief OAM DMA: bulk-copy 160 bytes to OAM at 1 byte per M-cycle.
 *
 * Triggered by writing any value V to 0xFF46. Source address is
 * (V << 8); destination is 0xFE00 (start of OAM). Transfer takes
 * 160 M-cycles (one byte per M-cycle).
 *
 * While a transfer is active:
 *   - The CPU can only access HRAM (0xFF80 - 0xFFFE) plus IE/IF.
 *     Reads anywhere else return 0xFF; writes anywhere else are
 *     silently dropped. This restriction is enforced by bus.c, which
 *     calls dma_active() to check.
 *   - OAM is being written by the DMA, so reading 0xFE00-0xFE9F via
 *     the CPU is also gated.
 *
 * The DMA module itself bypasses the CPU-facing gating: it uses
 * bus_peek to read the source bytes (peek is the no-tick / no-gate
 * internal accessor) and writes OAM directly via the PPU's poke
 * function.
 */
#ifndef DMA_H
#define DMA_H

#include <stdint.h>
#include <stdbool.h>

typedef struct Bus Bus;
typedef struct Dma Dma;

Dma* dma_create(Bus* bus);
void dma_destroy(Dma* d);
void dma_reset(Dma* d);

/**
 * @brief Advance the DMA by one M-cycle.
 *
 * Called from sys_tick at M-cycle granularity (once per 4 T-cycles).
 * If a transfer is active, copies one byte from source to OAM and
 * advances the position counter; when 160 bytes are done the
 * transfer ends.
 */
void dma_tick_1m(Dma* d);

/** @brief Read 0xFF46 (returns the last value written). */
uint8_t dma_read(const Dma* d);

/** @brief Write 0xFF46 (starts a new transfer from V << 8). */
void dma_write(Dma* d, uint8_t value);

/**
 * @brief True while a transfer is in progress. Used by bus.c to
 *        gate CPU access to anything other than HRAM.
 */
bool dma_active(const Dma* d);

#endif