#include "dma.h"
#include "bus.h"
#include "ppu.h"
#include <stdlib.h>

struct Dma {
  Bus*     bus;
  uint8_t  reg;       // last value written to 0xFF46 (readable)
  uint16_t source;    // base source address ((reg << 8) at start)
  uint8_t  pos;       // 0..160; 160 means inactive
};

Dma* dma_create(Bus* bus) {
  Dma* d = calloc(1, sizeof(Dma));
  if (d == NULL) return NULL;
  d->bus = bus;
  return d;
}

void dma_destroy(Dma* d) {
  free(d);
}

void dma_reset(Dma* d) {
  d->reg = 0xFF;       // post-boot default, mostly cosmetic
  d->source = 0;
  d->pos = 160;        // inactive
}

void dma_tick_1m(Dma* d) {
  if (d->pos >= 160) return;  // not active

  // Copy one byte. bus_peek reads without ticking; ppu_poke_oam
  // writes OAM without mode gating. This is the internal hardware
  // path; it does NOT go through the CPU-facing gating that bus.c
  // applies during DMA.
  uint8_t v = bus_peek(d->bus, (uint16_t)(d->source + d->pos));
  ppu_poke_oam(bus_ppu(d->bus), (uint16_t)(0xFE00 + d->pos), v);
  d->pos++;
}

uint8_t dma_read(const Dma* d) {
  return d->reg;
}

void dma_write(Dma* d, uint8_t value) {
  d->reg = value;
  // Start a new transfer. Real hardware has a 1-M-cycle delay before
  // the first byte copies; we skip it. The first dma_tick_1m after
  // this write will move byte 0.
  // TODO (Mooneye polish): model the 1-M-cycle setup delay.
  d->source = (uint16_t)(value << 8);
  d->pos = 0;
}

bool dma_active(const Dma* d) {
  return d->pos < 160;
}