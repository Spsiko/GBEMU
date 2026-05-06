#include "emu.h"
#include "cart.h"
#include "bus.h"
#include "cpu.h"
#include "ppu.h"
#include "joypad.h"
#include "apu.h"

#include <stdlib.h>

// 70224 T-cycles per frame on the DMG (4.194304 MHz / 59.7275 fps).
static const uint64_t kTCyclesPerFrame = 70224;

struct Emu {
  Cart* cart;
  Bus*  bus;
  CPU*  cpu;
};

Emu* emu_create(void) {
  Emu* e = calloc(1, sizeof(Emu));
  if (e == NULL) return NULL;

  e->cart = cart_create();
  if (e->cart == NULL) goto fail;

  e->bus = bus_create(e->cart);
  if (e->bus == NULL) goto fail;

  e->cpu = cpu_create(e->bus);
  if (e->cpu == NULL) goto fail;

  cpu_reset(e->cpu);
  return e;

fail:
  if (e->cpu)  cpu_destroy(e->cpu);
  if (e->bus)  bus_destroy(e->bus);
  if (e->cart) cart_free(e->cart);
  free(e);
  return NULL;
}

void emu_destroy(Emu* e) {
  if (e == NULL) return;
  cpu_destroy(e->cpu);
  bus_destroy(e->bus);
  cart_free(e->cart);
  free(e);
}

int emu_load_rom(Emu* e, const char* path) {
  return cart_load(e->cart, path);
}

void emu_run_frame(Emu* e) {
  uint64_t start = bus_total_t_cycles(e->bus);
  uint64_t deadline = start + kTCyclesPerFrame;
  while (bus_total_t_cycles(e->bus) < deadline) {
    cpu_step(e->cpu);
  }
}

void emu_set_buttons(Emu* e, uint8_t buttons) {
  joypad_set_buttons(bus_joypad(e->bus), buttons);
}

const uint8_t* emu_framebuffer(const Emu* e) {
  return ppu_framebuffer(bus_ppu(e->bus));
}
size_t emu_audio_read(Emu* e, int16_t* out, size_t max_samples) {
  return apu_read_samples(bus_apu(e->bus), out, max_samples);
}
